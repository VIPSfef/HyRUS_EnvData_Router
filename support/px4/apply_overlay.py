#!/usr/bin/env python3
"""Check/apply the versioned PX4 weather overlay without resets or forced writes.

The default is --check. All patches first run against disposable file copies.
Only absent or byte-identical module overlay files are accepted. Applying writes
atomically and restores the original files if an operation fails. Use a fresh
checkout when updating upstream; this tool never pulls, builds or flashes.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tempfile


class OverlayError(Exception):
    pass


def digest(data):
    return hashlib.sha256(data).hexdigest()


def run_git(directory, *args):
    try:
        return subprocess.run(["git", "-c", "core.autocrlf=false", "-C", str(directory), *args],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              text=True, encoding="utf-8", errors="replace")
    except OSError as exc:
        raise OverlayError("Cannot run git: " + str(exc)) from exc


def safe_path(root, name):
    """Refuse absolute paths, parent traversal, Git internals and symlinks."""
    if not isinstance(name, str) or not name or "\\" in name:
        raise OverlayError("Invalid relative path: " + repr(name))
    rel = Path(name)
    if rel.is_absolute() or any(part in ("..", ".git") for part in rel.parts):
        raise OverlayError("Unsafe relative path: " + name)
    result = root / rel
    cursor = root
    for part in rel.parts:
        cursor = cursor / part
        if cursor.is_symlink():
            raise OverlayError("Symlinks are not overlay targets: " + str(cursor))
    try:
        result.resolve().relative_to(root.resolve())
    except ValueError as exc:
        raise OverlayError("Path escapes root: " + name) from exc
    if result.exists() and not result.is_file():
        raise OverlayError("Expected a regular file: " + str(result))
    return result


def package_file(package_root, name, expected_hash):
    path = safe_path(package_root, name)
    if not path.is_file():
        raise OverlayError("Missing package file: " + str(path))
    data = path.read_bytes()
    if not isinstance(expected_hash, str) or not re.fullmatch(r"[0-9a-f]{64}", expected_hash):
        raise OverlayError("Invalid SHA256 in manifest for " + name)
    if digest(data) != expected_hash:
        raise OverlayError("Package SHA256 mismatch: " + name)
    return path, data


def patch_targets(data):
    # Overlay patches use ordinary same-path unified diffs, never renames,
    # binary patches or quoted filenames. Refuse unknown forms conservatively.
    text = data.decode("utf-8")
    if "GIT binary patch" in text or "rename from " in text or "deleted file mode " in text or "old mode " in text:
        raise OverlayError("Overlay patches cannot rename/delete/binary-patch files")
    names = []
    for line in text.splitlines():
        if not line.startswith("diff --git "):
            continue
        match = re.fullmatch(r"diff --git a/([^\s]+) b/([^\s]+)", line)
        if not match or match.group(1) != match.group(2):
            raise OverlayError("Unsupported patch path: " + line)
        if match.group(1) not in names:
            names.append(match.group(1))
    if not names:
        raise OverlayError("Patch has no ordinary file changes")
    return names


def read_existing(path):
    if not path.exists():
        return None, None
    if not path.is_file() or path.is_symlink():
        raise OverlayError("Not a regular file: " + str(path))
    return path.read_bytes(), stat.S_IMODE(path.stat().st_mode)


def atomic_write(path, data, mode):
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".weather-overlay-", dir=str(path.parent))
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(temporary, mode)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def load_plan(px4_root, manifest_path, allow_other_revision, profile="sitl"):
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise OverlayError("Cannot read overlay manifest: " + str(exc)) from exc
    if not isinstance(manifest, dict) or manifest.get("format") != 1:
        raise OverlayError("Unsupported manifest format")
    if profile not in manifest.get("profiles", ["sitl", "hardware"]):
        raise OverlayError("Unsupported overlay profile: " + profile)
    package_root = (manifest_path.parent / manifest.get("package_root", "../..")).resolve()
    repos = {}
    messages = []
    for name, spec in manifest.get("repositories", {}).items():
        relative = spec["path"]
        repo = px4_root if relative == "." else safe_path(px4_root, relative + "/.overlay-probe").parent
        if not repo.is_dir():
            raise OverlayError("Repository directory is missing: " + str(repo))
        top = run_git(repo, "rev-parse", "--show-toplevel")
        head = run_git(repo, "rev-parse", "HEAD")
        if top.returncode or head.returncode or Path(top.stdout.strip()).resolve() != repo.resolve():
            raise OverlayError("Expected a Git repository root: " + str(repo))
        actual = head.stdout.strip()
        if actual != spec["commit"]:
            if not allow_other_revision:
                raise OverlayError(name + " revision differs from the manifest; use a fresh pinned checkout or explicitly check --allow-other-revision")
            messages.append(name + ": unverified upstream revision " + actual)
        repos[name] = repo
    if "px4" not in repos:
        raise OverlayError("Manifest has no px4 repository")
    patches = []
    targets = {}
    for spec in manifest.get("patches", []):
        if "profiles" in spec and profile not in spec["profiles"]:
            continue
        if spec["repository"] not in repos:
            raise OverlayError("Unknown patch repository")
        source, data = package_file(package_root, spec["source"], spec["sha256"])
        names = patch_targets(data)
        repo_name = spec["repository"]
        for name in names:
            target = safe_path(repos[repo_name], name)
            previous = targets.get(target)
            if previous and previous != (repo_name, name):
                raise OverlayError("Different repositories target the same path")
            targets[target] = (repo_name, name)
        patches.append((repo_name, source, names))
    overlays = []
    for spec in manifest.get("overlays", []):
        repo_name = spec["repository"]
        if repo_name not in repos:
            raise OverlayError("Unknown overlay repository")
        _, data = package_file(package_root, spec["source"], spec["sha256"])
        target = safe_path(repos[repo_name], spec["target"])
        current, _ = read_existing(target)
        if current is not None and current != data:
            raise OverlayError("Refusing to overwrite a different module file: " + str(target))
        targets[target] = (repo_name, spec["target"])
        overlays.append((repo_name, spec["target"], data))
    originals = {path: read_existing(path) for path in targets}
    final = {}
    with tempfile.TemporaryDirectory(prefix="px4-weather-check-") as temporary:
        scratches = {}
        for name in repos:
            directory = Path(temporary) / name
            directory.mkdir()
            result = run_git(directory, "init", "--quiet")
            if result.returncode:
                raise OverlayError("Cannot initialize disposable patch check")
            scratches[name] = directory
        for target, (repo_name, name) in targets.items():
            data, mode = originals[target]
            if data is not None:
                scratch = safe_path(scratches[repo_name], name)
                scratch.parent.mkdir(parents=True, exist_ok=True)
                scratch.write_bytes(data)
                os.chmod(scratch, mode)
        # Reverse the whole series in reverse order first. This also detects
        # idempotence when two patches change adjacent lines in one file.
        already_all = bool(patches)
        for repo_name, source, _ in reversed(patches):
            directory = scratches[repo_name]
            if run_git(directory, "apply", "--reverse", "--check", str(source)).returncode:
                already_all = False
                break
            reversed_patch = run_git(directory, "apply", "--reverse", str(source))
            if reversed_patch.returncode:
                already_all = False
                break
        # Reconstruct the disposable initial contents after the reverse probe.
        for target, (repo_name, name) in targets.items():
            scratch = safe_path(scratches[repo_name], name)
            if scratch.exists():
                scratch.unlink()
            data, mode = originals[target]
            if data is not None:
                scratch.parent.mkdir(parents=True, exist_ok=True)
                scratch.write_bytes(data)
                os.chmod(scratch, mode)
        for repo_name, source, _ in patches:
            directory = scratches[repo_name]
            if already_all:
                messages.append(source.name + ": already applied")
                continue
            forward = run_git(directory, "apply", "--check", str(source))
            if not forward.returncode:
                applied = run_git(directory, "apply", str(source))
                if applied.returncode:
                    raise OverlayError("Disposable patch application failed: " + source.name)
                messages.append(source.name + ": ready")
            elif not run_git(directory, "apply", "--reverse", "--check", str(source)).returncode:
                messages.append(source.name + ": already applied")
            else:
                raise OverlayError("Patch context conflicts: " + source.name + "\n" + forward.stderr.strip())
        for repo_name, name, data in overlays:
            scratch = safe_path(scratches[repo_name], name)
            if scratch.exists() and scratch.read_bytes() != data:
                raise OverlayError("Patch and module overlay disagree: " + name)
            scratch.parent.mkdir(parents=True, exist_ok=True)
            scratch.write_bytes(data)
        for target, (repo_name, name) in targets.items():
            scratch = safe_path(scratches[repo_name], name)
            if not scratch.is_file():
                raise OverlayError("Patch removed a required file: " + name)
            final[target] = scratch.read_bytes()
    changes = [(path, data, originals[path][1] if originals[path][1] is not None else 0o644)
               for path, data in final.items() if originals[path][0] != data]
    return manifest, messages, originals, changes


def apply_plan(px4_root, originals, changes):
    lock_path = px4_root / ".weather-overlay.lock"
    try:
        descriptor = os.open(lock_path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    except FileExistsError as exc:
        raise OverlayError("Another overlay operation holds " + str(lock_path)) from exc
    os.close(descriptor)
    written = []
    try:
        for path, (before, _) in originals.items():
            current, _ = read_existing(path)
            if current != before:
                raise OverlayError("File changed since preflight: " + str(path))
        for path, data, mode in changes:
            # Refuse newly introduced symlinks between check and write.
            relative = path.relative_to(px4_root).as_posix()
            safe_path(px4_root, relative)
            current, _ = read_existing(path)
            if current != originals[path][0]:
                raise OverlayError("File changed during apply: " + str(path))
            atomic_write(path, data, mode)
            written.append(path)
    except Exception:
        for path in reversed(written):
            before, mode = originals[path]
            if before is None:
                path.unlink()
            else:
                atomic_write(path, before, mode)
        raise
    finally:
        lock_path.unlink()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--px4", required=True, type=Path, help="PX4 checkout root")
    parser.add_argument("--manifest", type=Path, default=Path(__file__).with_name("overlay-manifest.json"))
    parser.add_argument("--profile", choices=("sitl", "hardware"), default="sitl",
                        help="sitl adds native Linux I2C validation; hardware omits that patch")
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--check", action="store_true", help="check only (default)")
    modes.add_argument("--apply", action="store_true", help="apply after all preflight checks")
    parser.add_argument("--allow-other-revision", action="store_true",
                        help="explicitly inspect a fresh upstream revision; context check is not build compatibility")
    args = parser.parse_args(argv)
    try:
        root = args.px4.resolve()
        manifest, messages, originals, changes = load_plan(root, args.manifest.resolve(), args.allow_other_revision, args.profile)
        for message in messages:
            print(message)
        print("Overlay " + str(manifest.get("version", "unknown")) + " [" + args.profile + "]: " + str(len(changes)) + " file(s) would change")
        if args.apply:
            if changes:
                apply_plan(root, originals, changes)
            print("Applied. Review git diff in PX4 and its MAVLink submodule before building.")
        else:
            print("CHECK ONLY: PX4 files were not changed. Use --apply to install this exact plan.")
        return 0
    except (OverlayError, OSError, KeyError, TypeError, ValueError, UnicodeError) as exc:
        print("Overlay error: " + str(exc), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
