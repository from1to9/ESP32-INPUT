#!/usr/bin/env python3
"""Read bounded raw IMU evidence; never send commands, reset, or flash a device.

Wire completeness, accepted-publication continuity and physical qualification
are separate results. A complete log is never a mouse usability PASS.
"""
import argparse
import json
import math
from pathlib import Path
import re
import struct
import time

import device_smoke

MAX_BYTES = 4 * 1024 * 1024
MAX_ROWS = 6000
MAX_SESSIONS = 4
ANSI = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")


def unsigned(text, bits=64, base=10):
    pattern = r"[0-9]+" if base == 10 else r"[0-9a-f]{8}"
    if not re.fullmatch(pattern, text):
        raise ValueError("invalid integer")
    value = int(text, base)
    if value >= 1 << bits:
        raise ValueError("integer overflow")
    return value


def segment_issues(session, frames):
    """Validate relationships without erasing contradictory raw evidence."""
    rows = session["rows"]
    issues = set()
    first_frame = next((i for i, row in enumerate(rows) if row["flags"] & 1), len(rows))
    for i, row in enumerate(rows):
        present, state = bool(row["flags"] & 1), row["delivery_state"]
        if state not in range(5) or present != (state == 0):
            issues.add("frame_delivery_state_mismatch")
        if (state == 0 and row["discontinuity"]) or (state == 2 and not row["discontinuity"]):
            issues.add("discontinuity_state_mismatch")
        if row["discontinuity"] & ~0x1ff or row["flags"] & ~0xffff:
            issues.add("unknown_flags")
        if ((row["flags"] >> 9) & 3) > 2 or ((row["flags"] >> 11) & 3) > 1:
            issues.add("unknown_input_context")
        if row["flags"] & (1 << 15):
            issues.add("drop_counter_saturated")
        if row["source_error_bits"] or state in (3, 4):
            issues.add("source_or_delivery_failure")
        if not session["started_us"] <= row["observed_us"] <= session["finished_us"]:
            issues.add("observation_outside_capture")
        if present:
            if not row["sequence"] or not row["epoch"]:
                issues.add("invalid_frame_identity")
            if not session["started_us"] <= row["sample_us"] <= session["finished_us"]:
                issues.add("sample_outside_capture")
        elif row["sample_us"] or any(word != "00000000" for word in row["axes_bits"]):
            issues.add("marker_contains_sensor_frame")
        if row["discontinuity"] and (i >= first_frame or row["discontinuity"] & ~3):
            issues.add("segment_discontinuity")
    for a, b in zip(rows, rows[1:]):
        if b["observed_us"] < a["observed_us"]:
            issues.add("observation_clock_backstep")
        if b["dropped_frames"] < a["dropped_frames"]:
            issues.add("drop_counter_backstep")
    if frames:
        baseline = frames[0]["dropped_frames"]
        if any(row["dropped_frames"] != baseline for row in rows[first_frame:]):
            issues.add("new_dropped_publications")
    return issues


def parse_log(raw):
    if len(raw) > MAX_BYTES:
        raise ValueError("raw log exceeds bound")
    sessions, errors, active, identity = [], [], None, None
    for line_number, line in enumerate(raw.decode("utf-8", errors="replace").splitlines(), 1):
        line = ANSI.sub("", line)
        if "IMUCAP " not in line:
            continue
        fields = line[line.index("IMUCAP "):].split()
        try:
            kind = fields[1]
            if kind == "IMAGE":
                if len(fields) != 5 or not re.fullmatch(r"[0-9a-f]{40}|unknown", fields[2]) or fields[3] not in ("clean", "dirty", "unknown") or not re.fullmatch(r"[0-9a-f]{64}", fields[4]):
                    raise ValueError("invalid image identity")
                identity = {"git_commit": fields[2], "git_state": fields[3], "elf_sha256": fields[4]}
            elif kind == "BEGIN":
                if len(fields) != 8 or fields[2] != "1" or len(sessions) >= MAX_SESSIONS:
                    raise ValueError("invalid or excess session")
                if active is not None:
                    errors.append(f"line {line_number}: previous session missing END")
                session, count, start, end, reason = map(unsigned, fields[3:])
                if not session or count > MAX_ROWS or end < start or reason not in (1, 2):
                    raise ValueError("invalid session bounds")
                if any(s["session"] == session for s in sessions):
                    raise ValueError("duplicate session identity")
                active = {"session": session, "expected_rows": count, "started_us": start,
                          "finished_us": end, "stop_reason": reason, "image": identity,
                          "rows": [], "ended": False}
                sessions.append(active)
            elif kind == "ROW":
                if len(fields) != 20 or active is None:
                    raise ValueError("row outside session or malformed row")
                values = [unsigned(f) for f in fields[2:9]]
                session, index, sequence, epoch, sampled, observed, input_epoch = values
                if session != active["session"] or index != len(active["rows"]) or index >= active["expected_rows"]:
                    raise ValueError("missing, duplicate, excess or foreign row")
                words = [unsigned(f, 32, 16) for f in fields[9:17]]
                floats = [struct.unpack("<f", struct.pack("<I", v))[0] for v in words[:6]]
                active["rows"].append({"index": index, "sequence": sequence, "epoch": epoch,
                    "sample_us": sampled, "observed_us": observed, "input_epoch": input_epoch,
                    "axes_bits": fields[9:15], "axes": [v if math.isfinite(v) else None for v in floats],
                    "flags": words[6], "discontinuity": words[7], "dropped_frames": unsigned(fields[17]),
                    "source_error_bits": unsigned(fields[18], 32, 16), "delivery_state": unsigned(fields[19], 32, 16)})
            elif kind == "END":
                if len(fields) != 4 or active is None or unsigned(fields[2]) != active["session"] or unsigned(fields[3]) != active["expected_rows"]:
                    raise ValueError("invalid session END")
                active["ended"] = True
                active = None
            else:
                raise ValueError("unknown record")
        except (IndexError, ValueError) as error:
            errors.append(f"line {line_number}: {error}")
    for session in sessions:
        rows = session["rows"]
        session["wire_complete"] = session["ended"] and len(rows) == session["expected_rows"] and session["image"] is not None
        frames = [row for row in rows if row["flags"] & 1]
        intervals = [b["sample_us"] - a["sample_us"] for a, b in zip(frames, frames[1:])]
        ages = [r["observed_us"] - r["sample_us"] for r in frames]
        sequence_holes = sum(b["sequence"] != a["sequence"] + 1 or b["epoch"] != a["epoch"]
                             for a, b in zip(frames, frames[1:]))
        session["observation"] = {
            "frames": len(frames), "discontinuity_records": sum(r["discontinuity"] != 0 for r in rows),
            "sequence_or_epoch_breaks": sequence_holes,
            "nonfinite_frames": sum(any(v is None for v in r["axes"]) for r in frames),
            "sample_interval_min_us": min(intervals) if intervals else None,
            "sample_interval_max_us": max(intervals) if intervals else None,
            "nonpositive_intervals": sum(dt <= 0 for dt in intervals),
            "intervals_outside_100hz_5percent": sum(not 9500 <= dt <= 10500 for dt in intervals),
            "observed_age_min_us": min(ages) if ages else None,
            "observed_age_max_us": max(ages) if ages else None,
            "invalid_or_stale_ages": sum(not 0 <= age <= 50000 for age in ages),
        }
        obs = session["observation"]
        issues = segment_issues(session, frames)
        for failed, reason in ((not session["wire_complete"], "wire_incomplete"),
                               (len(frames) < 2, "insufficient_frames"),
                               (sequence_holes, "sequence_or_epoch_break"),
                               (obs["nonfinite_frames"], "nonfinite_sample"),
                               (obs["nonpositive_intervals"], "sample_clock_backstep"),
                               (obs["invalid_or_stale_ages"], "invalid_or_stale_age"),
                               (session["stop_reason"] != 1, "capacity_truncated")):
            if failed:
                issues.add(reason)
        session["segment_issues"] = sorted(issues)
        session["ordered_segment_usable"] = not issues
        session["fixed_100hz_candidate_compatible"] = bool(session["ordered_segment_usable"] and
            not obs["intervals_outside_100hz_5percent"])
    if errors:
        for session in sessions:
            session["wire_complete"] = False
            session["segment_issues"] = sorted(set(session["segment_issues"]) | {"wire_incomplete"})
            session["ordered_segment_usable"] = False
            session["fixed_100hz_candidate_compatible"] = False
    return {"schema_version": 1, "wire_complete": bool(sessions) and not errors and all(s["wire_complete"] for s in sessions),
            "errors": errors, "sessions": sessions, "physical_motion_qualified": False,
            "scope": "accepted board publications; timestamps are read completion, not sensor conversion time"}


class RawCapture:
    def __init__(self):
        self.data = bytearray()
        self.errors = []

    @property
    def has_partial_line(self):
        return bool(self.data) and self.data[-1] != 10

    def fail(self, reason):
        self.errors.append(reason)

    def feed(self, data, elapsed):
        del elapsed
        if len(self.data) + len(data) > MAX_BYTES:
            self.fail("raw_byte_limit")
            return False
        self.data.extend(data)
        return True

    def finish(self):
        pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--port")
    source.add_argument("--from-log", type=Path)
    parser.add_argument("--output", type=Path, required=True, help="new output prefix; existing files are preserved")
    parser.add_argument("--seconds", type=float, default=180)
    args = parser.parse_args()
    if not math.isfinite(args.seconds) or not 10 <= args.seconds <= 600:
        parser.error("seconds must be between 10 and 600")
    log_path, json_path = args.output.with_suffix(".log"), args.output.with_suffix(".json")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    # Reserve both files before opening any device; never overwrite evidence.
    with log_path.open("xb") as log_file, json_path.open("x") as json_file:
        capture = RawCapture()
        duration, completed = 0.0, True
        if args.from_log:
            with args.from_log.open("rb") as source_file:
                capture.feed(source_file.read(MAX_BYTES + 1), 0)
        else:
            import serial
            print("Read-only IMU capture started; no reset or device commands.", flush=True)
            duration, completed = device_smoke.capture_serial(serial, args.port, args.seconds, capture, time.monotonic)
        log_file.write(capture.data)
        result = parse_log(capture.data)
        result.update(capture_errors=capture.errors, capture_duration_seconds=duration, capture_completed=completed)
        if capture.errors or not completed:
            result["wire_complete"] = False
        json.dump(result, json_file, indent=2, allow_nan=False)
        json_file.write("\n")
    print(f"IMU wire complete: {result['wire_complete']}; sessions: {len(result['sessions'])}; {json_path}")
    return 0 if result["wire_complete"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
