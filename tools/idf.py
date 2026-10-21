#!/usr/bin/env python3
"""Set up or build the pinned local SDK. Never opens or writes a device."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
from idf_patches import verify_sdk

ROOT = Path(__file__).resolve().parents[1]
PIN = json.loads((ROOT / "toolchain.lock.json").read_text())
SDK = ROOT / ".tools/esp-idf"
TOOLS = ROOT / ".tools/espressif"


def run(args, **kwargs):
    return subprocess.run([str(arg) for arg in args], check=True, **kwargs)


def verify_revision(path, expected):
    actual = subprocess.check_output(
        ["git", "-C", str(path), "rev-parse", "HEAD"], text=True).strip()
    if actual != expected:
        raise SystemExit(f"Revision mismatch in {path.name}: {actual}; expected {expected}.")
    dirty = subprocess.check_output(
        ["git", "-C", str(path), "status", "--porcelain", "--untracked-files=no"], text=True)
    if dirty:
        raise SystemExit(f"Tracked changes in {path.name}; preserve and resolve them before building.")


def base_environment(python):
    env = os.environ.copy()
    # Do not import another checkout's activated SDK or Python environment.
    for key in list(env):
        if key.startswith("IDF_") or key == "VIRTUAL_ENV":
            env.pop(key)
    env.update(IDF_PATH=str(SDK), IDF_TOOLS_PATH=str(TOOLS), IDF_TARGET=PIN["target"])
    env["PATH"] = str(Path(python).parent) + os.pathsep + env.get("PATH", "")
    return env


def setup(python):
    python = shutil.which(python) or python
    run([python, "-c", "import sys; assert sys.version_info >= (3, 11), 'Python 3.11+ required'"])
    if not SDK.exists():
        SDK.parent.mkdir(parents=True, exist_ok=True)
        run(["git", "init", SDK])
        run(["git", "-C", SDK, "remote", "add", "origin", PIN["idf_repository"]])
        run(["git", "-C", SDK, "fetch", "--depth", "1", "origin", PIN["idf_commit"]])
        run(["git", "-C", SDK, "checkout", "--detach", "FETCH_HEAD"])
    verify_sdk(SDK, PIN["idf_commit"])
    run(["git", "-C", SDK, "submodule", "update", "--init", "--recursive", "--depth", "1"])
    run(["git", "-C", ROOT, "submodule", "update", "--init", "--recursive"])
    verify_revision(ROOT / "third_party/esp-mosaico-bsp", PIN["bsp_commit"])
    env = base_environment(python)
    installer = SDK / "tools/idf_tools.py"
    run([python, installer, "install", "--targets", PIN["target"]], env=env)
    # These are on-request tools in the pinned SDK, not installed by install.sh.
    run([python, installer, "install", "cmake", "ninja"], env=env)
    run([python, installer, "install-python-env"], env=env)
    print("Local toolchain ready. Run: python3 tools/idf.py build")


def build(action):
    if not SDK.exists():
        raise SystemExit("SDK missing. Run: python3 tools/idf.py setup --python <python3.11-or-newer>")
    verify_sdk(SDK, PIN["idf_commit"])
    verify_revision(ROOT / "third_party/esp-mosaico-bsp", PIN["bsp_commit"])
    candidates = list((TOOLS / "python_env").glob("idf6.2_py*_env/bin/python"))
    if len(candidates) != 1:
        raise SystemExit("Expected one project-local IDF Python environment; rerun setup or resolve duplicates.")
    env = base_environment(candidates[0])
    script = ('source "$IDF_PATH/export.sh" >/dev/null && '
              'exec "$IDF_PYTHON_ENV_PATH/bin/python" "$IDF_PATH/tools/idf.py" '
              '--preview -C "$1" "$2"')
    run(["bash", "-c", script, "esp32-input-idf", ROOT / "firmware", action], env=env)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action", required=True)
    install = sub.add_parser("setup", help="Install the pinned SDK and tools locally")
    install.add_argument("--python", default=sys.executable, help="Python 3.11+ interpreter")
    for command in ("build", "size", "reconfigure"):
        sub.add_parser(command)
    args = parser.parse_args()
    if args.action == "setup":
        setup(args.python)
    else:
        build(args.action)


if __name__ == "__main__":
    main()
