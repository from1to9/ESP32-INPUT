#!/usr/bin/env python3
"""Exercise production cross-task command ownership without SDK or devices."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
compiler = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
if not compiler:
    raise SystemExit("A C11 compiler with sanitizers is required.")
with tempfile.TemporaryDirectory(prefix="esp32-command-bridge-") as directory:
    binary = str(Path(directory) / "test")
    subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
        "-Wconversion", "-Wshadow", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        "-I", str(root / "firmware/main"), "-I", str(root / "components/control_protocol/include"),
        str(root / "tests/command_bridge/test_command_bridge.c"),
        str(root / "firmware/main/app_command_bridge.c"), "-o", binary], check=True)
    subprocess.run([binary], check=True)

    module = root / "components/control_protocol"
    protocol = str(Path(directory) / "protocol")
    subprocess.run([compiler, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra",
        "-Werror", "-pedantic", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        "-I", str(root / "firmware/main"), "-I", str(module / "include"), "-I", str(module),
        str(root / "tests/command_bridge/test_protocol.c"), str(root / "firmware/main/app_command_bridge.c"),
        *[str(module / name) for name in ("control_protocol.c", "control_registry.c", "control_json.c", "control_sha256.c")],
        "-lm", "-o", protocol], check=True)
    subprocess.run([protocol], check=True)
