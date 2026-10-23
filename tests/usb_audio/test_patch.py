#!/usr/bin/env python3
"""Failure-preserving checks of the real pinned-source copy generator."""
import importlib.util
import json
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("tinyusb_audio_patch", ROOT / "tools/tinyusb_audio_patch.py")
patcher = importlib.util.module_from_spec(spec); spec.loader.exec_module(patcher)


class PatchTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="usb-audio-patch-")
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name)
        self.source_root = self.base / "managed"
        self.source = self.source_root / "src/class/audio/audio_device.c"
        self.source.parent.mkdir(parents=True)
        self.original = (ROOT / "firmware/managed_components/espressif__tinyusb/src/class/audio/audio_device.c").read_bytes()
        self.source.write_bytes(self.original)
        self.output = self.base / "generated/audio_device.c"

    def test_exact_copy_and_idempotent_check(self):
        result = patcher.generate(self.source_root, self.output)
        self.assertEqual(self.source.read_bytes(), self.original)
        self.assertEqual(result["output_sha256"], patcher.digest(self.output.read_bytes()))
        self.assertEqual(self.output.read_bytes().split(b"#include", 1)[0], self.original.split(b"#include", 1)[0])
        stamp = self.output.stat().st_mtime_ns
        patcher.generate(self.source_root, self.output, check_only=True)
        self.assertEqual(self.output.stat().st_mtime_ns, stamp)

    def test_unknown_preimage_is_preserved(self):
        self.source.write_bytes(self.original + b"\n/* local change */\n")
        with self.assertRaisesRegex(ValueError, "preimage"):
            patcher.generate(self.source_root, self.output)
        self.assertFalse(self.output.exists())
        self.assertTrue(self.source.read_bytes().endswith(b"/* local change */\n"))

    def test_unknown_output_is_preserved(self):
        self.output.parent.mkdir(); self.output.write_text("important local artifact\n")
        with self.assertRaisesRegex(ValueError, "unknown content"):
            patcher.generate(self.source_root, self.output)
        self.assertEqual(self.output.read_text(), "important local artifact\n")

    def test_concurrent_unknown_output_is_preserved(self):
        def race(source, destination):
            Path(destination).write_text("concurrent local artifact\n")
            raise FileExistsError(destination)

        with mock.patch.object(patcher.os, "link", side_effect=race):
            with self.assertRaisesRegex(ValueError, "Concurrent.*unknown content"):
                patcher.generate(self.source_root, self.output)
        self.assertEqual(self.output.read_text(), "concurrent local artifact\n")
        self.assertEqual(self.source.read_bytes(), self.original)
        self.assertEqual(list(self.output.parent.glob(".audio-patch-*")), [])

    def test_missing_and_symlink_output_rejected(self):
        with self.assertRaisesRegex(ValueError, "missing"):
            patcher.generate(self.source_root, self.output, check_only=True)
        self.output.parent.mkdir(); self.output.symlink_to(self.source)
        with self.assertRaisesRegex(ValueError, "must not replace"):
            patcher.generate(self.source_root, self.output)
        self.assertEqual(self.source.read_bytes(), self.original)

    def test_wrong_lock_is_rejected(self):
        lock = self.base / "lock"
        lock.write_text((ROOT / "firmware/dependencies.lock").read_text().replace("version: 0.21.0~2", "version: 0.21.0~3"))
        with self.assertRaisesRegex(ValueError, "reviewed pin"):
            patcher.generate(self.source_root, self.output, lock_path=lock)
        self.assertFalse(self.output.exists())

    def test_patch_digest_and_context_rejected(self):
        manifest = json.loads(patcher.MANIFEST.read_text()); manifest["patch_sha256"] = "0" * 64
        path = self.base / "manifest.json"; path.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(ValueError, "reviewed digest"):
            patcher.generate(self.source_root, self.output, manifest_path=path)
        with self.assertRaisesRegex(ValueError, "context changed"):
            patcher.apply_patch("other source\n", "--- a/x\n+++ b/x\n@@ -1 +1 @@\n-old\n+new\n", "x")


if __name__ == "__main__":
    unittest.main()
