#!/usr/bin/env python3
"""Exercise the production board adapter and pinned ES8311 driver on the host."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

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
    subprocess.run([binary], check=True)
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
    startup_object = str(Path(temp) / "startup.o")
    subprocess.run(common + ["-pedantic", "-c", str(suite / "test_audio_startup.c"), "-o", startup_object], check=True)
    startup_binary = str(Path(temp) / "audio_startup")
    subprocess.run(common + [startup_object] + engine_objects[1:] + objects[1:] + ["-lm", "-o", startup_binary], check=True)
    for ordering in ("probe_first", "stop_first", "probe_failure", "control_failure"):
        subprocess.run([startup_binary, ordering], check=True)
