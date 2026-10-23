#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise production bounded telemetry framing without device I/O."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]

def main():
    cc = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
    with tempfile.TemporaryDirectory(prefix="sensor-usb-") as temporary:
        binary = Path(temporary) / "usb"
        module = ROOT / "components/control_protocol"
        subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
            "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer", "-g",
            "-I" + str(module / "include"), str(ROOT / "tests/control_runtime/test_sensor_usb.c"),
            *[str(module / name) for name in ("control_sensor_usb.c", "control_protocol.c", "control_json.c", "control_registry.c", "control_sha256.c")],
            "-lm", "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
        source = (ROOT / "firmware/main/app_control_runtime.c").read_text()
        fixture = (ROOT / "tests/control_runtime/test_usb_mux.c").read_text()
        def section(first, after):
            start = source.index(first)
            return source[start:source.index(after, start)]
        code = fixture.split("/* TYPES */")[0]
        code += section("#define INBOX_CAPACITY", "static uint64_t now_ms")
        code += fixture.split("/* PORTS */")[1].split("/* PRODUCTION */")[0]
        code += section("static void hex(", "static cp_error_t read_resource(")
        code += section("static void usb_reply(", "static void worker(")
        code += fixture.split("/* TESTS */")[1]
        test = Path(temporary) / "mux.c"
        test.write_text(code)
        command = [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
            "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer", "-g", "-pthread"]
        for path in ("tests/audio_engine/stubs", "tests/control_runtime/stubs", "firmware/main",
                     "components/control_protocol/include", "components/audio_engine/include",
                     "components/device_settings/include", "components/spoken_letters/include"):
            command.append("-I" + str(ROOT / path))
        command += [str(test), *[str(module / name) for name in ("control_sensor_usb.c", "control_protocol.c", "control_json.c", "control_registry.c", "control_sha256.c")], "-lm", "-o", str(binary)]
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True)

if __name__ == "__main__":
    main()
