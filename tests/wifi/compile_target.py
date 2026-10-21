#!/usr/bin/env python3
"""Compile the Wi-Fi translation unit without mutating the shared target build."""
import json
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    pin = json.loads((ROOT / "toolchain.lock.json").read_text())
    sdk = ROOT / ".tools/esp-idf"
    actual = subprocess.check_output(["git", "-C", str(sdk), "rev-parse", "HEAD"], text=True).strip()
    if actual != pin["idf_commit"]:
        raise SystemExit("SDK revision differs from toolchain.lock.json")
    database = ROOT / "firmware/build/compile_commands.json"
    if not database.exists():
        raise SystemExit("Prepare the normal pinned firmware build first; no device is needed.")
    entry = next(e for e in json.loads(database.read_text()) if e["file"].endswith("/main/main.c"))
    command = shlex.split(entry["command"])
    with tempfile.TemporaryDirectory(prefix="esp32-wifi-object-") as directory:
        for i, value in enumerate(command):
            if value == entry["file"]:
                command[i] = str(ROOT / "firmware/main/app_wifi.c")
            elif i > 0 and command[i - 1] == "-o":
                command[i] = str(Path(directory) / "app_wifi.o")
        command += ["-I" + str(sdk / "components/esp_http_server/include"),
                    "-I" + str(sdk / "components/http_parser"), "-Werror", "-Wextra"]
        result = subprocess.run(command, cwd=entry["directory"], text=True, capture_output=True)
        if result.returncode:
            raise SystemExit(result.stdout + result.stderr)
    print(f"PASS: Wi-Fi object compile; target={pin['target']}, SDK={actual}")
    print("This does not link the final firmware or qualify radio/browser behavior.")


if __name__ == "__main__":
    main()
