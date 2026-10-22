#!/usr/bin/env python3
"""Capture bounded, read-only serial health evidence without flashing or reset.

Run from the repository root with an explicit --port. pyserial is required only
for live capture; importing and testing the log parser needs the standard
library alone. A passing capture does not qualify host input or long runtimes.
An already-started final line has up to one extra second to finish; the reported
capture duration includes that grace period.
"""
import argparse
import json
from pathlib import Path
import re
import sys
import time


DEFAULT_SECONDS = 90
MIN_SECONDS = 35
# Firmware emits every 10 seconds; allow bounded scheduling/transport jitter.
MAX_HEALTH_GAP_SECONDS = 25.0
LINE_GRACE_SECONDS = 1.0
READ_TIMEOUT_SECONDS = 0.25
MAX_LOG_BYTES = 1024 * 1024
MAX_LINE_BYTES = 8192
READ_BYTES = 4096
QUEUE_CAPACITY = 32
MEMORY_FIELDS = ("heap", "min_heap", "free_internal", "largest_internal", "free_dma", "largest_dma")
STACK_FIELDS = ("stack", "stack_bytes", "stack_words")
BLUETOOTH_FIELDS = ("ble_initialized", "ble_connected", "ble_ready", "ble_failed")
REQUIRED_FIELDS = ("uptime_s", "heap", "min_heap", "queue", "locked")
ANSI_ESCAPE = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
FIELD = re.compile(r"([a-z][a-z0-9_]*)=([0-9]+)\Z")
RUNTIME_FIELD = re.compile(r"([a-z][a-z0-9_]*)=(-?[0-9]+)\Z")
# The board's persistent sampler runs independently of HID and calibration.
# Match IMU_MAX_AGE_US in board_mosaico.c; do not extend the firmware deadline.
MOTION_MAX_AGE_US = 50000
MIN_MOTION_SAMPLES = 3
MOTION_INPUT_KINDS = ("APP_INPUT_BLUETOOTH", "APP_INPUT_USB")
MOTION_PHASES = ("BOARD_IMU_UNINITIALIZED", "BOARD_IMU_ACTIVE", "BOARD_IMU_RECOVERING", "BOARD_IMU_FATAL")
MOTION_BOOLEANS = ("imu_available", "sample_age_known", "calibrated", "calibrating",
                   "input_connected", "input_ready")
MOTION_NUMBERS = ("samples", "sample_age_us", "calibration_samples", "input_kind")
MOTION_RECOVERY_FIELDS = ("phase", "worker_running", "recoveries", "failure_streak", "retry_ms",
                          "batch_time_known", "batch_us", "max_batch_us", "schedule_late_us")
MOTION_OPTIONAL_BOOLEANS = ("late_batches_saturated", "sample_count_saturated", "worker_running", "batch_time_known")
MOTION_OPTIONAL_NUMBERS = ("late_batches", "phase", "recoveries", "failure_streak", "retry_ms",
                          "batch_us", "max_batch_us", "schedule_late_us")
MOTION_ERROR_NAME = re.compile(r"ESP_(?:OK|FAIL|ERR_[A-Z0-9_]+)\Z")
# Keep these domains in sync with the owning firmware enum types. Offline tests
# compare the names with the headers; capture does not depend on a checkout.
AUDIO_STATES = ("AE_OFF", "AE_PREPARING", "AE_READY_MUTED", "AE_RUNNING", "AE_STOPPING", "AE_FAULT")
AUDIO_RESULTS = ("AE_OK", "AE_AGAIN", "AE_INVALID", "AE_UNAVAILABLE", "AE_NOT_SUPPORTED",
                 "AE_BUSY", "AE_STALE", "AE_DENIED", "AE_PRIVACY", "AE_QUOTA", "AE_OVERFLOW",
                 "AE_IO_ERROR", "AE_UNCERTAIN", "AE_TIMEOUT", "AE_NOT_FOUND", "AE_TOO_LATE",
                 "AE_INTEGRITY", "AE_TIME_UNKNOWN", "AE_EXHAUSTED")
SPEECH_STATES = ("SL_UNAVAILABLE", "SL_IDLE", "SL_LISTENING", "SL_CANDIDATE",
                 "SL_REJECTED", "SL_TIMED_OUT", "SL_CANCELLED", "SL_FAULT")
WIFI_STATES = ("APP_WIFI_UNAVAILABLE", "APP_WIFI_IDLE", "APP_WIFI_CONNECTING", "APP_WIFI_CONNECTED",
               "APP_WIFI_BACKOFF", "APP_WIFI_ERROR", "APP_WIFI_OBTAINING_IP")
CONTROL_STATES = ("APP_CONTROL_STOPPED", "APP_CONTROL_STARTING", "APP_CONTROL_READY",
                  "APP_CONTROL_STOPPING", "APP_CONTROL_ERROR")
RUNTIME_SCHEMAS = {
    "media": {
        "booleans": ("snapshot_available", "initialized", "audio_known", "off_confirmed",
                     "speech_known", "busy", "result_known"),
        "enums": {"audio_state": (AUDIO_STATES, "audio_known"),
                  "audio_error": (AUDIO_RESULTS, "audio_known"),
                  "speech_state": (SPEECH_STATES, "speech_known"),
                  "speech_result": (AUDIO_RESULTS, "speech_known"),
                  "result": (AUDIO_RESULTS, "result_known")},
        "integers": (),
    },
    "wifi": {
        "booleans": ("initialized", "lan_ready", "backend_available"),
        "enums": {"state": (WIFI_STATES, "initialized")},
        "integers": ("error", "backend_error"),
    },
    "control": {
        "booleans": ("initialized", "tls", "trust_known", "trust_present", "trust_uncertain", "clock_known"),
        "enums": {"server_state": (CONTROL_STATES, "initialized")},
        "integers": ("error", "server_error"),
    },
}
RUNTIME_FAULT_STATES = {
    "media": {"audio_state": "AE_FAULT", "speech_state": "SL_FAULT"},
    "wifi": {"state": "APP_WIFI_ERROR"},
    "control": {"server_state": "APP_CONTROL_ERROR"},
}
FAULT_PATTERNS = {
    "esp_error": re.compile(r"(?:^|\s)E\s+\(\s*\d+\s*\)|\bESP_ERR_[A-Z0-9_]+\b|"
                            r"\bESP_ERROR_CHECK(?:_WITHOUT_ABORT)?\s+failed"),
    "panic": re.compile(r"\bpanic\b|Guru Meditation|abort\(\) was called|"
                        r"\bassert(?:ion)? failed\b|stack smashing|stack canary",
                        re.IGNORECASE),
    "watchdog": re.compile(r"\b(?:watchdog|task_wdt|interrupt_wdt)\b|"
                           r"\b(?:TG[01]WDT|RTCWDT|WDT)[A-Z_]*RESET\b",
                           re.IGNORECASE),
    "boot_fault": re.compile(r"\bBoot\s+fault\b", re.IGNORECASE),
    "brownout": re.compile(r"\bbrownout\b", re.IGNORECASE),
    "motion_fault_latched": re.compile(r"\bFirst IMU fault\b"),
}


class HealthCapture:
    """Incremental parser with bounded raw log, line buffer and aggregate state."""

    def __init__(self):
        self.log = bytearray()
        self._line = bytearray()
        self.failures = set()
        self.sample_count = 0
        self.first_uptime = None
        self.last_uptime = None
        self.first_arrival = None
        self.last_arrival = None
        self.max_sample_gap = None
        self._line_arrival = None
        self.memory = {}
        self.stack = {}
        self.queue = {}
        self.last_locked = None
        self.bluetooth = {}
        self.runtime = {}
        self.motion = None
        self.log_truncated = False
        self._stopped = False

    def fail(self, code):
        # Only fixed codes from this module enter the nonpersonal summary.
        self.failures.add(code)

    @property
    def has_partial_line(self):
        return bool(self._line)

    @staticmethod
    def _range(destination, key, value):
        if key not in destination:
            destination[key] = {"min": value, "max": value}
        else:
            destination[key]["min"] = min(destination[key]["min"], value)
            destination[key]["max"] = max(destination[key]["max"], value)

    def _parse_line(self, data, arrival_seconds):
        line = ANSI_ESCAPE.sub("", data.decode("utf-8", errors="replace")).strip()
        for code, pattern in FAULT_PATTERNS.items():
            if pattern.search(line):
                self.fail(code)
        runtime = re.search(r"\bRuntime\s+(Wi-Fi|media|control)(?:\s+(.*))?$", line)
        if runtime:
            component = "wifi" if runtime.group(1) == "Wi-Fi" else runtime.group(1)
            self._parse_runtime(component, runtime.group(2) or "")
            return
        motion = re.search(r"(?:^|\binput:\s+)Motion(?:\s+(.*))?$", line)
        if motion:
            self._parse_motion(motion.group(1) or "", arrival_seconds)
            return
        marker = re.search(r"\bHealth\s+(.*)", line)
        if not marker:
            return
        values = {}
        for token in marker.group(1).split():
            match = FIELD.fullmatch(token)
            if not match or match.group(1) in values:
                self.fail("malformed_health")
                return
            # Health fields originate from bounded C integer types. Reject huge
            # integers before conversion, independently of Python's digit limit.
            if len(match.group(2)) > 20:
                self.fail("malformed_health")
                return
            value = int(match.group(2))
            if value > (1 << 64) - 1:
                self.fail("malformed_health")
                return
            values[match.group(1)] = value
        if any(field not in values for field in REQUIRED_FIELDS):
            self.fail("malformed_health")
            return
        if values["locked"] not in (0, 1) or values["min_heap"] > values["heap"]:
            self.fail("invalid_health_values")
            return
        if any(values[field] not in (0, 1) for field in BLUETOOTH_FIELDS if field in values):
            self.fail("invalid_health_values")
            return
        if values.get("ble_initialized") == 0:
            self.fail("ble_initialization_failed")
        if values.get("ble_failed") == 1:
            self.fail("ble_transport_failed")
        self.bluetooth = {field: values[field] for field in BLUETOOTH_FIELDS if field in values}
        if values["queue"] > QUEUE_CAPACITY:
            self.fail("queue_overflow")
        uptime = values["uptime_s"]
        if self.last_uptime is not None and uptime <= self.last_uptime:
            self.fail("uptime_not_increasing")
        if self.first_uptime is None:
            self.first_uptime = uptime
        self.last_uptime = uptime
        self.sample_count += 1
        if self.first_arrival is None:
            self.first_arrival = arrival_seconds
            if arrival_seconds > MAX_HEALTH_GAP_SECONDS:
                self.fail("health_initial_silence")
        else:
            gap = arrival_seconds - self.last_arrival
            self.max_sample_gap = max(self.max_sample_gap or 0.0, gap)
            if gap > MAX_HEALTH_GAP_SECONDS:
                self.fail("health_gap_exceeded")
        self.last_arrival = arrival_seconds
        for field in MEMORY_FIELDS:
            if field in values:
                self._range(self.memory, field, values[field])
        for field in STACK_FIELDS:
            if field in values:
                self._range(self.stack, field, values[field])
        self._range(self.queue, "occupancy", values["queue"])
        self.last_locked = values["locked"]

    def _parse_runtime(self, component, payload):
        schema = RUNTIME_SCHEMAS[component]
        values = {}
        for token in payload.split():
            match = RUNTIME_FIELD.fullmatch(token)
            if not match or match.group(1) in values or len(match.group(2)) > 11:
                self.fail("malformed_runtime_" + component)
                return
            value = int(match.group(2))
            if not -(1 << 31) <= value < (1 << 31):
                self.fail("malformed_runtime_" + component)
                return
            values[match.group(1)] = value
        required = set(schema["booleans"]) | schema["enums"].keys() | set(schema["integers"])
        if not required <= values.keys():
            self.fail("malformed_runtime_" + component)
            return
        invalid = any(values[key] not in (0, 1) for key in schema["booleans"])
        names = {}
        for key, (domain, known) in schema["enums"].items():
            value = values[key]
            if values[known] == 1:
                if not 0 <= value < len(domain):
                    invalid = True
                else:
                    names[key] = domain[value]
            else:
                invalid |= value != -1
                names[key] = None
        if component == "media":
            if not values["snapshot_available"]:
                invalid |= any(values[key] for key in schema["booleans"] if key != "snapshot_available")
            if not values["audio_known"]:
                invalid |= values["off_confirmed"] != 0
        if invalid:
            self.fail("invalid_runtime_" + component + "_values")
            return
        # Retain only fixed public fields. Extra numeric columns may be added
        # by firmware, but neither their names nor arbitrary log text is saved.
        previous = self.runtime.get(component, {})
        self.runtime[component] = {"valid_samples": previous.get("valid_samples", 0) + 1,
                                   "last_values": {key: values[key] for key in sorted(required)},
                                   "enum_names": names}
        for key, fault in RUNTIME_FAULT_STATES[component].items():
            if names[key] == fault:
                self.fail("runtime_" + component + "_fault")

    def _parse_motion(self, payload, arrival_seconds):
        if self.motion is None:
            self.motion = {"records": 0, "valid_samples": 0, "last_values": None,
                           "samples": {"first": None, "last": None}, "sample_age_us": {},
                           "progress": {"advanced": 0, "stalled": 0, "regressed": 0,
                                        "unverifiable": 0},
                           "first_arrival": None, "last_arrival": None, "max_gap": None}
        observed = self.motion
        observed["records"] += 1
        values = {}
        error_ok = None
        for token in payload.split():
            if token.startswith("imu_error="):
                name = token[len("imu_error="):]
                if error_ok is not None or len(name) > 64 or not MOTION_ERROR_NAME.fullmatch(name):
                    self.fail("malformed_motion")
                    return
                error_ok = name == "ESP_OK"
                continue
            match = RUNTIME_FIELD.fullmatch(token)
            if not match or match.group(1) in values or len(match.group(2).lstrip("-")) > 20:
                self.fail("malformed_motion")
                return
            value = int(match.group(2))
            if not -(1 << 31) <= value <= (1 << 64) - 1:
                self.fail("malformed_motion")
                return
            values[match.group(1)] = value
        required = set(MOTION_BOOLEANS) | set(MOTION_NUMBERS)
        if error_ok is None or not required <= values.keys():
            self.fail("malformed_motion")
            return
        recovery_fields = set(MOTION_RECOVERY_FIELDS)
        if values.keys() & recovery_fields and not recovery_fields <= values.keys():
            self.fail("malformed_motion")
            return
        invalid = any(values[key] not in (0, 1) for key in
                      MOTION_BOOLEANS + MOTION_OPTIONAL_BOOLEANS if key in values)
        invalid |= any(values[key] < 0 for key in MOTION_NUMBERS + MOTION_OPTIONAL_NUMBERS if key in values)
        invalid |= values["calibration_samples"] > (1 << 32) - 1
        invalid |= not 0 <= values["input_kind"] < len(MOTION_INPUT_KINDS)
        invalid |= any(values[key] > (1 << 32) - 1 for key in ("failure_streak", "retry_ms") if key in values)
        invalid |= "phase" in values and not 0 <= values["phase"] < len(MOTION_PHASES)
        if invalid:
            self.fail("invalid_motion_values")
            return
        previous = observed["samples"]["last"]
        samples = values["samples"]
        if previous is None:
            observed["samples"]["first"] = samples
        elif samples > previous:
            observed["progress"]["advanced"] += 1
        elif samples < previous:
            observed["progress"]["regressed"] += 1
            self.fail("motion_samples_regressed")
        elif samples == (1 << 64) - 1:
            # Saturation is valid uint64 data, but cannot prove advancement.
            observed["progress"]["unverifiable"] += 1
            self.fail("motion_progress_unverifiable")
        else:
            observed["progress"]["stalled"] += 1
            self.fail("motion_samples_stalled")
        observed["samples"]["last"] = samples
        if not values["imu_available"]:
            self.fail("motion_imu_unavailable")
        if not error_ok:
            self.fail("motion_imu_error")
        if samples == 0:
            self.fail("motion_no_samples")
        if not values["sample_age_known"]:
            self.fail("motion_sample_age_unknown")
        else:
            self._range(observed["sample_age_us"], "observed", values["sample_age_us"])
            if values["sample_age_us"] > MOTION_MAX_AGE_US:
                self.fail("motion_sample_stale")
        if values.get("sample_count_saturated") == 1:
            self.fail("motion_progress_unverifiable")
        if values.get("phase") == MOTION_PHASES.index("BOARD_IMU_FATAL"):
            self.fail("motion_phase_fatal")
        if values.get("worker_running") == 0:
            self.fail("motion_worker_stopped")
        if observed["last_arrival"] is None:
            observed["first_arrival"] = arrival_seconds
            if arrival_seconds > MAX_HEALTH_GAP_SECONDS:
                self.fail("motion_initial_silence")
        else:
            gap = arrival_seconds - observed["last_arrival"]
            observed["max_gap"] = max(observed["max_gap"] or 0.0, gap)
            if gap > MAX_HEALTH_GAP_SECONDS:
                self.fail("motion_gap_exceeded")
        observed["last_arrival"] = arrival_seconds
        observed["valid_samples"] += 1
        public = required | set(MOTION_OPTIONAL_BOOLEANS) | set(MOTION_OPTIONAL_NUMBERS)
        observed["last_values"] = {key: values[key] for key in sorted(public) if key in values}
        if "phase" in values:
            observed["last_values"]["phase_name"] = MOTION_PHASES[values["phase"]]
        # Preserve error truth without retaining arbitrary symbolic log strings.
        observed["last_values"]["imu_error_ok"] = error_ok

    def feed(self, data, arrival_seconds):
        """Consume bytes with monotonic host time elapsed since the port opened.

        A health line arrives when its last byte is received. Firmware uptime
        cannot substitute for host arrival time when detecting a stalled stream.
        False means capture must stop to retain bounds.
        """
        if self._stopped:
            return False
        remaining = MAX_LOG_BYTES - len(self.log)
        accepted = data[:remaining]
        self.log.extend(accepted)
        if len(data) > remaining:
            self.log_truncated = True
            self.fail("log_limit_exceeded")
            self._stopped = True
        cursor = 0
        while cursor < len(accepted):
            newline = accepted.find(b"\n", cursor)
            end = len(accepted) if newline == -1 else newline
            if len(self._line) + end - cursor > MAX_LINE_BYTES:
                self.fail("line_limit_exceeded")
                self._line.clear()
                self._stopped = True
                return False
            self._line.extend(accepted[cursor:end])
            self._line_arrival = arrival_seconds
            if newline == -1:
                break
            self._parse_line(self._line, arrival_seconds)
            self._line.clear()
            cursor = newline + 1
        return not self._stopped

    def finish(self):
        """Inspect an unterminated final line, including a final fault message."""
        if self._line:
            self.fail("unterminated_line")
            self._parse_line(self._line, self._line_arrival)
            self._line.clear()

    def summary(self, duration_seconds, requested_seconds, completed):
        failures = set(self.failures)
        if self.sample_count < 3:
            failures.add("insufficient_health_samples")
        if not completed:
            failures.add("capture_incomplete")
        final_age = None
        if self.last_arrival is None:
            if duration_seconds > MAX_HEALTH_GAP_SECONDS:
                failures.add("health_initial_silence")
        else:
            final_age = max(0.0, duration_seconds - self.last_arrival)
            if final_age > MAX_HEALTH_GAP_SECONDS:
                failures.add("health_final_silence")
        motion = None
        if self.motion is not None:
            observed = self.motion
            if observed["valid_samples"] < MIN_MOTION_SAMPLES:
                failures.add("insufficient_motion_samples")
            motion_age = None
            if observed["last_arrival"] is not None:
                motion_age = max(0.0, duration_seconds - observed["last_arrival"])
                if motion_age > MAX_HEALTH_GAP_SECONDS:
                    failures.add("motion_final_silence")
            motion = {"records": observed["records"], "valid_samples": observed["valid_samples"],
                      "last_values": observed["last_values"], "samples": observed["samples"],
                      "sample_progress": observed["progress"],
                      "sample_age_us": observed["sample_age_us"].get("observed"),
                      "freshness_limit_us": MOTION_MAX_AGE_US,
                      "timing_seconds": {"gap_limit": MAX_HEALTH_GAP_SECONDS,
                                         "first_arrival": observed["first_arrival"],
                                         "last_arrival": observed["last_arrival"],
                                         "max_inter_sample_gap": observed["max_gap"],
                                         "final_sample_age": motion_age}}
        return {
            "schema_version": 1,
            "status": "failed" if failures else "passed",
            "evidence": "bounded_serial_health_capture",
            "requested_duration_seconds": requested_seconds,
            "capture_duration_seconds": round(duration_seconds, 3),
            "capture_completed": completed,
            "line_grace_limit_seconds": LINE_GRACE_SECONDS,
            "health_samples": self.sample_count,
            "uptime_s": {"first": self.first_uptime, "last": self.last_uptime},
            "health_timing_seconds": {
                "gap_limit": MAX_HEALTH_GAP_SECONDS,
                "first_arrival": self.first_arrival,
                "last_arrival": self.last_arrival,
                "max_inter_sample_gap": self.max_sample_gap,
                "final_sample_age": final_age,
            },
            "memory_bytes": self.memory,
            "stack_watermarks": self.stack,
            "queue": self.queue.get("occupancy"),
            "last_locked": self.last_locked,
            "bluetooth": self.bluetooth,
            "runtime_observations": self.runtime,
            "motion": motion,
            "log_bytes": len(self.log),
            "log_limit_bytes": MAX_LOG_BYTES,
            "log_truncated": self.log_truncated,
            "failure_codes": sorted(failures),
            "qualification": "Serial health, reported Motion sampling and runtime-field integrity checks only; valid idle, unavailable "
                             "or unprovisioned observations do not qualify service readiness. "
                             "Host input and long-duration stability are unverified.",
        }


def capture_serial(serial_module, port, seconds, capture, clock=time.monotonic):
    """Read only, with a supplied module boundary for offline lifecycle tests."""
    device = None
    started = None
    duration = 0.0
    completed = False
    try:
        # Set inactive modem-control lines BEFORE open. No data writes, flush,
        # reset, auto-detection or input-buffer discard is performed.
        device = serial_module.Serial(port=None, baudrate=115200, timeout=READ_TIMEOUT_SECONDS,
                                      xonxoff=False, rtscts=False, dsrdtr=False)
        device.dtr = False
        device.rts = False
        device.port = port
        device.open()
        started = clock()
        deadline = started + seconds
        while (remaining := deadline - clock()) > 0:
            if not device.is_open:
                capture.fail("serial_disconnected")
                break
            device.timeout = min(READ_TIMEOUT_SECONDS, remaining)
            data = device.read(READ_BYTES)
            if not capture.feed(data, clock() - started):
                break
        else:
            completed = True
            # Finish only the line already in progress at the capture deadline.
            # Single-byte reads stop exactly at its newline rather than cutting
            # another line in the same read. Fault parsing and byte limits remain
            # active, and the fixed deadline is never extended by more data.
            grace_deadline = deadline + LINE_GRACE_SECONDS
            while capture.has_partial_line and (remaining := grace_deadline - clock()) > 0:
                if not device.is_open:
                    capture.fail("serial_disconnected")
                    completed = False
                    break
                device.timeout = min(READ_TIMEOUT_SECONDS, remaining)
                data = device.read(1)
                if not capture.feed(data, clock() - started):
                    completed = False
                    break
    except (serial_module.SerialException, OSError):
        capture.fail("serial_io_error" if started is not None else "serial_open_error")
        completed = False
    except KeyboardInterrupt:
        capture.fail("capture_interrupted")
        completed = False
    finally:
        if started is not None:
            duration = max(0.0, clock() - started)
        if device is not None:
            try:
                device.close()
            except (serial_module.SerialException, OSError):
                capture.fail("serial_close_error")
        capture.finish()
    return duration, completed


def duration_argument(value):
    try:
        seconds = int(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("seconds must be an integer") from error
    if seconds < MIN_SECONDS:
        raise argparse.ArgumentTypeError(f"seconds must be at least {MIN_SECONDS}")
    return seconds


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="explicit serial port; never auto-detected")
    parser.add_argument("--seconds", type=duration_argument, default=DEFAULT_SECONDS,
                        help=f"capture duration (default {DEFAULT_SECONDS}, minimum {MIN_SECONDS})")
    parser.add_argument("--output", type=Path, default=Path(".agent/local/device-smoke"),
                        help="output prefix for .log and .json; existing files are preserved")
    args = parser.parse_args(argv)
    log_path = Path(str(args.output) + ".log")
    summary_path = Path(str(args.output) + ".json")
    if log_path.exists() or summary_path.exists():
        parser.error("output already exists; choose a new --output prefix")
    capture = HealthCapture()
    try:
        log_path.parent.mkdir(parents=True, exist_ok=True)
        with log_path.open("xb") as log_file, summary_path.open("x", encoding="utf-8") as summary_file:
            duration, completed = 0.0, False
            try:
                import serial  # Optional runtime dependency; never imported by parser tests.
            except ImportError:
                capture.fail("pyserial_unavailable")
            else:
                duration, completed = capture_serial(serial, args.port, args.seconds, capture)
            result = capture.summary(duration, args.seconds, completed)
            log_file.write(capture.log)
            json.dump(result, summary_file, indent=2, sort_keys=True)
            summary_file.write("\n")
    except OSError as error:
        print(f"Unable to save capture files ({type(error).__name__}).", file=sys.stderr)
        return 2
    print(f"{result['status'].upper()}: {result['health_samples']} health samples, "
          f"{result['capture_duration_seconds']:.3f} seconds; summary: {summary_path}")
    if result["failure_codes"]:
        print("Failures: " + ", ".join(result["failure_codes"]), file=sys.stderr)
    print("Serial health evidence only; no host-input or long-duration qualification.")
    return 0 if result["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
