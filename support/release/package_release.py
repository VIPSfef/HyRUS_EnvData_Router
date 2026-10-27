#!/usr/bin/env python3
"""Prepare clean runtime assets and the current working-tree source snapshot.

No upload, Git mutation, device access, or running-process control is performed.
Only --version is executed, and only for binaries native to the packaging host.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import gzip
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
import zipfile


PROJECT = "mavlink_telemetry_data_router"
PLATFORMS = ("windows-x64", "linux-x64")
SOURCE_DIRS = {".github", "cmake", "config", "docs", "examples", "src", "support"}
SOURCE_ROOTS = {
    ".gitattributes", ".gitignore", ".gitlab-ci.yml", "CMakeLists.txt",
    "CMakePresets.json", "README.md", "install.sh", "LICENSE", "LICENSE.md",
    "LICENSE.txt", "NOTICE", "NOTICE.md",
}
PRIVATE_SUFFIXES = {
    ".ini", ".env", ".log", ".bundle", ".pcap", ".pcapng", ".tlog", ".ulg",
    ".pem", ".key", ".pfx", ".p12", ".exe", ".dll", ".o", ".obj",
    ".a", ".lib", ".so", ".dylib", ".pyc", ".zip", ".gz", ".bin", ".px4",
}
PRIVATE_PARTS = {".git", "__pycache__", "private", "captures", "recordings", "logs"}
FIRMWARE_DIRS = {("src", "px4"), ("support", "px4")}
ZIP_DATE = (1980, 1, 1, 0, 0, 0)


def git(repo: Path, *args: str) -> bytes:
    return subprocess.check_output(["git", "-C", str(repo), *args], stderr=subprocess.PIPE)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def version_from(repo: Path) -> str:
    match = re.search(r"project\(mavlink_telemetry_data_router\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)",
                      (repo / "CMakeLists.txt").read_text(encoding="utf-8"))
    if not match:
        raise ValueError("Project version is missing from CMakeLists.txt")
    return match.group(1)


def text_bytes(path: Path) -> bytes:
    return path.read_text(encoding="utf-8-sig").replace("\r\n", "\n").replace("\r", "\n").encode("utf-8")


def source_allowed(name: str) -> bool:
    path = PurePosixPath(name)
    if path.is_absolute() or ".." in path.parts or "\\" in name or ":" in name:
        return False
    if tuple(part.lower() for part in path.parts[:2]) in FIRMWARE_DIRS:
        return False
    if any(part.lower() in PRIVATE_PARTS for part in path.parts):
        return False
    if path.name.lower().startswith(".env") or path.suffix.lower() in PRIVATE_SUFFIXES:
        return False
    if ".ini.tmp." in path.name.lower():
        return False
    return (name in SOURCE_ROOTS or name == "release/README.md" or
            len(path.parts) > 1 and path.parts[0] in SOURCE_DIRS)


def source_entries(repo: Path) -> list[tuple[str, bytes, int]]:
    names = git(repo, "ls-files", "-z", "--cached", "--others", "--exclude-standard").split(b"\0")
    entries = []
    for name in sorted({name.decode("utf-8") for name in names if name}):
        if not source_allowed(name):
            continue
        path = repo / name
        if not path.exists():  # A tracked deletion is part of this working tree.
            continue
        if path.is_symlink() or not path.is_file() or not path.resolve().is_relative_to(repo):
            raise ValueError("Unsafe source entry: " + name)
        data = text_bytes(path)  # Public source payload is UTF-8 text only.
        if b"\0" in data:
            raise ValueError("Binary source payload is not allowed: " + name)
        entries.append((name, data, 0o755 if path.suffix == ".sh" else 0o644))
    if not entries:
        raise ValueError("Source snapshot is empty")
    return entries


def zip_entries(path: Path, entries: list[tuple[str, bytes, int]]) -> None:
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name, data, mode in entries:
            info = zipfile.ZipInfo(name, ZIP_DATE)
            info.create_system = 3
            info.external_attr = (0o100000 | mode) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, data)
    with zipfile.ZipFile(path) as archive:
        if archive.testzip() is not None:
            raise ValueError("ZIP integrity check failed")
        if archive.namelist() != [name for name, _, _ in entries]:
            raise ValueError("ZIP manifest mismatch")
        for name in archive.namelist():
            safe_archive_name(name)


def safe_archive_name(name: str) -> None:
    path = PurePosixPath(name)
    if path.is_absolute() or ".." in path.parts or "\\" in name or ":" in name:
        raise ValueError("Unsafe archive member")
    if path.suffix.lower() in {".ini", ".env"} or path.name.startswith(".env"):
        raise ValueError("Operational configuration must not be archived")


def tar_entries(path: Path, entries: list[tuple[str, bytes, int]]) -> None:
    with path.open("wb") as raw, gzip.GzipFile(filename="", fileobj=raw, mode="wb", mtime=0) as compressed:
        with tarfile.open(fileobj=compressed, mode="w", format=tarfile.PAX_FORMAT) as archive:
            for name, data, mode in entries:
                info = tarfile.TarInfo(name)
                info.size, info.mode, info.mtime = len(data), mode, 0
                archive.addfile(info, io.BytesIO(data))
    with tarfile.open(path, "r:gz") as archive:
        if archive.getnames() != [name for name, _, _ in entries]:
            raise ValueError("Tar manifest mismatch")
        for member in archive:
            safe_archive_name(member.name)
            if not member.isfile():
                raise ValueError("Only regular archive members are allowed")
            handle = archive.extractfile(member)
            if handle is None or len(handle.read()) != member.size:
                raise ValueError("Tar integrity check failed")


def verify_binary(path: Path, platform: str, version: str) -> dict:
    data = path.read_bytes()
    if platform == "windows-x64":
        if len(data) < 64 or data[:2] != b"MZ":
            raise ValueError("Expected a Windows PE binary: " + path.name)
        offset = struct.unpack_from("<I", data, 0x3c)[0]
        if offset + 26 > len(data) or data[offset:offset + 4] != b"PE\0\0":
            raise ValueError("Invalid PE header: " + path.name)
        if struct.unpack_from("<H", data, offset + 4)[0] != 0x8664 or struct.unpack_from("<H", data, offset + 24)[0] != 0x20b:
            raise ValueError("Expected PE AMD64/PE32+: " + path.name)
    elif len(data) < 20 or data[:6] != b"\x7fELF\x02\x01" or struct.unpack_from("<H", data, 18)[0] != 62:
        raise ValueError("Expected little-endian ELF x86-64: " + path.name)
    native = (platform.startswith("windows") and sys.platform == "win32" or
              platform.startswith("linux") and sys.platform.startswith("linux"))
    if native:
        result = subprocess.run([str(path), "--version"], capture_output=True, timeout=10, check=False)
        expected = re.compile(rb"(?<![0-9.])" + re.escape(version.encode("ascii")) + rb"(?![0-9.])")
        if result.returncode or not expected.search(result.stdout):
            raise ValueError("Executable --version does not match release " + version + ": " + path.name)
    return {"sha256": sha256(data), "bytes": len(data), "architecture": "x64", "native_version_checked": native}


def validate_example(path: Path, default: bool = False) -> None:
    values = {}
    for line in path.read_text(encoding="utf-8-sig").splitlines():
        if line.strip() and not line.lstrip().startswith("#"):
            key, sep, value = line.partition("=")
            if sep:
                values[key.strip()] = value.strip()
    if values.get("token") != "" or values.get("serial_port") != "auto":
        raise ValueError("Shared examples must use blank token and automatic serial discovery")
    if default and values.get("url") != "-":
        raise ValueError("Default release configuration must not select an operational server")


def launchers(platform: str) -> dict[str, tuple[bytes, int]]:
    if platform.startswith("windows"):
        router = '''@echo off
setlocal
if not exist "%~dp0telemetry.ini" (
  copy /b "%~dp0config\\telemetry.ini.example" "%~dp0telemetry.ini" >nul
  if errorlevel 1 exit /b 1
)
"%~dp0bin\\mavlink_telemetry_data_router_interactive.exe" --config "%~dp0telemetry.ini" %*
set "router_exit=%errorlevel%"
if not "%router_exit%"=="0" pause
exit /b %router_exit%
'''
        server = '''@echo off
setlocal
"%~dp0tools\\mavlink_telemetry_data_router_test_server.exe" --bind 127.0.0.1 --port 21000 --udp-port 21001 %*
set "server_exit=%errorlevel%"
if not "%server_exit%"=="0" pause
exit /b %server_exit%
'''
        return {"Start-Router.cmd": (router.replace("\n", "\r\n").encode("ascii"), 0o644),
                "Start-Test-Server.cmd": (server.replace("\n", "\r\n").encode("ascii"), 0o644)}
    router = '''#!/bin/sh
set -eu
package_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
config_dir="${XDG_CONFIG_HOME:-$HOME/.config}/mavlink_telemetry_data_router"
umask 077
mkdir -p -- "$config_dir"
config_file="$config_dir/telemetry.ini"
if [ ! -f "$config_file" ]; then
    cp -- "$package_dir/config/telemetry.ini.example" "$config_file"
fi
chmod 600 -- "$config_file"
exec "$package_dir/bin/mavlink_telemetry_data_router_interactive" --config "$config_file" "$@"
'''
    server = '''#!/bin/sh
set -eu
package_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec "$package_dir/tools/mavlink_telemetry_data_router_test_server" --bind 127.0.0.1 --port 21000 --udp-port 21001 "$@"
'''
    return {"start-router.sh": (router.encode("utf-8"), 0o755),
            "start-test-server.sh": (server.encode("utf-8"), 0o755)}


def runtime_entries(repo: Path, build: Path, platform: str, version: str) -> tuple[list, dict]:
    entries, binaries = [], {}
    suffix = ".exe" if platform.startswith("windows") else ""
    for role, folder in (("headless", "bin"), ("interactive", "bin"), ("test_server", "tools")):
        filename = PROJECT + "_" + role + suffix
        source = build / ("tools" if role == "test_server" else "") / filename
        binaries[role] = verify_binary(source, platform, version)
        entries.append((folder + "/" + filename, source.read_bytes(), 0o755))
    documents = {
        "docs/QUICKSTART.md": repo / "support/release/QUICKSTART.md",
        "docs/SERVER_API.md": repo / "docs/SERVER_API.md",
        "docs/UDP_BACKUP.md": repo / "docs/UDP_BACKUP.md",
        "docs/REFERENCE.md": repo / "docs/REFERENCE.md",
        "docs/PX4_MAVLINK_CONTRACT.md": repo / "docs/PX4_MAVLINK_CONTRACT.md",
        "docs/REVIEW.md": repo / "docs/REVIEW.md",
        "licenses/MAVLINK_COPYING.txt": repo / "src/vendor/MAVLINK_COPYING.txt",
        "licenses/MAVLINK_UPSTREAM.md": repo / "src/vendor/mavlink/UPSTREAM.md",
        "docs/releases/v" + version + ".md": repo / "docs/releases" / ("v" + version + ".md"),
    }
    for name in ("telemetry.ini.example", "shtc3-physical.ini.example"):
        source = repo / "config" / name
        validate_example(source, default=name == "telemetry.ini.example")
        documents["config/" + name] = source
    for source in sorted((repo / "docs/api").rglob("*")):
        if source.is_file() and "__pycache__" not in source.parts and source.suffix != ".pyc":
            documents["docs/api/" + source.relative_to(repo / "docs/api").as_posix()] = source
    for name, source in documents.items():
        safe_archive_name(name)
        data = text_bytes(source)
        if name.startswith("docs/releases/"):
            data = data.replace(b"../../release/README.md", b"../QUICKSTART.md")
        entries.append((name, data, 0o644))
    entries.extend([
        ("README.md", ("# MAVLink Router " + version + "\n\n"
                       "[실행 안내](docs/QUICKSTART.md) · [서버 API](docs/SERVER_API.md) · "
                       "[변경점](docs/releases/v" + version + ".md)\n").encode("utf-8"), 0o644),
        ("RELEASE_NOTES.md", ("# 변경점\n\n[" + version + " 릴리스 노트](docs/releases/v" + version + ".md)\n").encode("utf-8"), 0o644),
        ("docs/COLLABORATION.md", ("# 개발자용 소스\n\n"
             "실행 파일과 같은 버전의 `" + PROJECT + "-" + version + "-source.zip`을 사용하세요. "
             "소스 ZIP의 `docs/COLLABORATION.md`에 Git 공유 절차가 있습니다. "
             "이 배포물은 원격 저장소에 게시되었다는 뜻이 아닙니다.\n\n"
             "[실행 안내](QUICKSTART.md) · [서버 API](SERVER_API.md)\n").encode("utf-8"), 0o644),
    ])
    entries.append(("licenses/PROJECT_LICENSE_STATUS.txt",
                    b"No project-wide software license has been declared.\n"
                    b"Third-party components retain their individual notices.\n"
                    b"MAVLink generated code: see MAVLINK_COPYING.txt and MAVLINK_UPSTREAM.md.\n", 0o644))
    for name, (data, mode) in launchers(platform).items():
        entries.append((name, data, mode))
    entries.sort()
    checksum = "".join(sha256(data) + "  " + name + "\n" for name, data, _ in entries)
    entries.append(("SHA256SUMS.txt", checksum.encode("ascii"), 0o644))
    entries.sort()
    return entries, binaries


def replace_file(source: Path, destination: Path) -> None:
    """Replace only an explicitly generated file; preserve other output contents."""
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.is_symlink() or destination.exists() and not destination.is_file():
        raise ValueError("Generated output destination is not a regular file")
    temporary = destination.with_name(destination.name + ".packaging.tmp")
    if temporary.exists():
        raise ValueError("A packaging temporary file already exists")
    shutil.copyfile(source, temporary)
    temporary.replace(destination)


def package(repo: Path, build: Path, platform: str, output: Path) -> Path:
    repo, build, output = repo.resolve(), build.resolve(), output.resolve()
    version = version_from(repo)
    destination = output / ("v" + version)
    assets = destination / "assets"
    if destination.is_symlink() or assets.is_symlink():
        raise ValueError("Output directories must not be symlinks")
    sources = source_entries(repo)
    runtime, binaries = runtime_entries(repo, build, platform, version)
    base_commit = git(repo, "rev-parse", "HEAD").decode("ascii").strip()
    dirty = bool(git(repo, "status", "--porcelain").strip())
    tree_digest = hashlib.sha256()
    for name, data, mode in sources:
        tree_digest.update(name.encode("utf-8") + b"\0" + str(mode).encode("ascii") + b"\0" + sha256(data).encode("ascii") + b"\n")
    source_info = {"base_commit": base_commit, "working_tree_dirty": dirty,
                   "file_count": len(sources), "content_sha256": tree_digest.hexdigest(),
                   "snapshot": "current working tree; UTF-8 LF, deterministic archive metadata"}
    stage_parent = repo / "build/dev-tools/release-staging"
    stage_parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="package-", dir=stage_parent) as temporary:
        stage = Path(temporary)
        source_name = f"{PROJECT}-{version}-source.zip"
        source_zip = stage / source_name
        zip_entries(source_zip, sources)
        prefix = f"{PROJECT}-{version}-{platform}"
        archive_name = prefix + (".zip" if platform.startswith("windows") else ".tar.gz")
        archive = stage / archive_name
        payload = [(prefix + "/" + name, data, mode) for name, data, mode in runtime]
        (zip_entries if platform.startswith("windows") else tar_entries)(archive, payload)
        for name, data, mode in runtime:
            staged = stage / "runtime" / name
            staged.parent.mkdir(parents=True, exist_ok=True)
            staged.write_bytes(data)
            final = destination / platform / name
            if not final.resolve().is_relative_to(destination.resolve()):
                raise ValueError("Runtime output escapes release directory")
            replace_file(staged, final)
            if os.name != "nt":
                final.chmod(mode)
        assets.mkdir(parents=True, exist_ok=True)
        replace_file(archive, assets / archive_name)
        replace_file(source_zip, assets / source_name)
        manifest_file = assets / "manifest.json"
        manifest = json.loads(manifest_file.read_text(encoding="utf-8")) if manifest_file.exists() else {
            "format": "mavlink-router-release/v1", "version": version, "platforms": {}}
        if manifest.get("version") != version:
            raise ValueError("Existing manifest version differs")
        manifest["created_at_utc"] = datetime.now(timezone.utc).isoformat()
        manifest["source"] = source_info
        manifest["platforms"][platform] = {"archive": archive_name, "binaries": binaries,
                                            "source_content_sha256": source_info["content_sha256"]}
        manifest["published"] = False
        manifest["assets"] = {path.name: {"sha256": sha256(path.read_bytes()), "bytes": path.stat().st_size}
                              for path in sorted(assets.iterdir())
                              if path.is_file() and path.name.startswith(PROJECT + "-" + version + "-")
                              and (path.name.endswith(".zip") or path.name.endswith(".tar.gz"))}
        manifest_stage = stage / "manifest.json"
        manifest_stage.write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8", newline="\n")
        replace_file(manifest_stage, manifest_file)
        checksums = "".join(item["sha256"] + "  " + name + "\n" for name, item in sorted(manifest["assets"].items()))
        checksums += sha256(manifest_file.read_bytes()) + "  manifest.json\n"
        checksum_stage = stage / "SHA256SUMS.txt"
        checksum_stage.write_text(checksums, encoding="ascii", newline="\n")
        replace_file(checksum_stage, assets / "SHA256SUMS.txt")
    return destination


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--platform", choices=PLATFORMS, required=True)
    parser.add_argument("--output", type=Path, default=Path("release"))
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]
    try:
        result = package(repo, args.build_dir, args.platform, args.output)
    except (OSError, ValueError, subprocess.SubprocessError, zipfile.BadZipFile, tarfile.TarError) as error:
        parser.exit(1, "Release packaging stopped: " + str(error) + "\n")
    print("Prepared " + str(result))


if __name__ == "__main__":
    main()
