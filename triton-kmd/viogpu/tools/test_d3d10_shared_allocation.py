#!/usr/bin/env python3
"""Run the actual Vista kernel shared-texture validator and packed ABI types."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
source = (ROOT / 'triton-kmd/viogpu/viogpu3d/viogpu_allocation.cpp').read_text()
source = source[source.index('static BOOLEAN IsValidVistaSharedTexture('):
                source.index('BOOLEAN VioGpuAllocation::GetTransferLayout')]
header = (ROOT / 'triton-kmd/viogpu/shared/viogpum.h').read_text()
types = ''
for name in ('VIOGPU_BLOB_INFO', 'VIOGPU_RESOURCE_SHARED_TEXTURE_OPTIONS'):
    start = header.index('typedef struct _' + name)
    end = header.index('#pragma pack()', start)
    types += '#pragma pack(1)\n' + header[start:end] + '#pragma pack()\n'

fixture = r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
typedef uint32_t ULONG;
typedef uint64_t ULONGLONG;
typedef unsigned char BOOLEAN;
typedef SIZE_TYPE SIZE_T;
#define TRUE 1
#define FALSE 0
#define VIOGPU_TARGET_VISTA 1
#define MAXULONG UINT32_MAX
#define MAXULONGLONG UINT64_MAX
#define PAGE_SIZE 4096u
#define VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM 1
#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM 2
#define VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM 67
// TYPES
// IMPLEMENTATION
static unsigned checks;
#define CHECK(c) do { ++checks; if (!(c)) {fprintf(stderr,"FAIL line %u\n",__LINE__);exit(1);} } while(0)
#define BAD(field, value) do { VIOGPU_RESOURCE_SHARED_TEXTURE_OPTIONS bad = good; bad.field = (value); CHECK(!IsValidVistaSharedTexture(&bad,4096)); } while(0)
int main(void)
{
    VIOGPU_RESOURCE_SHARED_TEXTURE_OPTIONS good = {0};
    const ULONG formats[] = {87,88,28,29,91,93,24,10}, virgl[] = {1,2,67,104,100,101,8,94};
    good.blob_id=1;good.width=8;good.height=8;
    good.mip_levels=good.array_size=good.sample_count=1;
    good.bind_flags=0x28;good.misc_flags=2;good.plane_count=1;
    good.allocation_size=256;good.planes[0].pitch=32;
    good.ScanoutInfo.width=good.ScanoutInfo.height=8;
    good.ScanoutInfo.strides[0]=32;
    for(unsigned f=0;f<8;++f)for(unsigned primary=0;primary<2;++primary)for(unsigned layout=0;layout<3;++layout){
        good.format=formats[f];good.ScanoutInfo.format=virgl[f];
        good.primary=primary;good.texture_layout=layout;
        good.allocation_size=f==7?512:256;good.planes[0].pitch=f==7?64:32;
        good.ScanoutInfo.strides[0]=(ULONG)good.planes[0].pitch;
        if(primary&&f>=3){CHECK(!IsValidVistaSharedTexture(&good,4096));continue;}
        CHECK(IsValidVistaSharedTexture(&good,4096));
        CHECK(!IsValidVistaSharedTexture(&good,4095));
        CHECK(!IsValidVistaSharedTexture(&good,8192));
        BAD(blob_id,0);BAD(primary,2);BAD(width,0);BAD(height,0);
        BAD(width,4097);BAD(height,4097);BAD(mip_levels,0);BAD(mip_levels,2);
        BAD(array_size,0);BAD(array_size,2);BAD(sample_count,0);BAD(sample_count,4);
        BAD(usage,1);BAD(bind_flags,8);BAD(bind_flags,0x20);BAD(bind_flags,0x68);
        BAD(cpu_access_flags,1);BAD(misc_flags,0);BAD(misc_flags,3);
        BAD(misc_flags,0x40000002);BAD(misc_flags,0x80000002);
        BAD(texture_layout,3);BAD(plane_count,0);BAD(plane_count,2);
        BAD(allocation_size,0);BAD(allocation_size,4097);BAD(allocation_size,UINT64_MAX);
        BAD(allocation_size,UINT32_MAX+1ull);BAD(allocation_size,good.allocation_size-1);
        BAD(format,0);BAD(format,999);BAD(format,27);
        BAD(planes[0].pitch,0);BAD(planes[0].pitch,31);BAD(planes[0].pitch,UINT32_MAX+1ull);
        BAD(planes[0].offset,256);BAD(planes[0].offset,1);BAD(planes[0].offset,UINT32_MAX+1ull);
        BAD(ScanoutInfo.width,7);BAD(ScanoutInfo.height,7);BAD(ScanoutInfo.format,999);
        BAD(ScanoutInfo.strides[0],good.ScanoutInfo.strides[0]+1);BAD(ScanoutInfo.offsets[0],1);
        for(unsigned plane=1;plane<4;++plane){
            BAD(planes[plane].offset,1);BAD(planes[plane].pitch,1);
            BAD(ScanoutInfo.offsets[plane],1);BAD(ScanoutInfo.strides[plane],1);
        }
    }
    CHECK(!IsValidVistaSharedTexture(NULL,4096));
    good.primary=0;good.format=10;good.ScanoutInfo.format=94;
    good.allocation_size=256;good.planes[0].pitch=good.ScanoutInfo.strides[0]=32;
    CHECK(!IsValidVistaSharedTexture(&good,4096));
    /* D3D10's texture dimension limit exceeds the D3D9 display limit. */
    good.width=good.ScanoutInfo.width=8192;good.height=good.ScanoutInfo.height=1;
    good.planes[0].pitch=good.ScanoutInfo.strides[0]=65536;good.allocation_size=65536;
    CHECK(IsValidVistaSharedTexture(&good,65536));
    good.primary=1;CHECK(!IsValidVistaSharedTexture(&good,65536));good.primary=0;
    good.width=good.ScanoutInfo.width=8193;good.planes[0].pitch=good.ScanoutInfo.strides[0]=65544;good.allocation_size=65544;
    CHECK(!IsValidVistaSharedTexture(&good,69632));
    /* Exact final-row boundary and a valid nonzero suballocation offset. */
    good.width=good.height=good.ScanoutInfo.width=good.ScanoutInfo.height=8;
    good.format=28;good.ScanoutInfo.format=67;
    good.allocation_size=1024;good.planes[0].offset=128;good.ScanoutInfo.offsets[0]=128;
    good.planes[0].pitch=64;good.ScanoutInfo.strides[0]=64;
    CHECK(IsValidVistaSharedTexture(&good,4096));
    good.allocation_size=128+7*64+32;
    CHECK(IsValidVistaSharedTexture(&good,4096));
    --good.allocation_size;CHECK(!IsValidVistaSharedTexture(&good,4096));
    printf("D3D10 kernel shared allocation %u checks passed (%zu-bit SIZE_T)\n",checks,sizeof(SIZE_T)*8);
    return 0;
}
'''

for size_type in ('uint32_t', 'uint64_t'):
    for variant in ('current', 'rgba-rejected', 'private-flag-accepted', 'short-wide-row'):
        production = source
        if variant == 'rgba-rejected':
            old = 'else if (options->format == dxgiRgba8)'
            assert old in production
            production = production.replace(old, 'else if (FALSE && options->format == dxgiRgba8)')
        elif variant == 'private-flag-accepted':
            old = 'options->misc_flags != requiredMiscFlags'
            assert old in production
            production = production.replace(old, '(options->misc_flags & ~0x40000000u) != requiredMiscFlags')
        elif variant == 'short-wide-row':
            assert 'bytesPerPixel = 8;' in production
            production = production.replace('bytesPerPixel = 8;', 'bytesPerPixel = 4;')
        code = fixture.replace('SIZE_TYPE', size_type).replace('// TYPES', types).replace('// IMPLEMENTATION', production)
        with tempfile.TemporaryDirectory(prefix='vista-shared-') as temporary:
            work = Path(temporary)
            (work / 'test.c').write_text(code)
            subprocess.run([os.environ.get('CC', 'clang'), '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', str(work / 'test.c'), '-o', str(work / 'test')], check=True)
            result = subprocess.run([str(work / 'test')], capture_output=True, text=True)
            if variant == 'current':
                assert result.returncode == 0, result.stdout + result.stderr
                print(result.stdout.strip())
            else:
                assert result.returncode != 0 and 'FAIL' in result.stderr, result.stdout + result.stderr
                print('D3D10 kernel shared allocation ' + variant + ' negative control passed (' + size_type + ')')
