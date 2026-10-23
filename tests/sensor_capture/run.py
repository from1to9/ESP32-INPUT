#!/usr/bin/env python3
"""Build real sensor/USB codecs; run collector tests with named I/O substitutes."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
compiler = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
if not compiler:
    raise SystemExit("A C11 compiler with sanitizers is required")
with tempfile.TemporaryDirectory(prefix="esp32-sensor-collector-") as directory:
    out = Path(directory)
    config = out / "sdkconfig"
    config.write_text("CONFIG_APP_IMU_CAPTURE=y\n")
    subprocess.run([sys.executable, str(ROOT / "tools/build_identity.py"), "--root", str(ROOT),
        "--output", str(out / "app_build_identity.h"), "--config", str(config)], check=True)
    command = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-Wconversion", "-Wshadow",
        "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    for include in [out, ROOT / "tests/coordinator/stubs", ROOT / "firmware/main",
                    ROOT / "components/control_protocol/include", ROOT / "components/control_protocol"]:
        command += ["-I", str(include)]
    command += [str(ROOT / name) for name in ["tests/sensor_capture/fixture.c", "firmware/main/sensor_wire.c",
        "firmware/main/board_sensor_journal.c", "components/control_protocol/control_json.c",
        "components/control_protocol/control_sensor_usb.c", "components/control_protocol/control_sha256.c"]]
    command += ["-lm", "-o", str(out / "fixture")]
    subprocess.run(command, check=True)
    archive = command[:-1] + [str(out / "archive-fixture"), "-DFIXTURE_ARCHIVE=1"]
    subprocess.run(archive, check=True)
    env = dict(os.environ, SENSOR_FIXTURE=str(out / "fixture"), SENSOR_ARCHIVE_FIXTURE=str(out / "archive-fixture"),
        ASAN_OPTIONS="detect_leaks=0", UBSAN_OPTIONS="halt_on_error=1")
    interpreter = ROOT / ".tools/control-bridge-venv/bin/python"
    subprocess.run([str(interpreter) if interpreter.exists() else sys.executable,
        str(ROOT / "tests/sensor_capture/test_collector.py")], check=True, env=env)
