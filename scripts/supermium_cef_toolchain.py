#!/usr/bin/env python3
"""Local MSVC/SDK smoke check; not a Chromium build or Vista runtime test."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / 'third_party/supermium-cef'
TOOLCHAIN = BASE / 'toolchain'
BUILD = ROOT / 'build/supermium-cef-toolchain'
MANIFEST_SHA256 = 'b60efac8768e31b4b0bb74d312a3fd3145de9bef7f794c050d498d079e540e11'
DOWNLOADER_COMMIT = '514f8ea34842cd6d831804d0e9658d3a32870ae1'


def install(accept_license):
    if not accept_license:
        raise RuntimeError('Installation requires explicit --accept-license consent')
    manifest = BASE / '17.14.40.manifest'
    if hashlib.sha256(manifest.read_bytes()).hexdigest() != MANIFEST_SHA256:
        raise RuntimeError('Package manifest checksum mismatch')
    revision = subprocess.check_output(
        ['git', '-C', str(BASE / 'msvc-wine'), 'rev-parse', 'HEAD'], text=True).strip()
    if revision != DOWNLOADER_COMMIT:
        raise RuntimeError('Downloader revision mismatch')
    args = ['python3', 'msvc-wine/vsdownload.py', '--manifest', manifest.name,
            '--major', '17', '--msvc-version', '17.13', '--sdk-version', '10.0.26100',
            '--architecture', 'x64', 'x86', '--accept-license', '--dest', 'toolchain',
            '--cache', 'toolchain-cache']
    if shutil.which('msiextract'):
        subprocess.run(args, cwd=BASE, check=True)
    else:
        subprocess.run([str(ROOT / 'scripts/dev-container.sh'), 'run', 'sh', '-c',
                        'cd /workspace/third_party/supermium-cef && exec "$@"', 'builder', *args], check=True)
    debugger_args = ['python3', 'msvc-wine/vsdownload.py', '--manifest', manifest.name,
                     '--major', '17', '--accept-license', '--only-unpack',
                     '--dest', 'debugger-tools', '--cache', 'toolchain-cache',
                     'Microsoft.VisualStudio.Debugger.DbgHelp.Win8']
    if shutil.which('msiextract'):
        subprocess.run(debugger_args, cwd=BASE, check=True)
    else:
        subprocess.run([str(ROOT / 'scripts/dev-container.sh'), 'run', 'sh', '-c',
                        'cd /workspace/third_party/supermium-cef && exec "$@"', 'builder', *debugger_args], check=True)


def only(paths):
    paths = list(paths)
    if len(paths) != 1:
        raise RuntimeError(f'Expected one versioned directory, found {paths}')
    return paths[0]


def overlay_directory(path):
    contents = []
    for child in sorted(path.iterdir()):
        if child.is_dir():
            contents.append(overlay_directory(child))
        elif child.is_file():
            contents.append({'type': 'file', 'name': child.name,
                             'external-contents': str(child)})
    return {'type': 'directory', 'name': path.name, 'contents': contents}


def verify():
    vc = only((TOOLCHAIN / 'VC/Tools/MSVC').glob('14.43.*'))
    sdk = only((TOOLCHAIN / 'Windows Kits/10/Include').glob('10.0.26100.*'))
    sdk_lib = TOOLCHAIN / 'Windows Kits/10/Lib' / sdk.name
    BUILD.mkdir(parents=True, exist_ok=True)
    includes = [vc / 'include', vc / 'atlmfc/include',
                *(sdk / part for part in ('ucrt', 'shared', 'um', 'winrt'))]
    roots = []
    for include in includes:
        node = overlay_directory(include)
        node['name'] = str(include)
        roots.append(node)
    overlay = BUILD / 'sdk-overlay.json'
    overlay.write_text(json.dumps({'version': 0, 'case-sensitive': False,
                                   'roots': roots}))
    results = []
    # Materialize the pinned upstream setup code as a build artifact. Do not
    # alter the sparse source checkout just to run this environment probe.
    source_pin = '82756ad44eee3166e8ac4fa7605632690ac70bc9'
    for relative in ('build/toolchain/win/setup_toolchain.py', 'build/gn_helpers.py'):
        data = subprocess.check_output(['git', '-C', str(BASE / 'src'),
                                        'show', source_pin + ':' + relative])
        destination = BUILD / 'upstream' / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(data)
    for arch, triple, machine in (('x64', 'x86_64-pc-windows-msvc', 0x8664),
                                   ('x86', 'i686-pc-windows-msvc', 0x14c)):
        obj = BUILD / f'smoke-{arch}.obj'
        exe = BUILD / f'smoke-{arch}.exe'
        compiler = ['clang-cl', '--target=' + triple, '/c', '/MT', '/EHsc',
                    '/std:c++17', '/W4', '/WX', '/Z7', '-Wno-nonportable-include-path',
                    '/clang:-ivfsoverlay', '/clang:' + str(overlay)]
        for include in includes:
            compiler.extend(['/imsvc', str(include)])
        subprocess.run([*compiler, str(ROOT / 'packaging/supermium-cef/supermium_cef_toolchain_smoke.cc'),
                        '/Fo' + str(obj)], check=True)
        libraries = [vc / 'lib' / arch, vc / 'atlmfc/lib' / arch, sdk_lib / 'ucrt' / arch,
                     sdk_lib / 'um' / arch]
        # LLD's file lookup does not use Clang's VFS overlay. Preserve the
        # downloaded libraries and expose lowercase aliases in the build dir.
        aliases = BUILD / ('lib-' + arch)
        aliases.mkdir(exist_ok=True)
        for library_dir in libraries:
            for library in library_dir.iterdir():
                if library.suffix.lower() not in ('.lib', '.pdb'):
                    continue
                alias = aliases / library.name.lower()
                if alias.is_symlink():
                    if alias.resolve() != library.resolve():
                        raise RuntimeError(f'Conflicting library alias: {alias}')
                elif alias.exists():
                    raise RuntimeError(f'Refusing to overwrite {alias}')
                else:
                    alias.symlink_to(library)
        library_root = overlay_directory(aliases)
        library_root['name'] = str(aliases)
        library_overlay = BUILD / f'library-overlay-{arch}.json'
        library_overlay.write_text(json.dumps({'version': 0, 'case-sensitive': False,
                                               'roots': [library_root]}))
        subprocess.run(['lld-link', str(obj), '/out:' + str(exe),
                        '/subsystem:console', '/debug', '/WX', '/libpath:' + str(aliases),
                        '/vfsoverlay:' + str(library_overlay),
                        'kernel32.lib', 'Cfgmgr32.lib'], check=True)
        data = exe.read_bytes()
        pe = struct.unpack_from('<I', data, 0x3c)[0]
        if data[:2] != b'MZ' or data[pe:pe + 4] != b'PE\0\0':
            raise RuntimeError(f'{arch}: not a PE executable')
        if struct.unpack_from('<H', data, pe + 4)[0] != machine:
            raise RuntimeError(f'{arch}: incorrect machine type')
        results.append({'arch': arch, 'sha256': hashlib.sha256(data).hexdigest()})
        print(f'{arch}: Windows C++ executable compiled and linked', flush=True)
        sdk_root = TOOLCHAIN / 'Windows Kits/10'
        vc_bin = vc / 'bin/Hostx64' / arch
        environment = {
            'VSINSTALLDIR': [['.']],
            'INCLUDE': [list(p.relative_to(TOOLCHAIN).parts) for p in includes],
            'LIB': [list(p.relative_to(TOOLCHAIN).parts) for p in libraries],
            'PATH': [list(vc_bin.relative_to(TOOLCHAIN).parts),
                     ['Windows Kits', '10', 'bin', sdk.name, 'x64']],
        }
        (sdk_root / 'bin' / f'SetEnv.{arch}.json').write_text(
            json.dumps({'env': environment}, indent=2) + '\n')
        setup = subprocess.run(
            [sys.executable, str(BUILD / 'upstream/build/toolchain/win/setup_toolchain.py'),
             str(TOOLCHAIN), str(sdk_root), str(vc_bin), 'win', arch, 'none'],
            env=dict(os.environ, DEPOT_TOOLS_WIN_TOOLCHAIN='1'),
            cwd=BUILD, text=True, capture_output=True, check=True)
        if 'vc_bin_dir = ' not in setup.stdout or '/winsysroot' not in setup.stdout:
            raise RuntimeError('Chromium environment probe returned incomplete configuration')
        (BUILD / f'chromium-setup-{arch}.txt').write_text(setup.stdout)
        print(f'{arch}: pinned Chromium environment setup accepted the toolchain', flush=True)
    (BUILD / 'receipt.json').write_text(json.dumps(
        {'msvc': vc.name, 'sdk': sdk.name, 'artifacts': results,
         'runtime_tested': False}, indent=2) + '\n')
    print('SUPERMIUM CEF TOOLCHAIN SMOKE PASSED')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=['install', 'verify'])
    parser.add_argument('--accept-license', action='store_true')
    args = parser.parse_args()
    if args.command == 'install':
        install(args.accept_license)
    else:
        verify()
