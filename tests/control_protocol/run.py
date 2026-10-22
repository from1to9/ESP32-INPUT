#!/usr/bin/env python3
"""Compile and exercise the actual portable protocol, without hardware or listeners."""
from pathlib import Path
import os,shutil,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]

def main():
    cc=os.environ.get('CC') or shutil.which('clang') or shutil.which('cc')
    if not cc:raise SystemExit('C11 compiler required')
    module=ROOT/'components/control_protocol'
    subprocess.run(['python3',str(ROOT/'tools/generate_control_api.py'),'--check'],check=True)
    with tempfile.TemporaryDirectory(prefix='control-protocol-') as directory:
        binary=Path(directory)/'test_control'
        subprocess.run([cc,'-std=c11','-D_POSIX_C_SOURCE=200809L','-Wall','-Wextra','-Werror','-pedantic','-g','-fsanitize=address,undefined', '-fno-sanitize-recover=all','-fno-omit-frame-pointer','-I'+str(module/'include'),'-I'+str(module),str(ROOT/'tests/control_protocol/test_control.c'),*[str(module/name) for name in ['control_protocol.c','control_json.c','control_registry.c','control_sha256.c']],'-lm','-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)

if __name__=='__main__':main()
