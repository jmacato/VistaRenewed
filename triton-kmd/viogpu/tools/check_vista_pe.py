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

DEFAULT_IMPORTS["umd10"] = DEFAULT_IMPORTS["umd"].copy()

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


    def fixed_versions(self) -> list[tuple[tuple[int, ...], tuple[int, ...]]]:
        """Read actual RT_VERSION fixed file/product versions, not string matches."""
        base, directory_size = self.directories[2]
        if not base or not directory_size:
            return []

        def entries(relative: int):
            if relative < 0 or relative + 16 > directory_size:
                raise PeError("invalid resource directory")
            offset = self.rva_to_offset(base + relative)
            named, numbered = unpack_from("<HH", self.data, offset + 12)
            count = named + numbered
            if count > 4096 or relative + 16 + count * 8 > directory_size:
                raise PeError("invalid resource entry count")
            return [unpack_from("<II", self.data, offset + 16 + i * 8) for i in range(count)]

        blobs = []
        def descend(reference: int, depth: int):
            if depth > 4:
                raise PeError("resource directory recursion")
            relative = reference & 0x7fffffff
            if reference & 0x80000000:
                for _, child in entries(relative):
                    descend(child, depth + 1)
            else:
                if relative + 16 > directory_size:
                    raise PeError("invalid resource data entry")
                offset = self.rva_to_offset(base + relative)
                rva, size = unpack_from("<II", self.data, offset)
                start = self.rva_to_offset(rva)
                if start + size > len(self.data):
                    raise PeError("truncated version resource")
                blobs.append(self.data[start:start + size])

        for identifier, reference in entries(0):
            if identifier == 16:  # RT_VERSION
                descend(reference, 0)
        result = []
        for blob in blobs:
            length, value_length, value_type = unpack_from("<HHH", blob, 0)
            key = "VS_VERSION_INFO\0".encode("utf-16le")
            if length > len(blob) or blob[6:6 + len(key)] != key or value_type != 0:
                raise PeError("invalid VS_VERSION_INFO")
            offset = (6 + len(key) + 3) & ~3
            if value_length != 52 or offset + value_length > length:
                raise PeError("invalid VS_FIXEDFILEINFO size")
            signature, structure, file_ms, file_ls, product_ms, product_ls = unpack_from("<6I", blob, offset)
            if signature != 0xfeef04bd or structure != 0x10000:
                raise PeError("invalid VS_FIXEDFILEINFO signature")
            def version(ms, ls):
                return (ms >> 16, ms & 0xffff, ls >> 16, ls & 0xffff)
            result.append((version(file_ms, file_ls), version(product_ms, product_ls)))
        return result


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
        try:
            versions = image.fixed_versions()
            expected_version = (7, 15, 1, 0)
            if not versions or any(file != expected_version or product != expected_version
                                   for file, product in versions):
                errors.append(f"Vista KMD file/product version is {versions!r}, expected 7.15.1.0")
        except PeError as exc:
            errors.append(f"invalid Vista KMD version resource: {exc}")
        unsupported_hardening = image.dll_characteristics & 0x4000
        if unsupported_hardening:
            errors.append(
                "uses post-Vista DLL characteristics: "
                f"0x{unsupported_hardening:04x}"
            )
    if kind in {"umd", "umd10", "exe"} and image.subsystem not in {2, 3}:
        errors.append(f"subsystem is {image.subsystem}, expected WINDOWS_GUI (2) or WINDOWS_CUI (3) for user mode")

    imports = image.imports()
    allowed_imports = DEFAULT_IMPORTS[kind] | {name.lower() for name in extra_imports}
    unknown_imports = imports - allowed_imports
    if unknown_imports:
        errors.append("imports outside the Vista allow-list: " + ", ".join(sorted(unknown_imports)))
    if kind in {"umd", "umd10", "exe"}:
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
    if kind == "umd10":
        required_exports.add("OpenAdapter10")
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
