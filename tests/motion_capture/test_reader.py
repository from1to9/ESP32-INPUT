#!/usr/bin/env python3
"""Check evidence completeness independently of physical motion quality."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import motion_capture


IMAGE = "IMUCAP IMAGE " + "a" * 40 + " clean " + "b" * 64 + "\n"
BEGIN = "IMUCAP BEGIN 1 1 2 100 30000 1\n"
ROW0 = "IMUCAP ROW 1 0 7 2 10000 11000 4 00000000 00000000 00000000 00000000 00000000 3f800000 00000001 00000000 0 00000000 00000000\n"
ROW1 = "IMUCAP ROW 1 1 8 2 20000 21000 4 00000000 00000000 00000000 00000000 00000000 3f800000 00000001 00000000 0 00000000 00000000\n"
END = "IMUCAP END 1 2\n"


def parse(text):
    return motion_capture.parse_log(text.encode())


def edit_row(row, **changes):
    fields = row.split()
    positions = {"index": 3, "sequence": 4, "epoch": 5, "sample": 6, "observed": 7,
                 "flags": 15, "discontinuity": 16, "dropped": 17, "state": 19}
    for key, value in changes.items():
        fields[positions[key]] = str(value)
    return " ".join(fields) + "\n"


class ReaderTests(unittest.TestCase):
    def test_complete_is_not_physical_qualification(self):
        result = parse(IMAGE + BEGIN + ROW0 + ROW1 + END)
        self.assertTrue(result["wire_complete"])
        self.assertFalse(result["physical_motion_qualified"])
        self.assertTrue(result["sessions"][0]["ordered_segment_usable"])
        observation = result["sessions"][0]["observation"]
        self.assertEqual(observation["frames"], 2)
        self.assertEqual(observation["sample_interval_min_us"], 10000)
        self.assertEqual(observation["observed_age_max_us"], 1000)

    def test_missing_duplicate_and_truncated_rows(self):
        for text in (IMAGE + BEGIN + ROW1 + END, IMAGE + BEGIN + ROW0 + ROW0 + ROW1 + END,
                     IMAGE + BEGIN + ROW0 + ROW1[:-15] + "\n" + END,
                     IMAGE + BEGIN + ROW0 + ROW1, BEGIN + ROW0 + ROW1 + END):
            self.assertFalse(parse(text)["wire_complete"])

    def test_session_and_version_bounds(self):
        for begin in (BEGIN.replace("BEGIN 1", "BEGIN 2"), BEGIN.replace("1 2 100", "1 6001 100"),
                      BEGIN.replace("100 30000", "50000 30000")):
            self.assertFalse(parse(IMAGE + begin + ROW0 + ROW1 + END)["wire_complete"])
        self.assertFalse(parse(IMAGE + BEGIN + ROW0 + ROW1 + END + BEGIN + ROW0 + ROW1 + END)["wire_complete"])

    def test_sensor_faults_are_preserved_without_becoming_wire_loss(self):
        altered = ROW1.replace("8 2 20000 21000", "9 3 9000 100000").replace("3f800000", "7fc00000")
        result = parse(IMAGE + BEGIN + ROW0 + altered + END)
        self.assertTrue(result["wire_complete"])
        self.assertFalse(result["sessions"][0]["ordered_segment_usable"])
        observation = result["sessions"][0]["observation"]
        self.assertEqual(observation["sequence_or_epoch_breaks"], 1)
        self.assertEqual(observation["nonpositive_intervals"], 1)
        self.assertEqual(observation["nonfinite_frames"], 1)
        self.assertEqual(observation["invalid_or_stale_ages"], 1)
        self.assertEqual(result["sessions"][0]["rows"][1]["axes_bits"][-1], "7fc00000")

    def test_explicit_discontinuity_is_not_a_zero_acceleration_frame(self):
        marker = ROW1.replace("00000001 00000000", "00000000 00000004")
        result = parse(IMAGE + BEGIN + ROW0 + marker + END)
        self.assertTrue(result["wire_complete"])
        self.assertEqual(result["sessions"][0]["observation"]["frames"], 1)
        self.assertEqual(result["sessions"][0]["observation"]["discontinuity_records"], 1)

    def test_limits_and_integer_overflow(self):
        with self.assertRaises(ValueError):
            motion_capture.parse_log(b"x" * (motion_capture.MAX_BYTES + 1))
        self.assertFalse(parse(IMAGE + BEGIN + ROW0.replace("7 2", str(2**64) + " 2") + ROW1 + END)["wire_complete"])
        capture = motion_capture.RawCapture()
        self.assertTrue(capture.feed(b"line", 0))
        self.assertTrue(capture.has_partial_line)
        self.assertTrue(capture.feed(b"\n", 0))
        self.assertFalse(capture.has_partial_line)
        self.assertFalse(capture.feed(b"x" * motion_capture.MAX_BYTES, 0))
        self.assertEqual(capture.data, b"line\n")
        self.assertEqual(capture.errors, ["raw_byte_limit"])

    def test_wire_complete_contradictions_never_become_replay_ready(self):
        cases = [
            (BEGIN.replace("100 30000", "30000 40000"), ROW0, ROW1, "sample_outside_capture"),
            (BEGIN, edit_row(ROW0, observed=50000), ROW1, "observation_clock_backstep"),
            (BEGIN, ROW0, edit_row(ROW1, state="00000005"), "frame_delivery_state_mismatch"),
            (BEGIN, ROW0, edit_row(ROW1, state="00000002"), "discontinuity_state_mismatch"),
            (BEGIN, ROW0, edit_row(ROW1, dropped=1), "new_dropped_publications"),
            (BEGIN, ROW0, edit_row(ROW1, flags="00008001"), "drop_counter_saturated"),
            (BEGIN, edit_row(ROW0, dropped=3), edit_row(ROW1, dropped=2), "drop_counter_backstep"),
            (BEGIN, ROW0, edit_row(ROW1, flags="00100001"), "unknown_flags"),
            (BEGIN, ROW0, edit_row(ROW1, epoch=0), "invalid_frame_identity"),
        ]
        for begin, first, second, reason in cases:
            with self.subTest(reason=reason):
                result = parse(IMAGE + begin + first + second + END)
                self.assertTrue(result["wire_complete"])
                session = result["sessions"][0]
                self.assertEqual(len(session["rows"]), 2)
                self.assertFalse(session["ordered_segment_usable"])
                self.assertFalse(session["fixed_100hz_candidate_compatible"])
                self.assertIn(reason, session["segment_issues"])

    def test_initial_fence_and_historical_drops_define_a_valid_boundary(self):
        marker = "IMUCAP ROW 1 0 6 2 0 1000 4 " + "00000000 " * 6 + "00000000 00000002 10 00000000 00000002\n"
        first = edit_row(ROW0, index=1, dropped=10)
        second = edit_row(ROW1, index=2, dropped=10)
        result = parse(IMAGE + BEGIN.replace("1 2 100", "1 3 100") + marker + first + second + END.replace("1 2", "1 3"))
        self.assertTrue(result["wire_complete"])
        self.assertTrue(result["sessions"][0]["ordered_segment_usable"])
        self.assertEqual(result["sessions"][0]["segment_issues"], [])


if __name__ == "__main__":
    unittest.main()
