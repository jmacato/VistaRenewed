/* SPDX-License-Identifier: MIT
 * Actual guest upload layout, context override, dispatcher and ring guard.
 * The receiving CPU oracle reconstructs texel/block rows independently. */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
typedef unsigned mtx_t;
#define D3D11_RESOURCE_DIMENSION_UNKNOWN 0
#include <sanitizer/asan_interface.h>
typedef uint32_t UINT,DXGI_FORMAT,D3D11_TEXTURE_LAYOUT;typedef int32_t HRESULT;
typedef struct {UINT left,top,front,right,bottom,back;} D3D11_BOX;
#define NPT_STDMETHODCALLTYPE
#define npt_log(...) ((void)0)
#define NPT_TRANSPORT_SUBGROUP_RESOURCE 3
#define NPT_TRANSPORT_RESOURCE_UPDATE 9
#define NPT_TRANSPORT_CMD_TYPE(a,b) (((a)<<16)|(b))
#define D3D11_BIND_CONSTANT_BUFFER (1u << 2)
// FORMAT_CONSTANTS
struct npt_d3d_map_ring {int unused;};
struct npt_com_base {struct {uint64_t id;} base;void *aux;unsigned kind,width;};
typedef struct npt_com_base ID3D11Resource;
// RESOURCE_STRUCTURES
// WIRE_STRUCTURES
struct npt_ring {atomic_bool failed;unsigned direct_size,mutex;};
struct npt_device {struct npt_ring *ring,*method;};
struct npt_d3d11_texture;struct npt_d3d11_buffer;
static struct npt_ring primary,method;static struct npt_device device={&primary,&method};
static struct npt_com_base object;static struct npt_d3d11_texture_aux aux;
static unsigned checks,submits,fail_submit,live,allocations;static bool fail_alloc;
static UINT target_sub,rb,rows,depth,bh;static D3D11_BOX target;static bool is_buffer;
static uint8_t *result,*coverage;static size_t result_bytes;static unsigned predicate;
#define CHECK(x) do {++checks;if(!(x)){fprintf(stderr,"FAIL update transport line %u: %s\n",__LINE__,#x);exit(1);}}while(0)
static struct npt_device *npt_com_self_device(void*self){return &device;}
static struct npt_ring *npt_com_self_ring(void*self){return &method;}
static uint64_t npt_com_self_id(void*self){return 9;}
static struct npt_ring *npt_device_method_ring(struct npt_device*d){return d->method;}
static struct npt_d3d11_buffer *npt_d3d11_buffer_cast(void*r){return ((struct npt_com_base*)r)->kind==1?r:NULL;}
static struct npt_d3d11_texture *npt_d3d11_texture_cast(void*r){return ((struct npt_com_base*)r)->kind==2?r:NULL;}
/* Source geometry tests use known-create metadata; imported metadata has a
 * separate source-derived integration/concurrency regression. */
static bool npt_d3d11_texture_ensure_desc(struct npt_d3d11_texture*t,unsigned dimension,bool extended){return true;}
static uint32_t npt_d3d11_buffer_get_byte_width(struct npt_d3d11_buffer*b){return ((struct npt_com_base*)b)->width;}
static uint32_t npt_d3d11_buffer_get_bind_flags(struct npt_d3d11_buffer*b){(void)b;return 0;}
static bool npt_ring_is_healthy(struct npt_ring*r){return r&&!atomic_load(&r->failed);}
static HRESULT npt_call_ID3D11Device_GetDeviceRemovedReason(struct npt_ring*r,uint64_t id){return 0;}
static int mtx_lock(unsigned*m){return 0;}static int mtx_unlock(unsigned*m){return 0;}
static bool receive(struct npt_ring*r,const void*header,uint32_t hs,const void*data,uint32_t bytes,uint32_t padded){
 ++submits;if(fail_submit&&submits==fail_submit)return false;
 const struct npt_cmd_resource_update*c=header;
 CHECK(hs==sizeof(*c)&&c->header.object_id==77&&c->subresource==target_sub);
 CHECK(c->header.cmd_size==hs+padded&&padded>=bytes&&padded-bytes<8&&c->byte_size==bytes);
 CHECK(bytes<=(64u<<20)&&bytes&&predicate==1);
 D3D11_BOX box=target;if(c->has_box)box=(D3D11_BOX){c->box_left,c->box_top,c->box_front,c->box_right,c->box_bottom,c->box_back};
 CHECK(box.left>=target.left&&box.right<=target.right&&box.top>=target.top&&box.bottom<=target.bottom&&box.front>=target.front&&box.back<=target.back);
 if(is_buffer){
  CHECK(box.top==0&&box.front==0&&box.bottom==1&&box.back==1&&bytes==box.right-box.left);
  size_t off=box.left-target.left;CHECK(off+bytes<=result_bytes);
  for(UINT i=0;i<bytes;++i)CHECK(coverage[off+i]==0);
  memset(coverage+off,1,bytes);memcpy(result+off,data,bytes);
 }else{
  UINT cr=(box.bottom-box.top+bh-1)/bh,cd=box.back-box.front;
  CHECK(box.left==target.left&&box.right==target.right&&c->row_pitch==rb&&c->depth_pitch==rb*cr&&bytes==rb*cr*cd);
  CHECK((box.top-target.top)%bh==0);UINT start=(box.top-target.top)/bh;
  for(UINT z=0;z<cd;++z)for(UINT y=0;y<cr;++y){size_t off=((size_t)(box.front-target.front+z)*rows+start+y)*rb;
   CHECK(off+rb<=result_bytes);for(UINT i=0;i<rb;++i)CHECK(coverage[off+i]==0);memset(coverage+off,1,rb);
   memcpy(result+off,(const uint8_t*)data+(size_t)z*c->depth_pitch+y*c->row_pitch,rb);
  }
 }
 return true;
}
static bool npt_ring_submit_indirect_locked_split(struct npt_ring*r,const void*h,uint32_t hs,const void*p,uint32_t n,uint32_t pad){return receive(r,h,hs,p,n,pad);}
static bool npt_ring_submit_locked_split(struct npt_ring*r,const void*h,uint32_t hs,const void*p,uint32_t n,uint32_t pad){return receive(r,h,hs,p,n,pad);}
static void *checked_malloc(size_t n){++allocations;if(fail_alloc)return NULL;CHECK(n<=4u<<20);void*p=malloc(n);CHECK(p);++live;memset(p,0xe1,n);return p;}
static void checked_free(void*p){if(p){CHECK(live==1);--live;free(p);}}
// PRODUCTION_RESOURCE
// PRODUCTION_RING
// PRODUCTION_DISPATCH
#define malloc checked_malloc
#define free checked_free
// PRODUCTION_CONTEXT
#undef malloc
#undef free
// PRODUCTION_REMOVAL
static uint8_t value(size_t row,UINT col){return (uint8_t)(row*13u+col*71u+3u);}
static void setup(UINT width,UINT height,UINT z,UINT mips,UINT arrays,DXGI_FORMAT fmt){
 CHECK(!live);primary.failed=method.failed=false;method.direct_size=4096;submits=allocations=fail_submit=0;fail_alloc=false;predicate=1;
 memset(&aux,0,sizeof(aux));aux.width=width;aux.height=height;aux.depth=z;aux.mip_levels=mips;aux.array_size=arrays;aux.format=fmt;aux.sample_count=1;
 object=(struct npt_com_base){{77},&aux,2,0};
}
static void run_texture(UINT w,UINT h,UINT d,UINT sub,DXGI_FORMAT fmt,UINT block,UINT blockbytes,UINT pad,UINT slice_pad,bool partial,UINT modern,bool huge_pitch){
 setup(w,h,d,4,2,fmt);target_sub=sub;UINT mip=sub%4,mw=w>>mip,mh=h>>mip,md=d>>mip;if(!mw)mw=1;if(!mh)mh=1;if(!md)md=1;
 target=(D3D11_BOX){0,0,0,mw,mh,md};
 if(partial&&mw>block*3&&mh>block*3){target.left=block;target.top=block;target.right=(mw/block-1)*block;target.bottom=(mh/block-1)*block;}
 if(partial&&md>2){target.front=1;target.back=md-1;}
 bh=block;rb=((target.right-target.left+block-1)/block)*blockbytes;rows=(target.bottom-target.top+block-1)/block;depth=target.back-target.front;
 UINT pitch=rb+pad,sp=pitch*rows+slice_pad;if(huge_pitch)pitch=UINT32_MAX;
 size_t extent=(size_t)(depth-1)*sp+(size_t)(rows-1)*pitch+rb;
 uint8_t*source=malloc(extent);CHECK(source);memset(source,0xd7,extent);
 result_bytes=(size_t)rb*rows*depth;result=malloc(result_bytes);coverage=calloc(result_bytes,1);CHECK(result&&coverage);memset(result,0x6f,result_bytes);is_buffer=false;
 for(UINT z=0;z<depth;++z)for(UINT y=0;y<rows;++y)for(UINT x=0;x<rb;++x)source[(size_t)z*sp+(size_t)y*pitch+x]=value((size_t)z*rows+y,x);
 for(UINT z=0;z<depth;++z){for(UINT y=0;y+1<rows;++y)__asan_poison_memory_region(source+(size_t)z*sp+(size_t)y*pitch+rb,pitch-rb);
  if(z+1<depth)__asan_poison_memory_region(source+(size_t)z*sp+(size_t)(rows-1)*pitch+rb,sp-((size_t)(rows-1)*pitch+rb));}
 if(modern)ctx_UpdateSubresource1_override(NULL,&object,sub,partial?&target:NULL,source,pitch,sp,2);
 else ctx_UpdateSubresource_override(NULL,&object,sub,partial?&target:NULL,source,pitch,sp);
 if(primary.failed||method.failed||!submits)fprintf(stderr,"case w=%u h=%u d=%u sub=%u block=%u partial=%u row=%u slice=%u\n",w,h,d,sub,block,partial,pitch,sp);
 CHECK(submits&&!live&&!primary.failed&&!method.failed&&predicate==1&&dev_GetDeviceRemovedReason_override(NULL)==0);
 for(size_t row=0;row<(size_t)rows*depth;++row)for(UINT x=0;x<rb;++x)CHECK(coverage[row*rb+x]&&result[row*rb+x]==value(row,x));
 if(result_bytes>(64u<<20))CHECK(submits>16);
 __asan_unpoison_memory_region(source,extent);free(source);free(result);free(coverage);result=coverage=NULL;
}
static void buffer(UINT bytes,bool partial,UINT modern){
 setup(1,1,1,1,1,DXGI_FORMAT_R8_UNORM);object.kind=1;object.width=bytes+(partial?16:0);target_sub=0;is_buffer=true;target=(D3D11_BOX){partial?8u:0u,0,0,bytes+(partial?8u:0u),1,1};
 result_bytes=bytes;uint8_t*source=malloc(bytes);result=malloc(bytes);coverage=calloc(bytes,1);CHECK(source&&result&&coverage);for(UINT i=0;i<bytes;++i)source[i]=(uint8_t)i;
 if(modern)ctx_UpdateSubresource1_override(NULL,&object,0,partial?&target:NULL,source,UINT32_MAX,UINT32_MAX,1);
 else ctx_UpdateSubresource_override(NULL,&object,0,partial?&target:NULL,source,UINT32_MAX,UINT32_MAX);
 CHECK(!primary.failed&&!method.failed&&!live&&submits);for(UINT i=0;i<bytes;++i)CHECK(coverage[i]&&result[i]==source[i]);free(source);free(result);free(coverage);result=coverage=NULL;
}
static void failed_split(void){
 const UINT bytes=(65u<<20)+4;
 setup(1,1,1,1,1,DXGI_FORMAT_R8_UNORM);object.kind=1;object.width=bytes;target_sub=0;is_buffer=true;target=(D3D11_BOX){0,0,0,bytes,1,1};
 result_bytes=bytes;uint8_t*source=malloc(bytes);result=malloc(bytes);coverage=calloc(bytes,1);CHECK(source&&result&&coverage);memset(source,0xa3,bytes);memset(result,0x71,bytes);
 fail_submit=2;ctx_UpdateSubresource_override(NULL,&object,0,NULL,source,0,0);
 CHECK(primary.failed&&method.failed&&submits==2&&!live&&dev_GetDeviceRemovedReason_override(NULL)==(HRESULT)0x887A0005);
 for(UINT i=0;i<bytes;++i)CHECK(i<(4u<<20)?(coverage[i]&&result[i]==0xa3):(!coverage[i]&&result[i]==0x71));
 free(source);free(result);free(coverage);result=coverage=NULL;
}
int main(void){
 for(UINT modern=0;modern<2;++modern){
  for(UINT sub=0;sub<8;++sub)for(UINT partial=0;partial<2;++partial){
   run_texture(64,48,8,sub,DXGI_FORMAT_R8G8B8A8_UNORM,1,4,32,128,partial,modern,false);
   run_texture(64,48,1,sub,DXGI_FORMAT_BC1_UNORM,4,8,32,64,partial,modern,false);
   run_texture(65,49,1,sub,DXGI_FORMAT_BC3_UNORM,4,16,32,64,partial,modern,false);
  }
  run_texture(1,1,1,0,DXGI_FORMAT_R8G8B8A8_UNORM,1,4,0,0,false,modern,true);
  /* Valid one-row partial upload into a taller texture, only four source bytes. */
  setup(8,8,1,1,1,DXGI_FORMAT_R8G8B8A8_UNORM);target=(D3D11_BOX){2,3,0,3,4,1};target_sub=0;rb=4;rows=depth=bh=1;is_buffer=false;result_bytes=4;
  uint8_t pixel[4]={3,4,5,6};result=calloc(4,1);coverage=calloc(4,1);ctx_UpdateSubresource_override(NULL,&object,0,&target,pixel,UINT32_MAX,UINT32_MAX);CHECK(!primary.failed&&submits==1&&!memcmp(result,pixel,4));free(result);free(coverage);result=coverage=NULL;
  buffer(65536,false,modern);buffer((65u<<20)+7,true,modern);
 }
 run_texture(8192,2304,1,0,DXGI_FORMAT_R8G8B8A8_UNORM,1,4,0,0,false,1,false); /* 72 MiB */
 run_texture(256,256,72,0,DXGI_FORMAT_R8G8B8A8_UNORM,1,4,0,64,false,0,false); /* padded 3D */
 failed_split();
 /* Empty updates touch neither source nor transport. */
 setup(4,4,1,1,1,DXGI_FORMAT_R8G8B8A8_UNORM);D3D11_BOX empty={2,0,0,1,1,1};ctx_UpdateSubresource_override(NULL,&object,0,&empty,(void*)1,0,0);CHECK(!submits&&!allocations&&!primary.failed);
 uint8_t padded[128];memset(padded,0,sizeof(padded));
 setup(2,2,1,1,1,DXGI_FORMAT_R8G8B8A8_UNORM);fail_alloc=true;ctx_UpdateSubresource_override(NULL,&object,0,NULL,padded,32,64);CHECK(!submits&&!live&&allocations==1&&primary.failed&&method.failed&&dev_GetDeviceRemovedReason_override(NULL)==(HRESULT)0x887A0005);
 setup(2,2,1,1,1,DXGI_FORMAT_R8G8B8A8_UNORM);fail_submit=1;ctx_UpdateSubresource_override(NULL,&object,0,NULL,padded,32,64);CHECK(submits==1&&!live&&primary.failed&&method.failed);
 setup(2,2,1,1,1,DXGI_FORMAT_R8G8B8A8_UNORM);ctx_UpdateSubresource_override(NULL,&object,4,NULL,padded,8,16);CHECK(!submits&&primary.failed);
 setup(2,2,1,1,1,DXGI_FORMAT_R8G8B8A8_UNORM);ctx_UpdateSubresource_override(NULL,&object,0,NULL,padded,7,16);CHECK(!submits&&primary.failed);
 setup(2,2,2,1,1,DXGI_FORMAT_R8G8B8A8_UNORM);ctx_UpdateSubresource_override(NULL,&object,0,NULL,padded,16,8);CHECK(!submits&&primary.failed);
 setup(2,2,1,1,1,DXGI_FORMAT_R8G8B8A8_UNORM);ctx_UpdateSubresource_override(NULL,&object,0,NULL,(void*)(UINTPTR_MAX-4u),8,0);CHECK(!submits&&primary.failed);
 /* Actual ring guard rejects pre-existing overflow shapes without reading. */
 setup(1,1,1,1,1,DXGI_FORMAT_R8G8B8A8_UNORM);CHECK(!npt_dispatch_resource_update(&method,77,0,UINT32_MAX,0,NULL,(void*)1,UINT32_MAX,4));CHECK(!submits);
 printf("D3D10 update transport CPU contracts %u checks passed\n",checks);return 0;
}
