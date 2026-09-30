#!/usr/bin/env python3
"""Build and execute the production D3D9 converter against native DXVK."""
from __future__ import annotations
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

from test_config import ARTIFACTS, HOST_PREFIX, QEMU_BUILD, native_headers, native_libraries, sdk_header, container_command, container_path, vista_compile_database

ROOT = Path(__file__).resolve().parents[2]
D9 = ROOT / 'triton-umd/src/virtio/neptune/vista-d3d9'
CONV = D9 / 'third_party/d3d9on12-shaderconverter'


def run(command, **kwargs):
    subprocess.run([str(arg) for arg in command], check=True, **kwargs)


def host(sanitize=False):
    run(['python3', D9 / 'tests/native_compat/shader_cache_test.py'])
    prefix = HOST_PREFIX
    sdk = sdk_header('d3d12TokenizedProgramFormat.hpp').parent
    compat = D9 / 'tests/native_compat'
    include = [compat, sdk, CONV / 'Inc',
               CONV / 'ShaderConv', CONV / 'ShaderBinary', D9.parent,
               D9.parent / 'triton']
    common = ['-O1', '-g', '-include', compat / 'shaderconv_native.h']
    common += native_headers() + [f'-I{path}' for path in include]
    if sanitize:
        common += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    with tempfile.TemporaryDirectory(prefix='triton9-shaders-') as directory:
        build = Path(directory)
        dxbc = build / 'dxbc.o'
        run([os.environ.get('CC', 'cc'), '-std=c11', *common, '-c',
             D9.parent / 'triton/tritonDxbc.c', '-o', dxbc])
        sources = sorted((CONV / 'ShaderConv').glob('*.cpp'))
        sources += sorted((CONV / 'ShaderBinary').glob('*.cpp'))
        run([os.environ.get('CXX', 'c++'), '-std=c++17', *common,
             D9 / 'tests/triton9_shaderconv_test.cpp', D9 / 'triton9_fixed.cpp', *sources, dxbc,
             *['-L'+str(p) for p in native_libraries()], '-ldxvk_d3d11',
             '-ldxvk_dxgi', '-o', build / 'shader-test'])
        env = os.environ.copy()
        env['LD_LIBRARY_PATH'] = os.pathsep.join(map(str, native_libraries()))
        env['DXVK_WSI_DRIVER'] = 'Headless'
        env['DXVK_LOG_LEVEL'] = 'error'
        if sanitize:
            env['ASAN_OPTIONS'] = 'detect_leaks=0:halt_on_error=1'
            env['UBSAN_OPTIONS'] = 'halt_on_error=1:print_stacktrace=1'
        run([build / 'shader-test'], env=env)
    print('D3D9 shader checks passed')


def build_public():
    out = ARTIFACTS / 'dx9-shaders'
    out.mkdir(parents=True, exist_ok=True)
    for arch, compiler in [('x64', 'x86_64-w64-mingw32-gcc'), ('x86', 'i686-w64-mingw32-gcc')]:
        target = out / f'test-d3d9-shaders-{arch}.exe'
        run(container_command(compiler, '-std=c11', '-O2', '-g', '-static', '-D_WIN32_WINNT=0x0600',
             '-DWINVER=0x0600', '/workspace/tests/vista/test-d3d9-shaders.c',
             '-ld3d9', '-luser32', '-lm', '-Wl,--subsystem,console:6.0',
             '-o', container_path(target)))
        print(target)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', action='store_true')
    parser.add_argument('--build-public', action='store_true')
    parser.add_argument('--sanitize', action='store_true', help='instrument host converter and generator')
    args = parser.parse_args()
    if not (args.host or args.build_public):
        parser.error('select --host or --build-public')
    if args.host:
        host(args.sanitize)
    if args.build_public:
        build_public()
