#!/usr/bin/env python3
"""Freeze a new observer hypothesis; retain all original diagnostic failures."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parent
BASE = ROOT.parent
REPO = BASE.parents[1]
COMPONENT = REPO / "components/handheld_motion"
TEST = REPO / "tests/handheld_motion/test_handheld_motion.c"
spec = importlib.util.spec_from_file_location("original_benchmark", BASE / "benchmark.py")
bench = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bench)


def freeze():
    if (ROOT / "freeze.json").exists():
        raise RuntimeError("Candidate already frozen")
    baseline = json.loads((BASE / "freeze.json").read_text())
    for name, expected in baseline["files"].items():
        if bench.sha(BASE / name) != expected:
            raise RuntimeError("Original frozen input changed: " + name)
    paths = [ROOT / "adapter.c", ROOT / "protocol.json", ROOT / "run.py", ROOT / ".gitignore",
             COMPONENT / "handheld_motion.c", COMPONENT / "include/handheld_motion.h",
             COMPONENT / "CMakeLists.txt", TEST, BASE / "freeze.json"]
    paths += [BASE / name for name in baseline["files"]]
    bench.write_json(ROOT / "freeze.json", {"files": {
        os.path.relpath(p, ROOT): bench.sha(p) for p in paths},
        "policy": "Frozen before any new-candidate build or execution; original 28 fixtures and all criteria unchanged"})
    print("Candidate freeze SHA256 " + bench.sha(ROOT / "freeze.json"))


def run():
    frozen = json.loads((ROOT / "freeze.json").read_text())
    for name, expected in frozen["files"].items():
        if bench.sha(ROOT / name) != expected:
            raise RuntimeError("Frozen input changed: " + name)
    output = ROOT / "run-1"
    output.mkdir(exist_ok=False)
    common = ["clang", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
              "-g", "-O1", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
              "-I", str(COMPONENT / "include")]
    command = common + ["-I", str(BASE / "vendor/Fusion/Fusion"), str(ROOT / "adapter.c"),
        str(COMPONENT / "handheld_motion.c"), str(BASE / "vendor/Fusion/Fusion/FusionAhrs.c"),
        "-lm", "-o", str(output / "estimator")]
    built = subprocess.run(command, capture_output=True, text=True)
    (output / "build.log").write_text(built.stdout + built.stderr)
    bench.write_json(output / "build.json", {"command": command, "exit_code": built.returncode})
    built.check_returncode()
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0", UBSAN_OPTIONS="halt_on_error=1")
    path = output / "gyro-braking.csv"
    with (BASE / "fixtures.csv").open() as stdin, path.open("w") as stdout:
        executed = subprocess.run([str(output / "estimator")], stdin=stdin, stdout=stdout,
            stderr=subprocess.PIPE, text=True, env=env)
    (output / "gyro-braking.stderr").write_text(executed.stderr)
    executed.check_returncode()
    cases = json.loads((BASE / "cases.json").read_text())
    limits = json.loads((BASE / "protocol.json").read_text())["acceptance"]
    inputs = bench.number_rows(BASE / "fixtures.csv")
    rows = bench.number_rows(path)
    results = [bench.evaluate(case,
        [r for r in inputs if int(r["case_id"]) == case["id"]],
        [r for r in rows if int(r["case_id"]) == case["id"]], limits) for case in cases]
    checks = [check for case in results for check in case["checks"]]
    report = {"freeze_sha256": bench.sha(ROOT / "freeze.json"), "cases_passed": sum(c["passed"] for c in results),
        "case_count": len(results), "checks_passed": sum(c["passed"] for c in checks), "check_count": len(checks),
        "passed": all(c["passed"] for c in checks), "output_sha256": bench.sha(path), "cases": results}
    bench.write_json(output / "results.json", report)
    test_command = common + [str(TEST), str(COMPONENT / "handheld_motion.c"), "-lm", "-o", str(output / "test_handheld_motion")]
    compiled_test = subprocess.run(test_command, capture_output=True, text=True)
    (output / "unit-build.log").write_text(compiled_test.stdout + compiled_test.stderr)
    compiled_test.check_returncode()
    tested = subprocess.run([str(output / "test_handheld_motion")], capture_output=True, text=True, env=env)
    (output / "unit.stdout").write_text(tested.stdout)
    (output / "unit.stderr").write_text(tested.stderr)
    bench.write_json(output / "unit.json", {"command": test_command, "exit_code": tested.returncode})
    tested.check_returncode()
    for name, expected in frozen["files"].items():
        assert bench.sha(ROOT / name) == expected, name
    print(json.dumps({k: report[k] for k in ["cases_passed", "case_count", "checks_passed", "check_count", "passed"]}, indent=2))
    print(tested.stdout, end="")


if __name__ == "__main__":
    {"freeze": freeze, "run": run}[sys.argv[1]]()
