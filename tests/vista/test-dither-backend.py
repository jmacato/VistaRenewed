#!/usr/bin/env python3
"""Build isolated native DXVK and test the production private dither control."""
from pathlib import Path
import os
import subprocess
from test_config import ARTIFACTS, HOST_PREFIX, QEMU_BUILD, native_headers, native_libraries, sdk_header, container_command, container_path, vista_compile_database

import argparse
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--device', required=True, help='Hardware device-name filter')
parser.add_argument('--software-device', required=True, help='Software Vulkan device used by required fallback controls, e.g. llvmpipe')
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
evidence = ARTIFACTS / 'dither-backend'
evidence.mkdir(parents=True, exist_ok=True)
build = evidence / 'build'
a = root / 'triton-dxvk/src/d3d11/d3d11_dither_control.h'
b = root / 'triton-umd/src/virtio/neptune/triton/tritonDitherControl.h'
assert a.read_bytes() == b.read_bytes(), 'host/guest dither control ABI differs'
container = container_command()
relative = str(build.relative_to(root))
if not (build/'build.ninja').exists():
    subprocess.run(container+['meson','setup',relative,'triton-dxvk','--buildtype=release',
        '-Denable_d3d8=false','-Denable_d3d9=false','-Denable_d3d10=false',
        '-Dnative_headless=true','-Dnative_sdl2=disabled','-Dnative_sdl3=disabled','-Dnative_glfw=disabled'],check=True)
with (evidence/'build.log').open('w') as log:
    subprocess.run(container+['ninja','-C',relative,'-j','4'],stdout=log,stderr=subprocess.STDOUT,check=True)
exe = evidence/'test-dither-backend'
subprocess.run(['g++','-std=c++17','-O2',*native_headers(),
    str(root/'tests/vista/test-dither-backend.cpp'),'-L'+str(build/'src/d3d11'),
    '-L'+str(build/'src/dxgi'),'-ldxvk_d3d11','-ldxvk_dxgi','-o',str(exe)],check=True)
env = dict(os.environ, LD_LIBRARY_PATH=str(build/'src/d3d11')+':'+str(build/'src/dxgi'),
           DXVK_WSI_DRIVER='Headless',DXVK_LOG_PATH=str(evidence),DXVK_FILTER_DEVICE_NAME=args.device,
           DXVK_SHADER_CACHE_PATH=str(evidence/'cache'))
for name,config in [('libraries','dxvk.enableGraphicsPipelineLibrary = True'),
                    ('monolithic','dxvk.enableGraphicsPipelineLibrary = False'),
                    ('secondary','dxvk.tilerMode = True')]:
    run_env=dict(env,DXVK_CONFIG=config)
    with (evidence/(name+'.log')).open('w') as log:
        result=subprocess.run([str(exe)],env=run_env,stdout=log,stderr=subprocess.STDOUT,timeout=120)
    text=(evidence/(name+'.log')).read_text()
    print(name, text.splitlines()[-1] if text else '(empty log)')
    assert result.returncode==0, str(evidence/(name+'.log'))
# An independently exercised no-op control must fail the pixel oracle.
with (evidence/'negative-noop.log').open('w') as log:
    result=subprocess.run([str(exe),'--drop-pixel-enable'],env=env,stdout=log,stderr=subprocess.STDOUT,timeout=120)
assert result.returncode!=0, 'no-op dither negative control unexpectedly passed'
assert '[FAIL] dither_pixels: lowChanged>0' in (evidence/'negative-noop.log').read_text()
# llvmpipe lacks VK_EXT_legacy_dithering here. It still has to implement the
# control schema and explicitly reject enable without inventing support.
with (evidence/'unsupported.log').open('w') as log:
    result=subprocess.run([str(exe),'--unsupported'],env=dict(env,DXVK_FILTER_DEVICE_NAME=args.software_device),stdout=log,stderr=subprocess.STDOUT,timeout=120)
assert result.returncode==0, str(evidence/'unsupported.log')
print('Dither backend checks passed')
