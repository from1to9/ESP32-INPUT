#!/usr/bin/env python3
"""Run production Passport logic with substituted SDK/BSP boundaries."""
from pathlib import Path
import os
import re
import subprocess
import tempfile
import sys

root = Path(__file__).resolve().parents[2]
headers = (
    'bootloader_random.h esp_check.h esp_event.h esp_heap_caps.h esp_http_server.h '
    'esp_netif.h esp_random.h esp_wifi.h esp_wifi_default.h esp_err.h '
    'freertos/FreeRTOS.h freertos/task.h freertos/queue.h host/ble_hs.h '
    'nimble/nimble_port.h nvs_flash.h bsp_button.h bsp_display.h bsp_pins.h '
    'esp_log.h esp_timer.h lvgl.h'
).split()
def sdk_function(source, name):
    match = re.search(r'(?m)^(?:static )?(?:int|void)\n' + re.escape(name) + r'\(', source)
    if not match:
        raise RuntimeError(f'Missing pinned SDK function: {name}')
    start = source.index('{', match.start())
    depth = 1
    end = start + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end] + '\n'


tests = ('startup', 'ble', 'store', 'cccd', 'theme', 'wifi', 'setup')
for test in sys.argv[1:] or tests:
    if test not in tests:
        raise SystemExit(f'Unknown suite: {test}')
    with tempfile.TemporaryDirectory(prefix='passport-test-') as directory:
        work = Path(directory)
        extra = 'host/ble_sm.h services/gap/ble_svc_gap.h services/gatt/ble_svc_gatt.h nvs.h store/config/ble_store_config.h'.split()
        for header in headers + extra:
            path = work / header
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#include "' + ('stubs.h' if test == 'startup' else 'theme_stubs.h' if test == 'theme' else 'setup_stubs.h' if test == 'setup' else 'wifi_stubs.h' if test == 'wifi' else 'ble_stubs.h') + '"\n')
        defines = []
        if test == 'cccd':
            sdk = root / '.tools/passport-esp-idf/components/bt/host/nimble/nimble/nimble/host'
            source = (sdk / 'store/config/src/ble_store_config.c').read_text()
            keys = (sdk / 'src/ble_store.c').read_text()
            (work / 'sdk_cccd.inc').write_text(
                sdk_function(keys, 'ble_store_key_from_value_cccd') +
                ''.join(sdk_function(source, 'ble_store_config_' + name + '_cccd')
                        for name in ('find', 'read', 'write')))
            defaults = (root / 'passport/sdkconfig.defaults').read_text()
            capacity = re.search(r'^CONFIG_BT_NIMBLE_MAX_CCCDS=(\d+)$', defaults, re.M)
            if not capacity:
                raise RuntimeError('CCCD capacity must be explicit for this HID profile')
            defines = [f'-DTEST_BLE_STORE_MAX_CCCDS={capacity.group(1)}']
        sources = [root / f'passport/tests/test_{test}.c']
        if test == 'setup':
            sdk_json = root / '.tools/passport-esp-idf/components/json/cJSON'
            (work / 'cJSON.h').write_text((sdk_json / 'cJSON.h').read_text())
            (work / 'lwip').mkdir()
            (work / 'lwip/sockets.h').write_text('#include "setup_stubs.h"\n')
            # Compile the pinned third-party parser unchanged. Its legacy sprintf
            # triggers macOS deprecation warnings; first-party files retain -Werror.
            parser = work / 'cJSON.o'
            subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-fsanitize=address,undefined',
                            '-fno-omit-frame-pointer', '-c', str(sdk_json / 'cJSON.c'), '-o', str(parser)], check=True)
            sources += [parser]
        if test == 'ble':
            sources += [root / 'components/hid_engine/hid_engine.c', root / 'components/input_core/input_core.c']
        binary = work / f'test_{test}'
        subprocess.run([
            os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
            '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
            '-I', str(work), '-I', str(root / 'passport/tests'),
            '-I', str(root / 'components/hid_engine/include'), '-I', str(root / 'components/input_core/include'),
            *defines, *map(str, sources), '-o', str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)
