#!/usr/bin/env python3
"""Exercise SDK patch provenance and preservation using isolated Git fixtures."""
import difflib
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from idf_patches import verify_sdk  # noqa: E402
from idf import verify_revision  # noqa: E402


class PatchPolicy(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="esp32-sdk-patch-policy-")
        self.base = Path(self.temporary.name)
        self.sdk = self.base / "sdk"
        self.sdk.mkdir()
        self.git("init", "-q")
        self.before = {"api.h": "#define API_VERSION 1\n", "security.c": "void confirm(void) { accept(); }\n"}
        self.after = {"api.h": "#define API_VERSION 1\n#define ADMISSION_REQUIRED 1\n",
                      "security.c": "void confirm(void) { ask_application(); }\n"}
        for name, text in self.before.items():
            (self.sdk / name).write_text(text)
        (self.sdk / "unrelated.c").write_text("int existing;\n")
        self.git("add", ".")
        self.git("-c", "user.name=SDK Test", "-c", "user.email=sdk-test@example.invalid",
                 "commit", "-qm", "fixture")
        self.pin = self.git("rev-parse", "HEAD").strip()
        patch = "".join("".join(difflib.unified_diff(self.before[name].splitlines(True),
                           self.after[name].splitlines(True), n=0, fromfile="a/" + name, tofile="b/" + name))
                        for name in self.before)
        self.patch = self.base / "reviewed.patch"
        self.patch.write_text(patch)
        self.manifest = self.base / "manifest.json"
        self.manifest.write_text(json.dumps({"sdk_commit": self.pin, "patch_sha256": self.sha(patch),
            "files": {name: {"before": self.sha(self.before[name]), "after": self.sha(self.after[name])}
                      for name in self.before}}))

    def tearDown(self):
        self.temporary.cleanup()

    @staticmethod
    def sha(text):
        return hashlib.sha256(text.encode()).hexdigest()

    def git(self, *args):
        return subprocess.check_output(["git", "-C", str(self.sdk), *args], text=True)

    def verify(self, **kwargs):
        verify_sdk(self.sdk, self.pin, manifest_path=self.manifest, patch_path=self.patch, **kwargs)

    def test_clean_apply_and_verified_idempotence(self):
        self.verify()
        self.verify(check_only=True)
        self.verify()
        self.assertEqual(self.git("rev-parse", "HEAD").strip(), self.pin)
        for name, text in self.after.items():
            self.assertEqual((self.sdk / name).read_text(), text)

    def test_check_only_preserves_pristine_tree(self):
        with self.assertRaises(SystemExit):
            self.verify(check_only=True)
        self.assertEqual(self.git("status", "--porcelain"), "")

    def test_known_partial_patch_resumes(self):
        (self.sdk / "api.h").write_text(self.after["api.h"])
        self.verify()
        self.assertEqual((self.sdk / "security.c").read_text(), self.after["security.c"])

    def test_unrelated_dirty_and_staged_changes_are_preserved(self):
        path = self.sdk / "unrelated.c"
        path.write_text("int user_work;\n")
        with self.assertRaises(SystemExit):
            self.verify()
        self.assertEqual(path.read_text(), "int user_work;\n")
        self.assertEqual((self.sdk / "api.h").read_text(), self.before["api.h"])
        self.git("add", "unrelated.c")
        with self.assertRaises(SystemExit):
            self.verify()
        self.assertIn("user_work", self.git("show", ":unrelated.c"))

    def test_unknown_edit_to_patch_file_is_preserved(self):
        (self.sdk / "security.c").write_text("void custom_work(void);\n")
        with self.assertRaises(SystemExit):
            self.verify()
        self.assertEqual((self.sdk / "security.c").read_text(), "void custom_work(void);\n")
        self.assertEqual((self.sdk / "api.h").read_text(), self.before["api.h"])

    def test_staged_known_patch_is_not_mutated(self):
        (self.sdk / "api.h").write_text(self.after["api.h"])
        self.git("add", "api.h")
        with self.assertRaises(SystemExit):
            self.verify()
        self.assertEqual(self.git("show", ":api.h"), self.after["api.h"])

    def test_wrong_pin_and_modified_patch_are_rejected(self):
        with self.assertRaises(SystemExit):
            verify_sdk(self.sdk, "0" * 40, manifest_path=self.manifest, patch_path=self.patch)
        self.patch.write_text(self.patch.read_text() + "\n")
        with self.assertRaises(SystemExit):
            self.verify()
        self.assertEqual(self.git("status", "--porcelain"), "")

    def test_unpatched_bsp_verifier_still_rejects_dirt(self):
        verify_revision(self.sdk, self.pin)
        (self.sdk / "api.h").write_text(self.after["api.h"])
        with self.assertRaises(SystemExit):
            verify_revision(self.sdk, self.pin)


if __name__ == "__main__":
    unittest.main()
