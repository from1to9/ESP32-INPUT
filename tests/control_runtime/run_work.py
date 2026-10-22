#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compile exact runtime mailbox functions with named native/session boundaries."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[2]

def main():
    source = (ROOT / 'firmware/main/app_control_runtime.c').read_text()
    def section(first, after):
        start = source.index(first)
        return source[start:source.index(after, start)]
    # The fixture replaces only native work/session/clock/completion ports.
    # Actual admission, invalidation and mailbox lifecycle are compiled below.
    fixture = (ROOT / 'tests/control_runtime/test_work.c').read_text()
    code = fixture.split('/* PRODUCTION */')[0]
    code += section('#define INBOX_CAPACITY', 'static uint64_t now_ms')
    code += fixture.split('/* BOUNDARIES */')[1].split('/* PRODUCTION */')[0]
    code += section('static void wipe(', 'static bool random_source(')
    code += section('static cp_error_t submit(', 'static void hex(')
    code += section('static cp_error_t queued_authority(', 'static void usb_reply(')
    code += section('esp_err_t app_control_runtime_init(', 'esp_err_t app_control_runtime_publish_state(')
    code += section('esp_err_t app_control_runtime_publish_state(', 'esp_err_t app_control_runtime_complete(')
    # Exercise the actual native reorder allocation/parse/cleanup branch with
    # only its Wi-Fi mutation port replaced. Other native operations are not
    # synthesized by this fixture.
    code += 'static esp_err_t production_reorder(const cp_command_t *c) {\n'
    code += section('        if (c->operation == CP_OP_WIFI_PROFILES_REORDER)', '    if (update_operation(').rsplit('    }', 1)[0]
    code += 'return ESP_ERR_INVALID_ARG;\n}\n'
    code += fixture.split('/* TESTS */')[1]
    compiler = os.environ.get('CC') or shutil.which('clang') or shutil.which('cc')
    with tempfile.TemporaryDirectory(prefix='control-work-') as directory:
        test = Path(directory) / 'work.c'
        test.write_text(code)
        binary = test.with_suffix('')
        command = [compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', '-pedantic',
                   '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer', '-pthread', '-g']
        for path in ['tests/audio_engine/stubs', 'tests/control_runtime/stubs', 'firmware/main', 'components/control_protocol/include',
                     'components/control_protocol', 'components/audio_engine/include',
                     'components/device_settings/include', 'components/spoken_letters/include']:
            command += ['-I' + str(ROOT / path)]
        command += [str(test)] + [str(ROOT / 'components/control_protocol' / path) for path in
                   ['control_protocol.c', 'control_json.c', 'control_sha256.c', 'control_registry.c']]
        subprocess.run(command + ['-lm', '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)

if __name__ == '__main__':
    main()
