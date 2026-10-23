#!/usr/bin/env python3
"""Test production touch motion, coordinator, remote leases and both HID adapters."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SUITE = Path(__file__).resolve().parent
CC = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
if not CC:
    raise SystemExit("A native C11 compiler is required.")
COMMON = [CC, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror",
          "-pedantic", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
COMPONENTS = ["hid_engine", "input_core", "interaction", "device_settings", "audio_engine",
              "spoken_letters", "control_protocol"]
INCLUDES = [arg for name in COMPONENTS for arg in ["-I", str(ROOT / "components" / name / "include")]]
INCLUDES += ["-I", str(ROOT / "components/control_protocol"), "-I", str(ROOT / "firmware/main")]
CORE = ["components/hid_engine/hid_engine.c", "components/input_core/input_core.c"]
CONTROL = ["components/control_protocol/" + n + ".c" for n in
           ["control_protocol", "control_json", "control_registry", "control_sha256"]]

def build_run(name, extra, sources, cases=None):
    binary = Path(TEMP) / name
    command = COMMON + INCLUDES + extra + [str(SUITE / ("test_" + name + ".c"))]
    command += [str(ROOT / p) for p in sources] + ["-lm", "-o", str(binary)]
    subprocess.run(command, check=True)
    for case in (cases or [None]):
        subprocess.run([str(binary)] + ([case] if case else []), check=True)

with tempfile.TemporaryDirectory(prefix="esp32-touch-pointer-") as TEMP:
    # No Git discovery or target build. Pointer checks do not use build identity.
    Path(TEMP, "app_build_identity.h").write_text("\n".join([
        '#define APP_BUILD_DEVELOPER "Native causal test"',
        '#define APP_BUILD_REPOSITORY_URL ""', '#define APP_BUILD_GIT_COMMIT ""',
        '#define APP_BUILD_GIT_STATE "unknown"', '#define APP_BUILD_GIT_KNOWN 0',
        '#define APP_BUILD_GIT_DIRTY 0', '#define APP_BUILD_GIT_STATE_KNOWN 0',
    ]) + "\n")
    build_run("engine", [], CORE)
    build_run("interaction", [], ["components/input_core/input_core.c", "components/interaction/interaction.c"])
    coordinator = CORE + CONTROL + ["firmware/main/" + name + ".c" for name in
        ["app_remote_input", "app_remote_device", "app_remote_media", "app_command_bridge"]]
    coordinator += ["components/audio_engine/audio_media.c", "components/interaction/interaction.c",
                    "components/interaction/interaction_multitouch.c", "components/device_settings/device_settings.c"]
    build_run("coordinator", ["-Wconversion", "-Wshadow", "-I", str(ROOT / "tests/coordinator/stubs"), "-I", TEMP],
              coordinator, ["baseline", "busy_move", "busy_begin", "large_delta", "fifo", "boundaries", "existing"])
    build_run("usb", ["-I", str(ROOT / "tests/usb_hid/stubs"), "-isystem",
              str(ROOT / "firmware/managed_components/espressif__tinyusb/src")], CORE)
    build_run("ble", ["-Wconversion", "-Wno-sign-conversion", "-I", str(ROOT / "tests/ble_hid/stubs"),
              "-DCONFIG_BT_SMP_MAX_BONDS=15"], CORE + ["firmware/main/ble_hid_store.c",
              "firmware/main/app_bluetooth_management.c", "components/device_settings/device_settings.c"])

    build_run("remote", [], CORE + CONTROL + ["firmware/main/app_remote_input.c", "components/interaction/interaction.c"])
