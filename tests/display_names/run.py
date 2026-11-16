#!/usr/bin/env python3
"""Exercise the label catalog at the NVS and compiled glyph lookup boundaries."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SUITE = Path(__file__).resolve().parent
PROTOCOL = ROOT / "components/control_protocol"
with tempfile.TemporaryDirectory(prefix="esp32-display-names-") as directory:
    target = Path(directory) / "test_display_names"
    compiler = os.environ.get("CC") or shutil.which("clang") or "cc"
    command = [compiler, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror", "-pedantic",
               "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    for path in (SUITE / "stubs", ROOT / "tests/ble_hid/stubs", PROTOCOL, PROTOCOL / "include"):
        command += ["-I", str(path)]
    subprocess.run(command + [str(SUITE / "test_display_names.c"), str(PROTOCOL / "control_json.c"),
                              str(PROTOCOL / "control_sha256.c"), "-o", str(target)], check=True)
    subprocess.run([str(target)], check=True)
