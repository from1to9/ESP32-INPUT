#!/usr/bin/env python3
"""Exercise build identity against real temporary Git histories and archive fallbacks."""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("build_identity", ROOT / "tools/build_identity.py")
identity = importlib.util.module_from_spec(spec)
spec.loader.exec_module(identity)


class BuildIdentityTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="esp32-build-identity-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / "source"
        self.root.mkdir()
        (self.root / "firmware").mkdir()
        self.metadata = self.root / "firmware/project_identity.json"
        self.metadata.write_text(json.dumps({"schema_version": 1, "developer": "Example Developer",
                                            "repository_url": "https://github.com/example/firmware"}))
        (self.root / ".gitignore").write_text("build/\n")
        self.output = self.root / "build/generated/app_build_identity.h"
        self.git("init", "-q")
        self.git("config", "user.name", "Fixture Author")
        self.git("config", "user.email", "fixture@example.invalid")
        self.git("add", ".")
        self.git("-c", "commit.gpgsign=false", "commit", "-qm", "Initial fixture")

    def git(self, *args):
        return subprocess.check_output(["git", "-C", str(self.root), *args], text=True,
                                       stderr=subprocess.DEVNULL).strip()

    def test_commit_dirty_and_incremental_noop(self):
        first = identity.generate(self.root, self.output)
        self.assertEqual(first["git_commit"], self.git("rev-parse", "HEAD"))
        self.assertEqual(first["git_state"], "clean")
        stamp = self.output.stat().st_mtime_ns
        identity.generate(self.root, self.output)
        self.assertEqual(self.output.stat().st_mtime_ns, stamp)
        source = self.root / "input.c"
        source.write_text("int input;\n")
        self.assertEqual(identity.generate(self.root, self.output)["git_state"], "dirty")
        self.git("add", "input.c")
        self.assertEqual(identity.generate(self.root, self.output)["git_state"], "dirty")
        self.git("-c", "commit.gpgsign=false", "commit", "-qm", "Second fixture")
        second = identity.generate(self.root, self.output)
        self.assertEqual(second["git_state"], "clean")
        self.assertNotEqual(first["git_commit"], second["git_commit"])
        self.assertIn(second["git_commit"], self.output.read_text())
        source.write_text("int modified;\n")
        self.assertEqual(identity.generate(self.root, self.output)["git_state"], "dirty")
        self.git("checkout", "--", "input.c")
        self.git("checkout", "--detach", "-q")
        self.assertEqual(identity.generate(self.root, self.output), second)

    def test_archive_does_not_inherit_enclosing_checkout(self):
        archive = self.root / "archive"
        (archive / "firmware").mkdir(parents=True)
        shutil.copyfile(self.metadata, archive / "firmware/project_identity.json")
        result = identity.generate(archive, archive / "generated.h")
        self.assertEqual((result["git_commit"], result["git_state"]), ("", "unknown"))

    def test_unavailable_git_is_unknown_not_clean(self):
        with patch.dict(os.environ, {"PATH": str(self.root / "no-programs")}):
            result = identity.generate(self.root, self.output)
        self.assertEqual((result["git_commit"], result["git_state"]), ("", "unknown"))

    def test_status_failure_retains_commit_but_not_clean_claim(self):
        real_git = identity.git
        def failing_status(root, *args):
            if args[0] == "status":
                raise subprocess.TimeoutExpired("git status", 10)
            return real_git(root, *args)
        with patch.object(identity, "git", side_effect=failing_status):
            result = identity.generate(self.root, self.output)
        self.assertEqual(result["git_commit"], self.git("rev-parse", "HEAD"))
        self.assertEqual(result["git_state"], "unknown")

    def test_invalid_metadata_cannot_replace_previous_header(self):
        identity.generate(self.root, self.output)
        previous = self.output.read_bytes()
        original = json.loads(self.metadata.read_text())
        for key, value in (("schema_version", True), ("developer", "x" * 32),
                           ("developer", "\ncontrol"), ("repository_url", "https://user:secret@example.com/repo"),
                           ("repository_url", "https://example.com/repo?token=secret"),
                           ("repository_url", "https://example.com/a b")):
            with self.subTest(key=key, value=value):
                self.metadata.write_text(json.dumps(dict(original, **{key: value})))
                with self.assertRaises(ValueError):
                    identity.generate(self.root, self.output)
                self.assertEqual(self.output.read_bytes(), previous)

    def test_quoted_developer_compiles_and_preserves_value(self):
        metadata = json.loads(self.metadata.read_text())
        metadata["developer"] = 'A "B" \\ C'
        self.metadata.write_text(json.dumps(metadata))
        identity.generate(self.root, self.output)
        compiler = shutil.which("clang") or shutil.which("cc")
        self.assertIsNotNone(compiler, "A C compiler is required by the host validation contract")
        source = self.output.parent / "probe.c"
        source.write_text('#include "app_build_identity.h"\n#include <stdio.h>\n'
                          'int main(void) { puts(APP_BUILD_DEVELOPER); return 0; }\n')
        binary = source.with_suffix("")
        subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(binary)], check=True)
        self.assertEqual(subprocess.check_output([str(binary)], text=True).rstrip("\n"), metadata["developer"])

    def test_repository_url_and_developer_contract(self):
        original = json.loads(self.metadata.read_text())
        accepted = (
            "https://github.com/example/firmware",
            "https://git.example.com:8443/team/project",
            "https://git.example.com:00001/a",
            "https://git.example.com:65535/a%20b/repo",
            "https://Git-Host.example/a~b!$&'()*+,;=:@_-./project",
        )
        rejected = (
            "https://github..com/a/b", "https://-git.example.com/a",
            "https://git-.example.com/a", "https://git.example.com./a",
            "https://localhost/a", "https://[::1]/a", "HTTPS://git.example.com/a",
            "https://git.example.com", "https://git.example.com/",
            "https://git.example.com:0/a", "https://git.example.com:65536/a",
            "https://git.example.com:000001/a", "https://git.example.com:/a",
            "https://user@git.example.com/a", "https://user:secret@git.example.com/a",
            "https://git.example.com/a?token=secret", "https://git.example.com/a#fragment",
            "https://git.example.com/a?", "https://git.example.com/a#",
            "https://git.example.com/a%", "https://git.example.com/a%2Z",
            "https://git.example.com/a b", "https://git.example.com/a\\b",
            'https://git.example.com/a"b', "https://git.example.com/a[b]",
            " https://git.example.com/a", "https://git.example.com/a ",
        )
        for url in accepted:
            with self.subTest(accepted=url):
                self.metadata.write_text(json.dumps(dict(original, repository_url=url)))
                self.assertEqual(identity.generate(self.root, self.output)["repository_url"], url)
        previous = self.output.read_bytes()
        for url in rejected:
            with self.subTest(rejected=url):
                self.metadata.write_text(json.dumps(dict(original, repository_url=url)))
                with self.assertRaises(ValueError):
                    identity.generate(self.root, self.output)
                self.assertEqual(self.output.read_bytes(), previous)
        for developer in (" Author", "Author ", " ", "", "Author\tName"):
            with self.subTest(developer=developer):
                self.metadata.write_text(json.dumps(dict(original, developer=developer)))
                with self.assertRaises(ValueError):
                    identity.generate(self.root, self.output)
                self.assertEqual(self.output.read_bytes(), previous)


if __name__ == "__main__":
    unittest.main()
