#!/usr/bin/env python3
"""Test production presentation handoff with controlled LVGL/clock/lock boundaries."""
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
    with tempfile.TemporaryDirectory(prefix="esp32-presenter-test-") as temporary:
        binary = Path(temporary) / "test_presenter"
        command = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-Wconversion", "-Wshadow", "-g", "-fsanitize=address,undefined",
                   "-fno-omit-frame-pointer", "-pthread", "-I", str(SUITE / "stubs"),
                   "-I", str(ROOT / "components/input_core/include"),
                   str(SUITE / "test_presenter.c"), "-o", str(binary)]
        subprocess.run(command, check=True)
        cases = [args.case] if args.case else subprocess.check_output(
            [str(binary), "--list"], text=True).splitlines()
        for case in cases:
            # A fresh process preserves production startup semantics. A stalled
            # lock/render handoff fails finitely instead of hanging the runner.
            subprocess.run([str(binary), case], check=True, timeout=20)
        print(f"Presenter: {len(cases)} cases passed; target LVGL scheduling remains unqualified.")


if __name__ == "__main__":
    main()
