#!/usr/bin/env python3
"""Render production LVGL pages at RGB565; CMake and the pinned LVGL are required."""
from pathlib import Path
import argparse
import shutil
import struct
import subprocess
import zlib

source = Path(__file__).resolve().parent
root = source.parents[2]
parser = argparse.ArgumentParser()
parser.add_argument('--output', type=Path, default=root / '.agent/local/passport-ui/render')
args = parser.parse_args()
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
headers = out / 'headers'
for name in ('esp_err.h bsp_button.h bsp_display.h bsp_pins.h esp_timer.h '
             'freertos/FreeRTOS.h freertos/task.h freertos/queue.h').split():
    path = headers / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text('#include "platform.h"\n')
cmake = shutil.which('cmake') or str(root / '.tools/passport-espressif/tools/cmake/3.30.2/CMake.app/Contents/bin/cmake')
ninja = shutil.which('ninja') or str(root / '.tools/passport-espressif/tools/ninja/1.12.1/ninja')
(out / 'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.16)
project(passport_render C)
set(CMAKE_C_FLAGS "-Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer")
set(LV_BUILD_CONF_PATH "{source / 'lv_conf.h'}" CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory("{root / 'passport/managed_components/lvgl__lvgl'}" lvgl)
add_executable(render "{source / 'render.c'}" "{root / 'passport/main/passport_widgets.c'}")
target_link_libraries(render PRIVATE lvgl)
target_include_directories(render PRIVATE "{headers}" "{source}")
''')
subprocess.run([cmake, '-S', str(out), '-B', str(out / 'build'), '-G', 'Ninja', f'-DCMAKE_MAKE_PROGRAM={ninja}'], check=True)
subprocess.run([cmake, '--build', str(out / 'build'), '-j', '4'], check=True)
subprocess.run([str(out / 'build/render')], cwd=out, check=True)

def chunk(kind, data):
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))

def png(path, width, height, data):
    rows = b''.join(b'\0' + data[y * width * 3:(y + 1) * width * 3] for y in range(height))
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))

for path in out.glob('*.ppm'):
    data = path.read_bytes().split(b'\n', 3)[3]
    png(path.with_suffix('.png'), 240, 320, data)
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
