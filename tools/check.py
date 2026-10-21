#!/usr/bin/env python3
"""Run meaningful portable tests without installing the ESP-IDF toolchain."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
compiler = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
if not compiler:
    raise SystemExit("A C11 compiler is required (clang or cc).")
with tempfile.TemporaryDirectory(prefix="esp32-input-test-") as tmp:
    binary = str(Path(tmp) / "input_core_test")
    subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                    "-fsanitize=address,undefined", "-g",
                    "-I", str(root / "components/input_core/include"),
                    str(root / "components/input_core/input_core.c"),
                    str(root / "tests/test_input_core.c"), "-lm", "-o", binary], check=True)
    subprocess.run([binary], check=True)
print("Host tests only; BSP build and hardware compatibility remain unverified.")
