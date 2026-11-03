#!/usr/bin/env python3
"""Run production media policy with cached owner/runtime boundaries substituted."""
from pathlib import Path
import shutil
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="esp32-media-controls-") as directory:
    binary = Path(directory) / "test_media_controls"
    command = [shutil.which("clang") or "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wconversion", "-Wshadow", "-pedantic",
               "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-I", str(ROOT / "tests/presenter/stubs"),
               "-I", str(ROOT / "components/input_core/include"), "-I", str(ROOT / "components/audio_engine/include"),
               "-I", str(ROOT / "components/device_settings/include"),
               "-I", str(ROOT / "components/spoken_letters/include"), str(ROOT / "tests/media_controls/test_media_controls.c"),
               str(ROOT / "components/audio_engine/audio_media.c"), "-lm", "-o", str(binary)]
    subprocess.run(command, check=True)
    for case in subprocess.check_output([str(binary), "--list"], text=True).splitlines():
        subprocess.run([str(binary), case], check=True, timeout=20)
