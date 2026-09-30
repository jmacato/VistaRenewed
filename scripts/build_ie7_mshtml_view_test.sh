#!/usr/bin/env bash
set -euo pipefail
i686-w64-mingw32-g++ -std=c++11 -Os -Wall -Wextra -Werror -static -Wl,--subsystem,console:6.0 \
    tools/ie7_mshtml_view_test.cpp -o build/ie7-mshtml-view-test.exe \
    -lole32 -loleaut32 -luuid -lurlmon

python3 triton-kmd/viogpu/tools/check_vista_pe.py --kind exe --arch x86 \
    --allow-import ole32.dll --allow-import oleaut32.dll --allow-import urlmon.dll \
    build/ie7-mshtml-view-test.exe
