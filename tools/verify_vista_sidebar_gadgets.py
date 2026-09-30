#!/usr/bin/env python3
"""Verify that Vista Sidebar's original UI remains intact and its data works."""

from __future__ import annotations

import argparse
import os
import hashlib
import py_compile
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CONTROL = ROOT / "tools" / "vista_control.py"
ASSETS = ROOT / "packaging" / "vista-sidebar-gadgets"
PROXY = ASSETS / "vista_sidebar_proxy.py"
CONTAINER = os.environ.get("VISTA_VM_NAME", "triton-vista-x64-normal")
SOCKET_ARGS = []
BACKUP = r"C:\ProgramData\TritonSidebarGadgets\original"
UI_FILES = {
    r"C:\Program Files\Windows Sidebar\Gadgets\RSSFeeds.Gadget\en-US\RSSFeeds.html": "RSSFeeds.html",
    r"C:\Program Files\Windows Sidebar\Gadgets\Weather.Gadget\en-US\weather.html": "weather.html",
    r"C:\Program Files\Windows Sidebar\Gadgets\Currency.Gadget\en-US\currency.html": "currency.html",
}
BACKENDS = {
    r"C:\Program Files\Windows Sidebar\Gadgets\RSSFeeds.Gadget\en-US\js\RSSFeeds.js": ("g_localRssUrl", "http://10.0.2.2:8765/news"),
    r"C:\Program Files\Windows Sidebar\Gadgets\Weather.Gadget\en-US\js\weather.js": ("LocalWeatherService", "http://10.0.2.2:8765/weather"),
    r"C:\Program Files\Windows Sidebar\Gadgets\Weather.Gadget\en-US\js\settings.js": ("LocalWeatherLookupService", "http://10.0.2.2:8765/weather-search"),
    r"C:\Program Files\Windows Sidebar\Gadgets\Currency.Gadget\en-US\js\service.js": ("CurrencyService", "http://10.0.2.2:8765/currency"),
}


def run(command: list[str]) -> str:
    result = subprocess.run(command, cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(result.stdout.strip() or "command failed")
    return result.stdout


def get(remote: str, local: Path) -> None:
    run([sys.executable, str(CONTROL), *SOCKET_ARGS, "get", remote, str(local)])


def verify_source() -> None:
    required = (
        ASSETS / "configure_rss_feed.js",
        ASSETS / "rss_backend.js",
        ASSETS / "weather_backend.js",
        ASSETS / "weather_settings_backend.js",
        ASSETS / "currency_service_backend.js",
        ASSETS / "guest-http-probe.js",
        PROXY,
    )
    for path in required:
        if not path.is_file():
            raise RuntimeError(f"missing source asset: {path.relative_to(ROOT)}")
    for retired_ui in ("RSSFeeds.html", "weather.html", "currency.html"):
        if (ASSETS / retired_ui).exists():
            raise RuntimeError(f"obsolete UI replacement remains: {retired_ui}")
    source_text = "\n".join(path.read_text(encoding="utf-8") for path in required)
    for marker in ("10.0.2.2:8765/news", "10.0.2.2:8765/weather", "10.0.2.2:8765/weather-search", "10.0.2.2:8765/currency"):
        if marker not in source_text:
            raise RuntimeError(f"missing local provider marker: {marker}")
    if "wlsrvc.WLServices" in source_text:
        raise RuntimeError("retired Windows Live service remains in a replacement backend")
    py_compile.compile(str(PROXY), doraise=True)
    print("SIDEBAR BACKEND SOURCES VERIFIED")


def verify_guest() -> None:
    health = run([
        "podman", "exec", CONTAINER, "python3", "-c",
        'from urllib.request import urlopen; assert urlopen("http://127.0.0.1:8765/health", timeout=5).status == 200',
    ])
    if health:
        raise RuntimeError("unexpected relay health output")
    with tempfile.TemporaryDirectory(prefix="vista-sidebar-verify-") as temp_dir:
        temporary = Path(temp_dir)
        for remote_path, backup_name in UI_FILES.items():
            active = temporary / (backup_name + ".active")
            original = temporary / (backup_name + ".original")
            get(remote_path, active)
            get(BACKUP + "\\" + backup_name, original)
            if hashlib.sha256(active.read_bytes()).digest() != hashlib.sha256(original.read_bytes()).digest():
                raise RuntimeError(f"Sidebar UI differs from its original backup: {remote_path}")
        for index, (remote_path, markers) in enumerate(BACKENDS.items()):
            local = temporary / f"backend-{index}.js"
            get(remote_path, local)
            text = local.read_bytes().decode("utf-16")
            if not all(marker in text for marker in markers):
                raise RuntimeError(f"local data adapter is missing from: {remote_path}")
    remote_probe = r"C:\Windows\Temp\TritonSidebarHttpProbe.js"
    run([sys.executable, str(CONTROL), "put", str(ASSETS / "guest-http-probe.js"), remote_probe])
    output = run([sys.executable, str(CONTROL), "run", "--timeout", "300", f"cscript //nologo {remote_probe}"])
    for marker in ("NEWS OK", "NEWS DESCRIPTION OK", "WEATHER OK", "WEATHER SEARCH OK", "CURRENCY OK", "CURRENCY LABELS OK"):
        if marker not in output:
            raise RuntimeError("guest network probe did not prove " + marker)
    process = run([sys.executable, str(CONTROL), "run", "--timeout", "120", 'tasklist /fi "imagename eq sidebar.exe"'])
    if not any(line.lstrip().lower().startswith("sidebar.exe") for line in process.splitlines()):
        raise RuntimeError("Sidebar is not running")
    print("SIDEBAR UI INTEGRITY AND GUEST DATA VERIFIED")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--source", action="store_true")
    group.add_argument("--guest", action="store_true")
    parser.add_argument("--vm-name", default=CONTAINER, help="running VM container")
    parser.add_argument("--socket", help="guest control socket")
    args = parser.parse_args()
    globals()["CONTAINER"] = args.vm_name
    globals()["SOCKET_ARGS"] = ["--socket", args.socket] if args.socket else []
    if args.source:
        verify_source()
    else:
        verify_guest()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
