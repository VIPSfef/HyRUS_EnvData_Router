"""Check the target executable's PE/ELF architecture using only its file header.

Examples:
    python test_arch.py build/telemetry_send.exe --arch x86
    python test_arch.py build/telemetry_monitor --arch x64
    python test_arch.py build/telemetry_send --bits
No executable is run and the Python host's pointer width is never consulted.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import struct
import sys


def inspect_arch(path: Path) -> dict[str, object]:
    """Return format, arch, bits and machine; reject malformed/inconsistent headers."""
    with path.open("rb") as binary:
        binary.seek(0, 2)
        length = binary.tell()

        def read_at(offset: int, count: int) -> bytes:
            if offset < 0 or count < 0 or offset > length or count > length - offset:
                raise ValueError("truncated or out-of-range executable header")
            binary.seek(offset)
            result = binary.read(count)
            if len(result) != count:
                raise ValueError("could not read the complete executable header")
            return result

        magic = read_at(0, 4)
        if magic[:2] == b"MZ":
            dos = read_at(0, 64)
            pe_offset = struct.unpack_from("<I", dos, 0x3C)[0]
            if pe_offset < 64:
                raise ValueError("PE header overlaps the DOS header")
            coff = read_at(pe_offset, 24)
            if coff[:4] != b"PE\0\0":
                raise ValueError("DOS executable has no PE signature")
            machine = struct.unpack_from("<H", coff, 4)[0]
            optional_size = struct.unpack_from("<H", coff, 20)[0]
            if optional_size < 2:
                raise ValueError("PE optional header is absent")
            optional = read_at(pe_offset + 24, optional_size)
            kind = struct.unpack_from("<H", optional)[0]
            expected = {0x10B: (32, 0x014C, "x86", 96),
                        0x20B: (64, 0x8664, "x64", 112)}
            if kind not in expected:
                raise ValueError(f"unsupported PE optional-header magic 0x{kind:04x}")
            bits, required_machine, arch, minimum_size = expected[kind]
            if optional_size < minimum_size:
                raise ValueError("PE optional header is shorter than its declared class")
            if machine != required_machine:
                raise ValueError(
                    f"inconsistent/unsupported PE class {bits} and machine 0x{machine:04x}")
            return {"format": "PE32" if bits == 32 else "PE32+",
                    "arch": arch, "bits": bits, "machine": machine}

        if magic == b"\x7fELF":
            identity = read_at(0, 16)
            elf_class, encoding, version = identity[4:7]
            if elf_class not in (1, 2) or encoding not in (1, 2) or version != 1:
                raise ValueError("invalid ELF class, byte order or identification version")
            endian = "<" if encoding == 1 else ">"
            header = read_at(0, 52 if elf_class == 1 else 64)
            machine = struct.unpack_from(endian + "H", header, 18)[0]
            file_version = struct.unpack_from(endian + "I", header, 20)[0]
            header_size = struct.unpack_from(
                endian + "H", header, 40 if elf_class == 1 else 52)[0]
            bits, required_machine, arch = (32, 3, "x86") if elf_class == 1 else (64, 62, "x64")
            if header_size != len(header) or file_version != 1:
                raise ValueError("invalid ELF file-header size or version")
            if encoding != 1 or machine != required_machine:
                raise ValueError(
                    f"inconsistent/unsupported ELF class {bits}, encoding {encoding}, "
                    f"machine 0x{machine:04x}")
            return {"format": f"ELF{bits}", "arch": arch,
                    "bits": bits, "machine": machine}

        raise ValueError("file is neither an x86/x64 PE executable nor ELF binary")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", type=Path)
    parser.add_argument("--arch", choices=("x86", "x64", "amd64"),
                        help="fail on target mismatch; x64 is canonical and amd64 is an alias")
    parser.add_argument("--bits", action="store_true",
                        help="print only the target's 32/64 bit width after validation")
    args = parser.parse_args()
    try:
        result = inspect_arch(args.path)
        expected = "x64" if args.arch == "amd64" else args.arch
        if expected is not None and result["arch"] != expected:
            raise ValueError(
                f"expected {expected}, found {result['arch']} ({result['format']}, "
                f"machine 0x{result['machine']:04x})")
    except (OSError, ValueError, struct.error) as error:
        print(f"FAIL architecture check: {args.path}: {error}", file=sys.stderr)
        return 1
    if args.bits:
        print(result["bits"])
    else:
        print(f"PASS architecture: {result['format']} arch={result['arch']} "
              f"bits={result['bits']} machine=0x{result['machine']:04x} file={args.path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
