#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Prepare and assess the real SuperTuxKart 1.5 Vista game workload.

Preparation is host-only. The shared VM owner runs the emitted batch files.
Assessment never substitutes physics ticks or a menu for rendered race frames.
"""
from __future__ import annotations

import argparse
import csv
import contextlib
import hashlib
import importlib.util
import io
import json
import math
import os
from pathlib import Path, PureWindowsPath
import re
import shutil
import statistics
import struct
import subprocess
import sys
import tempfile
import urllib.request
import zipfile
import xml.etree.ElementTree as ET

from test_config import ARTIFACTS, HOST_PREFIX, QEMU_BUILD, native_headers, native_libraries, sdk_header, container_command, container_path, vista_compile_database

ROOT = Path(__file__).resolve().parents[2]
WORK = ARTIFACTS / 'kart'
VERSION = '1.5'
ARCHIVE = 'SuperTuxKart-1.5-win.zip'
ARCHIVE_SHA256 = '9df7e2d67e8562127a3bb633030f1bd4ee77fa9e7f0ae110897473025af8acdc'
URL = f'https://github.com/supertuxkart/stk-code/releases/download/{VERSION}/{ARCHIVE}'
SOURCE_COMMIT = '1fb491f507216c5d181ccd85f29ff08eca003827'
REPLAY_SHA256 = '0049a18428049c02dcfdb6e2944c7ba7196f2a0017f239d0305ed12fb15614df'
REPLAY_LAST_SECONDS = 37.137501
EXECUTABLE_SHA256 = {
    'x64': 'cbbc2e03a79c669c02878a7345e46094657a7a8c4f4fd97496f909a0bcd5a650',
    'x86': 'f5d164dd52242c8aa356dc8b141579680290e67dc1a1dea0ba02667ffd5a37ca',
}
BENCHMARK_WARMUP_SECONDS = 15
BENCHMARK_TAIL_SECONDS = 2
BENCHMARK_FILES = ('stdout.log', 'version.log', 'version-exit-code.txt',
                   'config.xml', 'exit-code.txt',
                   'stdout.log.profile-black_forest-cpu-0.csv')
# Microsoft June 2010 redist: D3DX loads the compiler dynamically; PE imports
# alone cannot establish this dependency closure.
DIRECTX_DLLS = {
    'x64': {
        'd3dx9_43.dll': '84b900dbd7fa978d6e0caee26fc54f2f61d92c9c75d10b35f00e3e82cd1d67b4',
        'd3dcompiler_43.dll': '44c3a7e330b54a35a9efa015831392593aa02e7da1460be429d17c3644850e8a',
    },
    'x86': {
        'd3dx9_43.dll': '0b28546be22c71834501f7d7185ede5d79742457331c7ee09efc14490dd64f5f',
        'd3dcompiler_43.dll': '2f23182ec6f4889397ac4bf03d62536136c5bdba825c7d2c4ef08c827f3a8a1c',
    },
}
SYSTEM_DLLS = {
    'advapi32.dll', 'dnsapi.dll', 'gdi32.dll', 'hid.dll', 'imm32.dll',
    'iphlpapi.dll', 'kernel32.dll', 'msvcrt.dll', 'ntdll.dll', 'ole32.dll',
    'oleaut32.dll', 'opengl32.dll', 'rpcrt4.dll', 'setupapi.dll', 'shell32.dll',
    'user32.dll', 'version.dll', 'winmm.dll', 'ws2_32.dll', 'wsock32.dll',
}
CONFIG = '''<?xml version="1.0"?>
<stkconfig version="8">
  <Video real_width="1280" real_height="720" width="1280" height="720"
    fullscreen="false" render_driver="directx9" show_fps="true"
    enable_dynamic_lights="false" enable_high_definition_textures="2"
    enable_texture_compression="true" max_texture_size="512"
    old_driver_popup="false" scale_rtts_factor="1" />
  <GFX particles-effecs="2" animated-characters="true" geometry-level="2"
    anisotropic="0" swap-interval-vsync="0" christmas-mode="2"
    easter-ear-mode="2" />
  <enable_internet value="2" />
</stkconfig>
'''


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def pe_class():
    path = ROOT / 'triton-kmd/viogpu/tools/check_vista_pe.py'
    spec = importlib.util.spec_from_file_location('kart_pe_checker', path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module.PeImage


def pe_audit(directory, arch):
    """Check file-machine/subsystem/dependency closure, not a Vista runtime proof."""
    image_class = pe_class()
    files = {p.name.lower(): p for p in directory.iterdir()
             if p.suffix.lower() in ('.exe', '.dll')}
    require('supertuxkart.exe' in files, 'official game executable is missing')
    reports = []
    for name, path in sorted(files.items()):
        image = image_class(path)
        require(image.machine == {'x64': 0x8664, 'x86': 0x14c}[arch],
                f'{name}: wrong architecture')
        subsystem = (image.subsystem_major, image.subsystem_minor)
        require(subsystem <= (6, 0), f'{name}: PE subsystem newer than Vista')
        imports = image.imports()
        missing = imports - files.keys() - SYSTEM_DLLS
        require(not missing, f'{name}: unsupported/unbundled DLL imports: {missing}')
        reports.append({'file': name, 'sha256': sha256(path),
                        'subsystem': list(subsystem), 'imports': sorted(imports)})
    return reports


def official(work, download=False):
    archive = work / ARCHIVE
    if download and not archive.exists():
        work.mkdir(parents=True, exist_ok=True)
        temporary = archive.with_suffix('.partial')
        with urllib.request.urlopen(URL) as response, temporary.open('wb') as output:
            shutil.copyfileobj(response, output)
        require(sha256(temporary) == ARCHIVE_SHA256, 'official archive digest mismatch')
        temporary.replace(archive)
    require(archive.exists(), f'missing {archive}; run prepare --download')
    require(sha256(archive) == ARCHIVE_SHA256, 'official archive digest mismatch')
    destination = work / 'official'
    top = destination / f'SuperTuxKart-{VERSION}-win'
    if not top.exists():
        with zipfile.ZipFile(archive) as package:
            for member in package.infolist():
                target = (destination / member.filename).resolve()
                require(target.is_relative_to(destination.resolve()),
                        'archive member escapes extraction directory')
            package.extractall(destination)
    # Match every staged file to the authenticated archive, including assets.
    return top


def verify_selected(top, archive):
    selected = []
    with zipfile.ZipFile(archive) as package:
        for item in package.infolist():
            relative = Path(item.filename).relative_to(top.name)
            if item.is_dir() or not relative.parts or relative.parts[0] != 'stk-code':
                continue
            if relative.parts[1] not in ('data', 'build-x86_64', 'build-i686'):
                continue
            source = top / relative
            require(source.is_file(), f'missing official file: {relative}')
            expected = hashlib.sha256(package.read(item)).hexdigest()
            require(sha256(source) == expected, f'official file changed: {relative}')
            selected.append((relative, expected))
    require(len(selected) > 1000, 'incomplete official game/assets selection')
    return selected


def build_probes(work):
    for arch, triplet in [('x64', 'x86_64'), ('x86', 'i686')]:
        output = work / f'd3d9-probe-{arch}.dll'
        args = [f'{triplet}-w64-mingw32-gcc', '-std=c11', '-O2', '-Wall',
                '-Wextra', '-Werror', '-D_WIN32_WINNT=0x0600', '-DWINVER=0x0600',
                '-shared', '/workspace/tests/vista/kart-present-probe.c',
                '/workspace/tests/vista/kart-present-probe.def',
                '-Wl,--enable-stdcall-fixup,--kill-at,--major-subsystem-version,6,--minor-subsystem-version,0',
                '-o', container_path(output)]
        subprocess.run(container_command(*args), check=True)
        image = pe_class()(output)
        require(image.machine == {'x64': 0x8664, 'x86': 0x14c}[arch],
                'measurement proxy has the wrong architecture')
        require(image.exports() == {'Direct3DCreate9'}, 'unexpected proxy export ABI')
        require(image.imports() <= {'kernel32.dll', 'msvcrt.dll'},
                'measurement proxy has an unexpected runtime dependency')
        require((image.subsystem_major, image.subsystem_minor) == (6, 0),
                'measurement proxy does not target Vista')


def batch(arch, mode, instrumented=True):
    triplet = {'x64': 'x86_64', 'x86': 'i686'}[arch]
    label = f'{mode}-{arch}' + ('' if instrumented else '-control')
    workload = ('--benchmark' if mode == 'benchmark' else
                '--track=black_forest --profile-laps=3 --numkarts=8 --difficulty=2')
    lines = [
        '@echo off', 'setlocal EnableExtensions',
        f'set "KART_RUN=C:\\triton-kart-results\\{label}-%RANDOM%-%RANDOM%"',
        'if exist "%KART_RUN%" exit /b 2',
        'set "SUPERTUXKART_SAVEDIR=%KART_RUN%"',
        'set "KART_CONFIG=%KART_RUN%\\config-0.10"',
        'mkdir "%KART_CONFIG%" || exit /b 3',
        'copy /y "%~dp0config.xml" "%KART_CONFIG%\\config.xml" >nul || exit /b 4',
        f'pushd "%~dp0game\\stk-code\\build-{triplet}\\bin" || exit /b 5',
        # --version exits before STK processes --stdout or opens its file log.
        # Capture inherited stdout/stderr and wait for the actual child exit.
        'start /b /wait "" supertuxkart.exe --version > "%KART_CONFIG%\\version.log" 2>&1',
        'set "KART_VERSION_EXIT=%ERRORLEVEL%"',
        '> "%KART_CONFIG%\\version-exit-code.txt" echo %KART_VERSION_EXIT%',
        'if not "%KART_VERSION_EXIT%"=="0" (popd & exit /b 6)',
        'findstr /L /C:"SuperTuxKart, 1.5." "%KART_CONFIG%\\version.log" >nul || (popd & exit /b 10)',
        'set "TRITON_KART_TIMINGS=%KART_CONFIG%\\present.csv"',
    ]
    if not instrumented:
        lines += ['if exist d3d9.dll.disabled (popd & exit /b 7)',
                  'move d3d9.dll d3d9.dll.disabled >nul || exit /b 8']
    lines += [
        'echo KART_RESULT_DIR=%KART_CONFIG%',
        f'start /wait "" supertuxkart.exe --render-driver=directx9 --screensize=1280x720 --windowed --seed=20260926 --xmas=2 --easter=2 --disable-addon-karts --disable-addon-tracks --no-console-log --log=1 {workload}',
        'set "KART_EXIT=%ERRORLEVEL%"',
        '> "%KART_CONFIG%\\exit-code.txt" echo %KART_EXIT%',
    ]
    if not instrumented:
        lines += ['move d3d9.dll.disabled d3d9.dll >nul || exit /b 9']
    lines += ['popd', 'echo KART_EXIT=%KART_EXIT%', 'exit /b %KART_EXIT%']
    return '\r\n'.join(lines) + '\r\n'


def directx_dependencies(directory, stage=None):
    """Validate supplied Microsoft DLLs; optionally stage them app-local."""
    reports = {}
    for arch, triplet in [('x64', 'x86_64'), ('x86', 'i686')]:
        reports[arch] = {}
        for name, expected in DIRECTX_DLLS[arch].items():
            source = directory / arch / name
            require(source.is_file(),
                    f'missing {source}; extract both June 2010 D3DX9_43 and '
                    'D3DCompiler_43 CABs from the Microsoft DirectX redist')
            require(sha256(source) == expected, f'{source}: DirectX DLL digest mismatch')
            image = pe_class()(source)
            require(image.machine == {'x64': 0x8664, 'x86': 0x14c}[arch],
                    f'{source}: wrong DirectX DLL architecture')
            export = 'D3DXAssembleShader' if name.startswith('d3dx') else 'D3DAssemble'
            require(export in image.exports(), f'{source}: missing {export}')
            require(image.imports() <= SYSTEM_DLLS, f'{source}: unbundled dependency')
            reports[arch][name] = expected
    # Validate the complete input set before changing any staged game file.
    if stage is not None:
        for arch, triplet in [('x64', 'x86_64'), ('x86', 'i686')]:
            target = stage / 'game' / 'stk-code' / f'build-{triplet}' / 'bin'
            target.mkdir(parents=True, exist_ok=True)
            for name in DIRECTX_DLLS[arch]:
                shutil.copy2(directory / arch / name, target / name)
    return reports


def prepare(args):
    work = args.work.resolve()
    require(work.is_relative_to(ROOT), 'build output must be in this workspace')
    top = official(work, args.download)
    selected = verify_selected(top, work / ARCHIVE)
    dependencies = directx_dependencies(args.directx_dir or work / 'directx-runtime')
    build_probes(work)
    stage = work / 'stage'
    stage.mkdir(exist_ok=True)
    for relative, expected in selected:
        source = top / relative
        target = stage / 'game' / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        if target.exists():
            require(sha256(target) == expected, f'staged official file changed: {relative}')
        else:
            shutil.copy2(source, target)
    directx_dependencies(args.directx_dir or work / 'directx-runtime', stage)
    (stage / 'config.xml').write_text(CONFIG)
    for arch, triplet in [('x64', 'x86_64'), ('x86', 'i686')]:
        shutil.copy2(work / f'd3d9-probe-{arch}.dll',
                     stage / 'game' / 'stk-code' / f'build-{triplet}' / 'bin' / 'd3d9.dll')
        for mode in ('benchmark', 'race'):
            (stage / f'run-{mode}-{arch}.cmd').write_bytes(batch(arch, mode).encode('ascii'))
        (stage / f'run-benchmark-{arch}-control.cmd').write_bytes(
            batch(arch, 'benchmark', False).encode('ascii'))
    manifest = {'game': 'SuperTuxKart', 'version': VERSION, 'source_commit': SOURCE_COMMIT,
                'archive_url': URL, 'archive_sha256': ARCHIVE_SHA256,
                'directx_dependencies': dependencies,
                'official_files': {str(path): digest for path, digest in selected},
                'probe_files': {f'd3d9-probe-{arch}.dll': sha256(work / f'd3d9-probe-{arch}.dll')
                                for arch in ('x64', 'x86')}}
    (stage / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    (stage / 'install.cmd').write_bytes(
        b'@echo off\r\nif exist C:\\triton-kart\\manifest.json exit /b 2\r\n'
        b'xcopy /E /I /H /Y "%~dp0*" C:\\triton-kart\\\r\nexit /b %ERRORLEVEL%\r\n')
    image = work / 'triton-kart-1.5.iso'
    # A running VM may still hold the previous media open. Build a new inode
    # and publish it atomically; never truncate the mounted image in place.
    with tempfile.TemporaryDirectory(prefix='kart-media-', dir=work) as temporary:
        candidate = Path(temporary) / image.name
        subprocess.run(['xorriso', '-as', 'mkisofs', '-quiet', '-iso-level', '3',
                        '-J', '-joliet-long', '-R', '-V', 'TRITON_KART', '-o', str(candidate),
                        str(stage)], check=True)
        candidate.replace(image)
    print(json.dumps({'iso': str(image), 'sha256': sha256(image),
                      'official_files': len(selected), 'staged': str(stage)}))
    print('Kart Vista artifacts prepared')


def percentile(values, fraction):
    require(bool(values), 'empty frame sample')
    return sorted(values)[max(0, math.ceil(fraction * len(values)) - 1)]


def metrics(microseconds):
    require(len(microseconds) >= 100, 'too few rendered frames')
    require(all(isinstance(v, int) and v > 0 for v in microseconds),
            'invalid/nonpositive frame duration')
    total = sum(microseconds)
    return {
        'frames': len(microseconds), 'duration_seconds': total / 1_000_000,
        'duration_microseconds': total,
        'average_fps': len(microseconds) * 1_000_000 / total,
        'p50_ms': percentile(microseconds, .50) / 1000,
        'p95_ms': percentile(microseconds, .95) / 1000,
        'p99_ms': percentile(microseconds, .99) / 1000,
        'worst_ms': max(microseconds) / 1000,
        'over_100ms_fraction': sum(v > 100_000 for v in microseconds) / len(microseconds),
    }


def acceptable(result, minimum_seconds):
    return (result['duration_seconds'] >= minimum_seconds
            and result['average_fps'] >= 30
            and result['p95_ms'] <= 33.334
            and result['p99_ms'] <= 50
            and result['over_100ms_fraction'] <= .001)


def benchmark_samples(path):
    with path.open(newline='') as stream:
        header = stream.readline()
        require(re.match(r'^#\s+"Main loop\(1\)"', header) is not None,
                'CSV is not the main rendered-frame thread')
        durations = []
        for index, row in enumerate(csv.reader(stream)):
            require(bool(row) and row[0].strip().isdigit(), 'malformed profiler frame')
            value = int(row[0])
            # Upstream explicitly records one zero-duration first frame.
            if value == 0 and index == 0:
                continue
            require(value > 0, 'nonpositive profiler frame after capture began')
            durations.append(value)
    return durations


def trim_samples(durations, warmup_seconds, tail_seconds):
    begin = 0
    limit = sum(durations) - tail_seconds * 1_000_000
    retained = []
    for value in durations:
        end = begin + value
        if begin >= warmup_seconds * 1_000_000 and end <= limit:
            retained.append(value)
        begin = end
    return retained


def benchmark_csv(path):
    return metrics(benchmark_samples(path))


def present_csv(path, warmup_seconds=15, tail_seconds=2,
                expected_arch=None, expected_wow64=None):
    metadata = {}
    lines = path.read_text().splitlines()
    while lines and lines[0].startswith('# '):
        key, separator, value = lines.pop(0)[2:].partition('=')
        require(separator and key not in metadata, 'duplicate/malformed Present metadata')
        metadata[key] = value
    allowed_metadata = {'format', 'os', 'qpc_frequency', 'device_type',
                        'behavior_flags', 'width', 'height', 'umd_path',
                        'adapter_driver', 'adapter_description', 'process_arch', 'wow64',
                        'system_directory'}
    require(not metadata.keys() - allowed_metadata, 'capture contains an instrumentation error or reset')
    require(metadata.get('format') == 'triton-kart-present-v2', 'wrong Present capture format')
    require(metadata.get('os') == '6.0', 'capture did not run under Windows Vista')
    require(metadata.get('device_type') == '1', 'capture was not a D3D9 HAL device')
    flags = int(metadata.get('behavior_flags', '0'), 16)
    require(flags & 0x40 and not flags & (0x20 | 0x80),
            'hardware vertex processing was not used')
    require(metadata.get('width') == '1280' and metadata.get('height') == '720',
            'rendered backbuffer is not 1280x720')
    arch, wow64 = metadata.get('process_arch'), metadata.get('wow64')
    require(arch in ('x64', 'x86') and wow64 in ('0', '1') and
            not (arch == 'x64' and wow64 != '0'), 'invalid process architecture')
    require(expected_arch is None or arch == expected_arch,
            'capture process architecture differs from the collected workload')
    require(expected_wow64 is None or (wow64 == '1') is expected_wow64,
            'capture WOW64 state differs from the collected workload')
    system_directory = PureWindowsPath(metadata.get('system_directory', ''))
    loaded = PureWindowsPath(metadata.get('umd_path', ''))
    expected_directory = 'syswow64' if wow64 == '1' else 'system32'
    expected_name = 'neptune_d3d9_wow.dll' if wow64 == '1' else 'neptune_d3d9.dll'
    require(system_directory.is_absolute() and
            re.fullmatch(r'[A-Za-z]:', system_directory.drive) is not None and
            system_directory.name.casefold() == expected_directory and
            '..' not in system_directory.parts and '..' not in loaded.parts and
            loaded == system_directory / expected_name,
            'actual architecture-specific system Neptune D3D9 UMD was not loaded')
    frequency = int(metadata.get('qpc_frequency', '0'))
    require(frequency > 0, 'invalid performance counter frequency')
    require(lines and lines[-1].startswith('# capture_complete='),
            'Present capture is incomplete or its process crashed')
    completed = int(lines.pop().split('=', 1)[1])
    rows = list(csv.DictReader(lines))
    require(len(rows) == completed, 'Present capture count does not match its footer')
    require(len(rows) >= 100, 'too few successful Present calls')
    frames = []
    spans = []
    previous_end = -1
    for expected, row in enumerate(rows):
        require(set(row) == {'frame', 'qpc_begin', 'qpc_end', 'hresult'}, 'bad Present columns')
        require(int(row['frame']) == expected, 'missing/reordered Present records')
        begin, end = int(row['qpc_begin']), int(row['qpc_end'])
        require(begin > previous_end and end >= begin, 'invalid/non-monotonic QPC records')
        require(int(row['hresult'], 16) == 0, 'Present failed or lost its device')
        frames.append(end)
        spans.append((begin, end))
        previous_end = end
    begin_limit = frames[0] + warmup_seconds * frequency
    end_limit = frames[-1] - tail_seconds * frequency
    intervals = [round((b - a) * 1_000_000 / frequency)
                 for a, b in zip(frames, frames[1:])
                 if a >= begin_limit and b <= end_limit]
    result = metrics(intervals)
    result['warmup_seconds'] = warmup_seconds
    result['tail_seconds'] = tail_seconds
    result['metadata'] = metadata
    result['capture_frames'] = len(frames)
    result['capture_duration_seconds'] = (frames[-1] - frames[0]) / frequency
    origin = spans[0][0]
    normalized = [frequency, [(begin - origin, end - origin) for begin, end in spans]]
    result['capture_fingerprint'] = hashlib.sha256(
        json.dumps(normalized, separators=(',', ':')).encode('ascii')).hexdigest()
    return result


def log_check(path, mode):
    content = path.read_text(errors='replace')
    version = (path.parent / 'version.log').read_text(errors='replace')
    require((path.parent / 'version-exit-code.txt').read_text().strip() == '0',
            'version process did not exit successfully')
    require(re.search(r'SuperTuxKart, 1\.5\.(?:\s|$)', version) is not None,
            'game version is not 1.5')
    require('Currently available Video Memory' in content,
            'the upstream Direct3D9 initialization log is absent')
    require(not re.search(r'\[fatal\s*\]|SuperTuxKart crashed|Aborting SuperTuxKart', content),
            'game log contains a fatal/crash')
    if mode == 'race':
        require('Profiling 3 laps.' in content, 'three-lap AI race was not requested')
        require('Nb of karts=8,' in content, 'race did not contain eight karts')
        require('Number of frames:' in content and 'Average FPS:' in content,
                'AI race did not reach normal completion')
        # This statistic is physics-update based. Deliberately never parse it
        # as render performance; use the independent Present capture instead.
    else:
        require('Benchmark mode requested from command-line' in content,
                'official rendered replay benchmark was not requested')
        require("Steady FPS '" in content, 'official replay did not finish profiling')
        replays = re.findall(r"Replay: Reading replay file '([^']+)'", content)
        require(len(replays) == 1 and
                replays[0].replace('\\', '/').endswith('/benchmark_black_forest.replay'),
                'benchmark did not use exactly the pinned Black Forest replay')


def settings_check(directory):
    config = ET.parse(directory / 'config.xml').getroot()
    expected = {'Video': {'real_width': '1280', 'real_height': '720',
                          'render_driver': 'directx9', 'fullscreen': 'false',
                          'enable_dynamic_lights': 'false', 'scale_rtts_factor': '1',
                          'enable_high_definition_textures': '2',
                          'enable_texture_compression': 'true', 'max_texture_size': '512'},
                'GFX': {'particles-effecs': '2', 'animated-characters': 'true',
                        'geometry-level': '2', 'anisotropic': '0',
                        'swap-interval-vsync': '0'}}
    for group, attributes in expected.items():
        node = config.find(group)
        require(node is not None, f'game did not save {group} settings')
        for name, value in attributes.items():
            actual = node.get(name)
            if name == 'scale_rtts_factor':
                require(actual is not None and float(actual) == float(value),
                        'game used a reduced render resolution')
            else:
                require(actual == value, f'game changed setting {group}/{name}: {actual}')


def benchmark_identity(identity):
    expected = {'run_id', 'architecture', 'instrumented', 'replay_sha256',
                'executable_sha256', 'driver_sha256', 'probe_sha256', 'seed', 'wow64'}
    require(isinstance(identity, dict) and set(identity) == expected,
            'missing/unknown benchmark identity fields')
    require(isinstance(identity['run_id'], str) and identity['run_id'].strip(),
            'missing collector-observed unique guest run directory')
    arch = identity['architecture']
    require(arch in EXECUTABLE_SHA256, 'invalid benchmark architecture')
    require(type(identity['wow64']) is bool and
            not (arch == 'x64' and identity['wow64']), 'invalid collector WOW64 identity')
    require(identity['replay_sha256'] == REPLAY_SHA256,
            'benchmark replay does not match the authenticated archive')
    require(identity['executable_sha256'] == EXECUTABLE_SHA256[arch],
            'benchmark executable does not match the authenticated archive')
    require(identity['seed'] == 20260926, 'benchmark random seed differs')
    require(type(identity['instrumented']) is bool, 'invalid instrumentation identity')
    require(isinstance(identity['driver_sha256'], str) and
            re.fullmatch('[0-9a-f]{64}', identity['driver_sha256']),
            'missing collector-observed driver package SHA-256')
    if identity['instrumented']:
        require(isinstance(identity['probe_sha256'], str) and
                re.fullmatch('[0-9a-f]{64}', identity['probe_sha256']),
                'missing collector-observed probe SHA-256')
    else:
        require(identity['probe_sha256'] is None,
                'uninstrumented identity still names a measurement probe')
    return identity


def capture_files(directory, instrumented):
    names = BENCHMARK_FILES + (('present.csv',) if instrumented else ())
    require(instrumented or not (directory / 'present.csv').exists(),
            'control still had a Present capture')
    return {name: sha256(directory / name) for name in names}


def seal_benchmark(args):
    # Identity must come from the trusted VM collector's observed run and
    # deployed files. This receipt detects later collection edits/mixups; it
    # cannot establish where a caller's supplied identity was measured.
    identity = benchmark_identity(json.loads(args.identity.read_text()))
    receipt = {'format': 'triton-kart-benchmark-integrity-v1', 'identity': identity,
               'files': capture_files(args.results, identity['instrumented'])}
    with (args.results / 'run-integrity.json').open('x') as stream:
        json.dump(receipt, stream, indent=2)
        stream.write('\n')
    print('Kart benchmark capture sealed; performance not assessed')


def benchmark_run(directory, instrumented):
    receipt = json.loads((directory / 'run-integrity.json').read_text())
    require(isinstance(receipt, dict) and
            set(receipt) == {'format', 'identity', 'files'} and
            receipt['format'] == 'triton-kart-benchmark-integrity-v1',
            'missing/invalid capture integrity receipt')
    identity = benchmark_identity(receipt['identity'])
    require(identity['instrumented'] is instrumented, 'wrong benchmark group')
    hashes = capture_files(directory, instrumented)
    require(receipt['files'] == hashes, 'collected capture changed after sealing')
    require((directory / 'exit-code.txt').read_text().strip() == '0',
            'benchmark process did not exit successfully')
    log_check(directory / 'stdout.log', 'benchmark')
    settings_check(directory)
    samples = benchmark_samples(directory / 'stdout.log.profile-black_forest-cpu-0.csv')
    raw = metrics(samples)
    summaries = re.findall(r"Profiler: Frame count '(\d+)', Time \(ms\) '(\d+)', "
                           r"Steady FPS '\d+'", (directory / 'stdout.log').read_text())
    require(len(summaries) == 1, 'missing/duplicate benchmark completion summary')
    count, milliseconds = map(int, summaries[0])
    require(count == len(samples) and milliseconds == sum(samples) // 1000,
            'profiler CSV does not match the independent completion summary')
    # The pinned playback's last sample is 37.137501 seconds. Allow 137.501 ms
    # for marker-boundary differences, but reject implausibly short captures.
    require(sum(samples) >= math.floor(REPLAY_LAST_SECONDS) * 1_000_000,
            'benchmark capture is shorter than the pinned complete replay')
    retained = trim_samples(samples, BENCHMARK_WARMUP_SECONDS, BENCHMARK_TAIL_SECONDS)
    cpu = metrics(retained)
    result = {'directory': str(directory.resolve()), 'identity': identity,
              'files': hashes, 'upstream_raw': raw, 'upstream_main_loop': cpu,
              'main_loop_fingerprint': hashlib.sha256(
                  json.dumps(samples, separators=(',', ':')).encode('ascii')).hexdigest(),
              'warmup_seconds': BENCHMARK_WARMUP_SECONDS,
              'tail_seconds': BENCHMARK_TAIL_SECONDS,
              'passed': acceptable(cpu, 0)}
    if instrumented:
        result['present'] = present_csv(directory / 'present.csv',
                                        BENCHMARK_WARMUP_SECONDS,
                                        BENCHMARK_TAIL_SECONDS,
                                        identity['architecture'], identity['wow64'])
        require(result['present']['capture_duration_seconds'] >= math.floor(REPLAY_LAST_SECONDS),
                'Present capture is shorter than the pinned complete replay')
        result['passed'] &= acceptable(result['present'], 0)
    return result


def benchmark_groups(instrumented, controls):
    require(len(instrumented) == 3 and len(controls) == 3,
            'supply exactly three instrumented and three control repetitions')
    directories = [p.resolve() for p in instrumented + controls]
    require(len(set(directories)) == 6, 'duplicate benchmark result directories')
    all_runs = []
    for directory, measured in [(p, True) for p in instrumented] + [(p, False) for p in controls]:
        # Retain each failed run instead of silently dropping it or stopping
        # before the other five runs have been diagnosed.
        try:
            all_runs.append(benchmark_run(directory, measured))
        except (OSError, ValueError, KeyError, TypeError, ET.ParseError) as error:
            all_runs.append({'directory': str(directory.resolve()),
                             'passed': False, 'error': str(error)})
    errors = []
    if any('error' in run for run in all_runs):
        errors.append('one or more benchmark repetitions failed integrity/completion')
    else:
        identities = [run['identity'] for run in all_runs]
        if len({identity['run_id'].casefold() for identity in identities}) != 6:
            errors.append('duplicate guest run identities')
        # Canonical samples reject copied captures even when whitespace,
        # unrelated CSV columns or absolute QPC origins were changed.
        if len({run['main_loop_fingerprint'] for run in all_runs}) != 6:
            errors.append('duplicate main-loop captures')
        if len({run['present']['capture_fingerprint'] for run in all_runs[:3]}) != 3:
            errors.append('duplicate Present captures')
        common = ('architecture', 'replay_sha256', 'executable_sha256',
                  'driver_sha256', 'seed', 'wow64')
        for name in common:
            if len({identity[name] for identity in identities}) != 1:
                errors.append(f'mismatched run identity: {name}')
        if len({identity['probe_sha256'] for identity in identities[:3]}) != 1:
            errors.append('mismatched instrumented probe versions')
    groups = {}
    for label, runs in [('instrumented', all_runs[:3]), ('controls', all_runs[3:])]:
        total = sum(run.get('upstream_main_loop', {}).get('duration_microseconds', 0)
                    for run in runs)
        group = {'runs': runs, 'combined_main_loop_seconds': total / 1_000_000,
                 'passed': all(run['passed'] for run in runs) and total >= 60_000_000}
        if label == 'instrumented':
            present_total = sum(run.get('present', {}).get('duration_microseconds', 0)
                                for run in runs)
            group['combined_present_seconds'] = present_total / 1_000_000
            group['passed'] &= present_total >= 60_000_000
        groups[label] = group
    report = {'workload': 'three complete pinned Black Forest replays per group',
              'replay_sha256': REPLAY_SHA256, 'last_replay_sample_seconds': REPLAY_LAST_SECONDS,
              'groups': groups, 'errors': errors,
              'passed': not errors and all(group['passed'] for group in groups.values())}
    if not errors:
        comparison = {}
        for key in ('average_fps', 'p50_ms', 'p95_ms', 'p99_ms', 'worst_ms',
                    'over_100ms_fraction'):
            measured = statistics.median(run['upstream_main_loop'][key] for run in all_runs[:3])
            control = statistics.median(run['upstream_main_loop'][key] for run in all_runs[3:])
            comparison[key] = {'instrumented_median': measured, 'control_median': control,
                               'difference': measured - control,
                               'difference_percent': (measured / control - 1) * 100
                               if control else None}
        report['instrumentation_comparison'] = comparison
    return report


def assess_benchmarks(args):
    protected = {directory.resolve() / name
                 for directory in args.instrumented + args.controls
                 for name in BENCHMARK_FILES + ('present.csv', 'run-integrity.json')}
    require(args.output.resolve() not in protected, 'report would overwrite capture evidence')
    report = benchmark_groups(args.instrumented, args.controls)
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    require(report['passed'], 'benchmark groups failed completeness, identity, duration or pacing')
    print('Kart complete benchmark and control groups passed')


def assess(args):
    directory = args.results
    require((directory / 'exit-code.txt').read_text().strip() == '0',
            'game process did not exit successfully')
    log_check(directory / 'stdout.log', 'race')
    settings_check(directory)
    present = present_csv(directory / 'present.csv', expected_arch=args.architecture)
    report = {'workload': 'race', 'present': present,
              'passed': acceptable(present, 120)}
    (directory / 'assessment.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    require(report['passed'], 'kart workload does not meet the declared 720p/30 FPS criterion')
    print('Kart game measured performance passed')


PROBE_FIXTURE = r'''#define _GNU_SOURCE
#include <assert.h>
#include <stdlib.h>
#include <wchar.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <stdatomic.h>
#include <strings.h>
#ifndef KART_FIXTURE_X86
#define _WIN64
#endif
#ifndef KART_FIXTURE_WOW64
#define KART_FIXTURE_WOW64 0
#endif
#if KART_FIXTURE_WOW64
#define FIXTURE_UMD_NAME L"neptune_d3d9_wow.dll"
#define FIXTURE_SYSTEM_DIRECTORY "C:\\Windows\\SysWOW64"
#define FIXTURE_UMD_FILE "neptune_d3d9_wow.dll"
#else
#define FIXTURE_UMD_NAME L"neptune_d3d9.dll"
#define FIXTURE_SYSTEM_DIRECTORY "C:\\Windows\\System32"
#define FIXTURE_UMD_FILE "neptune_d3d9.dll"
#endif
#ifdef KART_FIXTURE_FOREIGN_UMD
#define FIXTURE_UMD_PATH "C:\\foreign\\" FIXTURE_UMD_FILE
#else
#define FIXTURE_UMD_PATH FIXTURE_SYSTEM_DIRECTORY "\\" FIXTURE_UMD_FILE
#endif
#define WINAPI
#define CALLBACK
#define MAX_PATH 260
#define TRUE 1
#define FALSE 0
#define INVALID_HANDLE_VALUE ((void *)(intptr_t)-1)
#define HEAP_ZERO_MEMORY 8
#define GENERIC_WRITE 1
#define FILE_SHARE_READ 1
#define CREATE_NEW 1
#define FILE_ATTRIBUTE_NORMAL 0
#define DLL_PROCESS_ATTACH 1
#define DLL_PROCESS_DETACH 0
#define SUCCEEDED(x) ((x) >= 0)
#define FAILED(x) ((x) < 0)
#define ZeroMemory(p,n) memset(p,0,n)
#define PAGE_EXECUTE_READWRITE 0x40
#define TLS_OUT_OF_INDEXES 0xffffffffu
#define GET_MODULE_HANDLE_EX_FLAG_PIN 1
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 4
#define INIT_ONCE_STATIC_INIT {PTHREAD_MUTEX_INITIALIZER, 0}
typedef int BOOL;
typedef uint32_t UINT, DWORD, ULONG;
typedef int32_t HRESULT;
typedef void *HANDLE, *HMODULE, *HINSTANCE, *HWND, *LPVOID, *PVOID;
typedef const wchar_t *LPCWSTR;
typedef pthread_mutex_t CRITICAL_SECTION;
typedef struct { pthread_mutex_t mutex; int done; } INIT_ONCE, *PINIT_ONCE;
typedef void (*FARPROC)(void);
typedef struct { int64_t QuadPart; } LARGE_INTEGER;
typedef struct { DWORD dwOSVersionInfoSize,dwMajorVersion,dwMinorVersion; } OSVERSIONINFOW;
typedef struct { int left,top,right,bottom; } RECT;
typedef struct { int value; } RGNDATA;
static char written[2000000];
static size_t written_size;
static int allocated, freed, pinned, protected, fail_allocation, fail_protection;
static _Atomic int64_t tick;
static pthread_key_t tls;
static _Thread_local DWORD last_error;
static DWORD GetLastError(void) {return last_error;}
static void SetLastError(DWORD value) {last_error=value;}
static HANDLE GetProcessHeap(void) { return (HANDLE)1; }
static void *HeapAlloc(HANDLE h,DWORD flags,size_t n) {
    (void)h;(void)flags;
    if (fail_allocation) return NULL;
    allocated++;return calloc(1,n);
}
static BOOL HeapFree(HANDLE h,DWORD flags,void *p) {
    (void)h;(void)flags;freed++;free(p);return TRUE;
}
static void InitializeCriticalSection(CRITICAL_SECTION *p) {
    pthread_mutexattr_t a;
    assert(!pthread_mutexattr_init(&a));
    assert(!pthread_mutexattr_settype(&a,PTHREAD_MUTEX_RECURSIVE));
    assert(!pthread_mutex_init(p,&a));
    assert(!pthread_mutexattr_destroy(&a));
}
static void EnterCriticalSection(CRITICAL_SECTION *p) {assert(!pthread_mutex_lock(p));}
static void LeaveCriticalSection(CRITICAL_SECTION *p) {assert(!pthread_mutex_unlock(p));}
static BOOL InitOnceExecuteOnce(PINIT_ONCE p,BOOL (*f)(PINIT_ONCE,PVOID,PVOID *),PVOID a,PVOID *c) {
    assert(!pthread_mutex_lock(&p->mutex));
    if (!p->done) {assert(f(p,a,c));p->done=1;}
    assert(!pthread_mutex_unlock(&p->mutex));return TRUE;
}
static BOOL VirtualProtect(void *p,size_t n,DWORD flags,DWORD *old) {
    assert(p && n==sizeof(void *) && flags==PAGE_EXECUTE_READWRITE && pinned);
    if (fail_protection) return FALSE;
    protected++;*old=PAGE_EXECUTE_READWRITE;return TRUE;
}
static PVOID InterlockedExchangePointer(PVOID volatile *p,PVOID v) {
    assert(pinned);return __atomic_exchange_n(p,v,__ATOMIC_SEQ_CST);
}
static DWORD TlsAlloc(void) {assert(!pthread_key_create(&tls,NULL));return 0;}
static PVOID TlsGetValue(DWORD i) {assert(!i);SetLastError(0);return pthread_getspecific(tls);}
static BOOL TlsSetValue(DWORD i,PVOID v) {assert(!i);return !pthread_setspecific(tls,v);}
static BOOL GetModuleHandleExW(DWORD flags,LPCWSTR address,HMODULE *out) {
    assert(flags==(GET_MODULE_HANDLE_EX_FLAG_PIN|GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) && address);
    pinned=1;*out=(HMODULE)5;return TRUE;
}
static BOOL QueryPerformanceCounter(LARGE_INTEGER *p) {p->QuadPart=atomic_fetch_add(&tick,100)+100;return TRUE;}
static BOOL QueryPerformanceFrequency(LARGE_INTEGER *p) {p->QuadPart=1000000;return TRUE;}
static BOOL GetVersionExW(OSVERSIONINFOW *p) {p->dwMajorVersion=6;p->dwMinorVersion=0;return TRUE;}
static HMODULE GetModuleHandleW(const wchar_t *n) {
#ifdef KART_FIXTURE_NO_UMD
    (void)n;return NULL;
#elif defined(KART_FIXTURE_WRONG_NAME)
    return !wcscmp(n, KART_FIXTURE_WOW64 ? L"neptune_d3d9.dll" :
                                           L"neptune_d3d9_wow.dll") ? (HMODULE)2 : NULL;
#else
    assert(!wcscmp(n,FIXTURE_UMD_NAME));return (HMODULE)2;
#endif
}
static HANDLE GetCurrentProcess(void) {return (HANDLE)(intptr_t)-1;}
static BOOL IsWow64Process(HANDLE h,BOOL *wow64) {
    assert(h==(HANDLE)(intptr_t)-1);*wow64=KART_FIXTURE_WOW64;return TRUE;
}
static UINT GetSystemDirectoryA(char *p,UINT n) {
    assert(n>30);strcpy(p,"C:\\Windows\\System32");return strlen(p);
}
static UINT GetSystemWow64DirectoryA(char *p,UINT n) {
    assert(n>30);strcpy(p,"C:\\Windows\\SysWOW64");return strlen(p);
}
static int lstrcmpiA(const char *a,const char *b) {return strcasecmp(a,b);}

static DWORD GetModuleFileNameA(HMODULE h,char *p,DWORD n) {
    const char *s=FIXTURE_UMD_PATH;
    assert(h==(HMODULE)2 && n>strlen(s));strcpy(p,s);return strlen(s);
}
static UINT GetSystemDirectoryW(wchar_t *p,UINT n) {
    assert(n>30);wcscpy(p,L"C:\\Windows\\System32");return wcslen(p);
}
static DWORD GetEnvironmentVariableW(const wchar_t *name,wchar_t *p,DWORD n) {
    assert(!wcscmp(name,L"TRITON_KART_TIMINGS") && n>20);
    wcscpy(p,L"capture.csv");return wcslen(p);
}
static HMODULE LoadLibraryW(const wchar_t *p) {
    assert(!wcscmp(p,L"C:\\Windows\\System32\\d3d9.dll"));return (HMODULE)3;
}
static HANDLE CreateFileW(const wchar_t *p,DWORD access,DWORD share,void *sa,
                          DWORD creation,DWORD flags,HANDLE template_handle) {
    assert(!wcscmp(p,L"capture.csv") && access==GENERIC_WRITE &&
           share==FILE_SHARE_READ && !sa && creation==CREATE_NEW &&
           flags==FILE_ATTRIBUTE_NORMAL && !template_handle);
    return (HANDLE)4;
}
static BOOL WriteFile(HANDLE h,const void *p,DWORD n,DWORD *done,void *async) {
    assert(h==(HANDLE)4 && !async && written_size+n<sizeof(written));
    memcpy(written+written_size,p,n);written_size+=n;*done=n;return TRUE;
}
static BOOL CloseHandle(HANDLE h) {assert(h==(HANDLE)4);return TRUE;}
static BOOL DisableThreadLibraryCalls(HINSTANCE h) {(void)h;return TRUE;}
static FARPROC GetProcAddress(HMODULE,const char *);

typedef int D3DDEVTYPE;
#define D3DBACKBUFFER_TYPE_MONO 0
typedef struct { UINT Width,Height; } D3DSURFACE_DESC;
typedef struct { char Driver[512],Description[512]; } D3DADAPTER_IDENTIFIER9;
typedef struct { unsigned cookie; } D3DPRESENT_PARAMETERS;
typedef struct IDirect3D9 IDirect3D9;
typedef struct IDirect3DDevice9 IDirect3DDevice9;
typedef struct IDirect3DSurface9 IDirect3DSurface9;
typedef struct IDirect3DSwapChain9 IDirect3DSwapChain9;
#define COM_BASE HRESULT (*QueryInterface)(void *,const void *,void **); ULONG (*AddRef)(void *); ULONG (*Release)(void *)
typedef struct {
    COM_BASE;
    void *unused3[2];
    HRESULT (*GetAdapterIdentifier)(IDirect3D9 *,UINT,DWORD,D3DADAPTER_IDENTIFIER9 *);
    void *unused6[10];
    HRESULT (*CreateDevice)(IDirect3D9 *,UINT,D3DDEVTYPE,HWND,DWORD,D3DPRESENT_PARAMETERS *,IDirect3DDevice9 **);
} IDirect3D9Vtbl;
struct IDirect3D9 { IDirect3D9Vtbl *lpVtbl; _Atomic unsigned refs; };
typedef struct {
    COM_BASE;
    void *unused3[11];
    HRESULT (*GetSwapChain)(IDirect3DDevice9 *,UINT,IDirect3DSwapChain9 **);
    void *unused15;
    HRESULT (*Reset)(IDirect3DDevice9 *,D3DPRESENT_PARAMETERS *);
    HRESULT (*Present)(IDirect3DDevice9 *,const RECT *,const RECT *,HWND,const RGNDATA *);
    HRESULT (*GetBackBuffer)(IDirect3DDevice9 *,UINT,UINT,int,IDirect3DSurface9 **);
    void *unused19[25];
    HRESULT (*SetTransform)(IDirect3DDevice9 *,unsigned,const void *);
    void *unused45[74];
} IDirect3DDevice9Vtbl;
struct IDirect3DDevice9 { IDirect3DDevice9Vtbl *lpVtbl; _Atomic unsigned refs; };
typedef struct {
    COM_BASE;
    HRESULT (*Present)(IDirect3DSwapChain9 *,const RECT *,const RECT *,HWND,const RGNDATA *,DWORD);
    void *unused4[6];
} IDirect3DSwapChain9Vtbl;
struct IDirect3DSwapChain9 { IDirect3DSwapChain9Vtbl *lpVtbl; _Atomic unsigned refs; };
typedef struct {
    COM_BASE;
    HRESULT (*GetDesc)(IDirect3DSurface9 *,D3DSURFACE_DESC *);
} IDirect3DSurface9Vtbl;
struct IDirect3DSurface9 { IDirect3DSurface9Vtbl *lpVtbl; _Atomic unsigned refs; };
#define IDirect3D9_GetAdapterIdentifier(p,a,b,c) ((p)->lpVtbl->GetAdapterIdentifier(p,a,b,c))
#define IDirect3DDevice9_GetBackBuffer(p,a,b,c,d) ((p)->lpVtbl->GetBackBuffer(p,a,b,c,d))
#define IDirect3DSurface9_GetDesc(p,a) ((p)->lpVtbl->GetDesc(p,a))
#define IDirect3DSurface9_Release(p) ((p)->lpVtbl->Release(p))

/* PRODUCTION_PROBE */

static IDirect3D9 fake_adapter;
static IDirect3DDevice9 fake_device;
static IDirect3DSwapChain9 fake_chain;
static IDirect3DSurface9 fake_surface;
static D3DPRESENT_PARAMETERS parameters={0x1234};
static RECT rect={1,2,3,4};
static RGNDATA dirty={123};
static HRESULT next_create, next_present;
static _Atomic unsigned present_calls, private_calls, device_calls;
static pthread_barrier_t simultaneous;
static _Thread_local int barrier_present;
struct extended_device_table { IDirect3DDevice9Vtbl public; void *private_entries[34]; };
static struct extended_device_table dv;
static IDirect3D9Vtbl av;
static IDirect3DSwapChain9Vtbl cv;
static IDirect3DSurface9Vtbl sv;
static ULONG addref(void *p) {
    IDirect3D9 *o=p;
    return atomic_fetch_add(&o->refs,1)+1;
}
static ULONG release(void *p) {
    IDirect3D9 *o=p;
    unsigned before=atomic_fetch_sub(&o->refs,1);
    assert(before>0);return before-1;
}
static HRESULT query(void *p,const void *iid,void **out) {
    assert(iid==(void *)99);*out=p;addref(p);return 0;
}
static HRESULT surface_desc(IDirect3DSurface9 *p,D3DSURFACE_DESC *out) {
    assert(p==&fake_surface);out->Width=1280;out->Height=720;return 0;
}
static HRESULT real_create_device(IDirect3D9 *p,UINT ordinal,D3DDEVTYPE type,HWND h,
        DWORD flags,D3DPRESENT_PARAMETERS *pp,IDirect3DDevice9 **out) {
    assert(p==&fake_adapter && ordinal==0 && type==1 && h==(HWND)55 &&
           flags==0x40 && pp==&parameters && pp->cookie==0x1234);
    device_calls++;*out=next_create?NULL:&fake_device;
    if (!next_create) addref(*out);
    return next_create;
}
static HRESULT adapter_identifier(IDirect3D9 *p,UINT ordinal,DWORD flags,D3DADAPTER_IDENTIFIER9 *out) {
    assert(p==&fake_adapter && !ordinal && !flags);
    strcpy(out->Driver,"neptune_d3d9.dll");strcpy(out->Description,"Triton");return 0;
}
static HRESULT private_transform(IDirect3DDevice9 *p,unsigned state,const void *matrix) {
    IDirect3DSwapChain9 *chain;
    assert(p==&fake_device && p->lpVtbl==&dv.public && state==7 && matrix==&rect);
    /* Reenter the proxy from a private runtime call. */
    assert(!p->lpVtbl->GetSwapChain(p,0,&chain) && chain==&fake_chain);
    chain->lpVtbl->Release(chain);
    private_calls++;return 0x123;
}
static HRESULT real_transform(IDirect3DDevice9 *p,unsigned state,const void *matrix) {
    typedef HRESULT (*private_method)(IDirect3DDevice9 *,unsigned,const void *);
    /* The real Vista runtime performs this dispatch beyond public slot 118. */
    private_method f=(private_method)((void **)p->lpVtbl)[152];
    return f(p,state,matrix);
}
static HRESULT real_chain_present(IDirect3DSwapChain9 *p,const RECT *src,const RECT *dst,HWND h,const RGNDATA *r,DWORD flags) {
    assert(GetLastError()==1234);
    assert(p==&fake_chain && src==&rect && dst==&rect && h==(HWND)55 && r==&dirty && flags==17);
    assert(fake_device.lpVtbl->SetTransform(&fake_device,7,&rect)==0x123);
    if (barrier_present) {
        int status=pthread_barrier_wait(&simultaneous);
        assert(!status || status==PTHREAD_BARRIER_SERIAL_THREAD);
    }
    present_calls++;SetLastError(5678);return next_present;
}
static HRESULT real_present(IDirect3DDevice9 *p,const RECT *src,const RECT *dst,HWND h,const RGNDATA *r) {
    assert(p==&fake_device);
    return fake_chain.lpVtbl->Present(&fake_chain,src,dst,h,r,17);
}
static HRESULT real_reset(IDirect3DDevice9 *p,D3DPRESENT_PARAMETERS *pp) {
    assert(p==&fake_device && pp==&parameters);return (HRESULT)0x88760868;
}
static HRESULT get_backbuffer(IDirect3DDevice9 *p,UINT a,UINT b,int type,IDirect3DSurface9 **out) {
    assert(p==&fake_device && !a && !b && !type);*out=&fake_surface;addref(*out);return 0;
}
static HRESULT real_get_chain(IDirect3DDevice9 *p,UINT index,IDirect3DSwapChain9 **out) {
    assert(p==&fake_device && !index);*out=&fake_chain;addref(*out);return 0;
}
static IDirect3D9 *real_create(UINT sdk) {assert(sdk==32);addref(&fake_adapter);return &fake_adapter;}
static FARPROC GetProcAddress(HMODULE module,const char *name) {
    assert(module==(HMODULE)3 && !strcmp(name,"Direct3DCreate9"));return (FARPROC)real_create;
}
static void *worker(void *parameter) {
    int barrier=*(int *)parameter;
    IDirect3D9 *a=Direct3DCreate9(32);
    assert(a==&fake_adapter && a->lpVtbl==&av);
    barrier_present=barrier;
    for (int i=0;i<(barrier?1:1000);i++) {
        IDirect3DSwapChain9 *c;
        void *identity;
        assert(!fake_device.lpVtbl->GetSwapChain(&fake_device,0,&c));
        assert(c==&fake_chain && c->lpVtbl==&cv);
        assert(!c->lpVtbl->QueryInterface(c,(void *)99,&identity) && identity==c);
        c->lpVtbl->Release(c);
        SetLastError(1234);
        assert(!c->lpVtbl->Present(c,&rect,&rect,(HWND)55,&dirty,17));
        assert(GetLastError()==5678);
        c->lpVtbl->Release(c);
    }
    a->lpVtbl->Release(a);return NULL;
}
int main(void) {
    IDirect3D9 *a;IDirect3DDevice9 *d;IDirect3DSwapChain9 *c;void *identity;
    pthread_t threads[4];int barrier=0;
    _Static_assert(sizeof(IDirect3DDevice9Vtbl)==119*sizeof(void *),"public ABI");
    _Static_assert(offsetof(IDirect3DDevice9Vtbl,SetTransform)==44*sizeof(void *),"SetTransform ABI");
    av=(IDirect3D9Vtbl){.QueryInterface=query,.AddRef=addref,.Release=release,
        .CreateDevice=real_create_device,.GetAdapterIdentifier=adapter_identifier};
    dv.public=(IDirect3DDevice9Vtbl){.QueryInterface=query,.AddRef=addref,.Release=release,
        .Present=real_present,.Reset=real_reset,.GetBackBuffer=get_backbuffer,
        .GetSwapChain=real_get_chain,.SetTransform=real_transform};
    dv.private_entries[33]=(void *)private_transform;
    cv=(IDirect3DSwapChain9Vtbl){.QueryInterface=query,.AddRef=addref,.Release=release,.Present=real_chain_present};
    sv=(IDirect3DSurface9Vtbl){.QueryInterface=query,.AddRef=addref,.Release=release,.GetDesc=surface_desc};
    IDirect3D9Vtbl av_before=av;
    struct extended_device_table dv_before=dv;
    IDirect3DSwapChain9Vtbl cv_before=cv;
    fake_adapter.lpVtbl=&av;fake_device.lpVtbl=&dv.public;fake_chain.lpVtbl=&cv;fake_surface.lpVtbl=&sv;
    assert(DllMain((HINSTANCE)1,DLL_PROCESS_ATTACH,NULL));
    a=Direct3DCreate9(32);assert(a==&fake_adapter && a->lpVtbl==&av);
    next_create=(HRESULT)0x8876086c;
    assert(a->lpVtbl->CreateDevice(a,0,1,(HWND)55,0x40,&parameters,&d)==next_create && !d);
    next_create=0;
    assert(!a->lpVtbl->CreateDevice(a,0,1,(HWND)55,0x40,&parameters,&d) && d==&fake_device);
    assert(d->lpVtbl==&dv.public && dv.private_entries[33]==(void *)private_transform);
    assert(!d->lpVtbl->QueryInterface(d,(void *)99,&identity) && identity==d);
    assert(d->lpVtbl->Release(d)==1);
    assert(!d->lpVtbl->GetSwapChain(d,0,&c) && c==&fake_chain);
    SetLastError(1234);
    assert(!c->lpVtbl->Present(c,&rect,&rect,(HWND)55,&dirty,17));
    assert(GetLastError()==5678);
    assert(c->lpVtbl->Release(c)==0 && c->lpVtbl==&cv);
    assert(!d->lpVtbl->GetSwapChain(d,0,&c));
    next_present=(HRESULT)0x88760868;
    SetLastError(1234);
    assert(c->lpVtbl->Present(c,&rect,&rect,(HWND)55,&dirty,17)==next_present);
    assert(GetLastError()==5678);
    next_present=0;
    SetLastError(1234);
    assert(!d->lpVtbl->Present(d,&rect,&rect,(HWND)55,&dirty));
    assert(GetLastError()==5678);
    assert(frame==3 && present_calls==3 && private_calls==3);
    assert(c->lpVtbl->Release(c)==0 && c->lpVtbl==&cv);
    /* Stress table sharing, reentrant calls, concurrent installations and COM
     * reference counts; no probe record depends on a live object's address. */
    for (int i=0;i<4;i++) assert(!pthread_create(&threads[i],NULL,worker,&barrier));
    for (int i=0;i<4;i++) assert(!pthread_join(threads[i],NULL));
    barrier=1;assert(!pthread_barrier_init(&simultaneous,NULL,4));
    for (int i=0;i<4;i++) assert(!pthread_create(&threads[i],NULL,worker,&barrier));
    for (int i=0;i<4;i++) assert(!pthread_join(threads[i],NULL));
    assert(!pthread_barrier_destroy(&simultaneous));
    assert(present_calls==4007 && private_calls==4007 && frame==4007);
    assert(d->lpVtbl->Reset(d,&parameters)==(HRESULT)0x88760868);
    assert(!d->lpVtbl->Release(d) && d->lpVtbl==&dv.public);
    assert(!a->lpVtbl->Release(a) && a->lpVtbl==&av);
    assert(!fake_adapter.refs && !fake_device.refs && !fake_chain.refs && !fake_surface.refs);
    assert(device_calls==2 && allocated-freed==5 && protected==10);
    /* Every public and private entry except the five declared slots survives. */
    av_before.CreateDevice=av.CreateDevice;
    dv_before.public.GetSwapChain=dv.public.GetSwapChain;
    dv_before.public.Reset=dv.public.Reset;
    dv_before.public.Present=dv.public.Present;
    cv_before.Present=cv.Present;
    assert(!memcmp(&av_before,&av,sizeof(av)));
    assert(!memcmp(&dv_before,&dv,sizeof(dv)));
    assert(!memcmp(&cv_before,&cv,sizeof(cv)));
    /* Failed hook installation leaves the original function callable. */
    void *unmodified=(void *)private_transform;
    fail_allocation=1;
    assert(!install_slot(&unmodified,(void *)present_device));
    fail_allocation=0;fail_protection=1;
    assert(!install_slot(&unmodified,(void *)present_device));
    fail_protection=0;
    assert(unmodified==(void *)private_transform);
    assert(DllMain((HINSTANCE)1,DLL_PROCESS_DETACH,NULL));
#if defined(KART_FIXTURE_NO_UMD) || defined(KART_FIXTURE_WRONG_NAME)
    assert(strstr(written,"# umd_path=\n"));
#else
    assert(strstr(written,"# umd_path=" FIXTURE_UMD_PATH "\n"));
#endif
#if defined(KART_FIXTURE_NO_UMD) || defined(KART_FIXTURE_FOREIGN_UMD) || defined(KART_FIXTURE_WRONG_NAME)
    assert(strstr(written,"# instrumentation_error=loaded UMD identity mismatch\n"));
#else
    assert(!strstr(written,"# instrumentation_error=loaded UMD identity mismatch\n"));
#endif
#ifdef _WIN64
    assert(strstr(written,"# process_arch=x64\n# wow64=0\n"));
#elif KART_FIXTURE_WOW64
    assert(strstr(written,"# process_arch=x86\n# wow64=1\n"));
#else
    assert(strstr(written,"# process_arch=x86\n# wow64=0\n"));
#endif
    assert(strstr(written,"# capture_complete=4007\n"));
    assert(strstr(written,"1,300,400,88760868\n"));
    assert(strstr(written,"# instrumentation_error=concurrent presents\n"));
    /* The runtime owns process-lifetime tables; fixture cleanup happens only
     * after simulated process detach, when no hook can run again. */
    while (slots) {struct slot_hook *next=slots->next;free(slots);slots=next;}
    puts("Kart production probe forwarding, private dispatch, lifetime and concurrency checks passed");
    return 0;
}

'''


def probe_checks():
    production = (ROOT / "tests/vista/kart-present-probe.c").read_text()
    production = re.sub(r"^#include <(?:windows|d3d9)\.h>\n", "", production, flags=re.M)
    with tempfile.TemporaryDirectory(prefix="kart-probe-") as temporary:
        path = Path(temporary)
        source = path / "test.c"
        source.write_text(PROBE_FIXTURE.replace("/* PRODUCTION_PROBE */", production))
        subprocess.run([os.environ.get("CC", "clang"), "-std=c11", "-O1", "-g",
                        "-Wall", "-Wextra", "-Werror", "-pthread", "-fsanitize=address,undefined",
                        str(source), "-o", str(path / "test")], check=True)
        subprocess.run([str(path / "test")], check=True)
        for label, flags in [('native-x64', []), ('native-x86', ['-DKART_FIXTURE_X86']),
                             ('wow64', ['-DKART_FIXTURE_X86', '-DKART_FIXTURE_WOW64=1'])]:
            for scenario in ('valid', 'NO_UMD', 'FOREIGN_UMD', 'WRONG_NAME'):
                variant = flags + ([] if scenario == 'valid' else [f'-DKART_FIXTURE_{scenario}'])
                subprocess.run([os.environ.get("CC", "clang"), "-std=c11", "-O1", "-g",
                                "-Wall", "-Wextra", "-Werror", "-pthread",
                                "-fsanitize=address,undefined"] + variant +
                               [str(source), "-o", str(path / "identity")], check=True)
                subprocess.run([str(path / "identity")], check=True, stdout=subprocess.DEVNULL)
            print(f'Kart production UMD identity {label}: valid/missing/foreign/wrong-name checks passed')
        mutations = {
            'changed argument': production.replace('dirty, flags);', 'dirty, 0);'),
            'public table clone': production.replace(
                '    install_slot((void **)&(*result)->lpVtbl->Reset,',
                '    static IDirect3DDevice9Vtbl truncated;\n'
                '    truncated = *(*result)->lpVtbl;\n'
                '    (*result)->lpVtbl = &truncated;\n'
                '    install_slot((void **)&(*result)->lpVtbl->Reset,'),
            'double counted nested Present': production.replace('if (!depth)', 'if (TRUE)'),
            'missing concurrency detection': production.replace('if (active_presents++)',
                                                                 'if (0)'),
            'lost last error': production.replace('SetLastError(entry_error);', ';'),
        }
        for label, altered in mutations.items():
            require(altered != production, f'{label}: mutation did not apply')
            source.write_text(PROBE_FIXTURE.replace("/* PRODUCTION_PROBE */", altered))
            subprocess.run([os.environ.get("CC", "clang"), "-std=c11", "-O1", "-pthread",
                            str(source), "-o", str(path / "negative")], check=True)
            failed = subprocess.run([str(path / "negative")], capture_output=True)
            require(failed.returncode != 0 and b'Assertion' in failed.stderr,
                    f'{label}: mutation escaped the forwarding oracle')
            print(f'Kart probe {label} negative control passed')



def parser_checks():
    good = [16_667] * 6000
    require(acceptable(metrics(good), 60), 'positive performance control failed')
    require(not acceptable(metrics([50_000] * 6000), 60), '20 FPS negative control passed')
    spiky = good.copy()
    spiky[::10] = [80_000] * len(spiky[::10])
    require(not acceptable(metrics(spiky), 60), 'uneven pacing negative control passed')
    with tempfile.TemporaryDirectory(prefix='kart-parser-') as temp:
        path = Path(temp) / 'cpu.csv'
        path.write_text('#  "Main loop(1)", "Render(2)",\n0, 0,\n' + '16667, 10000,\n' * 6000)
        require(acceptable(benchmark_csv(path), 60), 'upstream CSV positive control failed')
        path.write_text('#  "Update race(1)",\n' + '16667,\n' * 6000)
        try:
            benchmark_csv(path)
        except ValueError:
            pass
        else:
            raise ValueError('physics-tick negative control passed')
    with tempfile.TemporaryDirectory(prefix='kart-present-parser-') as temp:
        path = Path(temp) / 'present.csv'
        header = ('# format=triton-kart-present-v2\n# os=6.0\n'
                  '# qpc_frequency=1000000\n# device_type=1\n'
                  '# behavior_flags=00000040\n# width=1280\n# height=720\n'
                  '# process_arch=x64\n# wow64=0\n# system_directory=C:\\Windows\\System32\n'
                  '# umd_path=C:\\Windows\\System32\\neptune_d3d9.dll\n'
                  'frame,qpc_begin,qpc_end,hresult\n')
        rows = ''.join(f'{i},{i * 16667 + 1},{i * 16667 + 101},00000000\n'
                       for i in range(10000))
        good_capture = header + rows + '# capture_complete=10000\n'
        path.write_text(good_capture)
        require(acceptable(present_csv(path), 120), 'Present parser positive control failed')
        mutations = {
            'software device': good_capture.replace('device_type=1', 'device_type=2'),
            'software vertices': good_capture.replace('00000040', '00000020', 1),
            'wrong OS': good_capture.replace('os=6.0', 'os=6.1'),
            'wrong resolution': good_capture.replace('width=1280', 'width=800'),
            'missing UMD': good_capture.replace('neptune_d3d9.dll', 'd3d9.dll'),
            'counter invalid': good_capture.replace('qpc_frequency=1000000', 'qpc_frequency=0'),
            'truncated capture': good_capture.rsplit('# capture_complete=', 1)[0],
            'lost frame': good_capture.replace('500,8333501', '501,8333501'),
            'Present failure': good_capture.replace('101,00000000', '101,88760868', 1),
            'reset': good_capture.replace('frame,qpc_begin', '# reset=00000000\nframe,qpc_begin'),
        }
        for arch, wow64 in [('x64', False), ('x86', False), ('x86', True)]:
            capture = good_capture.replace('process_arch=x64', f'process_arch={arch}')
            if wow64:
                capture = capture.replace('wow64=0', 'wow64=1').replace('System32', 'SysWOW64')
                capture = capture.replace('neptune_d3d9.dll', 'neptune_d3d9_wow.dll')
            path.write_text(capture)
            require(acceptable(present_csv(path, expected_arch=arch, expected_wow64=wow64), 120),
                    f'{arch}/{wow64}: valid architecture-specific UMD rejected')
            wrong_name = 'neptune_d3d9.dll' if wow64 else 'neptune_d3d9_wow.dll'
            right_name = 'neptune_d3d9_wow.dll' if wow64 else 'neptune_d3d9.dll'
            loaded = f"C:\\Windows\\{'SysWOW64' if wow64 else 'System32'}\\{right_name}"
            cases = {'absent module': capture.replace(loaded, ''),
                     'foreign directory': capture.replace(loaded, f'C:\\foreign\\{right_name}'),
                     'opposite basename': capture.replace(right_name, wrong_name)}
            for label, invalid in cases.items():
                path.write_text(invalid)
                try:
                    present_csv(path, expected_arch=arch, expected_wow64=wow64)
                except ValueError:
                    pass
                else:
                    raise ValueError(f'{arch}/{wow64}: {label} accepted')
            path.write_text(capture)
            for expected_arch, expected_wow64 in [('x86' if arch == 'x64' else 'x64', wow64),
                                                (arch, not wow64)]:
                try:
                    present_csv(path, expected_arch=expected_arch, expected_wow64=expected_wow64)
                except ValueError:
                    pass
                else:
                    raise ValueError('observed architecture differs from collector but was accepted')
        for label, mutated in mutations.items():
            require(mutated != good_capture, f'{label}: mutation did not apply')
            path.write_text(mutated)
            try:
                present_csv(path)
            except (ValueError, KeyError, TypeError):
                continue
            raise ValueError(f'{label}: invalid capture was accepted')
    print('Kart measurement parser checks passed')


def benchmark_group_checks():
    """Independent synthetic complete captures exercise group acceptance edges."""
    def rejected(call, label):
        try:
            result = call()
        except (OSError, ValueError, KeyError, TypeError, ET.ParseError):
            return
        require(not result['passed'], f'{label}: invalid benchmark group accepted')

    with tempfile.TemporaryDirectory(prefix='kart-groups-') as temporary:
        root = Path(temporary)
        runs = [root / f'run-{i}' for i in range(6)]
        for i, directory in enumerate(runs):
            directory.mkdir()
            count, duration = 2230, 16667 + i
            identity = {'run_id': f'C:\\triton-kart-results\\benchmark-x64-test-{i}',
                        'architecture': 'x64', 'wow64': False, 'instrumented': i < 3,
                        'replay_sha256': REPLAY_SHA256,
                        'executable_sha256': EXECUTABLE_SHA256['x64'],
                        'driver_sha256': 'a' * 64,
                        'probe_sha256': 'b' * 64 if i < 3 else None,
                        'seed': 20260926}
            (directory / 'identity.json').write_text(json.dumps(identity))
            (directory / 'config.xml').write_text(CONFIG)
            (directory / 'exit-code.txt').write_text('0\n')
            (directory / 'version.log').write_text('SuperTuxKart, 1.5.\n')
            (directory / 'version-exit-code.txt').write_text('0\n')
            (directory / 'stdout.log').write_text(
                'Currently available Video Memory\n'
                'Benchmark mode requested from command-line\n'
                "[info   ] Replay: Reading replay file '../../data/replay/benchmark_black_forest.replay'.\n"
                f"[info   ] Profiler: Frame count '{count}', Time (ms) '{count * duration // 1000}', "
                "Steady FPS '60', Mostly stable FPS '60', Typical FPS '60'\n")
            (directory / BENCHMARK_FILES[-1]).write_text(
                '#  "Main loop(1)", "Render(2)",\n0, 0,\n' + f'{duration}, 10000,\n' * count)
            if i < 3:
                header = ('# format=triton-kart-present-v2\n# os=6.0\n'
                          '# qpc_frequency=1000000\n# device_type=1\n'
                          '# behavior_flags=00000040\n# width=1280\n# height=720\n'
                          '# process_arch=x64\n# wow64=0\n# system_directory=C:\\Windows\\System32\n'
                  '# umd_path=C:\\Windows\\System32\\neptune_d3d9.dll\n'
                          'frame,qpc_begin,qpc_end,hresult\n')
                offset = (i + 1) * 100_000_000
                rows = ''.join(f'{n},{offset + n * duration},{offset + n * duration + 1},00000000\n'
                               for n in range(count + 1))
                (directory / 'present.csv').write_text(header + rows + f'# capture_complete={count + 1}\n')

        def reseal(directory):
            (directory / 'run-integrity.json').unlink(missing_ok=True)
            with contextlib.redirect_stdout(io.StringIO()):
                seal_benchmark(argparse.Namespace(results=directory,
                                                  identity=directory / 'identity.json'))

        for directory in runs:
            reseal(directory)
        baseline = {path: path.read_bytes() for directory in runs for path in directory.iterdir()}

        def restore():
            for directory in runs:
                for path in directory.iterdir():
                    path.unlink()
            for path, content in baseline.items():
                path.write_bytes(content)

        def group():
            return benchmark_groups(runs[:3], runs[3:])

        good = group()
        require(good['passed'], 'complete 37-second replay groups did not pass')
        require(all(run['upstream_raw']['duration_seconds'] < 38
                    for part in good['groups'].values() for run in part['runs']),
                'positive control did not exercise the short official replay')
        require(good['groups']['instrumented']['combined_present_seconds'] >= 60 and
                good['groups']['controls']['combined_main_loop_seconds'] >= 60,
                'positive control did not supply 60 seconds of retained samples')
        require('instrumentation_comparison' in good, 'matching controls were not compared')
        for wow64 in (False, True):
            for i, directory in enumerate(runs):
                identity = json.loads((directory / 'identity.json').read_text())
                identity.update(architecture='x86', wow64=wow64,
                                executable_sha256=EXECUTABLE_SHA256['x86'])
                (directory / 'identity.json').write_text(json.dumps(identity))
                if i < 3:
                    path = directory / 'present.csv'
                    capture = path.read_text().replace('process_arch=x64', 'process_arch=x86')
                    if wow64:
                        capture = capture.replace('wow64=0', 'wow64=1').replace('System32', 'SysWOW64')
                        capture = capture.replace('neptune_d3d9.dll', 'neptune_d3d9_wow.dll')
                    path.write_text(capture)
                reseal(directory)
            require(group()['passed'], f'complete native x86/WOW64 group rejected: {wow64}')
            # False collector identity must not relabel a real x86 capture.
            for directory in runs:
                identity = json.loads((directory / 'identity.json').read_text())
                identity.update(architecture='x64', wow64=False,
                                executable_sha256=EXECUTABLE_SHA256['x64'])
                (directory / 'identity.json').write_text(json.dumps(identity))
                reseal(directory)
            rejected(group, 'collector/capture architecture mismatch')
            restore()
        rejected(lambda: benchmark_groups(runs[:2], runs[3:]), 'missing repetition')
        rejected(lambda: benchmark_groups([runs[0], runs[0], runs[2]], runs[3:]),
                 'duplicate directory')

        (runs[0] / BENCHMARK_FILES[-1]).write_text('truncated')
        rejected(group, 'unsealed file change')
        restore()
        path = runs[0] / BENCHMARK_FILES[-1]
        path.write_text('\n'.join(path.read_text().splitlines()[:-100]) + '\n')
        reseal(runs[0])
        rejected(group, 'truncated CSV with original completion summary')
        restore()

        for name in ('stdout.log', BENCHMARK_FILES[-1]):
            (runs[1] / name).write_bytes((runs[0] / name).read_bytes())
        reseal(runs[1])
        rejected(group, 'renamed and resealed duplicate capture')
        path = runs[1] / BENCHMARK_FILES[-1]
        path.write_text(path.read_text().replace(', 10000,', ', 10001, '))
        reseal(runs[1])
        rejected(group, 'copied timing samples with changed unrelated columns')
        restore()
        lines = (runs[0] / 'present.csv').read_text().splitlines()
        index = lines.index('frame,qpc_begin,qpc_end,hresult') + 1
        for n in range(index, len(lines) - 1):
            frame, begin, end, status = lines[n].split(',')
            lines[n] = f'{frame},{int(begin) + 99999999},{int(end) + 99999999},{status}'
        (runs[1] / 'present.csv').write_text('\n'.join(lines) + '\n')
        reseal(runs[1])
        rejected(group, 'copied Present timings with shifted QPC origin')
        restore()
        identity = json.loads((runs[1] / 'identity.json').read_text())
        identity['run_id'] = json.loads((runs[0] / 'identity.json').read_text())['run_id']
        (runs[1] / 'identity.json').write_text(json.dumps(identity))
        reseal(runs[1])
        rejected(group, 'duplicate guest run identity')
        restore()
        identity = json.loads((runs[3] / 'identity.json').read_text())
        identity['driver_sha256'] = 'c' * 64
        (runs[3] / 'identity.json').write_text(json.dumps(identity))
        reseal(runs[3])
        mismatch = group()
        require(not mismatch['passed'] and 'instrumentation_comparison' not in mismatch,
                'mismatched drivers were compared')
        restore()
        for key in ('executable_sha256', 'replay_sha256'):
            receipt = json.loads((runs[0] / 'run-integrity.json').read_text())
            receipt['identity'][key] = '0' * 64
            (runs[0] / 'run-integrity.json').write_text(json.dumps(receipt))
            rejected(group, f'wrong pinned {key}')
            restore()

        for name in ('exit-code.txt', 'version.log', 'version-exit-code.txt', 'config.xml', 'present.csv'):
            (runs[0] / name).unlink()
            rejected(group, f'missing {name}')
            restore()
        (runs[0] / 'version-exit-code.txt').write_text('1\n')
        reseal(runs[0])
        rejected(group, 'failed version process')
        restore()
        (runs[0] / 'version.log').write_text('SuperTuxKart, 1.5.1.\n')
        reseal(runs[0])
        rejected(group, 'wrong version')
        restore()
        (runs[0] / 'exit-code.txt').write_text('1\n')
        reseal(runs[0])
        rejected(group, 'nonzero guest exit')
        restore()
        (runs[0] / 'config.xml').write_text(CONFIG.replace('max_texture_size="512"',
                                                          'max_texture_size="128"'))
        reseal(runs[0])
        rejected(group, 'changed render quality')
        restore()
        (runs[3] / 'present.csv').write_bytes((runs[0] / 'present.csv').read_bytes())
        rejected(group, 'instrumented control')
        restore()

        # Complete-looking summaries do not make a 20-second fragment a replay.
        samples = [16667] * 1200
        path = runs[0] / BENCHMARK_FILES[-1]
        path.write_text('#  "Main loop(1)",\n0,\n' + ''.join(f'{v},\n' for v in samples))
        log = (runs[0] / 'stdout.log').read_text()
        log = re.sub(r"Frame count '\d+', Time \(ms\) '\d+'",
                     f"Frame count '1200', Time (ms) '{sum(samples) // 1000}'", log)
        (runs[0] / 'stdout.log').write_text(log)
        reseal(runs[0])
        rejected(group, 'short replay')
        restore()

        # A bad run cannot be hidden by two fast runs, even if the combined
        # duration easily exceeds the minimum and the other five pass.
        samples = [33334] * 2230
        path = runs[0] / BENCHMARK_FILES[-1]
        path.write_text('#  "Main loop(1)",\n0,\n' + ''.join(f'{v},\n' for v in samples))
        log = (runs[0] / 'stdout.log').read_text()
        log = re.sub(r"Time \(ms\) '\d+'", f"Time (ms) '{sum(samples) // 1000}'", log)
        (runs[0] / 'stdout.log').write_text(log)
        reseal(runs[0])
        bad = group()
        require(not bad['passed'] and len(bad['groups']['instrumented']['runs']) == 3,
                'failed repetition was dropped or averaged away')
        restore()

        # Sum intervals within runs only. Moving QPC origins days apart cannot
        # add duration. Boundary-trimmed traces just below 20 seconds each
        # fail the 60-second group floor despite passing every per-run check.
        for i in range(3):
            path = runs[i] / 'present.csv'
            lines = path.read_text().splitlines()
            index = lines.index('frame,qpc_begin,qpc_end,hresult') + 1
            rows = lines[index:-1][:2221]
            offset = (i + 1) * 100_000_000
            rows[-1] = f'2220,{offset + 37_000_000},{offset + 37_000_001},00000000'
            path.write_text('\n'.join(lines[:index] + rows + ['# capture_complete=2221']) + '\n')
            reseal(runs[i])
        insufficient = group()
        require(not insufficient['passed'] and not insufficient['errors'] and
                all(run['passed'] for run in insufficient['groups']['instrumented']['runs']) and
                insufficient['groups']['instrumented']['combined_present_seconds'] < 60,
                'inter-process gaps were counted as measured duration')
        restore()
        print('Kart complete replay groups, integrity, pacing and control comparison checks passed')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    verify = commands.add_parser('verify')
    verify.add_argument('--work', type=Path, default=WORK)
    commands.add_parser('self-test')
    prep = commands.add_parser('prepare')
    prep.add_argument('--work', type=Path, default=WORK)
    prep.add_argument('--download', action='store_true')
    prep.add_argument('--directx-dir', type=Path,
                      help='extracted x64/x86 DirectX DLL directories; default WORK/directx-runtime')
    commands.add_parser('build-probes')
    seal = commands.add_parser('seal-benchmark')
    seal.add_argument('--results', type=Path, required=True)
    seal.add_argument('--identity', type=Path, required=True)
    groups = commands.add_parser('assess-benchmarks')
    groups.add_argument('--instrumented', type=Path, nargs=3, required=True)
    groups.add_argument('--controls', type=Path, nargs=3, required=True)
    groups.add_argument('--output', type=Path, required=True)
    result = commands.add_parser('assess')
    result.add_argument('--results', type=Path, required=True)
    result.add_argument('--mode', choices=('race',), required=True)
    result.add_argument('--architecture', choices=('x64', 'x86'), required=True)
    args = parser.parse_args()
    if args.command == 'self-test':
        parser_checks()
        benchmark_group_checks()
        probe_checks()
    elif args.command == 'seal-benchmark':
        seal_benchmark(args)
    elif args.command == 'assess-benchmarks':
        assess_benchmarks(args)
    elif args.command == 'prepare':
        prepare(args)
    elif args.command == 'build-probes':
        build_probes(WORK)
        print('Kart Vista probe builds passed')
    elif args.command == 'verify':
        top = official(args.work)
        verify_selected(top, args.work / ARCHIVE)
        reports = {arch: pe_audit(top / 'stk-code' / f'build-{triplet}' / 'bin', arch)
                   for arch, triplet in [('x64', 'x86_64'), ('x86', 'i686')]}
        (args.work / 'pe-audit.json').write_text(json.dumps(reports, indent=2) + '\n')
        print('Kart official archive and PE closure checks passed')
    else:
        assess(args)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, ET.ParseError, subprocess.CalledProcessError) as error:
        sys.exit(f'Kart check failed: {error}')
