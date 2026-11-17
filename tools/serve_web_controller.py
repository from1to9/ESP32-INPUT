#!/usr/bin/env python3
"""Serve local web controller assets with one fixed Mosaico upstream."""
import argparse
from contextlib import closing
from http.client import HTTPConnection, HTTPException, HTTPResponse, HTTPSConnection
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import re
from select import select
import ssl
import stat
from urllib.parse import urlsplit


ASSET_ROOT = Path(__file__).resolve().parents[1] / "firmware/main/web"
ASSETS = {"/": ("controller.html", "text/html"), "/controller.css": ("controller.css", "text/css"),
          "/controller.js": ("controller.js", "text/javascript")}
MAX_COMMAND_BYTES = 2048
MAX_REPLY_BYTES = 4096
BOOTSTRAP_SCRIPT = b"""
try {
  const response = await fetch("/__dev/identity", {
    method: "POST", mode: "cors", credentials: "same-origin", cache: "no-store", redirect: "error",
  });
  if (!response.ok) throw new Error();
  localStorage.setItem("mosaico.browser.identity.v1", JSON.stringify(await response.json()));
  await import("/controller.js");
} catch {
  document.getElementById("approval").textContent = "Could not load the approved local test identity.";
}
"""


def read_identity(path):
    info = path.stat()
    if not stat.S_ISREG(info.st_mode) or info.st_mode & 0o077 or info.st_size > 512:
        raise ValueError("The identity must be a private regular file, at most 512 bytes")
    try:
        value = json.loads(path.read_bytes())
    except (ValueError, UnicodeError):
        raise ValueError("The identity file must contain a browser identity JSON object") from None
    if (not isinstance(value, dict) or set(value) != {"client_id", "credential"}
            or not all(isinstance(value[key], str) and re.fullmatch(pattern, value[key])
                       for key, pattern in (("client_id", "[0-9a-f]{32}"), ("credential", "[0-9a-f]{64}")))):
        raise ValueError("The identity file must contain a browser identity JSON object")
    return json.dumps(value, separators=(",", ":")).encode()


class ControllerServer(ThreadingHTTPServer):
    def __init__(self, port, device_origin, legacy_state=False, identity_file=None, device_ca=None):
        self.device = urlsplit(device_origin)
        if (self.device.scheme not in ("http", "https") or not re.fullmatch(r"[A-Za-z0-9.-]+(?::[0-9]+)?", self.device.netloc)
                or self.device.path not in ("", "/") or self.device.query or self.device.fragment):
            raise ValueError("Use a fixed device origin, for example https://mosaico.local")
        self.tls = ssl.create_default_context(cafile=device_ca) if self.device.scheme == "https" else None
        self.device_port = self.device.port or (443 if self.tls else 80)
        self.device_origin = f"{self.device.scheme}://{self.device.netloc}"
        self.legacy_state = legacy_state
        self.identity = read_identity(identity_file) if identity_file is not None else None
        super().__init__(("127.0.0.1", port), ControllerHandler)
        self.local_host = f"127.0.0.1:{self.server_port}"
        self.local_origin = f"http://{self.local_host}"

    def connect_device(self):
        if self.tls:
            return HTTPSConnection(self.device.hostname, self.device_port, timeout=5, context=self.tls)
        return HTTPConnection(self.device.hostname, self.device_port, timeout=5)


class ControllerHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    timeout = 5
    upstream = None

    def log_message(self, *_):
        pass

    def finish(self):
        if self.upstream is not None:
            self.upstream.close()
        super().finish()

    def reply(self, status, data, content_type="application/json", cookies=()):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        if self.close_connection:
            self.send_header("Connection", "close")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        self.send_header("Content-Security-Policy",
                         f"default-src 'none'; script-src 'self'; style-src 'self'; connect-src 'self' ws://{self.server.local_host}; "
                         "base-uri 'none'; form-action 'self'; frame-ancestors 'none'; object-src 'none'")
        for cookie in cookies:
            self.send_header("Set-Cookie", cookie)
        self.end_headers()
        self.wfile.write(data)

    def relay_channel(self, origins):
        self.close_connection = True
        protocol = self.headers.get("Sec-WebSocket-Protocol", "")
        key = self.headers.get("Sec-WebSocket-Key", "")
        if (self.command != "GET" or origins != [self.server.local_origin]
                or self.headers.get("Transfer-Encoding") is not None or self.headers.get("Content-Length", "0") != "0"
                or self.headers.get("Upgrade", "").lower() != "websocket"
                or not re.fullmatch(r"[A-Za-z0-9+/]{22}==", key)
                or not re.fullmatch(r"esp32-control-v1, ticket\.[0-9a-f]{64}", protocol)
                or self.headers.get("Sec-WebSocket-Version") != "13"):
            return self.reply(403, b'{"error":"channel_forbidden"}')
        headers = {"Host": self.server.device.netloc, "Origin": self.server.device_origin,
                   "Cookie": self.headers.get("Cookie", ""), "Connection": "Upgrade", "Upgrade": "websocket",
                   "Sec-WebSocket-Key": key, "Sec-WebSocket-Version": "13", "Sec-WebSocket-Protocol": protocol}
        with closing(self.server.connect_device()) as device:
            try:
                device.request("GET", self.path, headers=headers)
                peer = device.sock
                with HTTPResponse(peer, method="GET") as response:
                    # Do not buffer any upgraded bytes while reading the response headers.
                    response.fp.close()
                    response.fp = peer.makefile("rb", buffering=0)
                    response.begin()
                    if response.status != 101:
                        return self.reply(response.status, response.read(MAX_REPLY_BYTES))
                    names = ("Upgrade", "Connection", "Sec-WebSocket-Accept", "Sec-WebSocket-Protocol")
                    if (response.headers.get("Sec-WebSocket-Protocol") != "esp32-control-v1"
                            or not all(response.headers.get(name) for name in names)):
                        return self.reply(502, b'{"error":"invalid_device_upgrade"}')
                    upgrade = [(name, response.headers[name]) for name in names]
            except (OSError, HTTPException, ValueError):
                return self.reply(502, b'{"error":"device_channel_unavailable"}')
            # Relay opaque bytes in order, with backpressure and no frame queue.
            try:
                self.send_response_only(101)
                for name, value in upgrade:
                    self.send_header(name, value)
                self.end_headers()
                while True:
                    ready = [peer] if self.server.tls and peer.pending() else select([self.connection, peer], [], [])[0]
                    for source in ready:
                        data = source.recv(16384)
                        if not data:
                            return
                        (peer if source is self.connection else self.connection).sendall(data)
            except OSError:
                pass

    def handle_controller(self):
        hosts = self.headers.get_all("Host", [])
        origins = self.headers.get_all("Origin", [])
        mutation = self.command != "GET"
        if (hosts != [self.server.local_host] or (origins and origins != [self.server.local_origin])
                or (mutation and not origins)):
            self.close_connection = True
            return self.reply(403, b'{"error":"forbidden"}')
        if self.path == "/api/v1/channel" and self.headers.get("Upgrade"):
            return self.relay_channel(origins)

        asset = ASSETS.get(self.path) if not mutation else None
        bootstrap_script = self.server.identity is not None and self.command == "GET" and self.path == "/__dev/bootstrap.js"
        bootstrap_identity = self.server.identity is not None and self.command == "POST" and self.path == "/__dev/identity"
        if not (asset or bootstrap_script or bootstrap_identity or self.path.startswith("/api/v1/")):
            self.close_connection = True
            return self.reply(404, b'{"error":"not_found"}')

        lengths = self.headers.get_all("Content-Length", [])
        if (self.headers.get("Transfer-Encoding") is not None or len(lengths) > 1
                or (lengths and not re.fullmatch(r"[0-9]{1,4}", lengths[0]))):
            self.close_connection = True
            return self.reply(400, b'{"error":"invalid_framing"}')
        length = int(lengths[0]) if lengths else 0
        if length > MAX_COMMAND_BYTES:
            self.close_connection = True
            return self.reply(413, b'{"error":"too_large"}')
        if length and (self.command != "POST" or bootstrap_identity):
            self.close_connection = True
            return self.reply(400, b'{"error":"invalid_framing"}')
        if asset is not None:
            filename, media_type = asset
            data = (ASSET_ROOT / filename).read_bytes()
            if self.path == "/" and self.server.identity is not None:
                script = b'<script type="module" src="/controller.js"></script>'
                if data.count(script) != 1:
                    return self.reply(500, b'{"error":"bootstrap_script_unavailable"}')
                data = data.replace(script, b'<script type="module" src="/__dev/bootstrap.js"></script>')
            return self.reply(200, data, f"{media_type}; charset=utf-8")
        if bootstrap_script:
            return self.reply(200, BOOTSTRAP_SCRIPT, "text/javascript; charset=utf-8")
        if bootstrap_identity:
            return self.reply(200, self.server.identity)
        body = self.rfile.read(length) if length else None
        if length and len(body) != length:
            self.close_connection = True
            return self.reply(400, b'{"error":"invalid_framing"}')

        headers = {"Host": self.server.device.netloc}
        for name in ("Accept", "Content-Type", "Content-Encoding", "Cookie", "X-CSRF-Token"):
            if name in self.headers:
                headers[name] = self.headers[name]
        if origins:
            headers["Origin"] = self.server.device_origin
        target = self.path
        if self.server.legacy_state and self.command == "GET" and target == "/api/v1/state?view=protocol":
            target = "/api/v1/state"
        try:
            if self.upstream is None:
                self.upstream = self.server.connect_device()
            self.upstream.request(self.command, target, body, headers)
            response = self.upstream.getresponse()
            expected = response.length
            data = response.read(MAX_REPLY_BYTES + 1)
            if len(data) > MAX_REPLY_BYTES or (expected is not None and len(data) != expected):
                raise HTTPException("Incomplete or oversized device reply")
            cookies = response.headers.get_all("Set-Cookie", [])
        except (OSError, HTTPException, ValueError):
            if self.upstream is not None:
                self.upstream.close()
                self.upstream = None
            return self.reply(502, b'{"error":"device_reply_unavailable"}')
        return self.reply(response.status, data, response.headers.get("Content-Type", "application/json"), cookies)

    do_GET = do_POST = do_DELETE = handle_controller


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device-origin", required=True)
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--legacy-state", action="store_true",
                        help="Forward protocol-only state reads to the original full route on older firmware")
    parser.add_argument("--identity-file", type=Path, help="Private JSON file for an already approved test browser")
    parser.add_argument("--device-ca", help="Public CA/leaf PEM used to verify the device HTTPS origin")
    args = parser.parse_args()
    try:
        server = ControllerServer(args.port, args.device_origin, args.legacy_state, args.identity_file, args.device_ca)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    with server:
        print(f"Serving {server.local_origin} with upstream {server.device_origin}", flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
