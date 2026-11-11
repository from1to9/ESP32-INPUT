#!/usr/bin/env python3
"""Offline health-parser and serial-lifecycle tests; no serial device is opened."""
import importlib.util
import json
from pathlib import Path
import re
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


def audio_job(**overrides):
    values = dict(slot=3, kind=3, state=1, stop_reason=0, error=0, first_frame=0,
                  first_frame_ms=0, deadline_ms=0, cleanup=0, received_bytes=0,
                  delivered_bytes=0, sample_frames=0, discarded_frames=0)
    values.update(overrides)
    return ("I (34296) audio_diag: Audio diag job " +
            " ".join(f"{key}={value}" for key, value in values.items()) + "\r\n").encode()


class AudioJobParserTests(unittest.TestCase):
    def capture(self, records):
        capture = smoke.HealthCapture()
        for data in records:
            for start in range(0, len(data), 7):
                feed(capture, data[start:start + 7])
        for uptime in (10, 20, 30):
            feed(capture, health(uptime))
        return capture

    def test_historical_absence_and_empty_slots_are_not_media_success(self):
        summary = result(self.capture([]))
        self.assertEqual(summary["status"], "passed")
        self.assertIsNone(summary["audio_jobs"])
        summary = result(self.capture([audio_job(slot=slot, kind=0, state=0) for slot in range(4)]))
        self.assertEqual(summary["failure_codes"], [])
        self.assertEqual(len(summary["audio_jobs"]["slots"]), 4)
        self.assertTrue(all(slot["last_report"]["assessment"] == "empty"
                            for slot in summary["audio_jobs"]["slots"].values()))
        self.assertIn("absent reports remain unobserved", summary["qualification"])

    def test_start_timeout_survives_cleanup_off_and_new_successful_job(self):
        # Sanitized production-shaped sequence: no error-level log is needed.
        failed = audio_job(state=7, stop_reason=11, error=13, cleanup=1)
        summary = result(self.capture([
            audio_job(), failed, audio_job(state=7, stop_reason=11, error=13),
            b"I (38415) audio_diag: Audio diag audio known=1 state=0 route=4 error=0 generation=8 off_confirmed=1\r\n",
            runtime("media"), audio_job(),
            audio_job(state=2, first_frame=1, first_frame_ms=40000, deadline_ms=50000,
                      received_bytes=960, delivered_bytes=960, sample_frames=480),
            audio_job(state=5, first_frame=1, first_frame_ms=40000, deadline_ms=50000,
                      received_bytes=960, delivered_bytes=960, sample_frames=480),
        ]))
        self.assertEqual(summary["failure_codes"], ["audio_job_failed"])
        slot = summary["audio_jobs"]["slots"]["3"]
        self.assertEqual(slot["failed_reports"], 2)
        first = slot["first_failure"]
        self.assertEqual(first["enum_names"], {"kind": "AE_PLAYBACK_STREAM", "state": "AE_JOB_ERROR",
                                               "stop_reason": "AE_STOP_START_TIMEOUT", "error": "AE_TIMEOUT"})
        self.assertEqual(first["values"]["cleanup"], 1)
        self.assertEqual(first["values"]["first_frame"], 0)
        self.assertEqual(first["values"]["first_frame_ms"], 0)
        self.assertEqual(slot["last_report"]["assessment"], "completed")
        self.assertNotIn("generation", first["values"])

    def test_late_capture_retained_failure_needs_no_start_or_global_snapshot(self):
        summary = result(self.capture([audio_job(state=7, stop_reason=11, error=13)]))
        self.assertEqual(summary["status"], "failed")
        self.assertEqual(summary["failure_codes"], ["audio_job_failed"])
        self.assertEqual(summary["runtime_observations"], {})

    def test_real_capture_overflow_retains_partial_delivery_evidence(self):
        overflow = audio_job(slot=0, kind=0, state=6, stop_reason=9, error=10, first_frame=1,
                             first_frame_ms=30687, deadline_ms=3630687, received_bytes=278400,
                             delivered_bytes=270720, sample_frames=135360, discarded_frames=8)
        summary = result(self.capture([overflow, runtime("media"), overflow,
                                       audio_job(slot=0, kind=0, state=0)]))
        self.assertEqual(summary["failure_codes"], ["audio_job_failed"])
        slot = summary["audio_jobs"]["slots"]["0"]
        self.assertEqual(slot["failed_reports"], 2)
        self.assertEqual(slot["last_report"]["assessment"], "empty")
        failure = slot["first_failure"]
        self.assertEqual(failure["enum_names"]["state"], "AE_JOB_PARTIAL")
        self.assertEqual(failure["enum_names"]["stop_reason"], "AE_STOP_OVERFLOW")
        self.assertEqual(failure["enum_names"]["error"], "AE_OVERFLOW")
        for field, expected in (("first_frame_ms", 30687), ("received_bytes", 278400),
                                ("delivered_bytes", 270720), ("sample_frames", 135360), ("discarded_frames", 8)):
            self.assertEqual(failure["values"][field], expected)

    def test_normal_stop_cancel_guard_and_limit_endings_are_not_failures(self):
        cases = [
            ({"state": 7}, "stopped"),
            ({"state": 7, "error": 8}, "stopped"),  # Shutdown safety guard before first frame.
            ({"state": 6, "stop_reason": 1, "first_frame": 1}, "cancelled"),
            ({"slot": 1, "kind": 1, "state": 6, "stop_reason": 2, "first_frame": 1}, "cancelled"),
            ({"state": 6, "stop_reason": 3, "error": 13, "first_frame": 1}, "limit_reached"),
            ({"state": 6, "stop_reason": 4, "error": 9, "first_frame": 1}, "limit_reached"),
            ({"state": 7, "stop_reason": 7, "error": 8}, "cancelled"),
            ({"state": 6, "stop_reason": 14, "error": 8, "first_frame": 1}, "cancelled"),
            ({"state": 7, "stop_reason": 7, "error": 0}, "cancelled"),  # Explicit ae_stop.
            ({"state": 6, "stop_reason": 14, "error": 0, "first_frame": 1}, "cancelled"),
        ]
        for values, assessment in cases:
            with self.subTest(values=values):
                summary = result(self.capture([audio_job(**values)]))
                self.assertEqual(summary["failure_codes"], [])
                slot = summary["audio_jobs"]["slots"][str(values.get("slot", 3))]
                self.assertEqual(slot["last_report"]["assessment"], assessment)
                self.assertIsNone(slot["first_failure"])

    def test_active_and_finalizing_reports_do_not_claim_completed_delivery(self):
        for state, cleanup, expected in ((1, 0, "in_progress"), (2, 0, "in_progress"),
                                         (3, 1, "ending"), (4, 1, "ending")):
            with self.subTest(state=state):
                summary = result(self.capture([audio_job(state=state, cleanup=cleanup)]))
                self.assertEqual(summary["failure_codes"], [])
                self.assertEqual(summary["audio_jobs"]["slots"]["3"]["last_report"]["assessment"], expected)

    def test_failure_reasons_and_cleanup_errors_cannot_be_hidden_by_normal_stops(self):
        for reason in (5, 6, 8, 9, 10, 11, 12, 13, 15):
            for state in (3, 4, 6, 7):
                with self.subTest(reason=reason, state=state):
                    summary = result(self.capture([audio_job(state=state, stop_reason=reason, error=13)]))
                    self.assertEqual(summary["failure_codes"], ["audio_job_failed"])
        for reason in (0, 1, 2, 3, 4, 7, 14):
            for error in (11, 12, 16, 18):  # IO, uncertain cleanup, integrity and exhausted.
                with self.subTest(reason=reason, error=error):
                    summary = result(self.capture([audio_job(state=6, stop_reason=reason, error=error)]))
                    self.assertEqual(summary["failure_codes"], ["audio_job_failed"])
        for reason in (7, 14):
            summary = result(self.capture([audio_job(state=7, stop_reason=reason, error=1)]))
            self.assertEqual(summary["failure_codes"], ["audio_job_failed"])

    def test_failure_can_be_observed_during_finalization_or_after_partial_media(self):
        for state, first_frame in ((4, 1), (6, 1), (7, 0), (5, 1), (2, 1)):
            with self.subTest(state=state):
                summary = result(self.capture([audio_job(slot=0, kind=0, state=state, stop_reason=9,
                                                        error=10, first_frame=first_frame)]))
                self.assertEqual(summary["failure_codes"], ["audio_job_failed"])
                self.assertEqual(summary["audio_jobs"]["slots"]["0"]["first_failure"]["enum_names"]["error"], "AE_OVERFLOW")

    def test_enum_boolean_slot_and_counter_domains_reject_poison(self):
        domains = {"slot": (-1, 4, 4294967295), "kind": (-1, 5), "state": (-1, 8, 4294967295),
                   "stop_reason": (-1, 16), "error": (-1, 19, -1515870811),
                   "first_frame": (-1, 2, 165), "cleanup": (-1, 2, 165)}
        for field in smoke.AUDIO_JOB_COUNTERS:
            domains[field] = (-1,)
        for field, invalid in domains.items():
            for value in invalid:
                with self.subTest(field=field, value=value):
                    summary = result(self.capture([audio_job(**{field: value})]))
                    self.assertEqual(summary["failure_codes"], ["invalid_audio_job_values"])
                    self.assertEqual(summary["audio_jobs"]["valid_reports"], 0)
                    self.assertEqual(summary["audio_jobs"]["slots"], {})

    def test_nonempty_capture_and_playback_slot_kinds_match_owner_layout(self):
        for slot, kind in ((0, 3), (1, 0), (2, 1), (3, 0), (3, 2)):
            with self.subTest(slot=slot, kind=kind):
                summary = result(self.capture([audio_job(slot=slot, kind=kind)]))
                self.assertEqual(summary["failure_codes"], ["invalid_audio_job_values"])
        for slot, kind in ((0, 0), (1, 1), (2, 2), (3, 3), (3, 4)):
            self.assertEqual(result(self.capture([audio_job(slot=slot, kind=kind)]))["failure_codes"], [])

    def test_missing_duplicate_malformed_and_overflow_fields_preserve_prior_failure(self):
        good = audio_job(state=7, stop_reason=11, error=13)
        malformed = [b"Audio diag job\n", good.replace(b"error=13", b"error=1.3"),
                     good.replace(b"error=13", b"error=0x0d"), good.replace(b"slot=3", b"slot=3 slot=0"),
                     good.replace(b"slot=3", b"slot=3 extra=1 extra=2"),
                     good.replace(b"received_bytes=0", b"received_bytes=18446744073709551616"),
                     good.replace(b"received_bytes=0", b"received_bytes=" + b"9" * 100)]
        for field in smoke.AUDIO_JOB_FIELDS:
            malformed.append(re.sub(rb"\b" + field.encode() + rb"=-?[0-9]+[ \t]*", b"", good))
        for line in malformed:
            with self.subTest(line=line):
                summary = result(self.capture([good, line]))
                self.assertEqual(summary["failure_codes"], ["audio_job_failed", "malformed_audio_job"])
                self.assertEqual(summary["audio_jobs"]["records"], 2)
                self.assertEqual(summary["audio_jobs"]["valid_reports"], 1)
                self.assertEqual(summary["audio_jobs"]["slots"]["3"]["last_report"]["enum_names"]["error"], "AE_TIMEOUT")

    def test_uint64_counters_remain_exact(self):
        maximum = (1 << 64) - 1
        counters = {field: maximum for field in smoke.AUDIO_JOB_COUNTERS}
        summary = result(self.capture([audio_job(state=2, first_frame=1, **counters)]))
        self.assertEqual(summary["failure_codes"], [])
        values = summary["audio_jobs"]["slots"]["3"]["last_report"]["values"]
        self.assertEqual({field: values[field] for field in counters}, counters)

    def test_repeated_reports_are_bounded_observations_not_distinct_job_counts(self):
        lines = [audio_job(slot=slot, kind=slot, state=7, stop_reason=11, error=13,
                           private_identifier=9321) for slot in range(4)] * 60
        summary = result(self.capture(lines))
        self.assertEqual(summary["failure_codes"], ["audio_job_failed"])
        self.assertEqual(summary["audio_jobs"]["valid_reports"], 240)
        self.assertEqual(len(summary["audio_jobs"]["slots"]), 4)
        for slot in summary["audio_jobs"]["slots"].values():
            self.assertEqual(slot["failed_reports"], 60)
        encoded = json.dumps(summary["audio_jobs"])
        self.assertLess(len(encoded), 8000)
        self.assertNotIn("private_identifier", encoded)
        self.assertNotIn("9321", encoded)

    def test_partial_last_report_still_records_failure_without_qualifying_capture(self):
        capture = self.capture([])
        line = audio_job(state=7, stop_reason=11, error=13).rstrip(b"\r\n")
        feed(capture, line)
        summary = result(capture)
        self.assertEqual(summary["failure_codes"], ["audio_job_failed", "unterminated_line"])

    def test_new_job_parsing_preserves_other_fault_gates(self):
        summary = result(self.capture([audio_job(state=7), b"E (12) other: ESP_ERR_TIMEOUT\n",
                                       runtime("media", audio_state=-1515870811)]))
        self.assertEqual(summary["failure_codes"], ["esp_error", "invalid_runtime_media_values"])


RUNTIME_VALUES = {
    "media": {"snapshot_available": 1, "initialized": 1, "audio_known": 1, "audio_state": 0,
              "audio_error": 0, "off_confirmed": 1, "speech_known": 1, "speech_state": 1,
              "speech_result": 0, "busy": 0, "result_known": 0, "result": -1},
    "wifi": {"initialized": 1, "state": 1, "lan_ready": 0, "backend_available": 0,
             "error": 0, "backend_error": 259},
    "control": {"initialized": 1, "server_state": 0, "tls": 0, "error": 0, "server_error": 0,
                "trust_known": 1, "trust_present": 0, "trust_uncertain": 0, "clock_known": 0},
}


def runtime(component, **overrides):
    values = dict(RUNTIME_VALUES[component], **overrides)
    label = "Wi-Fi" if component == "wifi" else component
    return ("I (10000) input: Runtime " + label + " " +
            " ".join(f"{key}={value}" for key, value in values.items()) + "\r\n").encode()


def motion(sample_count, legacy=False, **overrides):
    values = {"imu_available": 1, "imu_error": "ESP_OK", "samples": sample_count,
              "sample_age_known": 1, "sample_age_us": 1000,
              "calibrated": 0, "calibrating": 1, "calibration_samples": 0,
              "input_kind": 0, "input_connected": 0, "input_ready": 0}
    if not legacy:
        values.update(late_batches=0, late_batches_saturated=0)
    values.update(overrides)
    return ("I (10000) input: Motion " +
            " ".join(f"{key}={value}" for key, value in values.items()) + "\r\n").encode()


def recovering_motion(sample_count, **overrides):
    fields = {"phase": 1, "worker_running": 1, "recoveries": 0, "failure_streak": 0,
              "retry_ms": 0, "batch_time_known": 1, "batch_us": 1000,
              "max_batch_us": 1000, "schedule_late_us": 0}
    fields.update(overrides)
    return motion(sample_count, **fields)


class MotionParserTests(unittest.TestCase):
    def capture(self, records=None):
        records = records if records is not None else [motion(samples) for samples in (100, 1100, 2100)]
        capture = smoke.HealthCapture()
        for index, uptime in enumerate((10, 20, 30)):
            if index < len(records):
                for offset in range(0, len(records[index]), 7):
                    feed(capture, records[index][offset:offset + 7], uptime)
            feed(capture, health(uptime), uptime)
        for record in records[3:]:
            feed(capture, record, 30)
        return capture

    def test_freshness_limit_matches_the_production_board_contract(self):
        source = (ROOT / "firmware/main/board_mosaico.c").read_text()
        match = re.search(r"\bIMU_MAX_AGE_US\s*=\s*([0-9]+)", source)
        self.assertIsNotNone(match)
        self.assertEqual(smoke.MOTION_MAX_AGE_US, int(match.group(1)))

    def test_fragmented_progressing_motion_does_not_require_hid_or_calibration(self):
        for kind in (0, 1):
            with self.subTest(kind=kind):
                records = [motion(samples, input_kind=kind) for samples in (100, 1100, 2100)]
                summary = result(self.capture(records))
                self.assertEqual(summary["status"], "passed")
                self.assertEqual(summary["failure_codes"], [])
                observed = summary["motion"]
                self.assertEqual(observed["valid_samples"], 3)
                self.assertEqual(observed["samples"], {"first": 100, "last": 2100})
                self.assertEqual(observed["sample_progress"], {"advanced": 2, "stalled": 0,
                                                             "regressed": 0, "unverifiable": 0})
                self.assertEqual(observed["sample_age_us"], {"min": 1000, "max": 1000})
                self.assertEqual(observed["last_values"]["calibrated"], 0)
                self.assertEqual(observed["last_values"]["input_connected"], 0)
                self.assertEqual(observed["last_values"]["input_ready"], 0)

    def test_historical_absence_and_pre_late_counter_schema_remain_distinct(self):
        summary = result(self.capture([]))
        self.assertEqual(summary["status"], "passed")
        self.assertIsNone(summary["motion"])
        for legacy in (True, False):
            with self.subTest(legacy=legacy):
                summary = result(self.capture([motion(n, legacy=legacy) for n in (1, 2, 3)]))
                self.assertEqual(summary["status"], "passed")
                self.assertEqual("late_batches" in summary["motion"]["last_values"], not legacy)
        capture = self.capture([])
        feed(capture, b"I (30000) ui: Motion chooser remains open\n", 30)
        self.assertIsNone(result(capture)["motion"])

    def test_stalled_and_regressed_samples_fail_without_error_text(self):
        for counts, failure in [((20, 20, 20), "motion_samples_stalled"),
                                ((20, 19, 21), "motion_samples_regressed")]:
            with self.subTest(counts=counts):
                summary = result(self.capture([motion(n) for n in counts]))
                self.assertEqual(summary["failure_codes"], [failure])
                self.assertEqual(summary["motion"]["valid_samples"], 3)
                self.assertEqual(summary["motion"]["samples"]["last"], counts[-1])

    def test_freshness_boundary_is_independent_of_availability_or_error_text(self):
        for age, failures in ((50000, []), (50001, ["motion_sample_stale"])):
            with self.subTest(age=age):
                summary = result(self.capture([motion(n, sample_age_us=age) for n in (1, 2, 3)]))
                self.assertEqual(summary["failure_codes"], failures)
                self.assertEqual(summary["motion"]["sample_age_us"], {"min": age, "max": age})

    def test_unavailable_unknown_age_and_error_are_valid_failure_observations(self):
        cases = [({"imu_available": 0}, "motion_imu_unavailable"),
                 ({"imu_error": "ESP_FAIL"}, "motion_imu_error"),
                 ({"sample_age_known": 0, "sample_age_us": 0}, "motion_sample_age_unknown")]
        for overrides, failure in cases:
            with self.subTest(overrides=overrides):
                summary = result(self.capture([motion(n, **overrides) for n in (1, 2, 3)]))
                self.assertEqual(summary["failure_codes"], [failure])
                self.assertEqual(summary["motion"]["valid_samples"], 3)
                if not overrides.get("sample_age_known", 1):
                    self.assertIsNone(summary["motion"]["sample_age_us"])

    def test_missing_samples_never_become_fresh_success(self):
        summary = result(self.capture([motion(0, sample_age_known=0, sample_age_us=0)] +
                                      [motion(n) for n in (1, 2)]))
        self.assertIn("motion_no_samples", summary["failure_codes"])
        self.assertIn("motion_sample_age_unknown", summary["failure_codes"])
        self.assertNotIn("invalid_motion_values", summary["failure_codes"])

    def test_uint64_domains_and_saturation_do_not_wrap_or_invent_progress(self):
        maximum = (1 << 64) - 1
        summary = result(self.capture([motion(n, late_batches=maximum, late_batches_saturated=1)
                                       for n in (maximum - 2, maximum - 1, maximum)]))
        self.assertEqual(summary["status"], "passed")
        self.assertEqual(summary["motion"]["samples"]["last"], maximum)
        self.assertEqual(summary["motion"]["last_values"]["late_batches"], maximum)
        summary = result(self.capture([motion(maximum, sample_count_saturated=1) for _ in range(3)]))
        self.assertEqual(summary["failure_codes"], ["motion_progress_unverifiable"])
        self.assertEqual(summary["motion"]["valid_samples"], 3)
        self.assertEqual(summary["motion"]["sample_progress"]["unverifiable"], 2)
        summary = result(self.capture([motion(n, sample_age_us=maximum) for n in (1, 2, 3)]))
        self.assertEqual(summary["failure_codes"], ["motion_sample_stale"])

    def test_boolean_input_kind_and_unsigned_domains_reject_poison(self):
        for field in ("imu_available", "sample_age_known", "calibrated", "calibrating",
                      "input_connected", "input_ready", "late_batches_saturated", "sample_count_saturated"):
            for value in (-1, 2, 165):
                with self.subTest(field=field, value=value):
                    capture = self.capture()
                    feed(capture, motion(3100, **{field: value}), 30)
                    self.assertIn("invalid_motion_values", result(capture)["failure_codes"])
        for field, value in (("samples", -1), ("sample_age_us", -1), ("late_batches", -1),
                             ("calibration_samples", 1 << 32), ("input_kind", 2), ("input_kind", -1515870811)):
            with self.subTest(field=field, value=value):
                capture = self.capture()
                feed(capture, motion(3100, **{field: value}), 30)
                self.assertIn("invalid_motion_values", result(capture)["failure_codes"])

    def test_malformed_missing_duplicate_and_overflow_records_do_not_replace_valid_data(self):
        line = motion(3100)
        records = [line.replace(b"samples=3100", b"samples=nan"),
                   line.replace(b"samples=3100", b"samples=18446744073709551616"),
                   line.replace(b"samples=3100", b"samples=" + b"9" * 100),
                   line.replace(b"samples=3100 ", b""), line.rstrip() + b" samples=3100\n",
                   line.replace(b"imu_error=ESP_OK", b"imu_error=nan"),
                   line.rstrip() + b" imu_error=ESP_OK\n", b"Motion\n"]
        for record in records:
            with self.subTest(record=record):
                capture = self.capture()
                feed(capture, record, 30)
                summary = result(capture)
                self.assertIn("malformed_motion", summary["failure_codes"])
                self.assertEqual(summary["motion"]["valid_samples"], 3)
                self.assertEqual(summary["motion"]["records"], 4)
                self.assertEqual(summary["motion"]["samples"]["last"], 2100)

    def test_recovery_updates_last_values_but_preserves_observed_faults(self):
        records = [motion(20, imu_available=0, imu_error="ESP_FAIL", sample_age_us=1000000),
                   motion(21), motion(22)]
        capture = self.capture(records)
        feed(capture, motion(23), 40)
        feed(capture, health(40), 40)
        summary = capture.summary(45, 45, True)
        self.assertEqual(summary["failure_codes"], ["motion_imu_error", "motion_imu_unavailable", "motion_sample_stale"])
        self.assertEqual(summary["motion"]["last_values"]["imu_available"], 1)
        self.assertTrue(summary["motion"]["last_values"]["imu_error_ok"])
        self.assertEqual(summary["motion"]["samples"], {"first": 20, "last": 23})
        self.assertEqual(summary["motion"]["sample_age_us"]["max"], 1000000)

    def test_latched_first_fault_repetitions_do_not_become_new_fault_events(self):
        capture = self.capture()
        line = b"W (30000) input: First IMU fault stage=private_stage error=ESP_FAIL\n"
        for _ in range(9):
            feed(capture, line, 30)
        summary = result(capture)
        self.assertEqual(summary["failure_codes"], ["motion_fault_latched"])
        self.assertEqual(summary["motion"]["records"], 3)
        self.assertNotIn("private_stage", json.dumps(summary))

    def test_partial_motion_coverage_cannot_qualify_sustained_sampling(self):
        for count in (1, 2):
            with self.subTest(count=count):
                summary = result(self.capture([motion(n + 1) for n in range(count)]))
                self.assertIn("insufficient_motion_samples", summary["failure_codes"])

    def test_motion_cadence_stalls_fail_even_while_health_keeps_arriving(self):
        cases = [(range(10, 100, 10), (10, 20, 30), 90, "motion_final_silence"),
                 (range(10, 70, 10), (30, 40, 50), 60, "motion_initial_silence"),
                 (range(10, 70, 10), (10, 20, 50, 60), 60, "motion_gap_exceeded")]
        for health_times, motion_times, duration, failure in cases:
            with self.subTest(failure=failure):
                capture = smoke.HealthCapture()
                samples = 1
                for elapsed in health_times:
                    if elapsed in motion_times:
                        feed(capture, motion(samples), elapsed)
                        samples += 1
                    feed(capture, health(elapsed), elapsed)
                self.assertEqual(capture.summary(duration, duration, True)["failure_codes"], [failure])
        capture = smoke.HealthCapture()
        for count, elapsed in enumerate((25, 50, 75), 1):
            feed(capture, motion(count), elapsed)
            feed(capture, health(elapsed), elapsed)
        self.assertEqual(capture.summary(100, 100, True)["status"], "passed")

    def test_summary_does_not_copy_unknown_columns_or_symbolic_error_text(self):
        capture = self.capture()
        feed(capture, motion(3100, private_identity=12345, imu_error="ESP_ERR_PRIVATE_IDENTITY"), 30)
        summary = result(capture)
        self.assertIn("motion_imu_error", summary["failure_codes"])
        encoded = json.dumps(summary)
        self.assertNotIn("private_identity", encoded)
        self.assertNotIn("PRIVATE_IDENTITY", encoded)
        self.assertNotIn("12345", encoded)

    def test_optional_recovery_group_keeps_historical_batch_time_separate_from_freshness(self):
        maximum = (1 << 64) - 1
        records = [recovering_motion(n, phase=2, recoveries=maximum, failure_streak=1, retry_ms=10,
                                     batch_us=25000, max_batch_us=maximum, schedule_late_us=maximum)
                   for n in (1, 2, 3)]
        summary = result(self.capture(records))
        self.assertEqual(summary["status"], "passed")
        last = summary["motion"]["last_values"]
        self.assertEqual(last["phase_name"], "BOARD_IMU_RECOVERING")
        self.assertEqual(last["max_batch_us"], maximum)
        self.assertEqual(last["batch_us"], 25000)
        summary = result(self.capture([recovering_motion(n, batch_time_known=0) for n in (1, 2, 3)]))
        self.assertEqual(summary["status"], "passed")
        self.assertEqual(summary["motion"]["last_values"]["batch_time_known"], 0)

    def test_recovery_group_requires_complete_fields_and_typed_domains(self):
        line = recovering_motion(3100)
        fields = {"phase": 1, "worker_running": 1, "recoveries": 0, "failure_streak": 0,
                  "retry_ms": 0, "batch_time_known": 1, "batch_us": 1000,
                  "max_batch_us": 1000, "schedule_late_us": 0}
        for field, value in fields.items():
            with self.subTest(missing=field):
                capture = self.capture()
                record = line.replace(f" {field}={value}".encode(), b"", 1)
                feed(capture, record, 30)
                self.assertIn("malformed_motion", result(capture)["failure_codes"])
        cases = [("phase", -1), ("phase", 4), ("worker_running", 165), ("batch_time_known", 2),
                 ("failure_streak", 1 << 32), ("retry_ms", 1 << 32), ("recoveries", -1),
                 ("batch_us", -1), ("max_batch_us", -1), ("schedule_late_us", -1)]
        for field, value in cases:
            with self.subTest(field=field, value=value):
                capture = self.capture()
                feed(capture, recovering_motion(3100, **{field: value}), 30)
                self.assertIn("invalid_motion_values", result(capture)["failure_codes"])
        for field in ("recoveries", "batch_us", "max_batch_us", "schedule_late_us"):
            with self.subTest(overflow=field):
                capture = self.capture()
                feed(capture, recovering_motion(3100, **{field: 1 << 64}), 30)
                self.assertIn("malformed_motion", result(capture)["failure_codes"])

    def test_fatal_phase_or_stopped_worker_cannot_pass_even_with_fresh_counts(self):
        for overrides, failure in [({"phase": 3}, "motion_phase_fatal"),
                                   ({"worker_running": 0}, "motion_worker_stopped")]:
            with self.subTest(overrides=overrides):
                records = [recovering_motion(1, **overrides), recovering_motion(2), recovering_motion(3)]
                summary = result(self.capture(records))
                self.assertEqual(summary["failure_codes"], [failure])
                self.assertEqual(summary["motion"]["last_values"]["phase"], 1)
                self.assertEqual(summary["motion"]["valid_samples"], 3)


class RuntimeParserTests(unittest.TestCase):
    def capture(self, *records):
        capture = smoke.HealthCapture()
        for uptime in (10, 20, 30):
            feed(capture, health(uptime), uptime)
        for record in records:
            for offset in range(0, len(record), 7):
                feed(capture, record[offset:offset + 7], 30)
        return capture

    def test_enum_domains_match_the_owning_firmware_types(self):
        contracts = [
            ("components/audio_engine/include/audio_engine.h", "ae_state_t", smoke.AUDIO_STATES),
            ("components/audio_engine/include/audio_engine.h", "ae_job_kind_t", smoke.AUDIO_JOB_KINDS),
            ("components/audio_engine/include/audio_engine.h", "ae_job_state_t", smoke.AUDIO_JOB_STATES),
            ("components/audio_engine/include/audio_engine.h", "ae_stop_reason_t", smoke.AUDIO_STOP_REASONS),
            ("components/audio_engine/include/audio_io.h", "ae_result_t", smoke.AUDIO_RESULTS),
            ("components/spoken_letters/include/spoken_letters.h", "sl_state_t", smoke.SPEECH_STATES),
            ("firmware/main/app_wifi.h", "app_wifi_state_t", smoke.WIFI_STATES),
            ("firmware/main/app_control_server.h", "app_control_server_state_t", smoke.CONTROL_STATES),
            ("firmware/main/app_ui.h", "app_input_transport_t", smoke.MOTION_INPUT_KINDS),
            ("firmware/main/board_port.h", "board_imu_phase_t", smoke.MOTION_PHASES),
        ]
        for path, typedef, expected in contracts:
            with self.subTest(typedef=typedef):
                text = re.sub(r"/\*.*?\*/|//[^\n]*", "", (ROOT / path).read_text(), flags=re.S)
                match = re.search(r"typedef\s+enum\s*\{([^{}]*)\}\s*" + typedef + r"\s*;", text, re.S)
                self.assertIsNotNone(match)
                names = tuple(word.strip() for word in match.group(1).split(",") if word.strip())
                self.assertEqual(names, expected)  # Explicit values also require a reviewed parser update.
        audio_io = (ROOT / "components/audio_engine/include/audio_io.h").read_text()
        self.assertEqual(int(re.search(r"#define\s+AE_CONSUMERS_MAX\s+(\d+)U", audio_io).group(1)),
                         smoke.AUDIO_CAPTURE_SLOTS)

    def test_fragmented_idle_and_unprovisioned_observations_are_not_readiness_claims(self):
        summary = result(self.capture(*(runtime(component) for component in RUNTIME_VALUES)))
        self.assertEqual(summary["status"], "passed")
        self.assertEqual(summary["health_samples"], 3)
        self.assertEqual(summary["failure_codes"], [])
        self.assertEqual(summary["runtime_observations"]["wifi"]["enum_names"]["state"], "APP_WIFI_IDLE")
        self.assertEqual(summary["runtime_observations"]["wifi"]["last_values"]["backend_available"], 0)
        self.assertEqual(summary["runtime_observations"]["control"]["last_values"]["trust_present"], 0)
        self.assertEqual(summary["runtime_observations"]["control"]["enum_names"]["server_state"], "APP_CONTROL_STOPPED")
        self.assertEqual(summary["runtime_observations"]["media"]["enum_names"]["audio_state"], "AE_OFF")
        self.assertIn("do not qualify service readiness", summary["qualification"])

    def test_absent_runtime_records_remain_unobserved_for_historical_logs(self):
        summary = result(self.capture())
        self.assertEqual(summary["status"], "passed")
        self.assertEqual(summary["runtime_observations"], {})
        self.assertEqual(summary["failure_codes"], [])

    def test_valid_unknown_sentinels_and_legitimate_unavailable_speech(self):
        unknown = runtime("media", snapshot_available=0, initialized=0, audio_known=0,
                          audio_state=-1, audio_error=-1, off_confirmed=0, speech_known=0,
                          speech_state=-1, speech_result=-1)
        summary = result(self.capture(unknown, runtime("wifi", initialized=0, state=-1),
                                      runtime("control", initialized=0, server_state=-1, trust_known=0)))
        self.assertEqual(summary["status"], "passed")
        for component, observation in summary["runtime_observations"].items():
            self.assertEqual(observation["valid_samples"], 1)
            self.assertTrue(all(value is None for value in observation["enum_names"].values()), component)
        summary = result(self.capture(runtime("media", speech_state=0, speech_result=3,
                                               result_known=1, result=5)))
        self.assertEqual(summary["status"], "passed")
        names = summary["runtime_observations"]["media"]["enum_names"]
        self.assertEqual(names["speech_state"], "SL_UNAVAILABLE")
        self.assertEqual(names["speech_result"], "AE_UNAVAILABLE")
        self.assertEqual(names["result"], "AE_BUSY")

    def test_observed_a5_poison_fails_without_an_unrelated_error_log(self):
        poison = runtime("media", audio_state=-1515870811, audio_error=-1515870811, off_confirmed=165)
        capture = self.capture(poison)
        summary = result(capture)
        self.assertEqual(summary["status"], "failed")
        self.assertEqual(summary["failure_codes"], ["invalid_runtime_media_values"])
        self.assertEqual(summary["runtime_observations"], {})
        self.assertEqual(summary["health_samples"], 3)
        feed(capture, runtime("media"), 30)
        summary = result(capture)
        self.assertIn("invalid_runtime_media_values", summary["failure_codes"])
        self.assertEqual(summary["runtime_observations"]["media"]["valid_samples"], 1)

    def test_every_runtime_boolean_rejects_noncanonical_values(self):
        fields = {
            "media": ("snapshot_available", "initialized", "audio_known", "off_confirmed", "speech_known", "busy", "result_known"),
            "wifi": ("initialized", "lan_ready", "backend_available"),
            "control": ("initialized", "tls", "trust_known", "trust_present", "trust_uncertain", "clock_known"),
        }
        for component, keys in fields.items():
            for key in keys:
                for invalid in (-1, 2, 165):
                    with self.subTest(component=component, field=key, invalid=invalid):
                        summary = result(self.capture(runtime(component, **{key: invalid})))
                        self.assertEqual(summary["failure_codes"], ["invalid_runtime_" + component + "_values"])

    def test_known_enums_and_results_reject_unknown_or_out_of_domain_values(self):
        fields = [("media", "audio_state", 6), ("media", "audio_error", 19),
                  ("media", "speech_state", 8), ("media", "speech_result", 19),
                  ("media", "result", 19), ("wifi", "state", 7), ("control", "server_state", 5)]
        for component, key, limit in fields:
            for invalid in (-1515870811, -2, -1, limit):
                with self.subTest(component=component, field=key, invalid=invalid):
                    overrides = {key: invalid}
                    if key == "result":
                        overrides["result_known"] = 1
                    summary = result(self.capture(runtime(component, **overrides)))
                    self.assertEqual(summary["failure_codes"], ["invalid_runtime_" + component + "_values"])

    def test_unknown_flags_cannot_publish_success_shaped_values(self):
        cases = [runtime("media", audio_known=0), runtime("media", speech_known=0),
                 runtime("media", result=0), runtime("media", snapshot_available=0),
                 runtime("wifi", initialized=0), runtime("control", initialized=0)]
        for record in cases:
            with self.subTest(record=record):
                summary = result(self.capture(record))
                self.assertEqual(summary["status"], "failed")
                self.assertTrue(any(code.startswith("invalid_runtime_") for code in summary["failure_codes"]))

    def test_malformed_duplicate_and_missing_runtime_fields_fail(self):
        for component in RUNTIME_VALUES:
            line = runtime(component)
            label = "Wi-Fi" if component == "wifi" else component
            variants = [line.replace(b"initialized=1", b"initialized=nan"),
                        line.rstrip() + b" initialized=1\n", line.replace(b"initialized=1 ", b""),
                        line.replace(b"initialized=1", b"initialized=2147483648"),
                        line.replace(b"initialized=1", b"initialized=-2147483649"),
                        line.replace(b"initialized=1", b"initialized=" + b"9" * 100),
                        ("Runtime " + label + "\n").encode()]
            for record in variants:
                with self.subTest(component=component, record=record):
                    summary = result(self.capture(record))
                    self.assertEqual(summary["failure_codes"], ["malformed_runtime_" + component])
                    self.assertEqual(summary["runtime_observations"], {})

    def test_explicit_runtime_faults_are_distinct_from_corruption_and_sticky(self):
        cases = [("media", {"audio_state": 5, "audio_error": 11}),
                 ("media", {"speech_state": 7, "speech_result": 3}),
                 ("wifi", {"state": 5, "error": -1}),
                 ("control", {"server_state": 4, "error": -1})]
        for component, values in cases:
            with self.subTest(component=component, values=values):
                summary = result(self.capture(runtime(component, **values), runtime(component)))
                self.assertEqual(summary["failure_codes"], ["runtime_" + component + "_fault"])
                self.assertEqual(summary["runtime_observations"][component]["valid_samples"], 2)

    def test_runtime_summary_whitelists_fields_and_preserves_existing_fault_detection(self):
        capture = self.capture(runtime("media", private_identity=12345), b"E (42) battery: failure\n")
        summary = result(capture)
        self.assertEqual(summary["failure_codes"], ["esp_error"])
        self.assertNotIn("private_identity", json.dumps(summary))
        self.assertNotIn("12345", json.dumps(summary))


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

    def test_runtime_poison_completed_in_line_grace_still_fails(self):
        base = [(10, health(10)), (10, health(20)), (10, health(30))]
        line = runtime("media", audio_state=-1515870811, audio_error=-1515870811, off_confirmed=165)
        _, summary, _ = self.capture_events(base + [(5, line[:-1]), (0.4, line[-1:])])
        self.assertTrue(summary["capture_completed"])
        self.assertEqual(summary["capture_duration_seconds"], 35.4)
        self.assertEqual(summary["failure_codes"], ["invalid_runtime_media_values"])

    def test_stale_motion_completed_in_line_grace_still_fails(self):
        base = [(10, motion(1) + health(10)), (10, motion(2) + health(20)),
                (10, motion(3) + health(30))]
        line = motion(4, sample_age_us=50001)
        _, summary, _ = self.capture_events(base + [(5, line[:-1]), (0.4, line[-1:])])
        self.assertTrue(summary["capture_completed"])
        self.assertEqual(summary["capture_duration_seconds"], 35.4)
        self.assertEqual(summary["failure_codes"], ["motion_sample_stale"])
        self.assertEqual(summary["motion"]["valid_samples"], 4)

    def test_duration_has_documented_minimum_and_default(self):
        self.assertEqual(smoke.DEFAULT_SECONDS, 90)
        self.assertEqual(smoke.duration_argument("35"), 35)
        for value in ("34", "0", "-1", "nan", "35.5"):
            with self.subTest(value=value), self.assertRaises(smoke.argparse.ArgumentTypeError):
                smoke.duration_argument(value)


if __name__ == "__main__":
    unittest.main(verbosity=2)
