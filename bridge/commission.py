#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Create private local enrollment packages; transport only on explicit --port."""
from __future__ import annotations
import argparse
import datetime as dt
import hashlib
import ipaddress
import json
import os
from pathlib import Path
import re
import secrets
import sys
import time
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import NameOID
from secure_transport import BridgeError, MacKeychain, Keychain, strict_json

ROOT = Path(__file__).resolve().parents[1]
SCOPES = set(json.loads((ROOT / "api/control-v1.json").read_text())["scopes"])

def write_private(path: Path, data: bytes) -> None:
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, "wb") as file:
        file.write(data)
        file.flush()
        os.fsync(file.fileno())

def create_package(output: Path, host: str, port: int, scopes: list[str],
                   time_servers: list[str], grants: list[dict], keychain: Keychain,
                   export_client_secret: bool = False, update_source: dict | None = None,
                   update_policy: dict | None = None, replace_unknown_store: bool = False) -> dict:
    output = output.expanduser().resolve()
    if output == ROOT or ROOT in output.parents:
        raise BridgeError("Private enrollment output must be outside this repository")
    if not re.fullmatch(r"[A-Za-z0-9.-]{1,128}", host) or not 1 <= port <= 65535:
        raise BridgeError("Invalid device host/port")
    if not scopes or len(set(scopes)) != len(scopes) or not set(scopes) <= SCOPES or "view" not in scopes:
        raise BridgeError("Explicit unique scopes including view are required")
    if len(time_servers) > 2 or any(not re.fullmatch(r"[A-Za-z0-9.-]{1,128}", s) for s in time_servers):
        raise BridgeError("Invalid explicitly configured time servers")
    if len(grants) > 8:
        raise BridgeError("At most eight capture grants")
    key = ec.generate_private_key(ec.SECP256R1())
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, host)])
    try:
        san = x509.IPAddress(ipaddress.ip_address(host))
    except ValueError:
        san = x509.DNSName(host)
    now = dt.datetime.now(dt.timezone.utc)
    certificate = (x509.CertificateBuilder().subject_name(name).issuer_name(name)
                   .public_key(key.public_key()).serial_number(x509.random_serial_number())
                   .not_valid_before(now - dt.timedelta(days=1)).not_valid_after(now + dt.timedelta(days=365))
                   .add_extension(x509.SubjectAlternativeName([san]), critical=False)
                   .add_extension(x509.BasicConstraints(ca=True, path_length=0), critical=True)
                   .sign(key, hashes.SHA256()))
    pem = certificate.public_bytes(serialization.Encoding.PEM).decode()
    private = key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                serialization.NoEncryption()).decode()
    secret, principal = secrets.token_hex(32), secrets.token_hex(16)
    prepared = []
    for proposal in grants:
        g = dict(proposal)
        # Every issuance has a fresh ID and explicit reviewed principal.
        if any(k in g for k in ("id", "principal_id", "revision")):
            raise BridgeError("Grant IDs/principal/revision are issued by the enrollment tool")
        g.update(id=secrets.token_hex(16), principal_id=principal, revision="1")
        validate_grant(g, scopes)
        prepared.append(g)
    package = {"v": 1, "host": host, "port": port, "certificate_pem": pem, "private_key_pem": private,
               "principals": [{"id": principal, "credential_sha256": hashlib.sha256(bytes.fromhex(secret)).hexdigest(),
                               "scopes": scopes, "native_only": True, "enabled": True}],
               "capture_grants": prepared, "time_servers": time_servers}
    if update_source is not None:
        validate_update_source(update_source)
        package["update_source"] = dict(update_source)
    if update_policy is not None:
        if update_source is None:
            raise BridgeError("An update policy requires an explicit update source")
        policy = dict(update_policy)
        if any(k in policy for k in ("id", "principal_id", "revision")):
            raise BridgeError("Update policy identities are issued by the enrollment tool")
        policy.update(id=secrets.token_hex(16), principal_id=principal, revision="1")
        validate_update_policy(policy, scopes)
        package["update_policy"] = policy
    if replace_unknown_store:
        package["replace_unknown_store"] = True
    encoded = json.dumps(package, sort_keys=True, separators=(",", ":"), allow_nan=False).encode()
    if len(encoded) > 8192:
        raise BridgeError("Enrollment package exceeds 8192 bytes; reduce principals/grants")
    service = "esp32-control." + principal
    profile = {"v": 1, "host": host, "port": port, "principal_id": principal,
               "certificate_pem": pem,
               "certificate_der_sha256": certificate.fingerprint(hashes.SHA256()).hex(),
               "keychain_service": service, "keychain_account": principal}
    output.mkdir(mode=0o700, parents=False, exist_ok=False)
    write_private(output / "device-package.json", encoded)
    write_private(output / "client-profile.json", json.dumps(profile, indent=2).encode() + b"\n")
    if export_client_secret:
        write_private(output / "client-secret-backup.json", json.dumps({"principal_id": principal, "credential": secret}).encode())
    keychain.put(service, principal, secret)
    # This summary is safe to display; keys, credentials and candidate payload
    # are intentionally absent. The device Review displays its parsed summary.
    return {"package_sha256": hashlib.sha256(encoded).hexdigest(), "certificate_pem_sha256": hashlib.sha256(pem.encode()).hexdigest(), "certificate_der_sha256": profile["certificate_der_sha256"],
            "principal_id": principal, "capture_grants": [g["id"] for g in prepared]}

def validate_grant(g: dict, scopes: list[str]) -> None:
    required = {"id", "principal_id", "revision", "expires_unix_seconds", "source", "purpose", "maximum_bytes",
                "maximum_duration_seconds", "maximum_retention_seconds", "live_destination", "file_destination", "owner_download"}
    if set(g) != required or g["source"] != "local_microphone" or g["purpose"] not in {"speech", "live_capture", "recording", "background_recording"}:
        raise BridgeError("Invalid capture grant fields/purpose")
    if not isinstance(g["expires_unix_seconds"], str) or not g["expires_unix_seconds"].isdigit() or int(g["expires_unix_seconds"]) <= int(time.time()):
        raise BridgeError("Grant needs an explicit future expiry")
    maximum = g["maximum_bytes"]
    if not isinstance(maximum, str) or not maximum.isdigit() or not 1 <= int(maximum) <= 8589934592:
        raise BridgeError("Grant bytes must be a decimal string within 8 GiB")
    if type(g["maximum_duration_seconds"]) is not int or not 1 <= g["maximum_duration_seconds"] <= 28800:
        raise BridgeError("Grant duration exceeds eight hours")
    if type(g["maximum_retention_seconds"]) is not int or not 0 <= g["maximum_retention_seconds"] <= 31536000:
        raise BridgeError("Invalid retention")
    if any(type(g[k]) is not bool for k in ("live_destination", "file_destination", "owner_download")):
        raise BridgeError("Grant destinations/export must be Boolean")
    if not (g["live_destination"] or g["file_destination"]) or (g["owner_download"] and not g["file_destination"]):
        raise BridgeError("Invalid destination/export combination")
    if g["purpose"] == "speech" and (g["maximum_duration_seconds"] > 8 or g["file_destination"]):
        raise BridgeError("Speech grants are limited to eight seconds and no files")
    if g["purpose"] == "background_recording" and (g["live_destination"] or not g["file_destination"]):
        raise BridgeError("Background grants permit local files only")
    needed = "recording.control" if g["purpose"] in {"recording", "background_recording"} else "audio.capture"
    if needed not in scopes:
        raise BridgeError("Principal lacks the matching capture scope")

def validate_update_source(source: dict) -> None:
    from urllib.parse import urlsplit
    required = {"catalog_url", "tls_ca_pem", "signing_key_id", "signing_public_key", "product", "board", "layout", "minimum_release_index", "allow_private_source"}
    if not isinstance(source, dict) or set(source) != required:
        raise BridgeError("Invalid update source fields")
    url = urlsplit(source["catalog_url"])
    if url.scheme != "https" or not url.hostname or url.username or url.password or url.query or url.fragment or not url.path or len(source["catalog_url"]) >= 512:
        raise BridgeError("Update catalog needs an explicit HTTPS origin and path")
    if source["layout"] != "esp32s31-16m-ota6-v3" or type(source["minimum_release_index"]) is not int or not 0 <= source["minimum_release_index"] <= 2**32-1 or type(source["allow_private_source"]) is not bool:
        raise BridgeError("Invalid update layout/release/private-source policy")
    try:
        x509.load_pem_x509_certificate(source["tls_ca_pem"].encode())
        if len(source["tls_ca_pem"].encode()) >= 2049:
            raise ValueError("CA limit")
        encoded = bytes.fromhex(source["signing_public_key"])
        if len(encoded) != 65 or encoded[0] != 4:
            raise ValueError("Key limit")
        ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), encoded)
    except (ValueError, TypeError) as error:
        raise BridgeError("Invalid update TLS/signing trust") from error
    for key in ("signing_key_id", "product", "board"):
        if not isinstance(source[key], str) or not re.fullmatch(r"[A-Za-z0-9_.-]{1,64}", source[key]):
            raise BridgeError("Invalid update identity")

def validate_update_policy(policy: dict, scopes: list[str]) -> None:
    required = {"id", "principal_id", "revision", "expires_unix_seconds", "maximum_job_ms", "maximum_bytes", "allow_download", "allow_activate", "allow_restart", "continue_after_owner_loss", "allow_signed_releases"}
    if set(policy) not in (required, required | {"allowed_digest"}):
        raise BridgeError("Invalid update policy fields")
    for key in ("allow_download", "allow_activate", "allow_restart", "continue_after_owner_loss", "allow_signed_releases"):
        if type(policy[key]) is not bool:
            raise BridgeError("Update policy flags must be Boolean")
    if not isinstance(policy["expires_unix_seconds"], str) or not policy["expires_unix_seconds"].isdigit() or int(policy["expires_unix_seconds"]) <= int(time.time()):
        raise BridgeError("Update policy needs a finite future expiry")
    if type(policy["maximum_job_ms"]) is not int or not 1000 <= policy["maximum_job_ms"] <= 600000 or type(policy["maximum_bytes"]) is not int or not 1 <= policy["maximum_bytes"] <= 0x600000:
        raise BridgeError("Update job exceeds ten minutes or six MiB")
    for flag, scope in (("allow_download", "update.stage"), ("allow_activate", "update.activate"), ("allow_restart", "system.restart")):
        if policy[flag] and scope not in scopes:
            raise BridgeError("Update policy exceeds the principal scopes")
    if policy["allow_restart"] and not policy["allow_activate"]:
        raise BridgeError("Restart requires activation permission")
    if not policy["allow_signed_releases"] and not re.fullmatch(r"[0-9a-f]{64}", policy.get("allowed_digest", "")):
        raise BridgeError("A pinned release digest is required")

def send_package(port, package: bytes, deadline_seconds=120) -> str:
    """Boundary accepts an already opened serial object; tests supply a substitute."""
    deadline = time.monotonic() + deadline_seconds
    def exchange(line: bytes, expected: bytes) -> bytes:
        port.write(line + b"\n")
        while time.monotonic() < deadline:
            response = port.readline(384)
            if response.startswith(b"CP1 ERROR"):
                raise BridgeError("Device commissioning rejected the exchange")
            if response.startswith(expected):
                return response.strip()
        raise BridgeError("Local commissioning timed out")
    challenge = exchange(b"CP1 CHALLENGE", b"CP1 TOKEN ")
    token = challenge.removeprefix(b"CP1 TOKEN ")
    if not re.fullmatch(b"[a-f0-9]{64}", token) or not 0 < len(package) <= 8192:
        raise BridgeError("Invalid local challenge/package")
    exchange(b"CP1 BEGIN " + token + b" " + str(len(package)).encode() + b" " + hashlib.sha256(package).hexdigest().encode(), b"CP1 READY")
    for offset in range(0, len(package), 128):
        exchange(b"CP1 DATA " + str(offset).encode() + b" " + package[offset:offset+128].hex().encode(), b"CP1 ACK")
    exchange(b"CP1 END", b"CP1 PENDING_REVIEW")
    # Deliberately return pending. A USB command cannot assert local Review.
    return "pending_local_review"

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    create = sub.add_parser("create")
    create.add_argument("--output", type=Path, required=True)
    create.add_argument("--host", required=True)
    create.add_argument("--port", type=int, default=443)
    create.add_argument("--scope", action="append", required=True)
    create.add_argument("--time-server", action="append", default=[])
    create.add_argument("--grants", type=Path)
    create.add_argument("--export-client-secret", action="store_true")
    create.add_argument("--update-source", type=Path)
    create.add_argument("--update-policy", type=Path)
    create.add_argument("--replace-unknown-store", action="store_true")
    send = sub.add_parser("send")
    send.add_argument("--package", type=Path, required=True)
    send.add_argument("--port", required=True)
    args = parser.parse_args()
    try:
        if args.command == "create":
            grants = strict_json(args.grants.read_bytes()) if args.grants else []
            print(json.dumps(create_package(args.output, args.host, args.port, args.scope, args.time_server,
                                             grants, MacKeychain(), args.export_client_secret,
                                             strict_json(args.update_source.read_bytes()) if args.update_source else None,
                                             strict_json(args.update_policy.read_bytes()) if args.update_policy else None,
                                             args.replace_unknown_store)))
        else:
            data = args.package.read_bytes()
            if not 0 < len(data) <= 8192:
                raise BridgeError("Package exceeds limit")
            import serial
            with serial.Serial(args.port, baudrate=115200, timeout=1, write_timeout=2, exclusive=True) as port:
                print(send_package(port, data))
    except (BridgeError, OSError) as error:
        print(str(error) if isinstance(error, BridgeError) else "Local enrollment I/O failed", file=sys.stderr)
        return 1
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
