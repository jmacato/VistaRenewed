#!/usr/bin/env python3
"""Exercise the actual host sharing endpoints against native D3D11 devices."""
from pathlib import Path
import os
import subprocess
import tempfile

import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[6] / 'tests/vista'))
from test_config import native_headers, native_libraries, vista_compile_database, container_command

ROOT = Path(__file__).resolve().parents[6]
HOST = ROOT / 'triton-virglrenderer/src/neptune'
TRITON = Path(__file__).resolve().parents[1]
DXVK = ROOT / 'triton-dxvk'
source = (HOST / 'npt_shared.c').read_text()
mapping = source[source.index('static uint32_t\nnpt_shared_dxgi_to_virgl_format'):source.index('#ifdef __APPLE__', source.index('static uint32_t\nnpt_shared_dxgi_to_virgl_format'))]
validation = source[source.index('static bool\nnpt_shared_linear_desc_valid'):source.index('/* Restrict imports to')]
endpoints = source[source.index('/* Duplicate the attached resource fd'):]
wire = (HOST / 'npt_transport_defs.h').read_text()
blob = wire[wire.index('struct npt_blob_export_info {'):wire.index('/* Synchronous.', wire.index('struct npt_blob_export_info {'))]
command = wire[wire.index('struct npt_cmd_shared_open_res {'):wire.index('struct npt_cmd_shared_open_res_reply {')]
protocol = (HOST / 'neptune-protocol/npt_protocol_defs.h').read_text()
start = protocol.index('struct npt_command_header {')
command = protocol[start:protocol.index('};', start) + 2] + command
fixture = (TRITON / 'tests/tritonD3D10SharedTest.c').read_text()
layout_header = (ROOT / 'triton-virglrenderer/src/virgl_resource.h').read_text()
layout_start = layout_header.index('struct virgl_attachment_layout {')
layout = layout_header[layout_start:layout_header.index('};', layout_start) + 2]
fixture = fixture.replace('// HOST_LAYOUT', layout)
env = dict(os.environ, LD_LIBRARY_PATH=os.pathsep.join(map(str, native_libraries())),
           DXVK_WSI_DRIVER='Headless', DXVK_LOG_LEVEL='error')
for variant in ('current', 'missing-wide-format', 'short-wide-row'):
    production = mapping + validation + endpoints
    if variant == 'missing-wide-format':
        production = production.replace('case 10: /* DXGI_FORMAT_R16G16B16A16_FLOAT */', 'case 999: /* removed RGBA16F */')
    if variant == 'short-wide-row':
        assert '(format == 10 ? 8 : 4)' in production
        production = production.replace('(format == 10 ? 8 : 4)', '4')
    with tempfile.TemporaryDirectory(prefix='triton-sharing-') as temporary:
        work = Path(temporary)
        (work / 'test.cpp').write_text(fixture.replace('// WIRE_TYPES', blob + command).replace('// PRODUCTION_HOST_SHARED', production))
        compile_command = ['g++', '-std=c++17', '-O1', '-g', '-fpermissive', '-include', 'initializer_list',
                           '-I' + str(ROOT / 'triton-umd/src'), '-I' + str(DXVK / 'include/native'),
                           '-I' + str(DXVK / 'include/native/directx'), '-I' + str(DXVK / 'include/native/windows'),
                           str(work / 'test.cpp'), '-o', str(work / 'test'),
                           *['-L' + str(p) for p in native_libraries()],
                           '-ldxvk_d3d11', '-ldxvk_dxgi', '-pthread']
        result = subprocess.run(compile_command, capture_output=True, text=True)
        assert result.returncode == 0, result.stdout + result.stderr
        result = subprocess.run([str(work / 'test')], env=env, capture_output=True, text=True)
        if variant == 'current':
            assert result.returncode == 0, result.stdout + result.stderr
            print(result.stdout.strip())
        else:
            assert result.returncode != 0 and 'FAIL shared' in result.stderr, result.stdout + result.stderr
            print('D3D10 production sharing ' + variant + ' negative control passed')
