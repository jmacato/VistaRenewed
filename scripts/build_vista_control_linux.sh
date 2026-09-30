#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
mkdir -p build
x86_64-w64-mingw32-gcc -std=c11 -O2 -Wall -Wextra -Werror -static -static-libgcc -municode \
    -Wl,--subsystem,console:6.0 -Wl,--disable-high-entropy-va \
    packaging/vista-control-service.c -ladvapi32 -o build/triton-vista-control.exe
python3 triton-kmd/viogpu/tools/check_vista_pe.py --kind exe --arch x64 build/triton-vista-control.exe
