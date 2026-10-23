#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Collect bounded sensor observations without changing device input or state."""
from __future__ import annotations
import argparse
import base64
import collections
import csv
import hashlib
import io
import json
import math
import os
from pathlib import Path
import re
import secrets
import struct
import sys
import time
import zlib

U64_MAX = (1 << 64) - 1
MAX_BODY = 4096
MAX_LINE = 4096
RECORD_BYTES = 88
OPERATION_SECONDS = 6.5  # At most three bounded USB attempts; HTTPS retains 4 s.
HEADER = struct.Struct("<BBHIQ16sQQQ")
RECORD = struct.Struct("<QQQQQHHiII8I")
SENSOR_ROOT = "/api/v1/sensors/"
HEX32 = re.compile(r"[0-9a-f]{32}\Z")
HEX64 = re.compile(r"[0-9a-f]{64}\Z")
KINDS = {1: "imu_sample", 2: "imu_rejected", 3: "power", 4: "touch", 5: "button"}


class CaptureError(Exception):
    """Only fixed, non-sensitive error codes cross the CLI boundary."""


def require(condition, code):
    if not condition:
        raise CaptureError(code)


def u64(value):
    require(isinstance(value, str) and bool(re.fullmatch(r"0|[1-9][0-9]{0,19}", value)), "invalid_u64")
    result = int(value)
    require(result <= U64_MAX, "u64_overflow")
    return result


def strict_json(raw):
    require(len(raw) <= MAX_BODY, "json_bound")

    def pairs(items):
        result = {}
        for key, value in items:
            require(key not in result, "duplicate_json_member")
            result[key] = value
        return result

    def invalid_constant(_):
        raise CaptureError("nonfinite_json")

    try:
        value = json.loads(raw, object_pairs_hook=pairs, parse_constant=invalid_constant)
    except (UnicodeError, ValueError, RecursionError) as error:
        raise CaptureError("invalid_json") from error
    require(isinstance(value, dict), "json_object_required")
    return value


def unbase64(value):
    require(isinstance(value, (str, bytes)), "invalid_base64")
    try:
        raw = base64.b64decode(value, validate=True)
        require(base64.b64encode(raw) == (value.encode("ascii") if isinstance(value, str) else value), "noncanonical_base64")
        return raw
    except (ValueError, UnicodeError) as error:
        raise CaptureError("invalid_base64") from error


class USBFrames:
    """One bounded response assembler. Raw bytes are retained by the I/O owner."""
    def __init__(self, request_id, anomalies):
        self.request_id = request_id
        self.anomalies = anomalies
        self.buffer = bytearray()
        self.parts = {}
        self.total = None
        self.end = None
        self.reply = None

    def accept(self, kind, raw):
        if self.reply is not None:
            require(self.reply == (kind, raw), "conflicting_response")
            self.anomalies["duplicate_response"] += 1
        else:
            self.reply = kind, raw

    def feed(self, raw):
        self.buffer.extend(raw)
        while b"\n" in self.buffer:
            line, _, rest = self.buffer.partition(b"\n")
            self.buffer = bytearray(rest)
            if not line.startswith(b"ST1 "):
                continue
            try:
                self.line(bytes(line).rstrip(b"\r"))
            except UnicodeError:
                self.anomalies["malformed_frame"] += 1
            except CaptureError as error:
                if str(error) in {"conflicting_response", "conflicting_chunk", "conflicting_end", "conflicting_total"}:
                    raise
                self.anomalies[str(error)] += 1
        require(len(self.buffer) <= MAX_LINE, "line_bound")

    def line(self, line):
        require(len(line) <= 480, "frame_bound")
        fields = line.split(b" ")
        require(len(fields) >= 3 and re.fullmatch(rb"[0-9a-f]{16}", fields[2]), "malformed_frame")
        if fields[2].decode() != self.request_id:
            self.anomalies["foreign_request_frame"] += 1
            return
        if fields[1] == b"R":
            require(len(fields) == 5, "malformed_binary_frame")
            length = u64(fields[3].decode("ascii"))
            raw = unbase64(fields[4])
            require(60 <= length <= 324 and len(raw) == length, "binary_length")
            require(zlib.crc32(raw[:-4]) == struct.unpack_from("<I", raw, len(raw) - 4)[0], "binary_crc")
            require(struct.unpack_from("<Q", raw, 8)[0] == int(self.request_id, 16), "binary_request_id")
            self.accept("binary", raw)
        elif fields[1] == b"DATA":
            require(len(fields) == 7, "malformed_chunk")
            offset, total, length = [u64(x.decode("ascii")) for x in fields[3:6]]
            fragment = unbase64(fields[6])
            require(0 < total <= MAX_BODY and 0 < length <= 192 and len(fragment) == length and offset + length <= total, "chunk_bounds")
            require(self.total is None or self.total == total, "conflicting_total")
            self.total = total
            if offset in self.parts:
                require(self.parts[offset] == fragment, "conflicting_chunk")
                self.anomalies["duplicate_chunk"] += 1
            else:
                require(not any(offset < a + len(b) and a < offset + length for a, b in self.parts.items()), "overlapping_chunk")
                self.parts[offset] = fragment
                require(len(self.parts) <= MAX_BODY, "chunk_count_bound")
            self.finish()
        elif fields[1] == b"END":
            require(len(fields) == 5 and bool(re.fullmatch(rb"[0-9a-f]{64}", fields[4])), "malformed_end")
            total = u64(fields[3].decode("ascii"))
            require(0 < total <= MAX_BODY, "end_bound")
            require(self.total is None or self.total == total, "conflicting_total")
            end = total, fields[4].decode()
            require(self.end is None or self.end == end, "conflicting_end")
            self.total, self.end = total, end
            self.finish()
        else:
            raise CaptureError("unsupported_frame")

    def finish(self):
        if self.end is None:
            return
        offset = 0
        parts = []
        for start, fragment in sorted(self.parts.items()):
            if start != offset:
                return
            parts.append(fragment)
            offset += len(fragment)
        if offset != self.total:
            return
        raw = b"".join(parts)
        require(hashlib.sha256(raw).hexdigest() == self.end[1], "metadata_digest")
        strict_json(raw)
        self.accept("json", raw)


class USBTransport:
    limit = 3

    def __init__(self, port, sink, *, serial_module=None, clock=time.monotonic):
        self.port, self.sink, self.serial_module, self.clock = port, sink, serial_module, clock
        self.device = None
        self.request_number = secrets.randbelow(U64_MAX - 100000) + 1
        self.anomalies = collections.Counter()

    def open(self):
        if self.serial_module is None:
            import serial
            self.serial_module = serial
        self.device = self.serial_module.Serial(port=None, baudrate=115200, timeout=0.1,
            write_timeout=0.5, xonxoff=False, rtscts=False, dsrdtr=False)
        self.device.dtr = False
        self.device.rts = False
        self.device.port = self.port
        self.device.open()

    def get(self, operation, boot, after, limit, deadline):
        require(operation in {"catalog", "latest", "samples"}, "invalid_operation")
        for attempt in range(3):
            require(self.clock() < deadline, "response_timeout")
            require(self.request_number < U64_MAX, "request_id_exhausted")
            self.request_number += 1
            rid = f"{self.request_number:016x}"
            if operation == "samples":
                require(bool(HEX32.fullmatch(boot)) and 0 <= after <= U64_MAX and 1 <= limit <= 3, "invalid_request")
                command = f"ST1 READ {rid} {boot} {after} {limit}\n"
            else:
                command = f"ST1 {operation.upper()} {rid}\n"
            # A newline resynchronizes a previously truncated request. It never
            # flushes input, toggles modem lines or opens a CP1 import window.
            request = b"\n" + command.encode("ascii")
            try:
                count = self.device.write(request)
                require(count == len(request), "request_short_write")
            except CaptureError:
                self.anomalies["request_short_write"] += 1
                continue
            except Exception as error:
                raise CaptureError("serial_write_failed") from error
            frames = USBFrames(rid, self.anomalies)
            response_deadline = min(deadline, self.clock() + 2.1)
            while self.clock() < response_deadline:
                require(self.device.is_open, "serial_disconnected")
                self.device.timeout = min(0.1, response_deadline - self.clock())
                try:
                    # pyserial read(4096) waits for 4096 bytes or timeout even
                    # when an entire <=461-byte reply is already buffered.
                    # Wait only for the first byte, then copy a bounded ready
                    # snapshot. Never discard or flush any input bytes.
                    raw = self.device.read(1)
                    if raw:
                        available = min(4095, self.device.in_waiting)
                        if available:
                            raw += self.device.read(available)
                except Exception as error:
                    raise CaptureError("serial_disconnected") from error
                if raw:
                    self.sink.raw(raw)
                    frames.feed(raw)
                    if frames.reply is not None:
                        return (*frames.reply, int(rid, 16))
            self.anomalies["response_timeout"] += 1
        raise CaptureError("response_timeout")

    def close(self):
        if self.device is not None:
            self.device.close()


class LANTransport:
    limit = 8

    def __init__(self, profile_path, sink, *, session_factory=None, transport_factory=None, keychain=None):
        self.profile_path, self.sink = profile_path, sink
        self.session_factory, self.transport_factory, self.keychain = session_factory, transport_factory, keychain
        self.session = self.transport = None
        self.anomalies = collections.Counter()

    def open(self):
        sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "bridge"))
        from secure_transport import PinnedHTTPS, MacKeychain, load_profile
        if self.session_factory is None:
            from control_bridge import DeviceSession
            self.session_factory = DeviceSession
        profile = load_profile(self.profile_path)
        self.transport = (self.transport_factory or PinnedHTTPS)(profile)
        self.session = self.session_factory(profile, self.transport, self.keychain or MacKeychain())
        self.session.connect()

    def get(self, operation, boot, after, limit, deadline):
        del deadline  # Existing verified transport has its own finite 4 s bound.
        require(operation in {"catalog", "latest", "samples"}, "invalid_operation")
        path = SENSOR_ROOT + operation
        if operation == "samples":
            require(bool(HEX32.fullmatch(boot)) and 0 <= after <= U64_MAX and 1 <= limit <= 8, "invalid_request")
            path += f"?boot={boot}&after={after}&limit={limit}"
        # Only sensor GET response bodies enter evidence. Session credentials,
        # Authorization headers and authentication responses stay in memory.
        status, raw = self.transport.sensor_get_raw(path, self.session.login["session_token"])
        self.sink.raw(struct.pack(">I", len(raw)) + raw)
        require(status == 200, "sensor_http_error")
        data = strict_json(raw)
        if operation != "samples":
            return "json", raw, 0
        require(set(data) == {"schema", "encoding", "data"} and type(data["schema"]) is int and data["schema"] == 1 and data["encoding"] == "stj1-le-b64", "unsupported_http_batch")
        return "binary", unbase64(data["data"]), 0

    def close(self):
        if self.session is not None:
            self.session.close()


def float_word(word):
    value = struct.unpack("<f", struct.pack("<I", word))[0]
    return value if math.isfinite(value) else None


def decode_record(raw):
    values = RECORD.unpack(raw)
    sequence, source, epoch, sampled, observed, kind, flags, error, discontinuities, validity = values[:10]
    words = values[10:]
    require(kind in KINDS, "unsupported_record_kind")
    require(not flags & ~(0xff3f if kind == 4 else 0xff1f) and not discontinuities & ~0x1fc, "unsupported_record_flags")
    record = {"sequence": str(sequence), "kind": KINDS[kind], "source_sequence": str(source) if flags & 1 else None,
        "source_epoch": str(epoch) if flags & 2 else None, "sample_us": str(sampled) if flags & 4 else None,
        "observed_us": str(observed) if flags & 8 else None, "flags": flags, "error": error,
        "discontinuities": discontinuities, "validity": validity, "payload_words": [f"{x:08x}" for x in words],
        "raw_record_hex": raw.hex(), "issues": []}
    if error or discontinuities:
        record["issues"].append("source_error_or_discontinuity")
    if kind == 1:
        require(not validity & ~0x7f, "unsupported_imu_validity")
        for i, name in enumerate(["gx_dps", "gy_dps", "gz_dps", "ax_g", "ay_g", "az_g"]):
            record[name + "_bits"] = f"{words[i]:08x}"
            record[name] = float_word(words[i]) if validity & (1 << i) else None
            if validity & (1 << i) and record[name] is None:
                record["issues"].append("nonfinite_measurement")
        record["read_start_us"] = str(words[6] | words[7] << 32) if validity & 64 else None
        record["drdy_status"] = flags >> 8 if flags & 16 else None
        if validity & 63 != 63:
            record["issues"].append("missing_imu_measurement")
    elif kind == 2:
        require(not validity & ~0x3c0, "unsupported_rejection_validity")
        start, end, duration = struct.unpack_from("<QQQ", raw, 56)
        known, status, attempt, stage = struct.unpack_from("<BBBB", raw, 80)
        require(known in (0, 1) and 1 <= attempt <= 6 and 0 <= stage <= 15, "unsupported_rejection_payload")
        record.update(read_start_us=str(start) if validity & 64 else None,
            read_end_us=str(end) if validity & 128 else None, duration_us=str(duration) if validity & 256 else None,
            drdy_known=bool(known), drdy_status=status if known else None, attempt=attempt, stage=stage,
            cache_error=struct.unpack_from("<i", raw, 84)[0] if validity & 512 else None)
        record["issues"].append("rejected_attempt")
    elif kind == 3:
        require(not validity & ~31 and not words[6] and not words[7] and words[4] in (0, 1) and not words[5] & ~255, "unsupported_power_payload")
        record.update(temperature_c=float_word(words[0]) if validity & 8 else None,
            temperature_c_bits=f"{words[0]:08x}", current_ma=struct.unpack("<i", struct.pack("<I", words[1]))[0] if validity & 4 else None,
            voltage_mv=words[2] if validity & 2 else None, percent=words[3] if validity & 1 else None,
            gauge=bool(words[4]) if validity & 16 else None)
        for i, name in enumerate(["battery_truth", "external_truth", "charging_truth", "full_truth"]):
            value = (words[5] >> (i * 2)) & 3
            require(value <= 2, "unsupported_power_truth")
            record[name] = value if validity & 16 else None
        if validity & 8 and record["temperature_c"] is None:
            record["issues"].append("nonfinite_measurement")
        if validity & 1 and words[3] > 100:
            record["issues"].append("invalid_measurement_range")
    elif kind == 4:
        require(not validity & ~1 and words[0] <= 2 and not words[1] & ~3, "unsupported_touch_payload")
        redacted = bool(flags & 32)
        require(not redacted or validity == 0 and not any(words), "invalid_touch_redaction")
        record.update(touch_count=words[0] if validity & 1 else None,
            touch_valid=bool(words[1] & 1) if validity & 1 else None,
            touch_overflow=bool(words[1] & 2) if validity & 1 else None, touch_redacted=redacted)
        for i, name in enumerate(["id0", "x0", "y0", "id1", "x1", "y1"]):
            record[name] = words[i + 2] if validity & 1 and words[1] & 1 and i // 3 < words[0] else None
        if record["touch_overflow"]:
            record["issues"].append("touch_overflow")
    else:
        require(not validity & ~1 and words[0] <= 1 and not any(words[1:]), "unsupported_button_payload")
        record["button_down"] = words[0] == 0 if validity & 1 else None
    return record


def decode_batch(raw, boot, after, request_id, limit):
    require(60 <= len(raw) <= 764, "batch_bound")
    require(zlib.crc32(raw[:-4]) == struct.unpack_from("<I", raw, len(raw) - 4)[0], "batch_crc")
    version, count, size, flags, rid, raw_boot, cursor, oldest, latest = HEADER.unpack_from(raw)
    require(version == 1 and size == RECORD_BYTES and not flags & ~15, "unsupported_batch_schema")
    require(count <= limit and len(raw) == HEADER.size + size * count + 4, "batch_length")
    require(rid == request_id, "batch_request_id")
    require(raw_boot.hex() == boot, "boot_changed")
    require(cursor == after, "cursor_mismatch")
    if flags & 8:
        require(flags == 8 and count == 0 and oldest == latest == after == 0, "invalid_unavailable_page")
    elif latest == 0:
        require(not count and not oldest and not after and not flags & 3, "invalid_empty_page")
    else:
        require(0 < oldest <= latest and after <= latest, "invalid_journal_bounds")
    records = [decode_record(raw[56 + i * size:56 + (i + 1) * size]) for i in range(count)]
    next_after, lost = after, 0
    if flags & 1:
        require(not count and oldest > 0 and after < oldest - 1, "invalid_gap_page")
        lost, next_after = oldest - after - 1, oldest - 1
    elif records:
        require(oldest > 0 and int(records[0]["sequence"]) >= oldest, "record_before_retention")
        for record in records:
            require(next_after < U64_MAX and int(record["sequence"]) == next_after + 1, "record_sequence_gap")
            next_after += 1
        require(next_after <= latest, "record_after_latest")
    else:
        require(after == latest, "missing_page_records")
    require(bool(flags & 2) == (next_after < latest), "more_flag_mismatch")
    return {"boot": boot, "after": str(after), "next_after": str(next_after), "oldest": str(oldest),
        "latest": str(latest), "flags": flags, "lost_records": str(lost), "records": records}


CSV_FIELDS = ["sequence", "kind", "source_sequence", "source_epoch", "sample_us", "observed_us", "error", "discontinuities", "validity",
    "gx_dps", "gy_dps", "gz_dps", "ax_g", "ay_g", "az_g", "gx_dps_bits", "gy_dps_bits", "gz_dps_bits", "ax_g_bits", "ay_g_bits", "az_g_bits",
    "read_start_us", "read_end_us", "duration_us", "drdy_known", "drdy_status", "attempt", "stage", "cache_error",
    "temperature_c", "temperature_c_bits", "current_ma", "voltage_mv", "percent", "gauge", "battery_truth", "external_truth", "charging_truth", "full_truth",
    "touch_count", "touch_valid", "touch_overflow", "touch_redacted", "id0", "x0", "y0", "id1", "x1", "y1", "button_down", "issues", "raw_record_hex"]


class Evidence:
    def __init__(self, prefix, maximum_bytes):
        self.prefix, self.maximum_bytes = prefix, maximum_bytes
        self.used = 0
        self.files = {}
        self.hashes = {name: hashlib.sha256() for name in ("wire", "jsonl", "csv")}
        self.sizes = collections.Counter()

    def open(self):
        self.prefix.parent.mkdir(parents=True, exist_ok=True)
        paths = {name: Path(str(self.prefix) + suffix) for name, suffix in
            {"wire": ".wire", "jsonl": ".jsonl", "csv": ".csv", "receipt": ".receipt.json"}.items()}
        require(not any(p.exists() for p in paths.values()), "output_exists")
        try:
            for name, path in paths.items():
                self.files[name] = os.fdopen(os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), "wb")
        except OSError as error:
            self.close()
            raise CaptureError("output_reservation_failed") from error

    def write(self, name, raw):
        require(self.used + len(raw) <= self.maximum_bytes - 16384, "output_byte_bound")
        written = self.files[name].write(raw)
        require(type(written) is int and 0 <= written <= len(raw), "output_write_failed")
        self.used += written
        self.hashes[name].update(raw[:written])
        self.sizes[name] += written
        require(written == len(raw), "output_write_failed")

    def raw(self, raw):
        self.write("wire", raw)

    def event(self, value):
        self.write("jsonl", (json.dumps(value, separators=(",", ":"), allow_nan=False) + "\n").encode())

    def csv_rows(self, records):
        text = io.StringIO(newline="")
        writer = csv.DictWriter(text, CSV_FIELDS, extrasaction="ignore")
        for record in records:
            row = dict(record, issues="|".join(record["issues"]))
            writer.writerow(row)
        self.write("csv", text.getvalue().encode())

    def header(self, catalog, transport):
        self.event({"type": "catalog", "response": catalog})
        units = {"gyro": "degree/s", "acceleration": "g", "timestamps": "microseconds",
            "temperature": "gauge degrees C", "current": "mA signed", "voltage": "mV", "touch": "logical pixels",
            "truth_enum": {"0": "unknown", "1": "no", "2": "yes"}}
        self.write("csv", ("# " + json.dumps({"schema": 1, "boot": catalog["boot"], "units": units,
            "transport": transport, "unknown": "empty; inspect validity and raw bits"}) + "\n").encode())
        text = io.StringIO(newline="")
        csv.DictWriter(text, CSV_FIELDS).writeheader()
        self.write("csv", text.getvalue().encode())

    def finish(self, receipt):
        receipt["files"] = {name: {"bytes": self.sizes[name], "sha256": digest.hexdigest()}
            for name, digest in self.hashes.items()}
        raw = (json.dumps(receipt, indent=2, allow_nan=False) + "\n").encode()
        require(len(raw) <= 16384, "receipt_bound")
        self.files["receipt"].write(raw)
        for file in self.files.values():
            file.flush()

    def close(self):
        for file in self.files.values():
            file.close()


def identity(catalog):
    require(type(catalog.get("schema")) is int and catalog["schema"] == 1, "unsupported_catalog_schema")
    require(isinstance(catalog.get("boot"), str) and HEX32.fullmatch(catalog["boot"]), "invalid_boot_identity")
    require(catalog.get("record_bytes") == 88 and catalog.get("max_http_records") == 8 and catalog.get("max_usb_records") == 3,
        "unsupported_catalog_limits")
    require(isinstance(catalog.get("sources"), list), "invalid_catalog_sources")
    require(catalog.get("git_commit") == "unknown" or
        catalog.get("git_commit") == "" and catalog.get("git_state") == "unknown" or
        isinstance(catalog.get("git_commit"), str) and re.fullmatch(r"[0-9a-f]{40}", catalog["git_commit"]), "invalid_image_identity")
    require(catalog.get("git_state") in ("clean", "dirty", "unknown") and
        (catalog.get("elf_sha256") is None or isinstance(catalog["elf_sha256"], str) and HEX64.fullmatch(catalog["elf_sha256"])), "invalid_image_identity")
    require(catalog.get("config_sha256") is None or isinstance(catalog["config_sha256"], str) and HEX64.fullmatch(catalog["config_sha256"]), "invalid_config_identity")
    require(type(catalog.get("capacity")) is int and catalog["capacity"] == 256, "unsupported_catalog_capacity")
    return {key: catalog.get(key) for key in ["schema", "boot", "git_commit", "git_state", "elf_sha256", "config_sha256", "record_bytes", "capacity", "max_http_records", "max_usb_records"]}


def get_json(transport, operation, boot, after, limit, deadline):
    kind, raw, _ = transport.get(operation, boot, after, limit, deadline)
    require(kind == "json", "unexpected_binary_response")
    value = strict_json(raw)
    require("error" not in value, "remote_sensor_error")
    return value


def collect(transport, sink, seconds, *, after=0, interval=0.02, snapshot_seconds=1.0, clock=time.monotonic, sleep=time.sleep):
    started = clock()
    receipt = {"schema": 1, "collection_completed": False, "physical_motion_qualified": False,
        "requested_poll_seconds": seconds, "wire_verified_responses": 0, "record_count": 0, "cursor": str(after), "issues": {},
        "raw_format": "usb_rx_bytes" if transport.limit == 3 else "length_be32_prefixed_https_sensor_bodies"}
    issues = collections.Counter()
    source_last = None
    kind_counts = collections.Counter()
    opened = False
    try:
        transport.open()
        opened = True
        catalog = get_json(transport, "catalog", "", after, transport.limit, clock() + OPERATION_SECONDS)
        expected = identity(catalog)
        receipt["identity"] = expected
        if expected["git_commit"] in ("", "unknown") or expected["elf_sha256"] is None:
            issues["image_identity_unknown"] += 1
        boot = catalog["boot"]
        sink.header(catalog, receipt["raw_format"])
        receipt["wire_verified_responses"] += 1
        latest = get_json(transport, "latest", boot, after, transport.limit, clock() + OPERATION_SECONDS)
        require(type(latest.get("schema")) is int and latest["schema"] == 1 and latest.get("boot") == boot, "snapshot_identity_changed")
        sink.event({"type": "latest", "response": latest})
        receipt["wire_verified_responses"] += 1
        poll_started = clock()
        receipt["poll_started_elapsed_seconds"] = poll_started - started
        deadline = poll_started + seconds
        next_snapshot = poll_started + snapshot_seconds
        while clock() < deadline:
            if clock() >= next_snapshot:
                latest = get_json(transport, "latest", boot, after, transport.limit, clock() + OPERATION_SECONDS)
                require(type(latest.get("schema")) is int and latest["schema"] == 1 and latest.get("boot") == boot, "snapshot_identity_changed")
                sink.event({"type": "latest", "response": latest})
                receipt["wire_verified_responses"] += 1
                next_snapshot = clock() + snapshot_seconds
                continue
            kind, raw, rid = transport.get("samples", boot, after, transport.limit, clock() + OPERATION_SECONDS)
            if kind == "json":
                strict_json(raw)
                raise CaptureError("remote_sensor_error")
            batch = decode_batch(raw, boot, after, rid, transport.limit)
            receipt["wire_verified_responses"] += 1
            if int(batch["lost_records"]):
                issues["retention_gap_records"] += int(batch["lost_records"])
                sink.event({"type": "gap", **{k: v for k, v in batch.items() if k != "records"}})
            for record in batch["records"]:
                kind_counts[record["kind"]] += 1
                for issue in set(record["issues"]):
                    issues[issue] += 1
                if record["kind"] == "imu_sample" and record["source_sequence"] is not None and record["source_epoch"] is not None:
                    current = int(record["source_sequence"]), int(record["source_epoch"])
                    if source_last is not None and (current[1] != source_last[1] or current[0] != source_last[0] + 1):
                        issues["imu_source_sequence_or_generation_change"] += 1
                    source_last = current
                sink.event({"type": "record", "boot": boot, **record})
            sink.csv_rows(batch["records"])
            sink.event({"type": "page_commit", **{k: v for k, v in batch.items() if k != "records"}})
            # Do not acknowledge any unverified or partially written page.
            after = int(batch["next_after"])
            receipt["cursor"] = str(after)
            receipt["record_count"] += len(batch["records"])
            if batch["flags"] & 8:
                issues["journal_unavailable"] += 1
            if batch["flags"] & 4 and not batch["flags"] & 2:
                issues["journal_exhausted"] += 1
                break
            if not batch["flags"] & 2 and not batch["records"]:
                sleep(max(0, min(interval, deadline - clock())))
        # One final bounded metadata read checks immutable image/schema/boot.
        # Initial connection/metadata, one in-flight operation and this final
        # bounded check are outside the requested polling window. No unbounded
        # retry, more sample fetches or cursor acknowledgement happens here.
        final_catalog = get_json(transport, "catalog", "", after, transport.limit, clock() + OPERATION_SECONDS)
        require(identity(final_catalog) == expected, "image_or_schema_changed")
        sink.event({"type": "catalog_final", "response": final_catalog})
        receipt["wire_verified_responses"] += 1
        receipt["collection_completed"] = True
    except CaptureError as error:
        receipt["error"] = str(error)
    except KeyboardInterrupt:
        receipt["error"] = "interrupted"
    except Exception:
        receipt["error"] = "collection_io_or_auth_failed"
    finally:
        if opened or getattr(transport, "device", None) is not None or getattr(transport, "session", None) is not None:
            try:
                transport.close()
            except Exception:
                receipt["close_status"] = "unknown"
        receipt["elapsed_seconds"] = max(0, clock() - started)
        receipt["issues"] = dict(issues)
        receipt["records_by_kind"] = dict(kind_counts)
        receipt["transport_anomalies"] = dict(transport.anomalies)
        receipt["journal_contiguous"] = not issues and receipt["record_count"] > 0
        receipt["data_usable"] = receipt["collection_completed"] and receipt["journal_contiguous"]
        receipt["status"] = "failed" if not receipt["collection_completed"] else ("complete" if receipt["data_usable"] else "complete_with_issues")
        sink.finish(receipt)
    return receipt


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--port")
    source.add_argument("--profile", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--seconds", type=float, default=30)
    parser.add_argument("--after", default="0")
    parser.add_argument("--snapshot-seconds", type=float, default=1.0)
    parser.add_argument("--max-bytes", type=int, default=64 * 1024 * 1024)
    args = parser.parse_args(argv)
    try:
        after = u64(args.after)
        require(math.isfinite(args.seconds) and 1 <= args.seconds <= 600, "invalid_duration")
        require(math.isfinite(args.snapshot_seconds) and .5 <= args.snapshot_seconds <= 60, "invalid_snapshot_interval")
        require(65536 <= args.max_bytes <= 256 * 1024 * 1024, "invalid_output_bound")
        sink = Evidence(args.output, args.max_bytes)
        sink.open()
    except CaptureError as error:
        parser.error(str(error))
    try:
        transport = USBTransport(args.port, sink) if args.port else LANTransport(args.profile, sink)
        receipt = collect(transport, sink, args.seconds, after=after, snapshot_seconds=args.snapshot_seconds)
        print("Sensor collection: " + receipt["status"] + "; physical Motion remains unqualified")
        return 0 if receipt["collection_completed"] else 1
    finally:
        sink.close()


if __name__ == "__main__":
    raise SystemExit(main())
