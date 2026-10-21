#!/usr/bin/env python3
"""Compile production Bluetooth audio against deterministic SDK boundaries."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SUITE = Path(__file__).resolve().parent


def main():
    compiler = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
    if not compiler:
        raise SystemExit("A C11 compiler with address/undefined sanitizers is required.")
    with tempfile.TemporaryDirectory(prefix="esp32-bt-audio-") as temporary:
        binary = Path(temporary) / "test_bt_audio"
        command = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-Wconversion", "-Wno-sign-conversion", "-g",
                   "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                   "-I", str(SUITE / "stubs"), "-I", str(ROOT / "firmware/main"),
                   "-I", str(ROOT / "components/audio_engine/include"),
                   "-I", str(ROOT / "components/device_settings/include"),
                   str(SUITE / "test_bt_audio.c"), str(ROOT / "firmware/main/bt_audio.c"),
                   "-o", str(binary)]
        subprocess.run(command, check=True)
        for scenario in ("normal", "init-timeout", "init-event-failure", "command-timeout",
                         "command-failure", "allocation-failure", "registration-failure", "stack-unready",
                         "pairing", "pairing-expiry", "pairing-timeout", "discovery-timeout", "playback-binding"):
            subprocess.run([str(binary), scenario], check=True)
        subprocess.run(command + ["-DCONFIG_BT_CLASSIC_ENABLED=0"], check=True)
        subprocess.run([str(binary), "unsupported"], check=True)
        subprocess.run(command + ["-DESP_BT_GAP_JUST_WORKS_CONFIRM_REQUIRED=0"], check=True)
        subprocess.run([str(binary), "unsupported"], check=True)


if __name__ == "__main__":
    main()
