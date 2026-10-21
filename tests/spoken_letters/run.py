#!/usr/bin/env python3
"""Exercise production spoken-letter window/frame policy, without microphone access."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
compiler = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
if not compiler:
    raise SystemExit("A C11 compiler with address/undefined sanitizers is required.")
with tempfile.TemporaryDirectory(prefix="esp32-spoken-letters-") as temporary:
    binary = Path(temporary) / "spoken_letters"
    subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                    "-Wconversion", "-Wshadow", "-fsanitize=address,undefined", "-g",
                    "-fno-omit-frame-pointer", "-I", str(ROOT / "components/spoken_letters/include"),
                    "-I", str(ROOT / "components/audio_engine/include"),
                    str(ROOT / "components/spoken_letters/spoken_letters.c"),
                    str(Path(__file__).with_name("test_spoken_letters.c")), "-lm", "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

with tempfile.TemporaryDirectory(prefix="esp32-spoken-runtime-") as temporary:
    binary = Path(temporary) / "runtime"
    subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                    "-Wconversion", "-Wshadow", "-fsanitize=address,undefined", "-g",
                    "-fno-omit-frame-pointer", "-I", str(Path(__file__).parent / "stubs"),
                    "-I", str(ROOT / "components/spoken_letters/include"),
                    "-I", str(ROOT / "components/audio_engine/include"),
                    str(ROOT / "components/spoken_letters/spoken_letters.c"),
                    str(Path(__file__).with_name("test_runtime.c")), "-lm", "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
