#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Offline publisher signing for the bounded v3 OTA catalog. No network/device I/O."""
import argparse
import datetime as dt
import hashlib
import json
from pathlib import Path
import re
import struct
import sys
from urllib.parse import urlsplit
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec, utils
from commission import ROOT, write_private
from secure_transport import BridgeError

LAYOUT = "esp32s31-16m-ota6-v3"
MAX_IMAGE = 0x600000
SETTINGS_SCHEMA = int(re.search(r"^#define DEVICE_SETTINGS_SCHEMA_VERSION (\d+)U?$",
    (ROOT / "components/device_settings/include/device_settings.h").read_text(), re.M).group(1))

def private_destination(path: Path):
    path = path.expanduser().resolve()
    if path == ROOT or ROOT in path.parents:
        raise BridgeError("Publisher private keys must stay outside this repository")
    return path

def image_identity(data: bytes):
    # Pinned ESP-IDF esp_image_header_t (24), first segment header (8),
    # esp_app_desc_t: version at +16, project_name at +48. SDK verification
    # on the device remains authoritative for the full executable image.
    if not 288 <= len(data) <= MAX_IMAGE or data[0] != 0xE9 or struct.unpack_from("<H", data, 12)[0] != 0x20 or struct.unpack_from("<I", data, 32)[0] != 0xABCD5432:
        raise BridgeError("Image is not a bounded ESP32-S31 application")
    if struct.unpack_from("<I", data, 28)[0] < 256:
        raise BridgeError("Image descriptor segment is incomplete")
    def text(offset):
        raw = data[offset:offset+32]
        if b"\0" not in raw:
            raise BridgeError("Unterminated image identity")
        value = raw.split(b"\0", 1)[0].decode("ascii")
        if not re.fullmatch(r"[A-Za-z0-9_.+-]{1,31}", value):
            raise BridgeError("Invalid image identity")
        return value
    return text(48), text(80)

def sign_release(image: bytes, key_pem: bytes, *, key_id: str, product: str, board: str,
                 release_id: str, release_index: int, url: str, expires: int,
                 settings_schema: int = SETTINGS_SCHEMA) -> dict:
    version, project = image_identity(image)
    for value in (key_id, product, board, release_id):
        if not re.fullmatch(r"[A-Za-z0-9_.-]{1,64}", value):
            raise BridgeError("Invalid release identity")
    source = urlsplit(url)
    now = int(dt.datetime.now(dt.timezone.utc).timestamp())
    if source.scheme != "https" or not source.hostname or source.username or source.password or source.query or source.fragment or not source.path or len(url) >= 512:
        raise BridgeError("Artifact needs an explicit HTTPS origin and path")
    if not 0 <= release_index <= 2**32-1 or not now < expires <= now+604800 or settings_schema not in (4, SETTINGS_SCHEMA):
        raise BridgeError("Invalid release index, finite expiry or settings schema")
    key = serialization.load_pem_private_key(key_pem, password=None)
    if not isinstance(key, ec.EllipticCurvePrivateKey) or not isinstance(key.curve, ec.SECP256R1):
        raise BridgeError("Publisher signing requires an explicit P-256 private key")
    payload = {"v": 1, "release_id": release_id, "release_index": release_index,
               "product": product, "board": board, "chip": "esp32s31", "project": project,
               "layout": LAYOUT, "settings_schema": settings_schema, "version": version,
               "size": len(image), "sha256": hashlib.sha256(image).hexdigest(),
               "url": url, "expires_unix_seconds": str(expires)}
    encoded = json.dumps(payload, sort_keys=True, separators=(",", ":"), ensure_ascii=True)
    r, s = utils.decode_dss_signature(key.sign(encoded.encode(), ec.ECDSA(hashes.SHA256())))
    return {"v": 1, "alg": "ES256", "key_id": key_id, "payload": encoded,
            "signature": (r.to_bytes(32, "big") + s.to_bytes(32, "big")).hex()}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    keygen = commands.add_parser("keygen")
    keygen.add_argument("--output", required=True, type=Path)
    sign = commands.add_parser("sign")
    for flag in ("image", "private-key", "output"):
        sign.add_argument("--" + flag, required=True, type=Path)
    for flag in ("key-id", "product", "board", "release-id", "url"):
        sign.add_argument("--" + flag, required=True)
    sign.add_argument("--release-index", required=True, type=int)
    sign.add_argument("--expires", required=True, type=int)
    sign.add_argument("--settings-schema", type=int, choices=(4, SETTINGS_SCHEMA), default=SETTINGS_SCHEMA,
                      help="schema accepted by the running recipient (default: %(default)s); use 4 when upgrading schema-4 firmware")
    args = parser.parse_args()
    try:
        if args.command == "keygen":
            key = ec.generate_private_key(ec.SECP256R1())
            write_private(private_destination(args.output), key.private_bytes(serialization.Encoding.PEM,
                          serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
            public = key.public_key().public_bytes(serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint)
            print(json.dumps({"signing_public_key": public.hex(), "sha256": hashlib.sha256(public).hexdigest()}))
        else:
            key = private_destination(args.private_key).read_bytes()
            if args.image.stat().st_size > MAX_IMAGE:
                raise BridgeError("Image exceeds six MiB")
            catalog = sign_release(args.image.read_bytes(), key, key_id=args.key_id, product=args.product,
                                   board=args.board, release_id=args.release_id, release_index=args.release_index,
                                   url=args.url, expires=args.expires, settings_schema=args.settings_schema)
            write_private(args.output, json.dumps(catalog, indent=2).encode() + b"\n")
            print(json.dumps({"catalog_sha256": hashlib.sha256(args.output.read_bytes()).hexdigest()}))
    except (BridgeError, OSError, ValueError):
        print("Offline release signing failed; no private key was printed", file=sys.stderr)
        return 1
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
