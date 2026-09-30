#!/usr/bin/env python3
"""Execute imported texture metadata, upload and Map entry paths, including races."""
from pathlib import Path
import importlib.util
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[6]
NPT = ROOT / 'triton-umd/src/virtio/neptune'
spec = importlib.util.spec_from_file_location('extract', ROOT / 'tests/vista/test-d3d10-texture-map.py')
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
assembly_path = Path(__file__).with_name('test_d3d10_update_transport.py')
ns = {'__file__': str(assembly_path)}
exec(compile(assembly_path.read_text().split('for variant in (')[0], str(assembly_path), 'exec'), ns)
base = ns['base']
resource = (NPT / 'npt_resource.c').read_text()
header = (NPT / 'npt_resource.h').read_text()
context = (NPT / 'npt_overrides_d3d11_context.c').read_text()
texture = (NPT / 'npt_overrides_d3d11_texture.c').read_text()
bridge = (NPT / 'triton/tritonSharedBridge.c').read_text()
start = header.index('struct npt_d3d11_texture_desc {')
texture_desc = header[start:header.index('};', start) + 2]
base = '#define _POSIX_C_SOURCE 200809L\n' + base
base = base.replace('typedef unsigned mtx_t;', '''#include <pthread.h>
#include <time.h>
typedef pthread_mutex_t mtx_t;
#define mtx_plain 0
static int mtx_init(mtx_t*m,int type){(void)type;return pthread_mutex_init(m,NULL);}
static int mtx_destroy(mtx_t*m){return pthread_mutex_destroy(m);}
static int mtx_lock(mtx_t*m){return pthread_mutex_lock(m);}
static int mtx_unlock(mtx_t*m){return pthread_mutex_unlock(m);}
''')
base = base.replace('static int mtx_lock(unsigned*m)', 'static int ring_lock(unsigned*m)')
base = base.replace('static int mtx_unlock(unsigned*m)', 'static int ring_unlock(unsigned*m)')
base = base.replace('mtx_lock(&ring->mutex)', 'ring_lock(&ring->mutex)')
base = base.replace('mtx_unlock(&ring->mutex)', 'ring_unlock(&ring->mutex)')
base = base.replace('typedef uint32_t UINT,DXGI_FORMAT,D3D11_TEXTURE_LAYOUT;', '''typedef uint32_t UINT,DXGI_FORMAT,D3D11_TEXTURE_LAYOUT,D3D11_USAGE,D3D11_RESOURCE_DIMENSION,D3D11_MAP;
enum {D3D11_RESOURCE_DIMENSION_TEXTURE1D=2,D3D11_RESOURCE_DIMENSION_TEXTURE2D=3,D3D11_RESOURCE_DIMENSION_TEXTURE3D=4};
enum {D3D11_USAGE_DYNAMIC=2,D3D11_USAGE_STAGING=3,D3D11_CPU_ACCESS_READ=0x20000,D3D11_CPU_ACCESS_WRITE=0x10000};
typedef struct {UINT Count,Quality;} SAMPLE_DESC;
typedef struct {UINT Width,MipLevels,ArraySize,Format,Usage,BindFlags,CPUAccessFlags,MiscFlags;} D3D11_TEXTURE1D_DESC;
typedef struct {UINT Width,Height,MipLevels,ArraySize,Format;SAMPLE_DESC SampleDesc;UINT Usage,BindFlags,CPUAccessFlags,MiscFlags;} D3D11_TEXTURE2D_DESC;
typedef struct {UINT Width,Height,MipLevels,ArraySize,Format;SAMPLE_DESC SampleDesc;UINT Usage,BindFlags,CPUAccessFlags,MiscFlags,TextureLayout;} D3D11_TEXTURE2D_DESC1;
typedef struct {UINT Width,Height,Depth,MipLevels,Format,Usage,BindFlags,CPUAccessFlags,MiscFlags;} D3D11_TEXTURE3D_DESC;
typedef struct {UINT Width,Height,Depth,MipLevels,Format,Usage,BindFlags,CPUAccessFlags,MiscFlags,TextureLayout;} D3D11_TEXTURE3D_DESC1;
''')
base = base.replace('struct {uint64_t id;} base;', 'struct {uint64_t id;struct npt_com_base *parent;} base;')
base = base.replace('unsigned kind,width;};', 'unsigned kind,width;void(*aux_destroy)(void*);};')
base = base.replace('struct npt_device {struct npt_ring *ring,*method;};', 'struct npt_device {struct npt_ring *ring,*method;void *renderer;};')
base = base.replace('static struct npt_device device={&primary,&method};', 'static struct npt_device device={&primary,&method,(void*)1};')
base = base.replace('static bool npt_d3d11_texture_ensure_desc(struct npt_d3d11_texture*t,unsigned dimension,bool extended){return true;}', 'bool npt_d3d11_texture_ensure_desc(struct npt_d3d11_texture*t,D3D11_RESOURCE_DIMENSION dimension,bool extended);')
base = base.replace('static struct npt_d3d11_texture *npt_d3d11_texture_cast(void*r){return ((struct npt_com_base*)r)->kind==2?r:NULL;}', 'struct npt_d3d11_texture *npt_d3d11_texture_cast(void*r);')
base = base[:base.index('int main(void){')]
base += '\n' + texture_desc + '\n'
base += r'''
static unsigned base_rpcs,extended_rpcs,type_rpcs,opens;
static atomic_uint rpc_active,rpc_max;
static bool zero_reply,lose_transport,wrong_extended,wrap_failure;
static UINT host_dimension;
static struct npt_d3d11_texture_desc host_desc;
static void rpc_enter(void){
 unsigned count=atomic_fetch_add(&rpc_active,1)+1,old=atomic_load(&rpc_max);
 while(count>old&&!atomic_compare_exchange_weak(&rpc_max,&old,count)){}
 struct timespec delay={0,1000000};nanosleep(&delay,NULL);
}
static void rpc_leave(void){if(lose_transport)atomic_store(&method.failed,true);atomic_fetch_sub(&rpc_active,1);}
static void npt_id3d11resource_default_GetType(void*self,D3D11_RESOURCE_DIMENSION*out){rpc_enter();++type_rpcs;if(!zero_reply)*out=host_dimension;rpc_leave();}
static void npt_id3d11texture1d_default_GetDesc(void*self,D3D11_TEXTURE1D_DESC*out){rpc_enter();++base_rpcs;if(!zero_reply)*out=(D3D11_TEXTURE1D_DESC){host_desc.width,host_desc.mip_levels,host_desc.array_size,host_desc.format,host_desc.usage,host_desc.bind_flags,host_desc.cpu_access_flags,host_desc.misc_flags};rpc_leave();}
static void npt_id3d11texture2d_default_GetDesc(void*self,D3D11_TEXTURE2D_DESC*out){rpc_enter();++base_rpcs;if(!zero_reply)*out=(D3D11_TEXTURE2D_DESC){host_desc.width,host_desc.height,host_desc.mip_levels,host_desc.array_size,host_desc.format,{host_desc.sample_count,host_desc.sample_quality},host_desc.usage,host_desc.bind_flags,host_desc.cpu_access_flags,host_desc.misc_flags};rpc_leave();}
static void npt_id3d11texture2d1_default_GetDesc1(void*self,D3D11_TEXTURE2D_DESC1*out){rpc_enter();++extended_rpcs;if(!zero_reply)*out=(D3D11_TEXTURE2D_DESC1){host_desc.width+(wrong_extended?1:0),host_desc.height,host_desc.mip_levels,host_desc.array_size,host_desc.format,{host_desc.sample_count,host_desc.sample_quality},host_desc.usage,host_desc.bind_flags,host_desc.cpu_access_flags,host_desc.misc_flags,host_desc.texture_layout};rpc_leave();}
static void npt_id3d11texture3d_default_GetDesc(void*self,D3D11_TEXTURE3D_DESC*out){rpc_enter();++base_rpcs;if(!zero_reply)*out=(D3D11_TEXTURE3D_DESC){host_desc.width,host_desc.height,host_desc.depth,host_desc.mip_levels,host_desc.format,host_desc.usage,host_desc.bind_flags,host_desc.cpu_access_flags,host_desc.misc_flags};rpc_leave();}
static void npt_id3d11texture3d1_default_GetDesc1(void*self,D3D11_TEXTURE3D_DESC1*out){rpc_enter();++extended_rpcs;if(!zero_reply)*out=(D3D11_TEXTURE3D_DESC1){host_desc.width,host_desc.height,host_desc.depth,host_desc.mip_levels,host_desc.format,host_desc.usage,host_desc.bind_flags,host_desc.cpu_access_flags,host_desc.misc_flags,host_desc.texture_layout};rpc_leave();}
static void npt_d3d_map_ring_init(struct npt_d3d_map_ring*r,struct npt_com_base*com){}
static void npt_d3d_map_ring_fini(struct npt_d3d_map_ring*r){}
'''
resource_names = ['npt_d3d11_texture_aux_destroy', 'npt_d3d11_texture_aux_init', 'npt_d3d11_texture_cast', 'texture_publish_desc',
                  'npt_d3d11_texture_set_desc', 'npt_d3d11_texture_has_desc',
                  'npt_d3d11_texture_ensure_desc', 'npt_d3d11_texture_is_mappable',
                  'npt_d3d11_texture_get_mip_dimensions', 'npt_d3d11_texture_get_format']
resource_names += ['npt_d3d11_texture_fill_desc' + suffix for suffix in ('1d','2d','2d1','3d','3d1')]
base += '\n'.join(m.function(resource,n) for n in resource_names) + '\n'
base += '\n'.join(m.function(texture,n) for n in ('tex1d_GetDesc_override','tex2d_GetDesc_override','tex2d1_GetDesc1_override','tex3d_GetDesc_override','tex3d1_GetDesc1_override')) + '\n'
base += r'''
#define NPT_BLOB_EXPORT_MAX_PLANES 4
#define NPT_FAILED(hr) ((hr)<0)
static int NPT_IID_ID3D11Texture2D;
struct triton_shared_texture_desc {
 uint64_t blob_id;uint32_t create_ctx_id,plane_count,texture_layout;uint64_t modifier,allocation_size;
 struct {uint64_t offset,pitch;} planes[4];
 uint32_t width,height,mip_levels,array_size,format,sample_count,usage,bind_flags,cpu_access_flags,misc_flags;
};
struct npt_cmd_shared_open_res {
 uint64_t mint_object_id;uint32_t res_id,width,height,mip_levels,array_size,format,sample_count,usage,bind_flags,cpu_access_flags,misc_flags;
 struct {uint64_t modifier,allocation_size;uint32_t plane_count,texture_layout;struct {uint64_t offset,pitch;} planes[4];} export_info;
};
static uint64_t npt_com_allocate_next_id(void){return 77;}
static HRESULT npt_dispatch_shared_open_res(struct npt_ring*r,uint64_t id,struct npt_cmd_shared_open_res*c){++opens;CHECK(c->mint_object_id==77&&c->res_id==45&&c->width==8&&c->height==8);return 0;}
static void*npt_com_get_or_wrap_or_release(struct npt_device*d,const void*iid,uint64_t id,struct npt_com_base*parent){return wrap_failure?NULL:&object;}
static unsigned releases;
static unsigned npt_com_default_release(void*p){++releases;return 0;}
'''
base += m.function(bridge, 'tritonSharedBridgeOpenRes') + '\n'
# Exercise the actual Map entry. Storage and RPC are independent spy seams;
# the existing texture-map regression separately verifies their geometry.
base += r'''
#define NPT_S_OK 0
#define NPT_E_FAIL (-1)
#define NPT_E_NOTIMPL (-2)
#define NPT_E_OUTOFMEMORY (-3)
#define NPT_PERF(x) 0
#define D3D11_MAP_WRITE_DISCARD 4
#define D3D11_MAP_WRITE_NO_OVERWRITE 5
#define D3D11_MAP_READ 1
#define NPT_MAP_ACCESS_READ 1
#define NPT_MAP_ACCESS_WRITE 2
#define NPT_MAP_ACCESS_DISCARD 4
#define NPT_MAP_ACCESS_NO_OVERWRITE 8
#define D3D11_MAP_WRITE 2
#define D3D11_MAP_READ_WRITE 3
typedef struct {void*pData;UINT RowPitch,DepthPitch;} D3D11_MAPPED_SUBRESOURCE;
static unsigned maps,shmem_calls;
static uint8_t mapped_storage[256];
static bool npt_d3d11_texture_get_is_mapped(struct npt_d3d11_texture*t){return false;}
static bool npt_d3d11_texture_ensure_map_shmem(struct npt_d3d11_texture*t){++shmem_calls;CHECK(npt_d3d11_texture_has_desc(t)&&aux.width==8&&aux.height==8);return true;}
static UINT npt_d3d11_texture_get_slot_size(struct npt_d3d11_texture*t){return sizeof(mapped_storage);}
static UINT npt_d3d11_texture_get_cached_row_pitch(struct npt_d3d11_texture*t){return 0;}
static UINT npt_d3d11_texture_get_cached_depth_pitch(struct npt_d3d11_texture*t){return 0;}
static UINT npt_d3d11_texture_get_last_map_subresource(struct npt_d3d11_texture*t){return 0;}
static UINT npt_d3d11_texture_get_current_slot(struct npt_d3d11_texture*t){return 0;}
static UINT npt_d3d11_texture_rotate_slot(struct npt_d3d11_texture*t){return 0;}
static void*npt_d3d11_texture_slot_ptr(struct npt_d3d11_texture*t,UINT slot){return mapped_storage;}
static void*npt_d3d11_texture_shmem_ptr(struct npt_d3d11_texture*t){return mapped_storage;}
static void npt_d3d11_texture_set_current_slot(struct npt_d3d11_texture*t,UINT slot){}
static void npt_d3d11_texture_set_mapped_state(struct npt_d3d11_texture*t,UINT sub,UINT rp,UINT bytes,UINT flags){}
static void npt_d3d11_texture_set_cached_pitches(struct npt_d3d11_texture*t,UINT rp,UINT dp){}
static UINT npt_d3d11_texture_get_map_shmem_res_id(struct npt_d3d11_texture*t){return 99;}
static UINT npt_d3d11_texture_slot_offset(struct npt_d3d11_texture*t,UINT slot){return 0;}
static UINT npt_d3d11_texture_get_subresource_map_byte_size(struct npt_d3d11_texture*t,UINT sub,UINT rp,UINT dp){return 256;}
static bool npt_d3d11_texture_grow_map_shmem(struct npt_d3d11_texture*t,UINT size){return false;}
static HRESULT npt_dispatch_resource_abort(struct npt_ring*r,uint64_t c,uint64_t id,UINT sub){return 0;}
static HRESULT npt_dispatch_resource_map(struct npt_ring*r,uint64_t c,uint64_t id,UINT sub,UINT access,UINT flags,UINT shm,UINT size,UINT h,UINT d,UINT off,UINT*rp,UINT*dp){++maps;CHECK(h==8&&d==1&&size==256);*rp=32;*dp=256;return 0;}
'''
base += m.function(context, 'npt_d3d11_map_to_access_flags') + '\n' + m.function(context, 'ctx_Map_texture') + '\n'
base += r'''
static bool initialized;
static void fresh(UINT dimension){
 if(initialized)mtx_destroy(&aux.desc_mutex);initialized=true;
 setup(0,0,0,1,1,DXGI_FORMAT_UNKNOWN);npt_d3d11_texture_aux_init(&object,&device,77);
 host_dimension=dimension;host_desc=(struct npt_d3d11_texture_desc){8,8,1,1,1,DXGI_FORMAT_R8G8B8A8_UNORM,1,0,0,40,0,2,2};
 if(dimension==D3D11_RESOURCE_DIMENSION_TEXTURE1D)host_desc.height=1;
 if(dimension==D3D11_RESOURCE_DIMENSION_TEXTURE3D)host_desc.depth=4;
 base_rpcs=extended_rpcs=type_rpcs=opens=maps=shmem_calls=0;zero_reply=lose_transport=wrong_extended=wrap_failure=false;
 atomic_store(&rpc_active,0);atomic_store(&rpc_max,0);
 CHECK(atomic_load(&aux.desc_state)==0&&!npt_d3d11_texture_has_desc((void*)&object));
}
static void upload(void*resource){
 target=(D3D11_BOX){0,0,0,8,8,1};target_sub=0;rb=32;rows=8;depth=bh=1;is_buffer=false;result_bytes=256;
 uint8_t input[256];for(UINT i=0;i<256;++i)input[i]=(uint8_t)(i*13+19);
 result=calloc(256,1);coverage=calloc(256,1);ctx_UpdateSubresource_override(NULL,resource,0,NULL,input,32,0);
 CHECK(submits==1&&!primary.failed&&!method.failed&&!memcmp(result,input,256));free(result);free(coverage);result=coverage=NULL;
}
static void foreign_destroy(void*p){}
static void*reader(void*argument){
 uintptr_t extended=(uintptr_t)argument;
 for(unsigned i=0;i<200;++i){
  if(extended){D3D11_TEXTURE2D_DESC1 d={0};tex2d1_GetDesc1_override(&object,&d);if(d.Width!=8||d.Height!=8||d.TextureLayout!=2)return(void*)1;}
  else{D3D11_TEXTURE2D_DESC d={0};tex2d_GetDesc_override(&object,&d);if(d.Width!=8||d.Height!=8)return(void*)1;}
 }
 return NULL;
}
int main(void){
 fresh(D3D11_RESOURCE_DIMENSION_TEXTURE2D);
 struct triton_shared_texture_desc opts={0};opts.width=opts.height=8;opts.mip_levels=opts.array_size=opts.sample_count=opts.plane_count=1;opts.format=DXGI_FORMAT_R8G8B8A8_UNORM;opts.bind_flags=40;opts.misc_flags=2;opts.texture_layout=1;opts.allocation_size=4096;opts.planes[0].pitch=32;
 CHECK(tritonSharedBridgeOpenRes((void*)1,45,&opts)==&object);CHECK(atomic_load(&aux.desc_state)==2&&aux.width==8&&aux.texture_layout==1);upload(&object);CHECK(!base_rpcs&&!extended_rpcs&&!type_rpcs);
 struct npt_com_base alias=object;alias.aux_destroy=NULL;alias.base.parent=&object;CHECK(npt_d3d11_texture_cast(&alias)==(void*)&alias);submits=0;upload(&alias);struct npt_com_base alias2=alias;alias2.base.parent=&alias;CHECK(npt_d3d11_texture_cast(&alias2)==(void*)&alias2);D3D11_TEXTURE2D_DESC1 alias_desc={0};tex2d1_GetDesc1_override(&alias2,&alias_desc);CHECK(alias_desc.Width==8&&alias_desc.TextureLayout==1&&!base_rpcs&&!extended_rpcs);alias2.aux=NULL;CHECK(!npt_d3d11_texture_cast(&alias2));alias2.aux=(void*)1;CHECK(!npt_d3d11_texture_cast(&alias2));alias2.aux=object.aux;alias.aux_destroy=foreign_destroy;CHECK(!npt_d3d11_texture_cast(&alias2));CHECK(!npt_d3d11_texture_cast(NULL));
 fresh(D3D11_RESOURCE_DIMENSION_TEXTURE2D);wrap_failure=true;CHECK(!tritonSharedBridgeOpenRes((void*)1,45,&opts));CHECK(!atomic_load(&aux.desc_state));
 fresh(D3D11_RESOURCE_DIMENSION_TEXTURE2D);upload(&object);CHECK(type_rpcs==1&&base_rpcs==1&&atomic_load(&aux.desc_state)==1);
 fresh(D3D11_RESOURCE_DIMENSION_TEXTURE2D);host_desc.usage=D3D11_USAGE_STAGING;host_desc.cpu_access_flags=D3D11_CPU_ACCESS_READ;D3D11_MAPPED_SUBRESOURCE mapped={0};CHECK(ctx_Map_texture(&object,(void*)&object,0,D3D11_MAP_READ,0,&mapped)==0);CHECK(mapped.pData==mapped_storage&&maps==1&&shmem_calls==1&&type_rpcs==1&&base_rpcs==1);
 fresh(D3D11_RESOURCE_DIMENSION_TEXTURE2D);D3D11_TEXTURE2D_DESC d2={0};tex2d_GetDesc_override(&object,&d2);CHECK(d2.Width==8&&base_rpcs==1&&!type_rpcs);D3D11_TEXTURE2D_DESC1 x2={0};tex2d1_GetDesc1_override(&object,&x2);CHECK(x2.TextureLayout==2&&base_rpcs==1&&extended_rpcs==1);tex2d1_GetDesc1_override(&object,&x2);CHECK(extended_rpcs==1);
 fresh(D3D11_RESOURCE_DIMENSION_TEXTURE1D);D3D11_TEXTURE1D_DESC d1={0};tex1d_GetDesc_override(&object,&d1);CHECK(d1.Width==8&&aux.height==1&&aux.depth==1&&atomic_load(&aux.desc_state)==2&&!type_rpcs&&base_rpcs==1);
 fresh(D3D11_RESOURCE_DIMENSION_TEXTURE3D);D3D11_TEXTURE3D_DESC d3={0};tex3d_GetDesc_override(&object,&d3);CHECK(d3.Depth==4&&aux.array_size==1&&atomic_load(&aux.desc_state)==1);D3D11_TEXTURE3D_DESC1 x3={0};tex3d1_GetDesc1_override(&object,&x3);CHECK(x3.Depth==4&&x3.TextureLayout==2&&extended_rpcs==1);
 fresh(D3D11_RESOURCE_DIMENSION_TEXTURE2D);tex2d1_GetDesc1_override(&object,&x2);CHECK(x2.Width==8&&x2.TextureLayout==2&&!base_rpcs&&extended_rpcs==1);
 fresh(D3D11_RESOURCE_DIMENSION_TEXTURE2D);zero_reply=true;memset(&d2,0xa5,sizeof(d2));tex2d_GetDesc_override(&object,&d2);CHECK(!d2.Width&&!atomic_load(&aux.desc_state));zero_reply=false;tex2d_GetDesc_override(&object,&d2);CHECK(d2.Width==8&&base_rpcs==2);
 fresh(D3D11_RESOURCE_DIMENSION_TEXTURE2D);lose_transport=true;tex2d_GetDesc_override(&object,&d2);CHECK(!d2.Width&&!atomic_load(&aux.desc_state));lose_transport=false;atomic_store(&method.failed,false);tex2d_GetDesc_override(&object,&d2);CHECK(d2.Width==8&&base_rpcs==2);
 fresh(D3D11_RESOURCE_DIMENSION_TEXTURE2D);zero_reply=true;CHECK(!npt_d3d11_texture_ensure_desc((void*)&object,D3D11_RESOURCE_DIMENSION_UNKNOWN,false));CHECK(!atomic_load(&aux.desc_state));
 fresh(D3D11_RESOURCE_DIMENSION_TEXTURE2D);tex2d_GetDesc_override(&object,&d2);wrong_extended=true;tex2d1_GetDesc1_override(&object,&x2);CHECK(!x2.Width&&aux.width==8&&atomic_load(&aux.desc_state)==1);wrong_extended=false;tex2d1_GetDesc1_override(&object,&x2);CHECK(x2.Width==8&&x2.TextureLayout==2&&extended_rpcs==2);
 for(unsigned round=0;round<8;++round){fresh(D3D11_RESOURCE_DIMENSION_TEXTURE2D);pthread_t threads[8];for(uintptr_t i=0;i<8;++i)CHECK(!pthread_create(&threads[i],NULL,reader,(void*)(i&1)));for(unsigned i=0;i<8;++i){void*ret=NULL;CHECK(!pthread_join(threads[i],&ret)&&!ret);}CHECK(base_rpcs<=1&&extended_rpcs==1&&atomic_load(&rpc_max)==1&&atomic_load(&aux.desc_state)==2);}
 mtx_destroy(&aux.desc_mutex);printf("D3D10 imported metadata %u checks passed (real lazy/bridge/GetDesc/Map/update paths)\n",checks);return 0;
}
'''
# Optional auxiliary allocation can fail after a usable host wrapper exists.
# Exercise every public getter shape independently, including repeated uncached
# queries, null outputs, and no reply. Existing-aux RPC failures must stay one
# RPC and must not fall through to the uncached fallback.
getters = (
    ('1d', 'tex1d_GetDesc_override', 'npt_id3d11texture1d_default_GetDesc',
     'D3D11_TEXTURE1D_DESC', 'D3D11_RESOURCE_DIMENSION_TEXTURE1D',
     '{8,1,1,DXGI_FORMAT_R8G8B8A8_UNORM,0,40,0,2}'),
    ('2d', 'tex2d_GetDesc_override', 'npt_id3d11texture2d_default_GetDesc',
     'D3D11_TEXTURE2D_DESC', 'D3D11_RESOURCE_DIMENSION_TEXTURE2D',
     '{8,8,1,1,DXGI_FORMAT_R8G8B8A8_UNORM,{1,0},0,40,0,2}'),
    ('2d1', 'tex2d1_GetDesc1_override', 'npt_id3d11texture2d1_default_GetDesc1',
     'D3D11_TEXTURE2D_DESC1', 'D3D11_RESOURCE_DIMENSION_TEXTURE2D',
     '{8,8,1,1,DXGI_FORMAT_R8G8B8A8_UNORM,{1,0},0,40,0,2,2}'),
    ('3d', 'tex3d_GetDesc_override', 'npt_id3d11texture3d_default_GetDesc',
     'D3D11_TEXTURE3D_DESC', 'D3D11_RESOURCE_DIMENSION_TEXTURE3D',
     '{8,8,4,1,DXGI_FORMAT_R8G8B8A8_UNORM,0,40,0,2}'),
    ('3d1', 'tex3d1_GetDesc1_override', 'npt_id3d11texture3d1_default_GetDesc1',
     'D3D11_TEXTURE3D_DESC1', 'D3D11_RESOURCE_DIMENSION_TEXTURE3D',
     '{8,8,4,1,DXGI_FORMAT_R8G8B8A8_UNORM,0,40,0,2,2}'),
)
aux_cases = ''
for tag, getter, rpc, desc_type, dimension, expected in getters:
    aux_cases += f'''
 {{
  const {desc_type} expected={expected}, empty={{0}};
  {desc_type} got;
  fresh({dimension});object.aux=NULL;
  {getter}(&object,NULL);CHECK(!base_rpcs&&!extended_rpcs&&!type_rpcs);
  for(unsigned repeat=0;repeat<2;++repeat){{
   memset(&got,0xa5,sizeof(got));{getter}(&object,&got);
   CHECK(!memcmp(&got,&expected,sizeof(got)));
   CHECK(base_rpcs+extended_rpcs==repeat+1&&!type_rpcs&&!object.aux);
  }}
  zero_reply=true;memset(&got,0xa5,sizeof(got));{getter}(&object,&got);
  CHECK(!memcmp(&got,&empty,sizeof(got))&&base_rpcs+extended_rpcs==3);
  for(unsigned failure=0;failure<2;++failure){{
   fresh({dimension});zero_reply=!failure;lose_transport=failure;
   memset(&got,0xa5,sizeof(got));{getter}(&object,&got);
   CHECK(!memcmp(&got,&empty,sizeof(got)));
   CHECK(base_rpcs+extended_rpcs==1&&!type_rpcs&&!atomic_load(&aux.desc_state));
  }}
 }}
'''
base = base.replace('int main(void){', 'int main(void){\n' + aux_cases, 1)

# UNKNOWN is referenced by the lazy helper, not the original upload assembly.
if 'DXGI_FORMAT_UNKNOWN,' not in base:
    base = base.replace('enum { DXGI_FORMAT_', 'enum { DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_', 1)
mutants = {
    'tier-alias-rejected': ('if (!com->aux) return NULL;\n   /* QueryInterface', 'if (!com->aux || !com->aux_destroy) return NULL;\n   /* QueryInterface'),
    'bridge-cache-omitted': ('npt_d3d11_texture_set_desc(texture, &desc);', '(void)desc;'),
    'upload-lazy-omitted': ('if (!npt_d3d11_texture_ensure_desc(texture,\n                                     D3D11_RESOURCE_DIMENSION_UNKNOWN, false))\n      return false;', '/* missing import metadata */'),
    'map-lazy-omitted': ('if (!npt_d3d11_texture_ensure_desc(t, D3D11_RESOURCE_DIMENSION_UNKNOWN, false))\n      return NPT_E_FAIL;', '/* missing import metadata */'),
    'base-claims-extended': ('texture_publish_desc(t, &d, extended);', 'texture_publish_desc(t, &d, true);'),
    'lazy-unserialized': ('mtx_lock(&aux->desc_mutex);', '/* missing descriptor lock */'),
    'zero-reply-published': ('!d.mip_levels || d.mip_levels > 32 || !d.array_size ||\n       !d.sample_count || d.format == DXGI_FORMAT_UNKNOWN', 'false'),
}
# For the zero-reply mutant remove the entire dimensional guard as well.
mutants['zero-reply-published'] = ('if (!npt_ring_is_healthy(ring) || !d.width || !d.height || !d.depth ||\n       !d.mip_levels || d.mip_levels > 32 || !d.array_size ||\n       !d.sample_count || d.format == DXGI_FORMAT_UNKNOWN)', 'if (!npt_ring_is_healthy(ring))')
# Remove each newly restored fallback separately: every getter needs its own
# negative control, since a combined mutant could fail on only the first one.
for tag, getter, rpc, desc_type, dimension, expected in getters:
    branch = ('   if (!((struct npt_com_base *)self)->aux) {\n'
              '      memset(pDesc, 0, sizeof(*pDesc));\n'
              f'      {rpc}(self, pDesc);\n'
              '      return;\n'
              '   }')
    mutants['aux-oom-fallback-omitted-' + tag] = (branch, '/* omitted optional auxiliary fallback */')
current_getter = m.function(base, 'tex2d_GetDesc_override')
retry_getter = current_getter.replace('   memset(pDesc, 0, sizeof(*pDesc));\n}',
    '   memset(pDesc, 0, sizeof(*pDesc));\n   npt_id3d11texture2d_default_GetDesc(self, pDesc);\n}')
assert retry_getter != current_getter
mutants['failed-hydration-retries-rpc'] = (current_getter, retry_getter)

with tempfile.TemporaryDirectory(prefix='triton-import-metadata-') as directory:
    directory = Path(directory)
    variants = ('current',) if os.environ.get('TRITON_METADATA_TSAN') else ('current', *mutants)
    sanitizer = 'thread' if os.environ.get('TRITON_METADATA_TSAN') else 'address,undefined'
    for name in variants:
        code = base
        if name != 'current':
            old, new = mutants[name]
            assert old in code, name
            code = code.replace(old, new)
            if name == 'lazy-unserialized':
                code = code.replace('mtx_unlock(&aux->desc_mutex);', '/* missing descriptor unlock */')
        source = directory / 'test.c'
        exe = directory / 'test'
        source.write_text(code)
        command = ['clang', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                   '-Wno-unused-function', '-Wno-unused-parameter', '-Wno-missing-field-initializers',
                   '-pthread', '-fsanitize=' + sanitizer, str(source), '-o', str(exe)]
        result = subprocess.run(command, capture_output=True, text=True)
        assert result.returncode == 0, result.stdout + result.stderr
        result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=20,
                                env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})
        if name == 'current':
            assert result.returncode == 0, result.stdout + result.stderr
            print(result.stdout.strip())
        else:
            assert result.returncode != 0 and 'FAIL update transport' in result.stderr, result.stdout + result.stderr
            print('D3D10 imported metadata ' + name + ' negative control rejected')
