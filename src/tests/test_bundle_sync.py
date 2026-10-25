"""Offline integration checks for the OneDrive Git-bundle helper.

Run with Python 3.9+ and installed Git/PowerShell; no network or live router is
used. Every repository contains synthetic text and lives in an owned temporary
folder. Example: python src/tests/test_bundle_sync.py --powershell powershell.exe
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[2] / "support/sharing/Sync-MavlinkRouter.ps1"
POWERSHELL = None
ENV = dict(os.environ, GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull,
           GIT_TERMINAL_PROMPT="0")


def run(command, *, cwd=None, environment=None):
    return subprocess.run([str(part) for part in command], cwd=cwd,
                          env=ENV if environment is None else environment,
                          capture_output=True, text=True, encoding="utf-8",
                          errors="replace", timeout=30)


def git(directory, *arguments):
    result = run(["git", "-C", directory, *arguments])
    if result.returncode:
        raise AssertionError("Git failed: {}\n{}\n{}".format(
            " ".join(arguments), result.stdout, result.stderr))
    return result.stdout.strip()


def commit(directory, content):
    (directory / "version.txt").write_text(content, encoding="utf-8")
    git(directory, "add", "version.txt")
    git(directory, "-c", "user.name=Bundle Test", "-c",
        "user.email=bundle-test@example.invalid", "commit", "-m", content)
    return git(directory, "rev-parse", "HEAD")


class BundleSyncTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="mavlink bundle ")
        cls.root = Path(cls.temporary.name).resolve() / "한글 경로"
        cls.root.mkdir()
        source = cls.root / "source repository"
        source.mkdir()
        git(source, "init", "-b", "main")
        git(source, "config", "core.autocrlf", "false")
        (source / "docs").mkdir()
        (source / "docs/README.md").write_text("Synthetic test documentation.\n", encoding="utf-8")
        git(source, "add", "docs")
        cls.commit_a = commit(source, "Synthetic version A\n")
        cls.bundle_a = cls.root / "version A.bundle"
        git(source, "bundle", "create", cls.bundle_a, "main")
        cls.commit_b = commit(source, "Synthetic version B\n")
        cls.bundle_b = cls.root / "version B.bundle"
        git(source, "bundle", "create", cls.bundle_b, "main")

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def setUp(self):
        self.case = self.root / self._testMethodName
        self.case.mkdir()
        self.shared = self.case / "OneDrive 공유 폴더"
        self.shared.mkdir()
        self.cache = self.case / "local cache"
        self.destination = self.case / "working repository"
        for bundle in (self.bundle_a, self.bundle_b):
            shutil.copyfile(bundle, self.shared / bundle.name)
        self.publish("a")

    def publish(self, version, **overrides):
        bundle = self.bundle_a if version == "a" else self.bundle_b
        manifest = {
            "format": "mavlink-router-handoff/v1", "source_branch": "main",
            "source_commit": self.commit_a if version == "a" else self.commit_b,
            "bundle": {"file": bundle.name, "sha256": hashlib.sha256(
                bundle.read_bytes()).hexdigest(), "bytes": bundle.stat().st_size},
        }
        for key, value in overrides.items():
            if key.startswith("bundle_"):
                manifest["bundle"][key[7:]] = value
            else:
                manifest[key] = value
        (self.shared / "LATEST.json").write_text(
            json.dumps(manifest, ensure_ascii=False), encoding="utf-8")

    def sync(self, expected=0, destination=None, script=None, default_shared=False):
        command = [POWERSHELL, "-NoLogo", "-NoProfile", "-NonInteractive",
                   "-ExecutionPolicy", "Bypass", "-File", script or SCRIPT,
                   "-Destination", destination or self.destination,
                   "-CacheDirectory", self.cache]
        if not default_shared:
            command.extend(["-SharedDirectory", self.shared])
        result = run(command)
        self.assertEqual(result.returncode, expected,
                         "Unexpected helper result:\n" + result.stdout + result.stderr)
        return result

    def snapshot(self):
        """User refs, checked-out branch and remote configuration must survive."""
        return (git(self.destination, "rev-parse", "HEAD"),
                git(self.destination, "symbolic-ref", "HEAD"),
                git(self.destination, "for-each-ref", "--format=%(refname) %(objectname)"),
                (self.destination / ".git/config").read_bytes())

    def refuse_unchanged(self, destination=None):
        before = self.snapshot()
        self.sync(1, destination)
        self.assertEqual(self.snapshot(), before)
        self.assertEqual(git(self.destination, "for-each-ref",
                             "--format=%(refname)", "refs/mavlink-router-sync"), "")

    def test_first_clone_and_repeated_run(self):
        self.sync()
        self.assertEqual(git(self.destination, "rev-parse", "HEAD"), self.commit_a)
        self.assertEqual((self.destination / "version.txt").read_text(encoding="utf-8"),
                         "Synthetic version A\n")
        self.assertIn("onedrive", git(self.destination, "remote").splitlines())
        self.sync()
        self.assertEqual(git(self.destination, "rev-parse", "HEAD"), self.commit_a)
        self.assertEqual(git(self.destination, "status", "--porcelain"), "")
        self.assertEqual(git(self.destination, "rev-parse", "onedrive/main"), self.commit_a)
        self.assertEqual(git(self.destination, "rev-list", "--left-right", "--count",
                             "main...onedrive/main"), "0\t0")

    def test_fast_forward_update(self):
        self.sync()
        self.publish("b")
        self.sync()
        self.assertEqual(git(self.destination, "rev-parse", "HEAD"), self.commit_b)
        self.assertEqual((self.destination / "version.txt").read_text(encoding="utf-8"),
                         "Synthetic version B\n")
        self.assertEqual(git(self.destination, "status", "--porcelain"), "")
        self.assertEqual(git(self.destination, "rev-parse", "onedrive/main"), self.commit_b)
        self.assertEqual(git(self.destination, "rev-list", "--left-right", "--count",
                             "main...onedrive/main"), "0\t0")

    def test_script_folder_is_default_shared_directory(self):
        copied_script = self.shared / "Sync-MavlinkRouter.ps1"
        shutil.copyfile(SCRIPT, copied_script)
        self.sync(script=copied_script, default_shared=True)
        self.assertEqual(git(self.destination, "rev-parse", "HEAD"), self.commit_a)
        self.sync(script=copied_script, default_shared=True)
        self.publish("b")
        self.sync(script=copied_script, default_shared=True)
        self.assertEqual(git(self.destination, "rev-parse", "HEAD"), self.commit_b)
        self.assertEqual(git(self.destination, "rev-parse", "onedrive/main"), self.commit_b)
        self.assertEqual(git(self.destination, "status", "--porcelain"), "")
        # Zero-argument launch must also honor a separate user home/cache. Only
        # this child process receives these paths; no actual user folder is used.
        fake_home = self.case / "isolated user home"
        fake_local = self.case / "isolated local appdata"
        fake_home.mkdir()
        fake_local.mkdir()
        environment = dict(ENV, USERPROFILE=str(fake_home), LOCALAPPDATA=str(fake_local))
        result = run([POWERSHELL, "-NoLogo", "-NoProfile", "-NonInteractive",
                      "-ExecutionPolicy", "Bypass", "-File", copied_script],
                     environment=environment)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        default_destination = fake_home / "dev/mavlink_telemetry_data_router"
        self.assertEqual(git(default_destination, "rev-parse", "HEAD"), self.commit_b)
        self.assertTrue((fake_local / "mavlink-router/bundles").is_dir())

    def test_incomplete_bundled_git_is_not_replaced_by_installed_git(self):
        # A partially extracted runtime must not silently select a different Git.
        # This exercises the precedence rule with the real installed Git on PATH.
        copied_script = self.shared / "Sync-MavlinkRouter.ps1"
        shutil.copyfile(SCRIPT, copied_script)
        (self.shared / "runtime/git").mkdir(parents=True)
        result = self.sync(1, script=copied_script, default_shared=True)
        self.assertIn("ZIP 전체를 새 폴더에 풀었는지 확인하세요", result.stdout)
        self.assertFalse(self.destination.exists())

    def test_dirty_tracked_file_is_preserved(self):
        self.sync()
        self.publish("b")
        file = self.destination / "version.txt"
        file.write_text("Uncommitted local work.\n", encoding="utf-8")
        self.refuse_unchanged()
        self.assertEqual(file.read_text(encoding="utf-8"), "Uncommitted local work.\n")

    def test_untracked_file_is_preserved(self):
        self.sync()
        self.publish("b")
        file = self.destination / "local notes.txt"
        file.write_text("Untracked local work.\n", encoding="utf-8")
        self.refuse_unchanged()
        self.assertEqual(file.read_text(encoding="utf-8"), "Untracked local work.\n")

    def test_other_branch_is_preserved(self):
        self.sync()
        git(self.destination, "switch", "-c", "developer-work")
        self.publish("b")
        self.refuse_unchanged()

    def test_local_ahead_is_preserved(self):
        self.sync()
        local = commit(self.destination, "Local commit ahead of shared A.\n")
        self.refuse_unchanged()
        self.assertEqual(git(self.destination, "rev-parse", "HEAD"), local)

    def test_divergence_is_preserved(self):
        self.sync()
        commit(self.destination, "Local branch diverges from shared B.\n")
        self.publish("b")
        self.refuse_unchanged()

    def test_nonrepository_nonempty_folder_is_preserved(self):
        self.destination.mkdir()
        file = self.destination / "existing.txt"
        file.write_text("Keep this file.\n", encoding="utf-8")
        self.sync(1)
        self.assertEqual(list(self.destination.iterdir()), [file])
        self.assertEqual(file.read_text(encoding="utf-8"), "Keep this file.\n")

    def test_nested_destination_is_refused(self):
        self.sync()
        self.publish("b")
        self.refuse_unchanged(self.destination / "docs")

    def test_corrupt_sha_is_rejected_before_repository_mutation(self):
        self.sync()
        self.publish("b", bundle_sha256="0" * 64)
        self.refuse_unchanged()

    def test_manifest_commit_mismatch_is_rejected(self):
        self.sync()
        self.publish("b", source_commit=self.commit_a)
        self.refuse_unchanged()

    def test_traversal_bundle_name_is_rejected(self):
        self.publish("a", bundle_file="../version A.bundle")
        self.sync(1)
        self.assertFalse(self.destination.exists())


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--powershell", default=shutil.which("powershell.exe") or
                        shutil.which("pwsh") or shutil.which("pwsh.exe"))
    parser.add_argument("--script", type=Path, default=SCRIPT)
    args = parser.parse_args()
    POWERSHELL = args.powershell
    SCRIPT = args.script.resolve()
    if not POWERSHELL or not shutil.which("git") or not SCRIPT.is_file():
        parser.error("Installed Git, PowerShell and the sharing script are required.")
    unittest.main(argv=[sys.argv[0]], verbosity=2)
