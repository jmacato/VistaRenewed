#!/usr/bin/env python3
"""Run production guest texture maps against guarded host map storage."""
from pathlib import Path
import re
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[2]
NPT=ROOT/'triton-umd/src/virtio/neptune'

def function(source,name):
    match=re.search(r'\n(?:[A-Za-z_][^\n;{}]*\n)?[^\n;{}]*\b'+re.escape(name)+r'\([^;{}]*\)\s*\{',source)
    if not match: raise ValueError(name)
    start=match.start()+1; end=source.index('{',match.start())+1; depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]

resource=(NPT/'npt_resource.c').read_text();context=(NPT/'npt_overrides_d3d11_context.c').read_text();header=(NPT/'npt_resource.h').read_text()

def generate(r,c,fixture=None):
    names=['tex_aux','npt_dxgi_format_bytes_per_pixel','format_block_height','npt_dxgi_format_block_rows','npt_dxgi_format_row_bytes','npt_dxgi_format_subresource_rows','texture_publish_desc','npt_d3d11_texture_set_desc','npt_d3d11_texture_has_desc','npt_d3d11_texture_is_mappable','mip_dim','texture_subresource_mip','npt_d3d11_texture_get_subresource_byte_size','npt_d3d11_texture_get_mip_dimensions','npt_d3d11_texture_get_format','npt_d3d11_texture_ensure_map_shmem']
    for name in ('npt_d3d11_texture_get_subresource_map_byte_size','npt_d3d11_texture_grow_map_shmem'):
        if name+'(' in r:names.insert(-1,name)
    production='\n'.join(function(r,name) for name in names)
    production+='\n'+r[r.index('uint32_t npt_d3d11_texture_get_map_shmem_res_id'):]
    methods='\n'.join(function(c,name) for name in ['npt_d3d11_map_to_access_flags','ctx_Map_texture','ctx_Unmap_texture'])
    constants='enum { '+', '.join(sorted(set(re.findall(r'\bDXGI_FORMAT_\w+',production))))+' };\n'
    structures=''
    for name in ('npt_d3d11_texture_desc','npt_d3d11_texture_aux'):
        start=header.index('struct '+name+' {');structures+=header[start:header.index('};',start)+2]+'\n'
    if fixture is None:fixture=Path(__file__).with_suffix('.c').read_text()
    # Both geometry fixtures use known-create metadata. The import regression
    # executes the real lazy helper, including its mutex and RPC failures.
    metadata_abi='#include <stdatomic.h>\ntypedef unsigned mtx_t;\n#define D3D11_RESOURCE_DIMENSION_UNKNOWN 0\n'
    known_desc='\nstatic bool npt_d3d11_texture_ensure_desc(struct npt_d3d11_texture *t,unsigned dimension,bool extended){return npt_d3d11_texture_has_desc(t);}\n'
    return metadata_abi+fixture.replace('// FORMAT_CONSTANTS',constants).replace('// RESOURCE_STRUCTURES',structures).replace('// PRODUCTION_RESOURCE',production+known_desc).replace('// PRODUCTION_CONTEXT',methods)

def main():
    old='if (!npt_d3d11_texture_has_desc(t))'
    assert old in function(resource,'npt_d3d11_texture_is_mappable')
    variants={'current':(resource,context),'bc-rejected':(resource.replace(old,'if (!aux->bytes_per_pixel || !npt_d3d11_texture_has_desc(t))'),context),'texel-rows':(resource,context.replace('mip_h = npt_dxgi_format_subresource_rows(\n      npt_d3d11_texture_get_format(t), mip_h);','/* wrong texel row count */')),'cross-mip-cache':(resource,context.replace('npt_d3d11_texture_get_last_map_subresource(t) == Subresource &&',''))}
    with tempfile.TemporaryDirectory(prefix='triton-texture-map-') as tmp:
        tmp=Path(tmp)
        for name,(r,c) in variants.items():
            source=tmp/'test.c';exe=tmp/'test';source.write_text(generate(r,c))
            result=subprocess.run(['clang','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-parameter','-fsanitize=address,undefined',str(source),'-o',str(exe)],capture_output=True,text=True)
            assert result.returncode==0,result.stdout+result.stderr
            result=subprocess.run([str(exe)],capture_output=True,text=True)
            if name=='current':
                assert result.returncode==0,result.stdout+result.stderr;print(result.stdout.strip())
            else:
                assert result.returncode!=0 and ('FAIL map' in result.stderr or 'AddressSanitizer' in result.stderr),result.stdout+result.stderr
                print('D3D10 texture map '+name+' negative control passed')

if __name__ == "__main__":
    main()
