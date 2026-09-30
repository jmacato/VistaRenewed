#!/bin/bash
set -euo pipefail
i686-w64-mingw32-gcc -std=c11 -O2 -Wall -Wextra -Werror -static \
    -Wl,--subsystem,console:6.0 tools/ie7_mshtml_element_test.c \
    -o build/ie7-mshtml-element-test.exe -lole32 -loleaut32 -luuid
python3 triton-kmd/viogpu/tools/check_vista_pe.py --kind exe --arch x86 \
    --allow-import ole32.dll --allow-import oleaut32.dll build/ie7-mshtml-element-test.exe
