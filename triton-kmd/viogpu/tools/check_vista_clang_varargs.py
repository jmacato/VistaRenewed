#!/usr/bin/env python3
"""Check that optimized Clang x86 Vista builds use ABI-aware varargs.

WDK 7.1's x86 va_start macro derives the argument list from the address of a
named argument.  Clang does not preserve that address relationship when it
inlines optimized code.  This probe compiles the same forced compatibility
header used by the KMD and verifies that the generated LLVM IR keeps a
compiler varargs start for a distinct va_list slot while passing a separate
output buffer to the formatting call.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path


root = Path(__file__).resolve().parents[3]
wdk = root / "driver/toolchains/wdk71/extracted/WinDDK/7600.16385.win7_wdk.100208-1538"
header = root / "triton-kmd/viogpu/build-support/vista-clang.h"

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument(
    "--legacy-wdk-varargs",
    action="store_true",
    help="compile with the unmodified WDK 7.1 x86 macros; this must fail",
)
args = parser.parse_args()

source = r'''
#include <stdarg.h>

extern "C" __declspec(noinline) int format_with_list(
    char *output, const char *format, va_list arguments);

static __forceinline int capture(const char *format, ...)
{
    char output[256];
    va_list arguments;
    va_start(arguments, format);
    int result = format_with_list(output, format, arguments);
    va_end(arguments);
    return result;
}

extern "C" __declspec(noinline) int caller()
{
    return capture("---> VIOGPU %s %s", "DATE", "TIME");
}
'''

with tempfile.TemporaryDirectory(prefix="vista-clang-varargs-") as temporary:
    ir = Path(temporary) / "probe.ll"
    command = [
        "clang",
        "--target=i686-pc-windows-msvc",
        "-x",
        "c++",
        "-std=c++14",
        "-O2",
        "-S",
        "-emit-llvm",
        "-fno-discard-value-names",
        # Match /Gz in the KMD build.  Variadic capture itself remains cdecl.
        "-mrtd",
        "-D_X86_",
        "-DWINVER=0x0600",
        "-D_WIN32_WINNT=0x0600",
        "-DNTDDI_VERSION=0x06000200",
        "-I",
        str(wdk / "inc/ddk"),
        "-I",
        str(wdk / "inc/api"),
        "-I",
        str(wdk / "inc/crt"),
        "-o",
        str(ir),
        "-",
    ]
    if not args.legacy_wdk_varargs:
        command[command.index("-I"):command.index("-I")] = ["-include", str(header)]
    completed = subprocess.run(command, input=source, text=True, capture_output=True)
    if completed.returncode:
        sys.stderr.write(completed.stderr)
        raise SystemExit("Clang Vista x86 varargs probe did not compile")
    text = ir.read_text()

errors = []
if "%output = alloca [256 x i8]" not in text:
    errors.append("optimized probe lost the distinct formatting output buffer")
if "%arguments = alloca ptr" not in text:
    errors.append("optimized probe lost the va_list storage")
if not re.search(r"@llvm\.va_start[^\n]*%arguments", text):
    errors.append("va_start did not lower to Clang's ABI-aware intrinsic")
if not re.search(r"load ptr, ptr %arguments", text):
    errors.append("formatting call does not read its list from va_list storage")

call = re.search(
    r"format_with_list[^\n]*\(ptr[^,]*%output, ptr[^,]*[^,]*, ptr[^)]*%([^ )]+)\)",
    text,
)
if not call:
    errors.append("formatting call does not receive separate output and va_list arguments")
elif call.group(1) in {"output", "format"}:
    errors.append("formatting call aliases its va_list with a named or output argument")
if not re.search(r"call[^\n]*capture[^\n]*DATE[^\n]*TIME", text):
    errors.append("optimized caller did not retain distinct date/time-like varargs")

if errors:
    raise SystemExit("Vista Clang x86 varargs regression: " + "; ".join(errors))

if args.legacy_wdk_varargs:
    raise SystemExit("legacy WDK varargs unexpectedly passed the regression check")

print("Vista Clang x86 optimized varargs regression check passed")
