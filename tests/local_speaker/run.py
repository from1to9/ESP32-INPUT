#!/usr/bin/env python3
"""Strict native checks of the opt-in boot tone using the real board/codec."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import shlex
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--output-dir", required=True, type=Path)
args = parser.parse_args()
args.output_dir.mkdir(parents=True, exist_ok=False)
compiler = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
if not compiler:
    raise SystemExit("A C11 compiler is required")
codec = ROOT / "firmware/managed_components/espressif__esp_codec_dev"
common = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
          "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
for path in ["tests/local_speaker/stubs", "tests/board_audio/stubs", "firmware/main", "components/audio_engine/include"]:
    common += ["-I", str(ROOT / path)]
for path in ["include", "interface", "device/include", "device/priv_include"]:
    common += ["-isystem", str(codec / path)]
units = [ROOT / "tests/local_speaker/test_local_speaker.c"] + [codec / p for p in
    ["device/es8311/es8311.c", "esp_codec_dev_vol.c", "esp_codec_dev_if.c"]] + [ROOT / "components/audio_engine" / p for p in
    ["audio_engine.c", "audio_media.c", "audio_recording.c"]]
inputs = {Path(__file__).resolve()}
commands = []
objects = []
for index, unit in enumerate(units):
    flags = ["-pedantic"] if index == 0 or index >= 4 else ["-include", "stdlib.h", "-Wno-error=sign-compare"]
    dep = subprocess.check_output(common + flags + ["-MM", str(unit)], text=True, cwd=ROOT)
    inputs.update(Path(p).resolve() for p in shlex.split(dep.partition(":")[2].replace("\\\n", " ")))
    obj = args.output_dir / f"unit-{index}.o"
    commands.append(common + flags + ["-c", str(unit), "-o", str(obj)])
    objects.append(str(obj))
hashes = lambda: {str(p.relative_to(ROOT) if p.is_relative_to(ROOT) else p):
    hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(inputs)}
before = hashes()
with (args.output_dir / "build.log").open("w") as log:
    for command in commands:
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
    binary = args.output_dir / "local_speaker"
    subprocess.run(common + objects + ["-lm", "-o", str(binary)], stdout=log, stderr=subprocess.STDOUT, check=True)
environment = dict(os.environ, UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
if sys.platform == "darwin":
    environment["ASAN_OPTIONS"] = "detect_leaks=0"
cases = {}
for name in ["success", "locked", "cancel-before", "cancel", "cancel-unmute", "prepare-timeout", "write-timeout",
             "short-write", "sdk-error", "probe-error", "volume-error", "stop-timeout", "clock-regression"]:
    run = subprocess.run([str(binary), name], capture_output=True, text=True, env=environment, timeout=20)
    (args.output_dir / f"{name}.log").write_text(run.stdout + run.stderr)
    print(run.stdout + run.stderr, end="")
    cases[name] = run.returncode
after = hashes()
(args.output_dir / "receipt.json").write_text(json.dumps({"source_before": before, "source_after": after,
    "source_stable": before == after, "cases": cases, "commands": commands,
    "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
    "scope": "Host SDK-boundary checks only; no target, physical sound or public API qualification"}, indent=2) + "\n")
assert before == after and all(code == 0 for code in cases.values())
