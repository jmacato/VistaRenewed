#!/usr/bin/env python3
"""Exercise production D3D9 query/state code with source-derived fault controls."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

from test_config import ARTIFACTS, HOST_PREFIX, QEMU_BUILD, native_headers, native_libraries, sdk_header, container_command, container_path, vista_compile_database

ROOT = Path(__file__).resolve().parents[2]
SOURCE = Path('triton-umd/src/virtio/neptune/vista-d3d9/triton9_query.cpp')


def run_query(source, negative=False):
    fixture = (ROOT / 'tests/vista/test-d3d9-pipeline.cpp').read_text()
    assert fixture.count('// PRODUCTION_QUERY_IMPLEMENTATION') == 1
    assert source.count('#include "triton9.h"') == 1
    combined = fixture.replace('// PRODUCTION_QUERY_IMPLEMENTATION',
                               source.replace('#include "triton9.h"', ''))
    with tempfile.TemporaryDirectory(prefix='triton9-query-') as tmp:
        cpp = Path(tmp) / 'query.cpp'
        exe = Path(tmp) / 'query-test'
        cpp.write_text(combined)
        subprocess.run([os.environ.get('CXX', 'clang++'), '-std=c++17', '-O1',
                        '-Wall', '-Wextra', '-Werror', '-fsanitize=undefined',
                        str(cpp), '-o', str(exe)], check=True)
        result = subprocess.run([str(exe)], capture_output=True, text=True)
        if negative:
            assert result.returncode != 0, 'missing timestamp support unexpectedly passed'
            assert 'triton9CreateQuery' in result.stderr, result.stderr
        else:
            assert result.returncode == 0, result.stderr
            assert 'D3D9 query behavior passed' in result.stdout
            print(result.stdout.strip())


def check_queries():
    source = (ROOT / SOURCE).read_text()
    run_query(source)
    mapping = 'case D3DDDIQUERYTYPE_TIMESTAMP: return D3D11_QUERY_TIMESTAMP;'
    assert source.count(mapping) == 1
    run_query(source.replace(mapping,
        'case D3DDDIQUERYTYPE_TIMESTAMP: return static_cast<D3D11_QUERY>(-1);'),
        negative=True)
    print('D3D9 query missing-timestamp negative control passed')


def state_source(source):
    names = ['FloatFromBits', 'FiniteFloatBits', 'SamplerIndex', 'MapBlend',
             'MapBlendOp', 'MapAddress', 'ValidMinMagFilter', 'ValidMipFilter',
             'MapFilter', 'ColorToFloat', 'AlphaBlendFactor', 'OpaqueDestinationBlend', 'OpaqueRenderTarget', 'CreateBlendState',
             'CreateRasterizerState', 'CreateSamplerState', 'MapComparison',
             'ValidStencilOperation', 'ValidFogMode', 'ValidFixedOperation',
             'ValidFixedArgument', 'ValidateRenderState']
    functions = []
    for name in names:
        match = re.search(r'static\s+[\w *]+\ntriton9' + name + r'\(', source)
        assert match, name
        body = source.index('{', match.start())
        depth = 1
        end = body + 1
        while depth:
            depth += (source[end] == '{') - (source[end] == '}')
            end += 1
        functions.append(source[match.start():end])
    return '\n'.join(functions)


def run_state():
    path = ROOT / SOURCE.parent / 'triton9_state.cpp'
    production = state_source(path.read_text())
    wdk = (sdk_header('d3dumddi.h', 'microsoft.windows.wdk.x64')).read_text()
    enums = []
    for name in ['D3DDDIRENDERSTATETYPE', 'D3DDDITEXTURESTAGESTATETYPE']:
        match = re.search(r'typedef enum _' + name + r'\b.*?\}\s*' + name + ';', wdk, re.S)
        assert match, name
        enums.append(match.group())
    fixture = (ROOT / 'tests/vista/test-d3d9-pipeline.cpp').read_text()
    fixture = fixture.replace('// DDI_ENUMS', '\n'.join(enums))
    prefix = HOST_PREFIX
    env = dict(os.environ, LD_LIBRARY_PATH=os.pathsep.join(map(str, native_libraries())),
               DXVK_WSI_DRIVER='Headless')
    with tempfile.TemporaryDirectory(prefix='triton9-state-') as tmp:
        cpp = Path(tmp) / 'state.cpp'
        exe = Path(tmp) / 'state'
        for negative in [None, "mrt", "opaque"]:
            code = production
            if negative == "mrt":
                assert 'desc.IndependentBlendEnable = TRUE;' in code
                code = code.replace('desc.IndependentBlendEnable = TRUE;',
                                    'desc.IndependentBlendEnable = FALSE;')
            elif negative == "opaque":
                assert 'if (!triton9OpaqueRenderTarget(device->renderTargets[index]))' in code
                code = code.replace('if (!triton9OpaqueRenderTarget(device->renderTargets[index]))',
                                    'if (true)')
            cpp.write_text(fixture.replace('// PRODUCTION_STATE_IMPLEMENTATION', code))
            subprocess.run([os.environ.get('CXX', 'clang++'), '-std=c++17', '-O1',
                '-DTRITON9_TEST_STATE', '-fsanitize=undefined',
                *native_headers(),
                '-I' + str(ROOT / 'triton-umd/src/virtio/neptune/triton'), str(cpp),
                *['-L'+str(p) for p in native_libraries()],
                '-ldxvk_d3d11', '-ldxvk_dxgi', '-o', str(exe)], check=True)
            result = subprocess.run([str(exe)], env=env, capture_output=True, text=True)
            if negative:
                assert result.returncode != 0, result.stderr
                expected = 'IndependentBlendEnable' if negative == "mrt" else 'SrcBlend==D3D11_BLEND_ONE'
                assert expected in result.stderr, result.stderr
            else:
                assert result.returncode == 0, result.stderr
                assert 'D3D9 native state behavior passed' in result.stdout
                print(result.stdout.strip())
    print('D3D9 state negative control passed')


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--cpu', action='store_true', help='Query behavior only; no GPU')
    mode.add_argument('--native', action='store_true', help='Also execute native GPU state tests')
    args = parser.parse_args()
    check_queries()
    if args.native:
        run_state()
