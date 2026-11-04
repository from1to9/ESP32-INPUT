#!/usr/bin/env python3
"""Replay coordinator revocation before the next media poll against real Classic dispatch."""
from pathlib import Path
import argparse
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--case")
args = parser.parse_args()
compiler = shutil.which("clang") or "cc"
with tempfile.TemporaryDirectory(prefix="esp32-device-revocation-") as directory:
    temporary = Path(directory)
    common = [compiler, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror", "-pedantic",
              "-Wconversion", "-Wno-sign-conversion", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    radio = common + ["-DTEST_BT_AUDIO_LOCKS", "-I", str(ROOT / "tests/bt_audio/stubs"),
                      "-I", str(ROOT / "firmware/main"), "-I", str(ROOT / "components/audio_engine/include"),
                      "-I", str(ROOT / "components/device_settings/include")]
    for name in ("heap_caps_calloc_prefer", "ble_hid_pairing_close", "esp_fill_random", "esp_err_to_name"):
        radio += ["-D" + name + "=radio_" + name]
    objects = []
    for source in (ROOT / "tests/media_controls/device_owner_radio.c", ROOT / "firmware/main/bt_audio.c", ROOT / "firmware/main/bt_audio_btc.c"):
        output = temporary / (source.stem + ".o")
        flags = radio + (["-Desp_timer_get_time=fixture_bt_time"] if source.name == "bt_audio.c" else [])
        subprocess.run(flags + ["-c", str(source), "-o", str(output)], check=True)
        objects.append(str(output))
    subprocess.run(["python3", str(ROOT / "tools/build_identity.py"), "--root", str(ROOT),
                    "--output", str(temporary / "app_build_identity.h")], check=True)
    command = common + ["-I", str(ROOT / "tests/coordinator/stubs"), "-I", str(temporary)]
    for component in ("hid_engine", "input_core", "interaction", "device_settings", "audio_engine", "spoken_letters", "control_protocol"):
        command += ["-I", str(ROOT / "components" / component / "include")]
    command += ["-I", str(ROOT / "components/control_protocol")]
    sources = ["tests/coordinator/test_device_revocation.c", "firmware/main/app_media_controls.c",
               "firmware/main/app_remote_input.c", "firmware/main/app_remote_device.c", "firmware/main/app_remote_media.c",
               "components/audio_engine/audio_media.c", "firmware/main/app_command_bridge.c",
               *["components/control_protocol/" + name for name in ("control_protocol.c", "control_json.c", "control_registry.c", "control_sha256.c")],
               "components/hid_engine/hid_engine.c", "components/input_core/input_core.c", "components/interaction/interaction.c",
               "components/interaction/interaction_multitouch.c", "components/device_settings/device_settings.c"]
    binary = temporary / "test_device_revocation"
    subprocess.run(command + [str(ROOT / source) for source in sources] + objects + ["-lm", "-o", str(binary)], check=True)
    cases = ["normal", "normal-after-sync", "lock", "leave", "back", "pause", "device-pause", "lock-after-sync", "leave-after-sync", "pause-after-sync",
             "voice-normal", "voice-lock", "voice-leave", "voice-pause", "replacement-owner", "dispatch-first", "revoke-first",
             "expiry", "expiry-after-sync", "expiry-normal", "expiry-dispatch-first", "voice-expiry", "voice-expiry-normal"]
    for case in [args.case] if args.case else cases:
        subprocess.run([str(binary), case], check=True)
