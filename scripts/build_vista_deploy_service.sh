#!/bin/zsh
set -euo pipefail

script_dir=${0:A:h}
workspace=${script_dir:h}
source_file="$workspace/test-artifacts/vista-driver-deploy-service.c"
resource_file="$workspace/test-artifacts/vista-driver-deploy-service.rc"
output_dir="$workspace/test-artifacts/vista-deploy-service"
output_file="$output_dir/triton-vista-deploy.exe"
resource_object="$output_dir/vista-driver-deploy-service-resource.o"
crt_root="$workspace/triton-umd/.cache/vista-crt/x64/mingw64"
compiler=${CC_VISTA_DEPLOY:-x86_64-w64-mingw32-gcc}

if [[ ! -f "$crt_root/lib/libmsvcrt.a" || ! -f "$crt_root/include/windows.h" ]]; then
    "$workspace/triton-umd/build-support/bootstrap-vista-crt.sh"
fi

mkdir -p "$output_dir"
x86_64-w64-mingw32-windres \
    -I "$workspace/test-artifacts" \
    "$resource_file" "$resource_object"
"$compiler" \
    -std=c11 -Os -Wall -Wextra -Werror \
    -mcrtdll=msvcrt \
    -isystem "$crt_root/include" \
    -B"$crt_root/lib" -L"$crt_root/lib" \
    -static -static-libgcc -municode \
    -Wl,--subsystem,console:6.0 -Wl,--disable-high-entropy-va \
    "$source_file" "$resource_object" -o "$output_file" \
    -ladvapi32 -lcrypt32 -lwintrust

python3 "$workspace/triton-kmd/viogpu/tools/check_vista_pe.py" \
    --kind exe --arch x64 \
    --allow-import crypt32.dll --allow-import wintrust.dll \
    "$output_file"

imports=$(llvm-objdump --private-headers "$output_file" | \
    sed -n 's/.*DLL Name: //p' | tr '[:upper:]' '[:lower:]')
if print -r -- "$imports" | rg -q 'api-ms-win|ucrtbase|vcruntime'; then
    print -u2 -- "Vista deploy service imports a post-Vista runtime"
    print -u2 -- "$imports"
    exit 1
fi
if print -r -- "$imports" | rg -qi 'user32\.dll'; then
    print -u2 -- "Vista deploy service enters USER32 before driver recovery"
    print -u2 -- "$imports"
    exit 1
fi
if llvm-objdump --private-headers "$output_file" | \
   rg -q 'CryptCATAdminCalcHashFromFileHandle'; then
    print -u2 -- "Vista deploy service statically imports the optional catalog hash API"
    exit 1
fi

print -r -- "$output_file"
