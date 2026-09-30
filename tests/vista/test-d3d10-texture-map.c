/* SPDX-License-Identifier: MIT
 * The production guest resource and context methods use an independently sized
 * host allocation. ASan catches texel-row copies from block-compressed storage. */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned UINT;typedef int32_t HRESULT;typedef unsigned DXGI_FORMAT,D3D11_MAP,D3D11_TEXTURE_LAYOUT;
#define NPT_S_OK 0
#define NPT_E_FAIL (-1)
#define NPT_E_NOTIMPL (-2)
#define NPT_E_OUTOFMEMORY (-3)
#define NPT_FAILED(x) ((x)<0)
#define NPT_PERF(x) 0
static void npt_log(const char *format,...) {(void)format;}
#define NPT_D3D_MAP_SLOT_MAX 8
#define CHECK(c) do {++checks;if(!(c)){fprintf(stderr,"FAIL map line %u: %s\n",__LINE__,#c);exit(1);}}while(0)
static unsigned checks;
enum {D3D11_USAGE_DEFAULT=0,D3D11_USAGE_DYNAMIC=2,D3D11_USAGE_STAGING=3,D3D11_CPU_ACCESS_READ=1,D3D11_CPU_ACCESS_WRITE=2};
enum {D3D11_MAP_READ=1,D3D11_MAP_WRITE,D3D11_MAP_READ_WRITE,D3D11_MAP_WRITE_DISCARD,D3D11_MAP_WRITE_NO_OVERWRITE};
enum {NPT_MAP_ACCESS_READ=1,NPT_MAP_ACCESS_WRITE=2,NPT_MAP_ACCESS_DISCARD=4,NPT_MAP_ACCESS_NO_OVERWRITE=8,NPT_MAP_ACCESS_ABORT=64};
// FORMAT_CONSTANTS
struct npt_com_base {struct {uint64_t id;} base;void *aux;};
struct npt_d3d11_texture {struct npt_com_base com;};
struct npt_ring {int unused;};
struct npt_d3d_map_ring {uint32_t aligned_slot_size,active_count,current_slot,last_map_access_flags;bool is_mapped;unsigned char *data;};
// RESOURCE_STRUCTURES
typedef struct {void *pData;UINT RowPitch,DepthPitch;} D3D11_MAPPED_SUBRESOURCE;
static struct npt_ring ring;
static struct npt_ring *npt_com_self_ring(void *self){return &ring;}
static bool npt_d3d_map_ring_alloc_shmem(struct npt_d3d_map_ring *r,uint32_t bytes){if(r->data)return r->aligned_slot_size==bytes;r->data=calloc(NPT_D3D_MAP_SLOT_MAX,bytes);r->aligned_slot_size=bytes;r->active_count=NPT_D3D_MAP_SLOT_MAX;return r->data!=NULL;}
static uint32_t npt_d3d_map_ring_slot_res_id(const struct npt_d3d_map_ring *r,uint32_t slot){return 10+slot;}
static uint32_t npt_d3d_map_ring_slot_offset(const struct npt_d3d_map_ring *r,uint32_t slot){return r->aligned_slot_size*slot;}
static void *npt_d3d_map_ring_slot_ptr(const struct npt_d3d_map_ring *r,uint32_t slot){return r->data+r->aligned_slot_size*slot;}
static uint32_t npt_d3d_map_ring_rotate_slot(struct npt_d3d_map_ring *r){return r->current_slot=(r->current_slot+1)%NPT_D3D_MAP_SLOT_MAX;}
static void npt_d3d_map_ring_mark_slot_submitted(struct npt_d3d_map_ring *r,uint32_t slot,uint32_t seq,struct npt_ring *ring){}
// PRODUCTION_RESOURCE
static struct npt_d3d11_texture_aux *active;
static unsigned sync_maps,unmaps,async_unmaps,host_row,host_rows,host_depth;
static uint32_t last_wire_flags,last_wire_sub;
static unsigned char *host_data;
static size_t host_bytes;
static HRESULT npt_dispatch_resource_map(struct npt_ring *ring,uint64_t ctx,uint64_t resource,UINT sub,UINT access,UINT flags,UINT shmem,UINT size,UINT rows,UINT depth,UINT offset,UINT *rp,UINT *dp){
 ++sync_maps;last_wire_flags=flags;last_wire_sub=sub;CHECK(rows==host_rows&&depth==host_depth);CHECK(size>=host_bytes);
 if(access&NPT_MAP_ACCESS_READ)memcpy(active->map_ring.data+offset,host_data,(size_t)host_row*rows*depth);
 *rp=host_row;*dp=host_row*host_rows;return NPT_S_OK;
}
static HRESULT npt_dispatch_resource_abort(struct npt_ring *r,uint64_t ctx,uint64_t resource,UINT sub){return NPT_S_OK;}
static bool npt_dispatch_resource_unmap(struct npt_ring *ring,uint64_t ctx,uint64_t resource,UINT sub,UINT shmem,UINT bytes,UINT offset,UINT access){++unmaps;CHECK(bytes==host_bytes);CHECK(sub==last_wire_sub);return true;}
static void npt_dispatch_resource_unmap_seqno(struct npt_ring *ring,uint64_t ctx,uint64_t resource,UINT sub,UINT shmem,UINT bytes,UINT offset,UINT access,UINT *seq){++async_unmaps;CHECK(bytes==host_bytes);CHECK(access&NPT_MAP_ACCESS_WRITE);memcpy(host_data,active->map_ring.data+offset,bytes);*seq=async_unmaps;}
// PRODUCTION_CONTEXT
static void map_case(DXGI_FORMAT format,unsigned width,unsigned height,unsigned block_bytes){
 struct npt_d3d11_texture_aux aux={0};struct npt_d3d11_texture t={{{2},&aux}};struct npt_com_base context={{1},NULL};active=&aux;
 struct npt_d3d11_texture_desc desc={0};desc.width=width;desc.height=height;desc.depth=1;desc.mip_levels=6;desc.array_size=2;desc.format=format;desc.usage=D3D11_USAGE_STAGING;desc.cpu_access_flags=D3D11_CPU_ACCESS_READ|D3D11_CPU_ACCESS_WRITE;
 npt_d3d11_texture_set_desc(&t,&desc);CHECK(npt_d3d11_texture_has_desc(&t));CHECK(npt_d3d11_texture_is_mappable(&t));
 for(unsigned sub=0;sub<12;++sub){unsigned mip=sub%6,w=width>>mip,h=height>>mip;if(!w)w=1;if(!h)h=1;
  host_row=((w+3)/4)*block_bytes;host_rows=(h+3)/4;host_depth=1;host_bytes=host_row*host_rows;host_data=malloc(host_bytes);CHECK(host_data);
  for(unsigned i=0;i<host_bytes;++i)host_data[i]=(unsigned char)(i+sub+1);
  D3D11_MAPPED_SUBRESOURCE mapped={0};CHECK(ctx_Map_texture(&context,&t,sub,D3D11_MAP_READ,0x100000,&mapped)==NPT_S_OK);CHECK(mapped.RowPitch==host_row&&mapped.DepthPitch==host_bytes);CHECK(last_wire_flags==0x100000&&last_wire_sub==sub);CHECK(!memcmp(mapped.pData,host_data,host_bytes));ctx_Unmap_texture(&context,&t);CHECK(!aux.map_ring.is_mapped);
  /* Same subresource can reuse the known pitch; another mip must synchronize. */
  unsigned before=sync_maps;CHECK(ctx_Map_texture(&context,&t,sub,D3D11_MAP_WRITE_DISCARD,0,&mapped)==NPT_S_OK);CHECK(sync_maps==before);memset(mapped.pData,0xa5,host_bytes);ctx_Unmap_texture(&context,&t);for(unsigned i=0;i<host_bytes;++i)CHECK(host_data[i]==0xa5);
  free(host_data);
 }
 /* Explicitly map a different mip through the WRITE_DISCARD entry. */
 host_row=((width+3)/4)*block_bytes;host_rows=(height+3)/4;host_depth=1;host_bytes=host_row*host_rows;host_data=calloc(1,host_bytes);D3D11_MAPPED_SUBRESOURCE mapped={0};unsigned before=sync_maps;CHECK(ctx_Map_texture(&context,&t,0,D3D11_MAP_WRITE_DISCARD,0,&mapped)==NPT_S_OK);CHECK(sync_maps==before+1);CHECK(mapped.RowPitch==host_row);ctx_Unmap_texture(&context,&t);free(host_data);free(aux.map_ring.data);
}
int main(void){
 const DXGI_FORMAT formats[]={DXGI_FORMAT_BC1_TYPELESS,DXGI_FORMAT_BC1_UNORM,DXGI_FORMAT_BC1_UNORM_SRGB,DXGI_FORMAT_BC2_TYPELESS,DXGI_FORMAT_BC2_UNORM,DXGI_FORMAT_BC2_UNORM_SRGB,DXGI_FORMAT_BC3_TYPELESS,DXGI_FORMAT_BC3_UNORM,DXGI_FORMAT_BC3_UNORM_SRGB,DXGI_FORMAT_BC4_TYPELESS,DXGI_FORMAT_BC4_UNORM,DXGI_FORMAT_BC4_SNORM,DXGI_FORMAT_BC5_TYPELESS,DXGI_FORMAT_BC5_UNORM,DXGI_FORMAT_BC5_SNORM,DXGI_FORMAT_BC6H_TYPELESS,DXGI_FORMAT_BC6H_UF16,DXGI_FORMAT_BC6H_SF16,DXGI_FORMAT_BC7_TYPELESS,DXGI_FORMAT_BC7_UNORM,DXGI_FORMAT_BC7_UNORM_SRGB};
 for(unsigned i=0;i<sizeof(formats)/sizeof(formats[0]);++i){unsigned bytes=i<3||(i>=9&&i<12)?8:16;map_case(formats[i],16,16,bytes);map_case(formats[i],7,5,bytes);map_case(formats[i],1,1,bytes);}
 struct npt_d3d11_texture_aux aux={0};struct npt_d3d11_texture t={{{2},&aux}};CHECK(!npt_d3d11_texture_has_desc(&t));CHECK(!npt_d3d11_texture_is_mappable(&t));
 aux.width=aux.height=aux.depth=1;aux.format=DXGI_FORMAT_R8G8B8A8_UNORM;aux.bytes_per_pixel=4;atomic_store(&aux.desc_state,2);aux.usage=D3D11_USAGE_DYNAMIC;CHECK(!npt_d3d11_texture_is_mappable(&t));aux.cpu_access_flags=D3D11_CPU_ACCESS_WRITE;CHECK(npt_d3d11_texture_is_mappable(&t));aux.usage=D3D11_USAGE_DEFAULT;CHECK(!npt_d3d11_texture_is_mappable(&t));aux.usage=D3D11_USAGE_STAGING;aux.format=DXGI_FORMAT_UNKNOWN;CHECK(!npt_d3d11_texture_is_mappable(&t));
 aux.format=DXGI_FORMAT_R32G32B32A32_FLOAT;aux.width=UINT32_MAX;CHECK(!npt_d3d11_texture_ensure_map_shmem(&t));CHECK(npt_dxgi_format_row_bytes(aux.format,aux.width)==0);CHECK(npt_dxgi_format_block_rows(DXGI_FORMAT_BC3_UNORM,UINT32_MAX)==1073741824u);CHECK(npt_dxgi_format_row_bytes(DXGI_FORMAT_BC3_UNORM,UINT32_MAX)==0);
 aux.width=1;aux.height=aux.depth=UINT32_MAX;CHECK(!npt_d3d11_texture_ensure_map_shmem(&t));CHECK(npt_d3d11_texture_get_subresource_byte_size(&t,0,UINT32_MAX)==0);CHECK(mip_dim(UINT32_MAX,32)==1);
 printf("D3D10 texture map %u checks passed\n",checks);return 0;
}
