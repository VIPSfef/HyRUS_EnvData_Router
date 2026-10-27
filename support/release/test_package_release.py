#!/usr/bin/env python3
"""Offline packaging regressions; no device, socket, or production config access."""
import importlib.util
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import zipfile


spec = importlib.util.spec_from_file_location("package_release", Path(__file__).with_name("package_release.py"))
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


class ReleaseTests(unittest.TestCase):
    def test_public_source_boundary(self):
        for name in ("src/vendor/mavlink/protocol.h", "src/new_feature.c", "config/telemetry.ini.example",
                     ".github/workflows/ci.yml", "release/README.md"):
            self.assertTrue(release.source_allowed(name), name)
        for name in ("build/live.json", "release/v0.11.0/manifest.json", "src/.env", "src/.env.production",
                     "src/custom.ini", "src/custom.env", "docs/capture.tlog", "src/private/data.json", "src/key.pem",
                     "src/foo.ini.tmp.123", "../src/foo.c", "src/../../secret", "src/prog.exe",
                     "src/px4/shtc3/SHTC3.cpp", "support/px4/overlay-manifest.json",
                     "support/PX4/px4-sens-atmos.patch"):
            self.assertFalse(release.source_allowed(name), name)

    def test_working_tree_snapshot_includes_untracked_changes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            (root / "src").mkdir()
            (root / "src/new.c").write_bytes(b"current work\r\n")
            (root / "src/custom.ini").write_text("token=DO-NOT-PACK", encoding="utf-8")
            with patch.object(release, "git", return_value=b"src/new.c\0src/deleted.c\0src/custom.ini\0") as git:
                entries = release.source_entries(root)
            self.assertEqual(entries, [("src/new.c", b"current work\n", 0o644)])
            self.assertIn("--others", git.call_args.args)
            self.assertIn("--cached", git.call_args.args)

    def test_deterministic_archives_and_modes(self):
        entries = [("package/start.sh", b"#!/bin/sh\n", 0o755),
                   ("package/config/telemetry.ini.example", b"token=\n", 0o644)]
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for function, extension in ((release.zip_entries, ".zip"), (release.tar_entries, ".tar.gz")):
                a, b = root / ("a" + extension), root / ("b" + extension)
                function(a, entries)
                function(b, entries)
                self.assertEqual(a.read_bytes(), b.read_bytes())
            with zipfile.ZipFile(root / "a.zip") as archive:
                self.assertEqual(archive.getinfo("package/start.sh").external_attr >> 16 & 0o777, 0o755)

    def test_archive_rejects_runtime_credentials_and_traversal(self):
        for name in ("../secret", "/absolute", "C:/secret", "package/telemetry.ini", "package/.env", "bad\\name"):
            with self.assertRaises(ValueError):
                release.safe_archive_name(name)

    def test_native_version_must_match_before_packaging(self):
        with tempfile.TemporaryDirectory() as temporary:
            binary = Path(temporary) / "router"
            header = bytearray(64)
            header[:6] = b"\x7fELF\x02\x01"
            struct.pack_into("<H", header, 18, 62)
            binary.write_bytes(header)
            with patch.object(release.sys, "platform", "linux"), patch.object(release.subprocess, "run") as run:
                run.return_value = subprocess.CompletedProcess([], 0, stdout=b"router 0.10.0\n")
                with self.assertRaises(ValueError):
                    release.verify_binary(binary, "linux-x64", "0.11.0")
                run.return_value = subprocess.CompletedProcess([], 0, stdout=b"router 0.11.0\n")
                self.assertTrue(release.verify_binary(binary, "linux-x64", "0.11.0")["native_version_checked"])
            header[18:20] = b"\xb7\x00"  # AArch64 is not the requested x64.
            binary.write_bytes(header)
            with self.assertRaises(ValueError):
                release.verify_binary(binary, "linux-x64", "0.11.0")

    def test_existing_unrelated_runtime_files_are_preserved(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "new"
            source.write_text("generated", encoding="utf-8")
            target = root / "output/generated.txt"
            target.parent.mkdir()
            private = target.parent / "telemetry.ini"
            private.write_text("user setting", encoding="utf-8")
            release.replace_file(source, target)
            self.assertEqual(private.read_text(encoding="utf-8"), "user setting")
            self.assertEqual(target.read_text(encoding="utf-8"), "generated")


if __name__ == "__main__":
    unittest.main()
