"""Supply public checkout metadata and the ESP app descriptor for native UI tests."""
import json
import subprocess
import sys


def prepare_ui_build_info(root, headers):
    subprocess.run([sys.executable, str(root / 'tools/build_identity.py'), '--root', str(root),
                    '--output', str(headers / 'app_build_identity.h')], check=True)
    version = (root / 'passport/version.txt').read_text().strip()
    (headers / 'esp_app_desc.h').write_text(
        '#pragma once\ntypedef struct { char version[32]; } esp_app_desc_t;\n'
        'static inline const esp_app_desc_t *esp_app_get_description(void) {\n'
        f'    static const esp_app_desc_t app = {{.version = {json.dumps(version[:31])}}};\n'
        '    return &app;\n}\n')
