#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
mkdir -p build
x86_64-w64-mingw32-g++ -std=c++11 -Os -Wall -Wextra -Werror -static -municode \
    -Wl,--subsystem,console:6.0 tools/vista_uia.cpp -o build/vista-uia.exe \
    -lole32 -loleaut32 -luuid
