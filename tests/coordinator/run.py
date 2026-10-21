#!/usr/bin/env python3
"""Exercise production coordinator safety paths without SDK or device access."""
from pathlib import Path
import argparse
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SUITE = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case", help="run one named coordinator case for diagnosis")
    args = parser.parse_args()
    compiler = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
    if not compiler:
        raise SystemExit("A C11 compiler with address/undefined sanitizers is required.")
    with tempfile.TemporaryDirectory(prefix="esp32-coordinator-test-") as temporary:
        binary = Path(temporary) / "test_coordinator"
        command = [compiler, "-std=c11", "-D_POSIX_C_SOURCE=200809L",
                   "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-Wconversion", "-Wshadow", "-g",
                   "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                   "-I", str(SUITE / "stubs"),
                   "-I", str(ROOT / "components/hid_engine/include"),
                   "-I", str(ROOT / "components/input_core/include"),
                   "-I", str(ROOT / "components/interaction/include"),
                   "-I", str(ROOT / "components/device_settings/include"),
                   str(SUITE / "test_coordinator.c"),
                   str(ROOT / "components/hid_engine/hid_engine.c"),
                   str(ROOT / "components/input_core/input_core.c"),
                   str(ROOT / "components/interaction/interaction.c"),
                   str(ROOT / "components/device_settings/device_settings.c"),
                   "-lm", "-o", str(binary)]
        subprocess.run(command, check=True)
        subprocess.run([str(binary)] + ([args.case] if args.case else []), check=True)


if __name__ == "__main__":
    main()
