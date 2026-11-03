#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Test production release publisher at socket/TLS/clock boundaries; no listeners."""
import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import socket
import ssl
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "bridge"))
import serve_release as publisher


class Clock:
    def __init__(self): self.now = 0.0
    def __call__(self): return self.now


class Connection:
    def __init__(self, clock, incoming=b"GET /image.bin HTTP/1.1\r\nHost: release.test\r\n\r\n"):
        self.clock, self.incoming = clock, incoming
        self.timeouts, self.options, self.output, self.calls = [], [], bytearray(), 0
        self.delay, self.maximum, self.stall, self.read_delay = 0.0, publisher.CHUNK_BYTES, False, 0.0
        self.handshake_error, self.handshake_delay = None, 0
        self.closed = False
    def settimeout(self, seconds):
        assert 0 < seconds <= publisher.RESPONSE_SECONDS
        self.timeouts.append(seconds)
    def setsockopt(self, *args): self.options.append(args)
    def send(self, data):
        self.calls += 1
        assert len(data) <= publisher.CHUNK_BYTES
        if self.stall or self.delay >= self.timeouts[-1]:
            self.clock.now += self.timeouts[-1]
            raise socket.timeout("SECRET SOCKET ERROR")
        self.clock.now += self.delay
        count = min(self.maximum, len(data))
        self.output.extend(data[:count])
        return count
    def recv(self, maximum):
        self.clock.now += self.read_delay
        data, self.incoming = self.incoming[:maximum], self.incoming[maximum:]
        return data
    def do_handshake(self):
        self.clock.now += self.handshake_delay
        if self.handshake_error: raise self.handshake_error
    def __enter__(self): return self
    def __exit__(self, *_args): self.closed = True


class TLS:
    def wrap_socket(self, connection, *, server_side, do_handshake_on_connect):
        assert server_side and not do_handshake_on_connect
        return connection


class PublisherTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.catalog, self.image = self.directory / "catalog.json", self.directory / "image.bin"
        self.image.write_bytes(b"firmware-data" * 100)
        self.payload = dict(size=self.image.stat().st_size,
                            sha256=hashlib.sha256(self.image.read_bytes()).hexdigest(),
                            url="https://192.0.2.12:8443/image.bin")
        self.write_catalog()
    def write_catalog(self):
        self.catalog.write_text(json.dumps(dict(v=1, alg="ES256", key_id="test-key",
                                              payload=json.dumps(self.payload), signature="1" * 128)))
    def resources(self):
        return publisher.prepare_release(self.catalog, self.image, ("192.0.2.12", 8443))
    def handle(self, connection, *, expires=600):
        events = []
        server = SimpleNamespace(request_id=0, resources=self.resources(), context=TLS(),
                                 expires=expires, clock=connection.clock,
                                 emit=lambda event, **fields: events.append(dict(event=event, **fields)))
        publisher.ReleaseHandler(connection, ("unlogged-client", 0), server)
        return events

    def test_progressing_partial_writes_exceed_twenty_seconds_without_stalling(self):
        clock = Clock(); connection = Connection(clock)
        connection.delay, connection.maximum = 19.0, 4096
        data = b"x" * (publisher.CHUNK_BYTES * 2)
        events, state = [], {}
        publisher.send_bytes(connection, data, 600, state, clock=clock,
                             emit=lambda event, **fields: events.append(dict(event=event, **fields)))
        self.assertEqual(bytes(connection.output), data)
        self.assertEqual(clock.now, 152)
        self.assertEqual(state["socket_accepted_bytes"], len(data))
        self.assertEqual(connection.timeouts, [20] * 8)
        self.assertTrue(all(event["event"] == "body_progress" for event in events))

    def test_stalled_writer_and_total_deadline_preserve_accepted_progress(self):
        clock = Clock(); connection = Connection(clock); connection.stall = True
        state = dict(socket_accepted_bytes=0)
        with self.assertRaises(publisher.WriteInactivity):
            publisher.send_bytes(connection, b"body", 600, state, clock=clock)
        self.assertEqual((clock.now, state["socket_accepted_bytes"]), (20, 0))
        clock = Clock(); connection = Connection(clock)
        connection.delay, connection.maximum = 19.0, 1
        with self.assertRaises(publisher.ResponseDeadline):
            publisher.send_bytes(connection, b"long body", 45, state, clock=clock)
        self.assertEqual((clock.now, state["socket_accepted_bytes"]), (45, 2))
        self.assertEqual(connection.timeouts, [20, 20, 7])

    def test_error_after_partial_body_reports_phase_class_without_completion(self):
        clock = Clock(); connection = Connection(clock)
        original_send = connection.send
        body_calls = 0
        def send(data):
            nonlocal body_calls
            if b"HTTP/1.1" not in data.tobytes():
                body_calls += 1
                if body_calls == 1:
                    connection.maximum = 100
                else:
                    connection.stall = True
            return original_send(data)
        connection.send = send
        events = self.handle(connection)
        self.assertEqual(events[0]["event"], "socket_accepted")
        failure = events[-1]
        self.assertEqual(failure["event"], "request_error")
        self.assertEqual((failure["phase"], failure["error_class"]), ("body", "WriteInactivity"))
        self.assertEqual(failure["cause_class"], type(socket.timeout()).__name__)
        self.assertEqual(failure["socket_accepted_bytes"], 100)
        self.assertFalse(any(event["event"] == "body_socket_accepted" for event in events))
        self.assertNotIn("SECRET", json.dumps(events))
        connection = Connection(Clock()); connection.handshake_delay = 5
        events = self.handle(connection)
        self.assertEqual((events[-1]["phase"], events[-1]["error_class"]), ("handshake", "ResponseDeadline"))

    def test_exact_resources_and_rejections_do_not_expose_paths_or_headers(self):
        paths = [("/catalog.json", 200), ("/image.bin", 200), ("/missing", 404),
                 ("/", 400), ("/image.bin?token=SECRET", 400), ("/image.bin?", 400),
                 ("/%2e%2e/private-key.pem", 400), ("/../private-key.pem", 400),
                 ("//image.bin", 400), ("https://other/image.bin", 400)]
        for path, status in paths:
            with self.subTest(path=path):
                connection = Connection(Clock(), f"GET {path} HTTP/1.1\r\nAuthorization: SECRET\r\n\r\n".encode())
                events = self.handle(connection)
                headers, body = bytes(connection.output).split(b"\r\n\r\n", 1)
                self.assertTrue(headers.startswith(f"HTTP/1.1 {status} ".encode()))
                self.assertIn(f"Content-Length: {len(body)}\r\n".encode(), headers + b"\r\n")
                if status == 200:
                    self.assertEqual(body, self.resources()[path][2])
                else:
                    self.assertEqual(body, b"Request rejected\n")
                self.assertEqual(events[-1]["event"], "body_socket_accepted")
                self.assertFalse(events[-1]["device_receipt_observed"])
                self.assertNotIn("SECRET", json.dumps(events))
                self.assertTrue(connection.closed)
                self.assertIn((socket.IPPROTO_TCP, socket.TCP_NODELAY, 1), connection.options)

    def test_request_method_size_body_and_parse_deadline_are_bounded(self):
        for request, expected in [(b"POST /image.bin HTTP/1.1\r\n\r\n", 405),
                                  (b"GET /image.bin HTTP/1.1\r\nContent-Length: 3\r\n\r\nabc", 400),
                                  (b"GET /image.bin HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n", 400),
                                  (b"GET /image.bin HTTP/1.1\r\nBad header\r\n\r\n", 400),
                                  (b"G" * 8192, 431)]:
            self.assertEqual(publisher.read_request(Connection(Clock(), request), 5, clock=lambda: 0)[0], expected)
        connection = Connection(Clock(), b"GET " + b"x" * 3000)
        connection.read_delay = 2
        with self.assertRaises(publisher.ResponseDeadline):
            publisher.read_request(connection, 5, clock=connection.clock)
        self.assertEqual(connection.timeouts, [5, 3, 1])

    def test_server_lifetime_caps_body_and_handshake_errors_keep_original_class(self):
        connection = Connection(Clock()); connection.delay = 4
        events = self.handle(connection, expires=6)
        self.assertEqual(events[-1]["error_class"], "ResponseDeadline")
        self.assertEqual(connection.clock.now, 6)
        self.assertLessEqual(max(connection.timeouts), 6)
        connection = Connection(Clock()); connection.handshake_error = ssl.SSLError("SECRET KEY")
        events = self.handle(connection)
        self.assertEqual((events[-1]["phase"], events[-1]["error_class"]), ("handshake", "SSLError"))
        self.assertNotIn("SECRET", json.dumps(events))

    def test_idle_server_lifetime_stops_without_opening_a_listener(self):
        server = publisher.ReleaseServer.__new__(publisher.ReleaseServer)
        server.clock, server.expires = Clock(), 2.5
        timeouts = []
        def handle_request():
            timeouts.append(server.timeout)
            server.clock.now += server.timeout
        server.handle_request = handle_request
        server.run()
        self.assertEqual(timeouts, [1, 1, 0.5])
        self.assertEqual(server.clock.now, 2.5)

    def test_preflight_rejects_wrong_size_hash_origin_and_duplicate_keys(self):
        for key, value in [("size", 1), ("size", True), ("sha256", "0" * 64),
                           ("url", "https://192.0.2.13:8443/image.bin"),
                           ("url", "https://192.0.2.12:443/image.bin"),
                           ("url", "https://user:SECRET@192.0.2.12:8443/image.bin"),
                           ("url", "https://192.0.2.12:8443/catalog.json"),
                           ("url", "https://192.0.2.12:8443/image.bin?"),
                           ("url", "http://192.0.2.12:8443/image.bin")]:
            previous = self.payload[key]; self.payload[key] = value; self.write_catalog()
            with self.subTest(key=key, value=value), self.assertRaises(ValueError): self.resources()
            self.payload[key] = previous
        self.catalog.write_text('{"v":1,"v":1}')
        with self.assertRaises(ValueError): self.resources()
        self.payload["size"] = float("nan"); self.write_catalog()
        with self.assertRaises(ValueError): self.resources()

    def test_only_omitted_port_defaults_to_https(self):
        for port in ("", ":443", ":0"):
            self.payload["url"] = "https://192.0.2.12" + port + "/image.bin"
            self.write_catalog()
            if port == ":0":
                with self.assertRaises(ValueError):
                    publisher.prepare_release(self.catalog, self.image, ("192.0.2.12", 443))
            else:
                self.assertIn("/image.bin", publisher.prepare_release(self.catalog, self.image, ("192.0.2.12", 443)))

    def test_frozen_bytes_size_bounds_and_non_regular_files(self):
        resources = self.resources()
        original = self.image.read_bytes(); self.image.write_bytes(b"different")
        self.assertEqual(resources["/image.bin"][2], original)
        self.image.write_bytes(b"")
        with self.assertRaises(ValueError): self.resources()
        self.image.write_bytes(b"x" * (publisher.MAX_IMAGE + 1))
        with self.assertRaises(ValueError): self.resources()
        self.image.write_bytes(b"x" * publisher.MAX_IMAGE)
        self.payload.update(size=publisher.MAX_IMAGE, sha256=hashlib.sha256(self.image.read_bytes()).hexdigest())
        self.write_catalog(); self.assertEqual(len(self.resources()["/image.bin"][2]), publisher.MAX_IMAGE)
        fifo = self.directory / "fifo"; os.mkfifo(fifo)
        with self.assertRaises(ValueError): publisher.bounded_file(fifo, 20)
        self.catalog.write_bytes(b" " * (publisher.MAX_CATALOG + 1))
        with self.assertRaises(ValueError): self.resources()

    def test_cli_preflight_before_listen_and_tls_minimum_without_secret_logs(self):
        args = ["--catalog", str(self.catalog), "--image", str(self.image), "--certificate", "SECRET-cert.pem",
                "--private-key", "SECRET-private.pem", "--bind", "192.0.2.12:8443", "--seconds", "600"]
        with patch.object(publisher, "ReleaseServer") as server, patch.object(publisher.ssl, "SSLContext") as tls:
            tls.return_value.load_cert_chain.side_effect = ssl.SSLError("SECRET PEM")
            output = io.StringIO()
            with contextlib.redirect_stdout(output): self.assertEqual(publisher.main(args), 1)
            self.assertNotIn("SECRET", output.getvalue()); server.assert_not_called()
            self.assertEqual(tls.return_value.minimum_version, ssl.TLSVersion.TLSv1_2)
            self.assertEqual(tls.return_value.load_cert_chain.call_args.kwargs, {"password": b""})
            tls.return_value.load_cert_chain.side_effect = None
            with contextlib.redirect_stdout(output): self.assertEqual(publisher.main(args), 0)
            server.assert_called_once(); server.return_value.__enter__.return_value.run.assert_called_once()
            self.assertNotIn("SECRET", output.getvalue())
        self.payload["sha256"] = "0" * 64; self.write_catalog()
        with patch.object(publisher, "ReleaseServer") as server, patch.object(publisher.ssl, "SSLContext") as tls:
            with contextlib.redirect_stdout(io.StringIO()): self.assertEqual(publisher.main(args), 1)
            server.assert_not_called(); tls.assert_not_called()
        for invalid in ("0", "nan", "inf", "3601"):
            with contextlib.redirect_stdout(io.StringIO()): self.assertEqual(publisher.main(args[:-1] + [invalid]), 1)
        for invalid in ("0.0.0.0:8443", "224.0.0.1:8443", "192.0.2.12:0", "host.test:8443"):
            with self.assertRaises(ValueError): publisher.bind_address(invalid)


if __name__ == "__main__":
    unittest.main(verbosity=2)
