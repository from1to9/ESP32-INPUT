#!/usr/bin/env python3
"""Exercise production coordinator safety paths without SDK or device access."""
from pathlib import Path
import argparse
import os
import shutil
import subprocess
import tempfile
import sys

ROOT = Path(__file__).resolve().parents[2]
SUITE = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case", help="run one named coordinator case for diagnosis")
    parser.add_argument("--capture", action="store_true", help="also exercise optional raw IMU observer")
    parser.add_argument("--mouse-phase", action="store_true", help="enable the optional coordinator phase observer")
    args = parser.parse_args()
    compiler = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
    if not compiler:
        raise SystemExit("A C11 compiler with address/undefined sanitizers is required.")
    with tempfile.TemporaryDirectory(prefix="esp32-coordinator-test-") as temporary:
        subprocess.run([sys.executable, str(ROOT / "tools/build_identity.py"),
                        "--root", str(ROOT), "--output",
                        str(Path(temporary) / "app_build_identity.h")], check=True)
        binary = Path(temporary) / "test_coordinator"
        command = [compiler, "-std=c11", "-D_POSIX_C_SOURCE=200809L",
                   "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-Wconversion", "-Wshadow", "-g",
                   "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                   "-I", str(SUITE / "stubs"),
                   "-I", temporary,
                   "-I", str(ROOT / "components/hid_engine/include"),
                   "-I", str(ROOT / "components/input_core/include"),
                   "-I", str(ROOT / "components/interaction/include"),
                   "-I", str(ROOT / "components/device_settings/include"),
                   "-I", str(ROOT / "components/audio_engine/include"),
                   "-I", str(ROOT / "components/spoken_letters/include"),
                   "-I", str(ROOT / "components/control_protocol/include"),
                   "-I", str(ROOT / "components/control_protocol"),
                   str(SUITE / "test_coordinator.c"),
                   str(ROOT / "firmware/main/app_remote_input.c"),
                   str(ROOT / "firmware/main/app_remote_device.c"),
                   str(ROOT / "firmware/main/app_remote_media.c"),
                   str(ROOT / "components/audio_engine/audio_media.c"),
                   str(ROOT / "firmware/main/app_command_bridge.c"),
                   *[str(ROOT / "components/control_protocol" / name) for name in
                     ("control_protocol.c", "control_json.c", "control_registry.c", "control_sha256.c")],
                   str(ROOT / "components/hid_engine/hid_engine.c"),
                   str(ROOT / "components/input_core/input_core.c"),
                   str(ROOT / "components/interaction/interaction.c"),
        str(ROOT / "components/interaction/interaction_multitouch.c"),
                   str(ROOT / "components/device_settings/device_settings.c"),
                   "-lm", "-o", str(binary)]
        if args.capture:
            command += ["-DCONFIG_APP_IMU_CAPTURE=1",
                        str(ROOT / "firmware/main/motion_capture.c"),
                        str(ROOT / "firmware/main/board_imu_queue.c")]
        if args.mouse_phase:
            command += ["-DCONFIG_APP_MOUSE_PHASE_DIAGNOSTICS=1",
                        str(ROOT / "firmware/main/app_mouse_phase.c")]
        if args.case != "media_review":
            subprocess.run(command, check=True)
            subprocess.run([str(binary)] + ([args.case] if args.case else []), check=True)
        if args.case in (None, "media_review"):
            command[command.index(str(SUITE / "test_coordinator.c"))] = str(SUITE / "test_media_review.c")
            subprocess.run(command + [str(ROOT / "firmware/main/app_media_controls.c")], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
