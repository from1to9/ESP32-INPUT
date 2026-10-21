#!/usr/bin/env python3
"""Compile owned control sources with the prepared pinned target, without linking/flashing."""
from pathlib import Path
import json,shlex,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]

def main():
    pin=json.loads((ROOT/'toolchain.lock.json').read_text())
    sdk=ROOT/'.tools/esp-idf'
    actual=subprocess.check_output(['git','-C',str(sdk),'rev-parse','HEAD'],text=True).strip()
    if actual!=pin['idf_commit']:raise SystemExit('Pinned SDK mismatch')
    entries=json.loads((ROOT/'firmware/build/compile_commands.json').read_text())
    main_entry=next(x for x in entries if x['file'].endswith('/main/main.c'))
    cp_entry=next(x for x in entries if x['file'].endswith('/control_protocol/control_protocol.c'))
    sources=[str(p.relative_to(ROOT)) for p in (ROOT/'components/control_protocol').glob('*.c')]
    sources+=['firmware/main/app_control_server.c']
    sources += [str(p.relative_to(ROOT)) for pattern in ('app_control_trust.c','app_control_runtime.c','app_clock.c','app_firmware_update.c') for p in (ROOT/'firmware/main').glob(pattern)]
    includes=['esp_bootloader_format/include','bootloader_support/include','bootloader_support/bootloader_flash/include','esp_https_server/include','esp-tls','esp-tls/include','app_update/include','esp_http_client/include','esp_partition/include','nvs_flash/include','esp_netif/include']
    with tempfile.TemporaryDirectory(prefix='control-target-') as directory:
        for i,name in enumerate(sources):
            entry=cp_entry if name.startswith('components/') else main_entry
            cmd=shlex.split(entry['command'])
            for n,value in enumerate(cmd):
                if value==entry['file']:cmd[n]=str(ROOT/name)
                elif n and cmd[n-1]=='-o':cmd[n]=str(Path(directory)/f'object-{i}.o')
            cmd+=['-I'+str(sdk/'components'/p) for p in includes]
            cmd+=['-Werror','-Wextra','-DCONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT=1']
            r=subprocess.run(cmd,cwd=entry['directory'],capture_output=True,text=True)
            if r.returncode:raise SystemExit(name+'\n'+r.stdout+r.stderr)
    print(f'PASS: {len(sources)} control source objects; target={pin["target"]}, SDK={actual}')
    print('Object compilation only; no final link, network, USB, device or flash operation.')

if __name__=='__main__':main()
