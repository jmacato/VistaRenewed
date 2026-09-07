#!/usr/bin/env python3
"""Reject Vista package artifacts with the wrong PE ABI or imports.

Run this in the Windows packaging environment after building each artifact:

  check_vista_pe.py --kind kmd --arch x86 path\\to\\viogpu3d.sys
  check_vista_pe.py --kind kmd --arch x64 path\\to\\viogpu3d.sys
  check_vista_pe.py --kind umd --arch x86 path\\to\\neptune_d3d9.dll
  check_vista_pe.py --kind umd --arch x64 path\\to\\neptune_d3d9.dll
  check_vista_pe.py --kind exe --arch x64 path\\to\\triton-vista-deploy.exe

The UMD profile requires the undecorated OpenAdapter export.  All profiles
require PE subsystem version 6.0 and use a deliberately small import
allow-list.  Add an exception explicitly with --allow-import; do not weaken
the default profile for a one-off dependency.
"""

from __future__ import annotations

import argparse
import struct
import sys
from dataclasses import dataclass
from pathlib import Path


MACHINES = {"x86": 0x014C, "x64": 0x8664}

DEFAULT_IMPORTS = {
    "kmd": {"ntoskrnl.exe", "hal.dll", "displib.dll"},
    # msvcrt.dll is the Vista inbox CRT used by the standalone D3D9 UMD.
    "umd": {"kernel32.dll", "gdi32.dll", "user32.dll", "advapi32.dll", "ntdll.dll", "msvcrt.dll"},
    # SetupAPI is inbox on Vista. The deployment service resolves newdev.dll
    # dynamically so it can report a clean Win32 error if that API is absent.
    "exe": {"kernel32.dll", "user32.dll", "advapi32.dll", "setupapi.dll", "msvcrt.dll"},
}

FORBIDDEN_UMD_IMPORTS = {
    "d3d10.dll",
    "d3d10_1.dll",
    "d3d11.dll",
    "d3d12.dll",
    "dxgi.dll",
}


class PeError(RuntimeError):
    pass


def unpack_from(fmt: str, data: bytes, offset: int) -> tuple[int, ...]:
    size = struct.calcsize(fmt)
    if offset < 0 or offset + size > len(data):
        raise PeError("truncated PE data")
    return struct.unpack_from(fmt, data, offset)


@dataclass(frozen=True)
class Section:
    virtual_address: int
    virtual_size: int
    raw_offset: int
    raw_size: int


class PeImage:
    def __init__(self, path: Path):
        self.path = path
        self.data = path.read_bytes()
        if self.data[:2] != b"MZ":
            raise PeError("missing DOS signature")

        (pe_offset,) = unpack_from("<I", self.data, 0x3C)
        if self.data[pe_offset : pe_offset + 4] != b"PE\0\0":
            raise PeError("missing PE signature")

        coff = pe_offset + 4
        self.machine, section_count, _, _, _, optional_size, _ = unpack_from(
            "<HHIIIHH", self.data, coff
        )
        optional = coff + 20
        (magic,) = unpack_from("<H", self.data, optional)
        if magic == 0x10B:
            data_directory_offset = optional + 96
        elif magic == 0x20B:
            data_directory_offset = optional + 112
        else:
            raise PeError(f"unknown optional-header magic 0x{magic:04x}")

        self.subsystem_major, self.subsystem_minor = unpack_from("<HH", self.data, optional + 48)
        (self.subsystem,) = unpack_from("<H", self.data, optional + 68)
        (self.dll_characteristics,) = unpack_from("<H", self.data, optional + 70)

        self.directories = []
        for index in range(16):
            self.directories.append(unpack_from("<II", self.data, data_directory_offset + index * 8))

        section_offset = optional + optional_size
        self.sections = []
        for index in range(section_count):
            offset = section_offset + index * 40
            _, virtual_size, virtual_address, raw_size, raw_offset, _, _, _, _, _ = unpack_from(
                "<8sIIIIIIHHI", self.data, offset
            )
            self.sections.append(Section(virtual_address, virtual_size, raw_offset, raw_size))

    def rva_to_offset(self, rva: int) -> int:
        for section in self.sections:
            size = max(section.virtual_size, section.raw_size)
            if section.virtual_address <= rva < section.virtual_address + size:
                offset = section.raw_offset + rva - section.virtual_address
                if offset >= len(self.data):
                    break
                return offset
        if rva < len(self.data):
            return rva
        raise PeError(f"RVA 0x{rva:x} is not in an image section")

    def c_string(self, rva: int) -> str:
        offset = self.rva_to_offset(rva)
        end = self.data.find(b"\0", offset)
        if end < 0:
            raise PeError("unterminated PE string")
        try:
            return self.data[offset:end].decode("ascii")
        except UnicodeDecodeError as exc:
            raise PeError("non-ASCII PE string") from exc

    def imports(self) -> set[str]:
        rva, size = self.directories[1]
        if not rva or not size:
            return set()
        imports = set()
        offset = self.rva_to_offset(rva)
        limit = offset + size
        while offset + 20 <= limit:
            original_first_thunk, _, _, name_rva, first_thunk = unpack_from("<IIIII", self.data, offset)
            if not any((original_first_thunk, name_rva, first_thunk)):
                break
            imports.add(self.c_string(name_rva).lower())
            offset += 20
        return imports

    def exports(self) -> set[str]:
        rva, size = self.directories[0]
        if not rva or not size:
            return set()
        offset = self.rva_to_offset(rva)
        _, _, _, _, _, _, _, number_of_names, _, address_of_names, _ = unpack_from(
            "<IIHHIIIIIII", self.data, offset
        )
        exports = set()
        names_offset = self.rva_to_offset(address_of_names)
        for index in range(number_of_names):
            (name_rva,) = unpack_from("<I", self.data, names_offset + index * 4)
            exports.add(self.c_string(name_rva))
        return exports


def validate(image: PeImage, kind: str, arch: str, extra_imports: set[str], extra_exports: set[str]) -> list[str]:
    errors = []
    if image.machine != MACHINES[arch]:
        errors.append(
            f"machine is 0x{image.machine:04x}, expected 0x{MACHINES[arch]:04x} ({arch})"
        )
    if (image.subsystem_major, image.subsystem_minor) != (6, 0):
        errors.append(
            f"subsystem version is {image.subsystem_major}.{image.subsystem_minor}, expected 6.0"
        )
    if kind == "kmd" and image.subsystem != 1:
        errors.append(f"subsystem is {image.subsystem}, expected NATIVE (1) for a KMD")
    if kind == "kmd":
        unsupported_hardening = image.dll_characteristics & 0x4000
        if unsupported_hardening:
            errors.append(
                "uses post-Vista DLL characteristics: "
                f"0x{unsupported_hardening:04x}"
            )
    if kind in {"umd", "exe"} and image.subsystem not in {2, 3}:
        errors.append(f"subsystem is {image.subsystem}, expected WINDOWS_GUI (2) or WINDOWS_CUI (3) for user mode")

    imports = image.imports()
    allowed_imports = DEFAULT_IMPORTS[kind] | {name.lower() for name in extra_imports}
    unknown_imports = imports - allowed_imports
    if unknown_imports:
        errors.append("imports outside the Vista allow-list: " + ", ".join(sorted(unknown_imports)))
    if kind in {"umd", "exe"}:
        forbidden = {
            name
            for name in imports
            if name in FORBIDDEN_UMD_IMPORTS
            or name.startswith("api-ms-win-")
            or name.startswith("ext-ms-win-")
        }
        if forbidden:
            errors.append("forbidden user-mode imports: " + ", ".join(sorted(forbidden)))

    required_exports = set(extra_exports)
    if kind == "umd":
        required_exports.add("OpenAdapter")
    missing_exports = required_exports - image.exports()
    if missing_exports:
        errors.append("missing exports: " + ", ".join(sorted(missing_exports)))
    return errors


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kind", choices=sorted(DEFAULT_IMPORTS), required=True)
    parser.add_argument("--arch", choices=sorted(MACHINES), required=True)
    parser.add_argument(
        "--allow-import",
        action="append",
        default=[],
        metavar="DLL",
        help="add one explicitly reviewed import DLL to the allow-list",
    )
    parser.add_argument(
        "--require-export",
        action="append",
        default=[],
        metavar="NAME",
        help="require one additional undecorated export",
    )
    parser.add_argument("artifact", type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        image = PeImage(args.artifact)
        errors = validate(
            image,
            args.kind,
            args.arch,
            set(args.allow_import),
            set(args.require_export),
        )
    except (OSError, PeError) as exc:
        print(f"{args.artifact}: {exc}", file=sys.stderr)
        return 1

    if errors:
        for error in errors:
            print(f"{args.artifact}: {error}", file=sys.stderr)
        return 1

    print(f"{args.artifact}: Vista PE audit passed ({args.kind}, {args.arch})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
