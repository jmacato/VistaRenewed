#!/usr/bin/env python3
"""Build or explicitly run the actual DXVK direct-primary storage fixture.

The DXVK build directory must already contain the isolated candidate libraries.
Default is fixture compilation only. --run requires the GPU/KVM test slot and
must never overlap application performance captures. No result proves Vista
mapping or acceptable QEMU-window performance.
"""
import argparse
import hashlib
import itertools
import json
import os
from pathlib import Path
import subprocess
import time


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--run', action='store_true')
    parser.add_argument('--no-build', action='store_true')
    parser.add_argument('--case', choices=[f'{f}-{w}' for f, w in itertools.product(
        ['bgra', 'rgba'], [800, 1280, 1920])])
    parser.add_argument('--timeout', type=float, default=30)
    args = parser.parse_args()
    if not 10 <= args.timeout <= 60:
        parser.error('--timeout must be between 10 and 60 seconds')
    out, build = args.output.resolve(), args.build_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    lock = out / 'invocation-lock.json'
    try:
        reservation = lock.open('x')
    except FileExistsError:
        parser.error('output belongs to an active/interrupted invocation; use a fresh directory')
    try:
        with reservation:
            record = {'pid': os.getpid(), 'start_unix': time.time(), 'gpu_kvm_requested': args.run}
            reservation.write(json.dumps(record) + '\n')
            reservation.flush()
            os.fsync(reservation.fileno())
            if (out / 'run-start.json').exists() or (out / 'results.json').exists() or list(out.glob('*.run.log')):
                parser.error('prior run evidence exists; use a fresh directory')
            if args.run:
                with (out / 'run-start.json').open('x') as marker:
                    marker.write(json.dumps(record) + '\n')
                    marker.flush()
                    os.fsync(marker.fileno())
            source = root / 'tests/vista/test-direct-primary-dxvk.cpp'
            header = root / 'triton-dxvk/include/native/dxvk_shared_resource.h'
            libraries = [build / 'src' / name / ('libdxvk_' + name + '.so') for name in ('d3d11', 'dxgi')]
            exe = out / 'test-direct-primary-dxvk'
            cmd = ['g++', '-std=c++17', '-O2', '-Wall', '-Wextra',
                   '-I' + str(root / 'triton-dxvk/include/native'),
                   '-I' + str(root / 'host-linux/include/dxvk'), str(source),
                   '-L' + str(build / 'src/d3d11'), '-L' + str(build / 'src/dxgi'),
                   '-ldxvk_d3d11', '-ldxvk_dxgi', '-o', str(exe)]
            identity = {'source_sha256': digest(source), 'runner_sha256': digest(Path(__file__)),
                        'header_sha256': digest(header), 'compile_command': cmd,
                        'libraries': {str(p.resolve()): digest(p) for p in libraries}}
            if args.no_build:
                saved = json.loads((out / 'build-identity.json').read_text())
                for key, value in identity.items():
                    if saved.get(key) != value:
                        parser.error('--no-build identity changed: ' + key)
                if digest(exe) != saved.get('binary_sha256') or saved.get('build_exit_code') != 0:
                    parser.error('--no-build executable/build identity mismatch')
                identity = saved
            else:
                with (out / 'build.log').open('w') as log:
                    done = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, timeout=60)
                identity['build_exit_code'] = done.returncode
                if done.returncode == 0:
                    identity['binary_sha256'] = digest(exe)
                (out / 'build-identity.json').write_text(json.dumps(identity, indent=2) + '\n')
                if done.returncode:
                    print((out / 'build.log').read_text())
                    return done.returncode
            if not args.run:
                print('Fixture built only; no GPU/KVM/VM execution.')
                return 0
            env = {k: v for k, v in os.environ.items()
                   if not k.startswith('PREDICATION_VK_') and k not in ('LD_PRELOAD', 'DXVK_CONFIG', 'DXVK_CONFIG_FILE')}
            env.update(LD_LIBRARY_PATH=':'.join(str(build / 'src' / p) for p in ('d3d11', 'dxgi')),
                       DXVK_WSI_DRIVER='Headless', DXVK_FILTER_DEVICE_NAME='NVIDIA',
                       DXVK_SHADER_CACHE_PATH=str(out / 'cache'), DXVK_CONFIG_FILE='/dev/null',
                       DXVK_LOG_LEVEL='debug',
                       DXVK_CONFIG='dxgi.hideNvidiaGpu = False')
            results = []
            for fmt, (width, height) in itertools.product(['bgra', 'rgba'], [(800, 600), (1280, 720), (1920, 1080)]):
                name = f'{fmt}-{width}'
                if args.case and args.case != name:
                    continue
                directory = out / name
                directory.mkdir(exist_ok=False)
                start = time.monotonic()
                command = [str(exe), str(width), str(height), fmt]
                with (out / (name + '.run.log')).open('x') as log:
                    try:
                        done = subprocess.run(command, env=dict(env, DXVK_LOG_PATH=str(directory)),
                                              stdout=log, stderr=subprocess.STDOUT, timeout=args.timeout)
                        result = {'case': name, 'exit_code': done.returncode, 'timeout': False}
                    except subprocess.TimeoutExpired:
                        result = {'case': name, 'exit_code': None, 'timeout': True}
                result.update(command=command, elapsed_seconds=time.monotonic() - start)
                output = (out / (name + '.run.log')).read_text()
                result['passed'] = result['exit_code'] == 0 and 'RESULT direct_primary_dxvk_pass ' in output
                results.append(result)
                (out / 'results.json').write_text(json.dumps({'identity': identity, 'results': results,
                    'all_requested_passed': all(r['passed'] for r in results),
                    'application_performance_proven': False, 'persistent_vista_mapping_proven': False}, indent=2) + '\n')
                print(name, result, flush=True)
            return 0 if results and all(r['passed'] for r in results) else 2
    finally:
        lock.unlink()


if __name__ == '__main__':
    raise SystemExit(main())
