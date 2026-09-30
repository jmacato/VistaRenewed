#!/usr/bin/env python3
"""Independently validate extracted initial upload bytes and boxed geometry."""
from pathlib import Path
import os,re,subprocess,tempfile,sys
sys.dont_write_bytecode=True
ROOT=Path(__file__).resolve().parents[6]
NEPTUNE=ROOT/"triton-umd/src/virtio/neptune"

def function(source,name):
    match=re.search(r'\n(?:[A-Za-z_][^\n;{}]*\n)?[^\n;{}]*\b'+re.escape(name)+r'\([^;{}]*\)\s*\{',source)
    if not match: raise ValueError(name)
    start=match.start()+1; end=source.index('{',match.start())+1; depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]

resource=(NEPTUNE/'npt_resource.c').read_text();device=(NEPTUNE/'npt_overrides_d3d11_device.c').read_text()
formats='\n'.join(function(resource,n) for n in ('npt_dxgi_format_bytes_per_pixel','format_block_height','npt_dxgi_format_block_height','npt_dxgi_format_block_rows','npt_dxgi_format_row_bytes','npt_dxgi_format_subresource_rows'))
production='\n'.join(function(device,n) for n in ('npt_upload_texture_initial_split','npt_upload_texture_initial_data'))
constants='enum {'+', '.join(sorted(set(re.findall(r'\bDXGI_FORMAT_\w+',formats))))+'};\n'
prefix=r'''
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define NPT_INITIAL_UPDATE_CHUNK_BYTES (4u << 20)
typedef unsigned UINT;
typedef unsigned DXGI_FORMAT;
struct npt_device {int unused;}; struct npt_ring {int unused;};
typedef struct {UINT left,top,front,right,bottom,back;} D3D11_BOX;
typedef struct {const void *pSysMem;UINT SysMemPitch,SysMemSlicePitch;} D3D11_SUBRESOURCE_DATA;
static struct npt_ring ring;
static struct npt_ring *npt_device_method_ring(struct npt_device *d) {(void)d;return &ring;}
static unsigned width,height,depth,rows,row_bytes,bh,calls,fail_at;
static size_t rp,sp,total_rows;
static const unsigned char *origin;
static unsigned char *covered;
static bool fail;
#define REQUIRE(c) do {if (!(c)) {fprintf(stderr,"byte/geometry mismatch line %u\n",__LINE__);fail=true;return false;}}while(0)
static bool npt_dispatch_resource_update(struct npt_ring *r,uint64_t id,UINT sub,UINT pitch,UINT slice,const D3D11_BOX *box,const void *data,UINT bytes,UINT copy) {
 (void)r;(void)id;(void)sub;++calls;
 if(fail_at && calls==fail_at)return false;
 REQUIRE(bytes<=64u*1024u*1024u && copy<=bytes);
 unsigned top=0,bottom=height,front=0,back=depth;
 if(box){REQUIRE(box->left==0 && box->right==width);top=box->top;bottom=box->bottom;front=box->front;back=box->back;REQUIRE(top%bh==0 && bottom<=height && bottom>top && front<back && back<=depth);}
 unsigned first=top/bh,count=(bottom-top+bh-1)/bh;
 if(rows>height && !box)count=rows;
 size_t host_rp=count>1?pitch:row_bytes;
 size_t host_sp=back-front>1?slice:0;
 for(unsigned z=front;z<back;++z)for(unsigned y=0;y<count;++y){
  size_t idx=(size_t)z*rows+first+y;
  REQUIRE(idx<total_rows && !covered[idx]);covered[idx]=1;
  size_t off=(size_t)(z-front)*host_sp+(size_t)y*host_rp;
  REQUIRE(off+row_bytes<=copy);
  REQUIRE(memcmp((const unsigned char*)data+off,origin+(size_t)z*sp+(size_t)(first+y)*rp,row_bytes)==0);
 }
 return true;
}
'''
main=r'''
static int run(DXGI_FORMAT fmt,unsigned w,unsigned h,unsigned d,unsigned pad,unsigned slice_pad,unsigned injected) {
 width=w;height=h;depth=d;row_bytes=npt_dxgi_format_row_bytes(fmt,w);rows=npt_dxgi_format_subresource_rows(fmt,h);bh=npt_dxgi_format_block_height(fmt);
 rp=(size_t)row_bytes+pad;sp=rp*rows+slice_pad;total_rows=(size_t)d*rows;
 size_t extent=(size_t)(d-1)*sp+(rows-1)*rp+row_bytes;
 if(rows>h)extent=(size_t)d*sp;
 unsigned char *src=malloc(extent);covered=calloc(total_rows,1);if(!src||!covered)return 2;
 memset(src,0xda,extent);for(unsigned z=0;z<d;++z)for(unsigned y=0;y<rows;++y)for(unsigned b=0;b<row_bytes;++b)src[(size_t)z*sp+y*rp+b]=(unsigned char)(z*19u+y*7u+b);
 origin=src;calls=0;fail=false;fail_at=injected;struct npt_device dev={0};D3D11_SUBRESOURCE_DATA init={src,(UINT)rp,(UINT)sp};
 bool ok=npt_upload_texture_initial_data(&dev,9,&init,1,w,h,d,1,fmt);
 bool result=!fail && (injected ? (!ok && calls==injected) : ok);
 if(!injected)for(size_t i=0;i<total_rows;++i)if(!covered[i])result=false;
 printf("%ux%ux%u format=%u pad=%u slice-pad=%u fault=%u calls=%u %s\n",w,h,d,fmt,pad,slice_pad,injected,calls,result?"PASS":"FAIL");
 free(src);free(covered);return !result;
}
int main(void){int failures=0;
 failures+=run(DXGI_FORMAT_R8G8B8A8_UNORM,4,1,1,0,0,0);
 failures+=run(DXGI_FORMAT_R8G8B8A8_UNORM,17,11,3,28,36,0);
 failures+=run(DXGI_FORMAT_R8G8B8A8_UNORM,8192,2049,1,20,0,0);
 failures+=run(DXGI_FORMAT_R8G8B8A8_UNORM,512,512,65,16,64,0);
 failures+=run(DXGI_FORMAT_BC1_UNORM,16384,16381,1,12,0,0);
 failures+=run(DXGI_FORMAT_R8G8B8A8_UNORM,8192,2049,1,20,0,3);
 failures+=run(DXGI_FORMAT_NV12,128,64,1,16,0,0);
 failures+=run(DXGI_FORMAT_P010,128,64,1,16,0,0);
 failures+=run(DXGI_FORMAT_P016,128,64,1,16,0,0);
 failures+=run(DXGI_FORMAT_NV11,128,64,1,16,0,0);
 return failures?1:0;
}
'''
code=prefix+constants+formats+production+main
with tempfile.TemporaryDirectory(prefix='review12-initial-') as tmp:
 tmp=Path(tmp);(tmp/'check.c').write_text(code)
 p=subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror','-O1','-g','-fsanitize=address,undefined',str(tmp/'check.c'),'-o',str(tmp/'check')],capture_output=True,text=True)
 if p.returncode:print(p.stderr);sys.exit(p.returncode)
 p=subprocess.run([str(tmp/'check')],capture_output=True,text=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
 print(p.stdout+p.stderr,end='')
 assert p.returncode == 0, 'initial texture byte/geometry checks failed'

print("Initial texture transport byte and geometry checks passed")

# Degenerate mip strides are legal: no next row/slice is ever addressed.
mip_prefix = '\n#include <stdbool.h>\n#include <stdint.h>\n#include <stddef.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n#define NPT_INITIAL_UPDATE_CHUNK_BYTES (4u << 20)\ntypedef unsigned UINT;typedef unsigned DXGI_FORMAT;\nstruct npt_device {int unused;};struct npt_ring {int unused;};\ntypedef struct {UINT left,top,front,right,bottom,back;} D3D11_BOX;\ntypedef struct {const void *pSysMem;UINT SysMemPitch,SysMemSlicePitch;} D3D11_SUBRESOURCE_DATA;\nstatic unsigned calls;static struct npt_ring ring;\nstatic struct npt_ring *npt_device_method_ring(struct npt_device *d){(void)d;return &ring;}\nstatic unsigned npt_dxgi_format_subresource_rows(unsigned f,unsigned h){(void)f;return h;}\nstatic unsigned npt_dxgi_format_block_rows(unsigned f,unsigned h){(void)f;return h;}\nstatic unsigned npt_dxgi_format_block_height(unsigned f){(void)f;return 1;}\nstatic unsigned npt_dxgi_format_row_bytes(unsigned f,unsigned w){(void)f;return w*4;}\nstatic bool npt_dispatch_resource_update(struct npt_ring *r,uint64_t id,UINT sub,UINT row,UINT depth,const D3D11_BOX *box,const void *data,UINT bytes,UINT copy){(void)r;(void)id;(void)sub;(void)row;(void)depth;(void)box;(void)data;(void)bytes;(void)copy;++calls;return true;}\n'
mip_main = '\nint main(void){\n struct npt_device d={0};unsigned char a[256]={0},b[32]={0},c[4]={3,7,11,19};\n D3D11_SUBRESOURCE_DATA data[3]={{a,16,64},{b,8,16},{c,0,0}};\n bool ok=npt_upload_texture_initial_data(&d,1,data,3,4,4,4,3,28);\n printf("4x4x4 full mip chain with 1x1x1 zero-stride tail: ok=%u calls=%u (required 1,3)\\n",ok,calls);\n return !(ok && calls==3);\n}\n'
for label, variant in (
    ('current', production),
    ('base-depth', production.replace('md > 1u', 'depth > 1u')),
    ('base-rows', production.replace('rows > 1u ? d->SysMemPitch', 'height > 1u ? d->SysMemPitch')),
):
    with tempfile.TemporaryDirectory(prefix='initial-mips-') as temporary:
        work = Path(temporary)
        (work / 'test.c').write_text(mip_prefix + variant + mip_main)
        subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        str(work / 'test.c'), '-o', str(work / 'test')], check=True)
        result = subprocess.run([str(work / 'test')], capture_output=True, text=True)
        assert (result.returncode == 0) == (label == 'current'), result.stdout + result.stderr
        print('Initial mip strides ' + label + ': ' + result.stdout.strip())
layout_prefix = '\n#include <stdbool.h>\n#include <stdint.h>\n#include <stdio.h>\n#include <stddef.h>\ntypedef unsigned DXGI_FORMAT;typedef struct {unsigned left,top,front,right,bottom,back;} D3D11_BOX;\nstruct npt_com_base{void *aux;};struct npt_d3d11_texture{struct npt_com_base base;};\nstruct npt_d3d11_texture_aux{unsigned width,height,depth,mip_levels,array_size;DXGI_FORMAT format;};\n'
layout_main = '\nint main(void){\n struct npt_d3d11_texture_aux a={128,64,1,1,1,DXGI_FORMAT_NV11};struct npt_d3d11_texture t={{&a}};\n D3D11_BOX region;unsigned row_bytes,rows,bh;bool planar;\n bool ok=npt_d3d11_texture_get_update_layout(&t,0,NULL,&region,&row_bytes,&rows,&bh,&planar);\n printf("NV11 accepted=%u row_bytes=%u rows=%u planar=%u (required: 1,128,128,1)\\n",ok,row_bytes,rows,planar);\n return !(ok && row_bytes==128 && rows==128 && planar && bh==1);\n}\n'
layout_names = ('tex_aux', 'npt_dxgi_format_bytes_per_pixel', 'format_block_height', 'npt_dxgi_format_block_rows', 'npt_dxgi_format_row_bytes', 'npt_dxgi_format_subresource_rows', 'mip_dim', 'texture_subresource_mip', 'npt_d3d11_texture_get_update_layout')
layout = '\n'.join(function(resource, name) for name in layout_names)
for label, variant in (
    ('current', layout),
    ('missing-nv11', layout.replace('case DXGI_FORMAT_NV11:', 'case -111:')),
):
    with tempfile.TemporaryDirectory(prefix='initial-planar-') as temporary:
        work = Path(temporary)
        (work / 'test.c').write_text(layout_prefix + constants + variant + layout_main)
        subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        str(work / 'test.c'), '-o', str(work / 'test')], check=True)
        result = subprocess.run([str(work / 'test')], capture_output=True, text=True)
        assert (result.returncode == 0) == (label == 'current'), result.stdout + result.stderr
        print('NV11 update layout ' + label + ': ' + result.stdout.strip())
print('D3D10/11 initial transport checks passed (CPU source contracts)')
