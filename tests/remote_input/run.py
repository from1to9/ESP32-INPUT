#!/usr/bin/env python3
"""Exercise canonical remote input against the real shared portable engines."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
cc = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
if not cc:
    raise SystemExit("A C11 compiler with address/undefined sanitizers is required.")
with tempfile.TemporaryDirectory(prefix="esp32-remote-input-") as directory:
    output = Path(directory) / "remote_input"
    command = [cc, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror",
               "-pedantic", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    for component in ["input_core", "hid_engine", "interaction", "control_protocol"]:
        command += ["-I", str(ROOT / "components" / component / "include")]
    command += ["-I", str(ROOT / "components/control_protocol"), "-I", str(ROOT / "firmware/main")]
    sources = ["firmware/main/app_remote_input.c", "components/input_core/input_core.c",
               "components/hid_engine/hid_engine.c", "components/interaction/interaction.c",
               "components/control_protocol/control_protocol.c", "components/control_protocol/control_json.c",
               "components/control_protocol/control_registry.c", "components/control_protocol/control_sha256.c",
               "tests/remote_input/test_remote_input.c"]
    command += [str(ROOT / p) for p in sources] + ["-lm", "-o", str(output)]
    subprocess.run(command, check=True)
    subprocess.run([str(output)], check=True)
