#!/usr/bin/env bash
# Produces an unsigned service; package signing remains a separate step.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"
out=test-artifacts/linux-build
mkdir -p "$out"
signing_args=()
if [[ -n ${VISTA_DEPLOY_CERT:-} ]]; then
    python3 scripts/write_vista_signing_header.py "$VISTA_DEPLOY_CERT" "$out/deploy-signing.h"
    signing_args+=("-DTRITON_DEPLOY_SIGNING_HEADER=\"$root/$out/deploy-signing.h\"")
fi
x86_64-w64-mingw32-windres -I test-artifacts \
    test-artifacts/vista-driver-deploy-service.rc "$out/vista-deploy-resource.o"
x86_64-w64-mingw32-gcc -std=c11 -Os -Wall -Wextra -Werror \
    "${signing_args[@]}" -static -static-libgcc -municode -Wl,--subsystem,console:6.0 \
    -Wl,--disable-high-entropy-va test-artifacts/vista-driver-deploy-service.c \
    "$out/vista-deploy-resource.o" -o "$out/triton-vista-deploy.exe" \
    -ladvapi32 -lcrypt32 -lwintrust
python3 triton-kmd/viogpu/tools/check_vista_pe.py --kind exe --arch x64 \
    --allow-import crypt32.dll --allow-import wintrust.dll "$out/triton-vista-deploy.exe"
imports=$(x86_64-w64-mingw32-objdump -p "$out/triton-vista-deploy.exe")
case "${imports,,}" in
    *user32.dll*|*cryptcatadmincalchashfromfilehandle*)
        echo 'Service imports an API forbidden during recovery' >&2
        exit 1;;
esac
