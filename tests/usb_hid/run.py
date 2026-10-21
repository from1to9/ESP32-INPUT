#!/usr/bin/env python3
"""Test the real composite manager/HID adapter against pinned TinyUSB headers."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import re

root = Path(__file__).resolve().parents[2]
suite = Path(__file__).resolve().parent
compiler = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
if not compiler:
    raise SystemExit("A C11 compiler with sanitizers is required.")
headers = root / "firmware/managed_components/espressif__tinyusb/src"
if not headers.exists():
    raise SystemExit("Resolve the pinned firmware components before running USB adapter tests.")
with tempfile.TemporaryDirectory(prefix="esp32-usb-hid-test-") as temp:
    binary = str(Path(temp) / "usb_hid")
    subprocess.run([
        compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-g",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        "-I", str(suite / "stubs"), "-isystem", str(headers),
        "-I", str(root / "firmware/main"),
        "-I", str(root / "components/hid_engine/include"),
        "-I", str(root / "components/input_core/include"),
        str(suite / "test_usb_hid.c"),
        str(root / "components/hid_engine/hid_engine.c"),
        str(root / "components/input_core/input_core.c"), "-lm", "-o", binary,
    ], check=True)
    subprocess.run([binary], check=True)
    identity = str(Path(temp) / "usb_identity")
    subprocess.run([
        compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-g",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        "-I", str(suite / "stubs"), "-isystem", str(headers),
        "-isystem", str(root / "firmware/managed_components/espressif__esp_tinyusb/include_private"),
        str(suite / "test_sdk_identity.c"), "-o", identity,
    ], check=True)
    subprocess.run([identity], check=True)
defaults = (root / "firmware/sdkconfig.defaults").read_text()
if not re.search(r'^CONFIG_TINYUSB_DESC_SERIAL_STRING=""$', defaults, re.MULTILINE):
    raise SystemExit("Tracked USB defaults must select the SDK's chip-derived serial.")
