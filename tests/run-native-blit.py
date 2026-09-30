#!/usr/bin/env python3
"""Run the Vista GPU blit implementation against the installed native DXVK."""
from pathlib import Path
import os,re,subprocess,sys
root=Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / 'tests/vista'))
from test_config import ARTIFACTS, native_headers, native_libraries
src=(root/'triton-umd/src/virtio/neptune/vista-d3d9/triton9_blit.c').read_text()
src='\n'.join(l for l in src.splitlines() if not l.startswith('#include'))
resource=(root/'triton-umd/src/virtio/neptune/vista-d3d9/triton9_resource.c').read_text()
def function(name):
 match=re.search(r'(?:static\s+)?(?:void|HRESULT)\n'+name+r'\(',resource)
 assert match,name
 start=resource.index('{',match.start());end=start+1;depth=1
 while depth:
  depth+=(resource[end]=='{')-(resource[end]=='}');end+=1
 return resource[match.start():end]
src=function('triton9SwapPacked10Rows')+'\n'+function('triton9UpdateHostTexture')+'\n'+src
src=src.replace('converted = HeapAlloc(', 'converted = (BYTE *)HeapAlloc(')
src=src.replace('= {0};', '= {};')
macros=[]
for name in sorted(set(re.findall(r'ID3D11\w+_\w+(?=\()',src))):
 method=name.split('_',1)[1]
 macros.append(f'#define {name}(obj, ...) (obj)->{method}(__VA_ARGS__)')
src=src.replace('= device->stretchBlitState;', '= (TRITON9_STRETCH_STATE *)device->stretchBlitState;')
src=src.replace('state = HeapAlloc(', 'state = (TRITON9_STRETCH_STATE *)HeapAlloc(')
template=(root/'tests/linux-blit-test.cpp').read_text()
out=ARTIFACTS/'vista-blit/vista-blit-test.cpp';out.parent.mkdir(parents=True,exist_ok=True)
out.write_text(template.replace('/* PRODUCTION_BLIT */','\n'.join(macros)+'\n'+src))
prefix=root/'host-linux'
subprocess.run(['g++','-std=c++17','-O2',*native_headers(),str(out),*['-L'+str(p) for p in native_libraries()],'-ldxvk_d3d11','-ldxvk_dxgi','-o',str(out.with_suffix(''))],check=True)
env=dict(os.environ,LD_LIBRARY_PATH=os.pathsep.join(map(str,native_libraries())),DXVK_WSI_DRIVER='Headless')
subprocess.run([str(out.with_suffix(''))],env=env,check=True)
