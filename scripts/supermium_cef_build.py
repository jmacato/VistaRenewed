#!/usr/bin/env python3
"""Explicit stages for the experimental Supermium/CEF cross-build.

No stage installs or registers the result in the Vista VM.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import time

from supermium_cef_source import BASE, ROOT, git, verify

SRC = BASE / 'src'
OUT = SRC / 'out/Release_CEF_Vista_x64'


def environment():
    return dict(os.environ, PATH=os.pathsep.join((str(BASE / 'host-tools/usr/bin'),
                                                str(BASE / 'depot_tools'), os.environ['PATH'])),
                DEPOT_TOOLS_UPDATE='0', DEPOT_TOOLS_WIN_TOOLCHAIN='1',
                GYP_MSVS_VERSION='2022')


def run(args, cwd=SRC, env=None, **kwargs):
    print('+', ' '.join(map(str, args)), flush=True)
    subprocess.run(list(map(str, args)), cwd=cwd, env=env or environment(), check=True, **kwargs)


def patch_sources():
    verify()
    report = json.loads((ROOT / 'build/supermium-cef-merged-audit.json').read_text())
    if report['conflicts'] or report['applies'] != 107:
        raise RuntimeError('Root patch audit has not passed')
    combined = ROOT / 'build/supermium-cef-root.patch'
    # Idempotent, and refuse partial/foreign edits instead of resetting them.
    if git(SRC, 'apply', '--reverse', '--check', str(combined)).returncode:
        run(['git', 'apply', '--check', combined])
        run(['git', 'apply', combined])
    for name, relative in [('v8_build', 'v8'), ('tarball_gclient', 'third_party/depot_tools'),
                            ('angle_commit_config', 'third_party/angle')]:
        repo = SRC / relative
        patch = BASE / 'cef/patch/patches' / (name + '.patch')
        if not (repo / '.git').exists():
            raise RuntimeError(f'Missing dependency checkout: {repo}')
        if git(repo, 'apply', '-p0', '--reverse', '--check', str(patch)).returncode:
            run(['git', 'apply', '-p0', '--check', patch], cwd=repo)
            run(['git', 'apply', '-p0', patch], cwd=repo)
    cef_link = SRC / 'cef'
    if not cef_link.exists():
        cef_link.symlink_to('../cef', target_is_directory=True)
    elif cef_link.resolve() != (BASE / 'cef').resolve():
        raise RuntimeError('Unexpected src/cef directory')
    print('SUPERMIUM CEF PATCHES APPLIED')


def configure():
    from supermium_cef_toolchain import verify as check_toolchain, TOOLCHAIN, BUILD
    check_toolchain()
    # CEF discovers Chromium relative to its real source path, so it must
    # physically live inside src. Keep the old audit path as an alias.
    cef_link = SRC / 'cef'
    old_cef = BASE / 'cef'
    if cef_link.is_symlink() and not old_cef.is_symlink():
        if cef_link.resolve() != old_cef.resolve():
            raise RuntimeError('Refusing to replace unexpected CEF link')
        cef_link.unlink()
        old_cef.rename(cef_link)
        old_cef.symlink_to('src/cef', target_is_directory=True)
    cross_patch = ROOT / 'scripts/supermium_cef_cross.patch'
    if git(SRC, 'apply', '--recount', '--reverse', '--check', str(cross_patch)).returncode:
        run(['git', 'apply', '--recount', '--check', cross_patch])
        run(['git', 'apply', '--recount', cross_patch])
    sdk = TOOLCHAIN / 'Windows Kits/10'
    host_patch = ROOT / 'scripts/supermium_cef_host.patch'
    if git(SRC, 'apply', '--recount', '--reverse', '--check', str(host_patch)).returncode:
        run(['git', 'apply', '--recount', '--check', host_patch])
        run(['git', 'apply', '--recount', host_patch])
    compile_patch = ROOT / 'scripts/supermium_cef_compile.patch'
    if git(SRC, 'apply', '--recount', '--reverse', '--check', str(compile_patch)).returncode:
        run(['git', 'apply', '--recount', '--check', compile_patch])
        run(['git', 'apply', '--recount', compile_patch])
    rc_patch = ROOT / 'scripts/supermium_cef_rc.patch'
    if git(SRC, 'apply', '--recount', '--reverse', '--check', str(rc_patch)).returncode:
        run(['git', 'apply', '--recount', '--check', rc_patch])
        run(['git', 'apply', '--recount', rc_patch])
    cdm_patch = ROOT / 'scripts/supermium_cef_cdm.patch'
    if git(SRC, 'apply', '--reverse', '--check', str(cdm_patch)).returncode:
        run(['git', 'apply', '--check', cdm_patch])
        run(['git', 'apply', cdm_patch])
    # VS manifest debugger package is a build prerequisite, not a claim of
    # Vista runtime compatibility. Keep its origin separate from SDK payloads.
    for name in ('dbghelp.dll', 'dbgcore.dll'):
        source = BASE / 'debugger-tools/Common7/IDE' / name
        destination = sdk / 'Debuggers/x64' / name
        if not source.is_file():
            raise RuntimeError('Missing Microsoft VS debugger package: ' + str(source))
        payload = source.read_bytes()
        pe = struct.unpack_from('<I', payload, 0x3c)[0]
        if payload[pe:pe + 4] != b'PE\0\0' or struct.unpack_from('<H', payload, pe + 4)[0] != 0x8664:
            raise RuntimeError('Expected x64 debugger DLL: ' + str(source))
        destination.parent.mkdir(parents=True, exist_ok=True)
        if destination.exists() and destination.read_bytes() != payload:
            raise RuntimeError('Refusing to replace different debugger DLL: ' + str(destination))
        shutil.copy2(source, destination)
    runtime_base = TOOLCHAIN / 'VC/Redist/MSVC/14.42.34433'
    runtimes = [str(runtime_base / arch / 'Microsoft.VC143.CRT') for arch in ('x64', 'x86')]
    (SRC / 'build/win_toolchain.json').write_text(json.dumps({
        'path': str(TOOLCHAIN), 'version': '2022', 'win_sdk': str(sdk),
        'wdk': '', 'runtime_dirs': runtimes + ['Arm64Unused'],
    }, indent=2) + '\n')
    run([sys.executable, 'tools/version_manager.py', '-u', '--fast-check'], cwd=SRC / 'cef')
    # Supermium commits need not have Chromium's Change-Id footer. Record the
    # actual fork revision, without inventing an upstream commit position.
    run([sys.executable, 'build/util/lastchange.py', '--filter=',
         '-o', 'build/util/LASTCHANGE'])
    run([sys.executable, 'build/util/lastchange.py', '--filter=',
         '-m', 'GPU_LISTS_VERSION', '--revision-id-only',
         '--header', 'gpu/config/gpu_lists_version.h'])
    args = {
        'target_os': 'win', 'target_cpu': 'x64', 'is_debug': False,
        'is_component_build': False, 'is_official_build': False,
        'symbol_level': 1, 'blink_symbol_level': 0, 'v8_symbol_level': 0,
        'chrome_pgo_phase': 0, 'use_thin_lto': False, 'use_siso': False,
        'concurrent_links': 1,
        'use_remoteexec': False, 'use_sysroot': True,
        'enable_cef': True, 'clang_use_chrome_plugins': False,
        'enable_background_mode': False, 'enable_resource_allowlist_generation': False,
        'enable_downgrade_processing': False, 'enable_glic': False,
        'optimize_webui': True, 'enable_widevine': True,
        'enable_cdm_host_verification': True, 'enable_cdm_storage_id': True,
        'enable_rlz': True,
        'alternate_cdm_storage_id_key': 'triton-supermium-cef-vista-development-01',
        'visual_studio_path': str(TOOLCHAIN), 'visual_studio_version': '2022',
        'visual_studio_runtime_dirs': ':'.join(runtimes),
        'windows_sdk_path': str(sdk), 'windows_sdk_version': '10.0.26100.0',
        'triton_sdk_vfs_overlay': str(BUILD / 'sdk-overlay.json'),
        'triton_sdk_lib_root': str(BUILD),
        'triton_rc_sdk_overlay': str(BUILD / 'sdk-overlay.json'),
        'triton_rc_vctools_version': '14.43.34808',
    }
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / 'args.gn').write_text(''.join(f'{key} = {json.dumps(value)}\n'
                                      for key, value in args.items()))
    run([SRC / 'buildtools/linux64/gn', 'gen', OUT])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('stage', choices=['sync', 'patch', 'hooks', 'host-tools', 'configure', 'build'])
    parser.add_argument('--jobs', type=int, default=6)
    args = parser.parse_args()
    if args.jobs < 1 or args.jobs > 16:
        parser.error('jobs must be between 1 and 16')
    if args.stage == 'sync':
        verify()
        if shutil.disk_usage(BASE).free < 60 * 1024**3:
            raise RuntimeError('Less than 60 GiB free; refusing dependency synchronization')
        run([BASE / 'depot_tools/gclient', 'sync', '--nohooks', '--noprehooks',
             '--no-history', '--shallow', '--jobs=' + str(args.jobs),
             '--output-json=sync-result.json'], cwd=BASE)
    elif args.stage == 'patch':
        patch_sources()
    elif args.stage == 'hooks':
        run([BASE / 'depot_tools/gclient', 'runhooks'], cwd=BASE,
            env=dict(environment(), DEPOT_TOOLS_WIN_TOOLCHAIN='0'))
    elif args.stage == 'host-tools':
        prefix = [str(ROOT / 'scripts/dev-container.sh'), 'run', 'sh', '-c',
                  'cd /workspace/third_party/supermium-cef && exec "$@"', 'builder']
        run([*prefix, 'apt-get', 'download', 'gperf=3.1-1build1'], cwd=BASE)
        run([*prefix, 'dpkg-deb', '-x', 'gperf_3.1-1build1_amd64.deb', 'host-tools'], cwd=BASE)
        run([BASE / 'host-tools/usr/bin/gperf', '--version'], cwd=BASE)
    elif args.stage == 'configure':
        configure()
    else:
        if not (OUT / 'build.ninja').is_file():
            raise RuntimeError('GN configuration is not ready: ' + str(OUT))
        if shutil.disk_usage(OUT).free < 50 * 1024**3:
            raise RuntimeError('Less than 50 GiB free before build')
        log_path = ROOT / 'build/supermium-cef-compile.log'
        if log_path.exists():
            # Preserve diagnostics across incremental retries.
            previous = log_path.with_name(f'supermium-cef-compile-{time.time_ns()}.log')
            log_path.rename(previous)
        command = [str(SRC / 'third_party/ninja/ninja'), '-C', str(OUT),
                   '-j', str(args.jobs), 'libcef', 'cefclient']
        print('Build log:', log_path, flush=True)
        with log_path.open('w') as log:
            process = subprocess.Popen(command, cwd=SRC, env=environment(),
                                       stdout=log, stderr=subprocess.STDOUT,
                                       start_new_session=True)
            while process.poll() is None:
                stop_reason = None
                if shutil.disk_usage(OUT).free < 15 * 1024**3:
                    stop_reason = 'preserve 15 GiB free space'
                meminfo = Path('/proc/meminfo')
                if meminfo.exists():
                    available = next(int(line.split()[1]) for line in
                                     meminfo.read_text().splitlines()
                                     if line.startswith('MemAvailable:'))
                    if available < 2 * 1024**2:
                        stop_reason = 'preserve 2 GiB available host memory'
                if stop_reason:
                    import signal
                    os.killpg(process.pid, signal.SIGTERM)
                    process.wait()
                    raise RuntimeError('Build stopped to ' + stop_reason)
                time.sleep(5)
        if process.returncode:
            with log_path.open() as log:
                from collections import deque
                print(''.join(deque(log, maxlen=60)))
            raise RuntimeError(f'CEF build failed with exit {process.returncode}; see {log_path}')
        for name in ('libcef.dll', 'cefclient.exe'):
            if not (OUT / name).is_file():
                raise RuntimeError('Build did not produce ' + name)
        print('SUPERMIUM CEF WINDOWS BUILD PRODUCED')


if __name__ == '__main__':
    main()
