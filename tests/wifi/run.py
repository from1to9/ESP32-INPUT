#!/usr/bin/env python3
"""Run production Wi-Fi service fault tests at the SDK/network/storage boundary."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SUITE = Path(__file__).resolve().parent
HEADERS = ["esp_err.h", "sdkconfig.h", "soc/soc_caps.h", "esp_event.h",
           "esp_http_server.h", "esp_netif.h", "esp_random.h", "esp_timer.h",
           "esp_wifi.h", "esp_heap_caps.h", "freertos/FreeRTOS.h", "freertos/task.h", "nvs.h"]


def main():
    compiler = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
    if not compiler:
        raise SystemExit("A C11 compiler with address/undefined sanitizers is required.")
    with tempfile.TemporaryDirectory(prefix="esp32-wifi-tests-") as directory:
        temporary = Path(directory)
        for header in HEADERS:
            path = temporary / header
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#include "wifi_test_sdk.h"\n')
        binary = temporary / "test_wifi"
        subprocess.run([compiler, "-std=c11", "-D_DEFAULT_SOURCE", "-Wall", "-Wextra",
                        "-Werror", "-pedantic", "-g", "-fsanitize=address,undefined",
                        "-fno-omit-frame-pointer", "-I", str(temporary), "-I",
                        str(SUITE / "stubs"), str(SUITE / "test_wifi.c"),
                        "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
        unavailable = temporary / "test_unavailable"
        subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                        "-DSOC_WIFI_SUPPORTED=0", "-I", str(temporary), "-I", str(SUITE / "stubs"),
                        str(SUITE / "test_unavailable.c"), "-o", str(unavailable)], check=True)
        subprocess.run([str(unavailable)], check=True)


if __name__ == "__main__":
    main()
