#!/usr/bin/env python3
"""Execute the pinned DCD against explicit MMIO contracts, not physical hardware."""
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
SUITE = Path(__file__).resolve().parent
SRC = ROOT / "firmware/managed_components/espressif__tinyusb/src"
output = ROOT / ".agent/local/usb-dwc2" / datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
output.mkdir(parents=True)
files = list(SRC.rglob("*.h")) + [SRC / "portable/synopsys/dwc2/dcd_dwc2.c", SRC / "common/tusb_fifo.c", SRC / "class/audio/audio_device.c"]
files += [p for base in (SUITE, ROOT / "tests/usb_hid/stubs", ROOT / "components/audio_engine/include")
          for p in base.rglob("*") if p.is_file() and p.suffix in (".c", ".h", ".py")]
files += [ROOT / "firmware/main/usb_audio.c", ROOT / "firmware/main/usb_audio.h",
          ROOT / "firmware/main/usb_audio_config.h", ROOT / "firmware/main/usb_audio_descriptors.h", ROOT / "firmware/main/usb_audio_transport.h",
          ROOT / "tools/tinyusb_audio_patch.py", ROOT / "firmware/dependencies.lock",
          ROOT / "patches/tinyusb-audio-streams.json", ROOT / "patches/tinyusb-audio-streams.patch",
          ROOT / "patches/tinyusb-dwc2-observer.patch", ROOT / "firmware/tinyusb_audio_patch.cmake",
          ROOT / ".tools/esp-idf/components/soc/esp32s31/register/soc/usb_otghs_struct.h"]

def hashes():
    return {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(set(files))}

inputs_before_generation = hashes()
spec = importlib.util.spec_from_file_location("audio_patch", ROOT / "tools/tinyusb_audio_patch.py")
patcher = importlib.util.module_from_spec(spec); spec.loader.exec_module(patcher)
audio = output / "audio_device.c"; patcher.generate(SRC.parent, audio)
assert inputs_before_generation == hashes(), "Generation input drift"
reviewed = json.loads((ROOT / "patches/tinyusb-audio-streams.json").read_text())
generated_files = [audio, output / "dcd_dwc2.c"]
assert hashlib.sha256(generated_files[0].read_bytes()).hexdigest() == reviewed["output_sha256"]
assert hashlib.sha256(generated_files[1].read_bytes()).hexdigest() == reviewed["dcd"]["output_sha256"]
files += generated_files
before = hashes()
compiler = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
common = [compiler, "-std=c11", "-g", "-U__ARM_FEATURE_UNALIGNED", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
          "-ffunction-sections", "-fdata-sections",
          '-DUSB_AUDIO_CONFIG_SOURCE="' + str(ROOT / "firmware/main/usb_audio_config.h") + '"',
          '-DUSB_AUDIO_DCD_SOURCE="' + str(output / "dcd_dwc2.c") + '"',
          '-DUSB_AUDIO_DRIVER_SOURCE="' + str(audio) + '"']
for inc in (SUITE / "dwc2_stubs", SUITE / "stubs", ROOT / "tests/usb_hid/stubs", SUITE,
            ROOT / "firmware/main", ROOT / "components/audio_engine/include"):
    common += ["-I", str(inc)]
common += ["-isystem", str(SRC), "-isystem", str(SRC / "class/audio"),
           "-isystem", str(SRC / "portable/synopsys/dwc2")]
environment = dict(os.environ, UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
if sys.platform == "darwin":
    environment["ASAN_OPTIONS"] = "detect_leaks=0"
outcomes = {}
commands = []
log_hashes = {}
for pti in (0, 1):
    for case in ("register", "audio"):
        name = case + "-pti" + str(pti)
        units = [SUITE / "pinned_dcd.c", SUITE / ("test_dwc2.c" if case == "register" else "test_dwc2_audio.c")]
        if case == "audio":
            units += [SUITE / "pinned_dcd_audio.c", SRC / "common/tusb_fifo.c"]
        flags = common + ["-DCFG_TUD_DWC2_PTI_ENABLE=" + str(pti)]
        if pti:
            flags += ["-DCONFIG_IDF_TARGET_ESP32S31=1", "-DCONFIG_TINYUSB_MODE_DMA=1"]
        objects = []
        with (output / (name + "-build.log")).open("w") as log:
            for i, unit in enumerate(units):
                obj = output / (name + "-" + str(i) + ".o")
                warning = ["-Wall", "-Wextra", "-Werror", "-pedantic", "-Wconversion", "-Wshadow", "-Wno-enum-enum-conversion"] if i == 1 else ["-Wall", "-Wextra"]
                command = flags + warning + ["-c", str(unit), "-o", str(obj)]
                commands.append(command)
                subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
                objects.append(str(obj))
            binary = output / name
            link = "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections"
            command = flags + objects + [link, "-o", str(binary)]
            commands.append(command)
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
        scenarios = [(name, [])] if case == "register" else [
            ("audio-fs-p0-d0-pti" + str(pti), ["fs", "0", "0"])] + [
            ("audio-hs-p" + str(phase) + "-d" + str(delay) + "-pti" + str(pti),
             ["hs", str(phase), str(delay)])
            for phase in list(range(8)) + list(range(0x3ff0, 0x3ff8)) for delay in (0, 1, 3)]
        for scenario, arguments in scenarios:
            result = subprocess.run([str(binary)] + arguments, capture_output=True, text=True, env=environment, timeout=20)
            (output / (scenario + ".log")).write_text(result.stdout + result.stderr)
            outcomes[scenario] = result.returncode
            log_hashes[scenario] = hashlib.sha256((output / (scenario + ".log")).read_bytes()).hexdigest()
            if result.returncode:
                print(scenario, "exit", result.returncode)
            else:
                print(scenario, "PASS")

expected_failures = [name for name in outcomes if name == "register-pti0" or
    (name.startswith("audio-hs-") and "-d0-" in name and name.endswith("pti0"))]
after = hashes()
(output / "manifest.json").write_text(json.dumps({"source_before": before, "source_after": after,
    "source_stable": before == after, "inputs_before_generation": inputs_before_generation,
    "commands": commands, "scenario_log_sha256": log_hashes, "outcomes": outcomes, "hardware_access": False,
    "generated_audio_sha256": hashlib.sha256(audio.read_bytes()).hexdigest(),
    "generated_dcd_sha256": hashlib.sha256((output / "dcd_dwc2.c").read_bytes()).hexdigest(),
    "boundary": "USBD submission/control response boundary substituted; DCD, class open/SET_INTERFACE parsing and FIFO run real. PHY/reset/cache/MMIO/IRQ effects are modeled; no full host or USBD core emulation.",
    "compiler": compiler, "common_flags": common,
    "expected_baseline_failures": expected_failures,
    "positive_oracle": "Every scenario requires32 OUT completions/3072 bytes, three exact960-byte varied PCM frames, unchanged1ms descriptors; baseline failures retained explicitly.",
    "scope": "Actual DCD/class/FIFO code with modeled register/IRQ behavior; not physical simulation or USB host qualification."}, indent=2) + "\n")
print("Evidence:", output)
assert before == after
for name, code in outcomes.items():
    log = (output / (name + ".log")).read_text()
    baseline_failure = name == "register-pti0" or (name.startswith("audio-hs-") and "-d0-" in name and name.endswith("pti0"))
    if baseline_failure:
        expected = "actual DCD periodic OUT progress 1/32" if name.startswith("register") else "actual DCD/class OUT stops at 96 bytes"
        assert code in (-6, 134) and expected in log, name
    else:
        assert code == 0, name
