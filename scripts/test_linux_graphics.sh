#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"
out="$root/build"
src="$root/triton-umd/src/virtio/neptune/vista-d3d9"
prefix="$root/host-linux"
label=${TRITON_TEST_LABEL:-native}
[[ "$label" =~ ^[a-zA-Z0-9_-]+$ ]] || { echo 'Invalid test label' >&2; exit 1; }
mkdir -p "$out"
export LD_LIBRARY_PATH="$prefix/lib/x86_64-linux-gnu"
export DXVK_WSI_DRIVER=Headless
${MESON:-meson} test -C triton-virglrenderer/build-linux test_neptune_init --print-errorlogs
for test in cpu-layout clear-contract draw-contract shader-token-contract; do
    files=()
    case "$test" in
        cpu-layout) files+=("$src/triton9_cpu_layout.c");;
        draw-contract) files+=("$src/triton9_draw_contract.c");;
    esac
    files+=("$src/tests/triton9_${test//-/_}_test.c")
    "${CC:-gcc}" -std=c11 -O1 -g -fsanitize=address,undefined \
        -fno-omit-frame-pointer "${files[@]}" -lm -o "$out/$test"
    "$out/$test"
done
"${CXX:-g++}" -std=c++17 -O2 -I"$prefix/include/dxvk" \
    tests/linux-d3d11-test.cpp -L"$prefix/lib/x86_64-linux-gnu" \
    -ldxvk_d3d11 -ldxvk_dxgi -o "$out/d3d11-test"
"$out/d3d11-test" "$out/d3d11-$label-results.txt"
