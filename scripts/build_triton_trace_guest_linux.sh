#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
mkdir -p build
x86_64-w64-mingw32-gcc -std=gnu11 -O2 -s -Wall -Wextra -Werror \
    -Wno-cast-function-type -municode -Wl,--subsystem,console:6.0 \
    -Wl,--disable-high-entropy-va tools/triton_trace_guest.c \
    -lwtsapi32 -luserenv -ladvapi32 -lgdi32 -luser32 -o build/triton-trace-guest.exe
python3 triton-kmd/viogpu/tools/check_vista_pe.py --kind exe --arch x64 \
    --allow-import gdi32.dll --allow-import wtsapi32.dll --allow-import userenv.dll \
    build/triton-trace-guest.exe
