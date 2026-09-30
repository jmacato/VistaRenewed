#!/usr/bin/env python3
"""Restore Vista's shipped Sidebar pages and replace only their data providers."""

from __future__ import annotations

import argparse
import hashlib
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from vista_sidebar_paths import BACKUP, discover_gadgets, gadget_files


ROOT = Path(__file__).resolve().parents[1]
CONTROL = ROOT / "tools" / "vista_control.py"
ASSETS = ROOT / "packaging" / "vista-sidebar-gadgets"
SIDEBAR_TASK = "TritonSidebarHost"
SIDEBAR_EXECUTABLE = r'\"C:\Program Files\Windows Sidebar\sidebar.exe\"'
RSS_SETUP = ASSETS / "configure_rss_feed.js"
RSS_BACKEND = ASSETS / "rss_backend.js"
CURRENCY_BACKEND = ASSETS / "currency_service_backend.js"
WEATHER_BACKEND = ASSETS / "weather_backend.js"
WEATHER_SETTINGS_BACKEND = ASSETS / "weather_settings_backend.js"

SOCKET_ARGS = []


def control(*arguments: str) -> str:
    result = subprocess.run(
        [sys.executable, str(CONTROL), *SOCKET_ARGS, *arguments],
        cwd=ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    if result.stdout:
        print(result.stdout, end="")
    if result.returncode:
        raise RuntimeError("Vista control command failed: " + " ".join(arguments))
    return result.stdout


def guest(command: str, *, user: bool = False, timeout: int = 180) -> str:
    arguments = ["run"]
    if user:
        arguments.append("--user")
    arguments.extend(["--timeout", str(timeout), command])
    return control(*arguments)


def backup_path(name: str) -> str:
    return BACKUP + "\\" + name


def backup_files(files: dict[str, str]) -> None:
    for remote_path, name in files.items():
        destination = backup_path(name)
        parent = destination.rsplit("\\", 1)[0]
        guest(f'if not exist "{parent}" mkdir "{parent}"')
        guest(f'if not exist "{destination}" copy /y "{remote_path}" "{destination}"')


def remote_sha256(remote_path: str) -> str:
    with tempfile.TemporaryDirectory(prefix="vista-sidebar-sha-") as directory:
        downloaded = Path(directory) / "file"
        control("get", remote_path, str(downloaded))
        return hashlib.sha256(downloaded.read_bytes()).hexdigest()


def verify_shipped_pages(files: dict[str, str]) -> None:
    for remote_path, name in files.items():
        original_path = backup_path(name)
        if remote_sha256(remote_path) != remote_sha256(original_path):
            raise RuntimeError(f"the original gadget UI is not restored: {remote_path}")


def patched_weather_script(original: bytes) -> bytes:
    try:
        source = original.decode("utf-16")
    except UnicodeDecodeError as error:
        raise RuntimeError("Weather data script is not UTF-16") from error
    pattern = (r'try\s*\{\s*(?://[^\n]*\n\s*)*'
               r'var\s+oMSN\s*=\s*new\s+ActiveXObject\(\s*[\'"]wlsrvc\.WLServices[\'"]\s*\)\s*;'
               r'\s*this\.oMSN\s*=\s*oMSN\.GetService\(\s*[\'"]weather[\'"]\s*\)\s*;'
               r'\s*\}\s*catch\s*\([^)]*\)\s*\{[^{}]*\}')
    source, count = re.subn(pattern, 'this.oMSN = new LocalWeatherService();', source)
    if count != 1:
        raise RuntimeError("could not locate the retired Weather service provider")
    backend = WEATHER_BACKEND.read_text(encoding="utf-8").replace("\n", "\r\n")
    return (source + "\r\n" + backend).encode("utf-16")


def appended_backend_script(original: bytes, backend_path: Path) -> bytes:
    try:
        source = original.decode("utf-16")
    except UnicodeDecodeError as error:
        raise RuntimeError(f"{backend_path.name} target is not UTF-16") from error
    backend = backend_path.read_text(encoding="utf-8").replace("\n", "\r\n")
    return (source + "\r\n" + backend).encode("utf-16")


def patched_weather_settings_script(original: bytes) -> bytes:
    try:
        source = original.decode("utf-16")
    except UnicodeDecodeError as error:
        raise RuntimeError("Weather settings data script is not UTF-16") from error
    pattern = r'var\s+oMSN\s*=\s*new\s+ActiveXObject\(\s*[\'"]wlsrvc\.WLServices[\'"]\s*\)\s*;\s*MicrosoftGadget\.oMSN\s*=\s*oMSN\.GetService\(\s*[\'"]weather[\'"]\s*\)\s*;'
    source, count = re.subn(pattern, 'MicrosoftGadget.oMSN = new LocalWeatherLookupService();', source)
    if count != 1:
        raise RuntimeError("could not locate the retired Weather settings provider")
    backend = WEATHER_SETTINGS_BACKEND.read_text(encoding="utf-8").replace("\n", "\r\n")
    return (source + "\r\n" + backend).encode("utf-16")


def install_backends(backends) -> None:
    patchers = {
        "rss": lambda data: appended_backend_script(data, RSS_BACKEND),
        "weather": patched_weather_script,
        "weather-settings": patched_weather_settings_script,
        "currency": lambda data: b"\xff\xfe" + CURRENCY_BACKEND.read_text(encoding="utf-8").replace("\n", "\r\n").encode("utf-16le"),
    }
    # Prepare every language before changing any installed script.
    with tempfile.TemporaryDirectory(prefix="vista-sidebar-backend-") as directory:
        prepared = []
        for index, (remote_path, (name, kind)) in enumerate(backends.items()):
            original = Path(directory) / f"{index}.original"
            patched = Path(directory) / f"{index}.js"
            control("get", backup_path(name), str(original))
            patched.write_bytes(patchers[kind](original.read_bytes()))
            prepared.append((remote_path, patched))
        for remote_path, patched in prepared:
            guest(f'takeown /f "{remote_path}" /a & icacls "{remote_path}" /grant *S-1-5-18:(F)')
            control("put", str(patched), remote_path)


def configure_rss() -> None:
    remote_script = r"C:\Windows\Temp\TritonConfigureRss.js"
    control("put", str(RSS_SETUP), remote_script)
    output = guest(f"cscript //nologo {remote_script}", user=True, timeout=300)
    if "RSS FEED READY" not in output:
        raise RuntimeError("Vista did not add the maintained news subscription")


def restart_sidebar(user: str) -> None:
    guest('taskkill /f /im sidebar.exe >nul 2>&1 & ver >nul')
    guest(
        f'schtasks /end /tn {SIDEBAR_TASK} >nul 2>&1 '
        f'& schtasks /delete /tn {SIDEBAR_TASK} /f >nul 2>&1 '
        f'& schtasks /create /tn {SIDEBAR_TASK} /tr "{SIDEBAR_EXECUTABLE}" '
        f'/sc onlogon /ru "{user}" /it /f '
        f'& schtasks /run /tn {SIDEBAR_TASK}'
    )
    time.sleep(3)
    running = guest('tasklist /fi "imagename eq sidebar.exe"')
    if not any(line.lstrip().lower().startswith("sidebar.exe") for line in running.splitlines()):
        raise RuntimeError("Sidebar did not restart in the interactive user session")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--socket", help="guest control socket (see vista_control.py)")
    parser.add_argument("--user", help="active Windows user that owns the Sidebar")
    parser.add_argument("--no-restart", action="store_true", help="install data providers but leave Sidebar running")
    args = parser.parse_args()
    global SOCKET_ARGS
    SOCKET_ARGS = ["--socket", args.socket] if args.socket else []
    if not args.no_restart and not args.user:
        parser.error("--user DOMAIN\\username is required to restart Sidebar; use --no-restart to install only")
    if args.user and any(c in args.user for c in '\"&|<>^%\r\n'):
        parser.error("invalid Windows account name")

    for asset in (RSS_SETUP, RSS_BACKEND, CURRENCY_BACKEND, WEATHER_BACKEND, WEATHER_SETTINGS_BACKEND):
        if not asset.is_file():
            raise SystemExit(f"missing asset: {asset}")
    pages, backends = gadget_files(discover_gadgets(control))
    backup_files(pages)
    verify_shipped_pages(pages)
    backup_files({path: name for path, (name, kind) in backends.items()})
    install_backends(backends)
    configure_rss()
    if not args.no_restart:
        restart_sidebar(args.user)
    print("VISTA SIDEBAR DATA PROVIDERS UPDATED; ORIGINAL UI PRESERVED")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
