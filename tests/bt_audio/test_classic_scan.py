#!/usr/bin/env python3
"""Run Classic scan coordination with the actual generated BTU send and Complete bodies."""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SUITE = Path(__file__).resolve().parent
STUBS = {
    'sdkconfig.h': '#define CONFIG_BT_CTRL_PINNED_TO_CORE 0\n',
    'esp_cpu.h': 'int esp_cpu_get_core_id(void);\n',
    'freertos/FreeRTOS.h': '#include "scan_stubs.h"\n',
    'freertos/semphr.h': '#include "scan_stubs.h"\n',
    'hci/hci_layer.h': '#include "scan_stubs.h"\n',
    'osi/thread.h': '#include "scan_stubs.h"\n',
    'stack/btm_api.h': '#include "scan_stubs.h"\n',
    'stack/btu.h': '#include "scan_stubs.h"\n',
    'stack/hcimsgs.h': '#include "scan_stubs.h"\n',
}


def function(source, signature):
    return re.search(re.escape(signature) + r'[^;{]*\{.*?\n\}', source, re.S)[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', type=Path, default=ROOT / '.tools/esp-idf')
    parser.add_argument('--app-source', type=Path, default=ROOT / 'firmware/main/bt_audio.c')
    parser.add_argument('--owner-only', action='store_true')
    args = parser.parse_args()
    sdk = args.sdk / 'components/bt/host/bluedroid'
    compiler = shutil.which('clang') or 'cc'
    with tempfile.TemporaryDirectory(prefix='esp32-classic-scan-') as directory:
        tmp = Path(directory)
        owner = tmp / 'owner'
        legacy = []
        if 'void bt_audio_classic_scan_result(' not in args.app_source.read_text():
            # The original owner has no result boundary. A call here is a test error.
            shim = tmp / 'legacy.c'
            shim.write_text('''#include <stdlib.h>
#include "bt_classic_scan.h"
void bt_audio_classic_scan_result(uint64_t id, esp_err_t status) { (void)id; (void)status; abort(); }
void bt_audio_classic_scan_drained(uint64_t id, bool restore) { (void)id; (void)restore; abort(); }
''')
            legacy = [str(shim)]
        subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                        '-DTEST_BT_AUDIO_LOCKS', '-DCONFIG_APP_CLASSIC_SCAN_COORDINATION=1',
                        '-I', str(SUITE / 'stubs'), '-I', str(ROOT / 'firmware/main'),
                        '-I', str(ROOT / 'components/audio_engine/include'),
                        '-I', str(ROOT / 'components/device_settings/include'),
                        str(SUITE / 'test_bt_audio.c'), str(args.app_source),
                        str(ROOT / 'firmware/main/bt_audio_btc.c'), *legacy, '-o', str(owner)], check=True)
        for case in ('scan-owner', 'scan-cancel', 'scan-timeout', 'scan-ready-expired', 'scan-queue-full',
                     'forget-empty', 'forget-sole', 'forget-other', 'forget-rekey', 'forget-acl',
                     'forget-expired', 'forget-nvs-error', 'forget-multiblob', 'forget-storage-timeout'):
            subprocess.run([str(owner), case], check=True)
        if args.owner_only:
            return
        for path in ('stack/btu/btu_hcif.c', 'stack/btu/btu_init.c', 'bta/dm/bta_dm_act.c'):
            shutil.copy(sdk / path, tmp)
        subprocess.run(['git', 'apply', '--no-index', str(ROOT / 'patches/esp-idf-classic-scan-coordination.patch')],
                       cwd=tmp, check=True)
        source = (tmp / 'btu_hcif.c').read_text()
        bodies = [function(source, signature) for signature in
                  ('void btu_hcif_send_cmd (', 'static void btu_hcif_command_complete_evt_on_task(')]
        (tmp / 'scan_btu.inc').write_text('\n\n'.join(bodies))
        for name, text in STUBS.items():
            path = tmp / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
        binary = tmp / 'scan'
        command = [compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                   '-I', str(tmp), '-I', str(SUITE), '-I', str(ROOT / 'firmware/main'),
                   '-I', str(SUITE / 'stubs'), str(SUITE / 'classic_scan_fixture.c'), '-o', str(binary)]
        for observer in (0, 1):
            subprocess.run(command + [f'-DCONFIG_APP_CLASSIC_CONNECT_DIAGNOSTICS={observer}'], check=True)
            for case in ('drain', 'async', 'async-before-ack', 'no-scan', 'incoming', 'reuse', 'cancel',
                         'ack-failed', 'policy-reply-warning', 'alloc-failed', 'queue-full', 'async-queue-full', 'teardown', 'reenable',
                         'unsupported-context', 'forget', 'forget-released', 'forget-late-acl'):
                subprocess.run([str(binary), case], check=True)


if __name__ == '__main__':
    main()
