#!/usr/bin/env python3
"""Host production-code checks; PE/runtime checks are separate guest gates."""
from pathlib import Path
import argparse
import json
import shlex
import os
import re
import subprocess
import tempfile

from test_config import ARTIFACTS, HOST_PREFIX, QEMU_BUILD, native_headers, native_libraries, sdk_header, container_command, container_path, vista_compile_database

ROOT = Path(__file__).resolve().parents[2]
TRITON = ROOT / 'triton-umd/src/virtio/neptune/triton'


def compile_run(text, label, negative=False, headers=None):
    with tempfile.TemporaryDirectory(prefix='triton10-') as temporary:
        work = Path(temporary)
        for name, content in (headers or {}).items():
            (work / name).write_text(content)
        source = work / 'test.c'
        source.write_text(text)
        subprocess.run([os.environ.get('CC', 'clang'), '-std=gnu11', '-O1',
                        '-g', '-Wall', '-Wextra', '-Wno-unused-parameter',
                        '-Wno-unused-function', '-Werror', '-fsanitize=undefined,address',
                        '-I', str(work), '-I', str(TRITON), str(source),
                        '-o', str(work / 'test')], check=True)
        result = subprocess.run([str(work / 'test')], capture_output=True, text=True)
        if negative:
            assert result.returncode != 0 and 'FAIL' in result.stderr, result
            print(label + ' negative control passed')
        else:
            assert result.returncode == 0, result.stdout + result.stderr
            print(result.stdout.strip())


def query():
    fixture = (TRITON / 'tests/tritonD3D10QueryTest.c').read_text()
    original = (TRITON / 'tritonQuery.c').read_text()
    original = original[original.index('static D3D11_QUERY'):original.index('/* ---------- Format')]
    bad_predicate = 'if (d.Query == D3D11_QUERY_OCCLUSION_PREDICATE ||'
    assert bad_predicate in original
    for source, negative in [(original, False), (original.replace(bad_predicate, 'if (FALSE ||'), True)]:
        compile_run(fixture.replace('// PRODUCTION_QUERY_IMPLEMENTATION', source), 'query predicate-kind', negative)





def caps():
    fixture = (TRITON / 'tests/tritonD3D10CapsTest.c').read_text()
    original = (TRITON / 'tritonDDI.c').read_text(encoding='utf-8-sig')
    original = original[original.index('static HRESULT APIENTRY tritonGetSupportedVersions'):original.index('/* ---------- OpenAdapter10_2')]
    guard = 'if (pArgs->DataSize < required) return E_INVALIDARG;'
    assert guard in original
    for source, negative in [(original, False), (original.replace(guard, 'if (pArgs->DataSize < required && pArgs->DataSize == UINT32_MAX) return E_INVALIDARG;'), True)]:
        compile_run(fixture.replace('// PRODUCTION_CAPS_IMPLEMENTATION', source), 'caps short-buffer', negative)
        if not negative:
            compile_run('#define NPT_D3D10_RUNTIME_DDI\n' + fixture.replace(
                '// PRODUCTION_CAPS_IMPLEMENTATION', source), 'Vista caps')


def adapters():
    fixture = (TRITON / 'tests/tritonD3D10AdaptersTest.c').read_text()
    source = (TRITON / 'tritonD3D10.c').read_text()
    source = source[source.index('extern void APIENTRY'):source.index('void tritonFillD3D10DeviceFuncs')]
    compile_run(fixture.replace('// PRODUCTION_ADAPTER_IMPLEMENTATION', source), 'adapters')
    mutated = source.replace('up.TexCube.NumCubes = 1;', 'up.TexCube.NumCubes = 0;')
    compile_run(fixture.replace('// PRODUCTION_ADAPTER_IMPLEMENTATION', mutated),
                'missing legacy cube expansion', True)


def shader_inputs():
    fixture = (TRITON / 'tests/tritonD3D10DxbcTest.c').read_text()
    production = (TRITON / 'tritonDxbc.c').read_text(encoding='utf-8-sig')
    production = re.sub(r'^#include "(?:triton_log|npt_workaround)\.h".*$', '', production, flags=re.M)
    helper = (TRITON / 'tritonD3D10.c').read_text()
    helper = helper[helper.index('void *tritonD3D10BuildSOAlias'):]
    windows = """#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
typedef uint32_t UINT; typedef unsigned char BYTE; typedef size_t SIZE_T; typedef int BOOL;
#define TRUE 1
#define FALSE 0
#define HEAP_ZERO_MEMORY 8
#define ZeroMemory(p,n) memset(p,0,n)
static void *GetProcessHeap(void) {return NULL;}
static void *HeapAlloc(void *h,UINT flags,SIZE_T n) {(void)h;return flags?calloc(1,n):malloc(n);}
static void HeapFree(void *h,UINT flags,void *p) {(void)h;(void)flags;free(p);}
"""
    return fixture, production, helper, windows


def shaders():
    fixture, production, helper, windows = shader_inputs()
    text = fixture.replace('// PRODUCTION_DXBC_IMPLEMENTATION', production).replace(
        '// PRODUCTION_D3D10_SHADER_HELPERS', helper)
    compile_run(text, 'DXBC', headers={'windows.h': windows})
    # A version-only relabel leaves SM5 BFI behind; the independent opcode
    # validator must reject it even though its DXBC checksum is valid.
    mutated = helper.replace('if (opcode == 140)', 'if (opcode == 999)').replace(
        'if (opcode > 106 || (input[at] & 0x80000000u)) goto invalid;', '')
    compile_run(fixture.replace('// PRODUCTION_DXBC_IMPLEMENTATION', production).replace(
        '// PRODUCTION_D3D10_SHADER_HELPERS', mutated), 'SM5 relabel', True,
        {'windows.h': windows})



def abi():
    # Compile production assignments against the actual WDK twice. Native
    # shim execution checks values, while these checks pin the Windows ABI.
    for arch in ('x64', 'x86'):
        database = vista_compile_database(arch)
        entries = json.loads(database.read_text())
        entry = next(e for e in entries if e['file'].endswith('/triton/tritonD3D10.c')
                     and 'vista-d3d10' in e['command'])
        args = shlex.split(entry['command'])
        filtered = []
        i = 0
        while i < len(args):
            if args[i] in ('-o', '-MF', '-MQ', '-MT'):
                i += 2
                continue
            if args[i] in ('-c', '-MD', '-MMD') or args[i] == entry['file']:
                i += 1
                continue
            filtered.append(args[i])
            i += 1
        filtered += ['-Werror=incompatible-pointer-types', '-Werror=cast-function-type',
                     '-Wno-missing-prototypes', '-fsyntax-only', '-x', 'c', '-']
        source = '#include "/workspace/triton-umd/src/virtio/neptune/triton/tritonD3D10.c"\n'
        source += '_Static_assert(sizeof(D3D10DDIARG_SIGNATURE_ENTRY)==12,"signature ABI");\n'
        for callback, function in [('PFND3D10DDI_CREATERESOURCE', 'create_resource'),
                ('PFND3D10DDI_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT', 'create_so'),
                ('PFND3D10DDI_CALCPRIVATEGEOMETRYSHADERWITHSTREAMOUTPUT', 'so_size'),
                ('PFND3D10DDI_SETRENDERTARGETS', 'set_targets')]:
            source += f'_Static_assert(__builtin_types_compatible_p({callback}, __typeof__(&{function})),"{callback}");\n'
        command = container_command(*filtered, workdir=entry['directory'])
        result = subprocess.run(command, input=source, capture_output=True, text=True)
        assert result.returncode == 0, result.stdout + result.stderr
        bad = source + 'void bad(D3D10DDI_DEVICEFUNCS *p,D3D11DDI_DEVICEFUNCS *q){p->pfnCreateResource=q->pfnCreateResource;}\n'
        result = subprocess.run(command, input=bad, capture_output=True, text=True)
        assert result.returncode != 0 and 'incompatible' in result.stderr, result.stderr
        print(f'D3D10 {arch} WDK ABI checks and wrong-table negative control passed')
        modern = [a for a in filtered[:-3] if not a.startswith((
            '-DD3D_UMD_INTERFACE_VERSION', '-DDXGKDDI_INTERFACE_VERSION',
            '-DNPT_D3D10_RUNTIME_DDI'))]
        # Modern WDK SAL has one annotation absent from MinGW's sal.h.
        # This annotation is inert for C type checking; no ABI type is mocked.
        modern += ['-D_Maybenull_=', '-fsyntax-only']
        modern += ['/workspace/triton-umd/src/virtio/neptune/triton/' + name
                   for name in ('tritonDDI.c', 'tritonD3D10.c', 'tritonShader.c',
                                'tritonQuery.c', 'tritonResource.c', 'tritonDxgi.c')]
        command = container_command(*modern, workdir=entry['directory'])
        result = subprocess.run(command, capture_output=True, text=True)
        assert result.returncode == 0, result.stdout + result.stderr
        print(f'D3D11+ {arch} shared-source ABI regression compilation passed')



def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--host', action='store_true')
    mode.add_argument('--cpu', action='store_true', help='CPU behavioral checks; excludes GPU and Windows ABI builds')
    args = parser.parse_args()
    subprocess.run(['python3', str(TRITON / 'tests/test_d3d10_runtime_ownership.py')], check=True)
    subprocess.run(['python3', str(TRITON / 'tests/test_d3d10_protocol.py')], check=True)
    subprocess.run(['python3', str(TRITON / 'tests/test_d3d10_standard_import.py')], check=True)
    subprocess.run(['python3', str(TRITON / 'tests/test_d3d10_copy_conversion.py')], check=True)
    subprocess.run(['python3', str(TRITON / 'tests/test_d3d10_upload.py')], check=True)
    subprocess.run(['python3', str(TRITON / 'tests/test_d3d10_update_transport.py')], check=True)
    subprocess.run(['python3', str(TRITON / 'tests/test_d3d10_import_metadata.py')], check=True)
    subprocess.run(['python3', str(TRITON / 'tests/test_d3d10_initial_transport.py')], check=True)
    subprocess.run(['python3', str(ROOT / 'tests/vista/test-map-lifecycle.py')], check=True)
    if not args.cpu:
        subprocess.run(['python3', str(TRITON / 'tests/test_d3d10_presentation.py')], check=True)
        subprocess.run(['python3', str(TRITON / 'tests/test_d3d10_sharing.py')], check=True)
    subprocess.run(['python3', str(TRITON / 'tests/test_d3d10_shared_open.py')], check=True)
    subprocess.run(['python3', str(ROOT / 'tests/vista/test-d3d10-texture-map.py')], check=True)
    query()
    caps()
    subprocess.run(['python3', str(TRITON / 'tests/test_d3d10_extended_versions.py')] +
                   ([] if args.cpu else ['--abi']), check=True)
    subprocess.run(['python3', str(TRITON / 'tests/test_d3d10_format_caps.py')], check=True)
    shaders()
    adapters()
    if args.cpu:
        print('D3D10 CPU checks passed; GPU, ABI and Vista checks remain separate')
        return
    abi()
    subprocess.run(['python3', str(TRITON / 'tests/test_d3d10_blit_generation.py')], check=True)
    subprocess.run(['python3', str(ROOT / 'triton-kmd/viogpu/tools/test_d3d10_package.py')], check=True)
    subprocess.run(['python3', str(ROOT / 'triton-kmd/viogpu/tools/test_d3d10_deployment.py')], check=True)
    subprocess.run(['python3', str(ROOT / 'triton-kmd/viogpu/tools/test_d3d10_shared_allocation.py')], check=True)
    subprocess.run(['python3', str(ROOT / 'triton-kmd/viogpu/tools/test_d3d10_version.py')], check=True)
    print('D3D10 host checks passed')


if __name__ == '__main__':
    main()
