#!/usr/bin/env python3
"""Run a real HTTPS browser against production HTTP/input paths; no device I/O."""
import argparse
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import shutil
import ssl
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
    handler = source[source.index("static esp_err_t handle_request("):source.index("static esp_err_t initialize(")]
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
    args = parser.parse_args()
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
        started = time.monotonic()

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *unused):
                pass

            def request(self):
                nonlocal drop_next
                body = self.rfile.read(int(self.headers.get("Content-Length", "0"))).decode("utf-8")
                if self.path == "/_fixture/drop-next":
                    drop_next = True
                    self.send_response(200); self.send_header("Content-Length", "2"); self.end_headers(); self.wfile.write(b"{}"); return
                request = {"method": self.command, "path": self.path, "body": body,
                           "now_ms": str(1000 + int((time.monotonic() - started) * 1000))}
                for name in ("Host", "Origin", "Cookie", "Authorization", "X-CSRF-Token", "Content-Type", "Content-Encoding", "Transfer-Encoding"):
                    if name in self.headers:
                        request[name] = self.headers[name]
                with lock:
                    backend.stdin.write(json.dumps(request, separators=(",", ":")) + "\n"); backend.stdin.flush()
                    line = backend.stdout.readline()
                    if not line:
                        raise RuntimeError("Production fixture exited; inspect backend.log")
                    result = json.loads(line)
                    operation = json.loads(body).get("op") if self.path == "/api/v1/commands" else None
                    records.append({"method": self.command, "path": self.path, "status": result["status"], "op": operation,
                                    "origin": self.headers.get("Origin"), "csrf_present": "X-CSRF-Token" in self.headers,
                                    "header_bytes": len(str(self.headers).encode())})
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
        origin = f"https://localhost:{server.server_port}"
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls.load_cert_chain(temporary / "cert.pem", temporary / "key.pem")
        server.socket = tls.wrap_socket(server.socket, server_side=True)
        with (output / "backend.log").open("w") as error:
            backend = subprocess.Popen([temporary / "native_bridge", f"localhost:{server.server_port}", origin],
                                       stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=error, text=True, bufsize=1)
            thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
            environment = dict(os.environ, WEB_ORIGIN=origin, WEB_OUTPUT=str(output),
                               WEB_PLAYWRIGHT_MODULE=str(args.playwright_module.resolve()))
            if args.browser_executable:
                environment["WEB_BROWSER_EXECUTABLE"] = str(args.browser_executable.resolve())
            try:
                with (output / "browser.log").open("w") as log:
                    result = subprocess.run(["node", SUITE / "test_browser.mjs"], env=environment, stdout=log, stderr=subprocess.STDOUT)
            finally:
                server.shutdown(); backend.stdin.close(); backend.wait(timeout=10)
            (output / "http-metadata.json").write_text(json.dumps(records, indent=2) + "\n")
            manifest = {"browser_exit": result.returncode, "backend_exit": backend.returncode,
                        "boundaries": "Real loopback HTTPS/Chromium and production C HTTP/protocol/access/coordinator/HID engine; simulated station sockets, NVS, trusted clock, local approval and HID send API; no physical device",
                        "source_sha256_at_compile": hashes,
                        "native_binary_sha256": hashlib.sha256((temporary / "native_bridge").read_bytes()).hexdigest(),
                        "protocol_root": str(args.protocol_root)}
            (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
            print((output / "browser.log").read_text())
            if result.returncode or backend.returncode:
                raise SystemExit("Browser/production-boundary tests failed; see retained output")


if __name__ == "__main__":
    main()
