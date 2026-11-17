#!/usr/bin/env python3
"""Host CLI tests. Only in-memory transports and a newly created PTY are opened."""
import binascii
import importlib.util
import io
import json
import os
from pathlib import Path
import threading
import subprocess
import sys
import unittest

spec = importlib.util.spec_from_file_location("passport_usb", Path(__file__).resolve().parents[2] / "tools/passport_usb.py")
usb = importlib.util.module_from_spec(spec)
spec.loader.exec_module(usb)


def reply(boot, request_id, kind, code, fields=""):
    payload = f"P1 {boot:08x} {request_id:08x} {kind} {code}" + (" " + fields if fields else "")
    raw = payload.encode("ascii")
    return b"\x1e" + raw + f"*{binascii.crc_hqx(raw, 0xffff):04x}\r\n".encode("ascii")


class Port:
    def __init__(self, mode="ok"):
        self.mode, self.rx, self.writes = mode, bytearray(), []

    def write(self, raw):
        self.writes.append(raw)
        words = raw[1:].split(b"*", 1)[0].decode("ascii").split()
        boot, request_id, operation = int(words[1], 16), int(words[2], 16), words[4]
        if operation == "hello":
            self.rx.extend(reply(0x12345678, request_id, "data", "hello", "last=8 pending=0 protocol=1"))
            self.rx.extend(reply(0x12345678, request_id, "done", "applied", "detail=0"))
        elif self.mode == "partial":
            return len(raw) - 1
        elif self.mode == "reset":
            self.rx.extend(reply(0x12345679, request_id, "done", "stale"))
        elif self.mode not in ("timeout", "disconnect"):
            self.rx.extend(b"console log\r\n\x1eP1 broken*ffff\n")
            if self.mode != "no_ack":
                self.rx.extend(reply(boot, request_id, "ack", "accepted"))
            self.rx.extend(reply(boot, request_id, "done", "applied", "detail=0"))
        return len(raw)

    def read(self, size):
        assert size == 1
        if self.rx:
            byte = self.rx[:1]
            del self.rx[:1]
            return byte
        if self.mode == "disconnect":
            raise OSError("fixture disconnected")
        return b""


class Tests(unittest.TestCase):
    def test_session_recovery_and_secrets(self):
        port = Port("timeout")
        client = usb.Client(port, 0.01)
        client.hello()
        requests = [
            {"operation": "wifi.use_ap"},
            {"operation": "key", "arguments": [4, 9, 0, 40]},
            {"operation": "hello"},
            {"operation": "wifi.join", "arguments": [2, 1], "password": "secret123"},
            {"operation": "wifi.join", "arguments": [2, 1, "secret123"]},
        ]
        output = io.StringIO()
        self.assertEqual(usb.run_session(client, io.StringIO("\n".join(map(json.dumps, requests))), output), 0)
        records = [json.loads(line) for line in output.getvalue().splitlines()]
        self.assertEqual([r["result"] for r in records], ["unknown", "unavailable", "applied", "unknown", "invalid"])
        self.assertEqual(records[1]["reason"], "hello_required")
        self.assertEqual(len(port.writes), 4)  # Initial hello, AP once, explicit hello, join once.
        self.assertNotIn("secret123", output.getvalue())
        self.assertNotIn("736563726574313233", output.getvalue())
        self.assertIn(b"736563726574313233", port.writes[-1])
        with self.assertRaises(ValueError):
            usb.session_request({"operation": "key", "arguments": [True]})
        output = io.StringIO()
        self.assertEqual(usb.run_session(client, io.StringIO("x" * 1025), output), 2)
        self.assertEqual(len(port.writes), 4)

    def test_protocol(self):
        self.assertEqual(binascii.crc_hqx(b"123456789", 0xffff), 0x29b1)
        decoder = usb.Decoder()
        records = [r for c in b"noise" + reply(1, 2, "data", "wifi", "ap=1") if (r := decoder.feed(c))]
        self.assertEqual(records[0]["fields"], {"ap": "1"})
        for c in b"\x1e" + b"x" * 1000:
            decoder.feed(c)
        self.assertLess(len(decoder.buffer), 256)
        with self.assertRaises(ValueError):
            usb.encode(1, 1, "wifi.join", ["x" * 256])
        with self.assertRaises(ValueError):
            usb.encode(1, 1, "line\nbreak")

    def test_receipt_and_no_replay(self):
        for mode in ("ok", "no_ack"):
            port = Port(mode)
            client = usb.Client(port, 0.02)
            client.hello()
            result = client.command("key", ["4", "9", "0", "40"])
            self.assertEqual(result["result"], "applied")
            self.assertEqual(result["accepted"], mode == "ok")
            self.assertEqual(result["id"], 9)
            self.assertEqual(len(port.writes), 2)
        for mode, reason in (("partial", "partial_write"), ("reset", "boot_changed"),
                             ("timeout", "timeout"), ("disconnect", "disconnected")):
            port = Port(mode)
            client = usb.Client(port, 0.01)
            client.hello()
            with self.assertRaisesRegex(usb.OutcomeUnknown, reason):
                client.command("wifi.use_ap")
            self.assertEqual(len(port.writes), 2)  # No hidden retry, hello or reset.
        port = Port()  # A final reply alone is insufficient for a status read.
        client = usb.Client(port, 0.01)
        client.hello()
        with self.assertRaisesRegex(usb.OutcomeUnknown, "incomplete_reply"):
            client.command("status")
        self.assertEqual(len(port.writes), 2)

    def test_pty(self):
        import serial
        master, slave = os.openpty()
        port = serial.Serial(port=None, baudrate=115200, timeout=0.1, write_timeout=1, exclusive=True)
        port.dtr = False
        port.rts = False
        port.port = os.ttyname(slave)
        port.open()
        errors = []

        def device():
            try:
                for _ in range(2):
                    request = bytearray()
                    while not request.endswith(b"\n"):
                        request.extend(os.read(master, 1))
                    endpoint = Port()
                    endpoint.write(bytes(request))
                    # Split every response byte across real PTY writes.
                    for byte in endpoint.rx:
                        os.write(master, bytes([byte]))
            except Exception as exc:
                errors.append(exc)

        worker = threading.Thread(target=device, daemon=True)
        worker.start()
        try:
            client = usb.Client(port, 2)
            client.hello()
            self.assertEqual(client.command("wifi.use_ap")["result"], "applied")
            worker.join(2)
            self.assertFalse(worker.is_alive())
            self.assertFalse(errors)
        finally:
            port.close()
            os.close(master)
            os.close(slave)

    def test_persistent_cli_pty(self):
        master, slave = os.openpty()
        errors, seen = [], []
        script = Path(__file__).resolve().parents[2] / "tools/passport_usb.py"
        process = subprocess.Popen([sys.executable, str(script), "--port", os.ttyname(slave), "--timeout", "2", "session"],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1)

        def device():
            try:
                for expected in ("hello", "status", "ble.peer", "key"):
                    raw = bytearray()
                    while not raw.endswith(b"\n"):
                        raw.extend(os.read(master, 1))
                    words = raw[1:].split(b"*", 1)[0].decode("ascii").split()
                    seen.append(words)
                    boot, request_id, operation = int(words[1], 16), int(words[2], 16), words[4]
                    assert operation == expected
                    if operation == "hello":
                        assert boot == 0
                        data = reply(0x12345678, request_id, "data", "hello", "last=8 pending=0 protocol=1")
                    elif operation == "status":
                        assert boot == 0x12345678 and request_id == 9
                        data = b"".join(reply(boot, request_id, "data", name,
                                            "generation=42 input=73 bonds=7" if name == "ble_revision" else "observed=1")
                                        for name in ("usb", "ble", "ble_revision", "ble_target", "wifi", "wifi_revision", "setup", "settings", "names"))
                    elif operation == "ble.peer":
                        assert boot == 0x12345678 and request_id == 10 and words[5:] == ["7", "0"]
                        data = reply(boot, request_id, "data", "peer", "index=0 revision=7 type=1 address=010203040506 bond=01010101010101010101010101010101")
                    else:
                        assert boot == 0x12345678 and request_id == 11 and words[5:] == ["42", "73", "0", "40"]
                        data = reply(boot, request_id, "ack", "accepted")
                    data += reply(0x12345678, request_id, "done", "applied", "detail=0")
                    os.write(master, data)
            except Exception as exc:
                errors.append(exc)

        worker = threading.Thread(target=device, daemon=True)
        worker.start()
        try:
            hello = json.loads(process.stdout.readline())
            self.assertEqual(hello["event"], "hello")
            process.stdin.write(json.dumps({"operation": "status"}) + "\n")
            process.stdin.flush()
            state = json.loads(process.stdout.readline())
            revision = next(r for r in state["data"] if r["type"] == "ble_revision")
            process.stdin.write(json.dumps({"operation": "ble.peer", "arguments": [revision["bonds"], 0]}) + "\n")
            process.stdin.flush()
            peer = json.loads(process.stdout.readline())
            self.assertEqual(peer["result"], "applied")
            self.assertEqual(peer["data"][0]["type"], "peer")
            self.assertEqual(peer["data"][0]["address_type"], "1")
            self.assertEqual(peer["data"][0]["address"], "010203040506")
            self.assertEqual(peer["data"][0]["bond"], "01010101010101010101010101010101")
            process.stdin.write(json.dumps({"operation": "key", "arguments": [revision["generation"], revision["input"], 0, 40]}) + "\n")
            process.stdin.flush()
            result = json.loads(process.stdout.readline())
            self.assertEqual(result["result"], "applied")
            self.assertEqual(result["boot"], hello["boot"])
            self.assertTrue(result["accepted"])
            process.stdin.close()
            self.assertEqual(process.wait(timeout=3), 0)
            worker.join(2)
            self.assertFalse(errors)
            self.assertEqual(len(seen), 4)
            self.assertFalse(worker.is_alive())
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            process.stdout.close()
            process.stderr.close()
            os.close(master)
            os.close(slave)


if __name__ == "__main__":
    unittest.main()
