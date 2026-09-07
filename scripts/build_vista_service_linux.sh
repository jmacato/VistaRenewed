#!/usr/bin/env bash
# Produces an unsigned service; package signing remains a separate step.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"
arch=x64
if [[ ${1:-} == --arch ]]; then
    arch=${2:-}
    shift 2
fi
if [[ $# -ne 0 || ( $arch != x64 && $arch != x86 ) ]]; then
    echo "usage: $0 [--arch x64|x86]" >&2
    exit 2
fi
if [[ $arch == x64 ]]; then
    tool_prefix=x86_64-w64-mingw32
    out=build
    linker_args=(-Wl,--disable-high-entropy-va)
else
    tool_prefix=i686-w64-mingw32
    out=build/service-x86
    linker_args=()
fi
mkdir -p "$out"
signing_args=()
if [[ -n ${VISTA_DEPLOY_CERT:-} ]]; then
    python3 scripts/write_vista_signing_header.py "$VISTA_DEPLOY_CERT" "$out/deploy-signing.h"
    signing_args+=("-DTRITON_DEPLOY_SIGNING_HEADER=\"$root/$out/deploy-signing.h\"")
fi
compiler_args=(-std=c11 -Os -Wall -Wextra -Werror)
if [[ ${#signing_args[@]} -ne 0 ]]; then
    compiler_args+=("${signing_args[@]}")
fi
crt_args=()
if [[ $(uname -s) == Darwin ]]; then
    if [[ $arch == x64 ]]; then
        crt_dir="$root/triton-umd/.cache/vista-crt/x64/mingw64"
    else
        crt_dir="$root/triton-umd/.cache/vista-crt/x86/mingw32"
    fi
    if [[ ! -d $crt_dir/include || ! -d $crt_dir/lib ]]; then
        echo "missing Vista msvcrt toolchain at $crt_dir" >&2
        exit 1
    fi
    crt_args=(-mcrtdll=msvcrt
        -include "$root/triton-umd/src/virtio/neptune/vista-d3d9/triton9_crt_compat.h"
        -isystem "$crt_dir/include" -B"$crt_dir/lib" -L"$crt_dir/lib")
fi
"${tool_prefix}-windres" -I packaging \
    packaging/vista-driver-deploy-service.rc "$out/vista-deploy-resource.o"
"${tool_prefix}-gcc" "${compiler_args[@]}" "${crt_args[@]+${crt_args[@]}}" -static -static-libgcc -municode -Wl,--subsystem,console:6.0 \
    "${linker_args[@]+${linker_args[@]}}" packaging/vista-driver-deploy-service.c \
    "$out/vista-deploy-resource.o" -o "$out/triton-vista-deploy.exe" \
    -ladvapi32 -lcrypt32 -lwintrust
python3 triton-kmd/viogpu/tools/check_vista_pe.py --kind exe --arch "$arch" \
    --allow-import crypt32.dll --allow-import wintrust.dll "$out/triton-vista-deploy.exe"
imports=$("${tool_prefix}-objdump" -p "$out/triton-vista-deploy.exe")
if printf '%s' "$imports" | tr '[:upper:]' '[:lower:]' | grep -Eq 'user32\.dll|cryptcatadmincalchashfromfilehandle'; then
        echo 'Service imports an API forbidden during recovery' >&2
        exit 1
fi
