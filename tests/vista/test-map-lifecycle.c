/* SPDX-License-Identifier: MIT
 * Both transport endpoints execute production code. The backend is a guarded
 * CPU allocation with explicit mapped ownership and injected Map/realloc OOM. */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
typedef unsigned UINT;typedef int32_t HRESULT;typedef unsigned DXGI_FORMAT,D3D11_MAP,D3D11_TEXTURE_LAYOUT;
#define NPT_S_OK 0
#define NPT_E_FAIL (-1)
#define NPT_E_NOTIMPL (-2)
#define NPT_E_OUTOFMEMORY (-3)
#define NPT_FAILED(x) ((x)<0)
#define NPT_PERF(x) 0
#define UNUSED
#define NPT_D3D_MAP_SLOT_MAX 8
#define NPT_D3D_MAP_SLOT_INIT 1
#define CHECK(c) do {++checks;if(!(c)){fprintf(stderr,"FAIL lifecycle line %u: %s\n",__LINE__,#c);exit(1);}}while(0)
static unsigned checks;
static void npt_log(const char *format,...) {(void)format;}
enum {D3D11_USAGE_DEFAULT=0,D3D11_USAGE_DYNAMIC=2,D3D11_USAGE_STAGING=3,D3D11_CPU_ACCESS_READ=1,D3D11_CPU_ACCESS_WRITE=2};
enum {D3D11_MAP_READ=1,D3D11_MAP_WRITE,D3D11_MAP_READ_WRITE,D3D11_MAP_WRITE_DISCARD,D3D11_MAP_WRITE_NO_OVERWRITE};
// MAP_BITS
// FORMAT_CONSTANTS
// HOST_FORMATS
struct npt_ring {unsigned id;};
struct npt_device {struct npt_ring *ring;};
struct npt_com_base {struct {uint64_t id;struct npt_device *device;} base;void *aux;};
struct npt_d3d11_texture {struct npt_com_base com;};
struct npt_d3d_map_slot {bool in_flight;struct npt_ring *pending_ring;uint32_t pending_seqno;};
struct npt_d3d_map_ring {struct npt_com_base *com;uint32_t aligned_slot_size,active_count,current_slot,last_map_access_flags;bool is_mapped;struct npt_d3d_map_slot slots[8];unsigned char *data;};
// RESOURCE_STRUCTURES
typedef struct {void *pData;UINT RowPitch,DepthPitch;} D3D11_MAPPED_SUBRESOURCE;
static struct npt_ring ring={1},second_ring={2};
static struct npt_device device={&ring};
static unsigned host_mapped,sync_calls,abort_calls,backend_unmaps,waits,retired;
static bool fail_alloc,fail_realloc,fail_backend,metadata_only,fail_wait,fail_abort;
static unsigned qi_refs;
static uint32_t allocation_request;
static struct npt_ring *npt_com_self_ring(void *self){return &ring;}
static bool alloc_slot_locked(struct npt_d3d_map_ring *r,uint32_t slot){
 if(r->active_count>slot)return true;
 allocation_request=r->aligned_slot_size;if(fail_alloc)return false;
 if(!metadata_only){r->data=calloc(8,r->aligned_slot_size);CHECK(r->data!=NULL);}
 r->active_count=slot+1;return true;
}
static uint32_t npt_ring_wait_seqno(struct npt_ring *r,uint32_t seq){CHECK((r==&ring&&seq==31)||(r==&second_ring&&seq==32));++waits;return fail_wait?UINT32_MAX:seq;}
static void npt_d3d_map_ring_fini(struct npt_d3d_map_ring *r){CHECK(!host_mapped);++retired;free(r->data);r->data=NULL;r->active_count=0;r->aligned_slot_size=0;r->current_slot=0;memset(r->slots,0,sizeof(r->slots));}
// PRODUCTION_RING
static uint32_t npt_d3d_map_ring_slot_res_id(const struct npt_d3d_map_ring *r,uint32_t slot){return 10+slot;}
static uint32_t npt_d3d_map_ring_slot_offset(const struct npt_d3d_map_ring *r,uint32_t slot){return r->aligned_slot_size*slot;}
static void *npt_d3d_map_ring_slot_ptr(const struct npt_d3d_map_ring *r,uint32_t slot){return r->data+r->aligned_slot_size*slot;}
static uint32_t npt_d3d_map_ring_rotate_slot(struct npt_d3d_map_ring *r){return r->current_slot=0;}
static void npt_d3d_map_ring_mark_slot_submitted(struct npt_d3d_map_ring *r,uint32_t slot,uint32_t seq,struct npt_ring *ring){}
// PRODUCTION_RESOURCE
// HOST_MAP_ENTRY
struct npt_context {struct {struct npt_sync_map_entry *entries;uint32_t count,cap;} sync_maps;};
struct npt_resource {int fd_type;union {void *data;} u;size_t size;};
static struct npt_context host;
static struct npt_resource memory;
static struct npt_d3d11_texture_aux *active;
static unsigned char *host_data;static size_t host_bytes;
static uint32_t host_row,host_rows,host_depth_pitch,host_depth,drift_pitch;
static unsigned char resource_object,context_object;
#define VIRGL_RESOURCE_FD_SHM 1
#define NPT_OBJECT_TYPE_IUNKNOWN 1
#define NPT_OBJECT_TYPE_ID3D11DEVICECONTEXT 2
static void *npt_context_lookup_object(struct npt_context *ctx,void *ignored,uint64_t id,unsigned kind){return id==1?&context_object:id==2?&resource_object:NULL;}
static struct npt_resource *npt_context_get_resource(struct npt_context *ctx,uint32_t id){return id==10?&memory:NULL;}
static HRESULT PFN_ID3D11DeviceContext_Map_stub(void *c,void *resource,uint32_t sub,D3D11_MAP type,uint32_t flags,D3D11_MAPPED_SUBRESOURCE *out){
 ++sync_calls;CHECK(!host_mapped);if(fail_backend)return NPT_E_OUTOFMEMORY;++host_mapped;out->pData=host_data;out->RowPitch=host_row;out->DepthPitch=host_depth_pitch;return NPT_S_OK;
}
static void PFN_ID3D11DeviceContext_Unmap_stub(void *c,void *resource,uint32_t sub){CHECK(host_mapped==1);--host_mapped;++backend_unmaps;}
typedef HRESULT (*PFN_ID3D11DeviceContext_Map)(void*,void*,uint32_t,D3D11_MAP,uint32_t,D3D11_MAPPED_SUBRESOURCE*);
typedef void (*PFN_ID3D11DeviceContext_Unmap)(void*,void*,uint32_t);
typedef unsigned D3D11_RESOURCE_DIMENSION;
enum {D3D11_RESOURCE_DIMENSION_BUFFER=1,D3D11_RESOURCE_DIMENSION_TEXTURE1D,D3D11_RESOURCE_DIMENSION_TEXTURE2D,D3D11_RESOURCE_DIMENSION_TEXTURE3D};
typedef struct {UINT ByteWidth;} D3D11_BUFFER_DESC;
typedef struct {UINT Width,MipLevels,ArraySize;DXGI_FORMAT Format;} D3D11_TEXTURE1D_DESC;
typedef struct {UINT Width,Height,MipLevels,ArraySize;DXGI_FORMAT Format;} D3D11_TEXTURE2D_DESC;
typedef struct {UINT Width,Height,Depth,MipLevels;DXGI_FORMAT Format;} D3D11_TEXTURE3D_DESC;
static UINT backend_type,backend_width,backend_height,backend_depth,backend_mips,backend_arrays,backend_format,backend_buffer_bytes;
static int NPT_IID_ID3D11Resource;
static HRESULT npt_com_query_interface(void *o,void *iid,void **out){CHECK(o==&resource_object);*out=o;++qi_refs;return NPT_S_OK;}
static void npt_com_release(void *o){CHECK(o==&resource_object&&qi_refs);--qi_refs;}
typedef void (*PFN_ID3D11Resource_GetType)(void*,D3D11_RESOURCE_DIMENSION*);
typedef void (*PFN_ID3D11Buffer_GetDesc)(void*,D3D11_BUFFER_DESC*);
typedef void (*PFN_ID3D11Texture1D_GetDesc)(void*,D3D11_TEXTURE1D_DESC*);
typedef void (*PFN_ID3D11Texture2D_GetDesc)(void*,D3D11_TEXTURE2D_DESC*);
typedef void (*PFN_ID3D11Texture3D_GetDesc)(void*,D3D11_TEXTURE3D_DESC*);
static void PFN_ID3D11Resource_GetType_stub(void *o,D3D11_RESOURCE_DIMENSION *t){*t=backend_type;}
static void PFN_ID3D11Buffer_GetDesc_stub(void *o,D3D11_BUFFER_DESC *d){d->ByteWidth=backend_buffer_bytes;}
static void PFN_ID3D11Texture1D_GetDesc_stub(void *o,D3D11_TEXTURE1D_DESC *d){*d=(D3D11_TEXTURE1D_DESC){backend_width,backend_mips,backend_arrays,backend_format};}
static void PFN_ID3D11Texture2D_GetDesc_stub(void *o,D3D11_TEXTURE2D_DESC *d){*d=(D3D11_TEXTURE2D_DESC){backend_width,backend_height,backend_mips,backend_arrays,backend_format};}
static void PFN_ID3D11Texture3D_GetDesc_stub(void *o,D3D11_TEXTURE3D_DESC *d){*d=(D3D11_TEXTURE3D_DESC){backend_width,backend_height,backend_depth,backend_mips,backend_format};}
#define NPT_COM_VTBL_FUNC(type,vtbl,slot) type##_stub
static void *injected_realloc(void *p,size_t n){return fail_realloc?NULL:realloc(p,n);}
#define realloc injected_realloc
// PRODUCTION_HOST
#undef realloc
static HRESULT npt_dispatch_resource_map(struct npt_ring *r,uint64_t ctx,uint64_t resource,UINT sub,UINT access,UINT flags,UINT shmem,UINT size,UINT rows,UINT depth,UINT offset,UINT *rp,UINT *dp){
 memory.fd_type=VIRGL_RESOURCE_FD_SHM;memory.u.data=active->map_ring.data;memory.size=(size_t)active->map_ring.aligned_slot_size*8;
 CHECK(rows==host_rows&&depth==host_depth);UINT mapped_size=0;
 HRESULT hr=npt_resource_map(&host,ctx,resource,sub,access,flags,shmem,0,0,size,rows,depth,offset,rp,dp,&mapped_size);
 if(!NPT_FAILED(hr)&&drift_pitch){*rp=drift_pitch;drift_pitch*=2;}
 return hr;
}
// ABORT_WIRE
#define NPT_CMD_FLAG_REPLY 1u
#define NPT_TRANSPORT_SUBGROUP_RESOURCE 3u
#define NPT_TRANSPORT_RESOURCE_UNMAP 2u
#define NPT_TRANSPORT_CMD_TYPE(s,m) (((s)<<16)|(m))
struct npt_cs_encoder {uint8_t *cur,*end;};
struct npt_cs_decoder {const uint8_t *cur,*end;};
struct npt_ring_submit_command {struct npt_cs_encoder enc;struct npt_cs_decoder dec;struct npt_cmd_unmap_resource *cmd;struct npt_cmd_unmap_resource_reply reply;};
static bool fail_encode,short_reply;
static struct npt_cs_encoder *npt_ring_submit_command_init(struct npt_ring *r,struct npt_ring_submit_command *s,void *cmd,size_t size,size_t reply_size){
 CHECK(size==sizeof(struct npt_cmd_unmap_resource)&&reply_size==sizeof(s->reply));if(fail_encode)return NULL;s->cmd=cmd;s->enc.cur=cmd;s->enc.end=(uint8_t*)cmd+size;return &s->enc;
}
static void npt_ring_submit_command(struct npt_ring *r,struct npt_ring_submit_command *s){
 struct npt_cmd_unmap_resource *c=s->cmd;CHECK(s->enc.cur==s->enc.end);CHECK(c->header.cmd_flags==NPT_CMD_FLAG_REPLY&&c->header.cmd_size==sizeof(*c));CHECK(c->header.cmd_type==NPT_TRANSPORT_CMD_TYPE(3,2));CHECK(c->access_flags==NPT_MAP_ACCESS_ABORT&&!c->byte_size&&!c->shmem_res_id&&!c->shmem_offset);++abort_calls;
 s->reply.header.cmd_return=fail_abort?NPT_E_FAIL:npt_resource_unmap(&host,c->context_id,c->resource_id,c->subresource,c->shmem_res_id,c->shmem_offset,c->byte_size,c->access_flags,0,0);s->dec.cur=(const uint8_t*)&s->reply;s->dec.end=s->dec.cur+sizeof(s->reply)-(short_reply?1:0);
}
static struct npt_cs_decoder *npt_ring_get_command_reply(struct npt_ring *r,struct npt_ring_submit_command *s){return &s->dec;}
static void npt_ring_free_command_reply(struct npt_ring *r,struct npt_ring_submit_command *s){}
// PRODUCTION_ABORT
static bool npt_dispatch_resource_unmap(struct npt_ring *r,uint64_t ctx,uint64_t resource,UINT sub,UINT shmem,UINT bytes,UINT offset,UINT access){
 if(access==NPT_MAP_ACCESS_ABORT)++abort_calls;
 return npt_resource_unmap(&host,ctx,resource,sub,shmem,offset,bytes,access,0,0)==NPT_S_OK;
}
static void npt_dispatch_resource_unmap_seqno(struct npt_ring *r,uint64_t ctx,uint64_t resource,UINT sub,UINT shmem,UINT bytes,UINT offset,UINT access,UINT *seq){CHECK(npt_resource_unmap(&host,ctx,resource,sub,shmem,offset,bytes,access,0,0)==NPT_S_OK);*seq=1;}
// PRODUCTION_CONTEXT
static struct npt_com_base context={{1,&device},NULL};
static void setup(struct npt_d3d11_texture *t,struct npt_d3d11_texture_aux *aux,unsigned depth){
 memset(aux,0,sizeof(*aux));t->com.base.id=2;t->com.base.device=&device;t->com.aux=aux;aux->map_ring.com=&t->com;active=aux;
 struct npt_d3d11_texture_desc d={0};d.width=4;d.height=2;d.depth=depth;d.format=DXGI_FORMAT_R8G8B8A8_UNORM;d.usage=D3D11_USAGE_STAGING;d.cpu_access_flags=D3D11_CPU_ACCESS_READ|D3D11_CPU_ACCESS_WRITE;
 backend_type=depth>1?D3D11_RESOURCE_DIMENSION_TEXTURE3D:D3D11_RESOURCE_DIMENSION_TEXTURE2D;backend_width=4;backend_height=2;backend_depth=depth;backend_mips=backend_arrays=1;backend_format=d.format;backend_buffer_bytes=32;
 npt_d3d11_texture_set_desc(t,&d);host_row=16;host_rows=2;host_depth=depth;host_depth_pitch=32;host_bytes=32*depth;host_data=malloc(host_bytes);CHECK(host_data);memset(host_data,0x47,host_bytes);
 CHECK(!host.sync_maps.count&&!host_mapped);sync_calls=abort_calls=backend_unmaps=waits=retired=0;fail_alloc=fail_realloc=fail_backend=fail_wait=fail_abort=fail_encode=short_reply=false;metadata_only=false;drift_pitch=0;
}
static void resize_backend(unsigned row,unsigned dp){free(host_data);host_row=row;host_depth_pitch=dp;host_bytes=(size_t)dp*(host_depth-1)+row*host_rows;host_data=malloc(host_bytes);CHECK(host_data);for(size_t i=0;i<host_bytes;++i)host_data[i]=(unsigned char)(i*17+0x47);}
static void teardown(struct npt_d3d11_texture_aux *aux){CHECK(!host_mapped&&!host.sync_maps.count&&!qi_refs);free(aux->map_ring.data);free(host_data);free(host.sync_maps.entries);memset(&host,0,sizeof(host));}
static void lifecycle(unsigned map){
 struct npt_d3d11_texture t;struct npt_d3d11_texture_aux aux;D3D11_MAPPED_SUBRESOURCE m;setup(&t,&aux,1);
 /* Backend Map succeeds before bookkeeping allocation fails. */
 fail_realloc=true;CHECK(ctx_Map_texture(&context,&t,0,map,0,&m)==NPT_E_OUTOFMEMORY);CHECK(sync_calls==1&&backend_unmaps==1&&!host_mapped&&!host.sync_maps.count);CHECK(!aux.map_ring.is_mapped&&!m.pData);
 for(size_t i=0;i<host_bytes;++i)CHECK(host_data[i]==0x47);
 fail_realloc=false;CHECK(ctx_Map_texture(&context,&t,0,map,0,&m)==NPT_S_OK);CHECK(host_mapped==1&&host.sync_maps.count==1);
 if(map!=D3D11_MAP_READ)memset(m.pData,0x99,host_bytes);ctx_Unmap_texture(&context,&t);for(size_t i=0;i<host_bytes;++i)CHECK(host_data[i]==(map==D3D11_MAP_READ?0x47:0x99));
 teardown(&aux);
 /* Guest refuses an unknown larger row pitch; abort must not upload stale SHM. */
 setup(&t,&aux,1);resize_backend(512,1024);CHECK(npt_d3d11_texture_ensure_map_shmem(&t));memset(aux.map_ring.data,0xab,(size_t)aux.map_ring.aligned_slot_size*8);fail_alloc=true;
 unsigned char snapshot[1024];memcpy(snapshot,host_data,sizeof(snapshot));CHECK(ctx_Map_texture(&context,&t,0,map,0,&m)==NPT_E_OUTOFMEMORY);CHECK(abort_calls==1&&!host_mapped&&!host.sync_maps.count&&!m.pData);CHECK(!memcmp(snapshot,host_data,sizeof(snapshot)));
 fail_alloc=false;CHECK(ctx_Map_texture(&context,&t,0,map,0,&m)==NPT_S_OK);if(map!=D3D11_MAP_READ)memset(m.pData,0x81,host_bytes);ctx_Unmap_texture(&context,&t);if(map==D3D11_MAP_READ)CHECK(!memcmp(snapshot,host_data,sizeof(snapshot)));else for(size_t i=0;i<host_bytes;++i)CHECK(host_data[i]==0x81);teardown(&aux);
 /* Oversized row retry succeeds; padded slices retain their full strided data. */
 setup(&t,&aux,3);resize_backend(256,1024);unsigned char *expected=malloc(host_bytes);CHECK(expected);memcpy(expected,host_data,host_bytes);
 CHECK(ctx_Map_texture(&context,&t,0,map,0,&m)==NPT_S_OK);CHECK(abort_calls==1&&sync_calls==2&&retired==1);CHECK(m.RowPitch==256&&m.DepthPitch==1024);CHECK(aux.last_map_byte_size==2560);
 if(map!=D3D11_MAP_WRITE)CHECK(!memcmp(m.pData,expected,host_bytes));if(map!=D3D11_MAP_READ)for(size_t i=0;i<host_bytes;++i)((unsigned char*)m.pData)[i]=(unsigned char)(i*3);
 ctx_Unmap_texture(&context,&t);if(map==D3D11_MAP_READ)CHECK(!memcmp(host_data,expected,host_bytes));else for(size_t i=0;i<host_bytes;++i)CHECK(host_data[i]==(unsigned char)(i*3));free(expected);teardown(&aux);
}
static void host_bounds(void){
 struct npt_d3d11_texture t;struct npt_d3d11_texture_aux aux;D3D11_MAPPED_SUBRESOURCE mapped;UINT rp,dp,size;
 setup(&t,&aux,1);CHECK(npt_d3d11_texture_ensure_map_shmem(&t));memory=(struct npt_resource){VIRGL_RESOURCE_FD_SHM,{aux.map_ring.data},aux.map_ring.aligned_slot_size*8u};memset(memory.u.data,0xb5,memory.size);
 /* Host must reject wrong dimensions/subresources before Map or a copy. */
 CHECK(npt_resource_map(&host,1,2,0,NPT_MAP_ACCESS_READ,0,10,0,0,512,200,1,0,&rp,&dp,&size)==NPT_E_FAIL);CHECK(!sync_calls&&!host_mapped);
 CHECK(npt_resource_map(&host,1,2,1,NPT_MAP_ACCESS_READ,0,10,0,0,512,2,1,0,&rp,&dp,&size)==NPT_E_FAIL);CHECK(!sync_calls&&!qi_refs);
 CHECK(npt_resource_map(&host,1,2,0,NPT_MAP_ACCESS_ABORT,0,10,0,0,512,2,1,0,&rp,&dp,&size)==NPT_E_FAIL);
 /* A larger shared pool cannot authorize overrunning backend storage. */
 CHECK(npt_resource_unmap(&host,1,2,0,10,0,33,NPT_MAP_ACCESS_WRITE|NPT_MAP_ACCESS_DISCARD,0,0)==NPT_E_FAIL);CHECK(!host_mapped&&backend_unmaps==1);for(size_t i=0;i<host_bytes;++i)CHECK(host_data[i]==0x47);
 backend_type=D3D11_RESOURCE_DIMENSION_BUFFER;
 CHECK(npt_resource_map(&host,1,2,0,NPT_MAP_ACCESS_READ,0,10,0,0,33,0,0,0,&rp,&dp,&size)==NPT_E_FAIL);
 CHECK(npt_resource_unmap(&host,1,2,0,10,0,33,NPT_MAP_ACCESS_WRITE,0,0)==NPT_E_FAIL);CHECK(!host_mapped);for(size_t i=0;i<host_bytes;++i)CHECK(host_data[i]==0x47);
 CHECK(npt_resource_unmap(&host,1,2,0,10,0,32,NPT_MAP_ACCESS_WRITE,0,0)==NPT_S_OK);for(size_t i=0;i<host_bytes;++i)CHECK(host_data[i]==0xb5);
 CHECK(npt_resource_unmap(&host,1,2,0,10,1,UINT64_MAX,NPT_MAP_ACCESS_WRITE,0,0)==NPT_E_FAIL);
 backend_type=D3D11_RESOURCE_DIMENSION_TEXTURE2D;resize_backend(512,1024);memset(memory.u.data,0xa3,memory.size);
 CHECK(npt_resource_map(&host,1,2,0,NPT_MAP_ACCESS_READ,0,10,0,0,512,2,1,0,&rp,&dp,&size)==NPT_S_OK);CHECK(size==512);for(size_t i=512;i<memory.size;++i)CHECK(((unsigned char*)memory.u.data)[i]==0xa3);
 CHECK(npt_resource_unmap(&host,1,2,0,0,0,0,NPT_MAP_ACCESS_ABORT|NPT_MAP_ACCESS_WRITE,0,0)==NPT_E_FAIL);CHECK(host_mapped==1);
 CHECK(npt_dispatch_resource_abort(&ring,1,2,0)==NPT_S_OK);CHECK(!host_mapped);CHECK(npt_dispatch_resource_abort(&ring,1,2,0)==NPT_E_FAIL);teardown(&aux);
 /* An abort without a confirmed reply must keep the old storage alive. */
 setup(&t,&aux,1);resize_backend(512,1024);CHECK(npt_d3d11_texture_ensure_map_shmem(&t));void *old=aux.map_ring.data;fail_abort=true;
 CHECK(ctx_Map_texture(&context,&t,0,D3D11_MAP_WRITE,0,&mapped)==NPT_E_FAIL);CHECK(host_mapped==1&&aux.map_ring.data==old&&!retired&&!mapped.pData);fail_abort=false;CHECK(npt_dispatch_resource_abort(&ring,1,2,0)==NPT_S_OK);
 fail_encode=true;CHECK(npt_dispatch_resource_abort(&ring,1,2,0)==NPT_E_FAIL);fail_encode=false;
 CHECK(ctx_Map_texture(&context,&t,0,D3D11_MAP_WRITE,0,&mapped)==NPT_S_OK);short_reply=true;CHECK(npt_dispatch_resource_abort(&ring,1,2,0)==NPT_E_FAIL);CHECK(!host_mapped);aux.map_ring.is_mapped=false;teardown(&aux);
 /* Wait failure must retain every staging allocation for outstanding uploads. */
 setup(&t,&aux,1);CHECK(npt_d3d11_texture_ensure_map_shmem(&t));old=aux.map_ring.data;aux.map_ring.slots[0].in_flight=true;aux.map_ring.slots[0].pending_seqno=31;fail_wait=true;
 CHECK(!npt_d3d11_texture_grow_map_shmem(&t,1024));CHECK(aux.map_ring.data==old&&!retired&&aux.map_ring.aligned_slot_size==512);fail_wait=false;CHECK(npt_d3d11_texture_grow_map_shmem(&t,1024));teardown(&aux);
}
int main(void){
 host_bounds();lifecycle(D3D11_MAP_READ);lifecycle(D3D11_MAP_WRITE);lifecycle(D3D11_MAP_READ_WRITE);
 struct npt_d3d11_texture t;struct npt_d3d11_texture_aux aux;D3D11_MAPPED_SUBRESOURCE m;
 setup(&t,&aux,1);fail_backend=true;CHECK(ctx_Map_texture(&context,&t,0,D3D11_MAP_READ,0,&m)==NPT_E_OUTOFMEMORY);CHECK(!host_mapped&&!host.sync_maps.count&&!backend_unmaps);fail_backend=false;
 CHECK(ctx_Map_texture(&context,&t,0,D3D11_MAP_READ,0,&m)==NPT_S_OK);unsigned before=sync_calls;D3D11_MAPPED_SUBRESOURCE duplicate;CHECK(ctx_Map_texture(&context,&t,0,D3D11_MAP_READ,0,&duplicate)==NPT_E_FAIL&&sync_calls==before);ctx_Unmap_texture(&context,&t);teardown(&aux);
 setup(&t,&aux,1);CHECK(npt_d3d11_texture_ensure_map_shmem(&t));aux.map_ring.active_count=2;aux.map_ring.slots[0].in_flight=true;aux.map_ring.slots[0].pending_seqno=31;aux.map_ring.slots[1].in_flight=true;aux.map_ring.slots[1].pending_ring=&second_ring;aux.map_ring.slots[1].pending_seqno=32;
 CHECK(npt_d3d11_texture_grow_map_shmem(&t,1024));CHECK(waits==2&&retired==1&&aux.map_ring.aligned_slot_size==1024);CHECK(npt_d3d11_texture_ensure_map_shmem(&t)&&aux.map_ring.aligned_slot_size==1024);teardown(&aux);
 setup(&t,&aux,1);metadata_only=true;aux.width=aux.height=8192;CHECK(npt_d3d11_texture_ensure_map_shmem(&t));CHECK(allocation_request==256u*1024*1024);aux.map_ring.is_mapped=true;CHECK(!npt_d3d11_texture_grow_map_shmem(&t,512u*1024*1024));aux.map_ring.is_mapped=false;
 aux.format=DXGI_FORMAT_R16G16B16A16_FLOAT;CHECK(npt_d3d11_texture_ensure_map_shmem(&t));CHECK(allocation_request==512u*1024*1024);CHECK(!npt_d3d11_texture_grow_map_shmem(&t,UINT32_MAX));teardown(&aux);
 printf("Map lifecycle %u checks passed\n",checks);return 0;
}
