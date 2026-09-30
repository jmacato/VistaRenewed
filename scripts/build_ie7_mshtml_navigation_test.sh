#!/bin/bash
set -euo pipefail
i686-w64-mingw32-gcc -std=c11 -O2 -Wall -Wextra -Werror -static \
    -Wl,--subsystem,console:6.0 tools/ie7_mshtml_navigation_test.c \
    -o build/ie7-mshtml-navigation-test.exe -lole32 -loleaut32 -luuid -lurlmon -ladvapi32
python3 triton-kmd/viogpu/tools/check_vista_pe.py --kind exe --arch x86 \
    --allow-import ole32.dll --allow-import oleaut32.dll --allow-import urlmon.dll \
    build/ie7-mshtml-navigation-test.exe
echo 'MSHTML NAVIGATION CONTRACT TEST BUILD VERIFIED'
