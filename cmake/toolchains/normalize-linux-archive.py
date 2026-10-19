"""Restore POSIX modes in a Linux TGZ produced by Windows-hosted CPack."""
import os
from pathlib import Path
import sys
import tarfile


def normalize(path):
    path = Path(path)
    temporary = path.with_name(path.name + '.normalized')
    executables = {'mavlink_telemetry_data_router_headless', 'mavlink_telemetry_data_router_interactive'}
    found = set()
    try:
        with tarfile.open(path, 'r:gz') as source, tarfile.open(temporary, 'w:gz', format=tarfile.PAX_FORMAT) as target:
            for member in source:
                parts = Path(member.name).parts
                is_program = len(parts) >= 2 and parts[-2] == 'bin' and parts[-1] in executables
                member.uid = member.gid = 0
                member.uname = member.gname = 'root'
                member.mode = 0o755 if member.isdir() or is_program else 0o644
                if is_program:
                    found.add(parts[-1])
                target.addfile(member, source.extractfile(member) if member.isfile() else None)
        if found != executables:
            raise ValueError(f'Linux package missing executables: {executables - found}')
        os.replace(temporary, path)
    finally:
        if temporary.exists():
            temporary.unlink()
    print(f'Normalized Linux package permissions: {path.name}')


if __name__ == '__main__':
    for argument in sys.argv[1:]:
        normalize(argument)