#!/usr/bin/env python3
"""Validate canonical device control against the actual registry/settings code."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
suite = Path(__file__).resolve().parent
compiler = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
if not compiler:
    raise SystemExit("A strict C11 compiler with sanitizers is required.")
protocol = root / "components/control_protocol"
with tempfile.TemporaryDirectory(prefix="esp32-remote-device-") as temp:
    binary = str(Path(temp) / "remote_device")
    subprocess.run([
        compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-Wconversion", "-Wshadow", "-g",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        "-I", str(root / "firmware/main"), "-I", str(protocol / "include"),
        "-I", str(root / "components/device_settings/include"),
        str(suite / "test_remote_device.c"),
        str(root / "components/device_settings/device_settings.c"),
        *[str(protocol / name) for name in ("control_protocol.c", "control_registry.c", "control_json.c", "control_sha256.c")],
        "-lm", "-o", binary,
    ], check=True)
    subprocess.run([binary], check=True)
