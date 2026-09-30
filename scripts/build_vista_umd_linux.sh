#!/usr/bin/env bash
# Run inside the Linux driver build container with this workspace mounted.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"
meson=${MESON:-meson}
jobs=${JOBS:-4}
arches=(x64 x86)
if [[ $# -gt 0 ]]; then
    if [[ $# -ne 2 || $1 != --arch || ( $2 != x64 && $2 != x86 ) ]]; then
        echo "Usage: $0 [--arch x64|x86]" >&2
        exit 2
    fi
    arches=("$2")
fi
if [[ $(uname -s) == Darwin ]]; then
    sh triton-umd/build-support/bootstrap-vista-crt.sh
fi
for arch in "${arches[@]}"; do
    build="triton-umd/build-vista-linux-$arch"
    profile="triton-umd/build-support/vista-linux-$arch.ini"
    if [[ $(uname -s) == Darwin ]]; then
        profile="triton-umd/build-support/vista-$arch.ini"
    fi
    setup=(setup)
    [[ ! -f "$build/meson-private/coredata.dat" ]] || setup+=(--reconfigure)
    "$meson" "${setup[@]}" "$build" triton-umd \
        --cross-file "$profile" \
        --buildtype debugoptimized -Dplatforms=windows -Dneptune=true \
        -Dnpt_wine=false -Dnpt_umd=off -Dnpt_vista_d3d9=true -Dnpt_vista_d3d10=true \
        -Dmin-windows-version=6 -Dgallium-drivers= -Dvulkan-drivers= \
        -Dopengl=false -Degl=disabled -Dglx=disabled -Dllvm=disabled \
        -Dbuild-tests=false
    target=src/virtio/neptune/vista-d3d9
    ninja -C "$build" -j "$jobs" "$target/neptune_d3d9.dll" \
        "$target/triton9_runtime_probe.exe" "$target/libtriton9_abi_compile.a" \
        src/virtio/neptune/vista-d3d10/neptune_d3d10.dll \
        src/virtio/neptune/vista-d3d10/triton10_runtime_probe.exe \
        src/virtio/neptune/vista-d3d10/triton10_present_probe.exe
    python3 triton-kmd/viogpu/tools/check_vista_pe.py --kind umd --arch "$arch" \
        "$build/$target/neptune_d3d9.dll"
    python3 triton-kmd/viogpu/tools/check_vista_pe.py --kind umd10 --arch "$arch" \
        "$build/src/virtio/neptune/vista-d3d10/neptune_d3d10.dll"
    python3 triton-kmd/viogpu/tools/check_vista_pe.py --kind exe --arch "$arch" \
        --allow-import gdi32.dll "$build/src/virtio/neptune/vista-d3d10/triton10_runtime_probe.exe"
    python3 triton-kmd/viogpu/tools/check_vista_pe.py --kind exe --arch "$arch" \
        --allow-import gdi32.dll "$build/src/virtio/neptune/vista-d3d10/triton10_present_probe.exe"
    # The public probe uses GDI to draw its controlled evidence scene.
    python3 triton-kmd/viogpu/tools/check_vista_pe.py --kind exe --arch "$arch" \
        --allow-import gdi32.dll "$build/$target/triton9_runtime_probe.exe"
done
