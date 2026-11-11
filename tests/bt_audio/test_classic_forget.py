#!/usr/bin/env python3
"""Run the generated Classic forget boundaries with synthetic BTM and NVS owners."""
from pathlib import Path
import argparse
import shutil
import subprocess
import tempfile

from test_classic_scan import function

ROOT = Path(__file__).resolve().parents[2]
SUITE = Path(__file__).resolve().parent
HCI_SOURCE = 'host/bluedroid/stack/btu/btu_hcif.c'
SOURCES = {
    'common/osi/config.c': ('bool bt_classic_forget_fits_single_blob(',
                            'esp_err_t bt_classic_forget_check_single_blob('),
    'host/bluedroid/btc/core/btc_config.c': ('esp_err_t bt_classic_forget_config_check(',
                                            'bool bt_classic_forget_config_save('),
    'host/bluedroid/btc/core/btc_storage.c': ('static bool classic_identity_locked(',
                                             'static void classic_address_string(',
                                             'esp_err_t bt_classic_forget_review(',
                                             'esp_err_t bt_classic_forget_storage('),
    'host/bluedroid/stack/btm/btm_dev.c': ('esp_err_t bt_classic_forget_runtime(',),
}


def acl_retirement_source(bt, hci):
    stack = bt / 'host/bluedroid/stack'
    link = (stack / 'l2cap/l2c_link.c').read_text()
    utils = (stack / 'l2cap/l2c_utils.c').read_text()
    acl = (stack / 'btm/btm_acl.c').read_text()
    # Reuse the owning ACL-retirement predicate in l2cu_release_lcb.
    # Buffers, timers and other SDK callbacks stay at the fixture boundary.
    start = utils.index('    /* Tell BTM Acl management the link was removed */')
    end = utils.index('    /* Release any held buffers */', start)
    return '\n\n'.join((
        function(acl, 'static BOOLEAN btm_get_acl_db('),
        function(acl, 'tACL_CONN *btm_bda_to_acl ('),
        function(acl, 'tACL_CONN *btm_handle_to_acl ('),
        function(acl, 'BOOLEAN BTM_IsAclConnectionUp ('),
        'static void l2cu_release_lcb(tL2C_LCB *p_lcb)\n{\n' + utils[start:end] + '    lcb_present = false;\n}',
        function(link, 'BOOLEAN l2c_link_hci_disc_comp ('),
        function(link, 'void l2c_link_timeout ('),
        function(hci, 'static void btu_hcif_disconnection_comp_evt ('),
    ))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', type=Path, default=ROOT / '.tools/esp-idf')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='esp32-classic-forget-') as directory:
        scratch = Path(directory)
        bt = args.sdk / 'components/bt'
        for name in (*SOURCES, HCI_SOURCE):
            shutil.copy(bt / name, scratch)
        subprocess.run(['git', 'apply', '--no-index', str(ROOT / 'patches/esp-idf-classic-forget.patch')],
                       cwd=scratch, check=True)
        snippets = []
        for name, signatures in SOURCES.items():
            source = (scratch / Path(name).name).read_text()
            snippets.extend(function(source, signature) for signature in signatures)
        (scratch / 'classic_forget_sdk.inc').write_text('\n\n'.join(snippets))
        executable = scratch / 'classic_forget'
        compiler = shutil.which('clang') or 'cc'
        subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                        '-I', str(scratch), '-I', str(SUITE / 'stubs'), '-I', str(ROOT / 'firmware/main'),
                        str(SUITE / 'classic_forget_fixture.c'), '-o', str(executable)], check=True)
        subprocess.run([str(executable)], check=True)
        hci = (scratch / Path(HCI_SOURCE).name).read_text()
        (scratch / 'classic_acl_retirement_sdk.inc').write_text(acl_retirement_source(bt, hci))
        executable = scratch / 'classic_acl_retirement'
        subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                        '-I', str(scratch), str(SUITE / 'classic_acl_retirement_fixture.c'), '-o', str(executable)],
                       check=True)
        for case in ('no-retry', 'retry-rejected', 'pending', 'failed-receipt', 'synthetic', 'le-complete'):
            subprocess.run([str(executable), case], check=True)


if __name__ == '__main__':
    main()
