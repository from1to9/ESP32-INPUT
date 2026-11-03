#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Serve /catalog.json and its exact signed image path over bounded HTTPS.

--bind is IPv4:port and must match the signed image URL. Catalog/image bytes
are frozen before listening; signature verification remains with the device.
Logs count bytes accepted by the local TLS socket, never device completion.
"""
import argparse
import hashlib
import ipaddress
import json
import math
import os
from pathlib import Path
import re
import socket
import socketserver
import ssl
import stat
import sys
import time
from urllib.parse import urlsplit

MAX_IMAGE = 6 * 1024 * 1024
MAX_CATALOG = 8191
CHUNK_BYTES = 16 * 1024
HANDSHAKE_SECONDS = 5.0
REQUEST_SECONDS = 5.0
WRITE_INACTIVITY_SECONDS = 20.0
RESPONSE_SECONDS = 600.0
CATALOG_PATH = "/catalog.json"


class ReleaseMismatch(ValueError):
    pass


class ResponseDeadline(TimeoutError):
    pass


class WriteInactivity(TimeoutError):
    pass


def emit_event(event, **fields):
    print(json.dumps(dict(event=event, **fields), sort_keys=True), flush=True)


def bounded_file(path, maximum):
    # Nonblocking open also rejects FIFOs without waiting for a producer.
    with os.fdopen(os.open(path, os.O_RDONLY | os.O_NONBLOCK), "rb") as source:
        if not stat.S_ISREG(os.fstat(source.fileno()).st_mode):
            raise ReleaseMismatch("Regular file required")
        data = source.read(maximum + 1)
    if not data or len(data) > maximum:
        raise ReleaseMismatch("File size is outside its bound")
    return data


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ReleaseMismatch("Duplicate JSON key")
        result[key] = value
    return result


def load_json(data):
    def invalid_constant(_value):
        raise ReleaseMismatch("Non-finite JSON value")
    try:
        return json.loads(data, object_pairs_hook=unique_object, parse_constant=invalid_constant)
    except RecursionError as error:
        raise ReleaseMismatch("JSON nesting exceeds parser bound") from error


def bind_address(value):
    host, separator, port = value.rpartition(":")
    if not separator or not port.isascii() or not port.isdecimal():
        raise ReleaseMismatch("Expected IPv4:port")
    address = ipaddress.IPv4Address(host)
    if address.is_unspecified or address.is_multicast or address.is_link_local or not 1 <= int(port) <= 65535:
        raise ReleaseMismatch("Explicit unicast origin required")
    return str(address), int(port)


def resource_path(value):
    return (isinstance(value, str) and re.fullmatch(r"/[A-Za-z0-9_./-]+", value) is not None
            and all(part not in ("", ".", "..") for part in value[1:].split("/")))


def prepare_release(catalog_path, image_path, address):
    catalog = bounded_file(catalog_path, MAX_CATALOG)
    image = bounded_file(image_path, MAX_IMAGE)
    envelope = load_json(catalog)
    if (not isinstance(envelope, dict) or envelope.get("v") != 1 or envelope.get("alg") != "ES256"
            or not isinstance(envelope.get("payload"), str)
            or not isinstance(envelope.get("signature"), str)
            or re.fullmatch(r"[0-9a-f]{128}", envelope["signature"]) is None):
        raise ReleaseMismatch("Expected signed catalog envelope")
    payload = load_json(envelope["payload"])
    if (not isinstance(payload, dict) or type(payload.get("size")) is not int
            or payload["size"] != len(image) or payload.get("sha256") != hashlib.sha256(image).hexdigest()):
        raise ReleaseMismatch("Image size or digest differs from signed payload")
    url = payload.get("url")
    if not isinstance(url, str) or any(ord(char) <= 32 or ord(char) >= 127 for char in url):
        raise ReleaseMismatch("Explicit HTTPS image URL required")
    origin = urlsplit(url)
    if (origin.scheme != "https" or origin.username is not None or origin.password is not None
            or origin.query or origin.fragment or "?" in url or "#" in url
            or (origin.hostname, 443 if origin.port is None else origin.port) != address
            or not resource_path(origin.path) or origin.path == CATALOG_PATH):
        raise ReleaseMismatch("Signed image origin/path differs from publisher")
    return {CATALOG_PATH: ("catalog", "application/json", catalog),
            origin.path: ("image", "application/octet-stream", image)}


def send_bytes(connection, body, deadline, state, *, clock=time.monotonic, emit=None):
    """Positive partial sends reset inactivity; total deadline never moves.

    A completed return only proves local socket acceptance. The mutable count
    survives exceptions so errors can report exact locally accepted progress.
    """
    accepted = 0
    inactive_at = clock() + WRITE_INACTIVITY_SECONDS
    next_progress = 256 * 1024
    last_progress = clock()
    view = memoryview(body)
    while accepted < len(body):
        now = clock()
        if now >= deadline:
            raise ResponseDeadline()
        if now >= inactive_at:
            raise WriteInactivity()
        connection.settimeout(min(deadline, inactive_at) - now)
        try:
            count = connection.send(view[accepted:accepted + CHUNK_BYTES])
        except socket.timeout as error:
            if clock() >= deadline:
                raise ResponseDeadline() from error
            raise WriteInactivity() from error
        if not isinstance(count, int) or not 0 < count <= min(CHUNK_BYTES, len(body) - accepted):
            raise ConnectionError("No valid socket progress")
        accepted += count
        state["socket_accepted_bytes"] = accepted
        now = clock()
        if now >= deadline:
            raise ResponseDeadline()
        if now >= inactive_at:
            raise WriteInactivity()
        inactive_at = now + WRITE_INACTIVITY_SECONDS
        if emit and (accepted >= next_progress or now - last_progress >= 5):
            emit("body_progress", **state, total_bytes=len(body))
            next_progress = accepted + 256 * 1024
            last_progress = now


def read_request(connection, deadline, *, clock=time.monotonic):
    data = bytearray()
    while b"\r\n\r\n" not in data:
        remaining = deadline - clock()
        if remaining <= 0:
            raise ResponseDeadline()
        connection.settimeout(remaining)
        chunk = connection.recv(min(1024, 8192 - len(data)))
        if not chunk:
            raise ConnectionError("Incomplete request")
        data.extend(chunk)
        if len(data) >= 8192:
            return 431, None
    if clock() >= deadline:
        raise ResponseDeadline()
    lines = bytes(data).split(b"\r\n\r\n", 1)[0].split(b"\r\n")
    try:
        method, path, version = lines[0].decode("ascii").split(" ")
        if version not in ("HTTP/1.0", "HTTP/1.1"):
            return 400, None
        for line in lines[1:]:
            key, separator, value = line.partition(b":")
            if not separator or re.fullmatch(rb"[!#$%&'*+.^_`|~0-9A-Za-z-]+", key) is None:
                return 400, None
            if key.lower() == b"transfer-encoding" or (key.lower() == b"content-length" and value.strip() != b"0"):
                return 400, None
    except (UnicodeError, ValueError):
        return 400, None
    if method != "GET":
        return 405, None
    if not resource_path(path):
        return 400, None
    return 200, path


class ReleaseHandler(socketserver.BaseRequestHandler):
    def handle(self):
        server = self.server
        server.request_id += 1
        state = dict(request_id=server.request_id, resource=None, socket_accepted_bytes=0)
        phase = "handshake"
        server.emit("socket_accepted", **state)
        try:
            self.request.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            handshake_deadline = min(server.expires, server.clock() + HANDSHAKE_SECONDS)
            remaining = handshake_deadline - server.clock()
            if remaining <= 0:
                raise ResponseDeadline()
            self.request.settimeout(remaining)
            with server.context.wrap_socket(self.request, server_side=True, do_handshake_on_connect=False) as connection:
                remaining = handshake_deadline - server.clock()
                if remaining <= 0:
                    raise ResponseDeadline()
                connection.settimeout(remaining)
                connection.do_handshake()
                if server.clock() >= handshake_deadline:
                    raise ResponseDeadline()
                phase = "request"
                status, path = read_request(connection, min(server.expires, server.clock() + REQUEST_SECONDS), clock=server.clock)
                resource = server.resources.get(path) if status == 200 else None
                if status == 200 and resource is None:
                    status = 404
                if resource:
                    state["resource"], content_type, body = resource
                else:
                    content_type, body = "text/plain", b"Request rejected\n"
                deadline = min(server.expires, server.clock() + RESPONSE_SECONDS)
                phase = "headers"
                headers = (f"HTTP/1.1 {status} Response\r\nContent-Length: {len(body)}\r\n"
                           f"Content-Type: {content_type}\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n").encode("ascii")
                send_bytes(connection, headers, deadline, {}, clock=server.clock)
                phase = "body"
                server.emit("response_started", **state, status=status, total_bytes=len(body))
                send_bytes(connection, body, deadline, state, clock=server.clock, emit=server.emit)
                server.emit("body_socket_accepted", **state, status=status, total_bytes=len(body), device_receipt_observed=False)
        except Exception as error:
            server.emit("request_error", **state, phase=phase, error_class=type(error).__name__,
                        cause_class=type(error.__cause__).__name__ if error.__cause__ else None)


class ReleaseServer(socketserver.TCPServer):
    request_queue_size = 4

    def __init__(self, address, resources, context, seconds, *, emit=emit_event, clock=time.monotonic):
        self.resources, self.context, self.emit, self.clock = resources, context, emit, clock
        self.expires, self.request_id = clock() + seconds, 0
        super().__init__(address, ReleaseHandler)

    def run(self):
        while True:
            remaining = self.expires - self.clock()
            if remaining <= 0:
                return
            self.timeout = min(1.0, remaining)
            self.handle_request()

    def handle_error(self, _request, _address):
        self.emit("request_error", phase="handler", error_class=type(sys.exc_info()[1]).__name__)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("catalog", "image", "certificate", "private-key"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--bind", required=True, help="Exact signed artifact IPv4:port")
    parser.add_argument("--seconds", required=True, type=float, help="Server lifetime, 0 < seconds <= 3600")
    args = parser.parse_args(argv)
    phase = "preflight"
    try:
        if not math.isfinite(args.seconds) or not 0 < args.seconds <= 3600:
            raise ReleaseMismatch("Finite server lifetime required")
        address = bind_address(args.bind)
        resources = prepare_release(args.catalog, args.image, address)
        phase = "tls_configuration"
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        # Explicit empty password rejects encrypted keys without an interactive prompt.
        context.load_cert_chain(args.certificate, args.private_key, password=b"")
        phase = "listen"
        with ReleaseServer(address, resources, context, args.seconds) as server:
            emit_event("publisher_ready", image_bytes=len(next(value[2] for value in resources.values() if value[0] == "image")),
                       lifetime_seconds=args.seconds, signature_verified=False)
            server.run()
        emit_event("publisher_stopped", reason="lifetime_expired")
        return 0
    except KeyboardInterrupt:
        emit_event("publisher_stopped", reason="interrupted")
        return 130
    except (OSError, ValueError, TypeError) as error:
        emit_event("publisher_error", phase=phase, error_class=type(error).__name__)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
