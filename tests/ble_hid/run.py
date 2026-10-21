#!/usr/bin/env python3
"""Test the real BLE adapter through a deterministic hardware API boundary."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SUITE = Path(__file__).resolve().parent


def main():
    compiler = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
    if not compiler:
        raise SystemExit("A C11 compiler with address/undefined sanitizers is required.")
    with tempfile.TemporaryDirectory(prefix="esp32-ble-test-") as temporary:
        binary = Path(temporary) / "test_ble_hid"
        command = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-Wconversion", "-Wno-sign-conversion", "-g",
                   "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                   "-I", str(SUITE / "stubs"),
                   "-I", str(ROOT / "components/hid_engine/include"),
                   "-I", str(ROOT / "components/input_core/include"),
                   str(SUITE / "test_ble_hid.c"),
                   str(ROOT / "components/hid_engine/hid_engine.c"),
                   str(ROOT / "components/input_core/input_core.c"),
                   "-o", str(binary)]
        for capacity in (15, 32):
            subprocess.run(command + [f"-DCONFIG_BT_SMP_MAX_BONDS={capacity}"], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
