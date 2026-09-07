#!/usr/bin/env python3
"""Realize Neptune with KVM/EGL, paused and with no guest disk attached."""
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time

root = Path(__file__).resolve().parent.parent
out = root / 'test-artifacts/linux-build'
out.mkdir(parents=True, exist_ok=True)
env = dict(os.environ)
env['LD_LIBRARY_PATH'] = str(root / 'host-linux/lib/x86_64-linux-gnu')
env['RENDER_SERVER_EXEC_PATH'] = str(root / 'host-linux/libexec/virgl_render_server')
with tempfile.TemporaryDirectory(prefix='neptune-smoke-') as temporary:
    endpoint = Path(temporary) / 'qmp.sock'
    command = [str(root / 'triton-qemu/build-linux/qemu-system-x86_64'),
               '-name', 'vista-neptune-smoke', '-machine', 'pc,accel=kvm',
               '-cpu', 'host', '-m', '2048', '-S', '-nodefaults',
               '-L', str(root / 'triton-qemu/pc-bios'),
               '-device', 'virtio-vga-gl,blob=true,hostmem=1G,max_hostmem=1G,neptune=true',
               '-display', 'egl-headless,rendernode=/dev/dri/renderD128',
               '-qmp', f'unix:{endpoint},server=on,wait=off']
    with (out / 'qemu-smoke.stderr.log').open('w') as errors:
        process = subprocess.Popen(command, env=env, stdout=subprocess.DEVNULL, stderr=errors)
        try:
            deadline = time.monotonic() + 15
            while not endpoint.exists():
                if process.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError('QEMU did not start; see qemu-smoke.stderr.log')
                time.sleep(0.05)
            with socket.socket(socket.AF_UNIX) as client:
                client.settimeout(15)
                client.connect(str(endpoint))
                stream = client.makefile('rwb')
                greeting = json.loads(stream.readline())

                def execute(name):
                    stream.write((json.dumps({'execute': name}) + '\n').encode())
                    stream.flush()
                    while True:
                        line = stream.readline()
                        if not line:
                            raise RuntimeError('QMP closed before reply')
                        reply = json.loads(line)
                        if 'error' in reply:
                            raise RuntimeError(reply['error'])
                        if 'return' in reply:
                            return reply['return']

                execute('qmp_capabilities')
                status = execute('query-status')
                kvm = execute('query-kvm')
                pci = execute('query-pci')
                if status['running'] or not kvm['enabled']:
                    raise RuntimeError('Expected paused KVM VM')
                devices = [d for bus in pci for d in bus['devices']]
                if not any(d.get('qdev_id') is not None and
                           d.get('id', {}).get('vendor') == 0x1af4 for d in devices):
                    raise RuntimeError('Virtio PCI device not realized')
                execute('quit')
            process.wait(timeout=15)
            if process.returncode:
                raise RuntimeError(f'QEMU exited {process.returncode}')
            evidence = {'result': 'PASS', 'scope': 'paused device realization; no guest disk',
                        'command': command, 'version': greeting['QMP']['version'],
                        'status': status, 'kvm': kvm, 'pci': pci}
            (out / 'qemu-smoke.json').write_text(json.dumps(evidence, indent=2) + '\n')
            print('PASS: custom QEMU realizes Neptune with KVM and hardware EGL; no guest disk attached')
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
