#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Collect bounded authenticated LAN status; never issue device commands."""
from __future__ import annotations
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "bridge"))
from control_bridge import DeviceSession
from secure_transport import BridgeError, MacKeychain, PinnedHTTPS, load_profile

INITIAL = ("/api/v1/sensors/catalog",)
RESOURCES = ("/api/v1/state", "/api/v1/diagnostics/runtime",
             "/api/v1/firmware/state", "/api/v1/clock")
RECEIPT_RESERVE = 8192


class CollectionError(Exception):
    """Fixed error codes only; no credentials or response text."""


def collect(session, prefix: Path, seconds: float = 0, interval: float = 1,
            max_bytes: int = 8 * 1024 * 1024, *, clock=time.monotonic, sleep=time.sleep):
    if not (math.isfinite(seconds) and 0 <= seconds <= 600 and
            math.isfinite(interval) and 0.5 <= interval <= 60 and
            65536 <= max_bytes <= 16 * 1024 * 1024):
        raise CollectionError("invalid_collection_bounds")
    prefix = Path(prefix)
    paths = [Path(str(prefix) + suffix) for suffix in (".jsonl", ".receipt.json")]
    files = []
    try:
        for path in paths:
            fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            files.append(os.fdopen(fd, "wb"))
    except OSError:
        for file in files:
            file.close()
        raise CollectionError("output_unavailable_or_exists") from None
    digest = hashlib.sha256()
    count = size = rounds = 0
    issues = set()
    statuses = {}
    boot = None
    started = clock()
    completed = False
    error = None
    catalog = None

    def record(resource):
        nonlocal size, count, catalog
        before = clock()
        response = session.read(resource)
        if not isinstance(response, dict) or type(response.get("http_status")) is not int:
            raise CollectionError("invalid_response")
        status = response["http_status"]
        statuses[resource] = status
        # Only GET response bodies are retained. Login/session tokens and
        # credentials never enter this evidence path or the receipt.
        line = json.dumps({"resource": resource, "started_seconds": before - started,
                           "finished_seconds": clock() - started,
                           "read_attempts": session.read_attempts, "response": response},
                          separators=(",", ":"), allow_nan=False).encode() + b"\n"
        if size + len(line) > max_bytes - RECEIPT_RESERVE:
            raise CollectionError("output_limit")
        files[0].write(line)
        files[0].flush()
        digest.update(line)
        size += len(line)
        count += 1
        if status != 200:
            issues.add(resource + ":http_" + str(status))
            if status == 401:
                raise CollectionError("session_lost")
            return
        if resource == "/api/v1/state":
            protocol = response.get("protocol")
            if not isinstance(protocol, dict) or protocol.get("boot_id") != boot:
                raise CollectionError("boot_identity_changed_or_missing")
        elif resource == "/api/v1/sensors/catalog":
            catalog = {key: response.get(key) for key in
                       ("boot", "git_commit", "git_state", "elf_sha256", "config_sha256")}

    try:
        session.connect()
        boot = session.login.get("boot_id") if isinstance(session.login, dict) else None
        if not isinstance(boot, str) or not re.fullmatch(r"[0-9a-f]{32}", boot):
            raise CollectionError("invalid_authenticated_boot")
        for resource in INITIAL:
            record(resource)
        deadline = clock() + seconds
        while True:
            round_start = clock()
            for resource in RESOURCES:
                record(resource)
            rounds += 1
            if clock() >= deadline:
                break
            sleep(max(0, min(deadline - clock(), interval - (clock() - round_start))))
        # The final state check makes a reboot during the last GET visible.
        record("/api/v1/state")
        completed = True
    except CollectionError as caught:
        error = str(caught)
    except BridgeError:
        error = "authenticated_connection_failed"
    except (OSError, ValueError, TypeError, KeyError, OverflowError):
        error = "collection_io_or_response_failed"
    except KeyboardInterrupt:
        error = "interrupted"
    finally:
        try:
            session.close()
        except (BridgeError, OSError):
            issues.add("session_close_unconfirmed")
        receipt = {"schema": 1, "collection_completed": completed, "error": error,
                   "all_resources_observed": completed and not issues,
                   "device_health_qualified": False, "logout_acknowledged": None,
                   "authenticated_boot": boot, "image_identity": catalog,
                   "rounds": rounds, "responses": count, "bytes": size,
                   "elapsed_seconds": clock() - started,
                   "resource_http_status": statuses, "issues": sorted(issues),
                   "data_sha256": digest.hexdigest(),
                   "evidence_kind": "bounded_status_observations_not_raw_logs"}
        try:
            files[0].flush()
            os.fsync(files[0].fileno())
            encoded = json.dumps(receipt, indent=2, allow_nan=False).encode() + b"\n"
            if len(encoded) > RECEIPT_RESERVE:
                raise CollectionError("receipt_limit")
            files[1].write(encoded)
            files[1].flush()
            os.fsync(files[1].fileno())
        finally:
            for file in files:
                file.close()
    return receipt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True,
                        help="New private .jsonl and .receipt.json prefix")
    parser.add_argument("--seconds", type=float, default=0,
                        help="0 for one probe; otherwise a bounded window up to 600 seconds")
    parser.add_argument("--interval", type=float, default=1)
    parser.add_argument("--max-bytes", type=int, default=8 * 1024 * 1024)
    args = parser.parse_args()
    try:
        profile = load_profile(args.profile)
        session = DeviceSession(profile, PinnedHTTPS(profile), MacKeychain())
        receipt = collect(session, args.output, args.seconds, args.interval, args.max_bytes)
        print(json.dumps({key: receipt[key] for key in
                          ("collection_completed", "all_resources_observed", "error", "issues")}))
        return 0 if receipt["all_resources_observed"] else 1
    except (BridgeError, CollectionError, OSError, ValueError):
        print("LAN collection could not start; check the enrolled profile and new output path", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
