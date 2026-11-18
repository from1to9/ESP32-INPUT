#!/usr/bin/env python3
"""Render production LVGL pages at RGB565; CMake and the pinned LVGL are required."""
from pathlib import Path
import argparse
import shutil
import struct
import subprocess
import tempfile
import zlib
import sys

source = Path(__file__).resolve().parent
root = source.parents[2]
sys.path.insert(0, str(source.parent))
from build_info import prepare_ui_build_info
parser = argparse.ArgumentParser()
parser.add_argument('--output', type=Path, default=root / '.agent/local/passport-wifi/render')
parser.add_argument('--setup-only', action='store_true', help='Render only the enabled setup window, details, URL QR and confirmation')
parser.add_argument('--control-only', action='store_true', help='Render network controller status, active mouse and Lock')
parser.add_argument('--home-only', action='store_true', help='Render Home readiness transitions in both themes')
parser.add_argument('--compare-partial', action='store_true', help='Compare every selected page with the 16-line partial DMA buffer layout')
args = parser.parse_args()
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
headers = out / 'headers'
for name in ('esp_err.h esp_heap_caps.h esp_log.h bsp_button.h bsp_display.h bsp_pins.h esp_timer.h '
             'freertos/FreeRTOS.h freertos/task.h freertos/queue.h').split():
    path = headers / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text('#include "platform.h"\n')
prepare_ui_build_info(root, headers)
cmake = shutil.which('cmake') or str(root / '.tools/passport-espressif/tools/cmake/3.30.2/CMake.app/Contents/bin/cmake')
ninja = shutil.which('ninja') or str(root / '.tools/passport-espressif/tools/ninja/1.12.1/ninja')
(out / 'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.16)
project(passport_render C)
set(CMAKE_C_FLAGS "-Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer")
set(LV_BUILD_CONF_PATH "{source / 'lv_conf.h'}" CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory("{root / 'passport/managed_components/lvgl__lvgl'}" lvgl)
add_executable(render "{source / 'render.c'}" "{root / 'passport/main/passport_widgets.c'}"
    "{root / 'passport/main/passport_wifi_text.c'}" "{root / 'passport/main/passport_names_text.c'}")
target_link_libraries(render PRIVATE lvgl)
target_include_directories(render PRIVATE "{headers}" "{source}")
''')
subprocess.run([cmake, '-S', str(out), '-B', str(out / 'build'), '-G', 'Ninja', f'-DCMAKE_MAKE_PROGRAM={ninja}'], check=True)
subprocess.run([cmake, '--build', str(out / 'build'), '-j', '4'], check=True)
render_command = [str(out / 'build/render')] + (['--setup-only'] if args.setup_only else ['--control-only'] if args.control_only else ['--home-only'] if args.home_only else [])
subprocess.run(render_command, cwd=out, check=True)
if args.compare_partial:
    with tempfile.TemporaryDirectory(prefix='partial-', dir=out) as partial_dir:
        partial = Path(partial_dir)
        subprocess.run(render_command + ['--partial'], cwd=partial, check=True)
        frames = sorted(partial.glob('*.ppm'))
        assert frames, 'No partial frames rendered'
        for frame in frames:
            assert frame.read_bytes() == (out / frame.name).read_bytes(), f'Partial render differs: {frame.name}'
        print(f'PASS: {len(frames)} direct and 16-line partial frames are pixel-identical')

def chunk(kind, data):
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))

def png(path, width, height, data):
    rows = b''.join(b'\0' + data[y * width * 3:(y + 1) * width * 3] for y in range(height))
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))

for path in out.glob('*.ppm'):
    data = path.read_bytes().split(b'\n', 3)[3]
    png(path.with_suffix('.png'), 240, 320, data)
if args.setup_only or args.control_only or args.home_only:
    sys.exit(0)
for theme in ('dark', 'light'):
    names = ('home', 'mouse', 'keys', 'connections', 'bluetooth-scroll', 'forget', 'pair', 'appearance', 'appearance-error', 'bluetooth-empty', 'wifi', 'lock')
    width, height = 240 * 4, 320 * 3
    pixels = bytearray(width * height * 3)
    for i, name in enumerate(names):
        data = (out / f'{theme}-{name}.ppm').read_bytes().split(b'\n', 3)[3]
        for y in range(320):
            offset = ((i // 4 * 320 + y) * width + i % 4 * 240) * 3
            pixels[offset:offset + 720] = data[y * 720:(y + 1) * 720]
    png(out / f'{theme}-overview.png', width, height, pixels)

for theme in ('dark', 'light'):
    names = ('wifi', 'wifi-scan', 'wifi-saved', 'wifi-actions', 'wifi-confirm', 'wifi-pending', 'wifi-ap', 'wifi-details', 'wifi-join-qr', 'wifi-page-qr', 'status', 'home')
    width, height = 960, 960
    pixels = bytearray(width * height * 3)
    for i, name in enumerate(names):
        data = (out / f'{theme}-{name}.ppm').read_bytes().split(b'\n', 3)[3]
        for y in range(320):
            offset = ((i // 4 * 320 + y) * width + i % 4 * 240) * 3
            pixels[offset:offset + 720] = data[y * 720:(y + 1) * 720]
    png(out / f'{theme}-wifi-overview.png', width, height, pixels)
