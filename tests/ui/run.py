#!/usr/bin/env python3
"""Build the production LVGL UI with pinned LVGL, test hits, and render PNGs."""
from pathlib import Path
import argparse
from datetime import datetime, timezone
import hashlib
import json
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
    parser.add_argument("--output-dir", type=Path,
                        help="new or empty evidence directory; existing evidence is never overwritten")
    parser.add_argument("--suite", choices=("all", "visual", "resources"), default="all",
                        help="focused diagnosis only; complete acceptance requires the default all suites")
    args = parser.parse_args()
    if args.output_dir is None:
        stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
        args.output_dir = ROOT / ".agent/local/ui-focused-workspace" / stamp
    if args.output_dir.exists() and any(args.output_dir.iterdir()):
        raise SystemExit(f"Evidence directory must be empty: {args.output_dir}")
    sources = list((ROOT / "tests/ui").glob("*")) + [
        ROOT / "firmware/main/app_ui.c", ROOT / "firmware/main/app_ui.h",
        ROOT / "components/input_core/include/input_core.h",
        ROOT / "firmware/dependencies.lock",
        ROOT / "design/ui-plan.md", ROOT / "docs/interaction.md",
    ]

    def hashes():
        return {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
                for path in sorted(sources) if path.is_file()}

    before = hashes()
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
    results = {}
    commands = []
    if args.suite in ("all", "visual"):
        commands.append(("visual", [str(args.build_dir / "ui_visual"), str(args.output_dir)]))
    if args.suite in ("all", "resources"):
        commands.append(("resources", [str(args.build_dir / "ui_resources"),
                                       str(args.output_dir / "resources.json")]))
    binary_before = {name: hashlib.sha256(Path(command[0]).read_bytes()).hexdigest()
                     for name, command in commands}
    for name, command in commands:
        result = subprocess.run(command, cwd=ROOT, env=environment, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True)
        (args.output_dir / f"{name}.log").write_text(result.stdout)
        print(result.stdout, end="")
        results[name] = result.returncode
    for ppm in args.output_dir.glob("*.ppm"):
        png(ppm)
    after = hashes()
    binary_after = {name: hashlib.sha256(Path(command[0]).read_bytes()).hexdigest()
                    for name, command in commands}
    stable = before == after and binary_before == binary_after
    screenshots = {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                   for path in sorted(args.output_dir.glob("*.png"))}
    legacy = json.loads((ROOT / "tests/ui/legacy-scenarios.json").read_text())["screenshots"]
    missing = sorted(set(legacy) - set(screenshots)) if args.suite in ("all", "visual") else []
    manifest = {"suite": args.suite, "source_sha256_before": before,
                "source_sha256_after": after, "source_stable": stable,
                "native_binary_sha256_before": binary_before, "native_binary_sha256_after": binary_after,
                "results": results, "screenshot_count": len(screenshots),
                "legacy_scenarios_expected": len(legacy), "missing_legacy_scenarios": missing,
                "screenshots": screenshots, "render_mode": "FULL", "color_format": "RGB565",
                "all_suites_passed": args.suite == "all" and stable and not missing and all(code == 0 for code in results.values()),
                "visual_review": "Pending; assertions and image generation do not establish visual review."}
    (args.output_dir / "validation-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Rendered {len(screenshots)} production UI screenshots to {args.output_dir}")
    print("Host rendering does not prove physical display readability or device input behavior.")
    if not stable:
        print("FAIL: observed source changed during the run; evidence does not qualify a stable tree.")
    if missing:
        print(f"FAIL: {len(missing)} retained baseline render scenarios are missing.")
    if not stable or missing or any(code != 0 for code in results.values()):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
