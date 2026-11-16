#!/usr/bin/env python3
"""Run Passport startup failure tests with SDK/BSP boundaries substituted."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
headers = (
    'bootloader_random.h esp_check.h esp_event.h esp_heap_caps.h esp_http_server.h '
    'esp_netif.h esp_random.h esp_wifi.h esp_wifi_default.h esp_err.h '
    'freertos/FreeRTOS.h freertos/task.h freertos/queue.h host/ble_hs.h '
    'nimble/nimble_port.h nvs_flash.h bsp_button.h bsp_display.h bsp_pins.h '
    'esp_log.h lvgl.h'
).split()
with tempfile.TemporaryDirectory(prefix='passport-test-') as directory:
    work = Path(directory)
    for header in headers:
        path = work / header
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('#include "stubs.h"\n')
    binary = work / 'test_startup'
    subprocess.run([
        os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
        '-I', str(work), '-I', str(root / 'passport/tests'),
        str(root / 'passport/tests/test_startup.c'), '-o', str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)
