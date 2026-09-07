#!/bin/sh
# Copyright 2026 Turing Software LLC
# SPDX-License-Identifier: MIT

# Validate the loader-facing ABI of one Vista neptune_d3d9.dll artifact.

set -eu

if [ "$#" -ne 2 ]; then
   echo "usage: $0 <x86|x64> <neptune_d3d9.dll>" >&2
   exit 2
fi

architecture=$1
artifact=$2

case "$architecture" in
   x86)
      objdump=i686-w64-mingw32-objdump
      expected_machine='PE32 executable .*Intel 80386'
      ;;
   x64)
      objdump=x86_64-w64-mingw32-objdump
      expected_machine='PE32\+ executable .*x86-64'
      ;;
   *)
      echo "unsupported architecture: $architecture" >&2
      exit 2
      ;;
esac

test -f "$artifact"
command -v "$objdump" >/dev/null

file_output=$(file "$artifact")
printf '%s\n' "$file_output" | grep -Eq "$expected_machine"

pe_output=$($objdump -p "$artifact")
printf '%s\n' "$pe_output" | grep -Eq '^MajorSubsystemVersion[[:space:]]+6$'
printf '%s\n' "$pe_output" | grep -Eq '^MinorSubsystemVersion[[:space:]]+0$'
printf '%s\n' "$pe_output" | grep -Eq '^Subsystem[[:space:]]+.*\(Windows GUI\)$'

imports=$(printf '%s\n' "$pe_output" |
   awk '/DLL Name:/{print tolower($3)}' | LC_ALL=C sort -u)
expected_imports='gdi32.dll
kernel32.dll
msvcrt.dll
user32.dll'
test "$imports" = "$expected_imports"

exports=$(printf '%s\n' "$pe_output" |
   sed -n '/The Export Tables/,/PE File Base Relocations/p' |
   awk '/^[[:space:]]*\[[[:space:]]*[0-9]+\][[:space:]]+\+base\[/ && $NF != "RVA" {print $NF}')
test "$exports" = OpenAdapter

echo "Vista D3D9 PE audit passed: $architecture $artifact"
