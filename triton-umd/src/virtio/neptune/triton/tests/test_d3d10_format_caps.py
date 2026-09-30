#!/usr/bin/env python3
"""Check actual format DDI callbacks against a split host typeless family."""
from pathlib import Path
import subprocess
import tempfile

import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[6] / 'tests/vista'))
from test_config import native_headers, native_libraries, vista_compile_database, container_command

ROOT = Path(__file__).resolve().parents[6]
TRITON = ROOT / 'triton-umd/src/virtio/neptune/triton'
source = (TRITON / 'tritonQuery.c').read_text()
source = source[source.index('/* ---------- Format'):source.index('/* CheckCounter*')]
fixture = r'''
#include <d3d11.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#define APIENTRY
#define D3D10_DDI_FORMAT_SUPPORT_SHADER_SAMPLE 1u
#define D3D10_DDI_FORMAT_SUPPORT_RENDERTARGET 2u
#define D3D10_DDI_FORMAT_SUPPORT_BLENDABLE 4u
#define D3D10_DDI_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET 8u
#define D3D11_1DDI_FORMAT_SUPPORT_VERTEX_BUFFER 256u
#define TR_LOG(...) ((void)0)
#define InterlockedExchange(ptr, value) (*(ptr) = (value))
struct TRITON_DEVICE { void *pDev1; };
typedef TRITON_DEVICE *PTRITON_DEVICE;
struct D3D10DDI_HDEVICE { void *pDrvPrivate; };
static UINT supports[256], qualities[256][33];
static unsigned host_calls;
static HRESULT host_support(void *dev, DXGI_FORMAT format, UINT *out)
{ (void)dev; ++host_calls; *out = supports[format]; return S_OK; }
static HRESULT host_quality(void *dev, DXGI_FORMAT format, UINT count, UINT *out)
{ (void)dev; ++host_calls; *out = qualities[format][count]; return S_OK; }
static HRESULT host_feature(void *dev, D3D11_FEATURE feature, void *out, UINT size)
{ (void)dev; (void)out; (void)size; assert(feature == D3D11_FEATURE_FORMAT_SUPPORT2); return E_FAIL; }
#define ID3D11Device1_CheckFormatSupport host_support
#define ID3D11Device1_CheckMultisampleQualityLevels host_quality
#define ID3D11Device1_CheckFeatureSupport host_feature
#include <cassert>
/* PRODUCTION */
static void check(bool condition, const char *message)
{ if (!condition) { std::fprintf(stderr, "FORMAT FAIL: %s\n", message); std::exit(1); } }
static void reset(void)
{
    std::memset((void *)s_msaaCache, 0, sizeof(s_msaaCache));
    std::memset((void *)s_fmtCache, 0, sizeof(s_fmtCache));
    std::memset(supports, 0, sizeof(supports));
    std::memset(qualities, 0, sizeof(qualities)); host_calls = 0;
}
int main(void)
{
    TRITON_DEVICE device = {}; D3D10DDI_HDEVICE handle = {&device};
    const UINT sampled = D3D11_FORMAT_SUPPORT_SHADER_SAMPLE | D3D11_FORMAT_SUPPORT_IA_VERTEX_BUFFER;
    const UINT target = D3D11_FORMAT_SUPPORT_RENDER_TARGET | D3D11_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET;
    reset();
    for (unsigned f = 5; f <= 8; ++f) supports[f] = sampled;
    supports[6] |= target | D3D11_FORMAT_SUPPORT_BLENDABLE;
    qualities[6][2] = qualities[6][4] = 1;
    for (unsigned f = 5; f <= 8; ++f) {
        UINT caps = 0, quality = 99;
        tritonCheckFormatSupport(handle, (DXGI_FORMAT)f, &caps);
        check(!(caps & 8), "typed MSAA escaped unsupported typeless parent");
        check(!(caps & ~31u), "Vista callback returned a later DDI capability bit");
        for (UINT count : {2u, 4u, 8u}) {
            tritonCheckMultisampleQualityLevels(handle, (DXGI_FORMAT)f, count, &quality);
            check(quality == 0, "family quality is inconsistent with allocation support");
        }
    }
    UINT caps = 0, quality = 99;
    tritonCheckFormatSupport(handle, DXGI_FORMAT_R32G32B32_FLOAT, &caps);
    check((caps & 7) == 7, "valid single-sample RGB32 capabilities were lost");
    reset();
    for (unsigned f = 1; f <= 4; ++f) {
        supports[f] = sampled | target;
        qualities[f][2] = qualities[f][4] = 1;
    }
    tritonCheckFormatSupport(handle, DXGI_FORMAT_R32G32B32A32_FLOAT, &caps);
    check((caps & 8) != 0, "supported family lost MSAA");
    tritonCheckMultisampleQualityLevels(handle, DXGI_FORMAT_R32G32B32A32_TYPELESS, 4, &quality);
    check(quality == 1, "supported typeless quality changed");
    unsigned calls = host_calls;
    tritonCheckFormatSupport(handle, DXGI_FORMAT_R32G32B32A32_FLOAT, &caps);
    check(host_calls == calls, "format cache no longer answers repeat queries");
    for (UINT count : {0u, 1u, 33u}) {
        tritonCheckMultisampleQualityLevels(handle, DXGI_FORMAT_UNKNOWN, count, &quality);
        check(quality == (count == 1 ? 1u : 0u), "DDI sample-count boundary violated");
    }
    check(host_calls == calls, "sample-count boundary queried host");
    std::puts("D3D10 format-family and sample-count checks passed");
}
'''
fixture = fixture.replace('#include <d3d11.h>', '#include <d3d11.h>\n#include <cassert>\n#include <initializer_list>')
with tempfile.TemporaryDirectory(prefix='triton10-format-') as tmp:
    tmp = Path(tmp)
    for mutation in (None, 'parent', 'sample-zero', 'legacy-bits'):
        code = source
        if mutation == 'parent':
            code = code.replace('if (famMax > parentQuality)', 'if (false && famMax > parentQuality)')
        elif mutation == 'sample-zero':
            code = code.replace('*pNumQualityLevels = SampleCount == 1 ? 1 : 0;', '*pNumQualityLevels = SampleCount <= 1 ? 1 : 0;')
        elif mutation == 'legacy-bits':
            code = code.replace('#if !defined(NPT_D3D10_RUNTIME_DDI)', '#if 1')
        path, binary = tmp / 'format.cpp', tmp / 'format'
        path.write_text(fixture.replace('/* PRODUCTION */', code))
        subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-parameter', '-DNPT_D3D10_RUNTIME_DDI',
                        '-fsanitize=address,undefined', *native_headers(),
                        str(path), '-o', str(binary)], check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        if mutation:
            assert result.returncode and 'FORMAT FAIL:' in result.stderr, result
            print('D3D10 format negative control rejected:', mutation)
        else:
            assert result.returncode == 0, result.stdout + result.stderr
            print(result.stdout.strip())
