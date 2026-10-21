#!/usr/bin/env python3
"""Exercise the real display driver-interface observer without a device."""
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
with tempfile.TemporaryDirectory(prefix="esp32-display-transfer-") as temp:
    binary = str(Path(temp) / "display_transfer")
    subprocess.run([
        compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-g",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        "-I", str(suite / "stubs"), "-I", str(root / "tests/board_audio/stubs"),
        "-I", str(root / "firmware/main"),
        str(suite / "test_display_transfer.c"), "-o", binary,
    ], check=True)
    subprocess.run([binary], check=True)
