/* SPDX-License-Identifier: MIT
 * CPU contracts for actual DDI/conversion code. This is not a GPU pixel oracle. */
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
typedef unsigned UINT,DXGI_FORMAT,D3D11_RESOURCE_DIMENSION;typedef uint64_t UINT64;typedef size_t SIZE_T;typedef int BOOL;typedef int32_t HRESULT;
#define APIENTRY
#define TRUE 1
#define FALSE 0
#define S_OK 0
#define E_FAIL (-1)
#define E_INVALIDARG (-2)
#define E_OUTOFMEMORY (-3)
#define FAILED(x) ((x)<0)
#define SUCCEEDED(x) ((x)>=0)
#define D3D11_RESOURCE_DIMENSION_TEXTURE2D 3
#define D3D11_USAGE_DEFAULT 0
#define D3D11_USAGE_STAGING 3
#define D3D11_CPU_ACCESS_READ 1
#define D3D11_MAP_READ 1
#define D3D11_BIND_SHADER_RESOURCE 8
#define D3D11_BIND_RENDER_TARGET 32
enum {DXGI_FORMAT_UNKNOWN=0,DXGI_FORMAT_R8G8B8A8_TYPELESS=27,DXGI_FORMAT_R8G8B8A8_UNORM=28,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB=29,DXGI_FORMAT_B8G8R8A8_UNORM=87,DXGI_FORMAT_B8G8R8X8_UNORM=88,DXGI_FORMAT_B8G8R8A8_TYPELESS=90,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB=91,DXGI_FORMAT_B8G8R8X8_TYPELESS=92,DXGI_FORMAT_B8G8R8X8_UNORM_SRGB=93};
typedef struct {UINT Count,Quality;} SAMPLE;
typedef struct {UINT Width,Height,MipLevels,ArraySize;DXGI_FORMAT Format;SAMPLE SampleDesc;UINT Usage,BindFlags,CPUAccessFlags,MiscFlags;} D3D11_TEXTURE2D_DESC;
typedef struct {UINT left,top,front,right,bottom,back;} D3D11_BOX,D3D10_DDI_BOX;
typedef struct {const void *pSysMem;UINT SysMemPitch,SysMemSlicePitch;} D3D11_SUBRESOURCE_DATA;
typedef struct {void *pData;UINT RowPitch,DepthPitch;} D3D11_MAPPED_SUBRESOURCE;
typedef struct texture {D3D11_TEXTURE2D_DESC desc;unsigned char data[8192];BOOL owned,mapped;} ID3D11Texture2D,ID3D11Resource;
typedef ID3D11Resource ID3D11ShaderResourceView, ID3D11RenderTargetView;
typedef struct {unsigned refs;} ID3D11Predicate;
typedef struct {ID3D11Resource *pResource;DXGI_FORMAT Format,HostFormat;} RESOURCE,*PTRITON_RESOURCE;
typedef struct {void *pDev1,*pCtx1;} DEVICE,*PTRITON_DEVICE;
typedef struct {void *pDrvPrivate;} D3D10DDI_HDEVICE,D3D10DDI_HRESOURCE;
static unsigned checks,created,live,fail_create,resolve_calls,final_copies;static bool fail_convert,allow_copy;
static unsigned views,fail_view,view_calls,meta_calls;
static ID3D11Predicate predicate,*active_predicate;static BOOL active_value,original_value;static HRESULT error;
#define CHECK(x) do{++checks;if(!(x)){fprintf(stderr,"FAIL conversion line %u: %s\n",__LINE__,#x);exit(1);}}while(0)
static void tritonSetError(PTRITON_DEVICE d,HRESULT h){error=h;}
static UINT width(ID3D11Resource*t,UINT sub){UINT n=t->desc.Width>>(sub%t->desc.MipLevels);return n?n:1;}
static UINT height(ID3D11Resource*t,UINT sub){UINT n=t->desc.Height>>(sub%t->desc.MipLevels);return n?n:1;}
static UINT pitch(ID3D11Resource*t,UINT sub){return width(t,sub)*4+16;}
static unsigned char *pixel(ID3D11Resource*t,UINT sub,UINT x,UINT y){size_t offset=0;CHECK(sub<t->desc.MipLevels*t->desc.ArraySize&&x<width(t,sub)&&y<height(t,sub));for(UINT i=0;i<sub;++i)offset+=pitch(t,i)*height(t,i);offset+=pitch(t,sub)*y+x*4;CHECK(offset+4<=sizeof(t->data));return t->data+offset;}
static bool typeless(DXGI_FORMAT f){return f==27||f==90||f==92;}
static UINT family(DXGI_FORMAT f){return f==27||f==28||f==29?1:f==88||f==92||f==93?3:2;}
static void ID3D11Resource_GetType(ID3D11Resource*t,D3D11_RESOURCE_DIMENSION*out){*out=D3D11_RESOURCE_DIMENSION_TEXTURE2D;}
static void ID3D11Texture2D_GetDesc(ID3D11Texture2D*t,D3D11_TEXTURE2D_DESC*d){*d=t->desc;}
static HRESULT ID3D11Device1_CreateTexture2D(void*d,const D3D11_TEXTURE2D_DESC*desc,const D3D11_SUBRESOURCE_DATA*initial,ID3D11Texture2D**out){
 CHECK(desc->Usage==D3D11_USAGE_DEFAULT&&!desc->CPUAccessFlags&&!initial);
 *out=NULL;++created;if(created==fail_create)return E_OUTOFMEMORY;ID3D11Texture2D*t=calloc(1,sizeof(*t));CHECK(t);t->owned=1;t->desc=*desc;++live;*out=t;
 if(initial)for(UINT y=0;y<desc->Height;++y)memcpy(pixel(t,0,0,y),(const unsigned char*)initial->pSysMem+y*initial->SysMemPitch,desc->Width*4);return S_OK;
}
static void ID3D11Texture2D_Release(ID3D11Texture2D*t){CHECK(t->owned&&!t->mapped&&live);--live;free(t);}
static void ID3D11DeviceContext1_GetPredication(void*c,ID3D11Predicate**p,BOOL*v){*p=active_predicate;*v=active_value;if(*p)++(*p)->refs;}
static void ID3D11DeviceContext1_SetPredication(void*c,ID3D11Predicate*p,BOOL v){active_predicate=p;active_value=v;}
static void ID3D11Predicate_Release(ID3D11Predicate*p){CHECK(p->refs);--p->refs;}
static void ID3D11DeviceContext1_CopyResource(void*c,ID3D11Resource*d,ID3D11Resource*s){
 CHECK((family(d->desc.Format)==1)==(family(s->desc.Format)==1));CHECK(d->desc.SampleDesc.Count==s->desc.SampleDesc.Count);CHECK(!active_predicate);memcpy(d->data,s->data,sizeof(d->data));
}
static void ID3D11DeviceContext1_CopySubresourceRegion(void*c,ID3D11Resource*d,UINT ds,UINT x,UINT y,UINT z,ID3D11Resource*s,UINT ss,const D3D11_BOX*b){
 CHECK((family(d->desc.Format)==1)==(family(s->desc.Format)==1));CHECK(z==0);D3D11_BOX box={0,0,0,width(s,ss),height(s,ss),1};if(b)box=*b;
 if(d->owned)CHECK(!active_predicate);
 else{++final_copies;CHECK(active_predicate==&predicate&&active_value==original_value);if(!allow_copy)return;}
 for(UINT row=0;row<box.bottom-box.top;++row)for(UINT col=0;col<box.right-box.left;++col)memcpy(pixel(d,ds,x+col,y+row),pixel(s,ss,box.left+col,box.top+row),4);
}
static void ID3D11DeviceContext1_CopySubresourceRegion1(void*c,ID3D11Resource*d,UINT ds,UINT x,UINT y,UINT z,ID3D11Resource*s,UINT ss,const D3D11_BOX*b,UINT flags){ID3D11DeviceContext1_CopySubresourceRegion(c,d,ds,x,y,z,s,ss,b);}
static void ID3D11DeviceContext1_ResolveSubresource(void*c,ID3D11Resource*d,UINT ds,ID3D11Resource*s,UINT ss,DXGI_FORMAT f){
 ++resolve_calls;CHECK(!active_predicate);CHECK(s->desc.SampleDesc.Count>1&&d->desc.SampleDesc.Count==1);CHECK(d->desc.Format==f&&(typeless(s->desc.Format)||s->desc.Format==f));
 for(UINT row=0;row<height(d,ds);++row)for(UINT col=0;col<width(d,ds);++col)memcpy(pixel(d,ds,col,row),pixel(s,ss,col,row),4);
}
static HRESULT make_view(ID3D11Resource*t,UINT bind,ID3D11Resource**out){
 *out=NULL;++view_calls;if(fail_view==view_calls)return E_FAIL;
 CHECK(t->desc.BindFlags==bind&&t->desc.Usage==D3D11_USAGE_DEFAULT&&!t->desc.CPUAccessFlags);
 CHECK(t->desc.Format==28||t->desc.Format==87||t->desc.Format==88);++views;*out=t;return S_OK;
}
static HRESULT ID3D11Device1_CreateShaderResourceView(void*d,ID3D11Resource*t,const void*desc,ID3D11ShaderResourceView**out){CHECK(!desc);return make_view(t,D3D11_BIND_SHADER_RESOURCE,out);}
static HRESULT ID3D11Device1_CreateRenderTargetView(void*d,ID3D11Resource*t,const void*desc,ID3D11RenderTargetView**out){CHECK(!desc);return make_view(t,D3D11_BIND_RENDER_TARGET,out);}
static void ID3D11ShaderResourceView_Release(ID3D11ShaderResourceView*v){CHECK(v&&views);--views;}
static void ID3D11RenderTargetView_Release(ID3D11RenderTargetView*v){CHECK(v&&views);--views;}
/* Independent model of the transport contract. GPU pixels/query behavior is
 * tested by the native backend fixture; this model checks guest call ordering. */
static HRESULT tritonSharedBridgeCopyColor(void*c,ID3D11RenderTargetView*d,ID3D11ShaderResourceView*s){
 ++meta_calls;CHECK(!active_predicate&&views==2);if(fail_convert)return E_FAIL;
 CHECK(width(d,0)==width(s,0)&&height(d,0)==height(s,0));
 const bool swap=(family(d->desc.Format)==1)!=(family(s->desc.Format)==1);
 for(UINT y=0;y<height(d,0);++y)for(UINT x=0;x<width(d,0);++x){
  unsigned char*o=pixel(d,0,x,y);const unsigned char*i=pixel(s,0,x,y);
  o[0]=i[swap?2:0];o[1]=i[1];o[2]=i[swap?0:2];o[3]=family(s->desc.Format)==3?255:i[3];
 }
 return S_OK;
}
// PRODUCTION
static DEVICE device;static ID3D11Resource src,dst;static RESOURCE sr,dr;
static void setup(bool forward,UINT predicate_value,bool execute){
 CHECK(!live&&!views&&!predicate.refs);created=resolve_calls=final_copies=fail_create=0;fail_convert=false;fail_view=view_calls=meta_calls=0;allow_copy=execute;error=S_OK;
 active_predicate=&predicate;active_value=original_value=predicate_value;
 memset(&src,0,sizeof(src));memset(&dst,0,sizeof(dst));
 src.desc=(D3D11_TEXTURE2D_DESC){8,8,1,1,forward?28:91,{1,0},0,0,0,0};dst.desc=(D3D11_TEXTURE2D_DESC){8,8,1,1,forward?91:28,{1,0},0,0,0,0};
 sr=(RESOURCE){&src,87,forward?28:0};dr=(RESOURCE){&dst,87,forward?0:28};memset(dst.data,0xa5,sizeof(dst.data));
 for(UINT y=0;y<8;++y)for(UINT x=0;x<8;++x){unsigned char*p=pixel(&src,0,x,y);p[0]=x*17+3;p[1]=y*23+5;p[2]=x+y+90;p[3]=191;}
}
static void verify(UINT dx,UINT dy,UINT sx,UINT sy,UINT w,UINT h){
 CHECK(!live&&!views&&!predicate.refs&&active_predicate==&predicate&&active_value==original_value&&final_copies==1&&meta_calls==1);
 for(UINT y=0;y<8;++y)for(UINT x=0;x<8;++x){unsigned char*p=pixel(&dst,0,x,y);if(allow_copy&&x>=dx&&x<dx+w&&y>=dy&&y<dy+h){unsigned char*q=pixel(&src,0,sx+x-dx,sy+y-dy);CHECK(p[0]==q[2]&&p[1]==q[1]&&p[2]==q[0]&&p[3]==q[3]);}else CHECK(p[0]==0xa5&&p[1]==0xa5&&p[2]==0xa5&&p[3]==0xa5);}
}
int main(void){
 for(UINT direction=0;direction<2;++direction)for(UINT value=0;value<2;++value)for(UINT execute=0;execute<2;++execute){
  setup(direction,value,execute);tritonResourceCopy((D3D10DDI_HDEVICE){&device},(D3D10DDI_HRESOURCE){&dr},(D3D10DDI_HRESOURCE){&sr});CHECK(error==S_OK);verify(0,0,0,0,8,8);
  for(UINT modern=0;modern<2;++modern){setup(direction,value,execute);D3D10_DDI_BOX box={1,2,0,5,7,1};if(modern)tritonResourceCopyRegion_11_1((D3D10DDI_HDEVICE){&device},(D3D10DDI_HRESOURCE){&dr},0,2,1,0,(D3D10DDI_HRESOURCE){&sr},0,&box,1);else tritonResourceCopyRegion((D3D10DDI_HDEVICE){&device},(D3D10DDI_HRESOURCE){&dr},0,2,1,0,(D3D10DDI_HRESOURCE){&sr},0,&box);CHECK(error==S_OK);verify(2,1,1,2,4,5);}
 }
 /* Selected mip/array slices retain their own dimensions and untouched peers. */
 for(UINT direction=0;direction<2;++direction){
  setup(direction,1,true);ID3D11Resource*array=direction?&dst:&src;array->desc.Width=array->desc.Height=16;array->desc.MipLevels=array->desc.ArraySize=2;
  if(!direction)for(UINT sub=0;sub<4;++sub)for(UINT row=0;row<height(&src,sub);++row)for(UINT col=0;col<width(&src,sub);++col){unsigned char*p=pixel(&src,sub,col,row);p[0]=sub*31+col;p[1]=row+11;p[2]=201-col;p[3]=219;}
  D3D10_DDI_BOX box={1,2,0,5,7,1};UINT ds=direction?3:0,ss=direction?0:3;
  tritonResourceCopyRegion((D3D10DDI_HDEVICE){&device},(D3D10DDI_HRESOURCE){&dr},ds,2,1,0,(D3D10DDI_HRESOURCE){&sr},ss,&box);
  CHECK(error==S_OK&&!live&&!predicate.refs&&final_copies==1);
  for(UINT sub=0;sub<dst.desc.MipLevels*dst.desc.ArraySize;++sub)for(UINT row=0;row<height(&dst,sub);++row)for(UINT col=0;col<width(&dst,sub);++col){unsigned char*p=pixel(&dst,sub,col,row);if(sub==ds&&col>=2&&col<6&&row>=1&&row<6){unsigned char*q=pixel(&src,ss,col-1,row+1);CHECK(p[0]==q[2]&&p[1]==q[1]&&p[2]==q[0]&&p[3]==q[3]);}else CHECK(p[0]==0xa5&&p[1]==0xa5&&p[2]==0xa5&&p[3]==0xa5);}
 }
 /* Typed SRGB, normalized UNORM and typeless MSAA sources use legal typed resolves. */
 for(UINT fmt=0;fmt<3;++fmt){setup(false,1,true);src.desc.Format=fmt==0?91:fmt==1?87:90;src.desc.SampleDesc.Count=4;sr.Format=91;sr.HostFormat=fmt==1?87:0;tritonResourceResolveSubresource((D3D10DDI_HDEVICE){&device},(D3D10DDI_HRESOURCE){&dr},0,(D3D10DDI_HRESOURCE){&sr},0,91);CHECK(error==S_OK&&resolve_calls==1);verify(0,0,0,0,8,8);}
 for(UINT failure=1;failure<=4;++failure){setup(false,0,true);src.desc.Format=87;src.desc.SampleDesc.Count=4;sr.Format=91;sr.HostFormat=87;fail_create=failure;CHECK(tritonResourceCopyConverted(&device,&dr,0,0,0,0,&sr,0,NULL,91)==E_OUTOFMEMORY);CHECK(!live&&!predicate.refs&&!final_copies&&active_predicate==&predicate&&active_value==original_value);}
 setup(true,0,true);fail_convert=true;CHECK(tritonResourceCopyConverted(&device,&dr,0,0,0,0,&sr,0,NULL,0)==E_FAIL);CHECK(!live&&!predicate.refs&&!final_copies&&active_predicate==&predicate);
 for(UINT v=1;v<=2;++v){setup(true,0,true);fail_view=v;CHECK(tritonResourceCopyConverted(&device,&dr,0,0,0,0,&sr,0,NULL,0)==E_FAIL);CHECK(!live&&!views&&!predicate.refs&&!final_copies&&active_predicate==&predicate);}
 setup(false,0,true);src.desc.Format=88;sr.HostFormat=88;CHECK(tritonResourceCopyConverted(&device,&dr,0,0,0,0,&sr,0,NULL,0)==S_OK);for(UINT y=0;y<8;++y)for(UINT x=0;x<8;++x){const unsigned char*p=pixel(&dst,0,x,y),*q=pixel(&src,0,x,y);CHECK(p[0]==q[2]&&p[1]==q[1]&&p[2]==q[0]&&p[3]==255);}
 setup(true,0,true);dst.desc.Format=88;dr.HostFormat=88;CHECK(tritonResourceCopyConverted(&device,&dr,0,0,0,0,&sr,0,NULL,0)==S_OK);verify(0,0,0,0,8,8);
 setup(true,0,true);D3D11_BOX invalid={7,0,0,9,1,1};CHECK(tritonResourceCopyConverted(&device,&dr,0,0,0,0,&sr,0,&invalid,0)==E_INVALIDARG&&!created);
 setup(true,0,true);D3D11_BOX empty={0,0,0,4,4,0};CHECK(tritonResourceCopyConverted(&device,&dr,0,0,0,0,&sr,0,&empty,0)==S_OK&&!created&&!final_copies);empty=(D3D11_BOX){7,0,0,2,4,1};CHECK(tritonResourceCopyConverted(&device,&dr,0,0,0,0,&sr,0,&empty,0)==S_OK&&!created&&!final_copies);
 printf("D3D10 primary conversion CPU contracts %u checks passed\n",checks);return 0;
}
