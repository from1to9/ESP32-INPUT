#!/usr/bin/env python3
"""Validate the production cached audio observer without device or network IO."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    output = args.output_dir or ROOT / ".agent/local/audio-diagnostics" / datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    output.mkdir(parents=True, exist_ok=False)
    compiler = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
    if not compiler:
        raise SystemExit("A C11 compiler with sanitizers is required")
    common = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-Wconversion", "-Wshadow",
              "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    for name in ("tests/audio_diagnostics/stubs", "tests/presenter/stubs", "components/audio_engine/include",
                 "components/spoken_letters/include"):
        common += ["-I", str(ROOT / name)]
    units = [ROOT / "tests/audio_diagnostics/test_audio_diagnostics.c", ROOT / "components/audio_engine/audio_media.c"]
    files = {Path(__file__).resolve(), ROOT / "tests/audio_diagnostics/README.md"}
    for unit in units:
        dependencies = subprocess.check_output(common + ["-MM", str(unit)], text=True, cwd=ROOT)
        body = dependencies.partition(":")[2].replace("\\\n", " ")
        files.update(Path(name).resolve() for name in shlex.split(body))

    def hashes():
        return {str(path.relative_to(ROOT) if path.is_relative_to(ROOT) else path):
                hashlib.sha256(path.read_bytes()).hexdigest() for path in sorted(files)}

    before = hashes()
    binary = output / "audio_diagnostics"
    with (output / "build.log").open("w") as log:
        subprocess.run(common + [str(unit) for unit in units] + ["-o", str(binary)], cwd=ROOT,
                       stdout=log, stderr=subprocess.STDOUT, check=True)
    environment = dict(os.environ, UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    if sys.platform == "darwin":
        environment["ASAN_OPTIONS"] = "detect_leaks=0"
    names = subprocess.check_output([str(binary), "--list"], text=True, env=environment).splitlines()
    results = {}
    for name in names:
        result = subprocess.run([str(binary), name], capture_output=True, text=True, env=environment, timeout=20)
        (output / (name + ".log")).write_text(result.stdout + result.stderr)
        print(result.stdout + result.stderr, end="")
        results[name] = result.returncode
    after = hashes()
    manifest = {"source_before": before, "source_after": after, "source_stable": before == after,
                "cases": results, "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
                "hardware_access": False, "network_access": False}
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    assert before == after and all(code == 0 for code in results.values())
    print("Evidence:", output)


if __name__ == "__main__":
    main()
