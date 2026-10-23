#!/usr/bin/env python3
"""Freeze synthetic inputs, then execute the unmodified frozen benchmark."""
import csv
import hashlib
import json
import math
from pathlib import Path
import random
import subprocess
import sys

ROOT = Path(__file__).resolve().parent
G = 9.80665


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n")


def matmul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(3))
             for j in range(3)] for i in range(3)]


def rotate(axis, angle):
    c, s = math.cos(angle), math.sin(angle)
    if axis == "x":
        return [[1, 0, 0], [0, c, -s], [0, s, c]]
    if axis == "y":
        return [[c, 0, s], [0, 1, 0], [-s, 0, c]]
    return [[c, -s, 0], [s, c, 0], [0, 0, 1]]


def mv(m, v, transpose=False):
    return [sum((m[j][i] if transpose else m[i][j]) * v[j]
                for j in range(3)) for i in range(3)]


def case_list():
    cases = [
        dict(name="first_10ms_impulse", kind="impulse", duration=1),
        dict(name="accelerate_coast_brake", kind="stroke", duration=3),
        dict(name="five_second_constant_speed_coast", kind="stroke", duration=6.4,
             ramp=0.2, peak=0.1, coast_end=5.2, stop_from=5.7),
    ]
    for name, grip, axis, period, duration in [
        ("wave_flat_x", (0, 0), "x", 2, 12),
        ("wave_flat_y", (0, 0), "y", 2, 12),
        ("wave_diagonal", (0, 0), "diagonal", 2, 12),
        ("wave_figure_eight", (0, 0), "figure_eight", 2, 12),
        ("wave_roll_30", (30, 0), "x", 2, 12),
        ("wave_roll_60", (60, 0), "y", 2, 12),
        ("wave_roll_90", (90, 0), "y", 2, 12),
        ("wave_pitch_45", (0, 45), "x", 2, 12),
        ("wave_fast", (0, 0), "x", 0.8, 12),
        ("wave_slow", (0, 0), "x", 6, 18),
    ]:
        cases.append(dict(name=name, kind="wave", grip=grip, axis=axis,
                          period=period, duration=duration + 1, moving_until=duration))
    cases += [
        dict(name="wave_rotating_grip", kind="wave", period=2,
             duration=13, moving_until=12, dynamic_rotation="x"),
        dict(name="wave_small_clock_jitter", kind="wave", period=2,
             duration=13, moving_until=12, jitter=True),
        dict(name="wave_noise_bias_tilt", kind="wave", period=2,
             duration=13, moving_until=12, grip=(30, 20), noise=True, bias=True),
        dict(name="pure_roll_about_sensor", kind="rotation", duration=12,
             dynamic_rotation="x"),
        dict(name="pure_yaw_about_sensor", kind="rotation", duration=12,
             dynamic_rotation="z"),
        dict(name="rest_noise", kind="rest", duration=10, noise=True),
        dict(name="rest_bias_step", kind="rest", duration=10, bias=True),
        dict(name="rest_noise_bias_tilt", kind="rest", duration=10,
             grip=(30, 20), noise=True, bias=True),
        dict(name="cadence_missing_20ms", kind="fault", duration=6, fault="gap"),
        dict(name="latest_cache_15ms_rejected", kind="cadence", duration=2),
        dict(name="timestamp_duplicate", kind="fault", duration=6, fault="duplicate"),
        dict(name="sensor_nonfinite", kind="fault", duration=6, fault="nan"),
        dict(name="gyro_overrange_recovery", kind="fault", duration=6, fault="overrange"),
        dict(name="accel_rejection_recovery", kind="accel_recovery", duration=10),
        dict(name="owner_menu_output_gate", kind="gate", duration=4),
    ]
    for index, case in enumerate(cases):
        case["id"] = index
    return cases


def velocity(case, t):
    if t <= 0:
        return [0.0, 0.0, 0.0]
    kind = case["kind"]
    if kind == "impulse":
        return [0.008 * min(t / 0.01, 1), 0, 0]
    if kind == "stroke":
        ramp, peak, end = case.get("ramp", 0.3), case.get("peak", 0.3), case.get("coast_end", 1.5)
        speed = peak * min(t / ramp, 1) if t < end else max(0, peak * (1 - (t - end) / ramp))
        return [speed, 0, 0]
    if kind == "wave" and t < case["moving_until"] - 1e-8:
        w = 2 * math.pi / case["period"]
        x = 0.25 * math.sin(w * t)
        axis = case.get("axis", "x")
        if axis == "y":
            return [0, x, 0]
        if axis == "diagonal":
            return [x / math.sqrt(2), x / math.sqrt(2), 0]
        if axis == "figure_eight":
            return [x, 0.15 * math.sin(2 * w * t), 0]
        return [x, 0, 0]
    if kind in ("fault", "gate"):
        if t < 1:
            return [0.2 * math.sin(2 * math.pi * t), 0, 0]
        if kind == "gate" and 2 < t < 3:
            return [0.2 * math.sin(2 * math.pi * (t - 2)), 0, 0]
    return [0.0, 0.0, 0.0]


def angle(case, t):
    if not case.get("dynamic_rotation") or t <= 0:
        return 0.0
    amplitude = 60 if case["dynamic_rotation"] == "x" else 120
    return math.radians(amplitude) * math.sin(2 * math.pi * t / 3)


def freeze():
    if (ROOT / "freeze.json").exists():
        raise RuntimeError("Freeze already exists; preserving original inputs")
    cases = case_list()
    write_json(ROOT / "cases.json", cases)
    rng = random.Random(20261023)
    count = 0
    with (ROOT / "fixtures.csv").open("w", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(["case_id", "t", "time", "dt", "permitted", "recenter",
                         "gx", "gy", "gz", "ax", "ay", "az", "truth_ax", "truth_ay",
                         "truth_vx", "truth_vy"])
        for case in cases:
            roll, pitch = case.get("grip", (0, 0))
            grip = matmul(rotate("y", math.radians(pitch)), rotate("x", math.radians(roll)))
            elapsed = 0.0
            index = 0
            fault_done = False
            rearmed = False
            armed_initial = False
            previous_timestamp = 0.0
            while elapsed < 5 + case["duration"] - 1e-8:
                dt = [0.0098, 0.0102, 0.0101, 0.0099][index % 4] if case.get("jitter") else 0.01
                prior_t = elapsed - 5
                if case["kind"] == "cadence" and prior_t >= -1e-8:
                    dt = 0.015
                fault = (case["kind"] == "fault" and not fault_done and prior_t >= 0.99 - 1e-8)
                if fault and case["fault"] == "gap":
                    dt = 0.02
                elapsed += dt
                t = elapsed - 5
                prev_v, current_v = velocity(case, prior_t), velocity(case, t)
                local_accel = [(current_v[i] - prev_v[i]) / dt for i in range(3)]
                rotation_axis = case.get("dynamic_rotation", "x")
                body_to_world = matmul(grip, rotate(rotation_axis, angle(case, t)))
                world_accel = mv(grip, local_accel)
                body_specific_force = mv(body_to_world, [world_accel[0] / G,
                    world_accel[1] / G, world_accel[2] / G + 1], transpose=True)
                gyro = [0.0, 0.0, 0.0]
                gyro["xyz".index(rotation_axis)] = math.degrees(angle(case, t) - angle(case, prior_t)) / dt
                if case.get("bias") and t >= 1:
                    body_specific_force = [v + b for v, b in zip(body_specific_force, [0.005, -0.003, 0.002])]
                if case.get("noise"):
                    body_specific_force = [v + rng.gauss(0, 0.003) for v in body_specific_force]
                    gyro = [v + rng.gauss(0, 0.03) for v in gyro]
                if case["kind"] == "accel_recovery" and 0 < t <= 5.4 + 1e-8:
                    body_specific_force = [0.5, 0, math.sqrt(0.75)]
                permitted = t >= -1e-8
                if case["kind"] == "gate" and 0.4 - 1e-8 <= t < 1.5 - 1e-8:
                    permitted = False
                recenter = False
                if t >= -1e-8 and not armed_initial:
                    recenter = True
                    armed_initial = True
                if case["kind"] == "fault" and t >= 4.5 - 1e-8 and not rearmed:
                    recenter = True
                    rearmed = True
                if case["kind"] == "gate" and t >= 1.6 - 1e-8 and not rearmed:
                    recenter = True
                    rearmed = True
                timestamp = elapsed
                if fault:
                    if case["fault"] == "duplicate":
                        timestamp = previous_timestamp
                    elif case["fault"] == "nan":
                        body_specific_force[0] = float("nan")
                    elif case["fault"] == "overrange":
                        gyro[0] = 2500.0
                    fault_done = True
                writer.writerow([case["id"], format(t, ".9f"), format(timestamp, ".9f"),
                    format(dt, ".9f"), int(permitted), int(recenter)] +
                    [format(v, ".12g") for v in gyro + body_specific_force + local_accel[:2] + current_v[:2]])
                previous_timestamp = timestamp
                index += 1
                count += 1
    paths = [ROOT / name for name in ["protocol.json", "cases.json", "fixtures.csv",
                                      "estimator.c", "benchmark.py", "README.md"]]
    paths += sorted((ROOT / "vendor").rglob("*"))
    hashes = {str(p.relative_to(ROOT)): sha(p) for p in paths if p.is_file()}
    write_json(ROOT / "freeze.json", {"schema": 1, "rows": count, "cases": len(cases),
        "files": hashes, "policy": "Frozen before any estimator build or execution"})
    print(json.dumps({"cases": len(cases), "rows": count,
                      "freeze_sha256": sha(ROOT / "freeze.json")}, indent=2))


def number_rows(path):
    with path.open() as stream:
        return [{k: float(v) for k, v in row.items()} for row in csv.DictReader(stream)]


def evaluate(case, source_rows, output_rows, limits):
    checks = []

    def add(name, observed, lower=None, upper=None):
        passed = math.isfinite(observed) and (lower is None or observed >= lower - 1e-9) and (upper is None or observed <= upper + 1e-9)
        checks.append(dict(name=name, observed=observed, lower=lower, upper=upper, passed=passed))

    pairs = list(zip(source_rows, output_rows))
    assert len(source_rows) == len(output_rows)
    assert all(a["case_id"] == b["case_id"] and abs(a["t"] - b["t"]) < 1e-8 for a, b in pairs)
    active = [(a, b) for a, b in pairs if a["t"] > 1e-8]
    gated = [b for a, b in pairs if b["status"] != 0]
    add("gated_integer_counts", sum(abs(b["hid_x"]) + abs(b["hid_y"]) for b in gated), upper=0)
    # Explicit recenter has zero state too; READY is the only integrating status.
    add("gated_max_velocity_mps", max([math.hypot(b["vx"], b["vy"]) for b in gated] or [0]), upper=0)
    initial_ready = next((a["time"] for a, b in pairs if not b["startup"]), None)
    add("startup_elapsed_seconds", initial_ready if initial_ready is not None else 1e6,
        *limits["startup_elapsed_seconds_range"])
    kind = case["kind"]
    if kind in ("wave", "stroke"):
        moving = [(a, b) for a, b in active if math.hypot(a["truth_vx"], a["truth_vy"]) >= limits["moving_speed_floor_mps"]]
        rms_error = math.sqrt(sum((a["truth_vx"] - b["vx"]) ** 2 + (a["truth_vy"] - b["vy"]) ** 2 for a, b in moving) / len(moving))
        rms_truth = math.sqrt(sum(a["truth_vx"] ** 2 + a["truth_vy"] ** 2 for a, b in moving) / len(moving))
        add("moving_velocity_nrmse", rms_error / rms_truth, upper=limits["moving_velocity_nrmse_max"])
        def aligned(a, b):
            dot = a["truth_vx"] * b["rate_x"] + a["truth_vy"] * b["rate_y"]
            norm = math.hypot(a["truth_vx"], a["truth_vy"]) * math.hypot(b["rate_x"], b["rate_y"])
            return norm > 0 and dot / norm >= math.cos(math.radians(30))
        add("direction_agreement", sum(aligned(a, b) for a, b in moving) / len(moving), lower=limits["direction_agreement_min"])
        first_hid = next((a["t"] for a, b in active if abs(b["hid_x"]) + abs(b["hid_y"]) > 0), 1e6)
        add("motion_to_first_hid_seconds", first_hid, upper=limits["motion_to_first_hid_seconds_max"])
        stop_from = case.get("stop_from", 2.1) if kind == "stroke" else case["moving_until"] + 0.3
        stopped = [(a, b) for a, b in active if a["t"] >= stop_from - 1e-8]
        add("stopped_max_speed_mps", max(math.hypot(b["vx"], b["vy"]) for a, b in stopped), upper=limits["stop_speed_mps_max"])
        if kind == "stroke":
            ramp, peak, end = case.get("ramp", 0.3), case.get("peak", 0.3), case.get("coast_end", 1.5)
            coast = [b["vx"] / peak for a, b in active if ramp + 0.3 <= a["t"] <= end - 0.1]
            add("coast_min_speed_ratio", min(coast), lower=limits["coast_speed_ratio_range"][0])
            add("coast_max_speed_ratio", max(coast), upper=limits["coast_speed_ratio_range"][1])
            add("early_braking_opposite_seconds", sum(a["dt"] for a, b in active if end <= a["t"] <= end + 0.8 * ramp and b["rate_x"] < 0), upper=limits["braking_early_opposite_seconds_max"])
        else:
            # Every complete half-wave must keep serving the whole session, without a timeout.
            half = case["period"] / 2
            coverage = []
            delays = []
            primary = "y" if case.get("axis") == "y" else "x"
            for n in range(round(case["moving_until"] / half)):
                group = [(a, b) for a, b in moving if n * half < a["t"] <= (n + 1) * half + 1e-8]
                coverage.append(sum(aligned(a, b) for a, b in group) / len(group))
                if n:
                    sign = 1 if n % 2 == 0 else -1
                    switched = next((a["t"] for a, b in active if n * half <= a["t"] < (n + 1) * half and sign * b["rate_" + primary] > 0), None)
                    delays.append((switched - n * half) if switched is not None else half)
            add("minimum_half_wave_coverage", min(coverage), lower=limits["each_half_wave_active_fraction_min"])
            add("maximum_reversal_delay_seconds", max(delays), upper=limits["reversal_delay_seconds_max"])
    if kind == "impulse":
        first = next(b for a, b in active)
        add("first_impulse_retention", first["vx"] / 0.008,
            *limits["first_impulse_retention_ratio_range"])
    if kind in ("rest", "rotation"):
        path = sum(abs(b["hid_x"]) + abs(b["hid_y"]) for a, b in active)
        key = "pure_rotation_pointer_path_counts_max" if kind == "rotation" else "stationary_pointer_path_counts_max"
        add("pointer_path_counts", path, upper=limits[key])
    if kind == "fault":
        fault_time = next(a["t"] for a, b in active if b["status"] in (1, 3, 6) and a["t"] >= 0.99 - 1e-8)
        before_recenter = [b for a, b in active if fault_time - 1e-8 <= a["t"] < 4.5 - 1e-8]
        add("fault_to_recenter_counts", sum(abs(b["hid_x"]) + abs(b["hid_y"]) for b in before_recenter), upper=0)
        add("fault_to_recenter_armed_frames", sum(b["armed"] for b in before_recenter), upper=0)
        after_recenter = [b for a, b in active if a["t"] >= 4.5 - 1e-8]
        add("rearmed_after_explicit_recenter", max(b["armed"] for b in after_recenter), lower=1, upper=1)
        if case["fault"] == "overrange":
            first_recovered = next(a["t"] for a, b in active if a["t"] > fault_time and not b["startup"])
            add("overrange_recovery_seconds", first_recovered - fault_time,
                *limits["overrange_recovery_elapsed_seconds_range"])
    if kind == "cadence":
        add("irregular_cadence_rejected_fraction", sum(b["status"] == 3 for a, b in active) / len(active), lower=1, upper=1)
    if kind == "accel_recovery":
        recovery = [(a, b) for a, b in active if b["acceleration_recovery"]]
        add("first_accel_recovery_seconds", recovery[0][0]["t"] if recovery else 1e6,
            *limits["acceleration_recovery_elapsed_seconds_range"])
        add("recovery_pointer_counts", sum(abs(b["hid_x"]) + abs(b["hid_y"]) for a, b in recovery), upper=0)
    hid_events = [a["t"] for a, b in active if abs(b["hid_x"]) + abs(b["hid_y"]) > 0]
    suppressed = [a["t"] for a, b in active if b["status"] == 0 and not (b["rate_x"] or b["rate_y"])]
    return {"name": case["name"], "passed": all(c["passed"] for c in checks), "checks": checks,
            "diagnostics": {"max_speed_mps": max(math.hypot(b["vx"], b["vy"]) for a, b in active),
                            "pointer_path_counts": sum(abs(b["hid_x"]) + abs(b["hid_y"]) for a, b in active),
                            "first_hid_seconds": hid_events[0] if hid_events else None,
                            "last_hid_seconds": hid_events[-1] if hid_events else None,
                            "first_deadband_suppression_seconds": suppressed[0] if suppressed else None,
                            "last_deadband_suppression_seconds": suppressed[-1] if suppressed else None,
                            "deadband_suppressed_frames": len(suppressed),
                            "accel_ignored_frames": sum(b["accel_ignored"] for a, b in active),
                            "recovery_frames": sum(b["acceleration_recovery"] for a, b in active)}}


def run():
    freeze_data = json.loads((ROOT / "freeze.json").read_text())
    for name, expected in freeze_data["files"].items():
        if sha(ROOT / name) != expected:
            raise RuntimeError("Frozen source changed: " + name)
    output = ROOT / "run-1"
    output.mkdir(exist_ok=False)
    build = ["clang", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
             "-g", "-O1", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
             "-I", str(ROOT / "vendor/Fusion/Fusion"), str(ROOT / "estimator.c"),
             str(ROOT / "vendor/Fusion/Fusion/FusionAhrs.c"), "-lm", "-o", str(output / "estimator")]
    built = subprocess.run(build, capture_output=True, text=True)
    (output / "build.log").write_text(built.stdout + built.stderr)
    write_json(output / "build.json", {"command": build, "exit_code": built.returncode})
    built.check_returncode()
    cases = json.loads((ROOT / "cases.json").read_text())
    protocol = json.loads((ROOT / "protocol.json").read_text())
    inputs = number_rows(ROOT / "fixtures.csv")
    grouped = {case["id"]: [row for row in inputs if int(row["case_id"]) == case["id"]] for case in cases}
    import os
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0", UBSAN_OPTIONS="halt_on_error=1")
    results = {"freeze_sha256": sha(ROOT / "freeze.json"), "runs": {}}
    for candidate in ("integrate", "leaky"):
        for source in ("fusion", "oracle"):
            name = candidate + "-" + source
            csv_path = output / (name + ".csv")
            with (ROOT / "fixtures.csv").open() as stdin, csv_path.open("w") as stdout:
                executed = subprocess.run([str(output / "estimator"), candidate, source],
                                          stdin=stdin, stdout=stdout, stderr=subprocess.PIPE,
                                          text=True, env=env)
            (output / (name + ".stderr")).write_text(executed.stderr)
            executed.check_returncode()
            rows = number_rows(csv_path)
            case_results = [evaluate(case, grouped[case["id"]],
                [row for row in rows if int(row["case_id"]) == case["id"]],
                protocol["acceptance"]) for case in cases]
            checks = [check for case in case_results for check in case["checks"]]
            results["runs"][name] = {"cases_passed": sum(case["passed"] for case in case_results),
                "case_count": len(case_results), "checks_passed": sum(check["passed"] for check in checks),
                "check_count": len(checks), "passed": all(check["passed"] for check in checks),
                "output_sha256": sha(csv_path), "cases": case_results}
    write_json(output / "results.json", results)
    write_json(output / "evidence.json", {"files": {str(p.relative_to(ROOT)): sha(p)
        for p in sorted(output.iterdir()) if p.is_file()}, "freeze_sha256": sha(ROOT / "freeze.json")})
    print(json.dumps({name: {k: value[k] for k in ("cases_passed", "case_count", "checks_passed", "check_count", "passed")}
                      for name, value in results["runs"].items()}, indent=2))


if __name__ == "__main__":
    {"freeze": freeze, "run": run}[sys.argv[1]]()
