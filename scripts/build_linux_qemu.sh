#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
export PKG_CONFIG_PATH="$root/host-linux/lib/x86_64-linux-gnu/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
mkdir -p "$root/triton-qemu/build-linux"
cd "$root/triton-qemu/build-linux"
../configure --target-list=x86_64-softmmu --prefix="$root/host-linux" \
    --enable-kvm --enable-opengl --enable-virglrenderer --enable-gtk \
    --enable-slirp --disable-rust --disable-docs --disable-download --disable-werror
ninja -j "${JOBS:-4}" qemu-system-x86_64
