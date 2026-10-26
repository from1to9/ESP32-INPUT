#!/usr/bin/env python3
"""Exercise production USB audio admission/FIFO barriers against pinned headers."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import sys

ROOT = Path(__file__).resolve().parents[2]
SUITE = Path(__file__).resolve().parent
compiler = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
if not compiler:
    raise SystemExit("A C11 compiler with sanitizers is required.")
with tempfile.TemporaryDirectory(prefix="esp32-uac-test-") as directory:
    binary = str(Path(directory) / "test_usb_audio")
    subprocess.run([
        compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
        "-Wconversion", "-Wshadow", "-g", "-fsanitize=address,undefined",
        "-fno-omit-frame-pointer", "-I", str(SUITE / "stubs"),
        "-I", str(ROOT / "tests/usb_hid/stubs"),
        "-isystem", str(ROOT / "firmware/managed_components/espressif__tinyusb/src"),
        "-I", str(ROOT / "components/audio_engine/include"),
        "-I", str(ROOT / "firmware/main"),
        str(SUITE / "test_usb_audio.c"), "-o", binary,
    ], check=True)
    for case in range(17):
        subprocess.run([binary, str(case)], check=True)
subprocess.run([sys.executable, str(SUITE / "test_patch.py")], check=True)
subprocess.run([sys.executable, str(SUITE / "test_config.py")], check=True)
subprocess.run([sys.executable, str(SUITE / "run_pinned.py")], check=True)
subprocess.run([sys.executable, str(SUITE / "run_dwc2.py")], check=True)
