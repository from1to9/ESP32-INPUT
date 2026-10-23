#!/usr/bin/env python3
"""Verify the locked TinyUSB source and generate its reviewed audio-only copy."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "patches/tinyusb-audio-streams.json"


def digest(data):
    return hashlib.sha256(data).hexdigest()


def apply_patch(source, patch, name):
    """Apply one exact unified-diff file, rejecting every unmatched context."""
    lines = patch.splitlines(keepends=True)
    if lines[:2] != ["--- a/" + name + "\n", "+++ b/" + name + "\n"]:
        raise ValueError("Unexpected TinyUSB patch file boundary")
    original = source.splitlines(keepends=True)
    output, cursor, index = [], 0, 2
    while index < len(lines):
        match = re.fullmatch(r"@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@[^\n]*\n", lines[index])
        if not match:
            raise ValueError("Invalid TinyUSB patch hunk")
        start, old_count, new_start, new_count = [int(value) if value is not None else 1 for value in match.groups()]
        if start - 1 < cursor or start - 1 > len(original):
            raise ValueError("Invalid TinyUSB hunk offset")
        output.extend(original[cursor:start - 1]); cursor = start - 1
        if len(output) != new_start - 1:
            raise ValueError("Invalid TinyUSB output offset")
        old_seen = new_seen = 0; index += 1
        while index < len(lines) and not lines[index].startswith("@@ "):
            line = lines[index]; index += 1
            if line[:1] not in (" ", "+", "-"):
                raise ValueError("Invalid TinyUSB patch line")
            if line[0] != "+":
                if cursor >= len(original) or original[cursor] != line[1:]:
                    raise ValueError("TinyUSB patch context changed")
                cursor += 1; old_seen += 1
            if line[0] != "-":
                output.append(line[1:]); new_seen += 1
        if (old_seen, new_seen) != (old_count, new_count):
            raise ValueError("TinyUSB patch hunk count changed")
    output.extend(original[cursor:])
    return "".join(output)


def generate(source_root, output, *, check_only=False, manifest_path=MANIFEST,
             lock_path=ROOT / "firmware/dependencies.lock"):
    manifest = json.loads(Path(manifest_path).read_text())
    name = manifest["source_path"]
    if manifest["schema"] != 1 or name != "src/class/audio/audio_device.c":
        raise ValueError("Unexpected TinyUSB manifest scope")
    lock = Path(lock_path).read_text()
    block = re.search(r"(?m)^  " + re.escape(manifest["component"]) + r":\n((?:^ {4,}.*\n|^\n)+)", lock)
    if not block:
        raise ValueError("TinyUSB dependency lock entry missing")
    for key, expected in (("component_hash", manifest["component_hash"]), ("version", manifest["version"])):
        value = re.search(r"(?m)^    " + key + r": ([^\n]+)$", block[1])
        if not value or value[1].strip().strip("'\"") != expected:
            raise ValueError("TinyUSB dependency lock differs from reviewed pin")
    source_path = Path(source_root) / name
    if source_path.is_symlink() or not source_path.is_file():
        raise ValueError("TinyUSB source type changed")
    source = source_path.read_bytes()
    if digest(source) != manifest["source_sha256"]:
        raise ValueError("TinyUSB source differs from reviewed preimage")
    patch_path = ROOT / manifest["patch_file"]
    if patch_path.resolve() != (ROOT / "patches/tinyusb-audio-streams.patch").resolve():
        raise ValueError("TinyUSB patch path changed")
    patch = patch_path.read_bytes()
    if digest(patch) != manifest["patch_sha256"]:
        raise ValueError("TinyUSB patch differs from reviewed digest")
    generated = apply_patch(source.decode(), patch.decode(), name).encode()
    if digest(generated) != manifest["output_sha256"]:
        raise ValueError("Generated TinyUSB source differs from reviewed postimage")
    output = Path(output)
    if output.is_symlink() or output.resolve() == source_path.resolve():
        raise ValueError("Generated TinyUSB path must not replace managed source")
    if output.exists() and (not output.is_file() or output.read_bytes() != generated):
        raise ValueError("Existing TinyUSB generated output has unknown content; preserve it")
    if check_only and not output.is_file():
        raise ValueError("Generated TinyUSB output is missing")
    if not output.exists():
        output.parent.mkdir(parents=True, exist_ok=True)
        temporary = None
        try:
            with tempfile.NamedTemporaryFile(dir=output.parent, prefix=".audio-patch-", delete=False) as stream:
                temporary = Path(stream.name); stream.write(generated)
            try:
                os.link(temporary, output)
            except FileExistsError:
                if output.is_symlink() or not output.is_file() or output.read_bytes() != generated:
                    raise ValueError("Concurrent TinyUSB generated output has unknown content; preserve it")
        finally:
            if temporary is not None and temporary.exists():
                temporary.unlink()
    return {"component": manifest["component"], "version": manifest["version"],
            "source_sha256": digest(source), "output_sha256": digest(generated), "output": str(output)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=ROOT / "firmware/managed_components/espressif__tinyusb")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--check", action="store_true", help="Verify an existing generated copy without writes")
    args = parser.parse_args()
    print(json.dumps(generate(args.source_root, args.output, check_only=args.check)))


if __name__ == "__main__":
    main()
