#!/usr/bin/env python3
"""Fault-test production adapters at SDK/crypto/HTTP/NVS boundaries; no device I/O."""
from pathlib import Path
import os
import argparse
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SUITE = Path(__file__).resolve().parent
HEADERS = ["esp_err.h", "esp_heap_caps.h", "esp_random.h", "esp_timer.h", "esp_partition.h",
           "freertos/FreeRTOS.h", "freertos/semphr.h", "freertos/task.h", "mbedtls/pk.h",
           "mbedtls/x509_crt.h", "nvs.h", "nvs_flash.h", "esp_app_desc.h", "esp_http_client.h",
           "esp_ota_ops.h", "esp_system.h", "psa/crypto.h", "sdkconfig.h", "esp_netif_sntp.h"]

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case",choices=["trust","update","clock","server","work"])
    selected=parser.parse_args().case
    compiler = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
    with tempfile.TemporaryDirectory(prefix="control-adapters-") as directory:
        temporary = Path(directory)
        for header in HEADERS:
            path = temporary / header
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#include "control_test_sdk.h"\n')
        for name in ("trust", "update", "clock", "server"):
            if selected and selected!=name:
                continue
            binary = temporary / ("test_" + name)
            command = [compiler, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror",
                       "-pedantic", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g", "-pthread",
                       "-I" + str(temporary), "-I" + str(SUITE / "stubs"),
                       "-I" + str(ROOT / "components/control_protocol/include"),
                       "-I" + str(ROOT / "components/device_settings/include"),
                       "-I" + str(ROOT / "components/audio_engine/include"),
                       "-I" + str(ROOT / "components/spoken_letters/include"),
                       str(SUITE / ("test_" + name + ".c")),
                       str(ROOT / "components/control_protocol/control_json.c"),
                       str(ROOT / "components/control_protocol/control_sha256.c"),
                       str(ROOT / "components/control_protocol/control_registry.c"),
                       *([str(ROOT / "components/control_protocol/control_protocol.c")] if name=="server" else []), "-lm", "-o", str(binary)]
            subprocess.run(command, check=True)
            subprocess.run([str(binary)], check=True)
    if not selected or selected=="work":
        from run_work import main as run_work
        run_work()

if __name__ == "__main__":
    main()
