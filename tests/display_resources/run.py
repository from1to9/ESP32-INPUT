#!/usr/bin/env python3
"""Test board policy and the pinned SDK LCD transport at the SPI boundary."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
suite = Path(__file__).resolve().parent
sdk = Path(os.environ.get("IDF_PATH", root / ".tools/esp-idf"))
transport = sdk / "components/esp_lcd/spi/esp_lcd_panel_io_spi.c"
if not transport.is_file():
    raise SystemExit("Install the pinned ESP-IDF SDK before running display resource tests.")
compiler = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
if not compiler:
    raise SystemExit("A strict C11 compiler with sanitizers is required.")
flags = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-g",
         "-Wno-gnu-empty-struct",  # The pinned SDK public event type is empty.
         "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
         "-include", str(suite / "stubs/compat.h")]
for directory in (suite / "stubs", root / "tests/board_audio/stubs",
                  sdk / "components/esp_lcd/include", sdk / "components/esp_lcd/interface",
                  sdk / "components/esp_lcd/priv_include",
                  root / "firmware/main"):
    flags += ["-I", str(directory)]
with tempfile.TemporaryDirectory(prefix="esp32-display-resources-") as temp:
    for depth in (1, 2):
        build = Path(temp) / str(depth)
        build.mkdir()
        configured = flags + [f"-DCONFIG_APP_DISPLAY_SPI_QUEUE_DEPTH={depth}"]
        binary = str(build / "factory")
        policy = str(root / "firmware/main/board_display_resources.c")
        subprocess.run(configured + [policy, str(suite / "test_factory.c"), "-o", binary], check=True)
        subprocess.run([binary], check=True)
        sdk_object = str(build / "sdk_transport.o")
        subprocess.run(configured + ["-Desp_lcd_new_panel_io_spi=__real_esp_lcd_new_panel_io_spi",
                                    "-c", str(transport), "-o", sdk_object], check=True)
        binary = str(build / "sdk_transport")
        subprocess.run(configured + [policy, str(suite / "test_sdk_transport.c"), sdk_object,
                                     "-o", binary], check=True)
        subprocess.run([binary], check=True)
