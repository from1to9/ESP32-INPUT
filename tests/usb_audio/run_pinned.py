#!/usr/bin/env python3
"""Compare original/patched TinyUSB DMA class with the real FIFO and USB adapter."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
from datetime import datetime, timezone

ROOT = Path(__file__).resolve().parents[2]
SUITE = Path(__file__).resolve().parent
SRC = ROOT / "firmware/managed_components/espressif__tinyusb/src"
output = ROOT / ".agent/local/usb-audio-out" / datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
output.mkdir(parents=True)
files = [ROOT / name for name in ("tools/tinyusb_audio_patch.py", "patches/tinyusb-audio-streams.json",
    "patches/tinyusb-audio-streams.patch", "firmware/tinyusb_audio_patch.cmake", "firmware/dependencies.lock",
    "firmware/main/usb_audio.c", "firmware/main/usb_audio.h", "firmware/main/usb_audio_transport.h",
    "patches/tinyusb-dwc2-observer.patch", "firmware/managed_components/espressif__tinyusb/src/portable/synopsys/dwc2/dcd_dwc2.c", "firmware/main/usb_audio_config.h",
    "firmware/main/usb_audio_descriptors.h", "tests/usb_audio/run_pinned.py", "tests/usb_audio/pinned_driver.c",
    "tests/usb_audio/test_pinned_driver.c", "tests/usb_audio/test_usb_audio.c")]
files += [p for base in (SUITE / "stubs", ROOT / "tests/usb_hid/stubs", ROOT / "components/audio_engine/include")
          for p in base.rglob("*") if p.is_file()]
files += [SRC / "class/audio/audio_device.c", SRC / "common/tusb_fifo.c"]
files += list(SRC.rglob("*.h"))

def hashes():
    return {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(set(files))}

inputs_before_generation = hashes()
spec = importlib.util.spec_from_file_location("audio_patch", ROOT / "tools/tinyusb_audio_patch.py")
patcher = importlib.util.module_from_spec(spec); spec.loader.exec_module(patcher)
copy = output / "audio_device.c"
patcher.generate(SRC.parent, copy)
# The prior independently reviewed class isolates the finite-tail defect from
# OUT rearm and IN overwrite fixes; its exact hash is retained as provenance.
pre_tail = output / "before-short-read.c"
prior = copy.read_text().replace(
    "n_bytes_tx = tu_fifo_read_n(&audio->ep_in_ff, audio->lin_buf_in, n_bytes_tx);",
    "tu_fifo_read_n(&audio->ep_in_ff, audio->lin_buf_in, n_bytes_tx);")
prior = prior.replace('#include "usb_audio_transport.h"\n', "")
prior = prior.replace("  bool queued = usbd_edpt_xfer(rhport, audio->ep_out, audio->lin_buf_out, audio->ep_out_sz, true);\n"
    "  input_usb_audio_class_out(true, queued, XFER_RESULT_SUCCESS, n_bytes_received);\n  TU_VERIFY(queued);",
    "  TU_VERIFY(usbd_edpt_xfer(rhport, audio->ep_out, audio->lin_buf_out, audio->ep_out_sz, true));")
prior = prior.replace("      input_usb_audio_class_out(false, false, (uint8_t) result, (uint16_t) xferred_bytes);\n", "")
assert hashlib.sha256(prior.encode()).hexdigest() == "ff8896ec6d3fc9ba55c43a7a66e248bdd99d752854e7a043873e1dc33b6bf2f1"
pre_tail.write_text(prior)
assert inputs_before_generation == hashes(), "Generation input drift"
reviewed = json.loads((ROOT / "patches/tinyusb-audio-streams.json").read_text())
generated_files = [copy, copy.with_name("dcd_dwc2.c"), pre_tail]
assert hashlib.sha256(generated_files[0].read_bytes()).hexdigest() == reviewed["output_sha256"]
assert hashlib.sha256(generated_files[1].read_bytes()).hexdigest() == reviewed["dcd"]["output_sha256"]
files += generated_files
before = hashes()
compiler = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
flags = [compiler, "-std=c11", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
         "-ffunction-sections", "-fdata-sections"]
for option, path in [("-I", SUITE / "stubs"), ("-I", ROOT / "tests/usb_hid/stubs"), ("-isystem", SRC),
                     ("-I", ROOT / "components/audio_engine/include"), ("-I", ROOT / "firmware/main"),
                     ("-I", SRC / "class/audio")]:
    flags += [option, str(path)]
environment = dict(os.environ, UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
if sys.platform == "darwin":
    environment["ASAN_OPTIONS"] = "detect_leaks=0"
outcomes = {}
failures = []
for label, source in [("original", SRC / "class/audio/audio_device.c"), ("before-short-read", pre_tail), ("patched", copy)]:
    objects = []
    with (output / (label + "-build.log")).open("w") as log:
        for i, unit in enumerate([SUITE / "test_pinned_driver.c", SUITE / "pinned_driver.c", SRC / "common/tusb_fifo.c"]):
            obj = output / (label + "-" + str(i) + ".o")
            diagnostics = ["-Wall", "-Wextra", "-Werror", "-pedantic", "-Wconversion", "-Wshadow"] if i == 0 else ["-Wall", "-Wextra"]
            subprocess.run(flags + diagnostics + ['-DUSB_AUDIO_DRIVER_SOURCE="' + str(source) + '"',
                "-c", str(unit), "-o", str(obj)], stdout=log, stderr=subprocess.STDOUT, check=True)
            objects.append(str(obj))
        binary = output / label
        linker = "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections"
        subprocess.run(flags + objects + [linker, "-o", str(binary)], stdout=log, stderr=subprocess.STDOUT, check=True)
    cases = ("out", "capture_fs", "capture_hs") if label == "original" else ("single_fs", "single_hs")
    if label == "patched":
        cases = ("out", "capture_fs", "capture_hs", "single_fs", "single_hs", "duplex_fs", "duplex_hs")
    for case in cases:
        run = subprocess.run([str(binary), case], capture_output=True, text=True, env=environment, timeout=20)
        (output / (label + "-" + case + ".log")).write_text(run.stdout + run.stderr)
        outcomes[label + "-" + case] = run.returncode
        if label != "patched":
            expected = ("actual IN scheduled 96 bytes after only 94 bytes remained" if label == "before-short-read" else
                        "consumed ZLP without rearming; FIFO=0 rearms=1" if case == "out" else
                        "actual IN FIFO accepted 1920 bytes without completions; retained=392")
            if run.returncode in (-6, 134) and expected in run.stderr:
                print("PASS:", label, "class reproduces", case, "failure")
            else:
                failures.append(label + "-" + case + ": expected defect was not reproduced")
        else:
            print(run.stdout + run.stderr, end="")
            if run.returncode != 0:
                failures.append(label + "-" + case + ": regression failed")

after = hashes()
manifest = {"inputs_before_generation": inputs_before_generation, "source_before": before, "source_after": after, "source_stable": before == after,
            "outcomes": outcomes, "generated_source_sha256": hashlib.sha256(copy.read_bytes()).hexdigest(),
            "generated_dcd_sha256": hashlib.sha256(copy.with_name("dcd_dwc2.c").read_bytes()).hexdigest(),
            "before_short_read_sha256": hashlib.sha256(pre_tail.read_bytes()).hexdigest(),
            "hardware_access": False, "physical_zlp_observed": False, "failures": failures}
(output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
print("Evidence:", output)
assert before == after and not failures, failures
