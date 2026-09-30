#!/usr/bin/env bash
# Run inside the VM container, from the checkout root.
set -euo pipefail
if [[ ${TRITON_SIDEBAR_RELAY:-1} == 1 ]]; then
    python3 packaging/vista-sidebar-gadgets/vista_sidebar_proxy.py --host 127.0.0.1 --port 8765 &
    relay=$!
    for attempt in {1..50}; do
        if python3 -c 'from urllib.request import urlopen; assert urlopen("http://127.0.0.1:8765/health", timeout=1).status == 200' 2>/dev/null; then
            break
        fi
        kill -0 "$relay" || exit 1
        [[ $attempt != 50 ]] || exit 1
        sleep 0.1
    done
fi
exec "$@"
