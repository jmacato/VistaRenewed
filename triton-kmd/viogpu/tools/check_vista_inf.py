#!/usr/bin/env python3
"""Check the Vista D3D9/D3D10 INF template or a completed driver package.

Run this after the Vista KMD build and after Inf2Cat:

  check_vista_inf.py --arch x86 viogpu3d.inf --package-dir package-x86
  check_vista_inf.py --arch x64 viogpu3d.inf --package-dir package-x64

The checker requires the ordered D3D9/D3D10 UMD registration that Vista uses.  It rejects
modern Triton D3D11 file names in the INF.  When --package-dir is present,
it also requires the exact files that the catalog must cover.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


EXPECTED = {
    "x86": {
        "catalog": "viogpu3d-vista-x86.cat",
        "manufacturer": "[rhel.ntx86]",
        "files": {"viogpu3d.sys", "neptune_d3d9.dll", "neptune_d3d10.dll"},
        "service_file": "triton-vista-deploy.exe",
        "umds": {"neptune_d3d9.dll", "neptune_d3d10.dll"},
        "copy_sections": {
            "[viogpu3d_files.usermode]": {"neptune_d3d9.dll", "neptune_d3d10.dll"},
        },
        "destinations": {
            "viogpu3d_files.usermode=11",
        },
        "registry": {
            "hkr,,usermodedrivername,%reg_multi_sz%,neptune_d3d9.dll,neptune_d3d10.dll",
            "hkr,,installeddisplaydrivers,%reg_multi_sz%,neptune_d3d9,neptune_d3d10",
            "hkr,,vgacompatible,%reg_dword%,0",
            "hkr,,capabilityoverride,%reg_dword%,0x8",
        },
    },
    "x64": {
        "catalog": "viogpu3d-vista-x64.cat",
        "manufacturer": "[rhel.ntamd64]",
        "files": {
            "viogpu3d.sys",
            "neptune_d3d9.dll",
            "neptune_d3d10.dll",
            "neptune_d3d9_wow.dll",
            "neptune_d3d10_wow.dll",
            "triton-vista-deploy.exe",
        },
        "umds": {"neptune_d3d9.dll", "neptune_d3d10.dll", "neptune_d3d9_wow.dll", "neptune_d3d10_wow.dll"},
        "copy_sections": {
            "[viogpu3d_files.usermodenative]": {"neptune_d3d9.dll", "neptune_d3d10.dll"},
            "[viogpu3d_files.usermodewow]": {"neptune_d3d9_wow.dll", "neptune_d3d10_wow.dll"},
        },
        "destinations": {
            "viogpu3d_files.usermodenative=11",
            "viogpu3d_files.usermodewow=10,syswow64",
            "tritonvistadeploy_files=11",
        },
        "registry": {
            "hkr,,usermodedrivername,%reg_multi_sz%,neptune_d3d9.dll,neptune_d3d10.dll",
            "hkr,,usermodedrivernamewow,%reg_multi_sz%,neptune_d3d9_wow.dll,neptune_d3d10_wow.dll",
            "hkr,,installeddisplaydrivers,%reg_multi_sz%,neptune_d3d9,neptune_d3d10",
            "hkr,,vgacompatible,%reg_dword%,0",
            "hkr,,capabilityoverride,%reg_dword%,0x8",
        },
    },
}

FORBIDDEN_TEXT = ("d3d11", "dxgi", "neptune_umd")


def normalize(line: str) -> str:
    return "".join(line.lower().split())


def section_lines(path: Path) -> dict[str, list[str]]:
    sections: dict[str, list[str]] = {}
    current: list[str] | None = None

    for raw in path.read_text(encoding="utf-8-sig").splitlines():
        line = raw.strip()
        if not line or line.startswith(";"):
            continue
        if line.startswith("[") and line.endswith("]"):
            current = sections.setdefault(line.lower(), [])
        elif current is not None:
            current.append(line)
    return sections


def assignment_value(lines: list[str], key: str) -> str | None:
    for line in lines:
        if "=" not in line:
            continue
        candidate, value = line.split("=", 1)
        if candidate.strip().lower() == key.lower():
            return value.strip()
    return None


def file_names(lines: list[str]) -> set[str]:
    names = set()
    for line in lines:
        if "=" in line:
            names.add(line.split("=", 1)[0].strip().lower())
        elif line:
            names.add(line.split(",", 1)[0].strip().lower())
    return names


def validate(path: Path, arch: str, package_dir: Path | None) -> list[str]:
    expected = EXPECTED[arch]
    errors = []
    sections = section_lines(path)
    text = "\n".join(line for lines in sections.values() for line in lines).lower()

    for forbidden in FORBIDDEN_TEXT:
        if forbidden in text:
            errors.append(f"INF contains forbidden modern UMD reference: {forbidden}")

    version = sections.get("[version]", [])
    catalog = assignment_value(version, "CatalogFile")
    if catalog is None or catalog.lower() != expected["catalog"]:
        errors.append(f"CatalogFile is {catalog!r}, expected {expected['catalog']}")
    if expected["manufacturer"] not in sections:
        errors.append(f"missing manufacturer section {expected['manufacturer']}")

    driver_ver = assignment_value(version, "DriverVer") or ""
    version_match = re.fullmatch(
        r"\s*\d{1,2}/\d{1,2}/\d{4}\s*,\s*7\.15\.(\d{1,2})\.(\d{1,4})\s*",
        driver_ver,
    )
    if version_match is None or not (1 <= int(version_match.group(1)) <= 99):
        errors.append("DriverVer must use the Vista DirectX 10 WDDM 7.15.01.0000-7.15.99.9999 range")

    control_flags = {normalize(line) for line in sections.get("[controlflags]", [])}
    if "excludefromselect=*" not in control_flags:
        errors.append("ControlFlags must exclude this WDDM driver from manual selection")

    install = sections.get("[viogpu3d_inst]", [])
    if normalize(assignment_value(install, "FeatureScore") or "") != "f6":
        errors.append("the Vista vendor WDDM package must set FeatureScore=F6")

    source_files = file_names(sections.get("[sourcedisksfiles]", []))
    service_present = False
    if arch == "x86":
        service_files = expected["files"] | {expected["service_file"]}
        if source_files == service_files:
            service_present = True
        elif source_files != expected["files"]:
            errors.append(
                "SourceDisksFiles must use exactly the driver-only or "
                "deployment-service x86 layout"
            )
    elif source_files != expected["files"]:
        errors.append(
            "SourceDisksFiles does not match required files: "
            + ", ".join(sorted(expected["files"]))
        )
    copied_umds = set()
    for section, required_files in expected["copy_sections"].items():
        section_content = sections.get(section, [])
        actual_files = file_names(section_content)
        copied_umds.update(actual_files)
        if actual_files != required_files:
            errors.append(
                f"{section} does not match required UMDs: "
                + ", ".join(sorted(required_files))
            )
        copy_lines = {normalize(line) for line in section_content}
        for required_file in required_files:
            if f"{required_file},,,0x00004000" not in copy_lines:
                errors.append(f"{section} is missing the PnP-stop copy flag for {required_file}")
    if copied_umds != expected["umds"]:
        errors.append("user-mode copy sections do not cover exactly the required UMDs")

    destinations = {normalize(line) for line in sections.get("[destinationdirs]", [])}
    if not expected["destinations"].issubset(destinations):
        errors.append("user-mode destination directories do not match the Vista architecture profile")

    registry = {normalize(line) for line in sections.get("[viogpu3d_devicesettings]", [])}
    if registry != expected["registry"]:
        errors.append("user-mode driver registry values do not match the Vista package profile")

    if arch == "x64" or service_present:
        deploy_files = file_names(sections.get("[tritonvistadeploy_files]", []))
        if deploy_files != {"triton-vista-deploy.exe"}:
            errors.append("deployment-service copy section is invalid")
        deploy_copy_lines = {
            normalize(line) for line in sections.get("[tritonvistadeploy_files]", [])
        }
        if "triton-vista-deploy.exe,,,0x00004000" not in deploy_copy_lines:
            errors.append("deployment-service copy does not use the PnP-stop flag")
        services = {normalize(line) for line in sections.get("[viogpu3d_inst.services]", [])}
        if "addservice=tritonvistadeploy,0x00000000,tritonvistadeploy_service_inst" not in services:
            errors.append("deployment service is not installed by the display package")
        service = {normalize(line) for line in sections.get("[tritonvistadeploy_service_inst]", [])}
        required_service = {
            "servicetype=%service_win32_own_process%",
            "starttype=%service_auto_start%",
            "errorcontrol=%service_error_normal%",
            "servicebinary=%11%\\triton-vista-deploy.exe--service",
        }
        if not required_service.issubset(service):
            errors.append("deployment service does not have the Vista auto-start contract")
        safe_boot = {normalize(line) for line in sections.get("[tritonvistadeploy_safeboot]", [])}
        required_safe_boot = {
            'hklm,"system\\currentcontrolset\\control\\safeboot\\minimal\\tritonvistadeploy",,0x00000000,"service"',
            'hklm,"system\\currentcontrolset\\control\\safeboot\\network\\tritonvistadeploy",,0x00000000,"service"',
        }
        if safe_boot != required_safe_boot:
            errors.append("deployment service is not registered in both SafeBoot lists")
    elif arch == "x86":
        service_sections = {
            "[tritonvistadeploy_files]",
            "[tritonvistadeploy_service_inst]",
            "[tritonvistadeploy_safeboot]",
        }
        if service_sections & set(sections):
            errors.append("driver-only x86 layout must not contain deployment-service sections")

    if package_dir is not None:
        required_files = source_files | {expected["catalog"]}
        for name in required_files:
            if not (package_dir / name).is_file():
                errors.append(f"package is missing {name}")
    return errors


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", choices=sorted(EXPECTED), required=True)
    parser.add_argument(
        "--package-dir",
        type=Path,
        help="require the package files and catalog in this directory",
    )
    parser.add_argument("inf", type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        errors = validate(args.inf, args.arch, args.package_dir)
    except OSError as exc:
        print(f"{args.inf}: {exc}", file=sys.stderr)
        return 1
    if errors:
        for error in errors:
            print(f"{args.inf}: {error}", file=sys.stderr)
        return 1
    print(f"{args.inf}: Vista INF audit passed ({args.arch})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
