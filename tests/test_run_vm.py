"""Exercise the launcher with mocked Podman; no real guest is started."""
from pathlib import Path
import json
import os
import shutil
import socket
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
MOCK = '''#!/usr/bin/env python3
import json, os, sys
from pathlib import Path
args = sys.argv[1:]
entries = json.loads(Path(os.environ['VM_MOCK_ENTRIES']).read_text())
with open(os.environ['VM_MOCK_CALLS'], 'a') as log:
    log.write(json.dumps(args) + '\\n')
if args[:2] == ['container', 'exists']:
    sys.exit(not any(e['Name'].lstrip('/') == args[2] for e in entries))
if args == ['ps', '--quiet']:
    print('\\n'.join(e['Name'].lstrip('/') for e in entries))
elif args[0] == 'inspect':
    if args[1] == '--format': print('true')
    else: print(json.dumps([e for e in entries if e['Name'].lstrip('/') in args[1:]]))
elif args[0] in ('image', 'run', 'exec', 'logs', 'rm'):
    pass
else:
    sys.exit('unexpected Podman call: ' + repr(args))
'''


class LauncherTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='run-vm-')
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name)
        self.root = self.base / 'workspace'
        self.root.mkdir()
        self.bin = self.base / 'bin'
        self.bin.mkdir()
        self.entries = self.base / 'entries.json'
        self.entries.write_text('[]')
        self.calls = self.base / 'calls.jsonl'
        self.calls.touch()
        mock = self.bin / 'podman'
        mock.write_text(MOCK)
        mock.chmod(0o755)
        sleep = self.bin / 'sleep'
        sleep.write_text('#!/bin/sh\nexit 0\n')
        sleep.chmod(0o755)
        # Substitute only host prerequisites. The full launcher logic and all
        # filesystem/Podman arguments still run in Bash under their real names.
        source = (ROOT / 'run-vm.sh').read_text()
        for old, replacement in [('/dev/kvm', 'kvm'),
                                 ('/dev/dri/renderD128', 'render')]:
            path = self.base / replacement
            path.touch()
            source = source.replace(old, str(path))
        self.launcher = self.root / 'run-vm.sh'
        self.launcher.write_text(source)
        (self.root / 'scripts').mkdir()
        shutil.copy2(ROOT / 'scripts/vista_disk_mounts.py',
                     self.root / 'scripts/vista_disk_mounts.py')
        for relative in ('triton-qemu/build-linux/qemu-system-x86_64',
                         'host-linux/libexec/virgl_render_server'):
            path = self.root / relative
            path.parent.mkdir(parents=True)
            path.touch()
            path.chmod(0o755)
        self.disk = self.base / 'external disk.qcow2'
        self.make_image(self.disk)
        self.iso = self.base / 'driver.iso'
        self.iso.touch()
        self.auth = self.base / 'xauthority'
        self.auth.touch()
        self.pipewire = self.base / 'pipewire'
        self.socket = socket.socket(socket.AF_UNIX)
        self.socket.bind(str(self.pipewire))
        self.addCleanup(self.socket.close)
        self.state = self.base / 'state'
        inherited = {k: v for k, v in os.environ.items() if not k.startswith('VISTA_')}
        self.env = dict(inherited, PATH=str(self.bin) + os.pathsep + os.environ['PATH'],
                        DISPLAY=':99', XAUTHORITY=str(self.auth),
                        PIPEWIRE_REMOTE=str(self.pipewire),
                        VISTA_DISK=str(self.disk), VISTA_ISO=str(self.iso),
                        VISTA_STATE_DIR=str(self.state), VISTA_VM_NAME='test-vista',
                        VM_MOCK_ENTRIES=str(self.entries), VM_MOCK_CALLS=str(self.calls))
        for name in ('VISTA_RENDER_NODE', 'VISTA_GPU_FILTER', 'VISTA_NVIDIA_DEVICE'):
            self.env.pop(name, None)

    def make_image(self, path, backing=None, fmt='qcow2'):
        command = ['qemu-img', 'create', '-q', '-f', fmt]
        if backing:
            command += ['-F', 'qcow2', '-b', str(backing)]
        subprocess.run(command + [str(path), '1M'], check=True)

    def launch(self, *arguments):
        result = subprocess.run(['bash', str(self.launcher), *arguments], cwd=self.base,
                                env=self.env, text=True, capture_output=True, timeout=10)
        calls = [json.loads(line) for line in self.calls.read_text().splitlines()]
        return result, [call for call in calls if call[0] == 'run']

    def entry(self, name='test-vista', disk=None, state=None, labelled=True):
        disk = self.disk if disk is None else disk
        state = self.state if state is None else state
        args = ['-drive', 'file=' + str(disk).replace(',', ',,') + ',format=qcow2,if=ide',
                '-qmp', 'unix:' + str(state).replace(',', ',,') + '/qmp.sock,server=on,wait=off']
        return {'Name': '/' + name, 'Args': args,
                'Config': {'Labels': {'io.triton.vista.state-dir': str(state)} if labelled else {}}}

    def test_public_defaults_and_no_sidebar_dependency(self):
        self.env.pop('VISTA_ISO')
        iso = self.root / 'dist/triton-vista-x64.iso'
        iso.parent.mkdir()
        iso.touch()
        result, runs = self.launch()
        self.assertEqual(result.returncode, 0, result.stderr)
        args = runs[0]
        self.assertIn('cpus=4,sockets=1,cores=4,threads=1', args)
        self.assertIn('DXVK_FILTER_DEVICE_NAME=', args)
        self.assertIn('localhost/triton-vista-builder:preview', args)
        self.assertIn(str(iso) + ':/tmp/vista-driver.iso:ro', args)
        self.assertFalse(any('sidebar' in arg.lower() or '/var/tmp/triton-vista-media' in arg
                             for arg in args))
        calls = [json.loads(line) for line in self.calls.read_text().splitlines()]
        self.assertFalse(any(call[0] == 'exec' for call in calls))

    def test_custom_host_builds_and_runtime_image(self):
        prefix = self.base / 'custom host prefix'
        qemu = self.base / 'custom qemu build'
        for path in (prefix / 'libexec/virgl_render_server', qemu / 'qemu-system-x86_64'):
            path.parent.mkdir(parents=True)
            path.touch()
            path.chmod(0o755)
        self.env.update(VISTA_HOST_PREFIX=str(prefix), VISTA_QEMU_BUILD_DIR=str(qemu),
                        VISTA_BUILD_IMAGE='localhost/custom-vista:test')
        result, runs = self.launch()
        self.assertEqual(result.returncode, 0, result.stderr)
        args = runs[0]
        self.assertIn(str(qemu / 'qemu-system-x86_64'), args)
        self.assertIn('RENDER_SERVER_EXEC_PATH=' + str(prefix / 'libexec/virgl_render_server'), args)
        self.assertIn('LD_LIBRARY_PATH=' + str(prefix / 'lib/x86_64-linux-gnu'), args)
        self.assertIn('localhost/custom-vista:test', args)
        self.assertIn(str(prefix) + ':' + str(prefix) + ':ro', args)
        self.assertIn(str(qemu) + ':' + str(qemu) + ':ro', args)

    def test_local_rtc_preserves_explicit_timezone(self):
        self.env['TZ'] = 'Asia/Manila'
        result, runs = self.launch()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('TZ=Asia/Manila', runs[0])
        self.assertIn('base=localtime', runs[0])

    def test_local_rtc_uses_host_zone_without_exported_tz(self):
        self.env.pop('TZ', None)
        readlink = self.bin / 'readlink'
        readlink.write_text('#!/bin/sh\necho /usr/share/zoneinfo/Asia/Manila\n')
        readlink.chmod(0o755)
        result, runs = self.launch()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('TZ=Asia/Manila', runs[0])

    def test_audio_none_omits_socket_and_usb_audio(self):
        self.env['VISTA_AUDIO'] = 'none'
        result, runs = self.launch()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn('-audiodev', runs[0])
        self.assertFalse(any('usb-audio' in arg or 'vista-pipewire' in arg for arg in runs[0]))

    def test_audio_auto_without_socket_still_launches(self):
        self.env['PIPEWIRE_REMOTE'] = str(self.base / 'absent-pipewire')
        result, runs = self.launch()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn('-audiodev', runs[0])

    def test_audio_auto_with_socket_enables_usb_audio(self):
        result, runs = self.launch()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('-audiodev', runs[0])
        self.assertIn(str(self.pipewire) + ':/tmp/vista-pipewire:ro', runs[0])
        self.assertTrue(any(arg.startswith('usb-audio,') for arg in runs[0]))

    def test_required_pipewire_and_invalid_audio_fail_before_launch(self):
        self.env['PIPEWIRE_REMOTE'] = str(self.base / 'absent-pipewire')
        for value, message in [('pipewire', 'PipeWire socket is missing'),
                               ('bogus', 'VISTA_AUDIO must be')]:
            self.env['VISTA_AUDIO'] = value
            result, runs = self.launch()
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(message, result.stderr)
            self.assertFalse(runs)

    def test_help_needs_no_runtime_prerequisites(self):
        self.env.pop('DISPLAY', None)
        self.disk.unlink()
        result, runs = self.launch('--help')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('VISTA_BUILD_IMAGE', result.stdout)
        self.assertIn('VISTA_AUDIO', result.stdout)
        self.assertFalse(runs)

    def test_external_disk_and_state_mounts(self):
        result, runs = self.launch()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(len(runs), 1)
        args = runs[0]
        self.assertIn(str(self.disk) + ':' + str(self.disk), args)
        self.assertIn(str(self.state) + ':' + str(self.state), args)
        self.assertIn('io.triton.vista.state-dir=' + str(self.state), args)

    def test_relative_state_uses_caller_directory(self):
        self.env['VISTA_STATE_DIR'] = 'relative state'
        result, runs = self.launch()
        self.assertEqual(result.returncode, 0, result.stderr)
        expected = self.base / 'relative state'
        self.assertIn(str(expected) + ':' + str(expected), runs[0])
        self.assertIn('unix:' + str(expected) + '/qmp.sock,server=on,wait=off', runs[0])

    def test_nvidia_render_node_and_cdi_device(self):
        render = self.base / 'nvidia render'
        render.touch()
        self.env.update(VISTA_RENDER_NODE=str(render), VISTA_GPU_FILTER='NVIDIA',
                        VISTA_NVIDIA_DEVICE='nvidia.com/gpu=0')
        result, runs = self.launch()
        self.assertEqual(result.returncode, 0, result.stderr)
        args = runs[0]
        devices = [args[i + 1] for i, arg in enumerate(args) if arg == '--device']
        self.assertIn(str(render), devices)
        self.assertIn('nvidia.com/gpu=0', devices)
        self.assertNotIn(str(self.base / 'render'), devices)
        self.assertIn('DXVK_FILTER_DEVICE_NAME=NVIDIA', args)
        self.assertIn('GBM_BACKEND=nvidia-drm', args)
        self.assertIn('GBM_BACKENDS_PATH=/usr/lib64/gbm:/usr/lib/x86_64-linux-gnu/gbm', args)
        self.assertIn('VIRGL_GBM_LAYOUT_FORCE_ENABLE=1', args)

    def test_invalid_cdi_name_rejected_before_launch(self):
        self.env['VISTA_NVIDIA_DEVICE'] = 'nvidia.com/gpu=0 --privileged'
        result, runs = self.launch()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Invalid VISTA_NVIDIA_DEVICE', result.stderr)
        self.assertFalse(runs)

    def test_other_names_get_isolated_default_state(self):
        del self.env['VISTA_STATE_DIR']
        result, runs = self.launch()
        self.assertEqual(result.returncode, 0, result.stderr)
        expected = self.root / 'vista-kvm/x64-base/instances/test-vista'
        self.assertIn('io.triton.vista.state-dir=' + str(expected), runs[0])

    def test_running_instance_matches_disk_and_state(self):
        self.entries.write_text(json.dumps([self.entry()]))
        result, runs = self.launch()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(runs)
        self.assertIn('already running', result.stdout)

    def test_running_instance_rejects_wrong_state(self):
        self.entries.write_text(json.dumps([self.entry(state=self.base / 'other')]))
        result, runs = self.launch()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('different disk or state', result.stderr)
        self.assertFalse(runs)

    def test_running_instance_rejects_wrong_disk(self):
        self.entries.write_text(json.dumps([self.entry(disk=self.base / 'other.qcow2')]))
        result, runs = self.launch()
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(runs)

    def test_other_container_cannot_reuse_state(self):
        for labelled in (True, False):
            self.entries.write_text(json.dumps([self.entry(name='other', labelled=labelled)]))
            result, runs = self.launch()
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('already used by running container other', result.stderr)
            self.assertFalse(runs)

    def test_symlink_state_is_canonicalized(self):
        self.state.mkdir()
        alias = self.base / 'state-link'
        alias.symlink_to(self.state)
        self.env['VISTA_STATE_DIR'] = str(alias)
        self.entries.write_text(json.dumps([self.entry(name='other')]))
        result, runs = self.launch()
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(runs)

    def test_comma_in_disk_filename_is_escaped(self):
        disk = self.base / 'disk,readonly=on.qcow2'
        self.make_image(disk)
        self.env['VISTA_DISK'] = str(disk)
        result, runs = self.launch()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('file=' + str(disk).replace(',', ',,') + ',format=qcow2,if=ide', runs[0])

    def test_external_backing_chain_across_directories(self):
        folder = self.base / 'backing files'
        folder.mkdir()
        base = folder / 'base.qcow2'
        self.make_image(base)
        middle = self.base / 'middle.qcow2'
        self.make_image(middle, 'backing files/base.qcow2')
        self.disk.unlink()
        self.make_image(self.disk, 'middle.qcow2')
        result, runs = self.launch()
        self.assertEqual(result.returncode, 0, result.stderr)
        for path in (base, middle):
            self.assertIn(str(path) + ':' + str(path) + ':ro', runs[0])
        self.assertIn(str(self.disk) + ':' + str(self.disk), runs[0])

    def test_missing_backing_rejected_before_launch(self):
        base = self.base / 'base.qcow2'
        self.make_image(base)
        self.disk.unlink()
        self.make_image(self.disk, base)
        base.unlink()
        result, runs = self.launch()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('backing image is missing', result.stderr)
        self.assertFalse(runs)

    def test_invalid_overlay_rejected_before_launch(self):
        self.disk.write_bytes(b'not a qcow2 image')
        result, runs = self.launch()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Cannot prepare VM image mounts', result.stderr)
        self.assertFalse(runs)

    def test_symlink_backing_retains_qemu_path(self):
        base = self.base / 'actual.qcow2'
        self.make_image(base)
        alias = self.base / 'alias.qcow2'
        alias.symlink_to(base)
        self.disk.unlink()
        self.make_image(self.disk, alias)
        result, runs = self.launch()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(str(base) + ':' + str(alias) + ':ro', runs[0])

    def test_external_data_file_is_writable_for_overlay(self):
        data = self.base / 'disk.data'
        self.disk.unlink()
        subprocess.run(['qemu-img', 'create', '-q', '-f', 'qcow2', '-o',
                        'data_file=' + str(data) + ',data_file_raw=on',
                        str(self.disk), '1M'], check=True)
        result, runs = self.launch()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(str(data) + ':' + str(data), runs[0])


if __name__ == '__main__':
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(LauncherTests))
    if result.wasSuccessful():
        print('VM LAUNCHER PATH AND OWNERSHIP CHECKS PASS')
    raise SystemExit(not result.wasSuccessful())
