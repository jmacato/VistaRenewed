#!/usr/bin/env python3
"""Build native DXVK and exercise the real color-copy ABI on the selected GPU.

Run only outside VM performance captures. The fixture's explicit readbacks are
test observations and do not establish game performance or a CPU-free VM path.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import time

from test_config import ARTIFACTS, HOST_PREFIX, QEMU_BUILD, native_headers, native_libraries, sdk_header, container_command, container_path, vista_compile_database

root = Path(__file__).resolve().parents[2]
default_evidence = ARTIFACTS / 'backend'
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--evidence', type=Path, default=default_evidence)
parser.add_argument('--build-dir', type=Path)
parser.add_argument('--skip-build', action='store_true')
parser.add_argument('--device', required=True, help='Explicit DXVK device-name filter')
parser.add_argument('--timeout', type=float, default=120)
args = parser.parse_args()
evidence = args.evidence.resolve()
evidence.mkdir(parents=True, exist_ok=True)
build = (args.build_dir or evidence / 'build').resolve()
if not args.skip_build:
    relative = str(build.relative_to(root))
    container = container_command()
    with (evidence / 'build.log').open('w') as log:
        if not (build / 'build.ninja').exists():
            subprocess.run(container + ['meson', 'setup', relative, 'triton-dxvk',
                '--buildtype=release', '-Denable_d3d8=false', '-Denable_d3d9=false',
                '-Denable_d3d10=false', '-Dnative_headless=true',
                '-Dnative_sdl2=disabled', '-Dnative_sdl3=disabled',
                '-Dnative_glfw=disabled'], stdout=log, stderr=subprocess.STDOUT,
                check=True)
        subprocess.run(container + ['ninja', '-C', relative, '-j', '4'],
                       stdout=log, stderr=subprocess.STDOUT, check=True)

exe = evidence / 'test-color-copy-backend'
with (evidence / 'fixture-build.log').open('w') as log:
    subprocess.run(['g++', '-std=c++17', '-O2',
        '-I' + str(root / 'triton-dxvk/include/native'),
        *native_headers(),
        str(root / 'tests/vista/test-color-copy-backend.cpp'),
        '-L' + str(build / 'src/d3d11'), '-L' + str(build / 'src/dxgi'),
        '-ldxvk_d3d11', '-ldxvk_dxgi', '-o', str(exe)], stdout=log,
        stderr=subprocess.STDOUT, check=True)

# Do not inherit another test's Vulkan proxy or fault injection. Device choice
# is explicit and recorded so a software fallback cannot masquerade as NVIDIA.
env = {k: v for k, v in os.environ.items()
       if not k.startswith('PREDICATION_VK_') and k not in ('DXVK_CONFIG', 'LD_PRELOAD')}
env.update(LD_LIBRARY_PATH=':'.join(str(build / 'src' / p) for p in ('d3d11', 'dxgi')),
           DXVK_WSI_DRIVER='Headless', DXVK_FILTER_DEVICE_NAME=args.device,
           DXVK_SHADER_CACHE_PATH=str(evidence / 'cache'))
results = []
cases = [
    ('default', [], '', True),
    ('monolithic', [], 'dxvk.enableGraphicsPipelineLibrary = False', True),
    ('secondary', [], 'dxvk.tilerMode = True', True),
    ('negative-drop-copy', ['--drop-copy'], '', False),
    ('negative-query-pollution', ['--query-pollution'], '', False),
]
for name, options, config, expected_success in cases:
    directory = evidence / name
    directory.mkdir(exist_ok=True)
    run_env = dict(env, DXVK_CONFIG=config, DXVK_LOG_PATH=str(directory))
    start = time.monotonic()
    result = {'case': name, 'device_filter': args.device,
              'expected_success': expected_success, 'state': 'RUNNING'}
    results.append(result)
    try:
        with (directory / 'run.log').open('w') as log:
            completed = subprocess.run([str(exe)] + options, env=run_env,
                stdout=log, stderr=subprocess.STDOUT, timeout=args.timeout)
        output = (directory / 'run.log').read_text()
        summary = re.search(r'\[COLOR-COPY-SUMMARY\] passed=(\d+) failed=(\d+)', output)
        result.update(returncode=completed.returncode,
                      summary=summary.group(0) if summary else None,
                      seconds=time.monotonic() - start)
        assert summary and int(summary[1]) > 0, str(directory / 'run.log')
        assert (completed.returncode == 0) == expected_success, str(directory / 'run.log')
        assert (int(summary[2]) == 0) == expected_success, str(directory / 'run.log')
        if name == 'negative-drop-copy':
            assert '[FAIL] color_copy_pixels: exactPixels' in output
        if name == 'negative-query-pollution':
            assert '[FAIL] color_copy_queries: stats[mode].IAVertices == 12' in output
            assert '[FAIL] color_copy_queries: std::memcmp' in output
        result['state'] = 'PASS'
        print(name, summary.group(0), flush=True)
    except BaseException as error:
        result.update(state='FAILED', error=str(error), seconds=time.monotonic() - start)
        raise
    finally:
        (evidence / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
print('Color-copy backend checks passed')
