#!/usr/bin/env bash
# Run inside the Linux driver build container with this workspace mounted.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"
meson=${MESON:-meson}
jobs=${JOBS:-4}
for arch in x64 x86; do
    build="triton-umd/build-vista-linux-$arch"
    setup=()
    [[ ! -f "$build/meson-private/coredata.dat" ]] || setup+=(--reconfigure)
    "$meson" setup "${setup[@]}" "$build" triton-umd \
        --cross-file "triton-umd/build-support/vista-linux-$arch.ini" \
        --buildtype debugoptimized -Dplatforms=windows -Dneptune=true \
        -Dnpt_wine=false -Dnpt_umd=off -Dnpt_vista_d3d9=true \
        -Dmin-windows-version=6 -Dgallium-drivers= -Dvulkan-drivers= \
        -Dopengl=false -Degl=disabled -Dglx=disabled -Dllvm=disabled \
        -Dbuild-tests=false
    target=src/virtio/neptune/vista-d3d9
    ninja -C "$build" -j "$jobs" "$target/neptune_d3d9.dll" \
        "$target/triton9_runtime_probe.exe" "$target/libtriton9_abi_compile.a"
    python3 triton-kmd/viogpu/tools/check_vista_pe.py --kind umd --arch "$arch" \
        "$build/$target/neptune_d3d9.dll"
    # The public probe uses GDI to draw its controlled evidence scene.
    python3 triton-kmd/viogpu/tools/check_vista_pe.py --kind exe --arch "$arch" \
        --allow-import gdi32.dll "$build/$target/triton9_runtime_probe.exe"
done
