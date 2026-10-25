#!/usr/bin/env python3
"""Build the committed source, Git bundle and standalone Windows handoff ZIP."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
from datetime import datetime, timezone
import zipfile


def git(repo, *args):
    result = subprocess.run(["git", "-C", str(repo), *map(str, args)],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    if result.returncode:
        message = result.stderr.decode("utf-8", errors="replace").strip()
        raise RuntimeError(f"Git command failed ({args[0]}): {message}")
    return result.stdout


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def write_json(path, data):
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def verify_source(repo, commit, archive):
    entries = []
    for row in git(repo, "ls-tree", "-rz", "--full-tree", commit).split(b"\0"):
        if not row:
            continue
        meta, name = row.split(b"\t", 1)
        _mode, kind, oid = meta.decode().split()
        if kind != "blob":
            raise RuntimeError("Source snapshot contains a non-blob entry")
        entries.append((name.decode("utf-8"), oid))
    result = subprocess.run(["git", "-C", str(repo), "cat-file", "--batch"],
                            input="".join(oid + "\n" for _, oid in entries).encode("ascii"),
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
    offset = 0
    with zipfile.ZipFile(archive) as source:
        if source.testzip() is not None:
            raise RuntimeError("Source ZIP CRC check failed")
        if {n for n in source.namelist() if not n.endswith("/")} != {n for n, _ in entries}:
            raise RuntimeError("Source ZIP does not match the committed paths")
        for name, oid in entries:
            end = result.stdout.index(b"\n", offset)
            header = result.stdout[offset:end].decode().split()
            size = int(header[2])
            start = end + 1
            if header[0] != oid or source.read(name) != result.stdout[start:start + size]:
                raise RuntimeError("Source ZIP differs from Git: " + name)
            offset = start + size + 1
    return entries


def build(repo, output):
    repo = repo.resolve()
    output = output.resolve()
    if git(repo, "status", "--porcelain").strip():
        raise RuntimeError("Commit the intended changes before publishing a handoff")
    if git(repo, "symbolic-ref", "--short", "HEAD").decode().strip() != "main":
        raise RuntimeError("Publish from the main branch")
    commit = git(repo, "rev-parse", "HEAD").decode().strip()
    short = commit[:12]
    names = {
        "bundle": f"mavlink-router-{short}.bundle",
        "source": f"mavlink-router-source-{short}.zip",
        "handoff": f"mavlink-router-handoff-{short}.zip",
        "manifest": f"manifest-{short}.json",
        "checksums": f"SHA256SUMS-{short}.txt",
    }
    output.mkdir(parents=True, exist_ok=True)
    for name in names.values():
        if (output / name).exists():
            raise RuntimeError("A release for this commit already exists: " + str(output / name))
    stage_root = repo / "build/dev-tools/handoff-staging"
    stage_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="release-", dir=stage_root) as temporary:
        stage = Path(temporary).resolve()
        if stage.parent != stage_root.resolve():
            raise RuntimeError("Unexpected temporary directory")
        bundle, source_zip = stage / names["bundle"], stage / names["source"]
        git(repo, "bundle", "create", bundle, "main")
        git(repo, "bundle", "verify", bundle)
        head = git(repo, "bundle", "list-heads", bundle, "refs/heads/main").decode().split()
        if not head or head[0] != commit:
            raise RuntimeError("main changed while creating the bundle")
        git(repo, "-c", "core.autocrlf=false", "-c", "core.eol=lf", "archive",
            "--format=zip", f"--output={source_zip}", commit)
        entries = verify_source(repo, commit, source_zip)
        cmake = git(repo, "show", commit + ":CMakeLists.txt").decode("utf-8")
        match = re.search(r"project\(mavlink_telemetry_data_router VERSION ([0-9.]+)", cmake)
        manifest = {
            "format": "mavlink-router-handoff/v1", "application_version": match[1] if match else "unknown",
            "source_commit": commit, "source_branch": "main", "short_commit": short,
            "created_at": datetime.now(timezone.utc).isoformat(),
            "bundle": {"file": names["bundle"], "sha256": digest(bundle), "bytes": bundle.stat().st_size},
            "source_directory": "source",
        }
        readme = f"""다른 PC에서 소스와 문서를 가져오는 방법

이 압축 파일은 Git 커밋 {commit}을 기준으로 만들었습니다.
Git for Windows가 설치된 Windows PC에서 사용하세요. Git 자체는 포함하지 않았습니다.
Python이나 C 빌드 도구는 이 가져오기 작업에 필요하지 않습니다.

1. ZIP 전체를 새 전달 폴더에 풀어 주세요. ZIP 안에서 바로 실행하지 않습니다.
2. Start-Sync.cmd를 두 번 클릭합니다.
3. 처음에는 사용자 홈의 dev\\mavlink_telemetry_data_router에 복제하고,
   다음 실행부터는 같은 폴더를 검사한 뒤 새 버전으로 업데이트합니다.
   수정 중인 파일이 있거나 main 이력이 갈라져 있으면 작업을 보존하고 멈춥니다.

작업 경로를 직접 지정하려면 압축을 푼 폴더의 PowerShell에서 실행하세요.
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\\Sync-MavlinkRouter.ps1 -Destination "C:\\dev\\mavlink_telemetry_data_router"
이때 지정한 경로는 다음 업데이트에서도 같게 지정해야 합니다.
Bypass 옵션은 이 실행에만 적용되며 PC 전체의 실행 정책을 바꾸지 않습니다.

새 버전은 새 ZIP을 다른 전달 폴더에 풀고 같은 방법으로 실행하면 됩니다.
이 ZIP만 다시 실행해도 나중에 만들어진 버전을 자동으로 내려받지는 않습니다.
OneDrive의 갱신되는 공유 폴더를 직접 쓰려면 -SharedDirectory로 지정하세요.

읽을 문서는 source\\README.md, source\\docs\\COLLABORATION.md에 있습니다.
서버 개발자는 source\\docs\\SERVER_API.md와 source\\docs\\api\\README.md를 읽어 주세요.
source 폴더는 이 배포본의 읽기용 소스 사본입니다. 실제 수정은 Git으로 복제한 작업 폴더에서 하세요.
파일 목록과 SHA256은 SHA256SUMS.txt에 있습니다.

이 배포물에는 운용 설정, 인증 토큰, 관측 기록이나 실행 중인 장치의 로그를 넣지 않았습니다.
Git 원격 서버 게시나 OneDrive 클라우드 동기화 완료를 뜻하지는 않습니다.
"""
        package = stage / "payload"
        package.mkdir()
        with zipfile.ZipFile(source_zip) as source:
            for name, _ in entries:
                target = package / "source" / name
                if not target.resolve().is_relative_to((package / "source").resolve()):
                    raise RuntimeError("Unsafe committed source path")
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(source.read(name))
            for name in ("Sync-MavlinkRouter.ps1", "Start-Sync.cmd"):
                (package / name).write_bytes(source.read("support/sharing/" + name))
        shutil.copyfile(bundle, package / names["bundle"])
        write_json(package / "LATEST.json", manifest)
        (package / "README-FIRST.txt").write_text(readme, encoding="utf-8")
        contents = sorted(p for p in package.rglob("*") if p.is_file())
        checksums = "".join(digest(p) + "  " + p.relative_to(package).as_posix() + "\n" for p in contents)
        (package / "SHA256SUMS.txt").write_text(checksums, encoding="utf-8")
        handoff = stage / names["handoff"]
        with zipfile.ZipFile(handoff, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
            for path in sorted(p for p in package.rglob("*") if p.is_file()):
                archive.write(path, path.relative_to(package).as_posix())
        with zipfile.ZipFile(handoff) as archive:
            if archive.testzip() is not None:
                raise RuntimeError("Handoff ZIP CRC check failed")
        manifest.pop("source_directory")
        manifest["source_zip"] = {"file": names["source"], "sha256": digest(source_zip), "bytes": source_zip.stat().st_size}
        manifest["handoff_zip"] = {"file": names["handoff"], "sha256": digest(handoff), "bytes": handoff.stat().st_size}
        manifest["verification"] = {"git_bundle_verified": True, "source_zip_matches_commit": True,
                                    "source_files": len(entries), "handoff_zip_crc": True,
                                    "onedrive_cloud_sync_verified": False, "remote_publication_performed": False}
        write_json(stage / names["manifest"], manifest)
        (stage / names["checksums"]).write_text(
            "".join(digest(stage / names[key]) + "  " + names[key] + "\n" for key in ("bundle", "source", "handoff")), encoding="utf-8")
        # Publish complete files first. LATEST is replaced only after all payloads exist.
        for key in ("bundle", "source", "handoff", "manifest", "checksums"):
            shutil.copyfile(stage / names[key], output / names[key])
        for name in ("Sync-MavlinkRouter.ps1", "Start-Sync.cmd"):
            pending = output / (name + ".tmp")
            shutil.copyfile(package / name, pending)
            pending.replace(output / name)
        (stage / "README-FIRST.txt").write_text(readme, encoding="utf-8")
        shutil.copyfile(stage / "README-FIRST.txt", output / "README-FIRST.txt")
        write_json(output / ("LATEST-" + short + ".tmp"), manifest)
        (output / ("LATEST-" + short + ".tmp")).replace(output / "LATEST.json")
    return output / names["handoff"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--output-directory", type=Path)
    args = parser.parse_args()
    try:
        output = build(args.repo, args.output_directory or args.repo / "build/sharing")
    except (OSError, RuntimeError, subprocess.SubprocessError, ValueError, KeyError, zipfile.BadZipFile) as error:
        parser.exit(1, "Handoff creation stopped: " + str(error) + "\n")
    print(output)


if __name__ == "__main__":
    main()
