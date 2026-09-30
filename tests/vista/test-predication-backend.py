#!/usr/bin/env python3
"""Build isolated production DXVK; exercise GPU predication and CPU fallback."""
from pathlib import Path
import os,re,subprocess,time
from test_config import ARTIFACTS, HOST_PREFIX, QEMU_BUILD, native_headers, native_libraries, sdk_header, container_command, container_path, vista_compile_database

import argparse
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--device', required=True, help='Hardware device-name filter')
parser.add_argument('--software-device', required=True, help='Software Vulkan device used by required fallback controls, e.g. llvmpipe')
args = parser.parse_args()
root=Path(__file__).resolve().parents[2]
evidence=ARTIFACTS / 'predication'
build=evidence/'build';evidence.mkdir(parents=True,exist_ok=True)
container=container_command()
relative=str(build.relative_to(root))
if not (build/'build.ninja').exists():
    subprocess.run(container+['meson','setup',relative,'triton-dxvk','--buildtype=release','-Denable_d3d8=false','-Denable_d3d9=false','-Denable_d3d10=false','-Dnative_headless=true','-Dnative_sdl2=disabled','-Dnative_sdl3=disabled','-Dnative_glfw=disabled'],check=True)
with (evidence/'build.log').open('w') as log:
    subprocess.run(container+['ninja','-C',relative,'-j','4'],stdout=log,stderr=subprocess.STDOUT,check=True)
exe=evidence/'test-predication-backend'
subprocess.run(['g++','-std=c++17','-O2',*native_headers(),str(root/'tests/vista/test-predication-backend.cpp'),'-L'+str(build/'src/d3d11'),'-L'+str(build/'src/dxgi'),'-ldxvk_d3d11','-ldxvk_dxgi','-o',str(exe)],check=True)
proxy=evidence/'vulkan-proxy';proxy.mkdir(exist_ok=True)
subprocess.run(['g++','-std=c++17','-shared','-fPIC','-I'+str(root/'triton-dxvk/include/vulkan/include'),str(root/'tests/vista/predication-vulkan-proxy.cpp'),'-ldl','-o',str(proxy/'libvulkan.so')],check=True)
# Do not inherit fault injection or device-selection settings from another test.
base_env = {k:v for k,v in os.environ.items() if not k.startswith('PREDICATION_VK_') and k != 'DXVK_CONFIG'}
env=dict(base_env,LD_LIBRARY_PATH=':'.join(map(str,[proxy,build/'src/d3d11',build/'src/dxgi'])),DXVK_WSI_DRIVER='Headless',DXVK_LOG_PATH=str(evidence),DXVK_FILTER_DEVICE_NAME=args.device,DXVK_SHADER_CACHE_PATH=str(evidence/'cache'))
for name,extra,success in [('software',{'DXVK_FILTER_DEVICE_NAME':args.software_device},True),('native',{},True),('secondary',{'DXVK_CONFIG':'dxvk.tilerMode = True'},True),('fallback',{'PREDICATION_VK_HIDE':'1'},True),('negative-drop-gpu',{'PREDICATION_VK_DROP':'1'},False)]:
    start=time.monotonic()
    with (evidence/(name+'.log')).open('w') as log:
        result=subprocess.run([str(exe)],env=dict(env,**extra),stdout=log,stderr=subprocess.STDOUT,timeout=120)
    text=(evidence/(name+'.log')).read_text();summary=re.search(r'\[PREDICATION-SUMMARY\] passed=(\d+) failed=(\d+)',text)
    print(name,summary.group(0) if summary else '(no summary)',f'seconds={time.monotonic()-start:.3f}')
    assert summary and (result.returncode==0)==success,str(evidence/(name+'.log'))
    assert int(summary[1]) > 0 and (int(summary[2]) == 0) == success, name
    count=re.search(r'\[PREDICATE-VULKAN\] blocks=(\d+)',text);assert count,text[-1000:]
    assert (int(count[1])==0)==(name=='fallback'),name
    byte_count=re.search(r'byte_count_calls=(\d+) invalid_byte_count_calls=(\d+)',text)
    assert byte_count and int(byte_count[1]) > 0 and int(byte_count[2]) == 0,name
    assert '[DRAWAUTO-OFFSET] format=R8 offset=1 ' in text,name
    assert '[DRAWAUTO-OFFSET] format=R16 offset=2 ' in text,name
    assert '[DRAWAUTO-OFFSET] format=R8 offset=193 expected=0 observed=0' in text,name
    assert '[DRAWAUTO-OFFSET] format=R16 offset=196 expected=0 observed=0' in text,name
    assert '[PASS] draw_auto_offsets: c.device->GetDeviceRemovedReason()==S_OK' in text,name
    if not success:assert '[FAIL] predicate_draw: ok' in text
print('Predication backend checks passed')
