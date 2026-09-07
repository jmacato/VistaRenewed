#!/bin/sh
# Copyright 2026 Turing Software LLC
# SPDX-License-Identifier: MIT

# Fetch the MinGW-w64 MSVCRT runtime, headers and Winpthreads support used by
# the Vista cross profiles.  Homebrew's MinGW package uses UCRT forwarding
# import libraries even when GCC is passed -mcrtdll=msvcrt; Vista cannot load
# those api-ms-win-crt-* contracts.  These pinned MSYS2 packages contain the
# legacy msvcrt.dll import libraries for both target architectures.

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
umd_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
cache_dir=${VISTA_CRT_CACHE:-"$umd_dir/.cache/vista-crt"}
tmp_dir=$(mktemp -d "${TMPDIR:-/tmp}/vista-crt.XXXXXX")

cleanup() {
   rm -rf "$tmp_dir"
}

trap cleanup EXIT HUP INT TERM

fetch_package() {
   architecture=$1
   directory=$2
   package=$3
   expected_hash=$4
   archive="$tmp_dir/$package"

   curl -L --fail --retry 3 --silent --show-error \
      -o "$archive" "https://repo.msys2.org/mingw/$directory/$package"
   printf '%s  %s\n' "$expected_hash" "$archive" | shasum -a 256 -c -
   tar --zstd -xf "$archive" -C "$cache_dir/$architecture"
}

prepare_architecture() {
   architecture=$1
   directory=$2
   prefix=$3
   crt_hash=$4
   headers_hash=$5
   pthread_hash=$6
   library_dir="$cache_dir/$architecture/$prefix/lib"
   include_dir="$cache_dir/$architecture/$prefix/include"

   if [ -f "$library_dir/libmsvcrt.a" ] && \
      [ -f "$library_dir/libwinpthread.a" ] && \
      [ -f "$include_dir/windows.h" ]; then
      return
   fi

   mkdir -p "$cache_dir/$architecture"
   fetch_package "$architecture" "$directory" \
      "mingw-w64-${architecture_package}-crt-git-13.0.0.r91.gfc0b67305-1-any.pkg.tar.zst" \
      "$crt_hash"
   fetch_package "$architecture" "$directory" \
      "mingw-w64-${architecture_package}-headers-git-13.0.0.r91.gfc0b67305-1-any.pkg.tar.zst" \
      "$headers_hash"
   fetch_package "$architecture" "$directory" \
      "mingw-w64-${architecture_package}-winpthreads-git-12.0.0.r747.g1a99f8514-1-any.pkg.tar.zst" \
      "$pthread_hash"
}

architecture_package=x86_64
prepare_architecture x64 mingw64 mingw64 \
   c613a34e44abf31cbd60ec81c305f1236a80f8f6d2e0fa62b4ba676f0b98c3d5 \
   d1b3d8d86d1ddd2db0e205ad248ee1414ab21ee78f4b740c681fb9c96320611c \
   b2e4aace96027186b3a64e748858ef2664ca7881740f38c1b4b83ca9d8311fc8

architecture_package=i686
prepare_architecture x86 mingw32 mingw32 \
   512da09c746632c3030fe80cad2f3bc041728f3f595af70c830956ab3c1db3fc \
   f1f9e2ea345c49db8020e40886ca6f5d1a3dac49a2cda8654592be800b86a4ce \
   3efeeebb3c4295d80e628ac4db4b9457660e6892773b9b2aa3c98402a479e295
