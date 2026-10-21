#!/usr/bin/env python3
"""Test the production settings store with controlled NVS/task/clock boundaries."""
import argparse
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SUITE = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case", help="run one named case for diagnosis")
    args = parser.parse_args()
    compiler = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
    if not compiler:
        raise SystemExit("A C11 compiler with address/undefined sanitizers is required.")
    with tempfile.TemporaryDirectory(prefix="esp32-settings-test-") as temporary:
        binary = Path(temporary) / "test_settings_store"
        command = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-Wconversion", "-Wshadow", "-g", "-fsanitize=address,undefined",
                   "-fno-omit-frame-pointer", "-I", str(SUITE / "stubs"),
                   "-I", str(ROOT / "components/device_settings/include"),
                   str(SUITE / "test_settings_store.c"),
                   str(ROOT / "components/device_settings/device_settings.c"),
                   "-lm", "-o", str(binary)]
        subprocess.run(command, check=True)
        subprocess.run([str(binary)] + ([args.case] if args.case else []), check=True)


if __name__ == "__main__":
    main()
