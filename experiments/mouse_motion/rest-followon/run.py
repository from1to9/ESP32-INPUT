#!/usr/bin/env python3
"""A separately frozen rest heuristic; original benchmark remains immutable."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parent
BASE = ROOT.parent
spec = importlib.util.spec_from_file_location("frozen_benchmark", BASE / "benchmark.py")
bench = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bench)


def freeze():
    if (ROOT / "freeze.json").exists():
        raise RuntimeError("Follow-on already frozen")
    paths = [ROOT / "estimator.c", ROOT / "protocol.json", ROOT / "run.py",
             BASE / "freeze.json", BASE / "run-1/results.json"]
    original = json.loads((BASE / "freeze.json").read_text())
    paths += [BASE / name for name in original["files"]]
    bench.write_json(ROOT / "freeze.json", {"files": {
        os.path.relpath(p, ROOT): bench.sha(p) for p in paths},
        "policy": "Frozen after baseline results but before any rest-variant build or execution; original acceptance unchanged"})
    print("Rest follow-on freeze SHA256 " + bench.sha(ROOT / "freeze.json"))


def run():
    frozen = json.loads((ROOT / "freeze.json").read_text())
    for name, expected in frozen["files"].items():
        if bench.sha(ROOT / name) != expected:
            raise RuntimeError("Frozen input changed: " + name)
    output = ROOT / "run-1"
    output.mkdir(exist_ok=False)
    build = ["clang", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
             "-g", "-O1", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
             "-I", str(BASE / "vendor/Fusion/Fusion"), str(ROOT / "estimator.c"),
             str(BASE / "vendor/Fusion/Fusion/FusionAhrs.c"), "-lm", "-o", str(output / "estimator")]
    built = subprocess.run(build, capture_output=True, text=True)
    (output / "build.log").write_text(built.stdout + built.stderr)
    bench.write_json(output / "build.json", {"command": build, "exit_code": built.returncode})
    built.check_returncode()
    cases = json.loads((BASE / "cases.json").read_text())
    limits = json.loads((BASE / "protocol.json").read_text())["acceptance"]
    inputs = bench.number_rows(BASE / "fixtures.csv")
    grouped = {case["id"]: [row for row in inputs if int(row["case_id"]) == case["id"]] for case in cases}
    results = {"followon_freeze_sha256": bench.sha(ROOT / "freeze.json"),
               "original_freeze_sha256": bench.sha(BASE / "freeze.json"), "runs": {}}
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0", UBSAN_OPTIONS="halt_on_error=1")
    for source in ("fusion", "oracle"):
        name = "integrate-rest-" + source
        path = output / (name + ".csv")
        with (BASE / "fixtures.csv").open() as stdin, path.open("w") as stdout:
            executed = subprocess.run([str(output / "estimator"), "integrate", source],
                stdin=stdin, stdout=stdout, stderr=subprocess.PIPE, text=True, env=env)
        (output / (name + ".stderr")).write_text(executed.stderr)
        executed.check_returncode()
        rows = bench.number_rows(path)
        case_results = []
        for case in cases:
            subset = [row for row in rows if int(row["case_id"]) == case["id"]]
            result = bench.evaluate(case, grouped[case["id"]], subset, limits)
            events = []
            previous = 0
            for row in subset:
                if row["rest_events"] > previous:
                    events.append(row["t"])
                    previous = row["rest_events"]
            result["diagnostics"]["rest_candidate_reset_times_seconds"] = events
            case_results.append(result)
        checks = [check for case in case_results for check in case["checks"]]
        results["runs"][name] = {"cases_passed": sum(case["passed"] for case in case_results),
            "case_count": len(case_results), "checks_passed": sum(check["passed"] for check in checks),
            "check_count": len(checks), "passed": all(check["passed"] for check in checks),
            "output_sha256": bench.sha(path), "cases": case_results}
    bench.write_json(output / "results.json", results)
    bench.write_json(output / "evidence.json", {"files": {str(p.relative_to(ROOT)): bench.sha(p)
        for p in sorted(output.iterdir()) if p.is_file()}, "followon_freeze_sha256": bench.sha(ROOT / "freeze.json")})
    print(json.dumps({name: {k: value[k] for k in ("cases_passed", "case_count", "checks_passed", "check_count", "passed")}
                      for name, value in results["runs"].items()}, indent=2))


if __name__ == "__main__":
    {"freeze": freeze, "run": run}[sys.argv[1]]()
