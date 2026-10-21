#!/usr/bin/env python3
"""Strict native tests of the real portable audio code; no device access."""
from datetime import datetime, timezone
from pathlib import Path
import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import wave

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    output = args.output_dir or ROOT / ".agent/local/audio-engine" / datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    if output.exists() and any(output.iterdir()):
        raise SystemExit("Evidence directory must be new or empty")
    output.mkdir(parents=True, exist_ok=True)
    compiler = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
    if not compiler:
        raise SystemExit("A C11 compiler is required")
    files = sorted(p for base in [ROOT / "components/audio_engine", ROOT / "tests/audio_engine"]
                   for p in base.rglob("*") if p.is_file())

    files += [ROOT / "firmware/main" / name for name in
              ("app_audio.c", "app_audio.h", "app_recording_store.c", "app_recording_store.h")]

    def hashes():
        return {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest() for path in files}

    before = hashes()
    binary = output / "test_media"
    command = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wpedantic",
               "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g",
               "-I", str(ROOT / "components/audio_engine/include"),
               str(ROOT / "components/audio_engine/audio_media.c"),
               str(ROOT / "tests/audio_engine/test_media.c"), "-o", str(binary)]
    environment = dict(os.environ)
    environment["ASAN_OPTIONS"] = "detect_leaks=0" if sys.platform == "darwin" else "detect_leaks=1"
    environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    environment["AE_TEST_RECORD_WAV"] = str(output / "recorded.wav")
    with (output / "build.log").open("w") as log:
        subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT, cwd=ROOT)
    result = subprocess.run([str(binary), str(output / "synthetic.wav")], cwd=ROOT,
                            env=environment, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (output / "media.log").write_text(result.stdout)
    print(result.stdout, end="")
    if result.returncode:
        raise SystemExit(result.returncode)
    with wave.open(str(output / "synthetic.wav"), "rb") as wav:
        assert (wav.getframerate(), wav.getnchannels(), wav.getsampwidth(), wav.getnframes()) == (16000, 1, 2, 320)
        assert struct.unpack("<320h", wav.readframes(320)) == tuple(range(-160, 160))
    suites = {}
    common = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wpedantic",
              "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g",
              "-I", str(ROOT / "components/audio_engine/include")]
    engine_sources = [ROOT / "components/audio_engine" / name for name in
                      ("audio_engine.c", "audio_media.c", "audio_recording.c")]
    engine_command = common + [str(p) for p in engine_sources] + [str(ROOT / "tests/audio_engine/test_engine.c")]
    psa = ROOT / ".tools/esp-idf/components/mbedtls/mbedtls/tf-psa-crypto"
    psa_build = ROOT / ".build/audio-psa"
    cmake = shutil.which("cmake")
    if not cmake:
        candidates = sorted((ROOT / ".tools/espressif/tools/cmake").glob("*/CMake.app/Contents/bin/cmake"))
        cmake = str(candidates[-1]) if candidates else None
    if not cmake or not psa.is_dir():
        raise SystemExit("Pinned SDK and repository CMake are required for real crypto tests")
    with (output / "crypto-build.log").open("w") as log:
        subprocess.run([cmake, "-S", str(psa), "-B", str(psa_build), "-DENABLE_PROGRAMS=OFF",
                        "-DENABLE_TESTING=OFF", "-DGEN_FILES=OFF", "-DCMAKE_BUILD_TYPE=Debug",
                        "-DTF_PSA_CRYPTO_CONFIG_FILE=" + str(ROOT / "tests/audio_engine/psa_config.h")],
                       check=True, stdout=log, stderr=subprocess.STDOUT)
        subprocess.run([cmake, "--build", str(psa_build), "-j4"], check=True, stdout=log, stderr=subprocess.STDOUT)
    store_command = common + ["-I", str(psa / "include"), "-I", str(psa / "drivers/builtin/include"),
        '-DTF_PSA_CRYPTO_CONFIG_FILE="psa_config.h"', "-I", str(ROOT / "tests/audio_engine"),
        str(ROOT / "components/audio_engine/audio_store.c"), str(ROOT / "components/audio_engine/audio_media.c"),
        str(ROOT / "tests/audio_engine/test_store.c")]
    store_command += [str(psa_build / p) for p in ("core/libtfpsacrypto.a", "drivers/builtin/libbuiltin.a",
        "drivers/p256-m/libp256-m.a", "drivers/everest/libeverest.a", "drivers/pqcp/libpqcp.a",
        "platform/libplatform.a", "utilities/libutilities.a")]
    composition_command = [part for part in store_command if part != str(ROOT / "tests/audio_engine/test_store.c")]
    composition_command += ["-I", str(ROOT / "components/audio_engine"),
        str(ROOT / "components/audio_engine/audio_engine.c"), str(ROOT / "components/audio_engine/audio_recording.c"),
        str(ROOT / "tests/audio_engine/test_recording_composition.c")]
    app_command = common + ["-pthread", "-I", str(ROOT / "tests/audio_engine/stubs"),
        "-I", str(ROOT / "firmware/main"), str(ROOT / "firmware/main/app_audio.c")]
    app_command += [str(p) for p in engine_sources] + [str(ROOT / "tests/audio_engine/test_app_audio.c")]
    runtime_store_command = [part for part in store_command if part != str(ROOT / "tests/audio_engine/test_store.c")]
    runtime_store_command += ["-pthread", "-I", str(ROOT / "tests/audio_engine/stubs"), "-I", str(ROOT / "firmware/main"),
        str(ROOT / "tests/audio_engine/test_app_recording_store.c")]
    for name, build in [("engine", engine_command), ("store", store_command),
                        ("recording-composition", composition_command), ("app", app_command), ("recording-runtime", runtime_store_command)]:
        executable = output / ("test_" + name)
        with (output / (name + "-build.log")).open("w") as log:
            subprocess.run(build + ["-o", str(executable)], check=True, stdout=log, stderr=subprocess.STDOUT, cwd=ROOT)
        run = subprocess.run([str(executable)], env=environment, text=True,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, cwd=ROOT)
        (output / (name + ".log")).write_text(run.stdout)
        print(run.stdout, end="")
        if run.returncode:
            raise SystemExit(run.returncode)
        suites[name] = {"exit": run.returncode, "binary_sha256": hashlib.sha256(executable.read_bytes()).hexdigest()}
    recorded = output / "recorded.wav"
    recorded_hash = hashlib.sha256(recorded.read_bytes()).hexdigest()
    assert recorded_hash == (output / "recorded.wav.sha256").read_text()
    with wave.open(str(recorded), "rb") as wav:
        assert (wav.getframerate(), wav.getnchannels(), wav.getsampwidth(), wav.getnframes()) == (16000, 1, 2, 3840)
        assert wav.readframes(3840) == bytes([0x37]) * 4800 + bytes([0x5a]) * 2880
    after = hashes()
    evidence = {"source_before": before, "source_after": after, "source_stable": before == after,
                "native_exit": result.returncode, "suites": suites, "independent_wav_reader": "passed", "independent_recorded_wav_sha256": recorded_hash,
                "scope": "Real portable manager/media/encrypted-store code with boundary fault models; no physical device qualification."}
    (output / "manifest.json").write_text(json.dumps(evidence, indent=2) + "\n")
    assert before == after, "Sources changed during the test"
    print(f"PASS: independent WAV reader verified every PCM sample; evidence: {output}")


if __name__ == "__main__":
    main()
