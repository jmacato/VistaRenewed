#!/usr/bin/env bash
# Start the existing Vista x64 guest with the local Neptune graphics build.
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
vm="$root/vista-kvm/x64-base"
name=${VISTA_VM_NAME:-triton-vista-x64-normal}
image=${VISTA_BUILD_IMAGE:-localhost/triton-vista-builder:preview}
ram=${VISTA_RAM_MB:-4096}
cpus=${VISTA_CPUS:-4}
render_node=${VISTA_RENDER_NODE:-/dev/dri/renderD128}
gpu_filter=${VISTA_GPU_FILTER-}
nvidia_device=${VISTA_NVIDIA_DEVICE:-}
display_stats=${VISTA_DISPLAY_STATS:-0}
host_prefix=${VISTA_HOST_PREFIX:-$root/host-linux}
qemu_build=${VISTA_QEMU_BUILD_DIR:-$root/triton-qemu/build-linux}
iso=${VISTA_ISO:-$root/dist/triton-vista-x64.iso}
disk=${VISTA_DISK:-$vm/work.qcow2}
if [[ $name != triton-vista-x64-normal ]]; then
    vm="$vm/instances/$name"
fi
vm=${VISTA_STATE_DIR:-$vm}

if [[ ${1:-} == --help || ${1:-} == -h ]]; then
    cat <<'EOF'
Usage: ./run-vm.sh

Launch Vista in a QEMU window and leave it running in the background.
Running this again leaves an already running VM alone.
Includes USB Audio through native PipeWire and Intel PRO/1000 networking
with DHCP and outbound NAT.
Supply your own installed Vista qcow2 disk with VISTA_DISK.
Audio uses PipeWire when available; VISTA_AUDIO=none disables it.
Requires local qemu-img to inspect the overlay and its backing files.

Optional environment variables:
  VISTA_RAM_MB   Guest RAM in MiB (default: 4096)
  VISTA_CPUS     Guest CPU count (default: 4)
  VISTA_ISO      Driver CD image (default: dist/triton-vista-x64.iso)
  VISTA_DISK     Writable guest overlay (default: vista-kvm/x64-base/work.qcow2)
  VISTA_VM_NAME  Container/window name (default: triton-vista-x64-normal)
  VISTA_STATE_DIR Sockets, logs and cache (additional names default to instances/NAME)
  VISTA_RENDER_NODE Host DRM render node (default: /dev/dri/renderD128)
  VISTA_GPU_FILTER DXVK device-name filter (default: no filter)
  VISTA_NVIDIA_DEVICE Optional NVIDIA CDI device, e.g. nvidia.com/gpu=0
  VISTA_BUILD_IMAGE Builder/runtime image (default: localhost/triton-vista-builder:preview)
  VISTA_AUDIO    auto (default), pipewire (required), or none
  VISTA_DISPLAY_STATS 0 (default) or 1 to log QEMU draw diagnostics
  VISTA_HOST_PREFIX Host install prefix (default: host-linux under this checkout)
  VISTA_QEMU_BUILD_DIR QEMU build directory (default: triton-qemu/build-linux)

Logs:     podman logs -f triton-vista-x64-normal
Shutdown: use Shut Down inside Windows before starting it again.
EOF
    exit 0
fi

die() { printf '%s\n' "$*" >&2; exit 1; }
[[ $# == 0 ]] || die 'Unexpected arguments. Use --help for usage.'
command -v podman >/dev/null || die 'Podman is required.'
command -v flock >/dev/null || die 'flock is required (util-linux).'
[[ $name =~ ^[a-zA-Z0-9][a-zA-Z0-9_.-]*$ ]] || die 'Invalid VISTA_VM_NAME.'
[[ $display_stats == 0 || $display_stats == 1 ]] || die 'VISTA_DISPLAY_STATS must be 0 or 1.'
[[ -r $disk && -f $disk ]] || die "VM disk is missing or unreadable: $disk"
disk=$(realpath -- "$disk")

# Serialize launches, including removal of a stopped container. Keep the SYSTEM
# control channel private even when QEMU recreates its socket.
umask 077
mkdir -p "$vm/control"
vm=$(realpath -- "$vm")
chmod 700 "$vm/control"
exec 9>"$vm/control/launch.lock"
flock -n 9 || die 'Another VM launch is in progress.'

exists=false
if podman container exists "$name"; then
    exists=true
fi

# A running VM owns its state directory until it exits. The launch lock alone
# only protects startup, and QEMU replaces Unix socket paths when it binds them.
running_ids_text=$(podman ps --quiet)
if [[ -n $running_ids_text ]]; then
    mapfile -t running_ids <<<"$running_ids_text"
    running_match=$(podman inspect "${running_ids[@]}" | python3 -c '
import json, sys
name, disk, state = sys.argv[1:]
expected_drive = "file=" + disk.replace(",", ",,") + ",format=qcow2,if=ide"
expected_qmp = "unix:" + state.replace(",", ",,") + "/qmp.sock,server=on,wait=off"
matched = False
for entry in json.load(sys.stdin):
    args = entry.get("Args", [])
    owner = entry.get("Name", "").lstrip("/")
    labels = entry.get("Config", {}).get("Labels") or {}
    same_state = (labels.get("io.triton.vista.state-dir") == state or
                  expected_qmp in args)
    if owner == name:
        if expected_drive not in args or expected_qmp not in args:
            sys.exit("Running container " + name + " uses a different disk or state directory.")
        matched = True
    elif same_state:
        sys.exit("State directory is already used by running container " + owner + ": " + state)
print("yes" if matched else "no")
' "$name" "$disk" "$vm")
    if [[ $running_match == yes ]]; then
        printf 'Vista desktop is already running: %s (%s).\n' "$name" "$disk"
        exit 0
    fi
fi

[[ $ram =~ ^[1-9][0-9]*$ ]] || die 'VISTA_RAM_MB must be a positive integer.'
[[ $cpus =~ ^[1-9][0-9]*$ ]] || die 'VISTA_CPUS must be a positive integer.'
[[ -n ${DISPLAY:-} ]] || die 'Run this script from your graphical desktop session (DISPLAY is unset).'
auth=${XAUTHORITY:-$HOME/.Xauthority}
[[ -r $auth && -f $auth ]] || die "X authority file is missing or unreadable: $auth. Set XAUTHORITY to the current desktop session's file."
[[ -r /dev/kvm && -w /dev/kvm ]] || die 'Read/write access to /dev/kvm is required.'
[[ -r $render_node && -w $render_node ]] || die "Read/write access to $render_node is required."
gpu_args=(--device "$render_node")
if [[ -n $nvidia_device ]]; then
    [[ $nvidia_device =~ ^nvidia\.com/gpu=[a-zA-Z0-9_-]+$ ]] || die 'Invalid VISTA_NVIDIA_DEVICE CDI name.'
    # CDI retains the host's library layout. Include both Fedora's and
    # Ubuntu's GBM directories so the container finds NVIDIA's backend.
    gpu_args+=(--device "$nvidia_device" -e GBM_BACKEND=nvidia-drm
               -e GBM_BACKENDS_PATH=/usr/lib64/gbm:/usr/lib/x86_64-linux-gnu/gbm
               -e VIRGL_GBM_LAYOUT_FORCE_ENABLE=1)
fi
pw_remote=${PIPEWIRE_REMOTE:-pipewire-0}
if [[ $pw_remote == /* ]]; then
    pw_socket=$pw_remote
else
    pw_socket=${PIPEWIRE_RUNTIME_DIR:-${XDG_RUNTIME_DIR:-/run/user/$(id -u)}}/$pw_remote
fi
audio=${VISTA_AUDIO:-auto}
case $audio in
    auto) [[ ! -S $pw_socket ]] || audio=pipewire ;;
    pipewire) [[ -S $pw_socket ]] || die "PipeWire socket is missing: $pw_socket" ;;
    none) ;;
    *) die 'VISTA_AUDIO must be auto, pipewire, or none.' ;;
esac
audio_mounts=()
audio_args=()
if [[ $audio == pipewire ]]; then
    audio_mounts=(-v "$pw_socket:/tmp/vista-pipewire:ro" -e PIPEWIRE_REMOTE=/tmp/vista-pipewire)
    audio_args=(-audiodev pipewire,id=vista-audio,out.stream-name=Vista,out.frequency=48000
        -device piix3-usb-uhci,id=vista-audio-usb-bus
        -device usb-audio,id=vista-audio-usb,bus=vista-audio-usb-bus.0,audiodev=vista-audio)
fi
[[ -r $disk && -f $disk ]] || die "VM disk is missing or unreadable: $disk"
[[ -r $iso && -f $iso ]] || die "Driver ISO is missing: $iso"
disk=$(realpath -- "$disk")
iso=$(realpath -- "$iso")
auth=$(realpath -- "$auth")
[[ -x $qemu_build/qemu-system-x86_64 ]] || die 'Build QEMU first; see docs/BUILDING.md.'
[[ -x $host_prefix/libexec/virgl_render_server ]] || die 'Build the Linux graphics backend first; see docs/BUILDING.md.'
podman image exists "$image" || die "Container image is missing: $image. See docs/BUILDING.md."

# Windows interprets the RTC as local time. Containers otherwise default to UTC,
# which shifts the guest clock and can reject freshly signed media as not valid
# yet. Carry the desktop host's timezone into QEMU unless TZ is explicit.
rtc_timezone=${TZ:-}
if [[ -z $rtc_timezone ]]; then
    localtime_path=$(readlink -f /etc/localtime 2>/dev/null || true)
    if [[ $localtime_path == */zoneinfo/* ]]; then
        rtc_timezone=${localtime_path#*/zoneinfo/}
    elif [[ -r /etc/timezone ]]; then
        rtc_timezone=$(cat /etc/timezone)
    elif command -v timedatectl >/dev/null; then
        rtc_timezone=$(timedatectl show -p Timezone --value 2>/dev/null || true)
    fi
fi
[[ -n $rtc_timezone ]] || die 'Cannot determine host timezone for the local-time RTC; set TZ explicitly.'

# Inspect before removing a stopped container or starting a new one. Mount
# each backing layer read-only, including layers outside the overlay's folder.
disk_mount_plan=$(python3 "$root/scripts/vista_disk_mounts.py" "$disk")
mapfile -t disk_mounts < <(python3 -c '
import json, sys
for mount in json.loads(sys.argv[1]):
    print(mount)
' "$disk_mount_plan")

mkdir -p "$vm/shader-cache/mesa"
host_prefix=$(realpath -- "$host_prefix")
qemu_build=$(realpath -- "$qemu_build")
vm_mounts=(-v "$root:$root")
for build_path in "$host_prefix" "$qemu_build"; do
    if [[ $build_path != "$root" && $build_path != "$root/"* ]]; then
        vm_mounts+=(-v "$build_path:$build_path:ro")
    fi
done
for mount in "${disk_mounts[@]}"; do
    vm_mounts+=(-v "$mount")
done
if [[ $vm != "$root" && $vm != "$root/"* ]]; then
    vm_mounts+=(-v "$vm:$vm")
fi
if [[ $exists == true ]]; then
    # Recreate the stopped container to refresh the desktop's Xauthority mount.
    # The VM disk and shader cache live outside the container.
    podman logs "$name" >"$vm/previous-container.log" 2>&1
    podman rm "$name" >/dev/null
fi

# Keep the launch lock in this shell; conmon outlives the launcher.
podman run -d --name "$name" 9>&- \
    --security-opt label=disable --umask 0077 \
    --label "io.triton.vista.state-dir=$vm" \
    --device /dev/kvm "${gpu_args[@]}" --group-add keep-groups \
    -v /tmp/.X11-unix:/tmp/.X11-unix:ro \
    -v "$auth:/tmp/vista-xauthority:ro" \
    "${audio_mounts[@]}" \
    "${vm_mounts[@]}" \
    -v "$iso:/tmp/vista-driver.iso:ro" -w "$root" \
    -e XAUTHORITY=/tmp/vista-xauthority -e "DISPLAY=$DISPLAY" -e GDK_BACKEND=x11 \
    -e "LD_LIBRARY_PATH=$host_prefix/lib/x86_64-linux-gnu" \
    -e "RENDER_SERVER_EXEC_PATH=$host_prefix/libexec/virgl_render_server" \
    -e DXVK_WSI_DRIVER=Headless -e "DXVK_FILTER_DEVICE_NAME=$gpu_filter" \
    -e "TZ=$rtc_timezone" \
    -e "DXVK_SHADER_CACHE_PATH=$vm/shader-cache" \
    -e "MESA_SHADER_CACHE_DIR=$vm/shader-cache/mesa" \
    -e "TRITON_DISPLAY_STATS=$display_stats" \
    "$image" "$qemu_build/qemu-system-x86_64" \
    -name "$name" -L "$root/triton-qemu/pc-bios" \
    -machine pc,accel=kvm,max-ram-below-4g=2G -cpu host,monitor=off \
    -smp "cpus=$cpus,sockets=1,cores=$cpus,threads=1" -m "$ram" \
    -drive "file=${disk//,/,,},format=qcow2,if=ide" -boot c \
    -drive file=/tmp/vista-driver.iso,format=raw,media=cdrom,if=ide,index=2,readonly=on \
    -vga none \
    -device virtio-vga-gl,vgamem_mb=128,blob=true,hostmem=1G,max_hostmem=1G,neptune=true \
    -device piix3-usb-uhci,id=vista-usb -device usb-tablet,id=vista-tablet,bus=vista-usb.0 \
    "${audio_args[@]}" \
    -nic user,model=e1000,id=vista-net,mac=52:54:00:56:49:53 \
    -display gtk,gl=on,zoom-to-fit=off \
    -qmp "unix:${vm//,/,,}/qmp.sock,server=on,wait=off" \
    -serial "file:$vm/com1.log" \
    -serial "unix:${vm//,/,,}/control/control.sock,server=on,wait=off" \
    -debugcon "file:$vm/kmd-debugcon.log" -global isa-debugcon.iobase=0xe9 \
    -rtc base=localtime >/dev/null

sleep 2
if [[ $(podman inspect --format '{{.State.Running}}' "$name") != true ]]; then
    podman logs --tail 40 "$name" >&2
    die 'QEMU exited during startup; see the log above.'
fi
printf 'Vista started in its QEMU window (%s MiB RAM, %s CPUs).\n' "$ram" "$cpus"
printf 'Logs: podman logs -f %s\n' "$name"
