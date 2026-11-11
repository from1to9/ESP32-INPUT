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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', type=Path, default=ROOT / '.tools/esp-idf')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='esp32-classic-forget-') as directory:
        scratch = Path(directory)
        for name in SOURCES:
            shutil.copy(args.sdk / 'components/bt' / name, scratch)
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


if __name__ == '__main__':
    main()
