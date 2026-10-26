#!/usr/bin/env python3
"""Exercise the production board adapter and pinned ES8311 driver on the host."""
from pathlib import Path
import argparse
import os
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--clock-only", action="store_true", help="Run the shared-clock regression only")
args = parser.parse_args()

os.environ.setdefault("UBSAN_OPTIONS", "halt_on_error=1:print_stacktrace=1")
root = Path(__file__).resolve().parents[2]
suite = Path(__file__).resolve().parent
codec = root / "firmware/managed_components/espressif__esp_codec_dev"
compiler = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
if not compiler or not codec.exists():
    raise SystemExit("Resolve pinned firmware dependencies and install a C11 compiler first.")
with tempfile.TemporaryDirectory(prefix="esp32-board-audio-") as temp:
    objects = []
    includes = [suite / "stubs", root / "firmware/main", root / "components/audio_engine/include"]
    vendor_includes = [codec / path for path in ["include", "interface", "device/include", "device/priv_include"]]
    common = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
              "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    for path in includes:
        common += ["-I", str(path)]
    for path in vendor_includes:
        common += ["-isystem", str(path)]
    sources = [suite / "test_board_audio.c", codec / "device/es8311/es8311.c",
               codec / "esp_codec_dev_vol.c", codec / "esp_codec_dev_if.c"]
    for index, source in enumerate(sources):
        output = Path(temp) / f"{index}.o"
        # The pinned vendor coefficient loop compares int with sizeof(size_t).
        # Preserve the vendor source; first-party code retains strict errors.
        flags = ["-pedantic"] if index == 0 else ["-include", "stdlib.h", "-Wno-error=sign-compare"]
        subprocess.run(common + flags + ["-c", str(source), "-o", str(output)], check=True)
        objects.append(str(output))
    binary = str(Path(temp) / "board_audio")
    subprocess.run(common + objects + ["-lm", "-o", binary], check=True)
    clock_binary = str(Path(temp) / "audio_clock")
    subprocess.run(common + ["-pedantic", str(suite / "test_audio_clock.c")] + objects[1:] + ["-lm", "-o", clock_binary], check=True)
    subprocess.run([clock_binary], check=True)
    if args.clock_only:
        raise SystemExit(0)
    subprocess.run([binary], check=True)
    level_binary = str(Path(temp) / "capture_level")
    subprocess.run(common + ["-pedantic", str(suite / "test_capture_level.c")] + objects[1:] + ["-lm", "-o", level_binary], check=True)
    for scenario in ("amplitude", "silence", "extrema", "failure", "safety", "clock"):
        subprocess.run([level_binary, scenario], check=True)
    budget_binary = str(Path(temp) / "codec_control_budget")
    subprocess.run(common + ["-pedantic", str(suite / "test_codec_control_budget.c")] + objects[1:] + ["-lm", "-o", budget_binary], check=True)
    for scenario in ("shared_write", "shared_read", "exhausted", "transfer_failure", "stop", "mute"):
        subprocess.run([budget_binary, scenario], check=True)
    nand_binary = str(Path(temp) / "nand_probe")
    subprocess.run(common + ["-pedantic", str(suite / "test_nand_probe.c"), "-o", nand_binary], check=True)
    subprocess.run([nand_binary], check=True)
    flash_binary = str(Path(temp) / "recording_flash")
    subprocess.run(common + ["-pedantic", str(suite / "test_recording_flash.c"), "-o", flash_binary], check=True)
    subprocess.run([flash_binary], check=True)
    engine_objects = []
    for index, source in enumerate([suite / "test_board_engine.c"] + [
        root / "components/audio_engine" / name
        for name in ("audio_engine.c", "audio_recording.c", "audio_media.c")
    ]):
        output = Path(temp) / f"engine-{index}.o"
        subprocess.run(common + ["-pedantic", "-c", str(source), "-o", str(output)], check=True)
        engine_objects.append(str(output))
    engine_binary = str(Path(temp) / "board_engine")
    subprocess.run(common + engine_objects + objects[1:] + ["-lm", "-o", engine_binary], check=True)
    subprocess.run([engine_binary], check=True)
    probe_binary = str(Path(temp) / "probe_interleaving")
    subprocess.run(common + ["-pedantic", str(suite / "test_probe_interleaving.c")] + engine_objects[1:] + objects[1:] + ["-lm", "-o", probe_binary], check=True)
    for scenario in ("constructor", "configure", "enable", "initial_mute", "enable_blocked", "enable_failure",
                     "mute_constructor", "mute_configure", "mute_enable"):
        subprocess.run([probe_binary, scenario], check=True)
    startup_object = str(Path(temp) / "startup.o")
    subprocess.run(common + ["-pedantic", "-c", str(suite / "test_audio_startup.c"), "-o", startup_object], check=True)
    startup_binary = str(Path(temp) / "audio_startup")
    subprocess.run(common + [startup_object] + engine_objects[1:] + objects[1:] + ["-lm", "-o", startup_binary], check=True)
    orderings = ("probe_first", "stop_first", "probe_failure", "control_failure", "dma_tx_failure", "dma_rx_failure")
    for ordering in orderings:
        subprocess.run([startup_binary, ordering], check=True)
    # The expected placement is explicit for each SDK capability/configuration.
    # Exercise the same production adapter and actual engine/codec under each.
    for name, dma_capable, psram, iram_safe, i2c_iram_safe, allow_stack, expected_dma, expected_stack in (
        ("psram_dma", 1, 1, 0, 0, 1, 1, 1),
        ("no_psram", 1, 0, 0, 0, 1, 0, 0),
        ("no_psram_dma", 0, 1, 0, 0, 1, 0, 1),
        ("iram_safe", 1, 1, 1, 0, 1, 0, 0),
        ("i2c_iram_safe", 1, 1, 0, 1, 1, 1, 0),
        ("external_stack_disabled", 1, 1, 0, 0, 0, 1, 0),
    ):
        flags = [f"-DSOC_PSRAM_DMA_CAPABLE={dma_capable}", f"-DCONFIG_SPIRAM={psram}",
                 f"-DCONFIG_I2S_ISR_IRAM_SAFE={iram_safe}", f"-DTEST_EXPECT_PSRAM_DMA={expected_dma}",
                 f"-DCONFIG_I2C_ISR_IRAM_SAFE={i2c_iram_safe}",
                 f"-DCONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM={allow_stack}",
                 f"-DTEST_EXPECT_EXTERNAL_STACK={expected_stack}"]
        for source_name, support in (("test_board_audio.c", objects[1:]),
                                     ("test_audio_startup.c", engine_objects[1:] + objects[1:])):
            variant = str(Path(temp) / f"{name}-{source_name}")
            subprocess.run(common + flags + ["-pedantic", str(suite / source_name)] + support + ["-lm", "-o", variant], check=True)
            for args in ([[]] if source_name == "test_board_audio.c" else [[value] for value in orderings]):
                subprocess.run([variant] + args, check=True)
