#!/usr/bin/env python3
"""Check bounded touch observation and identical production reports on/off."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SUITE = Path(__file__).resolve().parent
CC = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
if not CC:
    raise SystemExit("A native C11 compiler with sanitizers is required.")
COMMON = [CC, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror", "-pedantic",
          "-Wconversion", "-Wshadow", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
COMPONENTS = ["hid_engine", "input_core", "interaction", "device_settings", "audio_engine", "spoken_letters", "control_protocol"]
INCLUDES = [arg for name in COMPONENTS for arg in ["-I", str(ROOT / "components" / name / "include")]]
INCLUDES += ["-I", str(ROOT / "components/control_protocol"), "-I", str(ROOT / "firmware/main")]
CORE = ["components/hid_engine/hid_engine.c", "components/input_core/input_core.c"]
CONTROL = ["components/control_protocol/" + name + ".c" for name in
           ["control_protocol", "control_json", "control_registry", "control_sha256"]]
OBSERVER = str(ROOT / "firmware/main/app_touch_trace.c")

with tempfile.TemporaryDirectory(prefix="esp32-touch-trace-") as temporary:
    tmp = Path(temporary)
    (tmp / "app_build_identity.h").write_text("\n".join([
        '#define APP_BUILD_DEVELOPER "Native observation test"', '#define APP_BUILD_REPOSITORY_URL ""',
        '#define APP_BUILD_GIT_COMMIT ""', '#define APP_BUILD_GIT_STATE "unknown"',
        '#define APP_BUILD_GIT_KNOWN 0', '#define APP_BUILD_GIT_DIRTY 0', '#define APP_BUILD_GIT_STATE_KNOWN 0',
    ]) + "\n")

    def build(name, sources, stubs, enabled, extra=()):
        binary = tmp / (name + ("-on" if enabled else "-off"))
        command = COMMON + INCLUDES + ["-I", str(stubs), "-I", str(tmp)] + list(extra)
        if enabled:
            command += ["-DCONFIG_APP_TOUCH_LATENCY_DIAGNOSTICS=1", OBSERVER]
        command += [str(SUITE / ("test_" + name + ".c"))] + [str(ROOT / p) for p in sources]
        subprocess.run(command + ["-lm", "-o", str(binary)], check=True)
        return binary

    observer = build("observer", [], ROOT / "tests/coordinator/stubs", True)
    for case in ["flow", "link", "failure"]:
        subprocess.run([str(observer), case], check=True)
    coordinator = CORE + CONTROL + ["firmware/main/" + name + ".c" for name in
        ["app_remote_input", "app_remote_device", "app_remote_media", "app_command_bridge"]]
    coordinator += ["components/audio_engine/audio_media.c", "components/interaction/interaction.c",
                    "components/interaction/interaction_multitouch.c", "components/device_settings/device_settings.c"]
    ble = CORE + ["firmware/main/ble_hid_store.c", "firmware/main/app_bluetooth_management.c",
                  "components/device_settings/device_settings.c"]
    for name, sources, stubs, flags in [
        ("coordinator", coordinator, ROOT / "tests/coordinator/stubs", []),
        ("ble", ble, ROOT / "tests/ble_hid/stubs", ["-Wno-sign-conversion", "-DCONFIG_BT_SMP_MAX_BONDS=15"]),
    ]:
        results = []
        clock_counts = []
        for enabled in [False, True]:
            binary = build(name, sources, stubs, enabled, flags)
            run = subprocess.run([str(binary)], capture_output=True, text=True, check=True)
            results.append(run.stdout)
            if name == "ble":
                clock_counts.append(int(run.stderr.split("TIMESTAMP_READS ")[1].split()[0]))
            if not enabled:
                symbols = subprocess.check_output(["nm", str(binary)], text=True)
                assert "app_touch_trace_" not in symbols, "Disabled image retains observer functions"
        assert results[0] == results[1], name + " report output differs with observation enabled"
        if name == "ble":
            assert clock_counts[1] - clock_counts[0] == 59
            print(f"PASS BLE timestamp calls off/on={clock_counts}: 20 begins/returns + 19 completions add 59")
        print(f"PASS {name}: identical on/off report output ({len(results[0].splitlines())} lines)")
