#!/usr/bin/env python3
"""Passport physical USB control. Explicit port; no reset or mutation retries."""
import argparse
import binascii
import json
import secrets
import sys
import time

FRAME_LIMIT = 256


def encode(boot, request_id, operation, arguments=()):
    tokens = ["P1", f"{boot:08x}", f"{request_id:08x}", "req", operation, *arguments]
    if any(not token or any(ord(c) < 33 or ord(c) > 126 for c in token) for token in tokens):
        raise ValueError("Arguments must be nonempty ASCII tokens; encode strings as hex")
    payload = " ".join(tokens).encode("ascii")
    frame = b"\x1e" + payload + f"*{binascii.crc_hqx(payload, 0xffff):04x}\n".encode("ascii")
    if len(frame) >= FRAME_LIMIT or len(arguments) > 11:
        raise ValueError("Request exceeds the firmware frame limit")
    return frame


class Decoder:
    """Bounded resynchronization amongst arbitrary console bytes."""
    def __init__(self):
        self.buffer = bytearray()
        self.active = False
        self.last_byte = 0.0

    def feed(self, byte):
        now = time.monotonic()
        if self.active and now - self.last_byte >= 1:
            self.buffer.clear()
            self.active = False
        self.last_byte = now
        if byte == 30:
            self.buffer.clear()
            self.active = True
        elif self.active:
            if byte == 10:
                self.active = False
                raw = bytes(self.buffer).removesuffix(b"\r")
                self.buffer.clear()
                try:
                    payload, checksum = raw.rsplit(b"*", 1)
                    if len(checksum) != 4 or any(c not in b"0123456789abcdefABCDEF" for c in checksum) or int(checksum, 16) != binascii.crc_hqx(payload, 0xffff):
                        return None
                    parts = payload.decode("ascii").split(" ")
                    if len(parts) < 5 or parts[0] != "P1" or len(parts[1]) != 8 or len(parts[2]) != 8:
                        return None
                    if any(c not in "0123456789abcdefABCDEF" for token in parts[1:3] for c in token):
                        return None
                    if parts[3] not in ("ack", "done", "data") or any(not p or any(ord(c) < 33 or ord(c) > 126 for c in p) for p in parts):
                        return None
                    pairs = [p.split("=", 1) for p in parts[5:]]
                    if any(len(pair) != 2 or not all(pair) for pair in pairs):
                        return None
                    fields = dict(pairs)
                    if len(fields) != len(pairs):
                        return None
                    return {"boot": int(parts[1], 16), "id": int(parts[2], 16),
                            "kind": parts[3], "code": parts[4], "fields": fields}
                except (ValueError, UnicodeError):
                    return None
            elif len(self.buffer) >= FRAME_LIMIT - 3 or byte < 32 and byte != 13 or byte > 126:
                self.buffer.clear()
                self.active = False
            else:
                self.buffer.append(byte)
        return None


class OutcomeUnknown(Exception):
    pass


class Client:
    def __init__(self, port, timeout=75):
        self.port, self.timeout = port, timeout
        self.decoder = Decoder()
        self.boot = 0
        self.last = 0
        self.accepted = False
        self.data = []
        self.request_id = 0

    def exchange(self, boot, request_id, operation, arguments=(), timeout=None):
        self.accepted = False
        self.data = []
        self.request_id = request_id
        frame = encode(boot, request_id, operation, arguments)
        # One transmission only, including after a partial/failed write.
        try:
            sent = self.port.write(frame)
        except Exception as exc:
            raise OutcomeUnknown("write_failed") from exc
        if sent != len(frame):
            raise OutcomeUnknown("partial_write")
        deadline = time.monotonic() + (self.timeout if timeout is None else timeout)
        while time.monotonic() < deadline:
            try:
                chunk = self.port.read(1)
            except Exception as exc:
                raise OutcomeUnknown("disconnected") from exc
            if not chunk:
                continue
            reply = self.decoder.feed(chunk[0])
            if reply is None:
                continue
            if boot and reply["boot"] != boot:
                raise OutcomeUnknown("boot_changed")
            if reply["id"] != request_id:
                continue
            if reply["kind"] == "ack":
                self.accepted = reply["code"] == "accepted"
            elif reply["kind"] == "data":
                self.data.append({"type": reply["code"], **reply["fields"]})
                if len(self.data) > 16:
                    raise OutcomeUnknown("too_many_reply_records")
            else:
                required = {
                    "hello": {"hello"},
                    "status": {"usb", "ble", "ble_revision", "ble_target", "wifi", "wifi_revision", "setup", "settings", "names"},
                    "ble.peer": {"peer"}, "wifi.profile": {"profile"}, "wifi.row": {"row"},
                    "wifi.ap_credentials": {"ap_credentials"}, "result": {"result"},
                }.get(operation, {"name"} if operation.startswith("name.") and len(arguments) == 3 else set())
                if reply["code"] == "applied" and not required.issubset({record["type"] for record in self.data}):
                    raise OutcomeUnknown("incomplete_reply")
                return {"boot": f"{reply['boot']:08x}", "id": request_id, "accepted": self.accepted,
                        "result": reply["code"], "detail": reply["fields"].get("detail"), "data": self.data}
        raise OutcomeUnknown("timeout")

    def hello(self):
        result = self.exchange(0, secrets.randbits(32) or 1, "hello", timeout=min(self.timeout, 5))
        record = next((r for r in result["data"] if r["type"] == "hello"), None)
        if result["result"] != "applied" or record is None or record.get("protocol") != "1":
            raise OutcomeUnknown("handshake_failed")
        self.boot = int(result["boot"], 16)
        self.last = int(record["last"])
        return result

    def command(self, operation, arguments=()):
        if not self.boot or self.last >= 0xffffffff:
            raise ValueError("Fresh boot identity and unexhausted sequence required")
        self.last += 1
        return self.exchange(self.boot, self.last, operation, arguments)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="Explicit USB serial port; never auto-detected")
    parser.add_argument("--timeout", type=float, default=75, help="Final-result deadline in seconds (1..120)")
    parser.add_argument("--password-stdin", action="store_true", help="wifi.join only: read a passphrase line from stdin")
    parser.add_argument("operation", help="Protocol operation; see docs/passport-usb-control.md")
    parser.add_argument("arguments", nargs="*")
    args = parser.parse_args(argv)
    if not 1 <= args.timeout <= 120:
        parser.error("--timeout must be 1..120 seconds")
    if args.operation == "wifi.join":
        if not args.password_stdin or len(args.arguments) != 2:
            parser.error("wifi.join requires scan_revision row and --password-stdin; never put passwords in argv")
        password = bytearray(sys.stdin.buffer.readline(66))
        if password.endswith(b"\n"):
            del password[-1:]
        if password.endswith(b"\r"):
            del password[-1:]
        if len(password) > 63 or any(c < 32 or c > 126 for c in password):
            password[:] = b"\0" * len(password)
            parser.error("Passphrase must be empty or at most 63 printable ASCII bytes")
        args.arguments.append(password.hex() or "-")
        password[:] = b"\0" * len(password)
    elif args.password_stdin:
        parser.error("--password-stdin is only valid for wifi.join")
    if args.operation == "hello" and args.arguments:
        parser.error("hello takes no arguments")
    # Validate before opening a port. Never include request content in errors.
    try:
        encode(1, 1, args.operation, args.arguments)
    except ValueError as exc:
        parser.error(str(exc))
    port = None
    client = None
    phase = "open"
    try:
        import serial
        port = serial.Serial(port=None, baudrate=115200, timeout=0.1, write_timeout=1, exclusive=True)
        port.dtr = False
        port.rts = False
        port.port = args.port
        port.open()
        client = Client(port, args.timeout)
        phase = "hello"
        result = client.hello()
        if args.operation != "hello":
            phase = "command"
            result = client.command(args.operation, args.arguments)
        print(json.dumps(result, ensure_ascii=True))
        return 0 if result["result"] == "applied" else 2
    except (Exception, KeyboardInterrupt) as exc:
        reason = str(exc) if isinstance(exc, OutcomeUnknown) else type(exc).__name__
        print(json.dumps({"result": "unknown" if phase == "command" else "unavailable", "phase": phase, "reason": reason,
                          "accepted": bool(client and client.accepted),
                          "id": client.request_id if client else None}))
        return 3
    finally:
        # Python/OS/pyserial may retain immutable copies; no secure-erasure claim.
        args.arguments.clear()
        if port is not None:
            port.close()


if __name__ == "__main__":
    sys.exit(main())
