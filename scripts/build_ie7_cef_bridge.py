#!/usr/bin/env python3
"""Build the private IE7 automation facade and its target test host for Vista."""

from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parent.parent
GCC = "x86_64-w64-mingw32-gcc"
AUDIT = "triton-kmd/viogpu/tools/check_vista_pe.py"
BRIDGE = "build/triton-ie7-cef-bridge.dll"
HOST = "build/triton-ie7-cef-bridge-host.exe"
SUPERMIUM_HOST = "build/triton-ie7-supermium-bridge-host.exe"


def in_builder(*command: str) -> None:
    subprocess.run(
        [str(ROOT / "scripts/dev-container.sh"), "run", *command],
        cwd=ROOT,
        check=True,
    )


def audit(artifact: str, *extra: str) -> None:
    in_builder(
        "python3", AUDIT, "--kind", "exe", "--arch", "x64",
        "--allow-import", "ole32.dll", "--allow-import", "oleaut32.dll",
        *extra, artifact,
    )


def main() -> None:
    (ROOT / "build").mkdir(exist_ok=True)
    common = (
        "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-static", "-static-libgcc",
        "-Wl,--disable-high-entropy-va",
    )
    in_builder(
        GCC, *common, "-shared", "-Wl,--subsystem,windows:6.0",
        "tools/ie7_cef_bridge.c", "-o", BRIDGE,
        "-ladvapi32", "-lole32", "-loleaut32",
    )
    audit(
        BRIDGE,
        "--allow-import", "advapi32.dll",
        "--require-export", "DllGetClassObject",
        "--require-export", "DllCanUnloadNow",
        "--require-export", "DllRegisterServer",
        "--require-export", "DllUnregisterServer",
    )
    in_builder(
        GCC, *common, "-municode", "-Wl,--subsystem,console:6.0",
        "tools/ie7_cef_bridge_host.c", "-o", HOST, "-lole32", "-loleaut32",
    )
    audit(HOST)
    in_builder(
        GCC, *common, "-municode", "-Wl,--subsystem,console:6.0",
        "tools/ie7_cef_bridge_host.c", "-o", SUPERMIUM_HOST, "-lole32", "-loleaut32",
    )
    audit(SUPERMIUM_HOST)
    print("IE7 CEF BRIDGE BUILD VERIFIED")
    print("IE7 SUPERMIUM ADAPTER BUILD VERIFIED")


if __name__ == "__main__":
    main()
