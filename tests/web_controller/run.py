#!/usr/bin/env python3
"""Run a real HTTPS browser against production HTTP/input paths; no device I/O."""
import argparse
from base64 import b64encode
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import shutil
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[2]
SUITE = Path(__file__).resolve().parent


def build(directory, protocol, log):
    if not (protocol / "firmware/main/app_control_access.c").is_file():
        raise SystemExit("The browser pairing/access owner must be integrated, or supplied with --protocol-root.")
    # Overlay only this public header's relative includes at the existing SDK
    # boundary. Production modules are separate, unchanged translation units.
    if not (ROOT / "firmware/main/app_control_access.h").is_file():
        shutil.copyfile(protocol / "firmware/main/app_control_access.h", directory / "app_control_access.h")
    source = (ROOT / "firmware/main/app_control_server.c").read_text()
    header = source[source.index("static bool header("):source.index('#include "app_control_server_media.inc"')]
    handler = source[source.index("static esp_err_t handle_request("):source.index("static esp_err_t hotspot_dns_start(")]
    (directory / "web_http_adapter.inc").write_text(header + handler)
    subprocess.run([sys.executable, ROOT / "tools/build_identity.py", "--root", ROOT,
                    "--output", directory / "app_build_identity.h"], check=True)
    assets = []
    for extension in ("html", "css", "js"):
        name = f"controller_{extension}"
        path = ROOT / f"firmware/main/web/controller.{extension}"
        assets += [f'.global _binary_{name}_start\n_binary_{name}_start:\n.incbin "{path}"\n.byte 0\n'
                   f'.global _binary_{name}_end\n_binary_{name}_end:\n']
    section = ".section __TEXT,__const\n" if sys.platform == "darwin" else ".section .rodata\n"
    (directory / "web_assets.S").write_text(section + "\n".join(assets))
    includes = [directory, SUITE / "stubs", ROOT / "tests/coordinator/stubs",
                protocol / "components/control_protocol/include", protocol / "components/control_protocol",
                ROOT / "firmware/main"]
    includes += [ROOT / f"components/{name}/include" for name in
                 ("hid_engine", "input_core", "interaction", "device_settings", "audio_engine", "spoken_letters")]
    files = [SUITE / "native_bridge.c", ROOT / "firmware/main/app_control_web.c", directory / "web_assets.S",
             protocol / "firmware/main/app_control_access.c"]
    files += [ROOT / f"firmware/main/{name}.c" for name in
              ("app_remote_input", "app_remote_device", "app_remote_media", "app_command_bridge")]
    files += [protocol / f"components/control_protocol/{name}.c" for name in
              ("control_protocol", "control_json", "control_registry", "control_sha256")]
    files += [ROOT / f"components/{component}/{name}.c" for component, name in
              (("audio_engine", "audio_media"), ("hid_engine", "hid_engine"), ("input_core", "input_core"),
               ("interaction", "interaction"), ("interaction", "interaction_multitouch"), ("device_settings", "device_settings"))]
    command = [os.environ.get("CC") or shutil.which("clang") or "cc", "-std=c11", "-D_POSIX_C_SOURCE=200809L",
               "-Wall", "-Wextra", "-Werror", "-pedantic", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    command += ["-I" + str(path) for path in includes]
    command += [str(path) for path in files] + ["-lm", "-o", str(directory / "native_bridge")]
    inputs = [path for path in files if path.suffix != ".S"] + [
        ROOT / "firmware/main/app_control_server.c", ROOT / "firmware/main/main.c",
        ROOT / "firmware/main/app_remote_input_bindings.inc",
        protocol / "components/control_protocol/include/control_protocol.h",
        protocol / "firmware/main/app_control_access.h",
        *[ROOT / f"firmware/main/web/controller.{extension}" for extension in ("html", "css", "js")]]
    hashes = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in inputs}
    subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT)
    if hashes != {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in inputs}:
        raise SystemExit("Relevant input sources changed during compilation; run again after the owner fixes its candidate.")
    return hashes


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--protocol-root", type=Path, default=ROOT)
    parser.add_argument("--playwright-module", required=True, type=Path)
    parser.add_argument("--browser-executable", type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--http", action="store_true", help="Exercise authenticated plaintext browser transport")
    parser.add_argument("--channel-direct", action="store_true",
                        help="Run the motion-channel case directly on the device origin and its CSP")
    parser.add_argument("--motion-poll", action="store_true",
                        help="Exercise the legacy WebSocket polling feature set in the motion-channel case")
    parser.add_argument("--channel-delay-ms", type=int, default=35,
                        help="One-way WebSocket delay in the motion-channel fixture (default: 35 ms)")
    parser.add_argument("--case", choices=("all", "pad-freshness", "management", "authorization-feedback", "lease-renewal",
                                           "control-timing", "motion-channel"),
                        default="all")
    args = parser.parse_args()
    channel_test = args.case == "motion-channel"
    if args.channel_direct and not channel_test:
        parser.error("--channel-direct requires --case motion-channel")
    if args.motion_poll and not channel_test:
        parser.error("--motion-poll requires --case motion-channel")
    if args.channel_delay_ms < 0:
        parser.error("--channel-delay-ms must be non-negative")
    channel_delay = args.channel_delay_ms / 1000
    args.protocol_root = args.protocol_root.resolve()
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=False)
    with tempfile.TemporaryDirectory(prefix="mosaico-web-test-") as temporary:
        temporary = Path(temporary)
        with (output / "build.log").open("w") as log:
            hashes = build(temporary, args.protocol_root, log)
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                        "-subj", "/CN=localhost", "-keyout", temporary / "key.pem", "-out", temporary / "cert.pem"],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        lock, records = threading.Lock(), []
        origin, backend, drop_next = None, None, False
        drop_motion = False
        started = time.monotonic()

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *unused):
                pass

            def submit(self, path, body):
                request = {"method": self.command, "path": path, "body": body}
                for name in ("Host", "Origin", "Cookie", "Authorization", "X-CSRF-Token", "Content-Type", "Content-Encoding", "Transfer-Encoding"):
                    if name in self.headers:
                        request[name] = self.headers[name]
                with lock:
                    request["now_ms"] = str(1000 + int((time.monotonic() - started) * 1000))
                    backend.stdin.write(json.dumps(request, separators=(",", ":")) + "\n"); backend.stdin.flush()
                    line = backend.stdout.readline()
                    if not line:
                        raise RuntimeError("Production fixture exited; inspect backend.log")
                    result = json.loads(line)
                    operation = json.loads(body).get("op") if path in ("/api/v1/commands", "/_fixture/ws-frame") else None
                    records.append({"method": self.command, "path": path, "status": result["status"], "op": operation,
                                    "origin": self.headers.get("Origin"), "csrf_present": "X-CSRF-Token" in self.headers,
                                    "header_bytes": len(str(self.headers).encode())})
                return result, operation

            def channel(self):
                nonlocal drop_motion
                offered = self.headers.get("Sec-WebSocket-Protocol", "").split(", ")
                if len(offered) != 2 or offered[0] != "esp32-control-v1" or not offered[1].startswith("ticket."):
                    self.send_error(400); return
                result, _ = self.submit("/_fixture/ws-open", offered[1][7:])
                if result["status"] != 101:
                    self.send_error(result["status"]); return
                key = self.headers["Sec-WebSocket-Key"] + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
                self.send_response_only(101)
                self.send_header("Connection", "Upgrade")
                self.send_header("Upgrade", "websocket")
                self.send_header("Sec-WebSocket-Accept", b64encode(hashlib.sha1(key.encode()).digest()).decode())
                self.send_header("Sec-WebSocket-Protocol", offered[0])
                self.end_headers()
                self.close_connection = True
                motion_enabled, renewal_enabled = False, False

                def send_text(payload):
                    payload = payload.encode()
                    prefix = bytes((0x81, len(payload))) if len(payload) < 126 else struct.pack("!BBH", 0x81, 126, len(payload))
                    self.wfile.write(prefix + payload)

                try:
                    while True:
                        header = self.rfile.read(2)
                        if len(header) != 2:
                            return
                        opcode, length = header
                        assert length & 0x80
                        length &= 0x7f
                        if length == 126:
                            length, = struct.unpack("!H", self.rfile.read(2))
                        assert length <= 2048
                        mask = self.rfile.read(4)
                        frame = bytes(value ^ mask[i % 4] for i, value in enumerate(self.rfile.read(length)))
                        if opcode == 0x88:
                            self.wfile.write(b"\x88\x00")
                            return
                        assert opcode == 0x81
                        # Apply one-way delay without advancing the native clock early.
                        time.sleep(channel_delay)
                        payload = frame.decode()
                        reply, operation = self.submit("/_fixture/ws-frame", payload)
                        admitted_at = time.monotonic()
                        reply_data = json.loads(reply["body"])
                        if operation is None and reply_data.get("motion_terminal") is True:
                            motion_enabled = True
                            renewal_enabled = json.loads(payload).get("type") == "renew_terminal"
                        push = reply_data.get("state") == "accepted" and (
                            (motion_enabled and operation == "mouse.move_relative") or
                            (renewal_enabled and operation == "control.renew"))
                        terminal = None
                        if push:
                            time.sleep(0.020)
                            terminal, _ = self.submit("/_fixture/ws-motion-result", "")
                            terminal_at = time.monotonic()
                        time.sleep(max(0, admitted_at + channel_delay - time.monotonic()))
                        if operation == "mouse.move_relative" and drop_motion:
                            drop_motion = False
                            if push:
                                send_text(reply["body"])  # Drop the terminal after the known admission.
                            return
                        send_text(reply["body"])
                        if push:
                            while terminal["status"] == 202 and time.monotonic() - admitted_at < 2.2:
                                time.sleep(0.010)
                                terminal, _ = self.submit("/_fixture/ws-motion-result", "")
                                terminal_at = time.monotonic()
                            if terminal["status"] not in (200, 400):
                                return
                            time.sleep(max(0, terminal_at + channel_delay - time.monotonic()))
                            send_text(terminal["body"])
                finally:
                    self.submit("/_fixture/ws-close", "")

            def request(self):
                nonlocal drop_next, drop_motion
                if channel_test and self.path == "/api/v1/channel" and self.headers.get("Upgrade") == "websocket":
                    return self.channel()
                body = self.rfile.read(int(self.headers.get("Content-Length", "0"))).decode("utf-8")
                if self.path in ("/_fixture/drop-next", "/_fixture/drop-motion"):
                    if self.path == "/_fixture/drop-next":
                        drop_next = True
                    else:
                        drop_motion = True
                    self.send_response(200); self.send_header("Content-Length", "2"); self.end_headers(); self.wfile.write(b"{}"); return
                result, operation = self.submit(self.path, body)
                if drop_next and operation == "keyboard.text.enqueue":
                    drop_next = False; self.close_connection = True; return
                content = result["body"].encode()
                self.send_response(result["status"])
                for name, value in result["headers"].items():
                    self.send_header(name, value)
                self.send_header("Content-Length", str(len(content))); self.send_header("Connection", "close")
                self.end_headers(); self.wfile.write(content); self.close_connection = True

            do_GET = do_POST = do_DELETE = request

        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        origin = f"{'http' if args.http else 'https'}://localhost:{server.server_port}"
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls.load_cert_chain(temporary / "cert.pem", temporary / "key.pem")
        if not args.http: server.socket = tls.wrap_socket(server.socket, server_side=True)
        with (output / "backend.log").open("w") as error:
            backend_env = dict(os.environ)
            if channel_test:
                backend_env["WEB_TEST_CHANNEL"] = "1"
                if not args.motion_poll:
                    backend_env["WEB_TEST_MOTION_TERMINAL"] = "1"
            backend = subprocess.Popen([temporary / "native_bridge", f"localhost:{server.server_port}", origin],
                                       stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=error, text=True, bufsize=1,
                                       env=backend_env)
            thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
            proxy = None
            environment = dict(os.environ, WEB_ORIGIN=origin, WEB_OUTPUT=str(output),
                               WEB_PLAYWRIGHT_MODULE=str(args.playwright_module.resolve()))
            if channel_test:
                environment["WEB_FIXTURE_ORIGIN"] = origin
                environment["WEB_EXPECT_MOTION_TERMINAL"] = "0" if args.motion_poll else "1"
                if not args.channel_direct:
                    sys.path.insert(0, str(ROOT / "tools"))
                    from serve_web_controller import ControllerServer
                    proxy = ControllerServer(0, origin, legacy_state=True,
                                             device_ca=None if args.http else temporary / "cert.pem")
                    threading.Thread(target=proxy.serve_forever, daemon=True).start()
                    environment["WEB_ORIGIN"] = proxy.local_origin
            if args.browser_executable:
                environment["WEB_BROWSER_EXECUTABLE"] = str(args.browser_executable.resolve())
            try:
                with (output / "browser.log").open("w") as log:
                    script = {"all": "test_browser.mjs", "pad-freshness": "test_pad_freshness.mjs",
                              "management": "test_management.mjs",
                              "authorization-feedback": "test_authorization_feedback.mjs",
                              "lease-renewal": "test_lease_renewal.mjs",
                              "control-timing": "test_control_timing.mjs",
                              "motion-channel": "test_motion_channel.mjs"}[args.case]
                    result = subprocess.run(["node", SUITE / script], env=environment, stdout=log, stderr=subprocess.STDOUT)
            finally:
                if proxy:
                    proxy.shutdown(); proxy.server_close()
                server.shutdown(); backend.stdin.close(); backend.wait(timeout=10)
            (output / "http-metadata.json").write_text(json.dumps(records, indent=2) + "\n")
            manifest = {"browser_exit": result.returncode, "backend_exit": backend.returncode,
                        "boundaries": "Real loopback HTTP(S)/Chromium and production C HTTP/protocol/access/coordinator/HID engine; simulated station sockets, NVS, trusted clock, local approval and HID send API; no physical device",
                        "source_sha256_at_compile": hashes,
                        "native_binary_sha256": hashlib.sha256((temporary / "native_bridge").read_bytes()).hexdigest(),
                        "protocol_root": str(args.protocol_root),
                        "channel_delay_ms": args.channel_delay_ms if channel_test else None}
            (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
            print((output / "browser.log").read_text())
            if result.returncode or backend.returncode:
                raise SystemExit("Browser/production-boundary tests failed; see retained output")


if __name__ == "__main__":
    main()
