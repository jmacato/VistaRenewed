#!/usr/bin/env python3
"""Production UpdateSubresource callbacks against poisoned pitched CPU inputs."""
from pathlib import Path
import importlib.util
import subprocess
import re
import tempfile
ROOT=Path(__file__).resolve().parents[6];TRITON=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('extract',ROOT/'tests/vista/test-d3d10-texture-map.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
resource=(TRITON/'tritonResource.c').read_text()
names=('tritonUploadColorOrder','tritonPrepareUpload','tritonResourceUpdateSubresourceUP','tritonResourceUpdateSubresourceUP_11_1','tritonDefaultCbUpdateSubresourceUP','tritonDefaultCbUpdateSubresourceUP_11_1')
code='\n'.join(m.function(resource,n) for n in names)
fixture=Path(__file__).with_name('tritonD3D10UploadTest.c').read_text()
for size in ('uint32_t','uint64_t'):
 for variant in ('current','missing-swap','tight-source-rows','offset-source','dropped-flags','first-mip','missing-free'):
  mutant=code
  if variant=='missing-swap':
   old='out[0] = in[2]; out[1] = in[1];\n            out[2] = in[0];'
   assert old in mutant;mutant=mutant.replace(old,'out[0] = in[0]; out[1] = in[1];\n            out[2] = in[2];')
  elif variant=='tight-source-rows':
   old='(const unsigned char *)source + (SIZE_T)row * rowPitch'
   assert old in mutant;mutant=mutant.replace(old,'(const unsigned char *)source + (SIZE_T)row * rowBytes')
  elif variant=='offset-source':
   old='(const unsigned char *)source + (SIZE_T)row * rowPitch'
   assert old in mutant;mutant=mutant.replace(old,old+' + (box ? box->left * 4u + box->top * rowPitch : 0u)')
  elif variant=='dropped-flags':
   old='upload, SrcRowPitch, SrcDepthPitch, CopyFlags);'
   assert old in mutant;mutant=mutant.replace(old,'upload, SrcRowPitch, SrcDepthPitch, 0);')
  elif variant=='first-mip':
   old='const UINT mip = subresource % desc.MipLevels;'
   assert old in mutant;mutant=mutant.replace(old,'const UINT mip = 0;')
  elif variant=='missing-free':
   old='if (allocation) HeapFree(GetProcessHeap(), 0, allocation);'
   assert old in mutant;mutant=mutant.replace(old,'/* omitted free */')
  with tempfile.TemporaryDirectory(prefix='triton-upload-') as d:
   d=Path(d);(d/'test.c').write_text(fixture.replace('SIZE_TYPE',size).replace('// PRODUCTION',mutant))
   p=subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-unused-function','-O1','-g','-fsanitize=address,undefined',str(d/'test.c'),'-o',str(d/'test')],capture_output=True,text=True);assert p.returncode==0,p.stdout+p.stderr
   p=subprocess.run([str(d/'test')],capture_output=True,text=True)
   if variant=='current':assert p.returncode==0,p.stdout+p.stderr;print(size+' '+p.stdout.strip())
   else:assert p.returncode!=0 and ('FAIL upload' in p.stderr or 'AddressSanitizer' in p.stderr),p.stdout+p.stderr;print(size+' '+variant+' negative control rejected')
print('D3D10 physical upload checks passed (CPU contracts; live Vista uploads pending)')

# Compile the production callbacks against real D3D11 interfaces. Handles and
# runtime allocation/error hooks are local scaffolding; this is not a WDK ABI run.
prefix = r"""
#include <cstdint>
#include <cstdlib>
#include <d3d11_1.h>
#define APIENTRY
#define GetProcessHeap() nullptr
#define HEAP_ZERO_MEMORY 8
#define HeapAlloc(h,f,n) calloc(1,n)
#define HeapFree(h,f,p) free(p)
using VOID=void;
using D3D10_DDI_BOX=D3D11_BOX;
struct Handle {void *pDrvPrivate;};
using D3D10DDI_HDEVICE=Handle;
using D3D10DDI_HRESOURCE=Handle;
struct TRITON_DEVICE {ID3D11DeviceContext1 *pCtx1;};
using PTRITON_DEVICE=TRITON_DEVICE*;
struct TRITON_RESOURCE {ID3D11Resource *pResource;DXGI_FORMAT Format,HostFormat;};
using PTRITON_RESOURCE=TRITON_RESOURCE*;
static void tritonSetError(PTRITON_DEVICE,HRESULT) {}
"""
macros='\n'.join('#define '+name+'(p,...) (p)->'+name.split('_',1)[1]+'(__VA_ARGS__)' for name in sorted(set(re.findall(r'ID3D11\w+_\w+(?=\()',code))))
with tempfile.TemporaryDirectory(prefix='triton-upload-headers-') as d:
 source=Path(d)/'test.cpp';source.write_text(prefix+macros+'\n'+code)
 args=['g++','-std=c++17','-fsyntax-only']+['-I'+str(ROOT/'triton-dxvk/include/native'/p) for p in ('directx','windows')]+[str(source)]
 p=subprocess.run(args,capture_output=True,text=True);assert p.returncode==0,p.stdout+p.stderr
 print('D3D10 upload actual-header compilation passed (no GPU execution or Windows ABI credit)')
