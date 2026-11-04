#!/usr/bin/env python3
"""Focused real-board/codec PCM observer checks; no device or target build."""
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
flags = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
         "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
for p in ["tests/board_audio/stubs", "firmware/main", "components/audio_engine/include"]:
    flags += ["-I", str(ROOT / p)]
for p in ["include", "interface", "device/include", "device/priv_include"]:
    flags += ["-isystem", str(codec / p)]
units = [ROOT / "tests/board_audio/test_pcm_snapshot.c"] + [codec / p for p in
    ["device/es8311/es8311.c", "esp_codec_dev_vol.c", "esp_codec_dev_if.c"]]
inputs = {Path(__file__).resolve()}
commands = []
objects = []
for i, src in enumerate(units):
    extra = ["-pedantic"] if i == 0 else ["-include", "stdlib.h", "-Wno-error=sign-compare"]
    dep = subprocess.check_output(flags + extra + ["-MM", str(src)], text=True, cwd=ROOT)
    inputs.update(Path(p).resolve() for p in shlex.split(dep.partition(":")[2].replace("\\\n", " ")))
    obj = args.output_dir / f"unit-{i}.o"
    commands.append(flags + extra + ["-c", str(src), "-o", str(obj)])
    objects.append(str(obj))
hashes = lambda: {str(p.relative_to(ROOT) if p.is_relative_to(ROOT) else p):
    hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(inputs)}
before = hashes()
with (args.output_dir / "build.log").open("w") as log:
    for command in commands:
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
    binary = args.output_dir / "pcm_snapshot"
    subprocess.run(flags + objects + ["-lm", "-o", str(binary)], stdout=log, stderr=subprocess.STDOUT, check=True)
environment = dict(os.environ, UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
if sys.platform == "darwin":
    environment["ASAN_OPTIONS"] = "detect_leaks=0"
cases = {}
for name in ["existing", "signed", "zero", "stereo", "prefix", "invalid", "capture", "generation", "levels", "saturation", "cadence"]:
    run = subprocess.run([str(binary), name], capture_output=True, text=True, env=environment, timeout=20)
    (args.output_dir / f"{name}.log").write_text(run.stdout + run.stderr)
    print(run.stdout + run.stderr, end="")
    cases[name] = run.returncode
after = hashes()
(args.output_dir / "receipt.json").write_text(json.dumps({"source_before": before, "source_after": after,
    "source_stable": before == after, "cases": cases, "commands": commands,
    "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
    "scope": "Actual board/codec with SDK boundary models; no target sections, DMA/cache, physical or acoustic proof"}, indent=2) + "\n")
assert before == after and all(code == 0 for code in cases.values())
