#!/usr/bin/env python3
"""Compile and run the actual Mosaico adapter against controllable driver doubles."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
compiler = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
if not compiler:
    raise SystemExit("A C11 compiler is required.")
with tempfile.TemporaryDirectory(prefix="esp32-board-test-") as tmp:
    binary = str(Path(tmp) / "board_port")
    subprocess.run([
        compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g",
        "-I", str(root / "tests/board_port/stubs"),
        "-I", str(root / "firmware/main"),
        "-I", str(root / "components/haptics/include"),
        str(root / "tests/board_port/test_board_port.c"),
        str(root / "components/haptics/haptics.c"), "-lm", "-o", binary,
    ], check=True)
    subprocess.run([binary], check=True)
