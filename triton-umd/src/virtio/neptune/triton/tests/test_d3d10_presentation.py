#!/usr/bin/env python3
"""Run real production resource creation and resolve against native host D3D11."""
from pathlib import Path
import os
import sys
import re
import importlib.util
import subprocess
import tempfile
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[6] / 'tests/vista'))
from test_config import native_headers, native_libraries, vista_compile_database, container_command

ROOT = Path(__file__).resolve().parents[6]
TRITON = Path(__file__).resolve().parents[1]
DXVK = ROOT / 'triton-dxvk'
fixture = (TRITON / 'tests/tritonD3D10PresentTest.c').read_text()
kernel = (ROOT / 'triton-kmd/viogpu/viogpu3d/viogpu_allocation.cpp').read_text()
kernel = kernel[kernel.index('static BOOLEAN IsValidVistaSharedTexture('):kernel.index('BOOLEAN VioGpuAllocation::GetTransferLayout')]
kernel = '''
using BOOLEAN = unsigned char;
#define VIOGPU_TARGET_VISTA 1
#define MAXULONG UINT32_MAX
#define MAXULONGLONG UINT64_MAX
#define PAGE_SIZE 4096u
#define VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM 1
#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM 2
#define VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM 67
''' + kernel
fixture = fixture.replace('// KERNEL_SHARED_VALIDATOR', kernel)
header = (TRITON / 'triton.h').read_text(encoding='utf-8-sig')
structure = header[header.index('typedef struct TRITON_RESOURCE {'):header.index('typedef struct TRITON_RTVIEW {')]
dxgi = (TRITON / 'tritonDxgi.c').read_text()
resolve = dxgi[dxgi.index('static void\ntritonResolveUnpredicated'):dxgi.index('static HRESULT APIENTRY\ntritonDxgiPresent(')]
start = dxgi.index('   {\n      PTRITON_RESOURCE r0', dxgi.index(' * import rig) together'))
rotation = dxgi[start:dxgi.index('   /* Host views', start)]
shared = (TRITON.parent / 'npt_shared_texture.c').read_text()
start = shared.index('uint32_t\nnpt_shared_texture_host_format')
end = shared.index('\n}', shared.index('uint32_t\nnpt_shared_texture_virgl_format', start)) + 2
fixture = fixture.replace('// RESOURCE_STRUCTURE', structure).replace('// HOST_FORMAT_IMPLEMENTATION', shared[start:end].replace('npt_log(', 'TR_LOG('))
fixture = fixture.replace('// RESOLVE_IMPLEMENTATION', resolve).replace('// ROTATE_IMPLEMENTATION', rotation)
blit = dxgi[dxgi.index('static BOOL\ntritonBlitEnsureLocked'):dxgi.index('static HRESULT APIENTRY\ntritonDxgiBlt(')]
callbacks = sorted(set(re.findall(r'cb->(pfnState\w+)', blit)))
fixture = fixture.replace('// BLIT_CALLBACKS', 'struct D3D11DDI_CORELAYER_DEVICECALLBACKS {' + ''.join('void (*'+name+')(Handle,...);' for name in callbacks) + '};')
macros = sorted(set(re.findall(r'ID3D11\w+_\w+(?=\()', blit)) - set(re.findall(r'#define (\w+)', fixture)))
fixture = fixture.replace('// BLIT_MACROS', '\n'.join('#define '+name+'(p,...) (p)->'+name.split('_',1)[1]+'(__VA_ARGS__)' for name in macros))
view = (TRITON / 'tritonView.c').read_text(encoding='utf-8-sig')
start = view.index('DXGI_FORMAT\ntritonResourceHostViewFormat')
view = view[start:view.index('/* ---------- per-resource', start)]
fixture = fixture.replace('// BLIT_IMPLEMENTATION', view + blit)
spec = importlib.util.spec_from_file_location('d3d10_checks', ROOT / 'tests/vista/run-d3d10-compat.py')
module = importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
shader_fixture, production, helper, windows = module.shader_inputs()
shader_source = shader_fixture[:shader_fixture.index('int main(void)')].replace('// PRODUCTION_DXBC_IMPLEMENTATION',production).replace('// PRODUCTION_D3D10_SHADER_HELPERS',helper)
env = os.environ.copy()
env['LD_LIBRARY_PATH'] = os.pathsep.join(map(str, native_libraries()))
env['DXVK_LOG_LEVEL'] = 'error'
env['DXVK_WSI_DRIVER'] = 'Headless'
for variant in (('current',) if '--compile-only' in sys.argv else ('current', 'multisample-export', 'ignored-registration-failure', 'srgb-resolve', 'predicated-copy', 'shared-rgba-rejected', 'linear-hint-persisted')):
    negative = variant != 'current'
    resource = (TRITON / 'tritonResource.c').read_text(encoding='utf-8-sig')
    resource = resource[resource.index('static void tritonTranslateUsage'):resource.index('static BOOL\ntritonSynthesizeStandardTexture')]
    with tempfile.TemporaryDirectory(prefix='d3d10-present-') as temporary:
        work = Path(temporary)
        source = work / 'test.cpp'
        code = fixture.replace('// RESOURCE_IMPLEMENTATION', resource)
        if variant == 'multisample-export':
            old = 'ID3D11Resource *exportResource = r->pPresentResource ? r->pPresentResource : r->pResource;'
            assert old in code
            code = code.replace(old, 'ID3D11Resource *exportResource = r->pResource;')
        if variant == 'ignored-registration-failure':
            old = 'tritonSetError(pD, hr);'
            assert old in code
            code = code.replace(old, '(void)hr;')

        if variant == 'shared-rgba-rejected':
            assert 'else if (options->format == dxgiRgba8)' in code
            code = code.replace('else if (options->format == dxgiRgba8)', 'else if (false && options->format == dxgiRgba8)')
        if variant == 'linear-hint-persisted':
            pattern = r'd3d11Misc &\s*~\(TRITON_D3D11_MISC_LINEAR_EXPORT \| TRITON_D3D11_MISC_SINGLE_PLANE_EXPORT\)'
            assert re.search(pattern, code)
            code = re.sub(pattern, 'd3d11Misc', code)
        if variant == 'srgb-resolve':
            old = '? r->Format : r->PresentFormat;'
            assert old in code
            code = code.replace(old, '? r->PresentFormat : r->PresentFormat;')
        if variant == 'predicated-copy':
            at = code.index('static HRESULT\ntritonDxgiBltCommon')
            old = 'ID3D11DeviceContext1_SetPredication(pD->pCtx1, NULL, FALSE);'
            head, tail = code[:at], code[at:]
            assert old in tail
            code = head + tail.replace(old, '', 1)
        source.write_text(code)
        (work/'windows.h').write_text(windows)
        (work/'shader.c').write_text(shader_source)
        subprocess.run(['clang','-std=gnu11','-O1','-I'+str(work),'-I'+str(TRITON),'-c',str(work/'shader.c'),'-o',str(work/'shader.o')],check=True)
        command = ['g++' ,'-std=c++17','-include','initializer_list','-I'+str(TRITON)]
        command += ['-I'+str(DXVK / 'include/native' / p) for p in ('directx','windows')]
        command += [str(source),str(work/'shader.o'),'-o',str(work/'test')]
        command += ['-L'+str(p) for p in native_libraries()]
        command += ['-ldxvk_d3d11','-ldxvk_dxgi']
        subprocess.run(command,check=True)
        if '--compile-only' in sys.argv:
            print('D3D10 presentation actual-header compilation passed; GPU execution not requested')
            continue
        for case in (('msaa','registration','shared') if variant == 'current' else ('registration',) if variant == 'ignored-registration-failure' else ('msaa',)):
            result = subprocess.run([str(work/'test'),case],env=env,capture_output=True,text=True)
            if negative:
                assert result.returncode != 0 and 'FAIL:' in result.stderr, result.stdout+result.stderr
                print('D3D10 '+variant+' '+case+' negative control passed')
            else:
                assert result.returncode == 0, result.stdout+result.stderr
                print(result.stdout.strip())
