#!/usr/bin/env bash
# Run in the Linux builder image, with a clean checkout at /workspace.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"
python3 scripts/fetch_vista_sdk_headers.py
python3 scripts/extract_archived_wdk71.py --download
bash scripts/build_vista_umd_linux.sh
python3 scripts/build_vista_kmd_linux.py --arch x64
python3 scripts/build_vista_kmd_linux.py --arch x86
python3 scripts/package_vista_ci_iso.py
