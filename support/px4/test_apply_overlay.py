"""Safety regression tests; only disposable fake Git checkouts are changed."""
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

SCRIPT = Path(__file__).with_name("apply_overlay.py")
spec = importlib.util.spec_from_file_location("px4_overlay", SCRIPT)
overlay = importlib.util.module_from_spec(spec)
spec.loader.exec_module(overlay)


def git(path, *args):
    result = subprocess.run(["git", "-C", str(path), *args], stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, check=True)
    return result.stdout


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class OverlaySafety(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="overlay-safety-")
        self.base = Path(self.temporary.name)
        self.root = self.base / "PX4"
        self.root.mkdir()
        self.mav = self.root / "src/modules/mavlink/mavlink"
        self.mav.mkdir(parents=True)
        self.package = self.base / "package"
        self.package.mkdir()
        for directory in (self.root, self.mav):
            git(directory, "init", "--quiet")
            git(directory, "config", "core.autocrlf", "false")
            git(directory, "config", "user.name", "Overlay Test")
            git(directory, "config", "user.email", "overlay-test@example.invalid")
            git(directory, "config", "commit.gpgsign", "false")
            (directory / "init.txt").write_bytes(b"stage=old\n")
            git(directory, "add", "init.txt")
            git(directory, "-c", "core.hooksPath=/dev/null", "commit", "--quiet", "--no-verify", "-m", "fixture")
        self.module = self.package / "SHTC3.cpp"
        self.module.write_bytes(b"/* fixture module */\n")
        self.target = self.root / "src/drivers/hygrometer/shtc3/SHTC3.cpp"
        self.manifest = {
            "format": 1, "version": "test", "package_root": ".",
            "repositories": {
                "px4": {"path": ".", "commit": git(self.root,"rev-parse","HEAD").decode().strip()},
                "mavlink": {"path": "src/modules/mavlink/mavlink", "commit": git(self.mav,"rev-parse","HEAD").decode().strip()}
            },
            "patches": [],
            "overlays": [{"repository":"px4","source":"SHTC3.cpp",
                          "target":"src/drivers/hygrometer/shtc3/SHTC3.cpp", "sha256":sha(self.module)}]
        }
        for name, repo in (("px4", self.root), ("mavlink", self.mav)):
            (repo / "init.txt").write_bytes(b"stage=new\n")
            patch = self.package / (name + ".patch")
            patch.write_bytes(git(repo,"diff","--no-ext-diff","--","init.txt"))
            (repo / "init.txt").write_bytes(b"stage=old\n")
            self.manifest["patches"].append({"repository":name,"source":patch.name,"sha256":sha(patch)})
        self.manifest_path = self.package / "manifest.json"
        self.save()

    def tearDown(self):
        self.temporary.cleanup()

    def save(self):
        self.manifest_path.write_text(json.dumps(self.manifest),encoding="utf-8")

    def command(self, *args):
        return subprocess.run([sys.executable,str(SCRIPT),"--px4",str(self.root),
                               "--manifest",str(self.manifest_path),*args],
                              stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)

    def original(self):
        self.assertEqual((self.root/"init.txt").read_bytes(),b"stage=old\n")
        self.assertEqual((self.mav/"init.txt").read_bytes(),b"stage=old\n")
        self.assertFalse(self.target.exists())
        self.assertFalse((self.root/".weather-overlay.lock").exists())

    def test_default_check_is_read_only(self):
        result=self.command()
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertIn("3 file(s) would change",result.stdout)
        self.assertIn("CHECK ONLY",result.stdout)
        self.original()

    def test_apply_is_idempotent_and_preserves_unrelated_files(self):
        unrelated=self.root/"operator-notes.txt"
        unrelated.write_bytes(b"keep me\n")
        first=self.command("--apply")
        self.assertEqual(first.returncode,0,first.stderr)
        self.assertEqual((self.root/"init.txt").read_bytes(),b"stage=new\n")
        self.assertEqual((self.mav/"init.txt").read_bytes(),b"stage=new\n")
        self.assertEqual(self.target.read_bytes(),self.module.read_bytes())
        second=self.command("--apply")
        self.assertEqual(second.returncode,0,second.stderr)
        self.assertIn("0 file(s) would change",second.stdout)
        self.assertEqual(unrelated.read_bytes(),b"keep me\n")
        self.assertFalse((self.root/".weather-overlay.lock").exists())

    def test_changed_package_hash_blocks_every_write(self):
        self.module.write_bytes(b"changed package source\n")
        result=self.command("--apply")
        self.assertNotEqual(result.returncode,0)
        self.assertIn("SHA256 mismatch",result.stderr)
        self.original()

    def test_different_existing_module_is_never_overwritten(self):
        self.target.parent.mkdir(parents=True)
        self.target.write_bytes(b"operator changes\n")
        result=self.command("--apply")
        self.assertNotEqual(result.returncode,0)
        self.assertIn("Refusing to overwrite",result.stderr)
        self.assertEqual(self.target.read_bytes(),b"operator changes\n")
        self.assertEqual((self.root/"init.txt").read_bytes(),b"stage=old\n")

    def test_patch_conflict_blocks_every_write(self):
        (self.mav/"init.txt").write_bytes(b"operator changes\n")
        result=self.command("--apply")
        self.assertNotEqual(result.returncode,0)
        self.assertIn("Patch context conflicts",result.stderr)
        self.assertEqual((self.root/"init.txt").read_bytes(),b"stage=old\n")
        self.assertEqual((self.mav/"init.txt").read_bytes(),b"operator changes\n")
        self.assertFalse(self.target.exists())

    def test_revision_mismatch_requires_explicit_fresh_revision_check(self):
        self.manifest["repositories"]["px4"]["commit"]="0"*40
        self.save()
        refused=self.command("--apply")
        self.assertNotEqual(refused.returncode,0)
        self.assertIn("revision differs",refused.stderr)
        allowed=self.command("--check","--allow-other-revision")
        self.assertEqual(allowed.returncode,0,allowed.stderr)
        self.assertIn("unverified upstream revision",allowed.stdout)
        self.original()

    def test_parent_traversal_is_rejected(self):
        self.manifest["overlays"][0]["target"]="../escape.cpp"
        self.save()
        result=self.command("--apply")
        self.assertNotEqual(result.returncode,0)
        self.assertIn("Unsafe relative path",result.stderr)
        self.assertFalse((self.base/"escape.cpp").exists())
        self.original()

    def test_apply_failure_rolls_back_prior_file_writes(self):
        _,_,originals,changes=overlay.load_plan(self.root,self.manifest_path,False)
        original_write=overlay.atomic_write
        failed=[False]
        def fail_once(path,data,mode):
            if path==self.target and not failed[0]:
                failed[0]=True
                raise OSError("injected storage failure")
            return original_write(path,data,mode)
        with mock.patch.object(overlay,"atomic_write",side_effect=fail_once):
            with self.assertRaisesRegex(OSError,"injected storage failure"):
                overlay.apply_plan(self.root,originals,changes)
        self.assertTrue(failed[0])
        self.original()

    def test_hardware_profile_omits_sitl_host_bus_patch(self):
        patch=self.package/"hostbus.patch"
        patch.write_bytes(b"diff --git a/hostbus.txt b/hostbus.txt\nnew file mode 100644\n--- /dev/null\n+++ b/hostbus.txt\n@@ -0,0 +1 @@\n+native host bus 1\n")
        self.manifest["patches"].append({"repository":"px4","source":patch.name,"sha256":sha(patch),"profiles":["sitl"]})
        self.save()
        hardware=self.command("--apply","--profile","hardware")
        self.assertEqual(hardware.returncode,0,hardware.stderr)
        self.assertFalse((self.root/"hostbus.txt").exists())
        self.assertEqual(self.target.read_bytes(),self.module.read_bytes())
        sitl=self.command("--apply","--profile","sitl")
        self.assertEqual(sitl.returncode,0,sitl.stderr)
        self.assertEqual((self.root/"hostbus.txt").read_bytes(),b"native host bus 1\n")
        again=self.command("--check")
        self.assertEqual(again.returncode,0,again.stderr)
        self.assertIn("0 file(s) would change",again.stdout)

    def test_sequential_same_file_patches_remain_idempotent(self):
        (self.root/"init.txt").write_bytes(b"stage=new\n")
        git(self.root,"add","init.txt")
        git(self.root,"-c","core.hooksPath=/dev/null","commit","--quiet","--no-verify","-m","temporary diff base")
        (self.root/"init.txt").write_bytes(b"stage=final\n")
        patch=self.package/"second.patch"
        patch.write_bytes(git(self.root,"diff","--no-ext-diff","--","init.txt"))
        self.manifest["repositories"]["px4"]["commit"]=git(self.root,"rev-parse","HEAD").decode().strip()
        self.manifest["patches"].append({"repository":"px4","source":patch.name,"sha256":sha(patch)})
        (self.root/"init.txt").write_bytes(b"stage=old\n")
        self.save()
        first=self.command("--apply")
        self.assertEqual(first.returncode,0,first.stderr)
        self.assertEqual((self.root/"init.txt").read_bytes(),b"stage=final\n")
        second=self.command("--apply")
        self.assertEqual(second.returncode,0,second.stderr)
        self.assertIn("0 file(s) would change",second.stdout)


if __name__=="__main__":
    unittest.main(verbosity=2)
