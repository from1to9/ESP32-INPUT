#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run the production runtime diagnostic encoder/dispatch at copied owner boundaries."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    source = (ROOT / "firmware/main/app_control_runtime.c").read_text()
    start = source.index("static cp_error_t read_resource(")
    dispatch = source[start:source.index("static bool u32(", start)]
    fixture = (ROOT / "tests/control_runtime/test_diagnostics.c").read_text()
    code = fixture.split("/* PRODUCTION */")[0] + dispatch + fixture.split("/* TESTS */")[1]
    with tempfile.TemporaryDirectory(prefix="runtime-diagnostics-") as directory:
        path = Path(directory) / "test.c"
        path.write_text(code)
        binary = path.with_suffix("")
        command = [os.environ.get("CC") or shutil.which("clang") or "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-Wconversion", "-Wshadow",
                   "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        for include in ("tests/coordinator/stubs", "firmware/main", "components/control_protocol/include", "components/control_protocol",
                        "components/audio_engine/include", "components/input_core/include", "components/device_settings/include", "components/spoken_letters/include"):
            command += ["-I", str(ROOT / include)]
        command += [str(path), str(ROOT / "firmware/main/app_runtime_diagnostics.c")]
        command += [str(ROOT / "components/control_protocol" / name) for name in ("control_protocol.c", "control_json.c", "control_registry.c", "control_sha256.c")]
        subprocess.run(command + ["-lm", "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True, env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0", UBSAN_OPTIONS="halt_on_error=1"))


if __name__ == "__main__":
    main()
