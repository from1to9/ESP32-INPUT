#!/usr/bin/env python3
"""Exercise real companion code with substituted Keychain/HTTP/serial boundaries."""
import asyncio
import hashlib
import json
from pathlib import Path
import stat
import struct
import subprocess
import sys
import tempfile
import time
import threading
import unittest
from unittest.mock import patch
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec, utils

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "bridge"))
from commission import create_package, send_package
from control_bridge import CATALOG_DIGEST, DeviceSession, TypedValidator, TOOLS, FORMAT
from secure_transport import BridgeError, PinnedHTTPS, sensor_samples_path
from sign_release import sign_release

class FakeKeychain:
    def __init__(self): self.values = {}
    def put(self, service, account, secret): self.values[service, account] = secret
    def get(self, service, account): return self.values[service, account]

class FakeHTTP:
    def __init__(self): self.commands = []; self.fail_once = False; self.sensor_reads = []
    def request(self, method, path, body=None, token=None):
        if path == "/api/v1/sessions":
            return dict(http_status=200, session_token="3"*64, session_id="4"*32,
                        request_prefix="5"*16, boot_id="6"*32, next_seq=1)
        if path == "/api/v1/capabilities": return dict(http_status=200, catalog_sha256=CATALOG_DIGEST)
        if path == "/api/v1/sessions/current": return dict(http_status=200, authenticated=True, state="completed")
        if path == "/api/v1/state":
            return dict(http_status=200, protocol={"boot_id": "6"*32, "now_ms": "1000", "lease_active": True, "lease_id": "7"*32})
        if path.startswith("/api/v1/sensors/"):
            assert method == "GET" and token == "3"*64 and body is None
            self.sensor_reads.append(path)
            return dict(http_status=200, schema=1)
        if path == "/api/v1/commands":
            self.commands.append(json.loads(json.dumps(body)))
            if self.fail_once:
                self.fail_once = False
                raise BridgeError("Synthetic unknown connection outcome")
            return dict(http_status=202, state="accepted", command_key="100", next_seq=body["seq"]+1)
        raise AssertionError(path)

class FakeSerial:
    def __init__(self): self.responses = []; self.received = bytearray(); self.expected = 0; self.digest = ""; self.ended = False
    def write(self, line):
        parts = line.strip().split()
        assert parts[0] == b"CP1"
        if parts[1] == b"CHALLENGE": self.responses.append(b"CP1 TOKEN " + b"1"*64 + b"\n")
        elif parts[1] == b"BEGIN":
            assert parts[2] == b"1"*64
            self.expected, self.digest = int(parts[3]), parts[4].decode()
            self.responses.append(b"CP1 READY\n")
        elif parts[1] == b"DATA":
            assert int(parts[2]) == len(self.received)
            self.received.extend(bytes.fromhex(parts[3].decode()))
            self.responses.append(b"CP1 ACK\n")
        elif parts[1] == b"END":
            assert len(self.received) == self.expected and hashlib.sha256(self.received).hexdigest() == self.digest
            self.ended = True
            self.responses.append(b"CP1 PENDING_REVIEW\n")
        else: raise AssertionError("USB cannot approve Review")
    def readline(self, limit): return self.responses.pop(0)

class CompanionTests(unittest.TestCase):
    def test_cancelled_async_reader_keeps_worker_session_ownership(self):
        entered, release, finished, close_attempt = [threading.Event() for _ in range(4)]
        keychain = FakeKeychain(); keychain.values["test", "1"*32] = "2"*64
        class BlockingHTTP(FakeHTTP):
            deleted = False
            def request(self, method, path, body=None, token=None):
                if path == "/api/v1/state":
                    entered.set()
                    if not release.wait(2): raise AssertionError("Reader was not released")
                    finished.set()
                if method == "DELETE": self.deleted = True
                return super().request(method, path, body, token)
        http = BlockingHTTP()
        session = DeviceSession(dict(keychain_service="test", keychain_account="1"*32,
                                     principal_id="1"*32), http, keychain)
        session.connect()
        def close():
            close_attempt.set()
            session.close()
        async def scenario():
            reader = asyncio.create_task(asyncio.to_thread(session.read, "/api/v1/state"))
            self.assertTrue(await asyncio.to_thread(entered.wait, 1))
            reader.cancel()
            with self.assertRaises(asyncio.CancelledError): await reader
            closer = asyncio.create_task(asyncio.to_thread(close))
            self.assertTrue(await asyncio.to_thread(close_attempt.wait, 1))
            try:
                await asyncio.sleep(0.02)
                self.assertFalse(http.deleted)
                self.assertFalse(session.closed)
            finally:
                release.set()
            await asyncio.wait_for(closer, 2)
        asyncio.run(scenario())
        self.assertTrue(finished.is_set() and http.deleted and session.closed)

    def test_transport_close_waits_for_complete_response(self):
        entered, release, closing = [threading.Event() for _ in range(3)]
        transport = PinnedHTTPS.__new__(PinnedHTTPS)
        transport.lock = threading.RLock()
        transport.host, transport.port, transport.context = "device.test", 443, object()
        transport.digest = hashlib.sha256(b"leaf").hexdigest()
        class Response:
            status = 200
            def getheader(self, name, default=None):
                return {"Content-Type": "application/json", "Content-Length": "2"}.get(name, default)
            def read(self, maximum):
                entered.set()
                if not release.wait(2): raise AssertionError("Socket reader was not released")
                return b"{}"
        class Connection:
            closed = False
            def __init__(self): self.sock = self
            def connect(self): pass
            def getpeercert(self, binary_form): return b"leaf"
            def request(self, *args): pass
            def getresponse(self): return Response()
            def close(self): self.closed = True
        connection = Connection(); errors = []
        def read():
            try: transport.request("GET", "/api/v1/state")
            except Exception as error: errors.append(error)
        def close():
            closing.set(); transport.close()
        with patch("secure_transport.http.client.HTTPSConnection", return_value=connection):
            reader = threading.Thread(target=read); reader.start()
            self.assertTrue(entered.wait(1))
            closer = threading.Thread(target=close); closer.start()
            self.assertTrue(closing.wait(1))
            try:
                time.sleep(0.02)
                self.assertFalse(connection.closed)
            finally:
                release.set(); reader.join(2); closer.join(2)
            self.assertFalse(reader.is_alive() or closer.is_alive())
        self.assertEqual(errors, [])
        self.assertTrue(connection.closed)

    def test_pending_read_is_polled_without_replaying_mutations(self):
        keychain = FakeKeychain(); keychain.values["test", "1"*32] = "2"*64
        class PendingHTTP(FakeHTTP):
            state_reads = 0
            def request(self, method, path, body=None, token=None):
                if path == "/api/v1/state":
                    self.state_reads += 1
                    if self.state_reads == 1:
                        return dict(http_status=429, error="busy")
                return super().request(method, path, body, token)
        http = PendingHTTP()
        session = DeviceSession(dict(keychain_service="test", keychain_account="1"*32,
                                     principal_id="1"*32), http, keychain)
        session.connect()
        with patch("time.sleep"):
            result = session.read("/api/v1/state")
        self.assertEqual(result["http_status"], 200)
        self.assertEqual(http.state_reads, 2)
        self.assertEqual(http.commands, [])

    def test_busy_read_has_a_bound_and_other_failures_are_not_retried(self):
        keychain = FakeKeychain(); keychain.values["test", "1"*32] = "2"*64
        http = FakeHTTP()
        session = DeviceSession(dict(keychain_service="test", keychain_account="1"*32,
                                     principal_id="1"*32), http, keychain)
        session.connect()
        for status, error, count in ((429, "busy", 4), (503, "busy", 4),
                                     (503, "not_implemented", 1), (401, "unauthenticated", 1)):
            with self.subTest(status=status, error=error), patch("time.sleep"), \
                 patch.object(http, "request", return_value=dict(http_status=status, error=error)) as send:
                self.assertEqual(session.read("/api/v1/state")["http_status"], status)
                self.assertEqual(send.call_count, count)
                self.assertEqual(session.read_attempts, count)
                self.assertTrue(all(call.args[0] == "GET" for call in send.call_args_list))

    def test_sensor_typed_reads_and_canonical_query(self):
        keychain = FakeKeychain(); keychain.values["test", "1"*32] = "2"*64
        http = FakeHTTP(); profile = dict(keychain_service="test", keychain_account="1"*32, principal_id="1"*32)
        session = DeviceSession(profile, http, keychain)
        for name in ("sensors_catalog", "sensors_latest"):
            self.assertTrue(next(t for t in TOOLS if t["name"] == name)["annotations"]["readOnlyHint"])
            session.invoke(name, {"context": {}, "args": {}})
        arguments = {"context": {}, "args": {"boot": "a"*32, "after": "18446744073709551615"}}
        session.invoke("sensors_samples", arguments)
        self.assertEqual(http.sensor_reads[-1], "/api/v1/sensors/samples?boot="+"a"*32+"&after=18446744073709551615&limit=8")
        self.assertTrue(sensor_samples_path(http.sensor_reads[-1])); self.assertEqual(http.commands, [])
        for cursor in ("00", "18446744073709551616", "-1", "1&limit=1"):
            with self.assertRaises(BridgeError):
                session.invoke("sensors_samples", {"context": {}, "args": {"boot": "a"*32, "after": cursor}})
        self.assertEqual(len(http.sensor_reads), 3)
        for suffix in ("&x=1", "&limit=1", "/"):
            self.assertFalse(sensor_samples_path(http.sensor_reads[-1] + suffix))

    def test_sensor_raw_evidence_uses_existing_verified_tls_boundary(self):
        transport = PinnedHTTPS.__new__(PinnedHTTPS)
        transport.lock = threading.RLock()
        transport.host, transport.port, transport.context = "device.test", 443, object()
        transport.digest = hashlib.sha256(b"synthetic-leaf").hexdigest()
        class Response:
            status = 403
            raw = b'{"error":"forbidden_scope","http_status":200}'
            headers = {"Content-Type": "application/json", "Content-Length": str(len(raw))}
            def getheader(self, name, default=None): return self.headers.get(name, default)
            def read(self, maximum): self.maximum = maximum; return self.raw
        class Socket:
            def getpeercert(self, binary_form=False): assert binary_form; return b"synthetic-leaf"
        class Connection:
            def __init__(self): self.sock = Socket(); self.response = Response(); self.closed = False; self.calls = []; self.connects = 0
            def connect(self): self.connects += 1
            def request(self, *args): self.calls.append(args)
            def getresponse(self): return self.response
            def close(self): self.closed = True
        connection = Connection()
        with patch("secure_transport.http.client.HTTPSConnection", return_value=connection):
            status, raw = transport.sensor_get_raw("/api/v1/sensors/catalog", "1"*64)
            self.assertEqual((status, raw), (403, Response.raw)); self.assertFalse(connection.closed)
            self.assertEqual(connection.response.maximum, 4097)
            self.assertEqual(transport.request("GET", "/api/v1/sensors/catalog", token="1"*64)["http_status"], 403)
            self.assertEqual(connection.connects, 1)
            self.assertTrue(all(call[3]["Connection"] == "keep-alive" for call in connection.calls))
            transport.close(); self.assertTrue(connection.closed)
            connection.response.headers = {"Content-Type": "application/json", "Content-Length": "4096"}
            with self.assertRaisesRegex(BridgeError, "Incomplete"):
                transport.sensor_get_raw("/api/v1/sensors/latest", "1"*64)
            connection.response.headers = {"Content-Type": "application/json", "Content-Encoding": "gzip"}
            with self.assertRaises(BridgeError): transport.sensor_get_raw("/api/v1/sensors/latest", "1"*64)
            connection.response.headers = {"Content-Type": "application/json"}; connection.response.status = 302
            with self.assertRaisesRegex(BridgeError, "redirect"): transport.sensor_get_raw("/api/v1/sensors/latest", "1"*64)
            connection.response.status = 200; connection.response.raw = b"x"*4097
            with self.assertRaisesRegex(BridgeError, "exceeds"): transport.sensor_get_raw("/api/v1/sensors/latest", "1"*64)
            before = len(connection.calls); transport.digest = "0"*64
            with self.assertRaisesRegex(BridgeError, "pin mismatch"): transport.sensor_get_raw("/api/v1/sensors/catalog", "1"*64)
            self.assertEqual(len(connection.calls), before)
        with self.assertRaises(BridgeError): transport.sensor_get_raw("/api/v1/trust", "1"*64)
        with self.assertRaises(BridgeError): transport.sensor_get_raw("/api/v1/sensors/catalog", "")

    def test_motion_recenter_uses_observed_input_lease_and_exact_retry(self):
        tool = next(tool for tool in TOOLS if tool["name"] == "input_motion_recenter")
        self.assertEqual(tool["x-device-operation"], "input.motion.recenter")
        self.assertEqual(tool["x-required-scopes"], ["input.control"])
        validator = TypedValidator(tool["inputSchema"], format_checker=FORMAT)
        self.assertTrue(validator.is_valid({"context": {}, "args": {}}))
        self.assertFalse(validator.is_valid({"context": {}, "args": {"enabled": True}}))
        with tempfile.TemporaryDirectory(prefix="control-recenter-") as directory:
            keychain = FakeKeychain()
            destination = Path(directory) / "owner"
            create_package(destination, "device.test", 443, ["view", "input.control"],
                           ["clock.example"], [], keychain)
            profile = json.loads((destination / "client-profile.json").read_text())
            http = FakeHTTP()
            session = DeviceSession(profile, http, keychain)
            arguments = {"context": {"epoch": "2", "transport_generation": "7",
                                     "target_id": "test-target"}, "args": {}}
            http.fail_once = True
            with self.assertRaises(BridgeError):
                session.invoke("input_motion_recenter", arguments)
            with self.assertRaises(BridgeError):
                session.invoke("input_motion_recenter", arguments)
            session.invoke("bridge_retry_pending", {})
            self.assertEqual(len(http.commands), 2)
            self.assertEqual(http.commands[0], http.commands[1])
            self.assertEqual(http.commands[0]["op"], "input.motion.recenter")
            self.assertEqual(http.commands[0]["args"], {})
            self.assertEqual(http.commands[0]["lease_id"], "7" * 32)
            session.close()

    def test_private_enrollment_and_protocol(self):
        with tempfile.TemporaryDirectory(prefix="control-private-") as directory:
            keychain = FakeKeychain(); destination = Path(directory) / "owner"
            summary = create_package(destination, "device.test", 443, ["view", "audio.capture"],
                ["clock.example"], [{"expires_unix_seconds": str(int(time.time())+3600), "source": "local_microphone", "purpose": "live_capture", "maximum_bytes": "8589934592", "maximum_duration_seconds": 28800, "maximum_retention_seconds": 0, "live_destination": True, "file_destination": False, "owner_download": False}], keychain)
            package = (destination / "device-package.json").read_bytes(); decoded = json.loads(package)
            profile = json.loads((destination / "client-profile.json").read_bytes())
            secret = next(iter(keychain.values.values()))
            self.assertNotIn(secret.encode(), package)
            self.assertEqual(hashlib.sha256(bytes.fromhex(secret)).hexdigest(), decoded["principals"][0]["credential_sha256"])
            self.assertEqual(stat.S_IMODE(destination.stat().st_mode), 0o700)
            self.assertEqual(stat.S_IMODE((destination / "device-package.json").stat().st_mode), 0o600)
            self.assertFalse((destination / "client-secret-backup.json").exists())
            certificate = x509.load_pem_x509_certificate(profile["certificate_pem"].encode())
            self.assertEqual(certificate.fingerprint(hashes.SHA256()).hex(), profile["certificate_der_sha256"])
            PinnedHTTPS(profile)  # Constructs real SSL verification with explicit CA/hostname/pin.
            changed = dict(profile, certificate_der_sha256="0"*64)
            with self.assertRaises(BridgeError): PinnedHTTPS(changed)
            serial = FakeSerial(); self.assertEqual(send_package(serial, package), "pending_local_review")
            self.assertEqual(bytes(serial.received), package); self.assertTrue(serial.ended)
            self.assertEqual(summary["capture_grants"], [decoded["capture_grants"][0]["id"]])
            http = FakeHTTP(); session = DeviceSession(profile, http, keychain)
            http.fail_once = True
            arguments = {"context": {"management_generation": "1", "resource_revision": "1"}, "args": {}}
            with self.assertRaises(BridgeError): session.invoke("wifi_scan_start", arguments)
            with self.assertRaises(BridgeError): session.invoke("wifi_scan_start", arguments)
            result = session.invoke("bridge_retry_pending", {})
            self.assertEqual(http.commands[0], http.commands[1]); self.assertEqual(result["state"], "accepted")
            self.assertNotIn("lease_id", http.commands[0])  # Management never borrows HID lease fields.
            self.assertEqual(session.sequence, 2)
            with self.assertRaises(BridgeError): session.invoke("wifi_join", {"context": {}, "args": {"password": "do-not-echo"}})
            self.assertEqual(len(http.commands), 2)
            session.close()

    def test_exact_decimal_schema(self):
        schema = next(tool["inputSchema"] for tool in TOOLS if tool["name"] == "config_stage")
        for value in (.25, 4.01, 4.02, 100):
            TypedValidator(schema).validate({"context": {}, "args": {"patch": {"pointer.gain_x_hid_per_degree": value}}})
        from jsonschema import ValidationError
        for value in (.24, 4.001, 100.01):
            with self.assertRaises(ValidationError):
                TypedValidator(schema).validate({"context": {}, "args": {"patch": {"pointer.gain_x_hid_per_degree": value}}})

    def test_wire_formats_before_authentication(self):
        from jsonschema import ValidationError
        schema = next(tool["inputSchema"] for tool in TOOLS if tool["name"] == "wifi_join")
        validator = TypedValidator(schema, format_checker=FORMAT)
        good = {"context": {}, "args": {"ssid": "Example", "password": "valid passphrase", "remember": False}}
        for value in ("0", "1", "18446744073709551615"):
            validator.validate({**good, "context": {"management_generation": value}})
        invalid = []
        for value in ("00", "01", "+1", " 1", "1 ", "18446744073709551616", "\u0661"):
            invalid.append({**good, "context": {"management_generation": value}})
        for value in ("x", "1234567", "long\nsecret", "long\x7fsecret", "secret\u00e9x", "a" * 64):
            invalid.append({**good, "args": {**good["args"], "password": value}})
        for value in ("", "bad\tname", "bad\x7fname", "\u00e9" * 17, "bad\ud800"):
            invalid.append({**good, "args": {**good["args"], "ssid": value}})
        validator.validate({**good, "args": {**good["args"], "ssid": "\u00e9" * 16}})
        # No profile/Keychain/HTTP is supplied: rejection must precede any login
        # or transport work and must not echo invalid secrets in the public error.
        session = DeviceSession({}, None, None)
        for arguments in invalid:
            with self.subTest(arguments=arguments):
                with self.assertRaises(ValidationError):
                    validator.validate(arguments)
                with self.assertRaisesRegex(BridgeError, "^Tool arguments violate the typed device schema$"):
                    session.invoke("wifi_join", arguments)
                self.assertIsNone(session.login)
                self.assertIsNone(session.pending)
        for kind, good_value, bad_values in (
            ("ansi_text", "Hello!", ("\u00e9", "line\nfeed", "\x7f")),
            ("device_name", "Desk \u00e9", (" leading", "trailing ", "bad\u0085", "")),
        ):
            text_validator = TypedValidator({"type": "string", "format": kind,
                "minLength": 1, "maxLength": 18}, format_checker=FORMAT)
            text_validator.validate(good_value)
            for value in (*bad_values, "\u00e9" * 10):
                with self.subTest(format=kind, value=value), self.assertRaises(ValidationError):
                    text_validator.validate(value)

    def test_real_signing(self):
        image = bytearray(512); image[0] = 0xE9
        struct.pack_into("<H", image, 12, 0x20); struct.pack_into("<I", image, 28, 480)
        struct.pack_into("<I", image, 32, 0xABCD5432); image[48:53] = b"2.0.0"; image[80:91] = b"esp32_input"
        key = ec.generate_private_key(ec.SECP256R1())
        pem = key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption())
        catalog = sign_release(bytes(image), pem, key_id="publisher", product="input", board="mosaico",
                               release_id="release-2", release_index=2, url="https://publisher.example/image.bin", expires=int(time.time())+3600)
        signature = bytes.fromhex(catalog["signature"])
        der = utils.encode_dss_signature(int.from_bytes(signature[:32], "big"), int.from_bytes(signature[32:], "big"))
        key.public_key().verify(der, catalog["payload"].encode(), ec.ECDSA(hashes.SHA256()))
        payload = json.loads(catalog["payload"])
        self.assertEqual(payload["project"], "esp32_input"); self.assertEqual(payload["version"], "2.0.0")
        self.assertEqual(payload["layout"], "esp32s31-16m-ota6-v3")
        self.assertEqual(payload["settings_schema"], 5)
        self.assertEqual(payload["sha256"], hashlib.sha256(image).hexdigest())
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            (directory / "image.bin").write_bytes(image)
            (directory / "publisher.pem").write_bytes(pem)
            for schema in (None, 4):
                output = directory / f"catalog-{schema}.json"
                command = [sys.executable, str(ROOT / "bridge/sign_release.py"), "sign",
                    "--image", str(directory / "image.bin"), "--private-key", str(directory / "publisher.pem"),
                    "--output", str(output), "--key-id", "publisher", "--product", "input", "--board", "mosaico",
                    "--release-id", "release-2", "--release-index", "2", "--url", "https://publisher.example/image.bin",
                    "--expires", str(int(time.time()) + 3600)]
                if schema is not None: command += ["--settings-schema", str(schema)]
                subprocess.run(command, check=True, capture_output=True)
                signed = json.loads(output.read_text())
                self.assertEqual(json.loads(signed["payload"])["settings_schema"], 5 if schema is None else 4)
                signature = bytes.fromhex(signed["signature"])
                der = utils.encode_dss_signature(int.from_bytes(signature[:32], "big"), int.from_bytes(signature[32:], "big"))
                key.public_key().verify(der, signed["payload"].encode(), ec.ECDSA(hashes.SHA256()))
        with self.assertRaises(BridgeError):
            sign_release(bytes(image), pem, key_id="publisher", product="input", board="mosaico",
                release_id="release-2", release_index=2, url="https://publisher.example/image.bin",
                expires=int(time.time())+3600, settings_schema=6)
        with self.assertRaises(BridgeError): sign_release(b"wrong", pem, key_id="publisher", product="input", board="mosaico", release_id="release-2", release_index=2, url="https://publisher.example/image.bin", expires=int(time.time())+3600)

    def test_actual_mcp_stdio(self):
        async def exercise(profile):
            from mcp import ClientSession, StdioServerParameters
            from mcp.client.stdio import stdio_client
            parameters = StdioServerParameters(command=sys.executable, args=[str(ROOT / "bridge/control_bridge.py"), "--profile", str(profile)])
            async with stdio_client(parameters) as (reader, writer):
                async with ClientSession(reader, writer) as client:
                    await client.initialize(); listing = await client.list_tools()
                    self.assertIn("wifi_join", {tool.name for tool in listing.tools})
                    self.assertIn("bridge_retry_pending", {tool.name for tool in listing.tools})
        with tempfile.TemporaryDirectory(prefix="control-mcp-") as directory:
            destination = Path(directory) / "owner"
            create_package(destination, "device.test", 443, ["view"], [], [], FakeKeychain())
            asyncio.run(exercise(destination / "client-profile.json"))

if __name__ == "__main__":
    unittest.main(verbosity=2)
