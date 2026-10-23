#!/usr/bin/env python3
"""Exercise the real collector and production encoders, never devices or Keychain."""
import base64
import collections
import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import ssl
import stat
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zlib

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "bridge"))
import sensor_capture as collector
import secure_transport

BOOT = "0102030405060708090a0b0c0d0e0f10"
RID = "1234567890abcdef"
FIXTURE = os.environ["SENSOR_FIXTURE"]


def fixture(command, *, mode="normal", transport="usb"):
    program = os.environ["SENSOR_ARCHIVE_FIXTURE"] if mode == "archive" else FIXTURE
    return subprocess.check_output([program, mode, transport, command])


def batch(after=0, limit=3, *, mode="normal", rid=RID):
    wire = fixture(f"ST1 READ {rid} {BOOT} {after} {limit}", mode=mode)
    return base64.b64decode(wire.split()[4], validate=True)


def changed(raw, offset, encoded):
    raw = bytearray(raw)
    raw[offset:offset + len(encoded)] = encoded
    raw[-4:] = struct.pack("<I", zlib.crc32(raw[:-4]))
    return bytes(raw)


class Clock:
    def __init__(self): self.now = 0.0
    def __call__(self): return self.now
    def sleep(self, amount): self.now += max(0, amount)


class FakeSerial:
    """Only the serial driver is substituted; replies use production C codecs."""
    def __init__(self, clock, *, mode="normal", fault=None):
        self.clock, self.mode, self.fault = clock, mode, fault
        self.is_open = False
        self.buffer = b""
        self.requests = []
        self.dtr = self.rts = True
        self.opens = self.closes = self.sample_requests = 0
        self.catalog_requests = 0
        self.read_calls = []

    def open(self):
        assert not self.dtr and not self.rts
        self.is_open = True
        self.opens += 1

    def close(self): self.is_open = False; self.closes += 1

    def write(self, data):
        assert data.startswith(b"\nST1 ") and data.endswith(b"\n")
        request = data.strip().decode()
        assert request.split()[1] in ("CATALOG", "LATEST", "READ")
        self.requests.append(request)
        if " CATALOG " in request: self.catalog_requests += 1
        if " READ " in request:
            self.sample_requests += 1
            if self.fault == "timeout_once" and self.sample_requests == 1:
                return len(data)
            if self.fault == "short_once" and self.sample_requests == 1:
                return 4
            if self.fault == "disconnect":
                self.is_open = False
                return len(data)
        mode = "image" if self.fault == "image_changed" and self.catalog_requests > 1 else self.mode
        wire = fixture(request, mode=mode)
        if " READ " in request and self.fault in ("boot_changed", "schema", "bad_crc"):
            fields = wire.split()
            raw = base64.b64decode(fields[4])
            if self.fault == "boot_changed": raw = changed(raw, 16, b"\xff"*16)
            elif self.fault == "schema": raw = changed(raw, 0, b"\x02")
            else: raw = raw[:-1] + bytes([raw[-1] ^ 1])
            wire = b"\n" + b" ".join(fields[:4] + [base64.b64encode(raw)]) + b"\n"
        self.buffer += b"ordinary log before frame\n" + wire + b"ordinary log after frame\n"
        return len(data)

    def read(self, length):
        self.read_calls.append((length, len(self.buffer)))
        # Model real pyserial: a request larger than ready input waits through
        # the timeout, even when that input contains a complete ST1 response.
        self.clock.now += 0.001 if len(self.buffer) >= length else min(self.timeout, 0.1)
        result, self.buffer = self.buffer[:length], self.buffer[length:]
        return result

    @property
    def in_waiting(self): return len(self.buffer)

    def reset_input_buffer(self): raise AssertionError("input discard forbidden")
    def flush(self): raise AssertionError("flush forbidden")


class SerialModule:
    def __init__(self, device): self.device = device
    def Serial(self, **kwargs):
        assert kwargs["port"] is None and not any(kwargs[x] for x in ("rtscts", "dsrdtr", "xonxoff"))
        return self.device


class CollectorTests(unittest.TestCase):
    def decode(self, raw, after=0, *, rid=RID, limit=3):
        return collector.decode_batch(raw, BOOT, after, int(rid, 16), limit)

    def test_real_encoder_all_kinds_and_unknown_values(self):
        wrapped = json.loads(fixture(collector.SENSOR_ROOT + f"samples?boot={BOOT}&after=0&limit=8", transport="http"))
        result = collector.decode_batch(base64.b64decode(wrapped["data"]), BOOT, 0, 0, 8)
        records = result["records"]
        self.assertEqual([r["kind"] for r in records], ["imu_sample", "imu_rejected", "power", "touch", "button", "touch", "imu_sample", "power"])
        self.assertEqual(records[0]["gy_dps_bits"], "7fc00123")
        self.assertIsNone(records[0]["gy_dps"])
        self.assertIn("nonfinite_measurement", records[0]["issues"])
        self.assertEqual(records[0]["gx_dps_bits"], "80000000")
        self.assertEqual(records[1]["duration_us"], "20100")
        self.assertEqual(records[1]["attempt"], 2)
        self.assertNotIn("ax_g", records[1])
        self.assertEqual(records[1]["error"], -4)
        self.assertEqual(records[1]["cache_error"], 0)
        self.assertEqual(records[2]["current_ma"], -42)
        self.assertEqual(records[2]["temperature_c"], 25.5)
        self.assertEqual(records[2]["external_truth"], 0)
        self.assertEqual(records[3]["x0"], 20)
        self.assertIsNone(records[3]["x1"])
        self.assertTrue(records[4]["button_down"])
        self.assertTrue(records[5]["touch_redacted"])
        self.assertIsNone(records[5]["x0"])
        self.assertIsNone(records[7]["voltage_mv"])
        self.assertIsNone(records[7]["gauge"])

    def test_actual_usb_and_http_records_match_exactly(self):
        response = json.loads(fixture(collector.SENSOR_ROOT + f"samples?boot={BOOT}&after=0&limit=8", transport="http"))
        http = collector.decode_batch(base64.b64decode(response["data"]), BOOT, 0, 0, 8)
        usb = []
        for after in (0, 3, 6): usb.extend(self.decode(batch(after), after)["records"])
        self.assertEqual(http["records"], usb)

    def test_production_framing_interleaving_and_arbitrary_read_boundaries(self):
        for command in (f"ST1 CATALOG {RID}", f"ST1 LATEST {RID}", f"ST1 READ {RID} {BOOT} 0 3"):
            wire = b"startup log\n" + fixture(command) + b"last log\n"
            for width in (1, 17, 4096):
                parser = collector.USBFrames(RID, collections.Counter())
                for offset in range(0, len(wire), width): parser.feed(wire[offset:offset + width])
                self.assertIsNotNone(parser.reply)
                if " READ " not in command: self.assertEqual(collector.strict_json(parser.reply[1])["boot"], BOOT)
                else: self.assertEqual(self.decode(parser.reply[1])["next_after"], "3")

    def test_real_short_writes_resynchronize_without_cursor_or_byte_repair(self):
        for command in (f"ST1 CATALOG {RID}", f"ST1 READ {RID} {BOOT} 0 3"):
            anomalies = collections.Counter()
            parser = collector.USBFrames(RID, anomalies)
            parser.feed(fixture(command, mode="partial"))
            self.assertIsNotNone(parser.reply)
            self.assertGreater(sum(anomalies.values()), 0)

    def test_usb_does_not_wait_for_4096_bytes_after_a_complete_short_response(self):
        with tempfile.TemporaryDirectory() as directory:
            clock = Clock(); sink = collector.Evidence(Path(directory) / "evidence", 65536); sink.open()
            device = FakeSerial(clock)
            transport = collector.USBTransport("named-driver", sink, serial_module=SerialModule(device), clock=clock)
            transport.open()
            try:
                kind, raw, rid = transport.get("samples", BOOT, 0, 3, 1)
                self.assertEqual(kind, "binary")
                self.assertEqual(collector.decode_batch(raw, BOOT, 0, rid, 3)["next_after"], "3")
                self.assertLess(clock.now, 0.01)
                self.assertTrue(all(requested == 1 or requested <= ready for requested, ready in device.read_calls))
            finally: transport.close(); sink.close()

    def test_metadata_requires_end_and_whole_digest(self):
        wire = fixture(f"ST1 CATALOG {RID}")
        no_end = b"\n".join(x for x in wire.split(b"\n") if not x.startswith(b"ST1 END"))
        parser = collector.USBFrames(RID, collections.Counter()); parser.feed(no_end)
        self.assertIsNone(parser.reply)
        corrupted = wire[:-66] + b"0" * 64 + b"\n"
        parser = collector.USBFrames(RID, collections.Counter()); parser.feed(corrupted)
        self.assertIsNone(parser.reply)

    def test_identical_duplicates_accepted_conflicts_rejected(self):
        wire = fixture(f"ST1 CATALOG {RID}")
        line = next(x for x in wire.splitlines() if x.startswith(b"ST1 DATA"))
        parser = collector.USBFrames(RID, collections.Counter()); parser.feed(b"\n" + line + b"\n" + wire)
        self.assertIsNotNone(parser.reply)
        fields = line.split(b" "); value = bytearray(base64.b64decode(fields[6])); value[0] ^= 1
        fields[6] = base64.b64encode(value)
        parser = collector.USBFrames(RID, collections.Counter())
        with self.assertRaisesRegex(collector.CaptureError, "conflicting_chunk"):
            parser.feed(b"\n" + line + b"\n" + b" ".join(fields) + b"\n")
        binary = fixture(f"ST1 READ {RID} {BOOT} 0 3")
        anomalies = collections.Counter(); parser = collector.USBFrames(RID, anomalies); parser.feed(binary + binary)
        self.assertIsNotNone(parser.reply); self.assertEqual(anomalies["duplicate_response"], 1)

    def test_binary_crc_boot_cursor_request_and_schema(self):
        raw = batch()
        damaged = bytearray(raw); damaged[90] ^= 1
        with self.assertRaisesRegex(collector.CaptureError, "batch_crc"): self.decode(bytes(damaged))
        for offset, value, error in [(0, b"\x02", "unsupported_batch_schema"), (2, b"\x59\x00", "unsupported_batch_schema"),
            (4, struct.pack("<I", 16), "unsupported_batch_schema"), (8, b"\x00" * 8, "batch_request_id"),
            (16, b"\xff" * 16, "boot_changed"), (32, struct.pack("<Q", 1), "cursor_mismatch"),
            (56, struct.pack("<Q", 2), "record_sequence_gap"), (96, struct.pack("<H", 9), "unsupported_record_kind")]:
            with self.subTest(error=error, offset=offset), self.assertRaisesRegex(collector.CaptureError, error):
                self.decode(changed(raw, offset, value))
        with self.assertRaises(collector.CaptureError): self.decode(raw[:-1])

    def test_gap_unavailable_and_counter_exhaustion_are_explicit(self):
        result = self.decode(batch(mode="gap"))
        self.assertEqual(result["lost_records"], "4"); self.assertEqual(result["next_after"], "4")
        self.assertEqual(result["records"], [])
        self.assertEqual(self.decode(batch(4, mode="gap"), 4)["records"][0]["sequence"], "5")
        result = self.decode(batch(mode="unavailable")); self.assertEqual(result["flags"], 8)
        result = self.decode(batch(collector.U64_MAX - 2, mode="max"), collector.U64_MAX - 2)
        self.assertEqual(result["next_after"], str(collector.U64_MAX)); self.assertEqual(result["flags"], 4)

    def test_zero_hardware_generation_and_unknown_clock_duration_preserved(self):
        records = self.decode(batch(mode="zero_epoch"))["records"]
        self.assertEqual(records[0]["source_epoch"], "0")
        rejected = self.decode(batch(mode="clock"))["records"][1]
        self.assertEqual(rejected["read_start_us"], "20200")
        self.assertEqual(rejected["read_end_us"], "100")
        self.assertIsNone(rejected["duration_us"])
        self.assertIn("rejected_attempt", rejected["issues"])

    def test_canonical_uint64_and_json_boundaries(self):
        for value in ("0", "1", str(collector.U64_MAX)): self.assertEqual(collector.u64(value), int(value))
        for value in ("00", "01", "-1", "+1", " 1", "1 ", "\u0661", str(collector.U64_MAX + 1)):
            with self.assertRaises(collector.CaptureError): collector.u64(value)
        for raw in (b'{"a":1,"a":2}', b'{"a":NaN}', b'{"a":Infinity}', b'[]', b'x' * 4097):
            with self.assertRaises(collector.CaptureError): collector.strict_json(raw)

    def test_archive_unknown_identity_is_preserved_and_not_upgraded(self):
        catalog = json.loads(fixture(collector.SENSOR_ROOT + "catalog", transport="http"))
        catalog.update(git_commit="", git_state="unknown", elf_sha256=None)
        result = collector.identity(catalog)
        self.assertEqual(result["git_commit"], "")
        self.assertIsNone(result["elf_sha256"])
        catalog["git_state"] = "clean"
        with self.assertRaisesRegex(collector.CaptureError, "invalid_image_identity"):
            collector.identity(catalog)
        with tempfile.TemporaryDirectory() as directory:
            result, _ = self.capture(directory, mode="archive")
            self.assertTrue(result["collection_completed"])
            self.assertEqual(result["identity"]["git_commit"], "")
            self.assertEqual(result["issues"]["image_identity_unknown"], 1)
            self.assertFalse(result["data_usable"])
            first = json.loads((Path(directory) / "evidence.jsonl").read_text().splitlines()[0])
            self.assertEqual(first["response"]["git_commit"], "")

    def capture(self, directory, *, mode="normal", fault=None, seconds=1):
        clock = Clock(); sink = collector.Evidence(Path(directory) / "evidence", 1024 * 1024); sink.open()
        serial = FakeSerial(clock, mode=mode, fault=fault)
        transport = collector.USBTransport("named-driver-boundary", sink, serial_module=SerialModule(serial), clock=clock)
        try: result = collector.collect(transport, sink, seconds, clock=clock, sleep=clock.sleep)
        finally: sink.close()
        return result, serial

    def test_real_collector_preserves_all_records_and_separates_snapshots(self):
        with tempfile.TemporaryDirectory() as directory:
            result, serial = self.capture(directory)
            self.assertTrue(result["collection_completed"]); self.assertEqual(result["cursor"], "8")
            self.assertFalse(result["data_usable"]); self.assertFalse(result["physical_motion_qualified"])
            values = [json.loads(line) for line in (Path(directory) / "evidence.jsonl").read_text().splitlines()]
            self.assertEqual(sum(x["type"] == "record" for x in values), 8)
            self.assertEqual(sum(x["type"] == "latest" for x in values), 1)
            self.assertEqual(sum(x["type"] == "catalog_final" for x in values), 1)
            self.assertEqual(serial.opens, 1); self.assertEqual(serial.closes, 1)
            self.assertTrue((Path(directory) / "evidence.csv").read_text().startswith('# {'))
            self.assertEqual(stat.S_IMODE((Path(directory) / "evidence.wire").stat().st_mode), 0o600)
            for name, value in result["files"].items():
                path = Path(directory) / ("evidence." + name)
                self.assertEqual(value["bytes"], path.stat().st_size)
                self.assertEqual(value["sha256"], hashlib.sha256(path.read_bytes()).hexdigest())

    def test_timeout_and_short_request_retry_same_cursor_new_identity(self):
        for fault in ("timeout_once", "short_once"):
            with self.subTest(fault=fault), tempfile.TemporaryDirectory() as directory:
                result, serial = self.capture(directory, fault=fault, seconds=3)
                requests = [r.split() for r in serial.requests if " READ " in r]
                self.assertTrue(result["collection_completed"]); self.assertEqual(result["record_count"], 8)
                self.assertNotEqual(requests[0][2], requests[1][2]); self.assertEqual(requests[0][4], requests[1][4])

    def test_disconnect_does_not_reopen_or_advance(self):
        with tempfile.TemporaryDirectory() as directory:
            result, serial = self.capture(directory, fault="disconnect")
            self.assertFalse(result["collection_completed"]); self.assertEqual(result["cursor"], "0")
            self.assertEqual(result["error"], "serial_disconnected"); self.assertEqual(serial.opens, 1)

    def test_unverified_and_foreign_boot_pages_never_advance_collector(self):
        for fault in ("boot_changed", "schema", "bad_crc"):
            with self.subTest(fault=fault), tempfile.TemporaryDirectory() as directory:
                result, _ = self.capture(directory, fault=fault)
                self.assertFalse(result["collection_completed"])
                self.assertEqual(result["cursor"], "0")
                self.assertEqual(result["record_count"], 0)
                self.assertGreater((Path(directory) / "evidence.wire").stat().st_size, 0)

    def test_same_boot_image_change_fails_final_identity_check(self):
        with tempfile.TemporaryDirectory() as directory:
            result, _ = self.capture(directory, fault="image_changed")
            self.assertFalse(result["collection_completed"])
            self.assertEqual(result["error"], "image_or_schema_changed")
            self.assertFalse(result["data_usable"])

    def test_real_gap_collects_retained_records_without_claiming_continuity(self):
        with tempfile.TemporaryDirectory() as directory:
            result, _ = self.capture(directory, mode="gap")
            self.assertTrue(result["collection_completed"]); self.assertEqual(result["cursor"], "8")
            self.assertEqual(result["issues"]["retention_gap_records"], 4)
            self.assertFalse(result["journal_contiguous"])
            values = [json.loads(line) for line in (Path(directory) / "evidence.jsonl").read_text().splitlines()]
            self.assertEqual(sum(x["type"] == "record" for x in values), 4)
            self.assertEqual(sum(x["type"] == "gap" for x in values), 1)

    def test_exclusive_files_and_total_output_bound(self):
        with tempfile.TemporaryDirectory() as directory:
            prefix = Path(directory) / "evidence"; existing = Path(str(prefix) + ".csv"); existing.write_text("keep")
            with self.assertRaisesRegex(collector.CaptureError, "output_exists"):
                collector.Evidence(prefix, 65536).open()
            self.assertEqual(existing.read_text(), "keep"); self.assertFalse(Path(str(prefix) + ".wire").exists())
        with tempfile.TemporaryDirectory() as directory:
            sink = collector.Evidence(Path(directory) / "evidence", 65536); sink.open()
            sink.raw(b"x" * (65536 - 16384))
            with self.assertRaisesRegex(collector.CaptureError, "output_byte_bound"): sink.raw(b"x")
            sink.finish({"physical_motion_qualified": False}); sink.close()
            self.assertLessEqual(sum(p.stat().st_size for p in Path(directory).iterdir()), 65536)


class FakeKeychain:
    def __init__(self): self.values = {}
    def put(self, service, account, secret): self.values[service, account] = secret
    def get(self, service, account): return self.values[service, account]


class FakeResponse:
    def __init__(self, raw, status=200): self.raw, self.status = raw, status
    def getheader(self, name, default=None):
        return {"Content-Type": "application/json", "Content-Length": str(len(self.raw))}.get(name, default)
    def read(self, bound): return self.raw[:bound]


class FakeHTTPSConnection:
    """Only socket/HTTP delivery is substituted; TLS policy and raw helper are real."""
    route = None
    def __init__(self, host, port, *, context, timeout):
        assert host == "device.test" and port == 443 and timeout <= 4
        assert context.check_hostname and context.verify_mode == ssl.CERT_REQUIRED
        assert context.minimum_version >= ssl.TLSVersion.TLSv1_2
    def connect(self): self.sock = self
    def getpeercert(self, binary_form): return self.route.der
    def request(self, method, path, body, headers): self.response = self.route.request(method, path, body, headers)
    def getresponse(self): return self.response
    def close(self): pass


class HTTPSRoute:
    def __init__(self, profile, keychain):
        self.profile, self.keychain = profile, keychain
        self.der = ssl.PEM_cert_to_DER_cert(profile["certificate_pem"])
        self.requests = []
    def request(self, method, path, body, headers):
        self.requests.append((method, path))
        if method == "POST":
            assert path == "/api/v1/sessions"
            data = json.loads(body)
            assert data["credential"] == self.keychain.get(self.profile["keychain_service"], self.profile["keychain_account"])
            value = dict(session_token="a"*64, session_id="b"*32, request_prefix="c"*16, boot_id="d"*32, next_seq=1)
        elif path == "/api/v1/capabilities":
            from control_bridge import CATALOG_DIGEST
            value = {"catalog_sha256": CATALOG_DIGEST}
        elif method == "DELETE":
            assert path == "/api/v1/sessions/current"
            value = {"state": "completed"}
        else:
            assert method == "GET" and path.startswith(collector.SENSOR_ROOT)
            assert headers["Authorization"] == "Bearer " + "a"*64
            return FakeResponse(fixture(path, transport="http"))
        return FakeResponse(json.dumps(value).encode())


class LANTests(unittest.TestCase):
    def test_real_pinned_transport_and_session_without_secret_evidence(self):
        from commission import create_package
        with tempfile.TemporaryDirectory() as directory:
            keychain = FakeKeychain(); private = Path(directory) / "private"
            # The existing package generator requires view; the collector itself
            # only reads capabilities and the diagnostics-scoped sensor routes.
            create_package(private, "device.test", 443, ["view", "diagnostics.read"], [], [], keychain)
            profile_path = private / "client-profile.json"; profile = json.loads(profile_path.read_text())
            FakeHTTPSConnection.route = HTTPSRoute(profile, keychain)
            sink = collector.Evidence(Path(directory) / "evidence", 1024 * 1024); sink.open()
            transport = collector.LANTransport(profile_path, sink, keychain=keychain)
            clock = Clock()
            original_get = transport.get
            def timed_get(*args):
                clock.now += .1
                return original_get(*args)
            transport.get = timed_get
            try:
                with patch.object(secure_transport.http.client, "HTTPSConnection", FakeHTTPSConnection):
                    result = collector.collect(transport, sink, 1, clock=clock, sleep=clock.sleep)
            finally: sink.close()
            self.assertTrue(result["collection_completed"], result)
            self.assertEqual(result["cursor"], "8")
            self.assertEqual(result["record_count"], 8)
            self.assertTrue(all(method == "GET" or path in ("/api/v1/sessions", "/api/v1/sessions/current") for method, path in FakeHTTPSConnection.route.requests))
            for output in Path(directory).glob("evidence.*"):
                data = output.read_bytes()
                self.assertNotIn(b"a"*64, data)
                for secret in keychain.values.values(): self.assertNotIn(secret.encode(), data)
            raw = (Path(directory) / "evidence.wire").read_bytes()
            length = struct.unpack_from(">I", raw)[0]
            self.assertEqual(collector.strict_json(raw[4:4+length])["boot"], BOOT)

    def test_real_sensor_path_allowlist_rejects_noncanonical_queries(self):
        for suffix in ["after=00&limit=1", "after=18446744073709551616&limit=1", "after=0&limit=9", "after=0&limit=1&x=1"]:
            self.assertFalse(secure_transport.sensor_samples_path(collector.SENSOR_ROOT + "samples?boot=" + BOOT + "&" + suffix))


if __name__ == "__main__":
    unittest.main(verbosity=2)
