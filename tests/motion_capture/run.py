#!/usr/bin/env python3
"""Test the production bounded IMU observer without hardware or SDK access."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
compiler = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
if not compiler:
    raise SystemExit("A C11 compiler with sanitizers is required.")
with tempfile.TemporaryDirectory(prefix="esp32-motion-capture-") as temporary:
    binary = Path(temporary) / "test_capture"
    subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                    "-Wconversion", "-Wshadow", "-fsanitize=address,undefined",
                    "-fno-omit-frame-pointer", "-g", "-I", str(ROOT / "firmware/main"),
                    str(ROOT / "firmware/main/motion_capture.c"),
                    str(ROOT / "tests/motion_capture/test_capture.c"), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
subprocess.run([sys.executable, str(ROOT / "tests/motion_capture/test_reader.py")], check=True)
