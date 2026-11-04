#!/usr/bin/env python3
"""Compose the real device controller and Classic dispatcher at SDK boundaries."""
from pathlib import Path
import argparse
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SUITE = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--case")
args = parser.parse_args()
compiler = shutil.which("clang") or "cc"
with tempfile.TemporaryDirectory(prefix="esp32-device-owner-") as directory:
    temporary = Path(directory)
    common = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wconversion", "-Wno-sign-conversion",
              "-pedantic", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    radio = common + ["-DTEST_BT_AUDIO_LOCKS", "-I", str(ROOT / "tests/bt_audio/stubs"),
                     "-I", str(ROOT / "firmware/main"), "-I", str(ROOT / "components/audio_engine/include"),
                     "-I", str(ROOT / "components/device_settings/include")]
    objects = []
    for source in (SUITE / "device_owner_radio.c", ROOT / "firmware/main/bt_audio.c", ROOT / "firmware/main/bt_audio_btc.c"):
        output = temporary / (source.stem + ".o")
        flags = radio + (["-Desp_timer_get_time=fixture_bt_time"] if source.name == "bt_audio.c" else [])
        subprocess.run(flags + ["-c", str(source), "-o", str(output)], check=True)
        objects.append(str(output))
    binary = temporary / "test_device_owner"
    media = common + ["-I", str(ROOT / "tests/presenter/stubs")]
    for component in ("input_core", "audio_engine", "device_settings", "spoken_letters"):
        media += ["-I", str(ROOT / "components" / component / "include")]
    subprocess.run(media + [str(SUITE / "test_device_owner.c"), str(ROOT / "components/audio_engine/audio_media.c"),
                           *objects, "-lm", "-o", str(binary)], check=True)
    for case in ([args.case] if args.case else ["normal", "same-before-sdk", "other-before-submit", "other-before-sdk",
                                               "lock", "leave", "maintenance", "voice", "voice-existing", "voice-lock", "voice-second-lock", "refresh"]):
        subprocess.run([str(binary), case], check=True)
