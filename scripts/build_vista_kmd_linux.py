#!/usr/bin/env python3
"""Build Vista KMD using Clang's Microsoft ABI and verified WDK 7.1 inputs."""
import argparse
import json
from pathlib import Path
import subprocess
import xml.etree.ElementTree as ET

root = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--arch', choices=['x64', 'x86'], default='x64')
parser.add_argument('--verbose-trace', action='store_true', help='Enable expensive per-command serial tracing')
args = parser.parse_args()
kmd = root / 'triton-kmd'
wdk = root / 'driver/toolchains/wdk71/extracted/WinDDK/7600.16385.win7_wdk.100208-1538'
out = root / f'test-artifacts/linux-build/kmd-{args.arch}'
out.mkdir(parents=True, exist_ok=True)

def vfs_entry(path):
    if path.is_dir():
        return {'type': 'directory', 'name': path.name,
                'contents': [vfs_entry(p) for p in sorted(path.iterdir()) if p.is_dir() or p.suffix.lower() in ['.h', '.hpp', '.inl']]}
    return {'type': 'file', 'name': path.name, 'external-contents': str(path)}

roots = []
for path in [wdk / 'inc', kmd / 'VirtIO', kmd / 'viogpu']:
    item = vfs_entry(path)
    item['name'] = str(path)
    roots.append(item)
vfs = out / 'wdk-vfs.json'
vfs.write_text(json.dumps({'version': 0, 'case-sensitive': False, 'roots': roots}, indent=2))
flags = ['/Zc:alignedNew-', f'/FI{kmd}/viogpu/build-support/vista-clang.h', '/nologo', '/c', '/kernel', '/GS-', '/GR-', '/EHs-c-', '/O2', '/Z7',
         '/clang:-Wno-invalid-token-paste', '/clang:-ivfsoverlay', f'/clang:{vfs}',
         '/DWINVER=0x0600', '/D_WIN32_WINNT=0x0600', '/DNTDDI_VERSION=0x06000200',
         '/D_CRT_SECURE_CPP_OVERLOAD_SECURE_NAMES=0', '/D_VISTA_', '/DUNICODE', '/D_UNICODE']
if args.arch == 'x64':
    flags += ['--target=x86_64-pc-windows-msvc', '/D_AMD64_']
else:
    flags += ['--target=i686-pc-windows-msvc', '/D_X86_', '/Gz']
for path in [wdk/'inc/ddk', wdk/'inc/api', wdk/'inc/crt', kmd/'VirtIO',
             kmd/'viogpu/common', kmd/'viogpu/shared', kmd/'viogpu/viogpu3d']:
    flags += [f'/I{path}']
commands = []
objects = []
for project in [kmd/'VirtIO/VirtioLib.vcxproj', kmd/'viogpu/viogpu3d/viogpu3d.vcxproj']:
    for item in ET.parse(project).iter('{http://schemas.microsoft.com/developer/msbuild/2003}ClCompile'):
        if 'Include' not in item.attrib:
            continue
        source = (project.parent / item.attrib['Include'].replace('\\', '/')).resolve()
        obj = out / (source.stem + '.obj')
        extra = []
        if source.suffix == '.cpp':
            extra = [('/std:c++20' if source.name == 'viogpu_allocation.cpp' else '/std:c++14'), '/DVIOGPU_3D=1', '/DVIOGPU_TARGET_VISTA=1',
                     '/DDXGKDDI_INTERFACE_VERSION=DXGKDDI_INTERFACE_VERSION_VISTA_SP1',
                     '/DVIOGPU_SERIAL_TRACE', '/DVIOGPU_DIAGNOSTIC_NO_BREAKS', '/Zc:strictStrings-']
        if args.verbose_trace and source.suffix == '.cpp':
            extra += ['/DVIOGPU_VERBOSE_TRACE']
        command = ['clang', '--driver-mode=cl', *flags, *extra, f'/Fo{obj}', ('/Tp' if source.suffix == '.cpp' else '/Tc') + str(source)]
        commands.append(command)
        (out/'compile-commands.json').write_text(json.dumps(commands, indent=2))
        with (out/(source.stem+'.log')).open('w') as log:
            status = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
        if status.returncode:
            raise SystemExit(f'Compile failed: {source}; see {out/(source.stem+".log")}')
        objects.append(obj)
        print(f'Compiled {source.name}', flush=True)
intrinsics = kmd/'viogpu/build-support/vista-clang-intrinsics.c'
obj = out/'vista-clang-intrinsics.obj'
command = ['clang', '--driver-mode=cl', *flags, f'/Fo{obj}', f'/Tc{intrinsics}']
commands.append(command)
(out/'compile-commands.json').write_text(json.dumps(commands, indent=2))
with (out/'vista-clang-intrinsics.log').open('w') as log:
    subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
objects.append(obj)
print(f'Compiled {len(objects)} objects; building resource and linking.', flush=True)
# Decode the original UTF-16 resource into a build artifact; source stays intact.
rc_source = out / 'viogpu3d.rc'
rc_source.write_text((kmd/'viogpu/viogpu3d/viogpu3d.rc').read_text(encoding='utf-16').replace('..\\..\\build\\vendor.ver', str(kmd/'build/vendor.ver')))
rc_preprocessed = out/'viogpu3d.preprocessed.rc'
resource = out/'viogpu3d.res'
rc_flags = [f'/I{wdk}/inc/api', f'/I{wdk}/inc/ddk', f'/I{wdk}/inc/crt',
            f'/I{kmd}/viogpu/viogpu3d', f'/I{kmd}/build',
            '/DRC_INVOKED', '/DVER_OS=Vista', f'/DVER_ARCH={args.arch}',
            '/DRHEL_COPYRIGHT_YEARS=2026', '/DWINVER=0x0600', '/D_WIN32_WINNT=0x0600']
with (out/'resource.log').open('w') as log:
    subprocess.run(['clang', '--driver-mode=cl', '/nologo', '/P', f'/Fi{rc_preprocessed}',
                    '/clang:-ivfsoverlay', f'/clang:{vfs}', *rc_flags, f'/Tc{rc_source}'],
                   stdout=log, stderr=subprocess.STDOUT, check=True)
    subprocess.run(['llvm-rc-18', '/no-preprocess', f'/fo{resource}', str(rc_preprocessed)],
                   stdout=log, stderr=subprocess.STDOUT, check=True)
libdir = wdk/'lib/wlh'/('amd64' if args.arch == 'x64' else 'i386')
link = ['lld-link', '/nologo', '/nodefaultlib', '/driver', '/subsystem:native,6.00',
        '/entry:GsDriverEntry', f'/machine:{args.arch}', '/debug', '/release',
        f'/out:{out}/viogpu3d.sys', f'/pdb:{out}/viogpu3d.pdb',
        *map(str, objects), str(resource),
        *[str(libdir/(name+'.lib')) for name in ['ntoskrnl', 'hal', 'wmilib', 'displib', 'BufferOverflowK']]]
(out/'link-command.json').write_text(json.dumps(link, indent=2))
with (out/'link.log').open('w') as log:
    subprocess.run(link, stdout=log, stderr=subprocess.STDOUT, check=True)
subprocess.run(['python3', str(kmd/'viogpu/tools/check_vista_pe.py'), '--kind', 'kmd',
                '--arch', args.arch, str(out/'viogpu3d.sys')], check=True)
