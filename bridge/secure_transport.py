#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Pinned native TLS and secret storage. No discovery, redirect or TOFU path."""
from __future__ import annotations
import hashlib
import http.client
import json
from pathlib import Path
import re
import ssl
import subprocess
import threading
from typing import Protocol

MAX_RESPONSE = 4096

class BridgeError(Exception):
    """A public, deliberately non-sensitive failure description."""

class Keychain(Protocol):
    def put(self, service: str, account: str, secret: str) -> None: ...
    def get(self, service: str, account: str) -> str: ...

class MacKeychain:
    @staticmethod
    def _names(service: str, account: str) -> None:
        if not re.fullmatch(r"[A-Za-z0-9_.-]{1,100}", service) or not re.fullmatch(r"[a-f0-9]{32}", account):
            raise BridgeError("Invalid Keychain reference")

    def put(self, service: str, account: str, secret: str) -> None:
        self._names(service, account)
        if not re.fullmatch(r"[a-f0-9]{64}", secret):
            raise BridgeError("Invalid credential")
        # Interactive security input keeps the credential out of argv/process
        # listings. No shell, echo, captured diagnostics or secret environment.
        command = f"add-generic-password -s {service} -a {account} -w {secret}\n"
        result = subprocess.run(["/usr/bin/security", "-i"], input=command.encode(),
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
        if result.returncode:
            raise BridgeError("Keychain import failed; no credential was printed")

    def get(self, service: str, account: str) -> str:
        self._names(service, account)
        result = subprocess.run(["/usr/bin/security", "find-generic-password", "-s", service,
                                 "-a", account, "-w"], capture_output=True, check=False)
        secret = result.stdout.decode("ascii", errors="ignore").strip()
        if result.returncode or not re.fullmatch(r"[a-f0-9]{64}", secret):
            raise BridgeError("Keychain credential unavailable")
        return secret

def strict_json(data: bytes):
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise BridgeError("Duplicate response member")
            result[key] = value
        return result
    try:
        return json.loads(data, object_pairs_hook=pairs,
                          parse_constant=lambda _: (_ for _ in ()).throw(BridgeError("Invalid JSON number")))
    except (UnicodeError, ValueError) as error:
        raise BridgeError("Invalid JSON response") from error

def sensor_samples_path(path: str) -> bool:
    match = re.fullmatch(r"/api/v1/sensors/samples\?boot=[0-9a-f]{32}&after=(0|[1-9][0-9]{0,19})&limit=[1-8]", path)
    return bool(match and int(match[1]) <= 2**64 - 1)

class PinnedHTTPS:
    def __init__(self, config: dict):
        if config.get("v") != 1 or not re.fullmatch(r"[A-Za-z0-9.-]{1,128}", config.get("host", "")):
            raise BridgeError("Invalid public connection profile")
        if type(config.get("port")) is not int or not 1 <= config["port"] <= 65535:
            raise BridgeError("Invalid HTTPS port")
        self.host, self.port = config["host"], config["port"]
        certificate = config["certificate_pem"]
        der = ssl.PEM_cert_to_DER_cert(certificate)
        digest = hashlib.sha256(der).hexdigest()
        if digest != config.get("certificate_der_sha256"):
            raise BridgeError("Connection profile certificate digest mismatch")
        self.digest = digest
        self.context = ssl.create_default_context(cadata=certificate)
        self.context.minimum_version = ssl.TLSVersion.TLSv1_2
        self.connection = None
        self.lock = threading.RLock()
        # CA signature/path and hostname checks remain enabled. The exact leaf
        # fingerprint is an additional check, not a replacement for either.

    def close(self):
        with self.lock:
            connection = getattr(self, "connection", None)
            self.connection = None
            if connection is not None:
                connection.close()

    def request(self, method: str, path: str, body: dict | None = None, token: str | None = None) -> dict:
        status, raw = self._request_bytes(method, path, body, token)
        result = strict_json(raw)
        if not isinstance(result, dict):
            raise BridgeError("Invalid device response object")
        return {**result, "http_status": status}

    def sensor_get_raw(self, path: str, token: str) -> tuple[int, bytes]:
        """Bounded sensor-response evidence only; never returns headers/secrets."""
        if path not in {"/api/v1/sensors/catalog", "/api/v1/sensors/latest"} and not sensor_samples_path(path):
            raise BridgeError("Invalid sensor resource")
        if not isinstance(token, str) or not re.fullmatch(r"[0-9a-f]{64}", token):
            raise BridgeError("Invalid sensor session")
        return self._request_bytes("GET", path, None, token)

    def _request_bytes(self, method: str, path: str, body: dict | None, token: str | None) -> tuple[int, bytes]:
        # asyncio cancellation does not stop a to_thread worker. Keep socket
        # request/response/close ownership serialized in the worker threads.
        with self.lock:
            return self._request_bytes_locked(method, path, body, token)

    def _request_bytes_locked(self, method: str, path: str, body: dict | None, token: str | None) -> tuple[int, bytes]:
        if method not in {"GET", "POST", "DELETE"} or not path.startswith("/api/v1/") or "#" in path:
            raise BridgeError("Invalid device resource")
        if "?" in path:
            base, query = path.split("?", 1)
            metadata = base in {"/api/v1/operations", "/api/v1/config/schema"} and re.fullmatch(r"cursor=[0-9]{1,5}&limit=[1-8]", query)
            if not metadata and not (method == "GET" and sensor_samples_path(path)):
                raise BridgeError("Invalid bounded metadata pagination")
        encoded = None if body is None else json.dumps(body, separators=(",", ":"), ensure_ascii=False,
                                                     allow_nan=False).encode()
        if encoded is not None and len(encoded) > 2048:
            raise BridgeError("Device command exceeds 2048 bytes")
        headers = {"Accept": "application/json", "Connection": "keep-alive"}
        if encoded is not None:
            headers["Content-Type"] = "application/json"
        if token:
            if not re.fullmatch(r"[a-f0-9]{64}", token):
                raise BridgeError("Invalid session token")
            headers["Authorization"] = "Bearer " + token
        try:
            connection = getattr(self, "connection", None)
            if connection is None or connection.sock is None:
                self.close()
                connection = http.client.HTTPSConnection(self.host, self.port, context=self.context, timeout=4)
                self.connection = connection
                connection.connect()
            # Check before every send, including after a server-closed socket
            # has required a new handshake. Failed requests are never replayed.
            actual = connection.sock.getpeercert(binary_form=True)
            if hashlib.sha256(actual).hexdigest() != self.digest:
                raise BridgeError("Device certificate pin mismatch")
            connection.request(method, path, encoded, headers)
            response = connection.getresponse()
            if response.status in range(300, 400):
                raise BridgeError("Device redirect rejected")
            if response.getheader("Content-Encoding") or response.getheader("Content-Type", "").split(";")[0] != "application/json":
                raise BridgeError("Unexpected device response encoding")
            raw = response.read(MAX_RESPONSE + 1)
            if len(raw) > MAX_RESPONSE:
                raise BridgeError("Device response exceeds limit")
            declared = response.getheader("Content-Length")
            if declared is not None and (len(declared) > 4 or not re.fullmatch(r"0|[1-9][0-9]*", declared) or int(declared) != len(raw)):
                raise BridgeError("Incomplete device response")
            if getattr(response, "will_close", False):
                self.close()
            return response.status, raw
        except BridgeError:
            self.close()
            raise
        except (OSError, ssl.SSLError, http.client.HTTPException) as error:
            self.close()
            # SDK/HTTP error strings may contain resource or credential data.
            raise BridgeError("Verified device connection failed") from error

def load_profile(path: Path) -> dict:
    if path.stat().st_size > 8192:
        raise BridgeError("Connection profile exceeds limit")
    config = strict_json(path.read_bytes())
    if not isinstance(config, dict):
        raise BridgeError("Invalid connection profile")
    return config
