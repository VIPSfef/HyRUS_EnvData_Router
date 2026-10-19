"""Prepare an extracted Ubuntu 22.04 x64 link sysroot; never install packages.

Python 3.9+ and zstandard (pip install zstandard). The resulting dynamic binaries require a
compatible Linux installation with libcurl.so.4. This sysroot is for linking;
it is not shipped as runtime libraries. Package files are SHA256 checked
against Ubuntu's HTTPS package index. No package maintainer scripts run.
"""
from __future__ import annotations
import argparse, concurrent.futures, gzip, hashlib, io, json, pathlib, re, tarfile, urllib.request
import zstandard

ARCHIVE = 'https://archive.ubuntu.com/ubuntu/'
INDEX = 'dists/jammy/main/binary-amd64/Packages.gz'
SEEDS = {'libc6', 'libc6-dev', 'linux-libc-dev', 'libgcc-11-dev', 'libgcc-s1', 'libcurl4-openssl-dev', 'libcurl4'}

def fetch(url, path):
    if not path.exists():
        with urllib.request.urlopen(url, timeout=90) as response:
            data = response.read()
        path.write_bytes(data)
    return path.read_bytes()

def index_packages(blob):
    result = {}
    for paragraph in gzip.decompress(blob).decode('utf-8').split('\n\n'):
        d = {}
        for line in paragraph.splitlines():
            if line and not line[0].isspace() and ': ' in line:
                key, value = line.split(': ', 1)
                d[key] = value
        if 'Package' in d:
            result[d['Package']] = d
    return result

def dep_names(value):
    for item in value.split(','):
        yield [re.split(r'\s|:', name.strip(), maxsplit=1)[0] for name in item.split('|')]

def data_tar(deb):
    if not deb.startswith(b'!<arch>\n'):
        raise ValueError('Invalid Debian archive')
    pos = 8
    while pos + 60 <= len(deb):
        hdr = deb[pos:pos+60]
        size = int(hdr[48:58])
        name = hdr[:16].decode('ascii').strip().rstrip('/')
        payload = deb[pos+60:pos+60+size]
        if name.startswith('data.tar'):
            if name.endswith('.zst'):
                return zstandard.ZstdDecompressor().stream_reader(io.BytesIO(payload)).read()
            return payload
        pos += 60 + size + (size & 1)
    raise ValueError('Debian archive has no data archive')

def safe_member(root, name):
    rel = pathlib.PurePosixPath(name)
    if rel.is_absolute() or '..' in rel.parts:
        raise ValueError(f'Unsafe archive member: {name}')
    return root.joinpath(*rel.parts)

def extract(deb, root, links):
    with tarfile.open(fileobj=io.BytesIO(data_tar(deb)), mode='r:*') as archive:
        for member in archive:
            path = safe_member(root, member.name)
            if member.isdir():
                path.mkdir(parents=True, exist_ok=True)
            elif member.isfile():
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(archive.extractfile(member).read())
            elif member.issym() or member.islnk():
                if member.issym():
                    target = pathlib.PurePosixPath(member.linkname)
                    base = root if target.is_absolute() else path.parent
                    parts = target.parts[1:] if target.is_absolute() else target.parts
                    source = base.joinpath(*parts).resolve()
                else:
                    source = safe_member(root, member.linkname).resolve()
                if not source.is_relative_to(root.resolve()):
                    raise ValueError(f'Link leaves sysroot: {member.name}')
                links.append((path, source))

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--destination', type=pathlib.Path, default=pathlib.Path(__file__).resolve().parents[2] / 'build' / 'toolchains' / 'ubuntu-jammy')
    args = parser.parse_args()
    base = args.destination.resolve()
    cache, root = base / 'packages', base / 'sysroot'
    cache.mkdir(parents=True, exist_ok=True)
    root.mkdir(parents=True, exist_ok=True)
    blob = fetch(ARCHIVE + INDEX, base / 'Packages.gz')
    packages = index_packages(blob)
    wanted = set(SEEDS)
    # Follow curl's runtime closure. Other GCC sanitizer runtimes are not needed
    # for ordinary C linking and deliberately are not downloaded.
    pending = ['libcurl4', 'libc6', 'libgcc-s1']
    seen = set()
    while pending:
        name = pending.pop()
        if name in seen:
            continue
        seen.add(name)
        for alternatives in dep_names(packages[name].get('Depends', '')):
            selected = next((dep for dep in alternatives if dep in packages), None)
            if selected:
                wanted.add(selected)
                pending.append(selected)
    def download(name):
        info = packages[name]
        path = cache / pathlib.PurePosixPath(info['Filename']).name
        data = fetch(ARCHIVE + info['Filename'], path)
        actual = hashlib.sha256(data).hexdigest()
        if actual != info['SHA256']:
            raise ValueError(f'SHA256 mismatch: {name}; remove {path} and retry')
        return name, path, info
    links, manifest = [], []
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        downloaded = list(pool.map(download, sorted(wanted)))
    for name, path, info in downloaded:
        extract(path.read_bytes(), root, links)
        manifest.append({key: info[key] for key in ('Package', 'Version', 'Filename', 'SHA256')})
    # Materialize file symlinks as regular files on Windows without requiring
    # administrator or Developer Mode privileges. Never copy directory trees.
    for _ in range(len(links) + 1):
        remaining = []
        for path, source in links:
            if source.is_file():
                path.parent.mkdir(parents=True, exist_ok=True)
                if path != source:
                    path.write_bytes(source.read_bytes())
            elif not source.is_dir():
                remaining.append((path, source))
        if len(remaining) == len(links):
            break
        links = remaining
    (base / 'manifest.json').write_text(json.dumps({'index_sha256': hashlib.sha256(blob).hexdigest(), 'packages': manifest}, indent=2) + '\n', encoding='utf-8')
    print(f'Sysroot ready: {root} ({len(manifest)} verified packages)')
    for path, source in links:
        # Some package links point to data only provided by optional tools.
        print(f'Unresolved optional link: {path.relative_to(root)} -> {source}')

if __name__ == '__main__':
    main()