#!/usr/bin/env python3
"""Run format-capability oracles against current C and asserted source mutants."""
from pathlib import Path
import importlib.util,re,subprocess
from test_config import ARTIFACTS, HOST_PREFIX, QEMU_BUILD, native_headers, native_libraries, sdk_header, container_command, container_path, vista_compile_database

ROOT=Path(__file__).resolve().parents[2]
HERE=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('resource_runner',HERE/'run-d3d9-resources.py')
runner=importlib.util.module_from_spec(spec);spec.loader.exec_module(runner)
SRC=runner.SRC;OUT=runner.OUT

def check():
    ddi=(sdk_header('d3dumddi.h', 'microsoft.windows.wdk.x64')).read_text()
    constants='\n'.join(re.findall(r'^\s*#define FORMATOP_[^\n]+',ddi,re.M))
    hdr=(SRC/'triton9.h').read_text()
    for baseline in [False,True]:
        source=(SRC/'triton9_format.c').read_text()
        if baseline:
            assert 'FORMATOP_CUBETEXTURE' in source
            source=source.replace('FORMATOP_CUBETEXTURE','0u')
        tokens=set(re.findall(r'D3DDDIFMT_(\w+)',source))|set(re.findall(r'(?:FMT|SRGB|BC|DEPTH)\((\w+),',source));tokens.discard('d')
        prefix='#include <d3d9.h>\n#include <dxgiformat.h>\n#include <cstdio>\nusing D3DDDIFORMAT=D3DFORMAT;\n#define D3DDDIMULTISAMPLE_NONE D3DMULTISAMPLE_NONE\n#define D3DDDIMULTISAMPLE_NONMASKABLE D3DMULTISAMPLE_NONMASKABLE\n'
        prefix+=constants+'\n'+'\n'.join('#define D3DDDIFMT_'+t+' D3DFMT_'+t for t in tokens)+'\n'+runner.structure(hdr,'TRITON9_FORMAT')
        prefix+='\nstruct FORMATOP {D3DDDIFORMAT Format;UINT Operations,FlipMsTypes,BltMsTypes,PrivateFormatBitCount;};\nconst TRITON9_FORMAT *triton9FormatLookup(D3DDDIFORMAT);\nUINT triton9FormatCount(void);\nUINT triton9FormatMultisampleQuality(D3DDDIFORMAT,UINT);\nBOOL triton9HasCompleteD24S8ClearContract(){return TRUE;}\n'
        suffix='''
int main(){
 const auto color=triton9FormatLookup(D3DFMT_A8R8G8B8);
 bool ok=color && (color->operations & FORMATOP_CUBETEXTURE) &&
  (color->operations & FORMATOP_VOLUMETEXTURE) &&
  (color->operations & FORMATOP_AUTOGENMIPMAP) &&
  triton9FormatLookup(D3DFMT_DXT1) && triton9FormatLookup(D3DFMT_DXT3) &&
  triton9FormatLookup(D3DFMT_DXT5) && triton9FormatLookup(D3DFMT_A16B16G16R16F);
 printf("D3D9 format resource oracle %s\\n",ok?"PASS":"FAIL");return ok?0:1;
}
'''
        if not baseline:
            matrix = r'''
 ok = ok && triton9FormatLookupHost(DXGI_FORMAT_R10G10B10A2_UNORM)->d3dFormat == D3DFMT_A2B10G10R10;
 const auto depth = triton9FormatLookup(D3DFMT_D16_LOCKABLE);
 ok = ok && depth && depth->hostFormat == DXGI_FORMAT_D16_UNORM &&
  depth->depthStencil && depth->bytesPerPixel == 2 &&
  (depth->operations & FORMATOP_ZSTENCIL) && !(depth->operations & FORMATOP_TEXTURE);
 FORMATOP operations[64] = {};
 const UINT count = triton9FormatCount();
 ok = ok && count == 35;
 triton9CopyFormatOperations(operations,64);
 for (UINT i=0;i<count;++i) {
  const D3DFORMAT f=operations[i].Format;
  const bool multi=f==D3DFMT_A8R8G8B8 || f==D3DFMT_X8R8G8B8 ||
   f==D3DFMT_A8B8G8R8 || f==D3DFMT_D16 || f==D3DFMT_D24S8 || f==D3DFMT_D24X8;
  const UINT mask=multi ? 1u|(1u<<1)|(1u<<3) : 0;
  const auto entry=triton9FormatLookup(f);
  ok=ok && entry && operations[i].Operations==entry->operations &&
   operations[i].FlipMsTypes==mask && operations[i].BltMsTypes==mask;
  for(UINT samples=0;samples<=17;++samples) {
   const UINT quality=samples==0 ? !!(operations[i].Operations &
    (FORMATOP_OFFSCREEN_RENDERTARGET|FORMATOP_ZSTENCIL)) :
    multi ? (samples==1 ? 2 : samples==2 || samples==4) : 0;
   ok=ok && triton9FormatMultisampleQuality(f,samples)==quality;
  }
 }
 ok=ok && !triton9FormatLookup(D3DFMT_UNKNOWN) &&
  !triton9FormatMultisampleQuality(D3DFMT_UNKNOWN,0) &&
  !triton9FormatMultisampleQuality(D3DFMT_D16_LOCKABLE,2) &&
  !triton9FormatMultisampleQuality(D3DFMT_D16_LOCKABLE,4);
'''
            suffix=suffix.replace(' printf("D3D9 format resource oracle',matrix+' printf("D3D9 format resource oracle')
        file=OUT/('format-baseline.cpp' if baseline else 'format-current.cpp');exe=file.with_suffix('')
        file.write_text(prefix+'\n'+source.replace('#include "triton9.h"','').replace('_Static_assert','static_assert')+suffix)
        subprocess.run(['g++','-std=c++17',*native_headers(),str(file),'-o',str(exe)],check=True)
        result=subprocess.run([str(exe)],capture_output=True,text=True)
        (OUT/(file.stem+'.log')).write_text(result.stdout)
        if (result.returncode==0)==baseline:raise AssertionError(result.stdout)
        if baseline and 'oracle FAIL' not in result.stdout:raise AssertionError('format mutant did not reach oracle')
        print(('Missing cube-capability negative control: ' if baseline else '')+result.stdout.strip())
def shared_rollback():
    source=(SRC/'triton9_resource.c').read_text()
    names=['triton9ValidateSharedPlane','triton9SharedFormat','triton9DeallocateResource',
           'triton9RegisterSharedTexture','triton9AllocateStandardPrimary',
           'triton9FreeUnpublishedResource','triton9DisposeFailedResource','triton9DestroyFailedResources']
    funcs=[runner.function(source,n) for n in names]
    prototypes='\n'.join(f[:f.index('{')].rstrip()+';' for f in funcs)
    fixture=(HERE/'test-d3d9-resources.cpp').read_text()
    for mutation in [False,True]:
        production='\n'.join(funcs)
        if mutation:
            # Retained pre-fix behavior: metadata rejection abandons export.
            production=production.replace('hr = E_FAIL;\n        goto rollback;', 'return E_FAIL;',2)
        path=OUT/('shared-rollback'+('-negative' if mutation else '')+'.cpp')
        path.write_text(fixture.replace('// GENERATED_ROLLBACK_PRODUCTION',prototypes+'\n'+production))
        exe=path.with_suffix('')
        subprocess.run(['g++','-std=c++17','-O1','-g','-DTRITON9_SHARED_ROLLBACK_TEST',str(path),'-o',str(exe)],check=True)
        result=subprocess.run([str(exe)],capture_output=True,text=True)
        (OUT/(path.stem+'.log')).write_text(result.stdout+result.stderr)
        if (result.returncode==0)==mutation:raise AssertionError(result.stdout+result.stderr)
        if mutation and '[FAIL]' not in result.stdout:raise AssertionError('rollback negative control failed without branch oracle')
        print(('Rollback metadata negative control rejected: ' if mutation else '')+result.stdout.splitlines()[-1])
if __name__=='__main__':
    OUT.mkdir(parents=True,exist_ok=True)
    check()
    shared_rollback()
