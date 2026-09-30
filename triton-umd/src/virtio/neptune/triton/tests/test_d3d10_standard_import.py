#!/usr/bin/env python3
"""Actual standard-resource functions with descriptor, lifetime and rotate faults."""
from pathlib import Path
import importlib.util
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[6];TRITON=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('extract',ROOT/'tests/vista/test-d3d10-texture-map.py');module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
resource=(TRITON/'tritonResource.c').read_text();view=(TRITON/'tritonView.c').read_text();dxgi=(TRITON/'tritonDxgi.c').read_text()
production=module.function((TRITON.parent/'npt_shared_texture.c').read_text(),'npt_shared_texture_virgl_format')+'\n'+'\n'.join(module.function(resource,n) for n in ('tritonValidateSharedOpen','tritonSynthesizeStandardTexture','tritonQueryStandardPrimary','tritonOpenResource','tritonDestroyResource'))+'\n'+module.function(view,'tritonResourceHostViewFormat')+'\n'+module.function(view,'tritonResourceRecreatedViewFormat')+'\n'+module.function(dxgi,'tritonDxgiRotateResourceIdentities')
header=(ROOT/'triton-kmd/viogpu/shared/viogpum.h').read_text();types=''
for name in ('VIOGPU_RESOURCE_3D_OPTIONS','VIOGPU_BLOB_INFO','VIOGPU_RESOURCE_SHARED_TEXTURE_OPTIONS'):
 start=header.index('typedef struct _'+name);end=header.index('#pragma pack()',start);types+='#pragma pack(1)\n'+header[start:end]+'#pragma pack()\n'
bridge=(TRITON/'tritonSharedBridge.h').read_text();start=bridge.index('struct triton_shared_texture_desc {');bridge=bridge[start:bridge.index('\n};',start)+3]
create=module.function(resource,'tritonCreateResource')
initializers=[]
for field in ('BorrowedKMAllocation','HostFormat'):
 import re
 match=re.search(r'    r->'+field+r'\s*= [^;]+;',create)
 assert match,field
 initializers.append(match.group(0))
production+='\nstatic void create_initialize_flags(PTRITON_RESOURCE r){'+'\n'.join(initializers)+'}\n'
fixture=(Path(__file__).with_name('tritonD3D10StandardTest.c')).read_text().replace('// PACKED_TYPES',types).replace('// BRIDGE_DESC',bridge)
for size in ('uint32_t','uint64_t'):
 for variant in ('current','old-pitch','borrowed-free','rotation-ownership','physical-view','rotation-view','poisoned-create','modern'):
  code=production
  if variant=='old-pitch':code=code.replace('const ULONGLONG stride = (ULONGLONG)options->width * 4ull;', 'const ULONGLONG stride = ((ULONGLONG)options->width * 4ull + 255ull) & ~255ull;')
  elif variant=='borrowed-free':code=code.replace('!r->BorrowedKMAllocation &&','')
  elif variant=='rotation-ownership':code=code.replace('a->BorrowedKMAllocation = b->BorrowedKMAllocation;','').replace('last->BorrowedKMAllocation = tmpBorrowed;','(void)tmpBorrowed;')
  elif variant=='physical-view':code=code.replace('if (!r->HostFormat)','if (1)')
  elif variant=='rotation-view':
   old='if (!viewResource.HostFormat) {'
   assert old in code
   code=code.replace(old,'if (0) {')
  elif variant=='poisoned-create':
   old=initializers[0]
   assert old in code
   code=code.replace(old,'')
  with tempfile.TemporaryDirectory(prefix='triton-standard-open-') as directory:
   directory=Path(directory);source=directory/'test.c';exe=directory/'test';source.write_text(fixture.replace('SIZE_TYPE',size).replace('// PRODUCTION',code))
   args=['clang','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-unused-function','-fsanitize=address,undefined']
   if variant!='modern':args+=['-DNPT_D3D10_RUNTIME_DDI']
   result=subprocess.run(args+[str(source),'-o',str(exe)],capture_output=True,text=True);assert result.returncode==0,result.stdout+result.stderr
   result=subprocess.run([str(exe)],capture_output=True,text=True)
   if variant in ('current','modern'):assert result.returncode==0,result.stdout+result.stderr;print(size+' '+result.stdout.strip())
   else:assert result.returncode!=0 and 'FAIL standard' in result.stderr,result.stdout+result.stderr;print(size+' '+variant+' negative control rejected')
print('D3D10 standard import checks passed (CPU bridge; Vista primary presentation pending)')
