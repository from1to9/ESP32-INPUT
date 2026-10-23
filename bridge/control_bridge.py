#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""One stdio MCP peer owns one independently authenticated device session."""
from __future__ import annotations
import argparse
import asyncio
import hashlib
import json
from pathlib import Path
import re
import sys
from decimal import Decimal, InvalidOperation
from jsonschema import Draft202012Validator, FormatChecker, ValidationError, validators
from secure_transport import BridgeError, Keychain, MacKeychain, PinnedHTTPS, load_profile

ROOT = Path(__file__).resolve().parents[1]
TOOLS = json.loads((ROOT / "bridge/tools.json").read_text())["tools"]
OPERATIONS = {o["name"]: o for o in json.loads((ROOT / "api/control-v1.json").read_text())["operations"]}
CATALOG_DIGEST = hashlib.sha256((ROOT / "api/control-v1.json").read_bytes()).hexdigest()
FORMAT = FormatChecker()

@FORMAT.checks("uint64")
def decimal_u64(value):
    return not isinstance(value, str) or bool(re.fullmatch(r"0|[1-9][0-9]{0,19}", value) and int(value) <= 2**64-1)

@FORMAT.checks("id128")
def identifier(value):
    return not isinstance(value, str) or bool(re.fullmatch(r"[0-9a-f]{32}", value))

@FORMAT.checks("sha256")
def digest(value):
    return not isinstance(value, str) or bool(re.fullmatch(r"[0-9a-f]{64}", value))

def wire_text(value, *, ascii_only=False):
    if not isinstance(value, str):
        return True
    try:
        value.encode("utf-8")
    except UnicodeEncodeError:
        return False
    return all(32 <= ord(c) and ord(c) != 127 and (not ascii_only or ord(c) <= 126) for c in value)

@FORMAT.checks("ansi_text")
def ansi_text(value):
    return wire_text(value, ascii_only=True)

@FORMAT.checks("passphrase")
def passphrase(value):
    return not isinstance(value, str) or (len(value) >= 8 and wire_text(value, ascii_only=True))

@FORMAT.checks("ssid")
def ssid(value):
    return not isinstance(value, str) or bool(value and wire_text(value))

@FORMAT.checks("device_name")
def device_name(value):
    return not isinstance(value, str) or bool(value and value[0] != " " and value[-1] != " " and
        wire_text(value) and not any(128 <= ord(c) <= 159 for c in value))

def wire_length(validator, bound, instance, schema, *, maximum):
    if not validator.is_type(instance, "string"):
        return
    try:
        length = len(instance.encode("utf-8"))
    except UnicodeEncodeError:
        yield ValidationError("String is not valid UTF-8")
        return
    if (maximum and length > bound) or (not maximum and length < bound):
        yield ValidationError("String violates device UTF-8 byte length")

def wire_max_length(validator, bound, instance, schema):
    yield from wire_length(validator, bound, instance, schema, maximum=True)

def wire_min_length(validator, bound, instance, schema):
    yield from wire_length(validator, bound, instance, schema, maximum=False)

def exact_multiple(validator, divisor, instance, schema):
    if not validator.is_type(instance, "number"):
        return
    try:
        value, step = Decimal(str(instance)), Decimal(str(divisor))
        valid = value.is_finite() and step.is_finite() and step > 0 and value % step == 0
    except (InvalidOperation, ValueError, OverflowError):
        valid = False
    if not valid:
        yield ValidationError("Number violates exact decimal step")

# The device wire contract measures decoded strings in UTF-8 bytes, not Python
# code points. Keep the catalog's bounds and match the production C validator.
TypedValidator = validators.extend(Draft202012Validator, {"multipleOf": exact_multiple,
    "minLength": wire_min_length, "maxLength": wire_max_length})

class DeviceSession:
    def __init__(self, profile: dict, transport, keychain: Keychain):
        self.profile, self.transport, self.keychain = profile, transport, keychain
        self.login = None
        self.sequence = 1
        self.pending = None
        self.closed = False

    def connect(self):
        if self.closed:
            raise BridgeError("Logical controller is closed")
        if self.login is not None:
            return
        secret = self.keychain.get(self.profile["keychain_service"], self.profile["keychain_account"])
        response = self.transport.request("POST", "/api/v1/sessions",
                                          {"client_id": self.profile["principal_id"], "credential": secret})
        secret = None
        if response.get("http_status") != 200 or not all(isinstance(response.get(k), str) for k in
                ("session_token", "session_id", "request_prefix", "boot_id")):
            raise BridgeError("Device authentication rejected")
        if not re.fullmatch(r"[a-f0-9]{64}", response["session_token"]) or not re.fullmatch(r"[a-f0-9]{16}", response["request_prefix"]):
            raise BridgeError("Invalid authenticated session response")
        self.login = response
        self.sequence = int(response["next_seq"])
        capabilities = self.read("/api/v1/capabilities")
        if capabilities.get("catalog_sha256") != CATALOG_DIGEST:
            self.close()
            raise BridgeError("Device/bridge operation catalog mismatch; update the companion")

    def read(self, resource):
        if self.login is None or self.closed:
            raise BridgeError("No authenticated logical controller")
        return self.transport.request("GET", resource, token=self.login["session_token"])

    def heartbeat(self):
        if self.login and not self.closed:
            result = self.read("/api/v1/sessions/current")
            if result.get("http_status") != 200 or not result.get("authenticated"):
                raise BridgeError("Logical controller session lost")

    def close(self):
        if self.login and not self.closed:
            try:
                self.transport.request("DELETE", "/api/v1/sessions/current", token=self.login["session_token"])
            except BridgeError:
                # Heartbeats have stopped; server independently expires media
                # liveness and input lease. A missing ACK is not logout proof.
                pass
        self.closed = True
        self.login = None
        self.pending = None

    def _send_pending(self):
        result = self.transport.request("POST", "/api/v1/commands", self.pending,
                                        token=self.login["session_token"])
        next_seq = result.get("next_seq")
        if next_seq is None:
            # Schema/identity rejection may not consume sequence. Do not infer
            # admission, silently skip it or replay it in a fresh session.
            raise BridgeError("Command admission unknown; retry the exact pending request explicitly")
        n = int(next_seq)
        if not self.sequence <= n <= self.sequence + 1:
            raise BridgeError("Unexpected command sequence acknowledgement")
        self.sequence = n
        self.pending = None
        return result

    def invoke(self, name: str, arguments: dict):
        if name == "bridge_retry_pending":
            if arguments != {} or self.pending is None:
                raise BridgeError("No exact pending request to retry")
            return self._send_pending()
        tool = next((t for t in TOOLS if t["name"] == name), None)
        if tool is None:
            raise BridgeError("Unknown tool")
        try:
            TypedValidator(tool["inputSchema"], format_checker=FORMAT).validate(arguments)
        except ValidationError as error:
            # Never include the invalid instance (it may contain a passphrase).
            raise BridgeError("Tool arguments violate the typed device schema") from error
        self.connect()
        op, args = tool["x-device-operation"], arguments["args"]
        resource = tool.get("x-device-resource")
        if resource:
            if op in {"operations.list", "config.schema"}:
                if args:
                    resource += "?cursor=" + str(args.get("cursor", "0")) + "&limit=" + str(args.get("limit", 8))
            elif op == "sensors.samples":
                resource += "?boot=" + args["boot"] + "&after=" + args["after"] + "&limit=" + str(args.get("limit", 8))
            elif op == "wifi.scan.results":
                if args:
                    resource += "/" + str(args["cursor"])
            elif args:
                raise BridgeError("This resource currently accepts an empty argument object")
            return self.read(resource)
        if op == "job.get":
            if "command_key" in args and "job_id" not in args:
                return self.read("/api/v1/requests/" + args["command_key"])
            if "job_id" in args and "command_key" not in args:
                return self.read("/api/v1/jobs/" + args["job_id"])
            raise BridgeError("Specify exactly one command_key or job_id")
        if op == "recordings.get":
            return self.read("/api/v1/recordings/" + args["recording_id"])
        if op == "session.close":
            self.close()
            return {"state": "closed", "logout_acknowledged": False}
        if self.pending is not None:
            raise BridgeError("A previous command outcome is unknown; use bridge_retry_pending")
        state = self.read("/api/v1/state").get("protocol")
        if not isinstance(state, dict) or state.get("boot_id") != self.login["boot_id"]:
            raise BridgeError("Device state unavailable or boot changed")
        envelope = {"v": 1, "boot_id": self.login["boot_id"], "session_id": self.login["session_id"],
                    "request_id": self.login["request_prefix"] + f"{self.sequence:016x}",
                    "seq": self.sequence, "deadline_ms": str(int(state["now_ms"]) + (200 if OPERATIONS[op].get("script_movement") else 1500)),
                    "op": op, "args": args, **arguments["context"]}
        # Only use an already observed lease. Never automatically acquire or
        # renew input ownership, reinterpret an expired script or widen bounds.
        if OPERATIONS[op]["input"] and state.get("lease_active") and state.get("lease_id"):
            envelope["lease_id"] = state["lease_id"]
        if len(json.dumps(envelope, separators=(",", ":"), ensure_ascii=False).encode()) > 2048:
            raise BridgeError("Device command exceeds frame limit")
        self.pending = envelope
        return self._send_pending()

async def run(profile: Path):
    from mcp.server.lowlevel import Server
    from mcp.server.stdio import stdio_server
    from mcp import types
    session = DeviceSession(load_profile(profile), None, MacKeychain())
    session.transport = PinnedHTTPS(session.profile)
    lock = asyncio.Lock()
    server = Server("esp32-input-control")

    @server.list_tools()
    async def list_tools():
        result = [types.Tool(name=t["name"], description=t["description"], inputSchema=t["inputSchema"],
                             annotations=types.ToolAnnotations(**t["annotations"])) for t in TOOLS]
        result.append(types.Tool(name="bridge_retry_pending", description="Explicitly retry the identical pending command after an unknown network outcome; does not create a new request.",
                                 inputSchema={"type": "object", "properties": {}, "additionalProperties": False}))
        return result

    # Exact decimal validation below retains every schema check and avoids the
    # SDK validator's binary-float multipleOf rounding for centi/milli fields.
    @server.call_tool(validate_input=False)
    async def call_tool(name, arguments):
        async with lock:
            try:
                result = await asyncio.to_thread(session.invoke, name, arguments or {})
                return [types.TextContent(type="text", text=json.dumps(result, separators=(",", ":")))]
            except BridgeError as error:
                return types.CallToolResult(isError=True, content=[types.TextContent(type="text", text=str(error))])

    async def heartbeat():
        while True:
            await asyncio.sleep(1)
            async with lock:
                try:
                    await asyncio.to_thread(session.heartbeat)
                except BridgeError:
                    await asyncio.to_thread(session.close)

    heartbeat_task = asyncio.create_task(heartbeat())
    try:
        async with stdio_server() as (reader, writer):
            await server.run(reader, writer, server.create_initialization_options())
    finally:
        heartbeat_task.cancel()
        try:
            await heartbeat_task
        except asyncio.CancelledError:
            pass
        async with lock:
            await asyncio.to_thread(session.close)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", required=True, type=Path)
    args = parser.parse_args()
    try:
        asyncio.run(run(args.profile))
    except (BridgeError, OSError):
        print("Control companion stopped: protected connection/profile unavailable", file=sys.stderr)
        return 1
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
