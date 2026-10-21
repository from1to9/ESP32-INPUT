#!/usr/bin/env python3
"""Offline health-parser and serial-lifecycle tests; no serial device is opened."""
import importlib.util
import json
from pathlib import Path
from types import SimpleNamespace
import unittest


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("device_smoke", ROOT / "tools/device_smoke.py")
smoke = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(smoke)


def health(uptime, heap=200000, minimum=190000, queue=0, locked=0, extra=""):
    return (f"I ({uptime * 1000}) input: Health uptime_s={uptime} heap={heap} "
            f"min_heap={minimum} queue={queue} locked={locked}{extra}\r\n").encode()


def feed(capture, data, elapsed=10.0):
    """Parser-only fixtures use an explicit synthetic host arrival time."""
    return capture.feed(data, elapsed)


def result(capture, completed=True):
    capture.finish()
    return capture.summary(35.0, 35, completed)


class HealthParserTests(unittest.TestCase):
    def test_valid_fragmented_samples_and_optional_metrics(self):
        capture = smoke.HealthCapture()
        data = (b"\x1b[0;32m" + health(10, extra=" free_internal=80000 largest_internal=70000 stack=1200") +
                b"\x1b[0m" + health(20, heap=198000, minimum=188000, queue=32, locked=1,
                                     extra=" free_internal=79000 largest_internal=69000 stack=1100") +
                health(30, heap=201000, minimum=188000))
        for offset in range(0, len(data), 7):
            self.assertTrue(feed(capture, data[offset:offset + 7]))
        summary = result(capture)
        self.assertEqual(summary["status"], "passed")
        self.assertEqual(summary["health_samples"], 3)
        self.assertEqual(summary["memory_bytes"]["heap"], {"min": 198000, "max": 201000})
        self.assertEqual(summary["memory_bytes"]["free_internal"], {"min": 79000, "max": 80000})
        self.assertEqual(summary["stack_watermarks"]["stack"], {"min": 1100, "max": 1200})
        self.assertEqual(summary["queue"], {"min": 0, "max": 32})
        self.assertEqual(summary["capture_duration_seconds"], 35.0)
        self.assertEqual(bytes(capture.log), data)

    def test_optional_dma_ranges_use_only_observed_samples(self):
        capture = smoke.HealthCapture()
        for uptime, extra in ((10, " free_dma=50000 largest_dma=16384"),
                              (20, ""),
                              (30, " free_dma=44000 largest_dma=24576")):
            data = health(uptime, extra=extra)
            for offset in range(0, len(data), 7):
                self.assertTrue(feed(capture, data[offset:offset + 7], uptime))
        summary = result(capture)
        self.assertEqual(summary["status"], "passed")
        self.assertEqual(summary["failure_codes"], [])
        self.assertEqual(summary["health_samples"], 3)
        self.assertEqual(summary["memory_bytes"], {
            "heap": {"min": 200000, "max": 200000},
            "min_heap": {"min": 190000, "max": 190000},
            "free_dma": {"min": 44000, "max": 50000},
            "largest_dma": {"min": 16384, "max": 24576},
        })

    def test_optional_dma_missing_partial_and_zero_are_distinct(self):
        scenarios = [
            ("", {}),
            (" free_dma=1024", {"free_dma": {"min": 1024, "max": 1024}}),
            (" largest_dma=512", {"largest_dma": {"min": 512, "max": 512}}),
            (" free_dma=0 largest_dma=0", {
                "free_dma": {"min": 0, "max": 0},
                "largest_dma": {"min": 0, "max": 0},
            }),
        ]
        for extra, expected in scenarios:
            with self.subTest(extra=extra):
                capture = smoke.HealthCapture()
                for uptime in (10, 20, 30):
                    feed(capture, health(uptime, extra=extra if uptime == 20 else ""), uptime)
                summary = result(capture)
                self.assertEqual(summary["status"], "passed")
                observed = {key: value for key, value in summary["memory_bytes"].items()
                            if key in ("free_dma", "largest_dma")}
                self.assertEqual(observed, expected)
                self.assertEqual(summary["failure_codes"], [])
                self.assertEqual(summary["health_samples"], 3)

    def test_missing_or_insufficient_health_is_failure(self):
        for count in range(3):
            with self.subTest(count=count):
                capture = smoke.HealthCapture()
                feed(capture, b"I (1) input: starting\n")
                for index in range(count):
                    feed(capture, health(10 * (index + 1)))
                summary = result(capture)
                self.assertEqual(summary["status"], "failed")
                self.assertIn("insufficient_health_samples", summary["failure_codes"])
                if count == 0:
                    self.assertEqual(summary["memory_bytes"], {})
                    self.assertIsNone(summary["queue"])

    def test_uptime_reset_and_duplicate_are_rejected(self):
        for last in (1, 20):
            with self.subTest(last=last):
                capture = smoke.HealthCapture()
                feed(capture, health(10) + health(20) + health(last))
                self.assertIn("uptime_not_increasing", result(capture)["failure_codes"])

    def test_faults_fail_even_with_valid_health(self):
        messages = {
            "esp_error": [b"\x1b[0;31mE (42) input: failure\x1b[0m\n",
                          b"ESP_ERROR_CHECK failed: 0x101\n", b"ESP_ERR_NO_MEM\n"],
            "panic": [b"Guru Meditation Error: Core panic'ed\n", b"abort() was called\n"],
            "watchdog": [b"Task watchdog got triggered\n", b"rst:0x8 (TG1WDT_SYS_RESET)\n"],
            "boot_fault": [b"Boot fault: display unavailable\n"],
            "brownout": [b"Brownout detector was triggered\n"],
        }
        for code, variants in messages.items():
            for message in variants:
                with self.subTest(message=message):
                    capture = smoke.HealthCapture()
                    feed(capture, health(10) + health(20) + health(30))
                    feed(capture, message.rstrip(b"\n"))  # Final fault need not have a newline.
                    summary = result(capture)
                    self.assertEqual(summary["status"], "failed")
                    self.assertIn(code, summary["failure_codes"])

    def test_queue_overflow_is_not_silently_clamped(self):
        capture = smoke.HealthCapture()
        feed(capture, health(10) + health(20, queue=33) + health(30))
        summary = result(capture)
        self.assertIn("queue_overflow", summary["failure_codes"])
        self.assertEqual(summary["queue"]["max"], 33)

    def test_malformed_health_is_not_counted_as_success(self):
        invalid = [
            b"Health uptime_s=10 heap=5 min_heap=4 queue=0\n",
            b"Health uptime_s=10 heap=5 min_heap=4 queue=0 locked=0 queue=1\n",
            b"Health uptime_s=10 heap=5 min_heap=4 queue=-1 locked=0\n",
            b"Health uptime_s=10 heap=5 min_heap=4 queue=nan locked=0\n",
            b"Health uptime_s=18446744073709551616 heap=5 min_heap=4 queue=0 locked=0\n",
        ]
        for line in invalid:
            with self.subTest(line=line):
                capture = smoke.HealthCapture()
                feed(capture, line)
                self.assertEqual(capture.sample_count, 0)
                self.assertIn("malformed_health", result(capture)["failure_codes"])
        for line in (health(10, locked=2), health(10, heap=1, minimum=2)):
            capture = smoke.HealthCapture()
            feed(capture, line)
            self.assertIn("invalid_health_values", result(capture)["failure_codes"])

    def test_log_and_partial_line_memory_are_bounded(self):
        capture = smoke.HealthCapture()
        chunk = b"ordinary status\n" * 1000
        while len(capture.log) + len(chunk) <= smoke.MAX_LOG_BYTES:
            self.assertTrue(feed(capture, chunk))
        self.assertFalse(feed(capture, chunk))
        self.assertEqual(len(capture.log), smoke.MAX_LOG_BYTES)
        self.assertTrue(capture.log_truncated)
        self.assertFalse(feed(capture, b"more data"))
        self.assertEqual(len(capture.log), smoke.MAX_LOG_BYTES)
        self.assertIn("log_limit_exceeded", result(capture, False)["failure_codes"])
        capture = smoke.HealthCapture()
        self.assertFalse(feed(capture, b"x" * (smoke.MAX_LINE_BYTES + 1)))
        self.assertLessEqual(len(capture._line), smoke.MAX_LINE_BYTES)
        self.assertIn("line_limit_exceeded", result(capture, False)["failure_codes"])

    def test_summary_excludes_raw_text_and_port_identity(self):
        capture = smoke.HealthCapture()
        feed(capture, b"host-name secret-device /dev/private-port\n")
        feed(capture, health(10) + health(20) + health(30))
        encoded = json.dumps(result(capture))
        self.assertNotIn("secret-device", encoded)
        self.assertNotIn("private-port", encoded)
        self.assertNotIn("host-name", encoded)

    def test_incomplete_duration_cannot_pass(self):
        capture = smoke.HealthCapture()
        feed(capture, health(10) + health(20) + health(30))
        self.assertIn("capture_incomplete", result(capture, False)["failure_codes"])

    def test_optional_bluetooth_fields_preserve_idle_and_failed_states(self):
        scenarios = [
            ("", {}, []),
            (" ble_initialized=1 ble_connected=0 ble_ready=0 ble_failed=0",
             {"ble_initialized": 1, "ble_connected": 0, "ble_ready": 0, "ble_failed": 0}, []),
            (" ble_initialized=0 ble_connected=0 ble_ready=0 ble_failed=1",
             {"ble_initialized": 0, "ble_connected": 0, "ble_ready": 0, "ble_failed": 1},
             ["ble_initialization_failed", "ble_transport_failed"]),
            (" ble_initialized=1 ble_failed=1",
             {"ble_initialized": 1, "ble_failed": 1}, ["ble_transport_failed"]),
        ]
        for extra, expected, failures in scenarios:
            with self.subTest(extra=extra):
                capture = smoke.HealthCapture()
                for uptime in (10, 20, 30):
                    feed(capture, health(uptime, extra=extra), uptime)
                summary = result(capture)
                self.assertEqual(summary["bluetooth"], expected)
                self.assertEqual(summary["failure_codes"], failures)
                self.assertEqual(summary["status"], "failed" if failures else "passed")
        # A later recovery is recorded, but cannot erase an observed failure.
        feed(capture, health(40, extra=" ble_initialized=1 ble_failed=0"), 40)
        summary = capture.summary(45, 45, True)
        self.assertEqual(summary["bluetooth"]["ble_failed"], 0)
        self.assertIn("ble_transport_failed", summary["failure_codes"])

    def test_optional_bluetooth_fields_reject_non_boolean_values(self):
        for field in ("ble_initialized", "ble_connected", "ble_ready", "ble_failed"):
            with self.subTest(field=field):
                capture = smoke.HealthCapture()
                feed(capture, health(10, extra=f" {field}=2"))
                summary = result(capture)
                self.assertEqual(summary["health_samples"], 0)
                self.assertIn("invalid_health_values", summary["failure_codes"])
                self.assertEqual(summary["bluetooth"], {})


class SerialLifecycleTests(unittest.TestCase):
    def fake_module(self, fail_read=False, events=None):
        class SerialFailure(Exception):
            pass

        clock = SimpleNamespace(now=0.0)
        devices = []

        class Device:
            def __init__(self, **settings):
                self.settings = settings
                self.timeout = settings["timeout"]
                self.port = settings["port"]
                self.dtr = True
                self.rts = True
                self.is_open = False
                self.reads = 0
                self.delivered = 0
                self.pending = b""
                self.event_at = None
                self.event_index = 0
                self.sizes = []
                self.closed = False
                devices.append(self)

            def open(self):
                assert self.settings["port"] is None
                assert self.dtr is False and self.rts is False
                assert self.port == "/offline/fake"
                self.is_open = True

            def read(self, size):
                assert size in (1, smoke.READ_BYTES)
                assert 0 < self.timeout <= smoke.READ_TIMEOUT_SECONDS
                self.reads += 1
                self.sizes.append(size)
                if fail_read and self.delivered == 3:
                    raise SerialFailure("simulated unplug")
                if self.event_at is None:
                    if events is None:
                        delay, self.pending = 10, health((self.event_index + 1) * 10)
                    elif self.event_index < len(events):
                        delay, self.pending = events[self.event_index]
                    else:
                        clock.now += self.timeout
                        return b""
                    self.event_at = clock.now + delay
                if self.event_at > clock.now + self.timeout:
                    clock.now += self.timeout
                    return b""
                clock.now = max(clock.now, self.event_at)
                data, self.pending = self.pending[:size], self.pending[size:]
                if not self.pending:
                    self.event_at = None
                    self.event_index += 1
                    self.delivered += 1
                return data

            def write(self, data):
                raise AssertionError("Capture must never transmit data")

            def close(self):
                self.closed = True
                self.is_open = False

        return SimpleNamespace(Serial=Device, SerialException=SerialFailure), clock, devices

    def capture_events(self, events, seconds=35, capture=None):
        module, clock, devices = self.fake_module(events=events)
        capture = capture if capture is not None else smoke.HealthCapture()
        duration, completed = smoke.capture_serial(module, "/offline/fake", seconds, capture,
                                                   clock=lambda: clock.now)
        self.assertTrue(devices[0].closed)
        return capture, capture.summary(duration, seconds, completed), devices[0]

    def test_inactive_control_lines_precede_open_and_capture_only_reads(self):
        module, clock, devices = self.fake_module()
        capture = smoke.HealthCapture()
        duration, completed = smoke.capture_serial(module, "/offline/fake", 35, capture,
                                                   clock=lambda: clock.now)
        self.assertTrue(completed)
        self.assertEqual(duration, 35)
        self.assertTrue(devices[0].closed)
        self.assertEqual(capture.summary(duration, 35, completed)["status"], "passed")

    def test_disconnect_fails_after_three_good_samples(self):
        module, clock, devices = self.fake_module(fail_read=True)
        capture = smoke.HealthCapture()
        duration, completed = smoke.capture_serial(module, "/offline/fake", 35, capture,
                                                   clock=lambda: clock.now)
        self.assertFalse(completed)
        self.assertEqual(capture.sample_count, 3)
        self.assertTrue(devices[0].closed)
        self.assertIn("serial_io_error", capture.summary(duration, 35, completed)["failure_codes"])

    def test_initial_final_and_intermediate_silence_fail_with_fake_host_clock(self):
        scenarios = {
            "health_initial_silence": [(30, health(100)), (10, health(110)),
                                       (10, health(120)), (10, health(130)),
                                       (10, health(140)), (10, health(150)), (10, health(160))],
            "health_final_silence": [(10, health(100)), (10, health(110)), (10, health(120))],
            "health_gap_exceeded": [(10, health(100)), (10, health(110)),
                                    (40, health(120)), (10, health(130)),
                                    (10, health(140)), (10, health(150))],
        }
        for expected, events in scenarios.items():
            with self.subTest(expected=expected):
                module, clock, devices = self.fake_module(events=events)
                clock.now = 5000.0  # Only elapsed host time may enter the summary.
                capture = smoke.HealthCapture()
                duration, completed = smoke.capture_serial(module, "/offline/fake", 90, capture,
                                                           clock=lambda: clock.now)
                summary = capture.summary(duration, 90, completed)
                self.assertTrue(completed)
                self.assertTrue(devices[0].closed)
                self.assertGreaterEqual(summary["health_samples"], 3)
                self.assertEqual(summary["status"], "failed")
                self.assertEqual(summary["failure_codes"], [expected])

    def test_normal_fragmentation_and_gap_limit_use_arrival_time(self):
        events = []
        for uptime in range(100, 190, 10):
            line = health(uptime, extra=" free_internal=80000 largest_internal=70000 stack_bytes=1200")
            events += [(8, line[:10]), (1, line[10:-1]), (1, line[-1:])]
        module, clock, _ = self.fake_module(events=events)
        capture = smoke.HealthCapture()
        duration, completed = smoke.capture_serial(module, "/offline/fake", 90, capture,
                                                   clock=lambda: clock.now)
        summary = capture.summary(duration, 90, completed)
        self.assertEqual(summary["status"], "passed")
        self.assertEqual(summary["health_samples"], 9)
        self.assertEqual(summary["health_timing_seconds"], {
            "gap_limit": 25.0, "first_arrival": 10.0, "last_arrival": 90.0,
            "max_inter_sample_gap": 10.0, "final_sample_age": 0.0,
        })
        self.assertEqual(summary["stack_watermarks"]["stack_bytes"], {"min": 1200, "max": 1200})
        capture = smoke.HealthCapture()
        for elapsed in (25.0, 50.0, 75.0):
            feed(capture, health(int(elapsed)), elapsed)
        self.assertEqual(capture.summary(100.0, 100, True)["status"], "passed")

    def test_final_partial_line_keeps_original_arrival_time(self):
        events = [(10, health(10)), (10, health(20)), (10, health(30).rstrip(b"\r\n"))]
        module, clock, _ = self.fake_module(events=events)
        capture = smoke.HealthCapture()
        duration, completed = smoke.capture_serial(module, "/offline/fake", 90, capture,
                                                   clock=lambda: clock.now)
        summary = capture.summary(duration, 90, completed)
        self.assertEqual(summary["health_samples"], 3)
        self.assertEqual(summary["health_timing_seconds"]["last_arrival"], 30.0)
        self.assertEqual(summary["health_timing_seconds"]["final_sample_age"], 61.0)
        self.assertIn("health_final_silence", summary["failure_codes"])
        self.assertIn("unterminated_line", summary["failure_codes"])

    def test_deadline_cut_line_finishes_in_grace_without_starting_another(self):
        line = health(40, extra=" ble_initialized=1 ble_connected=0 ble_ready=0 ble_failed=0")
        split = line.index(b" heap=") + 2
        events = [(10, health(10)), (10, health(20)), (10, health(30)),
                  (5, line[:split]), (0.4, line[split:] + b"next unread line")]
        capture, summary, device = self.capture_events(events)
        self.assertEqual(summary["status"], "passed")
        self.assertEqual(summary["health_samples"], 4)
        self.assertEqual(summary["capture_duration_seconds"], 35.4)
        self.assertEqual(summary["line_grace_limit_seconds"], 1.0)
        self.assertAlmostEqual(summary["health_timing_seconds"]["last_arrival"], 35.4)
        self.assertTrue(bytes(capture.log).endswith(line))
        self.assertEqual(device.pending, b"next unread line")
        self.assertIn(1, device.sizes)
        self.assertFalse(capture.has_partial_line)
        # A complete line at the deadline does not start a grace capture.
        _, summary, device = self.capture_events(events[:3] + [(5, line)])
        self.assertEqual(summary["status"], "passed")
        self.assertEqual(summary["capture_duration_seconds"], 35.0)
        self.assertNotIn(1, device.sizes)

    def test_grace_deadline_is_fixed_and_unfinished_evidence_fails(self):
        events = [(10, health(10)), (10, health(20)), (10, health(30)),
                  (5, b"Health uptime_s=40 h"), (0.4, b"e"), (0.4, b"a"), (0.4, b"p=5\n")]
        capture, summary, _ = self.capture_events(events)
        self.assertEqual(summary["capture_duration_seconds"], 36.0)
        self.assertEqual(summary["health_samples"], 3)
        self.assertIn("malformed_health", summary["failure_codes"])
        self.assertIn("unterminated_line", summary["failure_codes"])
        self.assertTrue(bytes(capture.log).endswith(b"Health uptime_s=40 hea"))
        # Even a syntactically complete sample without its newline stays failed.
        _, summary, _ = self.capture_events(events[:3] + [(5, health(40).rstrip(b"\r\n"))])
        self.assertEqual(summary["capture_duration_seconds"], 36.0)
        self.assertIn("unterminated_line", summary["failure_codes"])

    def test_grace_preserves_fault_malformed_and_freshness_checks(self):
        base = [(10, health(10)), (10, health(20)), (10, health(30))]
        for prefix, suffix, expected in [
                (b"E (42) inp", b"ut: failure\n", "esp_error"),
                (b"Guru Med", b"itation Error: panic\n", "panic"),
                (b"Health uptime_s=40 heap=", b"bad\n", "malformed_health")]:
            with self.subTest(expected=expected):
                capture, summary, _ = self.capture_events(base + [(5, prefix), (0.4, suffix)])
                self.assertEqual(summary["status"], "failed")
                self.assertIn(expected, summary["failure_codes"])
                self.assertNotIn("unterminated_line", summary["failure_codes"])
                self.assertTrue(bytes(capture.log).endswith(prefix + suffix))
        line = health(40)
        _, summary, _ = self.capture_events(base + [(25, line[:10]), (0.5, line[10:])], seconds=55)
        self.assertEqual(summary["capture_duration_seconds"], 55.5)
        self.assertEqual(summary["health_timing_seconds"]["max_inter_sample_gap"], 25.5)
        self.assertIn("health_gap_exceeded", summary["failure_codes"])

    def test_grace_keeps_line_and_log_bounds(self):
        base = [(10, health(10)), (10, health(20)), (10, health(30))]
        capture, summary, _ = self.capture_events(base + [(5, b"x" * smoke.MAX_LINE_BYTES),
                                                         (0.1, b"x\n")])
        self.assertEqual(summary["status"], "failed")
        self.assertIn("line_limit_exceeded", summary["failure_codes"])
        self.assertFalse(summary["capture_completed"])
        self.assertLessEqual(len(capture._line), smoke.MAX_LINE_BYTES)
        capture = smoke.HealthCapture()
        budget = smoke.MAX_LOG_BYTES - sum(len(data) for _, data in base) - 1
        feed(capture, b"padding\n" * (budget // 8) + b"\n" * (budget % 8), 0)
        capture, summary, _ = self.capture_events(base + [(5, b"x"), (0.1, b"y\n")], capture=capture)
        self.assertEqual(len(capture.log), smoke.MAX_LOG_BYTES)
        self.assertIn("log_limit_exceeded", summary["failure_codes"])
        self.assertTrue(summary["log_truncated"])
        self.assertFalse(summary["capture_completed"])

    def test_duration_has_documented_minimum_and_default(self):
        self.assertEqual(smoke.DEFAULT_SECONDS, 90)
        self.assertEqual(smoke.duration_argument("35"), 35)
        for value in ("34", "0", "-1", "nan", "35.5"):
            with self.subTest(value=value), self.assertRaises(smoke.argparse.ArgumentTypeError):
                smoke.duration_argument(value)


if __name__ == "__main__":
    unittest.main(verbosity=2)
