#!/usr/bin/env python3
"""Compare the pinned Fusion normal-sqrt option using unchanged frozen fixtures.

Run benchmark.py freeze first. Outputs are new local evidence, never firmware.
"""
from pathlib import Path
import hashlib
import importlib.util
import json
import os
import subprocess

ROOT = Path(__file__).resolve().parent
OUT = ROOT / 'normal-sqrt'
def digest(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()
freeze = json.loads((ROOT / 'freeze.json').read_text())
for name, expected in freeze['files'].items():
    assert digest(ROOT / name) == expected, name
OUT.mkdir(exist_ok=False)
protocol = json.loads((ROOT / 'protocol.json').read_text())
spec = importlib.util.spec_from_file_location('benchmark', ROOT / 'benchmark.py')
bench = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bench)
command = ['clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-pedantic',
    '-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
    '-DFUSION_USE_NORMAL_SQRT', '-I', str(ROOT / 'vendor/Fusion/Fusion'),
    str(ROOT / 'estimator.c'), str(ROOT / 'vendor/Fusion/Fusion/FusionAhrs.c'),
    '-lm', '-o', str(OUT / 'estimator')]
registration = {'scope': 'Only documented upstream normal sqrt compiler option differs; no source, fixture or acceptance changes',
    'baseline_freeze_sha256': digest(ROOT / 'freeze.json'), 'inputs': freeze['files'],
    'run_script_sha256': digest(Path(__file__)), 'compile_command': command,
    'prediction': 'Higher normalization precision may reduce gravity leakage; all failure criteria remain unchanged. Not a firmware result.'}
(OUT / 'registration.json').write_text(json.dumps(registration, indent=2) + '\n')
with (OUT / 'build.log').open('w') as log:
    subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT)
cases = json.loads((ROOT / 'cases.json').read_text())
inputs = bench.number_rows(ROOT / 'fixtures.csv')
grouped = {case['id']: [row for row in inputs if int(row['case_id']) == case['id']] for case in cases}
env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0', UBSAN_OPTIONS='halt_on_error=1')
results = {'registration_sha256': digest(OUT / 'registration.json'), 'runs': {}}
for candidate in ('integrate', 'leaky'):
    for source in ('fusion', 'oracle'):
        name = candidate + '-' + source
        output = OUT / (name + '.csv')
        with (ROOT / 'fixtures.csv').open() as stdin, output.open('w') as stdout, (OUT / (name + '.stderr')).open('w') as stderr:
            subprocess.run([str(OUT / 'estimator'), candidate, source], check=True, stdin=stdin, stdout=stdout, stderr=stderr, env=env)
        rows = bench.number_rows(output)
        cases_result = [bench.evaluate(case, grouped[case['id']], [row for row in rows if int(row['case_id']) == case['id']], protocol['acceptance']) for case in cases]
        checks = [check for case in cases_result for check in case['checks']]
        results['runs'][name] = {'cases_passed': sum(case['passed'] for case in cases_result), 'case_count': len(cases_result),
            'checks_passed': sum(check['passed'] for check in checks), 'check_count': len(checks), 'passed': all(check['passed'] for check in checks),
            'output_sha256': digest(output), 'cases': cases_result}
for name, expected in freeze['files'].items():
    assert digest(ROOT / name) == expected, name
(OUT / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
print(json.dumps({name: {key: value[key] for key in ('cases_passed', 'case_count', 'checks_passed', 'check_count', 'passed')} for name, value in results['runs'].items()}, indent=2))
