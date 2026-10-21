#!/usr/bin/env python3
"""Exercise the real bounded multi-contact recognizer without device access."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
compiler = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
if not compiler:
    raise SystemExit("A C11 compiler with address/undefined sanitizers is required.")
with tempfile.TemporaryDirectory(prefix="esp32-multitouch-") as temporary:
    binary = Path(temporary) / "multitouch"
    subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                    "-Wconversion", "-Wshadow", "-fsanitize=address,undefined", "-g",
                    "-fno-omit-frame-pointer", "-I", str(ROOT / "components/interaction/include"),
                    str(ROOT / "components/interaction/interaction_multitouch.c"),
                    str(Path(__file__).with_name("test_multitouch.c")), "-lm", "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
