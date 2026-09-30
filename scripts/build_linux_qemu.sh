#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
prefix=${TRITON_HOST_PREFIX:-$root/host-linux}
build=${TRITON_QEMU_BUILD:-$root/triton-qemu/build-linux}
[[ "$prefix" == /* && "$build" == /* ]] || {
    echo "TRITON_* paths must be absolute" >&2; exit 2;
}
export PKG_CONFIG_PATH="$prefix/lib/x86_64-linux-gnu/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
mkdir -p "$build"
cd "$build"
"$root/triton-qemu/configure" --target-list=x86_64-softmmu --prefix="$prefix" \
    --enable-kvm --enable-opengl --enable-virglrenderer --enable-gtk \
    --enable-slirp --enable-pipewire --disable-rust --disable-docs --disable-download --disable-werror
ninja -j "${JOBS:-4}" qemu-system-x86_64
