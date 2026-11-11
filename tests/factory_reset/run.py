#!/usr/bin/env python3
"""Exercise production reset storage at SDK boundaries; never access a device."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import sys

root = Path(__file__).resolve().parents[2]
suite = Path(__file__).resolve().parent
compiler = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
if not compiler:
    raise SystemExit("A strict C11 compiler with sanitizers is required.")
with tempfile.TemporaryDirectory(prefix="esp32-device-restart-") as temporary:
    binary = str(Path(temporary) / "restart")
    subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-Wconversion", "-Wshadow",
                    "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-I", str(suite / "stubs"),
                    "-I", str(root / "tests/board_audio/stubs"), str(suite / "test_reset.c"), "-o", binary], check=True)
    subprocess.run([binary, *sys.argv[1:]], check=True)
