#!/usr/bin/env python3
"""Exercise the real Passport protocol/remote owner/HID chain under sanitizers."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
sources = ["firmware/main/app_remote_input.c",
           "components/input_core/input_core.c", "components/hid_engine/hid_engine.c",
           "components/interaction/interaction.c", "components/control_protocol/control_protocol.c",
           "components/control_protocol/control_json.c", "components/control_protocol/control_registry.c",
           "components/control_protocol/control_sha256.c", "passport/tests/test_control.c"]
with tempfile.TemporaryDirectory(prefix="passport-control-") as directory:
    command = [os.environ.get("CC", "cc"), "-std=c11", "-D_POSIX_C_SOURCE=200809L",
               "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
               "-DCP_PASSPORT_BROWSER_LEASE_MS=30000U", "-DCP_MAX_RESULTS=4U", "-DCP_MAX_ACTIVE_REQUESTS=3U", "-DCP_MOTION_WINDOW_CONFIG_MAX=3U", "-DCJ_CAPACITY=4096U",
               "-DCJ_TOKENS=192U", "-DAPP_REMOTE_INPUT_JOBS=3U", "-DAPP_REMOTE_INPUT_RECENT=4U", "-DTEST_CONTROL_ALLOCATIONS=1"]
    for path in ["passport/main", "firmware/main", "components/control_protocol"] + [
            f"components/{component}/include" for component in ["input_core", "hid_engine", "interaction", "control_protocol"]]:
        command += ["-I", str(root / path)]
    binary = str(Path(directory) / "control")
    owner = str(Path(directory) / "owner.o")
    subprocess.run(command + ["-Dcalloc=fixture_calloc", "-Daligned_alloc=fixture_aligned_alloc",
                             "-Dfree=fixture_free", "-c", str(root / "passport/main/passport_control.c"),
                             "-o", owner], check=True)
    subprocess.run(command + [str(root / p) for p in sources] + [owner, "-lm", "-o", binary], check=True)
    subprocess.run([binary], check=True)
