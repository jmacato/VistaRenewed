#!/usr/bin/env python3
"""Compile actual Vista VERSIONINFO and validate parsed PE versions for both ABIs.

These resource-only fixtures are not functional kernel drivers. Final KMD
build/package gates still audit the real viogpu3d.sys.
"""
from pathlib import Path
import json
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
KMD = ROOT / 'triton-kmd'
EVIDENCE = ROOT / 'test-artifacts/d3d10-version'
EVIDENCE.mkdir(parents=True, exist_ok=True)
WDK = str(ROOT / 'driver/toolchains/wdk71/extracted/WinDDK/7600.16385.win7_wdk.100208-1538')
LLVM_RC = shutil.which('llvm-rc-18') or shutil.which('llvm-rc')


def guest(path):
    return str(path)


def run(command):
    result = subprocess.run(command, capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr


with tempfile.TemporaryDirectory(prefix='version-', dir=EVIDENCE) as temporary:
    work = Path(temporary)
    # Build this overlay from local absolute paths. Production KMD builds may
    # have last run in /workspace inside the container; their overlay is not
    # a portable test input on the host.
    def entry(path):
        if path.is_dir():
            return {'type': 'directory', 'name': path.name,
                    'contents': [entry(p) for p in sorted(path.iterdir())
                                 if p.is_dir() or p.suffix.lower() in ('.h', '.hpp', '.inl')]}
        return {'type': 'file', 'name': path.name, 'external-contents': str(path)}
    roots = []
    for directory in (Path(WDK) / 'inc', KMD / 'VirtIO', KMD / 'viogpu'):
        root = entry(directory)
        root['name'] = str(directory)
        roots.append(root)
    overlay = work / 'wdk-vfs.json'
    overlay.write_text(json.dumps({'version': 0, 'case-sensitive': False, 'roots': roots}))
    source = work / 'viogpu3d.rc'
    source.write_text((KMD / 'viogpu/viogpu3d/viogpu3d.rc').read_text(encoding='utf-16').replace(
        '..\\..\\build\\vendor.ver', str(KMD / 'build/vendor.ver')))
    for arch in ('x64', 'x86'):
        for vista in (True, False):
            name = f'{arch}-' + ('vista' if vista else 'vendor-baseline')
            preprocessed, resource, image = [work / (name + ext) for ext in ('.rc', '.res', '.sys')]
            flags = [f'/I{WDK}/inc/api', f'/I{WDK}/inc/ddk', f'/I{WDK}/inc/crt',
                     f'/I{KMD}/viogpu/viogpu3d', f'/I{KMD}/build',
                     '/DRC_INVOKED', '/DVER_OS=Vista', f'/DVER_ARCH={arch}',
                     '/DRHEL_COPYRIGHT_YEARS=2026', '/DWINVER=0x0600', '/D_WIN32_WINNT=0x0600']
            if vista:
                flags.append('/DVIOGPU_TARGET_VISTA=1')
            run(['clang', '--driver-mode=cl', '/nologo', '/P', '/Fi' + guest(preprocessed),
                 '/clang:-ivfsoverlay', f'/clang:{overlay}',
                 *flags, '/Tc' + guest(source)])
            run([LLVM_RC, '/no-preprocess', '/fo' + guest(resource), '--', guest(preprocessed)])
            run(['lld-link', '/nologo', '/nodefaultlib', '/noentry', '/dll', '/driver',
                 '/subsystem:native,6.00', f'/machine:{arch}', '/out:' + guest(image), guest(resource)])
            audit = subprocess.run(['python3', str(KMD / 'viogpu/tools/check_vista_pe.py'),
                                    '--kind', 'kmd', '--arch', arch, str(image)], capture_output=True, text=True)
            if vista:
                assert audit.returncode == 0, audit.stdout + audit.stderr
            else:
                assert audit.returncode != 0 and 'expected 7.15.1.0' in audit.stderr, audit.stdout + audit.stderr
        print(f'D3D10 {arch} VERSIONINFO and generic-version negative control passed')
