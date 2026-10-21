#!/usr/bin/env python3
"""Run host checks; USB tests use pinned SDK headers but need no device."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import sys

root = Path(__file__).resolve().parents[1]
compiler = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
if not compiler:
    raise SystemExit("A C11 compiler is required (clang or cc).")
with tempfile.TemporaryDirectory(prefix="esp32-input-test-") as tmp:
    suites = {
        "input_core": ["input_core"],
        "hid_engine": ["input_core", "hid_engine"],
        "interaction": ["input_core", "interaction"],
        "device_settings": ["device_settings"],
        "haptics": ["haptics"],
    }
    for name, components in suites.items():
        binary = str(Path(tmp) / name)
        command = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
        for component in components:
            command += ["-I", str(root / "components" / component / "include")]
            command += [str(root / "components" / component / (component + ".c"))]
        command += [str(root / "tests" / ("test_" + name + ".c")), "-lm", "-o", binary]
        subprocess.run(command, check=True)
        subprocess.run([binary], check=True)
subprocess.run([sys.executable, str(root / "tests/test_hid_contract.py")], check=True)
subprocess.run([sys.executable, str(root / "tests/ble_hid/run.py")], check=True)
subprocess.run([sys.executable, str(root / "tests/usb_hid/run.py")], check=True)
subprocess.run([sys.executable, str(root / "tests/coordinator/run.py")], check=True)
subprocess.run([sys.executable, str(root / "tests/board_port/run.py")], check=True)
subprocess.run([sys.executable, str(root / "tests/settings_store/run.py")], check=True)
subprocess.run([sys.executable, str(root / "tests/wifi/run.py")], check=True)
subprocess.run([sys.executable, str(root / "tests/test_device_smoke.py")], check=True)
print("Host logic/protocol checks passed; radio and host interoperability require device testing.")
