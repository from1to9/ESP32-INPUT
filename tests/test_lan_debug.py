#!/usr/bin/env python3
"""Real session/collector with explicit HTTP, Keychain and clock substitutes."""
import hashlib
import json
from pathlib import Path
import stat
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from lan_debug import CollectionError, collect
from control_bridge import CATALOG_DIGEST, DeviceSession
from secure_transport import BridgeError


class Clock:
    def __init__(self): self.now = 0
    def __call__(self): return self.now
    def sleep(self, duration): self.now += max(0.01, duration)


class Keychain:
    def get(self, service, account): return "b" * 64


class HTTP:
    def __init__(self, clock):
        self.clock, self.requests = clock, []
        self.runtime_status = 200
        self.break_path = None
        self.boot_change = False
        self.large = False

    def request(self, method, path, body=None, token=None):
        self.requests.append((method, path))
        self.clock.now += 0.01
        if path == self.break_path:
            raise BridgeError("Synthetic connection failure")
        if path == "/api/v1/sessions":
            return dict(http_status=200, session_token="c" * 64, session_id="d" * 32,
                        request_prefix="e" * 16, boot_id="f" * 32, next_seq=1)
        if path == "/api/v1/sessions/current":
            return dict(http_status=200)
        if path == "/api/v1/capabilities":
            return dict(http_status=200, catalog_sha256=CATALOG_DIGEST)
        if path == "/api/v1/state":
            return dict(http_status=200, protocol={"boot_id": ("0" if self.boot_change else "f") * 32})
        if path == "/api/v1/sensors/catalog":
            return dict(http_status=200, boot="1" * 32, git_commit="2" * 40,
                        git_state="clean", elf_sha256="3" * 64, config_sha256="4" * 64)
        if path == "/api/v1/diagnostics/runtime":
            return dict(http_status=self.runtime_status, schema=1, wifi={"known": False},
                        observed="x" * (3500 if self.large else 0))
        if path == "/api/v1/firmware/state":
            return dict(http_status=200, available=False, running_state_known=False,
                        reason="trust_layout_or_rollback_unavailable")
        if path == "/api/v1/clock":
            return dict(http_status=200, known=False, unix_seconds=None)
        raise AssertionError(path)


class LANCollectorTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.prefix = Path(self.temp.name) / "capture"
        self.clock = Clock()
        self.http = HTTP(self.clock)
        self.session = DeviceSession(dict(keychain_service="test", keychain_account="a" * 32,
                                          principal_id="a" * 32), self.http, Keychain())

    def run_capture(self, **kwargs):
        return collect(self.session, self.prefix, clock=self.clock, sleep=self.clock.sleep, **kwargs)

    def test_real_session_read_only_boundaries_and_unknown_health(self):
        receipt = self.run_capture(seconds=2, interval=0.5)
        self.assertTrue(receipt["collection_completed"])
        self.assertTrue(receipt["all_resources_observed"])
        self.assertFalse(receipt["device_health_qualified"])
        self.assertIsNone(receipt["logout_acknowledged"])
        self.assertGreater(receipt["rounds"], 1)
        # Telemetry identity and authenticated protocol boot are distinct.
        self.assertNotEqual(receipt["image_identity"]["boot"], receipt["authenticated_boot"])
        self.assertEqual([r for r in self.http.requests if r[0] != "GET"],
                         [("POST", "/api/v1/sessions"), ("DELETE", "/api/v1/sessions/current")])
        data = Path(str(self.prefix) + ".jsonl").read_bytes()
        self.assertEqual(receipt["data_sha256"], hashlib.sha256(data).hexdigest())
        self.assertNotIn(b"b" * 64, data)
        self.assertNotIn(b"c" * 64, data)
        responses = [json.loads(line)["response"] for line in data.splitlines()]
        self.assertIn(dict(http_status=200, known=False, unix_seconds=None), responses)
        for suffix in (".jsonl", ".receipt.json"):
            self.assertEqual(stat.S_IMODE(Path(str(self.prefix) + suffix).stat().st_mode), 0o600)

    def test_unimplemented_or_forbidden_runtime_is_explicit(self):
        for status in (403, 501, 503):
            with self.subTest(status=status):
                self.prefix = Path(self.temp.name) / str(status)
                self.http.runtime_status = status
                self.session = DeviceSession(self.session.profile, self.http, Keychain())
                result = self.run_capture()
                self.assertTrue(result["collection_completed"])
                self.assertFalse(result["all_resources_observed"])
                self.assertIn("/api/v1/diagnostics/runtime:http_" + str(status), result["issues"])

    def test_session_loss_is_not_reauthenticated_or_replayed(self):
        self.http.runtime_status = 401
        result = self.run_capture(seconds=30)
        self.assertFalse(result["collection_completed"])
        self.assertEqual(result["error"], "session_lost")
        self.assertEqual(self.http.requests.count(("POST", "/api/v1/sessions")), 1)

    def test_boot_change_fails_and_retains_received_evidence(self):
        self.http.boot_change = True
        result = self.run_capture()
        self.assertEqual(result["error"], "boot_identity_changed_or_missing")
        self.assertEqual(result["responses"], 2)
        self.assertTrue(Path(str(self.prefix) + ".jsonl").stat().st_size)

    def test_disconnect_keeps_partial_data_and_receipt(self):
        self.http.break_path = "/api/v1/clock"
        result = self.run_capture()
        self.assertEqual(result["error"], "authenticated_connection_failed")
        self.assertFalse(result["collection_completed"])
        self.assertGreater(result["responses"], 0)
        self.assertTrue(self.session.closed)

    def test_output_collision_never_opens_session(self):
        path = Path(str(self.prefix) + ".jsonl")
        path.write_text("old evidence")
        with self.assertRaises(CollectionError): self.run_capture()
        self.assertEqual(path.read_text(), "old evidence")
        self.assertEqual(self.http.requests, [])

    def test_output_limit_preserves_partial_without_false_completion(self):
        self.http.large = True
        result = self.run_capture(seconds=60, interval=0.5, max_bytes=65536)
        self.assertEqual(result["error"], "output_limit")
        self.assertFalse(result["collection_completed"])
        self.assertLessEqual(sum(Path(str(self.prefix) + s).stat().st_size
                                 for s in (".jsonl", ".receipt.json")), 65536)

    def test_nonfinite_or_unbounded_arguments_never_connect(self):
        for options in ({"seconds": float("nan")}, {"seconds": 601}, {"interval": 0.1},
                        {"interval": float("inf")}, {"max_bytes": 100}):
            with self.subTest(options=options), self.assertRaises(CollectionError):
                self.run_capture(**options)
        self.assertEqual(self.http.requests, [])


if __name__ == "__main__":
    unittest.main()
