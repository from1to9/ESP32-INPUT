#!/usr/bin/env python3
"""Test the actual board touch observer at its LCD/adapter boundary."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
suite = Path(__file__).resolve().parent
compiler = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
if not compiler:
    raise SystemExit("A C11 compiler with sanitizers is required.")
with tempfile.TemporaryDirectory(prefix="esp32-touch-") as directory:
    binary = str(Path(directory) / "board_touch")
    subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                    "-Wconversion", "-Wshadow", "-g", "-fsanitize=address,undefined",
                    "-fno-omit-frame-pointer", "-I", str(suite / "stubs"),
                    "-I", str(root / "tests/coordinator/stubs"),
                    "-I", str(root / "firmware/main"),
                    str(suite / "test_board_touch.c"), "-o", binary], check=True)
    subprocess.run([binary], check=True)
    vendor_binary = str(Path(directory) / "cst9220")
    vendor = root / "firmware/managed_components/espressif__esp_lcd_touch_cst9220"
    subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                    "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-I", str(suite / "vendor_stubs"), "-I", str(suite / "stubs"),
                    "-I", str(root / "tests/coordinator/stubs"),
                    "-I", str(root / "firmware/main"),
                    "-I", str(root / "components/interaction/include"),
                    "-isystem", str(vendor / "include"),
                    "-isystem", str(root / "firmware/managed_components/espressif__esp_lcd_touch/include"),
                    str(suite / "test_cst9220.c"),
                    str(root / "components/interaction/interaction_multitouch.c"),
                    "-lm", "-o", vendor_binary], check=True)
    subprocess.run([vendor_binary], check=True)
