#!/usr/bin/env python3
"""Apply the kernel descriptor matrix independently to the actual UMD import guard."""
from pathlib import Path
import ast
import re
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[6]
TRITON=Path(__file__).resolve().parents[1]
source=(TRITON/'tritonResource.c').read_text()
start=source.index('static BOOL\ntritonValidateSharedOpen(')
validator=source[start:source.index('\n#endif',start)]
# Reuse the descriptor values, not the kernel validation implementation.
tree=ast.parse((ROOT/'triton-kmd/viogpu/tools/test_d3d10_shared_allocation.py').read_text())
fixture=next(ast.literal_eval(n.value) for n in tree.body if isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='fixture' for t in n.targets))
header=(ROOT/'triton-kmd/viogpu/shared/viogpum.h').read_text();types=''
for name in ('VIOGPU_BLOB_INFO','VIOGPU_RESOURCE_SHARED_TEXTURE_OPTIONS'):
 start=header.index('typedef struct _'+name);end=header.index('#pragma pack()',start)
 types+='#pragma pack(1)\n'+header[start:end]+'#pragma pack()\n'
shared=(TRITON.parent/'npt_shared_texture.c').read_text();start=shared.index('uint32_t\nnpt_shared_texture_virgl_format');mapping=shared[start:shared.index('\n}',start)+2]
formats={'B8G8R8A8_UNORM':87,'B8G8R8X8_UNORM':88,'R8G8B8A8_UNORM':28,'R8G8B8A8_UNORM_SRGB':29,'B8G8R8A8_UNORM_SRGB':91,'B8G8R8X8_UNORM_SRGB':93,'R10G10B10A2_UNORM':24,'R16G16B16A16_FLOAT':10}
prefix='typedef unsigned UINT;typedef int BOOL;\n#define D3D11_USAGE_DEFAULT 0\n#define D3D11_BIND_SHADER_RESOURCE 8\n#define D3D11_BIND_RENDER_TARGET 32\n#define D3D11_RESOURCE_MISC_SHARED 2\n#define npt_log(...) ((void)0)\n'
prefix+='\n'.join('#define DXGI_FORMAT_'+name+' '+str(value) for name,value in formats.items())+'\n'
implementation=prefix+mapping+'\n'+validator+'\n#define IsValidVistaSharedTexture tritonValidateSharedOpen\n'
for size in ('uint32_t','uint64_t'):
 for variant in ('current','short-wide-row','missing-wide-format'):
  code=implementation
  if variant=='short-wide-row':code=code.replace('so->format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4','4')
  if variant=='missing-wide-format':code=code.replace('case DXGI_FORMAT_R16G16B16A16_FLOAT:', 'case 9999:')
  with tempfile.TemporaryDirectory(prefix='triton-shared-open-') as directory:
   directory=Path(directory);c=directory/'test.c';exe=directory/'test'
   c.write_text(fixture.replace('SIZE_TYPE',size).replace('// TYPES',types).replace('// IMPLEMENTATION',code))
   result=subprocess.run(['clang','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(c),'-o',str(exe)],capture_output=True,text=True)
   assert result.returncode==0,result.stdout+result.stderr
   result=subprocess.run([str(exe)],capture_output=True,text=True)
   if variant=='current':assert result.returncode==0,result.stdout+result.stderr;print('UMD shared import '+size+': '+result.stdout.strip())
   else:assert result.returncode!=0 and 'FAIL' in result.stderr,result.stdout+result.stderr;print('UMD shared import '+variant+' negative control passed ('+size+')')
# The guard must remain on the runtime path before attachment/import side effects.
body=source[source.index('void APIENTRY\ntritonOpenResource('):]
assert body.index('!tritonValidateSharedOpen(so, ax->Size)')<body.index('tritonPresentEnsureRuntimeCtx(pD)')
print('D3D10 shared import checks passed')
