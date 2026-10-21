#!/usr/bin/env python3
"""Validate canonical remote media commands against the actual typed registry."""
from pathlib import Path
from datetime import datetime, timezone
import hashlib
import json
import os
import shutil
import subprocess

root = Path(__file__).resolve().parents[2]
output = root / '.agent/local/remote-media' / datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
output.mkdir(parents=True)
compiler = os.environ.get('CC') or shutil.which('clang') or shutil.which('cc')
if not compiler:
    raise SystemExit('A strict C11 compiler with sanitizers is required.')
protocol = root / 'components/control_protocol'
sources = [root / 'firmware/main/app_remote_media.c', root / 'components/audio_engine/audio_media.c',
           root / 'tests/remote_media/test_remote_media.c']
sources += [protocol / name for name in ('control_protocol.c', 'control_registry.c', 'control_json.c', 'control_sha256.c')]
files = sources + [root / 'firmware/main/app_remote_media.h', root / 'firmware/main/app_remote_media_bindings.inc',
                  root / 'firmware/main/app_remote_media_runtime_port.h']
before = {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in files}
command = [compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wpedantic', '-Wconversion', '-Wshadow',
           '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
for include in ('firmware/main', 'components/control_protocol/include', 'components/audio_engine/include', 'components/spoken_letters/include'):
    command += ['-I', str(root / include)]
binary = output / 'remote_media'
with (output / 'build.log').open('w') as log:
    subprocess.run(command + [str(p) for p in sources] + ['-lm', '-o', str(binary)], check=True, stdout=log, stderr=subprocess.STDOUT)
env = dict(os.environ, UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
run = subprocess.run([str(binary)], env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
(output / 'test.log').write_text(run.stdout)
print(run.stdout, end='')
after = {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in files}
(output / 'manifest.json').write_text(json.dumps({'source_before': before, 'source_after': after,
    'source_stable': before == after, 'exit': run.returncode, 'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest()}, indent=2) + '\n')
assert before == after, 'Sources changed during validation'
if run.returncode:
    raise SystemExit(run.returncode)
print(f'Evidence: {output}')
