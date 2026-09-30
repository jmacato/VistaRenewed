#!/usr/bin/env python3
"""Build the IE/MSHTML adapter and navigation hosts for Vista."""
from pathlib import Path
import argparse
import subprocess

ROOT = Path(__file__).resolve().parent.parent
OUTPUT = "build/triton-mshtml-x86.dll"
STUBS = "build/ie7_ihtmldocument2_stubs.inc"


def builder(*command: str) -> None:
    subprocess.run([str(ROOT / "scripts/dev-container.sh"), "run", *command],
                   cwd=ROOT, check=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--both', action='store_true', help='also build the native x64 DLL and contract host')
    args = parser.parse_args()
    (ROOT / "build").mkdir(exist_ok=True)
    builder(
        "python3", "tools/generate_ihtmldocument2_stubs.py",
        "/usr/i686-w64-mingw32/include/mshtml.h", STUBS,
    )
    builder(
        "python3", "tools/generate_mshtml_element_stubs.py",
        "/usr/i686-w64-mingw32/include/mshtml.h", "build/ie7_element_stubs.inc",
    )
    builder(
        "python3", "tools/generate_mshtml_window_stubs.py",
        "/usr/i686-w64-mingw32/include/mshtml.h", "build/ie7_window_stubs.inc",
    )
    builder(
        "i686-w64-mingw32-gcc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
        "-Wno-unused-parameter",
        "-static", "-static-libgcc", "-shared", "-Wl,--kill-at", "-Wl,--no-insert-timestamp",
        "-Wl,--subsystem,windows:6.0", "tools/mshtml_adapter.c", "-o", OUTPUT,
        "-lole32", "-loleaut32", "-lgdi32", "-luuid", "-lshell32", "-ladvapi32", "-lurlmon",
    )
    builder(
        "python3", "triton-kmd/viogpu/tools/check_vista_pe.py", "--kind", "exe",
        "--arch", "x86", "--allow-import", "ole32.dll", "--allow-import", "oleaut32.dll",
        "--allow-import", "gdi32.dll", "--allow-import", "shell32.dll", "--allow-import", "urlmon.dll",
        "--require-export", "DllGetClassObject", "--require-export", "DllCanUnloadNow", OUTPUT,
    )
    builder(
        "i686-w64-mingw32-gcc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-static",
        "-Wl,--subsystem,console:6.0", "tools/ie7_mshtml_navigation_test.c",
        "-o", "build/ie7-mshtml-navigation-test.exe", "-lole32", "-loleaut32", "-luuid", "-lurlmon", "-ladvapi32",
    )
    builder(
        "python3", "triton-kmd/viogpu/tools/check_vista_pe.py", "--kind", "exe", "--arch", "x86",
        "--allow-import", "ole32.dll", "--allow-import", "oleaut32.dll", "--allow-import", "urlmon.dll",
        "build/ie7-mshtml-navigation-test.exe",
    )
    if args.both:
        builder(
            "x86_64-w64-mingw32-gcc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
            "-Wno-unused-parameter", "-static", "-static-libgcc", "-shared", "-Wl,--kill-at",
            "-Wl,--no-insert-timestamp",
            "-Wl,--subsystem,windows:6.0", "tools/mshtml_adapter.c",
            "-o", "build/triton-mshtml-x64.dll", "-lole32", "-loleaut32", "-lgdi32",
            "-luuid", "-lshell32", "-ladvapi32", "-lurlmon",
        )
        builder(
            "python3", "triton-kmd/viogpu/tools/check_vista_pe.py", "--kind", "exe",
            "--arch", "x64", "--allow-import", "ole32.dll", "--allow-import", "oleaut32.dll",
            "--allow-import", "gdi32.dll", "--allow-import", "shell32.dll", "--allow-import", "urlmon.dll",
            "--require-export", "DllGetClassObject", "--require-export", "DllCanUnloadNow",
            "build/triton-mshtml-x64.dll",
        )
        builder(
            "x86_64-w64-mingw32-gcc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-static",
            "-DTRITON_MSHTML_DLL_PATH=L\"C:\\\\TritonSupermiumBridge\\\\triton-mshtml-x64.dll\"",
            "-DTRITON_X64_HOST_CONTRACT=1",
            "-Wl,--subsystem,console:6.0", "tools/ie7_mshtml_navigation_test.c",
            "-o", "build/mshtml-x64-host-test.exe", "-lole32", "-loleaut32", "-luuid", "-lurlmon",
        )
        builder(
            "python3", "triton-kmd/viogpu/tools/check_vista_pe.py", "--kind", "exe", "--arch", "x64",
            "--allow-import", "ole32.dll", "--allow-import", "oleaut32.dll", "--allow-import", "urlmon.dll",
            "build/mshtml-x64-host-test.exe",
        )
        print("MSHTML X64 DLL AND CONTRACT HOST BUILD VERIFIED")
    print("IE/MSHTML BUILD VERIFIED")


if __name__ == "__main__":
    main()
