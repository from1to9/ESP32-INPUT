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
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from idf_patches import verify_sdk  # noqa: E402
from idf import verify_revision  # noqa: E402
import idf  # noqa: E402


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


class GeneratedSourceBuildGate(unittest.TestCase):
    """The wrapper must reject changed generated code before invoking IDF."""

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="esp32-generated-build-gate-")
        self.root = Path(self.temporary.name)
        self.sdk = self.root / "sdk"
        self.sdk.mkdir()
        self.tools = self.root / "toolchain"
        self.python = self.tools / "python_env/idf6.2_pytest_env/bin/python"
        self.python.parent.mkdir(parents=True)
        self.python.touch()
        self.output = self.root / "firmware/build/generated/tinyusb_audio/audio_device.c"
        self.dcd_output = self.output.with_name("dcd_dwc2.c")
        self.output.parent.mkdir(parents=True)
        self.actions = []

    def tearDown(self):
        self.temporary.cleanup()

    def invoke(self, *, compiled_output="reviewed", compiled_dcd="reviewed", fail_build=False):
        def boundary(args, **kwargs):
            if args[0] == "bash":
                self.actions.append("build")
                if fail_build:
                    raise subprocess.CalledProcessError(1, args)
                if compiled_output is not None:
                    self.output.write_text(compiled_output)
                if compiled_dcd is not None:
                    self.dcd_output.write_text(compiled_dcd)
            else:
                self.actions.append("check")
                self.assertEqual(args, [self.python, self.root / "tools/tinyusb_audio_patch.py",
                                       "--source-root", self.root / "firmware/managed_components/espressif__tinyusb",
                                       "--output", self.output, "--check"])
                # The production helper owns digest verification; substitute
                # only its rejection boundary to test build sequencing.
                if any(path.is_symlink() or not path.is_file() or path.read_text() != "reviewed"
                       for path in (self.output, self.dcd_output)):
                    raise SystemExit("Unverified generated source")

        with patch.multiple(idf, ROOT=self.root, SDK=self.sdk, TOOLS=self.tools), \
                patch.object(idf, "verify_sdk"), patch.object(idf, "verify_revision"), \
                patch.object(idf, "run", side_effect=boundary):
            idf.build("build")

    def test_existing_copy_is_checked_before_and_after_build(self):
        self.output.write_text("reviewed")
        self.dcd_output.write_text("reviewed")
        self.invoke()
        self.assertEqual(self.actions, ["check", "build", "check"])

    def test_unknown_existing_copy_never_reaches_build(self):
        self.output.write_text("unrelated work")
        with self.assertRaises(SystemExit):
            self.invoke()
        self.assertEqual(self.actions, ["check"])
        self.assertEqual(self.output.read_text(), "unrelated work")

    def test_first_configuration_requires_checked_output_after_build(self):
        self.invoke()
        self.assertEqual(self.actions, ["build", "check"])

    def test_missing_or_changed_build_output_does_not_pass(self):
        for output in (None, "unreviewed"):
            with self.subTest(output=output):
                self.actions.clear()
                self.output.unlink(missing_ok=True)
                self.dcd_output.unlink(missing_ok=True)
                with self.assertRaises(SystemExit):
                    self.invoke(compiled_output=output)
                self.assertEqual(self.actions, ["build", "check"])

    def test_dangling_symlink_is_not_a_pristine_first_build(self):
        self.output.symlink_to(self.root / "missing")
        with self.assertRaises(SystemExit):
            self.invoke()
        self.assertEqual(self.actions, ["check"])
        self.assertTrue(self.output.is_symlink())

    def test_failed_build_remains_failed_without_postcheck_success(self):
        self.output.write_text("reviewed")
        self.dcd_output.write_text("reviewed")
        with self.assertRaises(subprocess.CalledProcessError):
            self.invoke(fail_build=True)
        self.assertEqual(self.actions, ["check", "build"])

    def test_surviving_dcd_cannot_bypass_precheck_when_audio_is_missing(self):
        for contents in ("reviewed", "unrelated work", None):
            with self.subTest(contents=contents):
                self.actions.clear()
                self.dcd_output.unlink(missing_ok=True)
                if contents is None:
                    self.dcd_output.symlink_to(self.root / "missing-dcd")
                else:
                    self.dcd_output.write_text(contents)
                with self.assertRaises(SystemExit):
                    self.invoke()
                self.assertEqual(self.actions, ["check"])
                self.assertFalse(self.output.exists())
                if contents is None:
                    self.assertTrue(self.dcd_output.is_symlink())
                else:
                    self.assertEqual(self.dcd_output.read_text(), contents)

    def test_postcheck_requires_both_generated_sources(self):
        for dcd in (None, "unreviewed"):
            with self.subTest(dcd=dcd):
                self.actions.clear()
                self.output.unlink(missing_ok=True)
                self.dcd_output.unlink(missing_ok=True)
                with self.assertRaises(SystemExit):
                    self.invoke(compiled_dcd=dcd)
                self.assertEqual(self.actions, ["build", "check"])


if __name__ == "__main__":
    unittest.main()
