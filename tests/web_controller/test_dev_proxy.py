"""Exercise the loopback proxy against a local upstream without device credentials."""
from contextlib import contextmanager
from http.client import HTTPConnection
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import ssl
import subprocess
import sys
from tempfile import TemporaryDirectory
from threading import Thread
import unittest
from unittest.mock import patch


sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import serve_web_controller as proxy  # noqa: E402


@contextmanager
def serving(server):
    with server:
        thread = Thread(target=server.serve_forever)
        thread.start()
        try:
            yield server
        finally:
            server.shutdown()
            thread.join()


class Upstream(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_GET(self):
        self.handle_api()

    def do_POST(self):
        self.handle_api()

    def handle_api(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        self.server.calls.append((self.command, self.path, dict(self.headers), body))
        if self.path == "/api/v1/state":
            self.send_response(429)
            payload = b'{"error":"busy"}'
        elif self.path == "/api/v1/sessions":
            self.send_response(200)
            self.send_header("Set-Cookie", "control-http=fixture; HttpOnly; SameSite=Strict; Path=/")
            payload = b"{}"
        elif self.path == "/api/v1/commands":
            self.send_response(400)
            payload = b'{"state":"rejected","error":"expired"}'
        else:
            self.send_response(200)
            payload = b'{"protocol":{}}'
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)


class DevProxyTest(unittest.TestCase):
    def test_tls_upstream_verifies_certificate_name(self):
        with TemporaryDirectory() as tmp:
            certificate = Path(tmp) / "cert.pem"
            key = Path(tmp) / "key.pem"
            subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                            "-subj", "/CN=localhost", "-keyout", key, "-out", certificate],
                           check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            tls.load_cert_chain(certificate, key)
            server = ThreadingHTTPServer(("127.0.0.1", 0), Upstream)
            server.calls = []
            server.socket = tls.wrap_socket(server.socket, server_side=True)
            with serving(server) as upstream:
                for host, status in [("localhost", 200), ("127.0.0.1", 502)]:
                    with serving(proxy.ControllerServer(0, f"https://{host}:{upstream.server_port}",
                                                        device_ca=certificate)) as local:
                        connection = HTTPConnection(local.local_host, timeout=5)
                        try:
                            connection.request("GET", "/api/v1/state?view=protocol")
                            response = connection.getresponse()
                            response.read()
                            self.assertEqual(response.status, status)
                        finally:
                            connection.close()
                self.assertEqual(len(upstream.calls), 1)

    def test_test_identity_is_opt_in_and_same_origin_only(self):
        with TemporaryDirectory() as tmp:
            identity = {"client_id": "a" * 32, "credential": "b" * 64}
            credentials = Path(tmp) / "identity.json"
            credentials.write_text(json.dumps(identity))
            credentials.chmod(0o644)
            with self.assertRaises(ValueError):
                proxy.ControllerServer(0, "http://localhost", identity_file=credentials)
            credentials.chmod(0o600)
            with serving(proxy.ControllerServer(0, "http://localhost", identity_file=credentials)) as local:
                connection = HTTPConnection(local.local_host, timeout=5)
                self.addCleanup(connection.close)

                def request(method, route, headers=None):
                    connection.request(method, route, headers=headers or {})
                    response = connection.getresponse()
                    return response.status, response.read(), response.headers

                status, body, _ = request("GET", "/")
                self.assertEqual(status, 200)
                self.assertIn(b"/__dev/bootstrap.js", body)
                self.assertNotIn(identity["credential"].encode(), body)
                status, body, _ = request("GET", "/__dev/bootstrap.js")
                self.assertEqual(status, 200)
                self.assertNotIn(identity["credential"].encode(), body)
                self.assertEqual(request("GET", "/__dev/identity")[0], 404)
                for headers in [{}, {"Origin": "null"}, {"Origin": "http://foreign.invalid"}]:
                    self.assertEqual(request("POST", "/__dev/identity", headers)[0], 403)
                status, body, headers = request("POST", "/__dev/identity", {"Origin": local.local_origin})
                self.assertEqual(status, 200)
                self.assertEqual(json.loads(body), identity)
                self.assertEqual(headers["Cache-Control"], "no-store")
                self.assertIsNone(headers["Access-Control-Allow-Origin"])

    def test_local_assets_and_fixed_api(self):
        with serving(ThreadingHTTPServer(("127.0.0.1", 0), Upstream)) as upstream:
            upstream.calls = []
            upstream_host = f"127.0.0.1:{upstream.server_port}"
            with serving(proxy.ControllerServer(0, f"http://{upstream_host}", legacy_state=True)) as local:
                self.assertEqual(local.server_address[0], "127.0.0.1")
                connection = HTTPConnection(local.local_host, timeout=5)
                self.addCleanup(connection.close)

                def request(method, route, headers=None, data=None):
                    connection.request(method, route, body=data, headers=headers or {})
                    response = connection.getresponse()
                    return response.status, response.read(), response.headers

                with TemporaryDirectory() as tmp, patch.object(proxy, "ASSET_ROOT", Path(tmp)):
                    asset = Path(tmp) / "controller.js"
                    asset.write_text("first")
                    status, body, headers = request("GET", "/controller.js")
                    self.assertEqual((status, body), (200, b"first"))
                    self.assertEqual(headers["Cache-Control"], "no-store")
                    self.assertIn("connect-src 'self'", headers["Content-Security-Policy"])
                    asset.write_text("second")
                    self.assertEqual(request("GET", "/controller.js")[:2], (200, b"second"))

                for headers in [{"Host": "foreign.invalid", "Origin": local.local_origin},
                                {"Origin": "http://foreign.invalid"}, {"Origin": "null"}, {}]:
                    self.assertEqual(request("POST", "/api/v1/commands", headers, b"{}")[0], 403)
                self.assertEqual(request("GET", "http://foreign.invalid/api/v1/state",
                                         {"Host": local.local_host})[0], 404)
                self.assertEqual(request("POST", "/__dev/identity", {"Origin": local.local_origin})[0], 404)
                self.assertEqual(upstream.calls, [])

                state = "/api/v1/state?view=protocol"
                self.assertEqual(request("GET", state)[:2], (429, b'{"error":"busy"}'))
                self.assertEqual(upstream.calls[-1][1], "/api/v1/state")
                local.legacy_state = False
                self.assertEqual(request("GET", state)[0], 200)
                self.assertEqual(upstream.calls[-1][1], state)

                local_headers = {"Origin": local.local_origin, "Content-Type": "application/json"}
                status, _, headers = request("POST", "/api/v1/sessions", local_headers, b"{}")
                self.assertEqual(status, 200)
                self.assertEqual(headers["Set-Cookie"], "control-http=fixture; HttpOnly; SameSite=Strict; Path=/")
                self.assertIsNone(headers["Access-Control-Allow-Origin"])
                local_headers.update({"Cookie": "control-http=fixture", "X-CSRF-Token": "fixture-csrf"})
                status, body, _ = request("POST", "/api/v1/commands", local_headers, b"{}")
                self.assertEqual((status, body), (400, b'{"state":"rejected","error":"expired"}'))
                method, route, headers, data = upstream.calls[-1]
                self.assertEqual((method, route, data), ("POST", "/api/v1/commands", b"{}"))
                self.assertEqual(headers["Host"], upstream_host)
                self.assertEqual(headers["Origin"], f"http://{upstream_host}")
                self.assertEqual(headers["Cookie"], "control-http=fixture")
                self.assertEqual(headers["X-CSRF-Token"], "fixture-csrf")


if __name__ == "__main__":
    unittest.main()
