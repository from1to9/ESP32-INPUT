#!/usr/bin/env python3
"""Build, validate and render the isolated production boot UI."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import zlib

ROOT = Path(__file__).resolve().parents[2]
SOURCES = (
    "firmware/main/app_boot_ui.c", "firmware/main/app_boot_ui.h",
    "tests/boot_ui/CMakeLists.txt", "tests/boot_ui/test_boot_ui.c",
    "tests/boot_ui/run.py", "tests/ui/lv_conf.h", "tests/ui/ui_memory.c",
    "tests/ui/ui_memory.h", "firmware/dependencies.lock",
)


def hashes():
    return {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in SOURCES}


def executable(name):
    found = shutil.which(name)
    if found:
        return found
    candidates = sorted((ROOT / ".tools/espressif/tools" / name).glob(f"**/bin/{name}"))
    if name == "ninja":
        candidates += sorted((ROOT / ".tools/espressif/tools/ninja").glob("*/ninja"))
    if not candidates:
        raise SystemExit(f"{name} is required; see firmware/README.md")
    return str(candidates[-1])


def convert_png(path):
    content = path.read_bytes()
    header = b"P6\n480 480\n255\n"
    if not content.startswith(header) or len(content) != len(header) + 480 * 480 * 3:
        raise ValueError(f"Invalid framebuffer: {path.name}")
    pixels = content[len(header):]
    rows = b"".join(b"\0" + pixels[y * 1440:(y + 1) * 1440] for y in range(480))

    def chunk(kind, payload):
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload))

    result = b"\x89PNG\r\n\x1a\n"
    result += chunk(b"IHDR", struct.pack(">IIBBBBB", 480, 480, 8, 2, 0, 0, 0))
    result += chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b"")
    path.with_suffix(".png").write_bytes(result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / ".build/boot-ui")
    parser.add_argument("--output-dir", type=Path, help="new evidence directory; existing evidence is preserved")
    args = parser.parse_args()
    output = args.output_dir or ROOT / ".agent/local/boot-ui-runs" / datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    build = args.build_dir.resolve()
    before = hashes()
    manifest = {"sources_before": before, "passed": False, "sanitizers": "address,undefined",
                "leak_check": "LVGL allocator observer; platform LeakSanitizer disabled"}
    try:
        cmake = executable("cmake")
        commands = [
            ("configure", [cmake, "-S", str(ROOT / "tests/boot_ui"), "-B", str(build), "-G", "Ninja",
                           f"-DCMAKE_MAKE_PROGRAM={executable('ninja')}", "-DCMAKE_BUILD_TYPE=Debug",
                           "-DBOOT_UI_SANITIZERS=ON"]),
            ("build", [cmake, "--build", str(build), "-j", "6"]),
            ("test", [str(build / "boot_ui"), str(output)]),
        ]
        environment = dict(os.environ, ASAN_OPTIONS="detect_leaks=0", UBSAN_OPTIONS="halt_on_error=1")
        for name, command in commands:
            with (output / f"{name}.log").open("w") as log:
                subprocess.run(command, cwd=ROOT, env=environment, stdout=log, stderr=subprocess.STDOUT, check=True)
        result = json.loads((output / "test.log").read_text().splitlines()[-1])
        if not result.get("passed"):
            raise RuntimeError("Native harness did not report a successful result")
        expected = {"starting", "progress", "fault", "ready", "maximum-text"}
        captures = list(output.glob("*.ppm"))
        if {path.stem for path in captures} != expected:
            raise RuntimeError("Expected boot renders are incomplete")
        for capture in captures:
            convert_png(capture)
        manifest["result"] = result
        manifest["images"] = {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                              for path in sorted(output.glob("*.png"))}
        manifest["passed"] = True
    except (subprocess.CalledProcessError, ValueError, RuntimeError, OSError) as error:
        manifest["error"] = str(error)
    finally:
        manifest["sources_after"] = hashes()
        manifest["source_stable"] = before == manifest["sources_after"]
        manifest["passed"] = manifest["passed"] and manifest["source_stable"]
        (output / "validation-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps({"passed": manifest["passed"], "source_stable": manifest["source_stable"],
                      "evidence": str(output)}, indent=2))
    raise SystemExit(0 if manifest["passed"] else 1)


if __name__ == "__main__":
    main()
