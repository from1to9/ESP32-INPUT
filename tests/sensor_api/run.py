#!/usr/bin/env python3
"""Exercise production sensor API/codec with cached board and RTOS boundaries."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="esp32-sensor-api-") as temp:
    out = Path(temp)
    config = out / "sdkconfig"
    config.write_text("CONFIG_APP_IMU_CAPTURE=y\n")
    subprocess.run([sys.executable, str(ROOT / "tools/build_identity.py"), "--root", str(ROOT),
                    "--output", str(out / "app_build_identity.h"), "--config", str(config)], check=True)
    compiler = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
    if not compiler:
        raise SystemExit("A C11 compiler with sanitizers is required")
    command = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-Wconversion",
               "-Wshadow", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    for path in (out, ROOT / "tests/coordinator/stubs", ROOT / "firmware/main",
                 ROOT / "components/control_protocol/include", ROOT / "components/control_protocol"):
        command += ["-I", str(path)]
    command += [str(ROOT / name) for name in (
        "tests/sensor_api/test_sensor_api.c", "firmware/main/sensor_wire.c",
        "firmware/main/board_sensor_journal.c", "components/control_protocol/control_json.c")]
    command += ["-lm", "-o", str(out / "sensor-api")]
    subprocess.run(command, check=True)
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0", UBSAN_OPTIONS="halt_on_error=1")
    subprocess.run([str(out / "sensor-api")], check=True, env=env)
