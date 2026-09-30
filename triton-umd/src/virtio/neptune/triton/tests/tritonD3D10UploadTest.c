/* SPDX-License-Identifier: MIT
 * Actual DDI upload helpers with ASan-poisoned source row padding. */
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <sanitizer/asan_interface.h>
typedef unsigned UINT,DXGI_FORMAT,D3D11_RESOURCE_DIMENSION;typedef uint64_t UINT64;typedef SIZE_TYPE SIZE_T;typedef int32_t HRESULT;typedef int BOOL;typedef void VOID;
#define APIENTRY
#define S_OK 0
#define S_FALSE 1
#define E_INVALIDARG (-1)
#define E_OUTOFMEMORY (-2)
#define E_FAIL (-3)
#define FAILED(x) ((x)<0)
#define D3D11_RESOURCE_DIMENSION_TEXTURE2D 3
#define D3D11_USAGE_DEFAULT 0
#define D3D11_USAGE_IMMUTABLE 1
#define D3D11_USAGE_DYNAMIC 2
#define HEAP_ZERO_MEMORY 8
enum {DXGI_FORMAT_UNKNOWN=0,DXGI_FORMAT_R8G8B8A8_TYPELESS=27,DXGI_FORMAT_R8G8B8A8_UNORM=28,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB=29,DXGI_FORMAT_B8G8R8A8_UNORM=87,DXGI_FORMAT_B8G8R8X8_UNORM=88,DXGI_FORMAT_B8G8R8A8_TYPELESS=90,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB=91,DXGI_FORMAT_B8G8R8X8_TYPELESS=92,DXGI_FORMAT_B8G8R8X8_UNORM_SRGB=93};
typedef struct {UINT Count,Quality;} SAMPLE;
typedef struct {UINT Width,Height,MipLevels,ArraySize;DXGI_FORMAT Format;SAMPLE SampleDesc;UINT Usage;} D3D11_TEXTURE2D_DESC;
typedef struct {UINT left,top,front,right,bottom,back;} D3D11_BOX,D3D10_DDI_BOX;
typedef struct {D3D11_TEXTURE2D_DESC desc;unsigned char pixels[8192];} ID3D11Resource,ID3D11Texture2D;
typedef struct {void *pCtx1;} DEVICE,*PTRITON_DEVICE;
typedef struct {ID3D11Resource *pResource;DXGI_FORMAT Format,HostFormat;} RESOURCE,*PTRITON_RESOURCE;
typedef struct {void *pDrvPrivate;} D3D10DDI_HDEVICE,D3D10DDI_HRESOURCE;
static unsigned checks,allocations,live,updates,expected_modern,expected_sub,expected_row,expected_depth,expected_flags,descriptor_calls;
static HRESULT error;static bool fail_alloc,host_failure,poison_padding;static void *allocated;static size_t allocation_bytes;
static unsigned predicate_value,predicate_outcome;static const void *expected_source;static const D3D11_BOX *expected_box;
#define CHECK(x) do {++checks;if(!(x)){fprintf(stderr,"FAIL upload line %u: %s\n",__LINE__,#x);exit(1);}}while(0)
static void *GetProcessHeap(void){return (void*)1;}
static void *HeapAlloc(void*h,UINT flags,SIZE_T bytes){CHECK(h==(void*)1);++allocations;if(fail_alloc)return NULL;CHECK(!live);allocated=malloc(bytes);CHECK(allocated);++live;allocation_bytes=bytes;memset(allocated,flags&HEAP_ZERO_MEMORY?0:0xe1,bytes);return allocated;}
static void HeapFree(void*h,UINT flags,void*p){CHECK(h==(void*)1&&!flags&&live==1&&p==allocated);free(p);allocated=NULL;allocation_bytes=0;--live;}
static void tritonSetError(PTRITON_DEVICE d,HRESULT hr){error=hr;}
static void ID3D11Resource_GetType(ID3D11Resource*r,D3D11_RESOURCE_DIMENSION*out){++descriptor_calls;*out=D3D11_RESOURCE_DIMENSION_TEXTURE2D;}
static void ID3D11Texture2D_GetDesc(ID3D11Texture2D*r,D3D11_TEXTURE2D_DESC*out){++descriptor_calls;*out=r->desc;}
static UINT mw(ID3D11Resource*r,UINT sub){UINT w=r->desc.Width>>(sub%r->desc.MipLevels);return w?w:1;}
static UINT mh(ID3D11Resource*r,UINT sub){UINT h=r->desc.Height>>(sub%r->desc.MipLevels);return h?h:1;}
static unsigned char *pixel(ID3D11Resource*r,UINT sub,UINT x,UINT y){size_t offset=0;for(UINT i=0;i<sub;++i)offset+=mw(r,i)*mh(r,i)*4;offset+=(y*mw(r,sub)+x)*4;CHECK(offset+4<=sizeof(r->pixels));return r->pixels+offset;}
static void update(void*c,ID3D11Resource*r,UINT sub,const D3D11_BOX*b,const void*p,UINT row,UINT depth,UINT flags,UINT modern){
 ++updates;CHECK(sub==expected_sub&&b==expected_box&&row==expected_row&&depth==expected_depth&&flags==expected_flags&&modern==expected_modern);
 CHECK(predicate_value<2&&predicate_outcome<2);
 if(expected_source)CHECK(p==expected_source);else CHECK(p==allocated&&live==1);
 D3D11_BOX box={0,0,0,mw(r,sub),mh(r,sub),1};if(b)box=*b;
 UINT w=box.right-box.left,h=box.bottom-box.top;
 if(!expected_source&&poison_padding)for(UINT y=0;y+1<h;++y)for(UINT i=w*4;i<row;++i)CHECK(((const unsigned char*)p)[(size_t)y*row+i]==0);
 /* Test both possible backend predicate outcomes without changing state here. */
 if(predicate_outcome)for(UINT y=0;y<h;++y)for(UINT x=0;x<w;++x)memcpy(pixel(r,sub,box.left+x,box.top+y),(const unsigned char*)p+(size_t)y*row+x*4,4);
 if(host_failure)error=E_FAIL;
}
static void ID3D11DeviceContext1_UpdateSubresource(void*c,ID3D11Resource*r,UINT sub,const D3D11_BOX*b,const void*p,UINT row,UINT depth){update(c,r,sub,b,p,row,depth,0,0);}
static void ID3D11DeviceContext1_UpdateSubresource1(void*c,ID3D11Resource*r,UINT sub,const D3D11_BOX*b,const void*p,UINT row,UINT depth,UINT flags){update(c,r,sub,b,p,row,depth,flags,1);}
// PRODUCTION
static DEVICE device;static ID3D11Resource texture;static RESOURCE resource;
static void reset(DXGI_FORMAT logical,DXGI_FORMAT physical){
 CHECK(!live);allocations=updates=descriptor_calls=0;error=S_OK;fail_alloc=host_failure=poison_padding=false;expected_source=NULL;
 memset(&texture,0,sizeof(texture));memset(texture.pixels,0x79,sizeof(texture.pixels));texture.desc=(D3D11_TEXTURE2D_DESC){8,8,4,2,physical,{1,0},0};resource=(RESOURCE){&texture,logical,physical};
}
static void call(UINT modern,UINT sub,const D3D10_DDI_BOX*b,const void*src,UINT row,UINT depth,UINT flags){
 expected_modern=modern;expected_sub=sub;expected_box=b;expected_row=row;expected_depth=depth;expected_flags=modern?flags:0;
 if(modern)tritonResourceUpdateSubresourceUP_11_1((D3D10DDI_HDEVICE){&device},(D3D10DDI_HRESOURCE){&resource},sub,b,src,row,depth,flags);
 else tritonResourceUpdateSubresourceUP((D3D10DDI_HDEVICE){&device},(D3D10DDI_HRESOURCE){&resource},sub,b,src,row,depth);
}
static void one(DXGI_FORMAT logical,DXGI_FORMAT physical,UINT modern,UINT sub,bool partial,UINT padded,UINT pred,bool outcome){
 reset(logical,physical);predicate_value=pred;predicate_outcome=outcome;
 UINT w=mw(&texture,sub),h=mh(&texture,sub);D3D10_DDI_BOX box={0,0,0,w,h,1};if(partial&&w>2&&h>2){box.left=1;box.top=1;--box.right;--box.bottom;}
 w=box.right-box.left;h=box.bottom-box.top;UINT row=padded?((w*4+31)&~31u)+32:w*4;size_t extent=(size_t)(h-1)*row+w*4;
 unsigned char *source=malloc(extent);CHECK(source);memset(source,0xd3,extent);
 for(UINT y=0;y<h;++y)for(UINT x=0;x<w;++x){unsigned char*p=source+(size_t)y*row+x*4;p[0]=x*11+3;p[1]=y*19+7;p[2]=x+y+173;p[3]=99+y;}
 poison_padding=padded;
 if(poison_padding)for(UINT y=0;y+1<h;++y)__asan_poison_memory_region(source+(size_t)y*row+w*4,row-w*4);
 call(modern,sub,partial?&box:NULL,source,row,0x1234u+sub,modern?2:0);
 CHECK(error==S_OK&&updates==1&&allocations==1&&!live&&predicate_value==pred&&predicate_outcome==outcome);
 for(UINT level=0;level<8;++level)for(UINT y=0;y<mh(&texture,level);++y)for(UINT x=0;x<mw(&texture,level);++x){unsigned char*p=pixel(&texture,level,x,y);if(outcome&&level==sub&&x>=box.left&&x<box.right&&y>=box.top&&y<box.bottom){UINT sx=x-box.left,sy=y-box.top;CHECK(p[0]==sx+sy+173&&p[1]==sy*19+7&&p[2]==sx*11+3&&p[3]==99+sy);}else CHECK(p[0]==0x79&&p[1]==0x79&&p[2]==0x79&&p[3]==0x79);}
 __asan_unpoison_memory_region(source,extent);
 for(UINT y=0;y<h;++y)for(UINT x=0;x<w;++x){unsigned char*p=source+(size_t)y*row+x*4;CHECK(p[0]==x*11+3&&p[2]==x+y+173);}
 free(source);
}
int main(void){
 const UINT pairs[][2]={{87,28},{91,29},{88,28},{93,29},{28,87},{29,91},{90,27}};const UINT subs[]={0,1,3,4,7};
 for(UINT pair=0;pair<7;++pair)for(UINT modern=0;modern<2;++modern)for(UINT sub=0;sub<5;++sub)for(UINT partial=0;partial<2;++partial)for(UINT pad=0;pad<2;++pad)for(UINT pred=0;pred<2;++pred)one(pairs[pair][0],pairs[pair][1],modern,subs[sub],partial,pad,pred,(pair+modern+pred)%2);
 unsigned char source[256];memset(source,0x31,sizeof(source));
 for(UINT modern=0;modern<2;++modern){
  reset(87,28);fail_alloc=true;call(modern,0,NULL,source,32,77,1);CHECK(error==E_OUTOFMEMORY&&!updates&&!live&&allocations==1);
  reset(87,28);call(modern,0,NULL,source,31,0,0);CHECK(error==E_INVALIDARG&&!updates&&!allocations);
  reset(87,28);call(modern,8,NULL,source,32,0,0);CHECK(error==E_INVALIDARG&&!updates&&!allocations);
  reset(87,28);call(modern,0,NULL,NULL,32,0,0);CHECK(error==E_INVALIDARG&&!updates&&!allocations);
  reset(87,28);D3D10_DDI_BOX box={0,0,0,9,1,1};call(modern,0,&box,source,36,0,0);CHECK(error==E_INVALIDARG&&!updates&&!allocations);
  reset(87,28);box=(D3D10_DDI_BOX){0,0,0,4,4,0};call(modern,0,&box,(void*)1,0,0,0);CHECK(error==S_OK&&!updates&&!allocations&&!descriptor_calls);
  reset(87,28);box=(D3D10_DDI_BOX){7,0,0,2,4,1};call(modern,0,&box,(void*)1,0,0,0);CHECK(error==S_OK&&!updates&&!allocations);
  reset(999,998);call(modern,0,NULL,source,32,0,0);CHECK(error==E_INVALIDARG&&!updates&&!allocations);
  reset(87,28);texture.desc.SampleDesc.Count=4;call(modern,0,NULL,source,32,0,0);CHECK(error==E_INVALIDARG&&!updates&&!allocations);
  reset(87,28);texture.desc.Usage=D3D11_USAGE_DYNAMIC;call(modern,0,NULL,source,32,0,0);CHECK(error==E_INVALIDARG&&!updates&&!allocations);
  reset(87,28);texture.desc.Format=87;call(modern,0,NULL,source,32,0,0);CHECK(error==E_INVALIDARG&&!updates&&!allocations);
  reset(87,28);texture.desc.Width=2;texture.desc.Height=1;texture.desc.MipLevels=1;call(modern,0,NULL,(void*)(UINTPTR_MAX-4u),8,0,0);CHECK(error==E_INVALIDARG&&!updates&&!allocations);
  reset(87,28);texture.desc.Width=1;texture.desc.Height=2;texture.desc.MipLevels=1;fail_alloc=true;call(modern,0,NULL,source,UINT32_MAX,0,0);CHECK(error==E_OUTOFMEMORY&&!updates&&!live);CHECK(allocations==(sizeof(SIZE_T)==4?0:1));
  /* Same-order SRGB/UNORM and ordinary resources retain the exact source. */
  reset(91,87);expected_source=source;call(modern,0,NULL,source,32,13,1);CHECK(error==S_OK&&updates==1&&!allocations&&!descriptor_calls);
  reset(87,28);resource.HostFormat=0;expected_source=source;call(modern,0,NULL,source,32,0,0);CHECK(error==S_OK&&updates==1&&!allocations&&!descriptor_calls);
  reset(87,28);host_failure=true;call(modern,0,NULL,source,32,0,0);CHECK(error==E_FAIL&&updates==1&&!live);
 }
 /* Constant-buffer callbacks share the corrected route and remain passthrough
  * for actual buffers, whose HostFormat is zero. */
 reset(0,0);expected_source=source;expected_modern=0;expected_sub=0;expected_box=NULL;expected_row=32;expected_depth=77;expected_flags=0;
 tritonDefaultCbUpdateSubresourceUP((D3D10DDI_HDEVICE){&device},(D3D10DDI_HRESOURCE){&resource},0,NULL,source,32,77);CHECK(updates==1&&!allocations&&!descriptor_calls);
 expected_modern=1;expected_flags=2;tritonDefaultCbUpdateSubresourceUP_11_1((D3D10DDI_HDEVICE){&device},(D3D10DDI_HRESOURCE){&resource},0,NULL,source,32,77,2);CHECK(updates==2&&!allocations&&!descriptor_calls);
 printf("D3D10 physical upload CPU contracts %u checks passed\n",checks);return 0;
}
