#!/usr/bin/env python3
"""Run the pinned Classic connection-failure path with synthetic link owners."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

from test_classic_scan import function

SUITE = Path(__file__).resolve().parent
ROOT = SUITE.parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', type=Path, default=ROOT / '.tools/esp-idf')
    args = parser.parse_args()
    l2cap = args.sdk / 'components/bt/host/bluedroid/stack/l2cap'
    with tempfile.TemporaryDirectory(prefix='esp32-classic-connect-') as directory:
        scratch = Path(directory)
        utils = scratch / 'l2c_utils.c'
        shutil.copy(l2cap / utils.name, utils)
        patch = ROOT / 'patches/esp-idf-classic-connect-capacity.patch'
        subprocess.run(['git', 'apply', '--no-index', str(patch)], cwd=scratch, check=True)
        (scratch / 'classic_connect_capacity_sdk.inc').write_text('\n\n'.join((
            function(utils.read_text(), 'BOOLEAN l2cu_lcb_disconnecting ('),
            function((l2cap / 'l2c_link.c').read_text(), 'BOOLEAN l2c_link_hci_conn_comp ('),
        )))
        compiler = shutil.which('clang') or 'cc'
        executable = scratch / 'classic_connect_capacity'
        subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                        '-I', directory, str(SUITE / 'classic_connect_capacity_fixture.c'), '-o', str(executable)],
                       check=True)
        for case in ('no-other', 'br-busy', 'br-draining', 'br-idle', 'br-last-closing',
                     'br-connecting-empty', 'le-fixed-only', 'le-draining'):
            subprocess.run([str(executable), case], check=True)


if __name__ == '__main__':
    main()
