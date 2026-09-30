#!/usr/bin/env python3
"""Make a read-only ISO containing the pinned Supermium portable runtime."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path, PurePosixPath

ROOT = Path(__file__).resolve().parents[1]
PIN_PATH = ROOT / "packaging/supermium-runtime-pin.json"
PIN = json.loads(PIN_PATH.read_text(encoding="utf-8"))
ARCHIVE = ROOT / PIN["archive_path"]
BUILD = ROOT / "build"
STAGE = BUILD / "supermium-144-runtime"
ISO = BUILD / "supermium-144-runtime.iso"
MANIFEST_NAME = "SUPERMIUM_RUNTIME_MANIFEST.json"
SMOKE_NAME = "smoke-supermium.cmd"
VISIBLE_NAME = "visible-supermium.cmd"
VISIBLE_PAGE = "visible-supermium.html"


def sha256(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def safe_destination(root: Path, name: str) -> Path:
    member = PurePosixPath(name)
    if member.is_absolute() or ".." in member.parts or not member.parts:
        raise RuntimeError(f"unsafe archive entry: {name}")
    destination = root.joinpath(*member.parts)
    if root not in destination.parents and destination != root:
        raise RuntimeError(f"archive entry escapes stage: {name}")
    return destination


def write_smoke(stage: Path) -> None:
    (stage / SMOKE_NAME).write_text(
        "@echo off\r\n"
        "setlocal EnableExtensions EnableDelayedExpansion\r\n"
        "set \"PROFILE=C:\\TritonSupermiumProfile\"\r\n"
        "if not exist \"!PROFILE!\" md \"!PROFILE!\"\r\n"
        "cd /d \"%~dp0Supermium\"\r\n"
        "echo SUPERMIUM_SMOKE_BEGIN\r\n"
        "echo SUPERMIUM_SMOKE_VERSION=144.0.7559.256\r\n"
        "chrome.exe --headless --no-sandbox --disable-gpu --no-first-run --disable-background-networking --disable-component-update --disable-default-apps --metrics-recording-only --user-data-dir=\"!PROFILE!\" --dump-dom \"data:text/html,%%3Cmain%%20id%%3D%%22triton-supermium%%22%%3ETRITON_SUPERMIUM_144_DOM_OK%%3C/main%%3E\" > \"!PROFILE!\\supermium-dom.txt\" 2>&1\r\n"
        "set \"DOM_RC=!ERRORLEVEL!\"\r\n"
        "type \"!PROFILE!\\supermium-dom.txt\"\r\n"
        "findstr /c:\"TRITON_SUPERMIUM_144_DOM_OK\" \"!PROFILE!\\supermium-dom.txt\" >nul\r\n"
        "set \"MARKER_RC=!ERRORLEVEL!\"\r\n"
        "echo SUPERMIUM_SMOKE_DOM_EXIT=!DOM_RC!\r\n"
        "echo SUPERMIUM_SMOKE_MARKER_EXIT=!MARKER_RC!\r\n"
        "if not \"!DOM_RC!\"==\"0\" exit /b !DOM_RC!\r\n"
        "exit /b !MARKER_RC!\r\n",
        encoding="ascii",
        newline="",
    )


def write_visible(stage: Path) -> None:
    (stage / VISIBLE_PAGE).write_text(
        "<!doctype html>\n"
        "<meta charset=\"utf-8\">\n"
        "<title>Supermium 144 Vista renderer smoke</title>\n"
        "<style>body{background:#20252d;color:#e9edf5;font:20px Segoe UI,Arial;padding:3em}"
        "main{max-width:48rem}code{color:#8dd6ff}strong{color:#a9ef95}</style>\n"
        "<main id=\"triton-supermium-visible\">\n"
        "<h1>Supermium 144 is rendering on Vista</h1>\n"
        "<p><strong>Chromium 144.0.7559.256 R5</strong> is running from the disposable test ISO.</p>\n"
        "<p>This is the future out-of-process renderer target for the private IE7 bridge. "
        "Stock <code>mshtml.dll</code> and IE registrations remain untouched.</p>\n"
        "</main>\n",
        encoding="utf-8",
    )
    (stage / VISIBLE_NAME).write_text(
        "@echo off\r\n"
        "setlocal EnableExtensions\r\n"
        "cd /d \"%~dp0Supermium\"\r\n"
        "start \"Supermium Vista renderer smoke\" chrome.exe --no-first-run --disable-background-networking --disable-component-update --disable-default-apps --disable-gpu --user-data-dir=\"%LOCALAPPDATA%\\TritonSupermiumVisibleProfile\" \"file:///%~dp0visible-supermium.html\"\r\n"
        "exit /b %ERRORLEVEL%\r\n",
        encoding="ascii",
        newline="",
    )


def main() -> None:
    argparse.ArgumentParser(description=__doc__).parse_args()
    if not ARCHIVE.is_file():
        raise SystemExit(f"missing pinned archive: {ARCHIVE}")
    if sha256(ARCHIVE) != PIN["sha256"]:
        raise SystemExit("pinned archive SHA-256 mismatch")
    if STAGE.exists() or ISO.exists():
        raise SystemExit(f"refusing to overwrite existing output: {STAGE} or {ISO}")
    if shutil.which("xorriso") is None:
        raise SystemExit("xorriso is required to create the read-only runtime ISO")

    BUILD.mkdir(exist_ok=True)
    temporary = Path(tempfile.mkdtemp(prefix="supermium-144-", dir=BUILD))
    stage = temporary / "stage"
    staged_root = stage / PIN["portable_root"]
    try:
        with zipfile.ZipFile(ARCHIVE) as archive:
            entries = [entry for entry in archive.infolist() if entry.filename.startswith("Supermium/")]
            if not entries:
                raise RuntimeError("the archive has no portable Supermium tree")
            for entry in entries:
                destination = safe_destination(stage, entry.filename)
                if entry.is_dir():
                    destination.mkdir(parents=True, exist_ok=True)
                    continue
                destination.parent.mkdir(parents=True, exist_ok=True)
                with archive.open(entry) as source, destination.open("wb") as target:
                    shutil.copyfileobj(source, target)

        write_smoke(stage)
        write_visible(stage)
        files = []
        for path in sorted(item for item in stage.rglob("*") if item.is_file()):
            files.append({"path": path.relative_to(stage).as_posix(), "sha256": sha256(path), "bytes": path.stat().st_size})
        manifest = {
            "release_tag": PIN["release_tag"],
            "product_version": PIN["product_version"],
            "archive": PIN["archive_path"],
            "archive_sha256": PIN["sha256"],
            "launch": "D:\\smoke-supermium.cmd",
            "runtime_root": "D:\\Supermium",
            "files": files,
        }
        (stage / MANIFEST_NAME).write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")

        temporary_iso = temporary / ISO.name
        subprocess.run(
            ["xorriso", "-as", "mkisofs", "-iso-level", "3", "-J", "-R", "-V", "SUPERMUM144", "-o", str(temporary_iso), str(stage)],
            check=True,
        )
        os.replace(stage, STAGE)
        os.replace(temporary_iso, ISO)
    finally:
        shutil.rmtree(temporary, ignore_errors=True)

    print(f"SUPERMIUM RUNTIME ISO CREATED: {ISO}")
    print(f"SUPERMIUM RUNTIME FILES: {len(files)}")


if __name__ == "__main__":
    main()
