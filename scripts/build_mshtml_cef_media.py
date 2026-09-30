#!/usr/bin/env python3
"""Assemble the disposable controller and MSHTML-dumper ISO for winvista-3."""

from pathlib import Path
import shutil
import subprocess


ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"
MEDIA = BUILD / "mshtml-cef-media"
ISO = BUILD / "mshtml-cef-media.iso"
INPUTS = {
    "triton-vista-control.exe": BUILD / "triton-vista-control.exe",
    "mshtml-interface-dump.exe": BUILD / "mshtml-interface-dump.exe",
}
COMMANDS = {
    "install-control.cmd": "@echo off\r\n\"%~dp0triton-vista-control.exe\" --install > C:\\control-install.txt 2>&1\r\ntype C:\\control-install.txt > COM1\r\n",
    "dump-mshtml.cmd": "@echo off\r\n\"%~dp0mshtml-interface-dump.exe\" C:\\Windows\\Temp\\mshtml-interface-dump.txt\r\n",
    "README.txt": "Run install-control.cmd from an elevated Vista prompt. After the service is running, use the host control client to run dump-mshtml.cmd and retrieve C:\\Windows\\Temp\\mshtml-interface-dump.txt.\r\n",
}


def main():
    for path in INPUTS.values():
        if not path.is_file():
            raise SystemExit(f"missing built input: {path}")
    if MEDIA.exists():
        raise SystemExit(f"refusing to overwrite existing media directory: {MEDIA}")
    if ISO.exists():
        raise SystemExit(f"refusing to overwrite existing ISO: {ISO}")
    MEDIA.mkdir()
    for name, source in INPUTS.items():
        shutil.copy2(source, MEDIA / name)
    for name, content in COMMANDS.items():
        (MEDIA / name).write_bytes(content.encode("ascii"))
    subprocess.run(["xorriso", "-as", "mkisofs", "-volid", "MSHTML_CEF_DEV", "-J", "-r",
                    "-o", str(ISO), str(MEDIA)], check=True)
    print(f"MSHTML CEF development ISO created: {ISO}")


if __name__ == "__main__":
    main()
