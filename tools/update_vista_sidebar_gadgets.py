#!/usr/bin/env python3
"""Restore Vista's shipped Sidebar pages and replace only their data providers."""

from __future__ import annotations

import argparse
import hashlib
import subprocess
import sys
import tempfile
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CONTROL = ROOT / "tools" / "vista_control.py"
ASSETS = ROOT / "packaging" / "vista-sidebar-gadgets"
BACKUP = r"C:\ProgramData\TritonSidebarGadgets\original"
SIDEBAR_TASK = "TritonSidebarHost"
SIDEBAR_EXECUTABLE = r'\"C:\Program Files\Windows Sidebar\sidebar.exe\"'
RSS_SETUP = ASSETS / "configure_rss_feed.js"
RSS_BACKEND = ASSETS / "rss_backend.js"
CURRENCY_BACKEND = ASSETS / "currency_service_backend.js"
WEATHER_BACKEND = ASSETS / "weather_backend.js"
WEATHER_SETTINGS_BACKEND = ASSETS / "weather_settings_backend.js"

UI_FILES = {
    r"C:\Program Files\Windows Sidebar\Gadgets\RSSFeeds.Gadget\en-US\RSSFeeds.html": "RSSFeeds.html",
    r"C:\Program Files\Windows Sidebar\Gadgets\Weather.Gadget\en-US\weather.html": "weather.html",
    r"C:\Program Files\Windows Sidebar\Gadgets\Currency.Gadget\en-US\currency.html": "currency.html",
}
WEATHER_SCRIPT = r"C:\Program Files\Windows Sidebar\Gadgets\Weather.Gadget\en-US\js\weather.js"
WEATHER_SETTINGS_SCRIPT = r"C:\Program Files\Windows Sidebar\Gadgets\Weather.Gadget\en-US\js\settings.js"
CURRENCY_SCRIPT = r"C:\Program Files\Windows Sidebar\Gadgets\Currency.Gadget\en-US\js\service.js"
RSS_SCRIPT = r"C:\Program Files\Windows Sidebar\Gadgets\RSSFeeds.Gadget\en-US\js\RSSFeeds.js"
BACKEND_FILES = {
    RSS_SCRIPT: "rss.backend-original.js",
    WEATHER_SCRIPT: "weather.backend-original.js",
    WEATHER_SETTINGS_SCRIPT: "weather-settings.backend-original.js",
    CURRENCY_SCRIPT: "currency.backend-original.js",
}


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
    guest(f'if not exist "{BACKUP}" mkdir "{BACKUP}"')
    for remote_path, name in files.items():
        destination = backup_path(name)
        guest(f'if not exist "{destination}" copy /y "{remote_path}" "{destination}"')


def remote_sha256(remote_path: str) -> str:
    with tempfile.TemporaryDirectory(prefix="vista-sidebar-sha-") as directory:
        downloaded = Path(directory) / "file"
        control("get", remote_path, str(downloaded))
        return hashlib.sha256(downloaded.read_bytes()).hexdigest()


def verify_shipped_pages() -> None:
    for remote_path, name in UI_FILES.items():
        original_path = backup_path(name)
        if remote_sha256(remote_path) != remote_sha256(original_path):
            raise RuntimeError(f"the original gadget UI is not restored: {remote_path}")


def to_utf16_file(text: str, destination: Path) -> None:
    destination.write_bytes(b"\xff\xfe" + text.replace("\n", "\r\n").encode("utf-16le"))


def patched_weather_script(original: bytes) -> bytes:
    try:
        source = original.decode("utf-16")
    except UnicodeDecodeError as error:
        raise RuntimeError("Weather data script is not UTF-16") from error
    start = source.find("  try \r\n  {\t\t\r\n    // Connect to Weather Service .dll")
    end = source.find("  ////////////////////////////////////////////////////////////////////////////////\r\n  //\r\n  // Public Methods", start)
    if start == -1 or end == -1:
        raise RuntimeError("could not locate the retired Weather service provider")
    replacement = "  // Data provider; the original display and behavior below are unchanged.\r\n  this.oMSN = new LocalWeatherService();\r\n\r\n"
    backend = WEATHER_BACKEND.read_text(encoding="utf-8").replace("\n", "\r\n")
    return (source[:start] + replacement + source[end:] + "\r\n" + backend).encode("utf-16")


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
    old_settings = (
        '  var theWeatherLocation            = unescape(readSetting("WeatherLocation")) || gDefaultWeatherLocation;\r\n'
        '  var theWeatherLocationCode        = readSetting("WeatherLocationCode") || gDefaultWeatherLocationCode;\r\n'
        '  var theDisplayDegreesIn            = readSetting("DisplayDegreesIn")   || gDefaultDisplayDegreesIn;'
    )
    new_settings = (
        '  var storedWeatherLocation = unescape(readSetting("WeatherLocation"));\r\n'
        '  var storedWeatherLocationCode = readSetting("WeatherLocationCode");\r\n'
        '  var theWeatherLocation = storedWeatherLocation || "Manila";\r\n'
        '  var theWeatherLocationCode = storedWeatherLocationCode || "14.5995;120.9842;Manila";\r\n'
        '  var theDisplayDegreesIn = readSetting("DisplayDegreesIn") || gDefaultDisplayDegreesIn;'
    )
    old_service = (
        '  // Create instance of MSNServices.dll and attach to our Global Object\r\n'
        '  var oMSN = new ActiveXObject("wlsrvc.WLServices");\r\n'
        '  MicrosoftGadget.oMSN = oMSN.GetService("weather"); '
    )
    new_service = '  MicrosoftGadget.oMSN = new LocalWeatherLookupService(); '
    if old_settings not in source or old_service not in source:
        raise RuntimeError("could not locate the retired Weather settings provider")
    source = source.replace(old_settings, new_settings, 1).replace(old_service, new_service, 1)
    backend = WEATHER_SETTINGS_BACKEND.read_text(encoding="utf-8").replace("\n", "\r\n")
    return (source + "\r\n" + backend).encode("utf-16")


def install_backends() -> None:
    with tempfile.TemporaryDirectory(prefix="vista-sidebar-backend-") as directory:
        temporary = Path(directory)
        weather_original = temporary / "weather-original.js"
        weather_patched = temporary / "weather.js"
        weather_settings_original = temporary / "weather-settings-original.js"
        weather_settings_patched = temporary / "weather-settings.js"
        currency_patched = temporary / "currency-service.js"
        rss_original = temporary / "rss-original.js"
        rss_patched = temporary / "RSSFeeds.js"
        control("get", backup_path(BACKEND_FILES[WEATHER_SCRIPT]), str(weather_original))
        control("get", backup_path(BACKEND_FILES[WEATHER_SETTINGS_SCRIPT]), str(weather_settings_original))
        control("get", backup_path(BACKEND_FILES[RSS_SCRIPT]), str(rss_original))
        weather_patched.write_bytes(patched_weather_script(weather_original.read_bytes()))
        weather_settings_patched.write_bytes(patched_weather_settings_script(weather_settings_original.read_bytes()))
        rss_patched.write_bytes(appended_backend_script(rss_original.read_bytes(), RSS_BACKEND))
        to_utf16_file(CURRENCY_BACKEND.read_text(encoding="utf-8"), currency_patched)
        for remote_path in (RSS_SCRIPT, WEATHER_SCRIPT, WEATHER_SETTINGS_SCRIPT, CURRENCY_SCRIPT):
            guest(f'takeown /f "{remote_path}" /a & icacls "{remote_path}" /grant *S-1-5-18:(F)')
        control("put", str(rss_patched), RSS_SCRIPT)
        control("put", str(weather_patched), WEATHER_SCRIPT)
        control("put", str(weather_settings_patched), WEATHER_SETTINGS_SCRIPT)
        control("put", str(currency_patched), CURRENCY_SCRIPT)


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
    backup_files(UI_FILES)
    verify_shipped_pages()
    backup_files(BACKEND_FILES)
    install_backends()
    configure_rss()
    if not args.no_restart:
        restart_sidebar(args.user)
    print("VISTA SIDEBAR DATA PROVIDERS UPDATED; ORIGINAL UI PRESERVED")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
