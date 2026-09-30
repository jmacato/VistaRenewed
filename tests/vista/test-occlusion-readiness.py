#!/usr/bin/env python3
"""Build an isolated backend and check numeric query readiness on real drivers.

This single-output regression does not certify the separate Intel MRT exact-count
gate. --negative-library accepts a directory containing an uncorrected backend.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import subprocess

from test_config import ARTIFACTS, HOST_PREFIX, QEMU_BUILD, native_headers, native_libraries, sdk_header, container_command, container_path, vista_compile_database

ROOT = Path(__file__).resolve().parents[2]
OUT = ARTIFACTS / 'occlusion-review/readiness'
BUILD = OUT / 'build'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-only', action='store_true')
    parser.add_argument('--device', help='Required for runs: hardware device-name filter')
    parser.add_argument('--software-device', help='Required for runs: software device-name filter, e.g. llvmpipe')
    parser.add_argument('--run-only', action='store_true')
    parser.add_argument('--negative-library', type=Path)
    args = parser.parse_args()
    if args.build_only and args.run_only:
        parser.error('--build-only and --run-only are mutually exclusive')
    if not args.build_only and (not args.device or not args.software_device):
        parser.error('--device and --software-device are required when running GPU cases')
    OUT.mkdir(parents=True, exist_ok=True)
    executable = OUT / 'test-occlusion-readiness'
    if not args.run_only:
        container = container_command()
        relative = str(BUILD.relative_to(ROOT))
        with (OUT / 'build.log').open('w') as log:
            if not (BUILD / 'build.ninja').exists():
                subprocess.run(container + ['meson', 'setup', relative, 'triton-dxvk',
                    '--buildtype=release', '-Denable_d3d8=false', '-Denable_d3d9=false',
                    '-Denable_d3d10=false', '-Dnative_headless=true', '-Dnative_sdl2=disabled',
                    '-Dnative_sdl3=disabled', '-Dnative_glfw=disabled'],
                    stdout=log, stderr=subprocess.STDOUT, check=True)
            subprocess.run(container + ['ninja', '-C', relative, '-j', '1'],
                           stdout=log, stderr=subprocess.STDOUT, check=True)
            subprocess.run(['g++', '-std=c++17', '-O2',
                *native_headers(),
                '-I' + str(ROOT / 'triton-umd/src/virtio/neptune/triton'),
                str(ROOT / 'tests/vista/test-occlusion-readiness.cpp'),
                '-L' + str(BUILD / 'src/d3d11'), '-L' + str(BUILD / 'src/dxgi'),
                '-ldxvk_d3d11', '-ldxvk_dxgi', '-o', str(executable)],
                stdout=log, stderr=subprocess.STDOUT, check=True)
        paths = [ROOT / 'tests/vista/test-occlusion-readiness.cpp',
                 ROOT / 'tests/vista/test-occlusion-readiness.py',
                 ROOT / 'triton-umd/src/virtio/neptune/triton/tritonBlitShaders.h', executable]
        paths += sorted((ROOT / 'triton-dxvk/src/dxvk').glob('dxvk_*query.*'))
        paths += sorted((ROOT / 'triton-dxvk/src/dxvk').glob('dxvk_queue.*'))
        paths += sorted((ROOT / 'triton-dxvk/src/dxvk').glob('dxvk_device.*'))
        paths += sorted((ROOT / 'triton-dxvk/src/dxvk').glob('dxvk_adapter.*'))
        paths += sorted((ROOT / 'triton-dxvk/src/d3d11').glob('d3d11_query.*'))
        paths += [BUILD / 'src/d3d11/libdxvk_d3d11.so', BUILD / 'src/dxgi/libdxvk_dxgi.so']
        (OUT / 'source-and-binary-sha256.json').write_text(json.dumps({
            str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in paths}, indent=2) + '\n')
    if args.build_only:
        print('Occlusion readiness build passed; GPU tests not executed')
        return

    frozen = json.loads((OUT / 'source-and-binary-sha256.json').read_text())
    for relative, expected in frozen.items():
        if hashlib.sha256((ROOT / relative).read_bytes()).hexdigest() != expected:
            raise RuntimeError(f'Build evidence changed: {relative}; rebuild before running')

    # Keep old fault injection, adapter overrides and external config out.
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(('DXVK_', 'DIAG_', 'PREDICATION_VK_', 'VK_', 'MESA_', 'INTEL_', 'LD_'))}
    env.update(LD_LIBRARY_PATH=os.pathsep.join(map(str, [BUILD / 'src/d3d11', BUILD / 'src/dxgi'])),
               DXVK_WSI_DRIVER='Headless', DXVK_LOG_LEVEL='info', DXVK_LOG_PATH=str(OUT))
    modes = [('software', {'DXVK_FILTER_DEVICE_NAME': args.software_device}, True),
             ('native', {'DXVK_FILTER_DEVICE_NAME': args.device}, True),
             ('secondary', {'DXVK_FILTER_DEVICE_NAME': args.device,
                            'DXVK_CONFIG': 'dxvk.tilerMode = True'}, True)]
    if args.negative_library:
        modes.append(('negative-unpatched', {'DXVK_FILTER_DEVICE_NAME': args.software_device,
                     'LD_LIBRARY_PATH': str(args.negative_library.resolve())}, False))
    for name, extra, success in modes:
        with (OUT / (name + '.log')).open('w') as log:
            result = subprocess.run([str(executable)], env=env | extra,
                                    stdout=log, stderr=subprocess.STDOUT, timeout=90)
        text = (OUT / (name + '.log')).read_text()
        device = extra['DXVK_FILTER_DEVICE_NAME']
        if device.casefold() not in text.casefold():
            raise RuntimeError(f'{name}: expected driver was not selected')
        summary = re.search(r'OCCLUSION-READINESS passed=(\d+) failed=(\d+)', text)
        if not summary or int(summary[1]) + int(summary[2]) != 2002:
            raise RuntimeError(f'{name}: incomplete oracle; inspect {OUT / (name + ".log")}')
        expected_exit = 0 if success else 1
        if result.returncode != expected_exit or (int(summary[2]) == 0) != success:
            raise RuntimeError(f'{name}: unexpected exit {result.returncode} '
                               f'(expected {expected_exit}): {summary.group(0)}')
        if not success:
            failures = re.findall(r'^FAIL iteration=\d+ (.+)$', text, re.MULTILINE)
            if not failures or any(label not in ('inner exact count', 'overlapping outer exact count',
                                                'fragmented scope exact count')
                                   for label in failures):
                raise RuntimeError(f'{name}: negative control failed outside the readiness oracle')
        print(name, summary.group(0))
    print('Occlusion readiness checks passed; Intel MRT precision remains a separate gate')


if __name__ == '__main__':
    main()
