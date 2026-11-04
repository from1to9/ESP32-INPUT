#!/usr/bin/env python3
"""Compile production Bluetooth audio against deterministic SDK boundaries."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SUITE = Path(__file__).resolve().parent


def main():
    compiler = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
    if not compiler:
        raise SystemExit("A C11 compiler with address/undefined sanitizers is required.")
    with tempfile.TemporaryDirectory(prefix="esp32-bt-audio-") as temporary:
        binary = Path(temporary) / "test_bt_audio"
        command = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-Wconversion", "-Wno-sign-conversion", "-g", "-DTEST_BT_AUDIO_LOCKS",
                   "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                   "-I", str(SUITE / "stubs"), "-I", str(ROOT / "firmware/main"),
                   "-I", str(ROOT / "components/audio_engine/include"),
                   "-I", str(ROOT / "components/device_settings/include"),
                   str(SUITE / "test_bt_audio.c"), str(ROOT / "firmware/main/bt_audio.c"),
                   str(ROOT / "firmware/main/bt_audio_btc.c"),
                   "-o", str(binary)]
        subprocess.run(command, check=True)
        for scenario in ("normal", "init-timeout", "init-event-failure", "command-timeout",
                         "command-failure", "allocation-failure", "registration-failure", "stack-unready",
                         "audio-exclusion", "reviewed-handoff", "retry-capacity", "rename-good", "rename-set-fail", "rename-mismatch", "rename-short-eir", "rename-timeout", "rename-late-eir", "rename-late-read", "name-normal", "name-invalid", "name-missing", "name-event-failure", "name-submit-failure", "name-timeout",
                         "pair-failure", "pair-late", "pair-late-failure", "pair-terminal", "connect-race", "peer-switch", "pair-foreign", "pair-prewindow", "pair-unsaved",
                         "other-profile", "foreign-bonds", "a2dp-reconnect", "observed-disconnect", "dispatch-replace", "dispatch-down", "dispatch-cancel",
                         "dispatch-post-failure", "dispatch-reentrant", "profile-actions", "timeout-hfp", "timeout-a2dp", "pairing", "pairing-expiry", "pairing-timeout", "discovery-timeout", "playback-binding", "expiry-observation", "sustained-sbc",
                         "a2dp-queued-gap", "a2dp-overrun-future", "a2dp-next-head-clock", "hfp-transmit-clock", "hfp-write-clock"):
            subprocess.run([str(binary), scenario], check=True)
        diag = command[:command.index(str(SUITE / "test_bt_audio.c"))]
        diag.remove("-DTEST_BT_AUDIO_LOCKS")
        subprocess.run(diag + ["-DCONFIG_APP_A2DP_ACL_DIAGNOSTICS=1", str(SUITE / "test_acl_diagnostics.c"),
            str(ROOT / "firmware/main/bt_acl_diag.c"), "-o", str(binary)], check=True)
        for scenario in ("before-host", "enqueue", "headers", "window", "handle"):
            subprocess.run([str(binary), scenario], check=True)
        composition = [part for part in command if part != str(SUITE / "test_bt_audio.c")]
        composition += [str(SUITE / "engine_pairing_transport.c"), str(SUITE / "test_engine_pairing.c")]
        composition += [str(ROOT / "components/audio_engine" / name) for name in
                        ("audio_engine.c", "audio_media.c", "audio_recording.c")]
        subprocess.run(composition, check=True)
        for scenario in ("before-start", "before-prepare", "owner-loss", "privacy", "duplex-reconnect", "duplex-privacy"):
            subprocess.run([str(binary), scenario], check=True)
        subprocess.run(command + ["-DCONFIG_BT_CLASSIC_ENABLED=0"], check=True)
        subprocess.run([str(binary), "unsupported"], check=True)
        subprocess.run(command + ["-DESP_BT_GAP_JUST_WORKS_CONFIRM_REQUIRED=0"], check=True)
        subprocess.run([str(binary), "unsupported"], check=True)


if __name__ == "__main__":
    main()
