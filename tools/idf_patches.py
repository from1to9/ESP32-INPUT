#!/usr/bin/env python3
"""Apply or verify the exact reviewed Classic pairing patch on the pinned SDK."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "patches/esp-idf-classic-pairing-admission.json"
PATCH = ROOT / "patches/esp-idf-classic-pairing-admission.patch"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify_sdk(sdk, expected_commit, *, check_only=False, manifest_path=MANIFEST, patch_path=PATCH):
    """Reject every unrelated/staged tracked change before any SDK mutation.

    A known partial application can resume, one exact preimage at a time.
    Untracked local files are preserved, matching the existing toolchain policy.
    The SDK's Git HEAD stays at the upstream pin; patch provenance is separate.
    """
    sdk = Path(sdk).resolve()
    manifest = json.loads(Path(manifest_path).read_text())
    if manifest["sdk_commit"] != expected_commit:
        raise SystemExit("SDK patch manifest differs from the toolchain lock pin.")
    head = subprocess.check_output(["git", "-C", str(sdk), "rev-parse", "HEAD"], text=True).strip()
    if head != expected_commit:
        raise SystemExit("SDK revision differs from the reviewed pairing patch pin.")
    if digest(Path(patch_path)) != manifest["patch_sha256"]:
        raise SystemExit("SDK patch content differs from its reviewed digest.")
    files = manifest["files"]
    for name in files:
        if Path(name).is_absolute() or ".." in Path(name).parts:
            raise SystemExit("Invalid SDK patch path.")
    status = subprocess.check_output(
        ["git", "-C", str(sdk), "status", "--porcelain", "-z", "--untracked-files=no"])
    for record in filter(None, status.split(b"\0")):
        name = record[3:].decode("utf-8")
        if record[:3] != b" M " or name not in files:
            raise SystemExit(f"SDK has unapproved tracked status {record[:2].decode()!r} in {name}; preserve and resolve it before building.")
    states = {}
    for name, hashes in files.items():
        path = sdk / name
        if path.is_symlink() or not path.is_file():
            raise SystemExit("SDK patch file type changed; refusing to overwrite.")
        actual = digest(path)
        if actual == hashes["after"]:
            states[name] = "after"
        elif actual == hashes["before"]:
            states[name] = "before"
        else:
            raise SystemExit("SDK patch file has unknown content; refusing to overwrite.")
    if check_only and "before" in states.values():
        raise SystemExit("Pinned SDK pairing-admission patch is not fully applied.")
    for name, state in states.items():
        if state == "after":
            continue
        command = ["git", "-C", str(sdk), "apply", "--unidiff-zero", "--include=" + name]
        subprocess.run(command + ["--check", str(patch_path)], check=True)
        subprocess.run(command + [str(patch_path)], check=True)
        if digest(sdk / name) != files[name]["after"]:
            raise SystemExit("Applied SDK file does not match its reviewed digest.")
    print("Pinned SDK revision and exact pairing-admission patch verified.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", type=Path, default=ROOT / ".tools/esp-idf")
    parser.add_argument("--check", action="store_true", help="Verify the applied patch without writes")
    args = parser.parse_args()
    pin = json.loads((ROOT / "toolchain.lock.json").read_text())
    verify_sdk(args.sdk, pin["idf_commit"], check_only=args.check)


if __name__ == "__main__":
    main()
