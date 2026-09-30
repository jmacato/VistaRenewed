#!/usr/bin/env python3
"""Production host/guest Map lifecycle under allocation, pitch and retry failures."""
from pathlib import Path
import importlib.util
import os
import re
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[2]
NPT=ROOT/'triton-umd/src/virtio/neptune'
HOST=ROOT/'triton-virglrenderer/src/neptune'
spec=importlib.util.spec_from_file_location('texture_maps',Path(__file__).with_name('test-d3d10-texture-map.py'))
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
resource=(NPT/'npt_resource.c').read_text();context=(NPT/'npt_overrides_d3d11_context.c').read_text();host=(HOST/'npt_resource.c').read_text()
wire=(NPT/'npt_transport_defs.h').read_text()
host_wire=(HOST/'npt_transport_defs.h').read_text()
for declaration in ('struct npt_cmd_map_resource {','struct npt_cmd_map_resource_reply {','struct npt_cmd_unmap_resource {'):
    a=wire.index(declaration);b=host_wire.index(declaration)
    assert wire[a:wire.index('};',a)+2]==host_wire[b:host_wire.index('};',b)+2],declaration
bits='\n'.join(re.findall(r'^#define NPT_MAP_ACCESS_.*$',wire,re.M))
assert bits=='\n'.join(re.findall(r'^#define NPT_MAP_ACCESS_.*$',host_wire,re.M))
fixture=Path(__file__).with_suffix('.c').read_text().replace('// MAP_BITS',bits)
# Match both scalar fields and pointer fields of the actual host map record.
header=(HOST/'npt_context.h').read_text();start=header.index('struct npt_sync_map_entry {');entry=header[start:header.index('};',start)+2]
fixture=fixture.replace('// HOST_MAP_ENTRY',entry)
protocol=(NPT/'neptune-protocol/npt_protocol_defs.h').read_text()
structs=[]
for source,name in [(protocol,'npt_command_header'),(protocol,'npt_reply_header'),(wire,'npt_cmd_unmap_resource'),(wire,'npt_cmd_unmap_resource_reply')]:
    start=source.index('struct '+name+' {');structs.append(source[start:source.index('};',start)+2])
fixture=fixture.replace('// ABORT_WIRE','\n'.join(structs))
dispatch=(NPT/'npt_dispatch.c').read_text()
fixture=fixture.replace('// PRODUCTION_ABORT','\n'.join(module.function(dispatch,n) for n in ('fill_unmap_cmd','npt_dispatch_resource_abort')))

for variant in ('current','oom-no-unmap','abort-copies','guest-no-abort','tight-depth','old-cap','guest-dimensions','rename-bound','wait-failure'):
    r,c,h=resource,context,host
    if variant=='oom-no-unmap':
        old='npt_log("map_resource: sync map table OOM");\n      npt_resource_backend_unmap(imm_ctx, resource, subresource);'
        assert old in h;h=h.replace(old,'npt_log("map_resource: sync map table OOM");')
    elif variant=='abort-copies':
        old='if (access_flags & NPT_MAP_ACCESS_ABORT) {'
        assert old in h
        # Route abort through legacy paired Unmap: zero byte_size copied all writes.
        h=h.replace(old,'if (false && (access_flags & NPT_MAP_ACCESS_ABORT)) {')
        marker='   struct npt_resource *shmem';at=h.index(marker,h.index('HRESULT\nnpt_resource_unmap('));h=h[:at]+'   if (access_flags == NPT_MAP_ACCESS_ABORT) access_flags = 0;\n'+h[at:]
    elif variant=='guest-no-abort':
        old='const HRESULT aborted = npt_dispatch_resource_abort('
        assert old in c
        begin=c.index(old);end=c.index(';',begin);c=c[:begin]+'const HRESULT aborted = NPT_S_OK'+c[end:]
    elif variant=='tight-depth':
        r=r.replace('return (uint32_t)((uint64_t)depth_pitch * (depth - 1) + slice);','return (uint32_t)(slice * depth);')
    elif variant=='old-cap':
        r=r.replace('slice > (UINT32_MAX - 63u) / aux->depth','slice > (64u << 20) / aux->depth')
    elif variant=='guest-dimensions':
        h=h.replace('(mip_height != layout.rows || mip_depth != layout.depth)', '(false)')
    elif variant=='rename-bound':
        h=h.replace(' || byte_size > actual_size', '')
    elif variant=='wait-failure':
        r=r.replace('if (npt_ring_wait_seqno(ring, r->slots[i].pending_seqno) == UINT32_MAX)', 'if (npt_ring_wait_seqno(ring, r->slots[i].pending_seqno) == 0)')
    host_names=['npt_access_flags_to_d3d11_map','npt_sync_map_find','npt_sync_map_add','npt_sync_map_remove','npt_resource_map_layout','npt_resource_mapped_size','npt_resource_backend_unmap','npt_resource_map','npt_resource_unmap']
    code=module.generate(r,c,fixture)
    start=h.index('struct npt_resource_map_layout {');layout=h[start:h.index('};',start)+2]
    code=code.replace('// PRODUCTION_HOST',layout+'\n'+'\n'.join(module.function(h,n) for n in host_names))
    constants=set(re.findall(r'\bDXGI_FORMAT_\w+',h))-set(re.findall(r'\bDXGI_FORMAT_\w+',module.generate(r,c,fixture)))
    code=code.replace('// HOST_FORMATS','enum { '+', '.join(x+'='+str(1000+i) for i,x in enumerate(sorted(constants)))+' };' if constants else '')
    code=code.replace('// PRODUCTION_RING',module.function(r,'npt_d3d_map_ring_alloc_shmem'))
    with tempfile.TemporaryDirectory(prefix='triton-map-lifecycle-') as temporary:
        temporary=Path(temporary);source=temporary/'test.c';exe=temporary/'test';source.write_text(code)
        result=subprocess.run(['clang','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-parameter','-fsanitize=address,undefined',str(source),'-o',str(exe)],capture_output=True,text=True)
        assert result.returncode==0,result.stdout+result.stderr
        result=subprocess.run([str(exe)],capture_output=True,text=True)
        if variant=='current':assert result.returncode==0,result.stdout+result.stderr;print(result.stdout.strip())
        else:assert result.returncode!=0 and ('FAIL lifecycle' in result.stderr or 'AddressSanitizer' in result.stderr),result.stdout+result.stderr;print('Map lifecycle '+variant+' negative control passed')
if 'detect_leaks=0' in os.environ.get('ASAN_OPTIONS',''):
    print('ASan/UBSan enabled; LeakSanitizer disabled explicitly for restricted environment')
print('Map lifecycle checks passed')
