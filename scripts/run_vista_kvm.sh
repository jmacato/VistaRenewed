#!/usr/bin/env bash
# Baseline Vista boot on Linux. Driver deployment needs the Triton host backend.
set -euo pipefail
workspace=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cpus=${VISTA_CPUS:-4}
[[ "$cpus" =~ ^[1-9][0-9]*$ ]] && (( cpus <= 16 )) || { echo "VISTA_CPUS must be 1..16" >&2; exit 1; }
base=${VISTA_BASE:-$workspace/winvista-3.qcow2}
work=${VISTA_WORK:-$workspace/vista-kvm/work.qcow2}
base=$(realpath -- "$base")
mkdir -p -- "$(dirname -- "$work")"
work=$(realpath -m -- "$work")
[[ "$base" != "$work" ]] || { echo 'Base and working disk must differ.' >&2; exit 1; }
[[ -r /dev/kvm && -w /dev/kvm ]] || { echo '/dev/kvm is not accessible.' >&2; exit 1; }
if [[ ! -e "$work" ]]; then
    qemu-img create -f qcow2 -F qcow2 -b "$base" "$work"
fi
python3 - "$base" "$work" <<'PY'
import json, pathlib, subprocess, sys
base, work = map(pathlib.Path, sys.argv[1:])
info = json.loads(subprocess.check_output(['qemu-img', 'info', '--output=json', str(work)]))
backing = info.get('full-backing-filename', info.get('backing-filename', ''))
if not backing or pathlib.Path(backing).resolve() != base.resolve():
    raise SystemExit('Working disk does not directly back onto the selected base.')
PY
mkdir -p "$workspace/vista-kvm/runs"
run=$(mktemp -d "$workspace/vista-kvm/runs/boot-XXXXXXXX")
args=(qemu-system-x86_64 -name vista-kvm-baseline
    -machine pc -accel kvm -cpu core2duo,monitor=off -smp "cpus=$cpus,sockets=1,cores=$cpus,threads=1" -m 2048
    -drive "file=$work,format=qcow2,if=ide" -boot c
    -vga std -nic none -display none -vnc "unix:$run/vnc.sock"
    -qmp "unix:$run/qmp.sock,server=on,wait=off"
    -serial "file:$run/com1.log" -serial "file:$run/status.log"
    -pidfile "$run/qemu.pid" -D "$run/qemu-debug.log" -daemonize)
printf '%s\n' "$base" > "$run/base-path"
printf '%s\n' "$work" > "$run/work-path"
printf '%q ' "${args[@]}" > "$run/command.sh"
printf '\n' >> "$run/command.sh"
"${args[@]}" 2> "$run/qemu.log"
ln -sfn "$run" "$workspace/vista-kvm/latest"
printf 'Vista started. Run directory: %s\n' "$run"
