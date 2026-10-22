#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compile actual server owner task functions with named SDK lifetime boundaries."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[2]
SUITE=Path(__file__).resolve().parent

def main():
    source=(ROOT/'firmware/main/app_control_server.c').read_text()
    def section(first,after):
        start=source.index(first)
        return source[start:source.index(after,start)]
    fixture=(SUITE/'test_stack.c').read_text()
    code=fixture.split('/* TYPES */')[0]
    code+=section('#define CERT_BYTES','static portMUX_TYPE s_lock')
    code+=fixture.split('/* PORTS */')[1].split('/* OWNER */')[0]
    code+=section('static void cache_sessions(', 'bool app_control_server_session_alive(')
    code+=section('static void publish(', 'static bool random_source(')
    code+=section('static esp_err_t initialize(', 'esp_err_t app_control_server_start(')
    code+=section('esp_err_t app_control_server_start(', 'esp_err_t app_control_server_stop(')
    code+=fixture.split('/* TESTS */')[1]
    compiler=os.environ.get('CC') or shutil.which('clang') or shutil.which('cc')
    with tempfile.TemporaryDirectory(prefix='control-stack-') as directory:
        temporary=Path(directory)
        for header in ['sdkconfig.h','esp_heap_caps.h','freertos/FreeRTOS.h','freertos/task.h','freertos/idf_additions.h','esp_err.h']:
            path=temporary/header;path.parent.mkdir(parents=True,exist_ok=True);path.write_text('#include "control_test_sdk.h"\n')
        test=temporary/'test_stack.c';test.write_text(code)
        for name,defines in [('external',['-DCONFIG_SPIRAM=1','-DCONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM=1']),
                             ('internal-no-psram',['-DCONFIG_SPIRAM=0','-DCONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM=0']),
                             ('internal-no-external-stack',['-DCONFIG_SPIRAM=1','-DCONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM=0'])]:
            binary=temporary/name
            command=[compiler,'-std=c11','-D_DEFAULT_SOURCE','-Wall','-Wextra','-Werror','-pedantic','-g','-fsanitize=address,undefined', '-fno-sanitize-recover=all','-fno-omit-frame-pointer','-I'+str(temporary),'-I'+str(SUITE/'stubs'),'-I'+str(ROOT/'firmware/main'),'-I'+str(ROOT/'components/control_protocol/include'),str(test)]
            command+=[str(ROOT/'components/control_protocol'/p) for p in ['control_protocol.c','control_json.c','control_registry.c','control_sha256.c']]
            subprocess.run(command+defines+['-lm','-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)

if __name__=='__main__':
    main()
