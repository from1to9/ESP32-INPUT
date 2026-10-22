#!/usr/bin/env python3
"""Exercise actual media composition at its independently tested module boundaries."""
from pathlib import Path
import os
import shutil
import subprocess
import hashlib
import json
from datetime import datetime, timezone

root = Path(__file__).resolve().parents[2]
suite = Path(__file__).resolve().parent
compiler = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
if not compiler:
    raise SystemExit("A C11 compiler with sanitizers is required.")
directory = root / ".agent/local/media-runtime" / datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
directory.mkdir(parents=True)
files = [root / name for name in ("firmware/main/app_media_runtime.c", "firmware/main/app_media_runtime.h",
    "firmware/main/app_remote_media_runtime_port.h", "firmware/main/app_remote_media.h",
    "firmware/main/app_audio.h", "tests/media_runtime/test_media_runtime.c", "tests/media_runtime/run.py",
    "components/audio_engine/audio_media.c", "components/audio_engine/include/audio_engine.h",
    "components/audio_engine/include/audio_io.h", "components/audio_engine/include/audio_store.h",
    "tests/media_runtime/test_network_composition.c", "firmware/main/app_audio.c", "firmware/main/app_control_server.h",
    "components/control_protocol/include/control_media.h", "components/audio_engine/audio_engine.c",
    "components/audio_engine/audio_recording.c", "tests/audio_engine/stubs/esp_err.h",
    "tests/audio_engine/heap_caps_model.h", "tests/audio_engine/stubs/esp_heap_caps.h")]
files += sorted(p for p in (suite / "stubs").rglob("*") if p.is_file())
before = {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in files}
binary = str(directory / "media_runtime")
includes = [suite / "stubs", root / "tests/usb_audio/stubs", root / "tests/usb_hid/stubs",
            root / "components/audio_engine/include", root / "components/control_protocol/include", root / "components/spoken_letters/include",
            root / "firmware/main"]
command = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
           "-Wconversion", "-Wshadow", "-g", "-fsanitize=address,undefined",
           "-fno-omit-frame-pointer"]
for include in includes:
    command += ["-I", str(include)]
with (directory / "build.log").open("w") as log:
    subprocess.run(command + [str(suite / "test_media_runtime.c"),
        str(root / "components/audio_engine/audio_media.c"), "-o", binary], check=True, stdout=log, stderr=subprocess.STDOUT)
run = subprocess.run([binary], env=dict(os.environ, UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1"),
    text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
(directory / "test.log").write_text(run.stdout)
print(run.stdout, end="")
network_binary=directory / "network_composition"
network_command=[compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-Wconversion", "-Wshadow",
    "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-pthread"]
for include in [suite / "stubs", root / "tests/audio_engine/stubs", root / "components/audio_engine/include",
                root / "components/control_protocol/include", root / "components/spoken_letters/include", root / "firmware/main"]:
    network_command += ["-I", str(include)]
network_sources=[suite / "test_network_composition.c", root / "firmware/main/app_audio.c"]
network_sources += [root / "components/audio_engine" / name for name in ("audio_engine.c", "audio_media.c", "audio_recording.c")]
with (directory / "network-build.log").open("w") as log:
    subprocess.run(network_command+[str(p) for p in network_sources]+["-o",str(network_binary)],
        check=True, stdout=log, stderr=subprocess.STDOUT)
network_run=subprocess.run([str(network_binary)], env=dict(os.environ, UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1"),
    text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20)
(directory / "network-test.log").write_text(network_run.stdout)
print(network_run.stdout,end="")
if network_run.returncode:
    raise SystemExit(network_run.returncode)
variants = {}
for name, psram, external_allowed, expected in (
    ("default", 0, 0, 0), ("external", 1, 1, 1),
    ("no-psram", 0, 1, 0), ("external-disabled", 1, 0, 0),
):
    flags = [f"-DCONFIG_SPIRAM={psram}",
             f"-DCONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM={external_allowed}",
             f"-DTEST_EXPECT_EXTERNAL_STACK={expected}"]
    unit = Path(binary) if name == "default" else directory / ("media-" + name)
    network = network_binary if name == "default" else directory / ("network-" + name)
    if name != "default":
        with (directory / (name + "-build.log")).open("w") as log:
            subprocess.run(command + flags + [str(suite / "test_media_runtime.c"),
                str(root / "components/audio_engine/audio_media.c"), "-o", str(unit)],
                check=True, stdout=log, stderr=subprocess.STDOUT)
            subprocess.run(network_command + flags + [str(p) for p in network_sources] + ["-o", str(network)],
                check=True, stdout=log, stderr=subprocess.STDOUT)
    checks = {}
    for label, arguments in [("runtime", [str(unit)]), ("creation-failure", [str(unit), "task-failure"]),
                             ("network", [str(network)])]:
        if name == "default" and label in ("runtime", "network"):
            outcome = run if label == "runtime" else network_run
        else:
            outcome = subprocess.run(arguments, env=dict(os.environ, UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1"),
                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20)
            (directory / (name + "-" + label + ".log")).write_text(outcome.stdout)
            print(outcome.stdout, end="")
        checks[label] = outcome.returncode
        if outcome.returncode:
            raise SystemExit(outcome.returncode)
    variants[name] = {"checks": checks, "external_stack": bool(expected),
        "runtime_binary_sha256": hashlib.sha256(unit.read_bytes()).hexdigest(),
        "network_binary_sha256": hashlib.sha256(network.read_bytes()).hexdigest()}
after = {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in files}
(directory / "manifest.json").write_text(json.dumps({"source_before": before, "source_after": after,
    "source_stable": before == after, "exit": run.returncode, "network_exit": network_run.returncode,
    "variants": variants,
    "network_binary_sha256": hashlib.sha256(network_binary.read_bytes()).hexdigest(),
    "binary_sha256": hashlib.sha256(Path(binary).read_bytes()).hexdigest()}, indent=2) + "\n")
assert before == after, "Sources changed during validation"
if run.returncode:
    raise SystemExit(run.returncode)
print(f"Evidence: {directory}")
