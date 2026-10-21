#!/usr/bin/env python3
"""Build the production LVGL UI with pinned LVGL, test hits, and render PNGs."""
from pathlib import Path
import argparse
import os
import shutil
import struct
import subprocess
import sys
import zlib

ROOT = Path(__file__).resolve().parents[2]


def executable(name):
    system = shutil.which(name)
    if system:
        return system
    candidates = sorted((ROOT / ".tools/espressif/tools" / name).glob(f"**/bin/{name}"))
    if name == "ninja":
        candidates += sorted((ROOT / ".tools/espressif/tools/ninja").glob("*/ninja"))
    if not candidates:
        raise SystemExit(f"{name} is required. See firmware/README.md for toolchain setup.")
    return str(candidates[-1])


def png(ppm):
    data = ppm.read_bytes()
    header = b"P6\n480 480\n255\n"
    if not data.startswith(header) or len(data) != len(header) + 480 * 480 * 3:
        raise SystemExit(f"Invalid framebuffer: {ppm}")
    pixels = data[len(header):]
    rows = b"".join(b"\0" + pixels[y * 1440:(y + 1) * 1440] for y in range(480))

    def chunk(kind, payload):
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload))

    output = b"\x89PNG\r\n\x1a\n"
    output += chunk(b"IHDR", struct.pack(">IIBBBBB", 480, 480, 8, 2, 0, 0, 0))
    output += chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b"")
    ppm.with_suffix(".png").write_bytes(output)
    ppm.unlink()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / ".build/ui-host")
    parser.add_argument("--output-dir", type=Path, default=ROOT / ".agent/local/ui-nine-transport")
    args = parser.parse_args()
    cmake = executable("cmake")
    configure = [cmake, "-S", str(ROOT / "tests/ui"), "-B", str(args.build_dir),
                 "-G", "Ninja", f"-DCMAKE_MAKE_PROGRAM={executable('ninja')}",
                 "-DCMAKE_BUILD_TYPE=Debug"]
    args.build_dir.mkdir(parents=True, exist_ok=True)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    log = args.build_dir / "build.log"
    with log.open("w") as stream:
        for command in [configure, [cmake, "--build", str(args.build_dir), "-j", "4"]]:
            result = subprocess.run(command, cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT)
            if result.returncode:
                print(log.read_text()[-12000:])
                raise SystemExit(result.returncode)
    environment = dict(os.environ)
    # Apple's bundled ASan has no LeakSanitizer; address/UB instrumentation
    # remains enabled. Linux also checks leaks.
    environment["ASAN_OPTIONS"] = "detect_leaks=0" if sys.platform == "darwin" else "detect_leaks=1"
    subprocess.run([str(args.build_dir / "ui_visual"), str(args.output_dir)],
                   cwd=ROOT, env=environment, check=True)
    for ppm in args.output_dir.glob("*.ppm"):
        png(ppm)
    print(f"Rendered {len(list(args.output_dir.glob('*.png')))} production UI screenshots to {args.output_dir}")
    print("Host rendering does not prove physical display readability or device input behavior.")


if __name__ == "__main__":
    main()
