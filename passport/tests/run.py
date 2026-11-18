#!/usr/bin/env python3
"""Run production Passport logic with substituted SDK/BSP boundaries."""
from pathlib import Path
import importlib.util
import os
import re
import subprocess
import tempfile
import sys
from unittest.mock import patch

from build_info import prepare_ui_build_info

root = Path(__file__).resolve().parents[2]
headers = (
    'bootloader_random.h esp_check.h esp_event.h esp_heap_caps.h esp_http_server.h '
    'esp_netif.h esp_random.h esp_wifi.h esp_wifi_default.h esp_err.h '
    'freertos/FreeRTOS.h freertos/task.h freertos/queue.h host/ble_hs.h '
    'nimble/nimble_port.h nvs_flash.h bsp_button.h bsp_display.h bsp_pins.h '
    'esp_log.h esp_timer.h lvgl.h'
).split()
def sdk_function(source, name):
    match = re.search(r'(?m)^(?:static )?(?:int|void|esp_err_t|uint32_t|bool|size_t|ssize_t|hid_engine_report_result_t)\s+' +
                      re.escape(name) + r'\([^;{]*\)\s*\{', source)
    if not match:
        raise RuntimeError(f'Missing pinned SDK function: {name}')
    start = source.index('{', match.start())
    depth = 1
    end = start + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end] + '\n'


tests = ('startup', 'ble', 'store', 'cccd', 'theme', 'wifi', 'setup', 'sound', 'names', 'control_http', 'management', 'usb')
fixtures = {
    'startup': 'stubs.h', 'theme': 'theme_stubs.h', 'wifi': 'wifi_stubs.h',
    'setup': 'setup_stubs.h', 'sound': 'sound_stubs.h', 'names': 'names_stubs.h',
    'control_http': 'control_http_stubs.h', 'management': 'stubs.h',
    'usb': 'usb_stubs.h',
}
for test in sys.argv[1:] or tests:
    if test not in tests:
        raise SystemExit(f'Unknown suite: {test}')
    with tempfile.TemporaryDirectory(prefix='passport-test-') as directory:
        work = Path(directory)
        extra = 'host/ble_sm.h services/gap/ble_svc_gap.h services/gatt/ble_svc_gatt.h nvs.h store/config/ble_store_config.h freertos/semphr.h'.split()
        if test == 'sound':
            extra += ['bsp_i2c.h', 'driver/i2s_std.h']
        if test == 'usb':
            extra += ['driver/usb_serial_jtag.h', 'esp_app_desc.h']
            sdk = root / '.tools/passport-esp-idf/components/esp_driver_usb_serial_jtag/src'
            driver = (sdk / 'usb_serial_jtag.c').read_text()
            vfs = (sdk / 'usb_serial_jtag_vfs.c').read_text()
            (work / 'sdk_usb_rx.inc').write_text(
                ''.join(sdk_function(driver, name) for name in
                        ('usb_serial_jtag_is_driver_installed', 'usb_serial_jtag_get_read_bytes_available')) +
                ''.join(sdk_function(vfs, name) for name in
                        ('usb_serial_jtag_rx_char_no_driver', 'usb_serial_jtag_read_char',
                         'usb_serial_jtag_return_char', 'usb_serial_jtag_read', 'usb_serial_jtag_fcntl')))
        if test == 'names':
            extra.append('mbedtls/md.h')
        for header in headers + extra:
            path = work / header
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(f'#include "{fixtures.get(test, "ble_stubs.h")}"\n')
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
        if test == 'store':
            sdk = root / '.tools/passport-esp-idf/components/bt/host/nimble/nimble/nimble/host'
            (work / 'sdk_store_init.inc').write_text(sdk_function(
                (sdk / 'store/config/src/ble_store_config.c').read_text(), 'ble_store_config_init'))
            (work / 'sdk_store_deinit.inc').write_text(sdk_function(
                (sdk / 'store/config/src/ble_store_config.c').read_text(), 'ble_store_config_deinit'))
            (work / 'sdk_default_irk.inc').write_text(sdk_function(
                (sdk / 'src/ble_hs_pvcy.c').read_text(), 'ble_hs_pvcy_set_default_irk'))
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
        if test == 'control_http':
            (work / 'lwip').mkdir()
            (work / 'lwip/sockets.h').write_text('#include "control_http_stubs.h"\n')
            (work / 'ble_submit.inc').write_text(sdk_function(
                (root / 'passport/main/passport_ble.c').read_text(), 'send_report'))
            sdk_ws = (root / '.tools/passport-esp-idf/components/esp_http_server/src/httpd_ws.c').read_text()
            # Keep the actual length/header and payload semantics; substitute socket I/O only.
            (work / 'sdk_ws_receive.inc').write_text(''.join(sdk_function(sdk_ws, name)
                for name in ('httpd_ws_unmask_payload', 'httpd_ws_recv_frame')))
        if test in ('ble', 'control_http', 'management'):
            sources += [root / 'components/hid_engine/hid_engine.c', root / 'components/input_core/input_core.c']
            sources += [root / p for p in ('passport/main/passport_control.c', 'firmware/main/app_remote_input.c',
                'components/interaction/interaction.c', 'components/control_protocol/control_protocol.c',
                'components/control_protocol/control_json.c', 'components/control_protocol/control_registry.c',
                'components/control_protocol/control_sha256.c')]
            defines += ['-D_POSIX_C_SOURCE=200809L', '-DCP_PASSPORT_BROWSER_LEASE_MS=30000U', '-DCP_MAX_RESULTS=4U', '-DCP_MAX_ACTIVE_REQUESTS=3U', '-DCP_MOTION_WINDOW_CONFIG_MAX=3U',
                        '-DCJ_CAPACITY=4096U', '-DCJ_TOKENS=192U', '-DAPP_REMOTE_INPUT_JOBS=3U', '-DAPP_REMOTE_INPUT_RECENT=4U']
        if test == 'management':
            sources += [root / 'passport/main/passport_management.c', root / 'passport/main/passport_wifi_text.c']
        if test == 'sound':
            codec = root / 'passport/managed_components/espressif__esp_codec_dev'
            for directory in ('device/include', 'device/priv_include', 'include', 'interface'):
                defines += ['-I', str(codec / directory)]
            driver = work / 'es8311_host.c'
            driver.write_text('#include <stdlib.h>\n#pragma GCC diagnostic ignored "-Wsign-compare"\n'
                              f'#include "{codec / "device/es8311/es8311.c"}"\n')
            sources += [driver, codec / 'esp_codec_dev_if.c', codec / 'esp_codec_dev_vol.c']
        if test in ('startup', 'usb'):
            sources += [root / 'passport/main/passport_wifi_text.c']
        if test == 'startup':
            prepare_ui_build_info(root, work)
            sys.path.insert(0, str(root / 'tools'))
            from passport_button_patch import patch_source
            button_dir = root / 'passport/managed_components/espressif__button'
            source = patch_source((button_dir / 'iot_button.c').read_text())
            declarations = source[source.index('enum {\n    PRESS_DOWN_CHECK'):source.index('//button handle list head.')]
            macros = source[source.index('#define TICKS_INTERVAL'):source.index('/**\n  * @brief  Button driver core')]
            (work / 'sdkconfig.h').write_text('#include "stubs.h"\n')
            defines += ['-I', str(button_dir / 'include'), '-I', str(button_dir / 'interface'),
                        '-DCONFIG_BUTTON_DEBOUNCE_TICKS=2', '-DCONFIG_BUTTON_LONG_PRESS_HOLD_SERIAL_TIME_MS=20']
            # The actual pinned state machine drives production board/UI code.
            # Only key-level/time/queue SDK boundaries are substituted. Suppress
            # the upstream signedness warnings inside the extracted code only.
            (work / 'sdk_button.inc').write_text(
                '#include <stdlib.h>\n#include "iot_button.h"\n'
                '#pragma GCC diagnostic push\n#pragma GCC diagnostic ignored "-Wsign-compare"\n'
                + declarations + macros + '#define BTN_CHECK(a, message, result) assert(a)\n'
                + sdk_function(source, 'iot_button_get_pressed_time')
                + sdk_function(source, 'button_handler')
                + '#undef BTN_CHECK\n#pragma GCC diagnostic pop\n')
        if test in ('startup', 'ble', 'store', 'setup'):
            defines += ['-DCONFIG_PASSPORT_DIAGNOSTICS=1']
        binary = work / f'test_{test}'
        subprocess.run([
            os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
            '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
            '-I', str(work), '-I', str(root / 'passport/tests'), '-I', str(root / 'passport/main'),
            '-I', str(root / 'components/hid_engine/include'), '-I', str(root / 'components/input_core/include'),
            '-I', str(root / 'components/control_protocol/include'), '-I', str(root / 'components/control_protocol'),
            '-I', str(root / 'components/interaction/include'), '-I', str(root / 'firmware/main'),
            *defines, *map(str, sources), '-lm', '-o', str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)
        if test == 'usb':
            spec = importlib.util.spec_from_file_location('passport_usb', root / 'tools/passport_usb.py')
            usb = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(usb)

            class CapturedPort:
                def __init__(self, data):
                    self.data = data

                def read(self, size):
                    chunk, self.data = self.data[:size], self.data[size:]
                    return chunk

                def write(self, data):
                    return len(data)

            port = CapturedPort(subprocess.check_output([str(binary), '--status-fixtures']))
            client = usb.Client(port, timeout=0.1)
            with patch.object(usb.secrets, 'randbits', return_value=88):
                assert client.hello()['result'] == 'applied'
            phases = []
            for phase in range(5):
                result = client.command('status')
                assert result['result'] == 'applied' and len(result['data']) == 9
                wifi = next(row for row in result['data'] if row['type'] == 'wifi')
                phases.append(wifi['phase'])
                assert wifi['sta_ip'] == ('192.0.2.10' if phase == 4 else '-')
            assert phases == list(map(str, range(5)))
            assert not port.data
            print('PASS: production C status frames decoded by host Client in every Wi-Fi phase')
