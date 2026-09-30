#!/usr/bin/env bash
# Desktop shortcut entry point; all launch settings use run-vm.sh's VISTA_* variables.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
exec "$root/run-vm.sh" "$@"
