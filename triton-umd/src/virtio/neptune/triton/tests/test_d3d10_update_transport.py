#!/usr/bin/env python3
"""Production texture/buffer upload normalization, wire sizing and fault state."""
from pathlib import Path
import importlib.util
import re
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[6];NPT=ROOT/'triton-umd/src/virtio/neptune'
spec=importlib.util.spec_from_file_location('extract',ROOT/'tests/vista/test-d3d10-texture-map.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
r=(NPT/'npt_resource.c').read_text();c=(NPT/'npt_overrides_d3d11_context.c').read_text();h=(NPT/'npt_resource.h').read_text()
resource='\n'.join(m.function(r,n) for n in ('tex_aux','npt_dxgi_format_bytes_per_pixel','format_block_height','npt_dxgi_format_block_rows','npt_dxgi_format_row_bytes','npt_dxgi_format_subresource_rows','mip_dim','texture_subresource_mip','npt_d3d11_texture_get_update_layout'))
start=c.index('#define NPT_RESOURCE_UPDATE_CHUNK_BYTES');end=c.index('\nvoid\nnpt_overrides_d3d11_context_init',start);context=c[start:end]
ring=m.function((NPT/'npt_ring.c').read_text(),'npt_ring_submit_raw_with_payload')
dispatch=m.function((NPT/'npt_dispatch.c').read_text(),'npt_dispatch_resource_update')
removal=m.function((NPT/'npt_overrides_d3d11_device.c').read_text(),'dev_GetDeviceRemovedReason_override')
structures=''
for name in ('npt_d3d11_texture_aux',):
 start=h.index('struct '+name+' {');structures+=h[start:h.index('};',start)+2]+'\n'
wire=''
for file,name in ((NPT/'neptune-protocol/npt_protocol_defs.h','npt_command_header'),(NPT/'npt_transport_defs.h','npt_cmd_resource_update')):
 s=file.read_text();start=s.index('struct '+name+' {');wire+=s[start:s.index('};',start)+2]+'\n'
fixture=Path(__file__).with_name('tritonD3D10UpdateTransportTest.c').read_text()
constants='enum { '+', '.join(sorted(set(re.findall(r'\bDXGI_FORMAT_\w+',resource+fixture))))+' };\n'
base=fixture.replace('// FORMAT_CONSTANTS',constants).replace('// RESOURCE_STRUCTURES',structures).replace('// WIRE_STRUCTURES',wire).replace('// PRODUCTION_RESOURCE',resource).replace('// PRODUCTION_RING',ring).replace('// PRODUCTION_DISPATCH',dispatch).replace('// PRODUCTION_CONTEXT',context).replace('// PRODUCTION_REMOVAL',removal)
mutants={
 'source-row-stride':('data + (size_t)row * row_pitch','data + (size_t)row * row_bytes'),
 'source-slice-stride':('(size_t)z * depth_pitch + (size_t)y * row_pitch','(size_t)z * row_pitch * rows + (size_t)y * row_pitch'),
 'first-mip':('const uint32_t mip = texture_subresource_mip(aux, subresource);','const uint32_t mip = 0;'),
 'host-size-cap':('#define NPT_RESOURCE_UPDATE_CHUNK_BYTES (4u << 20)','#define NPT_RESOURCE_UPDATE_CHUNK_BYTES (128u << 20)'),
 'ignored-failure':('if (!ok)\n      ctx_update_failed(dev, ring);','if (!ok)\n      (void)dev;'),
 'missing-free':('free(packed);','/* leaked packed memory */'),
 'huge-pitch':('const uint64_t row_pitch = rows > 1 ? source_row_pitch : row_bytes;', 'if (source_row_pitch == UINT32_MAX) return npt_dispatch_resource_update(ring, resource_id, subresource, source_row_pitch, source_depth_pitch, box, source, UINT32_MAX, row_bytes);\n   const uint64_t row_pitch = rows > 1 ? source_row_pitch : row_bytes;'),
}
for variant in ('current',*mutants):
 code=base
 if variant!='current':
  old,new=mutants[variant];assert old in code,variant;code=code.replace(old,new)
 with tempfile.TemporaryDirectory(prefix='triton-update-') as d:
  d=Path(d);(d/'test.c').write_text(code)
  p=subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-parameter','-O1','-g','-fsanitize=address,undefined',str(d/'test.c'),'-o',str(d/'test')],capture_output=True,text=True);assert p.returncode==0,p.stdout+p.stderr
  p=subprocess.run([str(d/'test')],capture_output=True,text=True)
  if variant=='current':assert p.returncode==0,p.stdout+p.stderr;print(p.stdout.strip())
  else:assert p.returncode!=0 and ('FAIL update transport' in p.stderr or 'AddressSanitizer' in p.stderr),p.stdout+p.stderr;print('D3D10 update transport '+variant+' negative control rejected')
print('D3D10 update transport checks passed (CPU wire contracts; live paired runtime pending)')
