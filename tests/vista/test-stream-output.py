#!/usr/bin/env python3
"""Render real D3D10 SO aliases through a selected native DXVK backend."""
import argparse
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile

from test_config import ARTIFACTS, HOST_PREFIX, QEMU_BUILD, native_headers, native_libraries, sdk_header, container_command, container_path, vista_compile_database

ROOT = Path(__file__).resolve().parents[2]
OUT = ARTIFACTS / 'stream-output'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--backend', type=Path, help='isolated DXVK build directory; default installed backend')
    parser.add_argument('--baseline', action='store_true', help='allow historical backend without overflow predicates')
    args = parser.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    spec = importlib.util.spec_from_file_location('d3d10_checks', ROOT / 'tests/vista/run-d3d10-compat.py')
    checks = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(checks)
    fixture, production, helper, windows = checks.shader_inputs()
    prefix = fixture[:fixture.index('// PRODUCTION_DXBC_IMPLEMENTATION')]
    (OUT / 'windows.h').write_text(windows)
    (OUT / 'shader.c').write_text(prefix + production + '\n' + helper)
    includes = ['-I' + str(OUT), '-I' + str(checks.TRITON)]
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-O1', *includes,
                    '-c', str(OUT / 'shader.c'), '-o', str(OUT / 'shader.o')], check=True)
    libdirs = native_libraries() if not args.backend else []
    if args.backend:
        libdirs = [args.backend.resolve() / 'src/d3d11', args.backend.resolve() / 'src/dxgi'] + libdirs
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-O1', '-g', '-Wall', '-Wextra',
                    *native_headers(), str(Path(__file__).with_suffix('.cpp')),
                    str(OUT / 'shader.o'), *['-L' + str(p) for p in libdirs], '-ldxvk_d3d11', '-ldxvk_dxgi',
                    '-o', str(OUT / 'test')], check=True)
    env = os.environ.copy()
    env['LD_LIBRARY_PATH'] = ':'.join(map(str, libdirs)) + ':' + env.get('LD_LIBRARY_PATH', '')
    env['DXVK_WSI_DRIVER'] = 'Headless'
    env['DXVK_LOG_LEVEL'] = 'error'
    with tempfile.TemporaryDirectory(prefix='so-shader-cache-') as cache:
        env['DXVK_SHADER_CACHE_PATH'] = cache
        subprocess.run([str(OUT / 'test')] + (['--baseline'] if args.baseline else []), env=env, check=True)


if __name__ == '__main__':
    main()
