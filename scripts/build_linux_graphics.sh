#!/usr/bin/env bash
# Build the native Linux graphics backend and Neptune renderer in place.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"
prefix=${TRITON_HOST_PREFIX:-$root/host-linux}
dxvk_build=${TRITON_DXVK_BUILD:-$root/triton-dxvk/build-linux}
renderer_build=${TRITON_RENDERER_BUILD:-$root/triton-virglrenderer/build-linux}
# Keep the library layout stable across distributions and native test runners.
libdir=lib/x86_64-linux-gnu
[[ "$prefix" == /* && "$dxvk_build" == /* && "$renderer_build" == /* ]] || {
    echo "TRITON_* paths must be absolute" >&2; exit 2;
}
meson=${MESON:-meson}
jobs=${JOBS:-4}
setup() {
    local build=$1 source=$2
    shift 2
    local flags=()
    [[ ! -f "$build/meson-private/coredata.dat" ]] || flags+=(--reconfigure)
    "$meson" setup "${flags[@]}" "$build" "$source" --prefix="$prefix" --libdir="$libdir" "$@"
}
setup "$dxvk_build" triton-dxvk -Dbuildtype=release \
    -Denable_d3d8=false -Denable_d3d9=false -Denable_d3d10=false \
    -Dnative_headless=true -Dnative_sdl2=disabled -Dnative_sdl3=disabled \
    -Dnative_glfw=disabled
ninja -C "$dxvk_build" -j "$jobs" install
export PKG_CONFIG_PATH="$prefix/lib/x86_64-linux-gnu/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
setup "$renderer_build" triton-virglrenderer \
    -Dbuildtype=debugoptimized -Dneptune=true -Dtests=true -Dplatforms=egl
ninja -C "$renderer_build" -j "$jobs" install
"$meson" test -C "$renderer_build" test_neptune_init --print-errorlogs
