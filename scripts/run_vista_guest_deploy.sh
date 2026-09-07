#!/bin/zsh
set -euo pipefail

# This launcher only defines hardware. After QEMU starts, TritonVistaDeploy in
# the guest verifies the package, replaces files, handles recovery, and
# restarts Vista. This script never sends VM input or monitor commands.

script_dir=${0:A:h}
workspace=${script_dir:h}
transfer_root="$workspace/aaaaa/vista-signing-transfer"
media_bundle=${VISTA_DEPLOY_BUNDLE:-"$transfer_root/vista-deploy-current"}
vista_disk=${VISTA_DEPLOY_DISK:-"$workspace/winvista-3.shrunk.qcow2"}
qemu_binary=${VISTA_DEPLOY_QEMU:-"$workspace/triton-qemu/build/qemu-system-x86_64"}
npt_backend_library="$workspace/host-triton/lib/libdxmt-native.dylib"
npt_render_server="$workspace/host-triton/libexec/virgl_render_server"
run_parent="$transfer_root/vista-deploy-runs"
requested_run_dir=${VISTA_DEPLOY_RUN_DIR:-}
# VioGpu's checked build can emit tens of megabytes per minute.  COM2 carries
# the deployment and public-probe checkpoints; retain KMD debug only when a
# caller explicitly provides a diagnostic path.
debugcon_log=${VISTA_DEPLOY_DEBUGCON_LOG:-/dev/null}

[[ -x "$qemu_binary" ]] || { print -u2 -- "missing QEMU: $qemu_binary"; exit 1; }
qemu_binary_canonical=$(realpath "$qemu_binary")
[[ -x "$qemu_binary_canonical" ]] || {
    print -u2 -- "QEMU does not resolve to an executable: $qemu_binary"
    exit 1
}
[[ -s "$vista_disk" ]] || { print -u2 -- "missing Vista disk: $vista_disk"; exit 1; }
[[ -h "$media_bundle" ]] || {
    print -u2 -- "deployment bundle is not an atomic publication pointer: $media_bundle"
    exit 1
}
bundle_canonical=$(realpath "$media_bundle")
publication_canonical=$(realpath "$transfer_root/vista-deploy-publications")
[[ "${bundle_canonical:h}" == "$publication_canonical" &&
   -f "$bundle_canonical/deployment-id" &&
   -f "$bundle_canonical/media.iso.sha256" ]] || {
    print -u2 -- "deployment bundle does not resolve to one immutable publication"
    exit 1
}
media_root="$bundle_canonical/tree"
media_iso="$bundle_canonical/media.iso"
[[ -s "$media_root/triton-deploy.ini" ]] || {
    print -u2 -- "missing deployment media; run scripts/stage_vista_deploy_media.sh"
    exit 1
}
[[ -s "$media_iso" ]] || {
    print -u2 -- "missing deployment ISO; run scripts/stage_vista_deploy_media.sh"
    exit 1
}
deployment_id=$(<"$bundle_canonical/deployment-id")
[[ -n "$deployment_id" && "$deployment_id" != *[^0-9a-f]* &&
   ${#deployment_id} -eq 64 &&
   "${bundle_canonical:t}" == "$deployment_id" ]] || {
    print -u2 -- "deployment bundle identity is invalid"
    exit 1
}
ini_id=$(sed -n 's/^id=//p' "$media_root/triton-deploy.ini")
[[ "$ini_id" == "$deployment_id" ]] || {
    print -u2 -- "deployment directory identity does not match its bundle"
    exit 1
}
(cd "$bundle_canonical" && shasum -a 256 -c media.iso.sha256)
[[ -s "$npt_backend_library" ]] || { print -u2 -- "missing Neptune backend"; exit 1; }
[[ -x "$npt_render_server" ]] || { print -u2 -- "missing Neptune render server"; exit 1; }

umask 077
mkdir -p "$run_parent"
chmod 700 "$run_parent"
run_parent_canonical=$(realpath "$run_parent")
run_token=$(openssl rand -hex 32)
if [[ -n "$requested_run_dir" ]]; then
    [[ "$requested_run_dir" == /* &&
       "$(realpath "${requested_run_dir:h}")" == "$run_parent_canonical" &&
       -n "${requested_run_dir:t}" &&
       "${requested_run_dir:t}" != *[^A-Za-z0-9._-]* ]] || {
        print -u2 -- "VISTA_DEPLOY_RUN_DIR must be a safe new child of $run_parent"
        exit 1
    }
    run_dir="$requested_run_dir"
else
    # Darwin limits a Unix-domain socket path to 103 bytes plus its terminator.
    # Keep the default leaf short; the full random identity remains in run-token.
    run_dir="$run_parent/r-${run_token[1,12]}"
fi
[[ ! -e "$run_dir" && ! -h "$run_dir" ]] || {
    print -u2 -- "deployment run directory already exists: $run_dir"
    exit 1
}
status_log="$run_dir/status.log"
qmp_socket="$run_dir/qmp.sock"
qmp_socket_bytes=$(LC_ALL=C printf %s "$qmp_socket" | wc -c | tr -d '[:space:]')
[[ "$qmp_socket_bytes" == <-> && "$qmp_socket_bytes" -lt 104 ]] || {
    print -u2 -- "QMP socket path must contain fewer than 104 bytes: $qmp_socket"
    exit 1
}
mkdir -m 700 "$run_dir"
run_dir_canonical=$(realpath "$run_dir")
serial1_log=${VISTA_DEPLOY_COM1_LOG:-/dev/null}
for diagnostic_log in "$serial1_log" "$debugcon_log"; do
    if [[ "$diagnostic_log" != /dev/null ]]; then
        diagnostic_parent=$(realpath "${diagnostic_log:h}" 2>/dev/null || true)
        diagnostic_leaf=${diagnostic_log:t}
        [[ "$diagnostic_log" == /* &&
           "$diagnostic_parent" == "$run_dir_canonical" &&
           -n "$diagnostic_leaf" &&
           "$diagnostic_leaf" != *[^A-Za-z0-9._-]* &&
           "$diagnostic_leaf" != . && "$diagnostic_leaf" != .. &&
           ! -e "$diagnostic_log" && ! -h "$diagnostic_log" ]] || {
            print -u2 -- "diagnostic output must be a new file in $run_dir"
            exit 1
        }
    fi
done
print -r -- "$run_token" > "$run_dir/run-token"
print -r -- "$deployment_id" > "$run_dir/deployment-id"
print -r -- "$$" > "$run_dir/qemu-pid"
print -r -- "$qemu_binary_canonical" > "$run_dir/qemu-path"
if [[ "$serial1_log" != /dev/null ]]; then
    [[ ! -e "$serial1_log" && ! -h "$serial1_log" ]] || {
        print -u2 -- "COM1 output log already exists; refusing to overwrite it: $serial1_log"
        exit 1
    }
    mkdir -p "${serial1_log:h}"
fi
if lsof -t -- "$vista_disk" >/dev/null 2>&1; then
    print -u2 -- "the Vista qcow2 is already open: $vista_disk"
    lsof -- "$vista_disk" >&2
    exit 1
fi
available_kib=$(df -Pk "$vista_disk" | awk 'NR == 2 { print $4 }')
[[ "$available_kib" == <-> ]] || {
    print -u2 -- "cannot determine free space for the Vista qcow2"
    exit 1
}
(( available_kib >= 8 * 1024 * 1024 )) || {
    print -u2 -- "less than 8 GiB is free; refusing to grow the existing qcow2"
    exit 1
}

export NPT_BACKEND=dxmt
export NPT_D3D11_LIBRARY_PATH="$npt_backend_library"
export NPT_DXGI_LIBRARY_PATH="$npt_backend_library"
export RENDER_SERVER_EXEC_PATH="$npt_render_server"

# The deployment source is an immutable ISO9660/Joliet optical image.  Vista's
# inbox CD stack mounts it in both normal and Safe Mode; the guest service owns
# package verification, installation, and all reboots.
print -r -- "deployment run directory: $run_dir"
print -r -- "deployment run token: $run_token"
exec "$qemu_binary_canonical" \
    -name "vista-aero-${run_token[1,16]}" \
    -accel tcg \
    -machine q35 \
    -m 768M \
    -smp 1 \
    -drive "file=$vista_disk,format=qcow2,if=ide" \
    -drive "file=$media_iso,format=raw,if=none,id=triton_deploy_media,readonly=on" \
    -device 'ide-cd,drive=triton_deploy_media' \
    -vga none \
    -device 'virtio-vga-gl,vgamem_mb=128,hostmem=1G,max_hostmem=1G,blob=true,neptune=true' \
    -nic 'user,model=rtl8139' \
    -device 'qemu-xhci,id=vista_xhci' \
    -device 'usb-tablet,id=vista_tablet' \
    -display 'cocoa,gl=on' \
    -qmp "unix:$qmp_socket,server=on,wait=off" \
    -serial "file:$serial1_log" \
    -serial "file:$status_log" \
    -debugcon "file:$debugcon_log" \
    -global 'isa-debugcon.iobase=0xe9'
