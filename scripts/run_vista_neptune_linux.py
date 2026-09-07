#!/usr/bin/env python3
"""Start verified Vista deployment media and Neptune hardware; no guest input."""
import hashlib
import json
import os
from pathlib import Path
import secrets
import subprocess
import tempfile
import time

root = Path(__file__).resolve().parent.parent
disk = root/'vista-kvm/work.qcow2'
bundle = (root/'vista-kvm/deploy-current').resolve(strict=True)
image = 'localhost/vista-driver-builder:20260907'
if bundle.parent != root/'vista-kvm/publications':
    raise SystemExit('Media is not an immutable publication')
with (bundle/'media.iso').open('rb') as stream:
    actual = hashlib.file_digest(stream, 'sha256').hexdigest()
if actual != (bundle/'media.iso.sha256').read_text().split()[0]:
    raise SystemExit('Media hash mismatch')
# qemu-img takes an exclusive lock for this read-only check, rejecting live use.
subprocess.run(['qemu-img', 'check', str(disk)], check=True)
cpus = int(os.environ.get('VISTA_CPUS', '4'))
accel = os.environ.get('VISTA_ACCEL', 'kvm')
display = os.environ.get('VISTA_DISPLAY', 'egl-headless')
if accel not in ('kvm', 'tcg'):
    raise SystemExit('VISTA_ACCEL must be kvm or tcg')
if display not in ('gtk', 'egl-headless'):
    raise SystemExit('VISTA_DISPLAY must be gtk or egl-headless')
gui_options = []
if display == 'gtk':
    xauthority = Path(os.environ.get('XAUTHORITY', ''))
    if not os.environ.get('DISPLAY') or not xauthority.is_file():
        raise SystemExit('Native GTK display requires DISPLAY and an XAUTHORITY file')
    gui_options = ['-v', '/tmp/.X11-unix:/tmp/.X11-unix:ro',
                   '-v', f'{xauthority}:/tmp/vista-xauthority:ro',
                   '-e', 'XAUTHORITY=/tmp/vista-xauthority',
                   '-e', 'DISPLAY=' + os.environ['DISPLAY'],
                   '-e', 'GDK_BACKEND=x11']
recovery_vga = os.environ.get('VISTA_RECOVERY_VGA') == '1'
if not 1 <= cpus <= 16:
    raise SystemExit('VISTA_CPUS must be 1..16')
run = Path(tempfile.mkdtemp(prefix='neptune-', dir=root/'vista-kvm/runs'))
token = secrets.token_hex(32)
name = 'vista-aero-'+token[:16]
qemu = root/'triton-qemu/build-linux/qemu-system-x86_64'
command = ['podman', 'run', '-d', '--name', name, '--security-opt', 'label=disable',
           *(['--device', '/dev/kvm'] if accel == 'kvm' else []),
           '--device', '/dev/dri/renderD128', '--group-add', 'keep-groups',
           *gui_options,
           '-v', f'{root}:{root}', '-w', str(root),
           '-e', f'LD_LIBRARY_PATH={root}/host-linux/lib/x86_64-linux-gnu',
           '-e', f'RENDER_SERVER_EXEC_PATH={root}/host-linux/libexec/virgl_render_server',
           '-e', 'DXVK_WSI_DRIVER=Headless', '-e', 'DXVK_FILTER_DEVICE_NAME=Intel',
           image, str(qemu), '-name', name,
           '-L', str(root/'triton-qemu/pc-bios'), '-machine', f'pc,accel={accel}',
           '-cpu', 'core2duo,monitor=off', '-smp', f'cpus={cpus},sockets=1,cores={cpus},threads=1',
           '-m', '2048', '-drive', f'file={disk},format=qcow2,if=ide', '-boot', 'c',
           '-drive', f'file={bundle}/media.iso,format=raw,media=cdrom,if=ide,readonly=on',
           *(['-vga', 'std'] if recovery_vga else
             ['-vga', 'none', '-device', 'virtio-vga-gl,vgamem_mb=128,blob=true,hostmem=1G,max_hostmem=1G,neptune=true']),
           '-device', 'piix3-usb-uhci,id=vista-usb',
           '-device', 'usb-tablet,id=vista-tablet,bus=vista-usb.0',
           '-nic', 'none', '-display',
           ('gtk,gl=on' if display == 'gtk' else 'egl-headless,rendernode=/dev/dri/renderD128'),
           *(['-vnc', f'unix:{run}/vnc.sock'] if display == 'egl-headless' else []),
           '-qmp', f'unix:{run}/qmp.sock,server=on,wait=off',
           '-serial', f'file:{run}/com1.log', '-serial', f'file:{run}/status.log',
           '-debugcon', f'file:{run}/kmd-debugcon.log',
           '-global', 'isa-debugcon.iobase=0xe9']
(run/'command.json').write_text(json.dumps(command, indent=2)+'\n')
(run/'run-token').write_text(token+'\n')
(run/'deployment-id').write_text((bundle/'deployment-id').read_text())
(run/'qemu-path').write_text(str(qemu)+'\n')
(run/'container-name').write_text(name+'\n')
container = subprocess.check_output(command, text=True).strip()
(run/'container-id').write_text(container+'\n')
for _ in range(50):
    state = json.loads(subprocess.check_output(['podman','inspect',container],text=True))[0]['State']
    if not state['Running']:
        raise SystemExit(subprocess.check_output(['podman','logs',container],text=True,stderr=subprocess.STDOUT))
    if (run/'qmp.sock').is_socket():
        break
    time.sleep(.1)
else:
    raise SystemExit(f'QMP unavailable; inspect container {container}')
(run/'qemu-pid').write_text(str(state['Pid'])+'\n')
(run/'qemu.pid').write_text(str(state['Pid'])+'\n')
latest = root/'vista-kvm/latest'
next_link = latest.with_name('.latest-neptune.tmp')
next_link.symlink_to(run)
next_link.replace(latest)
print(f'Neptune VM started: {run}\nHost QEMU PID: {state["Pid"]}')
