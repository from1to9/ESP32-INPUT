#!/usr/bin/env python3
"""Exercise the closed-window decision through pinned BTM, BTA and BTC cleanup."""
import argparse
from pathlib import Path
import importlib.util
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SUITE = ROOT / 'tests/bt_audio'

spec = importlib.util.spec_from_file_location('classic_failure', ROOT / 'tests/ble_hid/test_sdk_classic_failure.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

SOURCES = {
    'stack/btm/btm_sec.c': ('static void btm_sec_change_pairing_state (', 'void BTM_ConfirmReqReply(',
                            'void btm_proc_sp_req_evt (', 'void btm_simple_pair_complete (',
                            'void btm_sec_auth_complete (', 'BOOLEAN btm_sec_disconnected (',
                            'void btm_sec_link_key_request ('),
    'bta/dm/bta_dm_act.c': ('void bta_dm_confirm(', 'static BOOLEAN bta_dm_remove_bredr_sec_dev_entry(',
                            'static UINT8 bta_dm_authentication_complete_cback('),
    'btc/core/btc_config.c': ('void btc_config_flush(',),
    'btc/core/btc_storage.c': ('bt_status_t btc_storage_add_bonded_device(',
                               'static bt_status_t _btc_storage_in_fetch_bonded_bt_device(',
                               'bt_status_t btc_storage_remove_bonded_device(',
                               'int btc_storage_get_num_bt_bond_devices(',
                               'bt_status_t btc_storage_get_bonded_bt_devices_list('),
    'api/esp_gap_bt_api.c': ('int esp_bt_gap_get_bond_device_num(', 'esp_err_t esp_bt_gap_get_bond_device_list('),
    'btc/core/btc_dm.c': ('static void btc_dm_auth_cmpl_evt (', 'static void btc_dm_sp_cfm_req_evt('),
}
TAGGED_FUNCTIONS = ('static BOOLEAN btm_closed_window_failed(', 'static void btm_confirm_req_reply(',
                    'void BTM_RejectClosedWindow(', 'void btm_closed_window_reply_complete(')
CASES = ('saved-ssp-denied', 'closed-while-queued', 'le-disconnect', 'auth-complete', 'invalid-key',
         'stale-reply', 'stale-completion', 'controller-refusal', 'unknown-ssp-denied',
         'open-window-replacement', 'saved-key-reuse')


def run():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', type=Path, default=ROOT / '.tools/esp-idf')
    parser.add_argument('--app-source', type=Path, default=ROOT / 'firmware/main/bt_audio.c')
    parser.add_argument('--case', choices=CASES)
    args = parser.parse_args()
    sdk = args.sdk / 'components/bt/host/bluedroid'
    btm = (sdk / 'stack/btm/btm_sec.c').read_text()
    tagged = 'void BTM_RejectClosedWindow(' in btm
    with tempfile.TemporaryDirectory() as directory:
        tmp = Path(directory)
        snippets = []
        for file, signatures in SOURCES.items():
            source = (sdk / file).read_text()
            if file == 'stack/btm/btm_sec.c' and tagged:
                signatures = signatures[:1] + TAGGED_FUNCTIONS + signatures[1:]
            for signature in signatures:
                snippets.append(module.function(source, signature))
        (tmp / 'sdk.inc').write_text('\n'.join(snippets))
        binary = tmp / 'classic_auth'
        compiler = shutil.which('clang') or 'cc'
        subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', '-DTEST_BT_AUDIO_LOCKS',
            f'-DTEST_TAGGED_CONFIRMATION={int(tagged)}', '-fsanitize=address,undefined',
            '-I', str(tmp), '-I', str(SUITE / 'stubs'), '-I', str(SUITE),
            '-I', str(ROOT / 'firmware/main'), '-I', str(ROOT / 'components/audio_engine/include'),
            '-I', str(ROOT / 'components/device_settings/include'),
            str(SUITE / 'classic_closed_window.c'), str(args.app_source),
            str(ROOT / 'firmware/main/bt_audio_btc.c'), '-o', str(binary)], check=True)
        for case in (args.case,) if args.case else CASES:
            subprocess.run([str(binary), case], check=True)

if __name__ == '__main__':
    run()
