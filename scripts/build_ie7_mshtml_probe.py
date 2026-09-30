#!/usr/bin/env python3
"""Build a Vista-compatible, read-only MSHTML activation probe."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent.parent
OUTPUT = "build/triton-ie7-mshtml-probe.exe"


def builder(*command: str) -> None:
    subprocess.run([str(ROOT / "scripts/dev-container.sh"), "run", *command],
                   cwd=ROOT, check=True)


def main() -> None:
    (ROOT / "build").mkdir(exist_ok=True)
    builder(
        "x86_64-w64-mingw32-gcc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
        "-static", "-static-libgcc", "-Wl,--disable-high-entropy-va",
        "-Wl,--subsystem,console:6.0", "tools/ie7_mshtml_probe.c", "-o", OUTPUT,
        "-lole32", "-loleaut32", "-ladvapi32",
    )
    builder(
        "python3", "triton-kmd/viogpu/tools/check_vista_pe.py", "--kind", "exe",
        "--arch", "x64", "--allow-import", "ole32.dll", "--allow-import", "oleaut32.dll",
        OUTPUT,
    )
    print("IEXPLORE MSHTML PROBE BUILD VERIFIED")


if __name__ == "__main__":
    main()
