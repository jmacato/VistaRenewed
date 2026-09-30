#!/usr/bin/env python3
"""Run production D3D9 query and pipeline regressions on the host GPU."""
import argparse
import os
from pathlib import Path
import subprocess
import sys

from test_config import ARTIFACTS, HOST_PREFIX, QEMU_BUILD, native_headers, native_libraries, sdk_header, container_command, container_path, vista_compile_database

ROOT = Path(__file__).resolve().parents[2]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', action='store_true')
    parser.add_argument('--build-public', action='store_true')
    args = parser.parse_args()
    if not (args.host or args.build_public):
        parser.error('select --host or --build-public')
    if args.host:
        for script in ('tests/vista/test-d3d9-pipeline.py', 'tests/run-native-blit.py'):
            subprocess.run([sys.executable, str(ROOT / script)] + (['--native'] if script.endswith('test-d3d9-pipeline.py') else []), cwd=ROOT, check=True)
        print('D3D9 pipeline checks passed')
    if args.build_public:
        out = ARTIFACTS / 'dx9-pipeline'
        out.mkdir(parents=True, exist_ok=True)
        for arch, prefix in [('x64', 'x86_64'), ('x86', 'i686')]:
            target = out / f'test-d3d9-pipeline-{arch}.exe'
            subprocess.run(container_command(prefix + '-w64-mingw32-gcc', '-std=c11', '-O2', '-g', '-static',
                '-Wall', '-Wextra', '-Werror', '-D_WIN32_WINNT=0x0600', '-DWINVER=0x0600',
                '/workspace/tests/vista/test-d3d9-pipeline.c', '-ld3d9', '-luser32',
                '-Wl,--subsystem,console:6.0', '-o', container_path(target)), check=True)
            print(target)
        print('D3D9 public pipeline builds passed')

if __name__ == '__main__':
    main()
