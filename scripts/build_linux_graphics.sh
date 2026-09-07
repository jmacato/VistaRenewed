#!/usr/bin/env bash
# Build the native Linux graphics backend and Neptune renderer in place.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"
prefix="$root/host-linux"
meson=${MESON:-meson}
jobs=${JOBS:-4}
setup() {
    local build=$1 source=$2
    shift 2
    local flags=()
    [[ ! -f "$build/meson-private/coredata.dat" ]] || flags+=(--reconfigure)
    "$meson" setup "${flags[@]}" "$build" "$source" --prefix="$prefix" "$@"
}
setup triton-dxvk/build-linux triton-dxvk -Dbuildtype=release \
    -Denable_d3d8=false -Denable_d3d9=false -Denable_d3d10=false \
    -Dnative_headless=true -Dnative_sdl2=disabled -Dnative_sdl3=disabled \
    -Dnative_glfw=disabled
ninja -C triton-dxvk/build-linux -j "$jobs" install
export PKG_CONFIG_PATH="$prefix/lib/x86_64-linux-gnu/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
setup triton-virglrenderer/build-linux triton-virglrenderer \
    -Dbuildtype=debugoptimized -Dneptune=true -Dtests=true -Dplatforms=egl
ninja -C triton-virglrenderer/build-linux -j "$jobs" install
"$meson" test -C triton-virglrenderer/build-linux test_neptune_init --print-errorlogs
