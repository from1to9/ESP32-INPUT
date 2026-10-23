#!/usr/bin/env python3
"""Run portable observer invariants; preserve build and failure evidence locally."""
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
COMPONENT = ROOT / "components/handheld_motion"
LOCAL = ROOT / ".agent/local"
LOCAL.mkdir(parents=True, exist_ok=True)
OUTPUT = Path(tempfile.mkdtemp(prefix="handheld-motion-host-", dir=LOCAL))
command = ["clang", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
           "-g", "-O1", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
           "-I", str(COMPONENT / "include"), str(Path(__file__).with_name("test_handheld_motion.c")),
           str(COMPONENT / "handheld_motion.c"), "-lm", "-o", str(OUTPUT / "test_handheld_motion")]
built = subprocess.run(command, capture_output=True, text=True)
(OUTPUT / "build.log").write_text(built.stdout + built.stderr)
(OUTPUT / "build.json").write_text(json.dumps({"command": command, "exit_code": built.returncode}, indent=2) + "\n")
built.check_returncode()
env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0", UBSAN_OPTIONS="halt_on_error=1")
tested = subprocess.run([str(OUTPUT / "test_handheld_motion")], capture_output=True, text=True, env=env)
(OUTPUT / "stdout.txt").write_text(tested.stdout)
(OUTPUT / "stderr.txt").write_text(tested.stderr)
(OUTPUT / "result.json").write_text(json.dumps({"exit_code": tested.returncode,
    "scope": "Portable invariants only; the separate 28-case diagnostic remains failed"}, indent=2) + "\n")
print(tested.stdout, end="")
print("Evidence: " + str(OUTPUT.relative_to(ROOT)))
tested.check_returncode()
