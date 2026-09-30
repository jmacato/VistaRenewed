#!/usr/bin/env python3
"""Compile exact production DMA-BUF routing and native oracle; --run owns GPU slot."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import time


def function(source, name):
    match = re.search(r'^(?:static )?(?:struct VirtIOGPUExternalContext|void|int|bool)\s*\n?' +
                      name + r'\([^;]*?\n\{.*?^\}', source, re.M | re.S)
    if not match:
        raise ValueError('missing production function: ' + name)
    return match.group(0) + '\n'


from test_config import QEMU_BUILD, required_file

def main():
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--run', action='store_true')
    parser.add_argument('--no-build', action='store_true')
    parser.add_argument('--device', help='Required with --run: Vulkan device-name filter')
    args = parser.parse_args()
    if args.run and not args.device:
        parser.error('--run requires --device')
    required_file(QEMU_BUILD / "config-host.h", "Configure/build public QEMU first, or set VISTA_QEMU_BUILD_DIR.")
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    reservation = root / 'tests/vista/evidence.py'
    spec = importlib.util.spec_from_file_location('storage_runner', reservation)
    storage = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(storage)
    with storage.reserve_output(out, args.run, parser):
        files = [Path(__file__).resolve(),
                 root / 'tests/vista/test-primary-external-integration.c',
                 root / 'tests/vista/test-primary-external-integration.cpp',
                 root / 'tests/vista/test-primary-external-integration.h',
                 root / 'triton-qemu/hw/display/virtio-gpu-virgl.c',
                 root / 'triton-qemu/ui/egl-external-copy.c',
                 root / 'triton-qemu/include/ui/egl-external-copy.h',
                 root / 'tests/vista/test-primary-external-copy.cpp',
                 root / 'tests/vista/external_copy_fixture.h',
                 reservation, QEMU_BUILD / 'config-host.h',
                 root / 'triton-qemu/include/standard-headers/drm/drm_fourcc.h']
        source = files[4].read_text()
        structs = source[source.index('struct VirtIOGPUExternalCopy {'):
                         source.index('static struct VirtIOGPUExternalContext')]
        names = ['virtio_gpu_external_context', 'virtio_gpu_external_stop',
                 'virtio_gpu_neptune_copy_destroy', 'virtio_gpu_neptune_copy_texture',
                 'virtio_gpu_neptune_external_only', 'virtio_gpu_neptune_external_copy',
                 'virtio_gpu_neptune_transfer_dmabuf']
        extracted = structs + '\n'.join(function(source, n) for n in names)
        original = files[7].read_text()
        oracle = original[:original.index('\nint main()')]
        for old, new in [('qemu_egl_external_copy_new', 'bridge_new'),
                         ('qemu_egl_external_copy_run', 'bridge_run'),
                         ('qemu_egl_external_copy_destroy', 'bridge_destroy')]:
            oracle = oracle.replace(old, new)
        generated = {'production-transfer.inc': extracted, 'helper-oracle.inc': oracle}
        flags = shlex.split(subprocess.check_output(
            ['pkg-config', '--cflags', 'glib-2.0', 'epoxy'], text=True))
        libs = shlex.split(subprocess.check_output(
            ['pkg-config', '--libs', 'glib-2.0', 'epoxy'], text=True))
        includes = ['-I' + str(out), '-I' + str(root / 'tests/vista'),
                    '-I' + str(root / 'triton-qemu'), '-I' + str(root / 'triton-qemu/include'),
                    '-I' + str(QEMU_BUILD)]
        helper = out / 'egl-external-copy.o'
        bridge = out / 'production-bridge.o'
        binary = out / 'primary-external-integration'
        commands = []
        for path, obj in [(files[5], helper), (files[1], bridge)]:
            commands.append([os.environ.get('CC', 'cc'), '-std=gnu11', '-O1', '-g',
                             '-Wall', '-Wextra', '-Wno-unused-parameter',
                             *includes, *flags, '-c', str(path), '-o', str(obj)])
        commands.append([os.environ.get('CXX', 'c++'), '-std=c++17', '-O1', '-g',
                         '-Wall', '-Wextra', '-Wno-missing-field-initializers',
                         *includes, '-I' + str(root / 'triton-dxvk/include/vulkan/include'),
                         *flags, str(files[2]), str(bridge), str(helper), '-o', str(binary),
                         '-l:libvulkan.so.1', *libs])
        identity = {'source_sha256': {str(p): hashlib.sha256(p.read_bytes()).hexdigest()
                                      for p in files}, 'commands': commands,
                    'generated_sha256': {name: hashlib.sha256(body.encode()).hexdigest()
                                         for name, body in generated.items()}}
        if args.no_build:
            saved = json.loads((out / 'build-identity.json').read_text())
            for key, value in identity.items():
                if saved.get(key) != value:
                    parser.error('--no-build identity changed: ' + key)
            for name in generated:
                if hashlib.sha256((out / name).read_bytes()).hexdigest() != identity['generated_sha256'][name]:
                    parser.error('generated production/oracle input changed: ' + name)
            if hashlib.sha256(binary.read_bytes()).hexdigest() != saved['binary_sha256']:
                parser.error('binary identity changed')
            identity = saved
        else:
            for name, body in generated.items():
                (out / name).write_text(body)
            with (out / 'build.log').open('w') as log:
                for command in commands:
                    result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=60)
                    if result.returncode:
                        identity['build_exit_code'] = result.returncode
                        (out / 'build-identity.json').write_text(json.dumps(identity, indent=2) + '\n')
                        print((out / 'build.log').read_text())
                        return result.returncode
            identity.update(build_exit_code=0, binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest())
            (out / 'build-identity.json').write_text(json.dumps(identity, indent=2) + '\n')
        if not args.run:
            print('Compiled only; no GPU/KVM/VM execution.')
            print(shlex.join(['python3', str(Path(__file__).resolve()), '--run', '--device', 'DEVICE_NAME', '--no-build', '--output', str(out)]))
            return 0
        start = time.time()
        with (out / 'bridge.stdout.jsonl').open('x') as stdout, (out / 'bridge.stderr.log').open('x') as stderr:
            try:
                run = subprocess.run([str(binary)], env=dict(os.environ, TRITON_TEST_DEVICE=args.device), stdout=stdout, stderr=stderr, timeout=30)
                result = {'exit_code': run.returncode, 'timeout': False}
            except subprocess.TimeoutExpired:
                result = {'exit_code': None, 'timeout': True}
        records, errors = [], []
        for line in (out / 'bridge.stdout.jsonl').read_text(errors='replace').splitlines():
            try:
                record = json.loads(line)
                if not isinstance(record, dict):
                    raise ValueError('not a JSON object')
                records.append(record)
            except ValueError:
                errors.append(line)
        result.update(start_unix=start, elapsed_seconds=time.time() - start, records=records, parse_errors=errors)
        result['production_transfer_bridge_pass'] = result['exit_code'] == 0 and not errors and any(
            r.get('stage') == 'result' and r.get('status') == 'production_transfer_bridge_pass' for r in records)
        result['normal_renderable_path_passed'] = any(
            r.get('stage') == 'normal_renderable_path' and r.get('status') == 'passed' for r in records)
        (out / 'results.json').write_text(json.dumps({'identity': identity, 'result': result,
            'full_qemu_integration_proven': False, 'application_performance_proven': False}, indent=2) + '\n')
        print(json.dumps({k: v for k, v in result.items() if k not in ('records', 'parse_errors')}))
        return 0 if result['production_transfer_bridge_pass'] else 2


if __name__ == '__main__':
    raise SystemExit(main())
