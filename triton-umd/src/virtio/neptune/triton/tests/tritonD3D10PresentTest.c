/* SPDX-License-Identifier: MIT
 * Native D3D11 test with production resource/presentation code inserted by the
 * runner. Only the runtime allocation boundary is simulated; texture creation,
 * sample count, resolve and pixel readback use the real host backend. */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <d3d11.h>
#include "tritonSharedBridge.h"
#define CHECK(c,m) do { if (!(c)) { fprintf(stderr,"FAIL: %s line=%u\n",m,__LINE__); exit(1); } } while(0)
#define TR_LOG(...) ((void)0)
#define APIENTRY
#define NPT_D3D10_RUNTIME_DDI 1
#define TRITON_D3D11_MISC_LINEAR_EXPORT 0x40000000u
#define TRITON_D3D11_MISC_SINGLE_PLANE_EXPORT 0x20000000u
#define D3D10_DDI_USAGE_DEFAULT D3D11_USAGE_DEFAULT
#define D3D10_DDI_USAGE_IMMUTABLE D3D11_USAGE_IMMUTABLE
#define D3D10_DDI_USAGE_DYNAMIC D3D11_USAGE_DYNAMIC
#define D3D10_DDI_USAGE_STAGING D3D11_USAGE_STAGING
#define D3D10_DDI_CPU_ACCESS_READ D3D11_CPU_ACCESS_READ
#define D3D10_DDI_CPU_ACCESS_WRITE D3D11_CPU_ACCESS_WRITE
#define D3D10_DDI_BIND_VERTEX_BUFFER 1
#define D3D10_DDI_BIND_INDEX_BUFFER 2
#define D3D10_DDI_BIND_CONSTANT_BUFFER 4
#define D3D10_DDI_BIND_SHADER_RESOURCE 8
#define D3D10_DDI_BIND_STREAM_OUTPUT 16
#define D3D10_DDI_BIND_RENDER_TARGET 32
#define D3D10_DDI_BIND_DEPTH_STENCIL 64
#define D3D10_DDI_BIND_PRESENT 128
#define D3D11_DDI_BIND_UNORDERED_ACCESS 256
#define D3D11_DDI_BIND_DECODER 512
#define D3D11_DDI_BIND_VIDEO_ENCODER 1024
#define D3D10_DDI_RESOURCE_AUTO_GEN_MIP_MAP 1
#define D3D10_DDI_RESOURCE_MISC_SHARED 2
#define D3D11_DDI_RESOURCE_MISC_DRAWINDIRECT_ARGS 16
#define D3D11_DDI_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS 32
#define D3D11_DDI_RESOURCE_MISC_BUFFER_STRUCTURED 64
#define D3D11_DDI_RESOURCE_MISC_RESOURCE_CLAMP 128
#define VIOGPU_RESOURCE_TYPE_SHARED 1
using D3DKMT_HANDLE=uint32_t;
using ULONGLONG=uint64_t;
using TRITON_VIEWLINK=void;
struct Handle {union {void *pDrvPrivate;uintptr_t handle;};};
using D3D10DDI_HDEVICE=Handle;using D3D10DDI_HRESOURCE=Handle;using D3D10DDI_HRTRESOURCE=Handle;
enum {D3D10DDIRESOURCE_BUFFER,D3D10DDIRESOURCE_TEXTURE1D,D3D10DDIRESOURCE_TEXTURE2D,D3D10DDIRESOURCE_TEXTURE3D,D3D10DDIRESOURCE_TEXTURECUBE,D3D11DDIRESOURCE_BUFFEREX};
struct Mip {UINT TexelWidth,TexelHeight,TexelDepth;};
struct D3D11DDIARG_CREATERESOURCE {
 const Mip *pMipInfoList;const D3D11_SUBRESOURCE_DATA *pInitialDataUP;
 UINT ResourceDimension,Usage,BindFlags,MapFlags,MiscFlags;DXGI_FORMAT Format;
 DXGI_SAMPLE_DESC SampleDesc;UINT MipLevels,ArraySize,ByteStride;const void *pPrimaryDesc;
};
struct VIOGPU_RESOURCE_SHARED_TEXTURE_OPTIONS {
 uint64_t blob_id;UINT create_ctx_id,plane_count,texture_layout;uint64_t modifier,allocation_size;
 struct {uint64_t offset,pitch;} planes[4];
 UINT primary,width,height,mip_levels,array_size,format,sample_count,usage,bind_flags,cpu_access_flags,misc_flags;
 struct {UINT width,height,format,strides[4],offsets[4];} ScanoutInfo;
};
struct VIOGPU_CREATE_ALLOCATION_EXCHANGE {UINT Type;VIOGPU_RESOURCE_SHARED_TEXTURE_OPTIONS OptionsShared;uint64_t Size;};
struct D3DDDI_ALLOCATIONINFO {void *pPrivateDriverData;UINT PrivateDriverDataSize;struct {UINT Primary;} Flags;UINT VidPnSourceId;D3DKMT_HANDLE hAllocation;};
struct D3DDDICB_ALLOCATE {uintptr_t hResource;UINT NumAllocations;D3DDDI_ALLOCATIONINFO *pAllocationInfo;};
struct D3DDDICB_DEALLOCATE {UINT NumAllocations;D3DKMT_HANDLE *HandleList;};
// BLIT_CALLBACKS
struct TRITON_DEVICE {
 ID3D11VertexShader *pBlitVS;ID3D11PixelShader *pBlitPS;ID3D11SamplerState *pBlitSampler;ID3D11Buffer *pBlitCB;
 D3D_FEATURE_LEVEL FeatureLevel;BOOL blitInitFailed,blitReady;LONG presentInFlight;UINT uIfVersion;Handle hRTCoreLayer;
 D3D11DDI_CORELAYER_DEVICECALLBACKS *pUMCallbacks;
 ID3D11Device *pDev1;ID3D11DeviceContext *pCtx1;Handle hRTDevice;
 struct {HRESULT (*pfnAllocateCb)(uintptr_t,D3DDDICB_ALLOCATE*);HRESULT (*pfnDeallocateCb)(uintptr_t,D3DDDICB_DEALLOCATE*);} KTCallbacks;
};
using PTRITON_DEVICE=TRITON_DEVICE*;
using ID3D11DeviceContext1=ID3D11DeviceContext;
using DXGI_DDI_HDEVICE=uintptr_t;using DXGI_DDI_HRESOURCE=uintptr_t;
struct DXGI_DDI_ARG_BLT_FLAGS {BOOL Resolve,Stretch,Convert,Present;};
#define D3D11_0_DDI_INTERFACE_VERSION 11
#define InterlockedIncrement(p) (++*(p))
#define InterlockedDecrement(p) (--*(p))
// RESOURCE_STRUCTURE
static HRESULT error;
static unsigned errors,creates,releases,allocations,deallocations,create_calls,fail_create;
static bool fail_export,fail_allocate,partial_allocate,pending;
static unsigned bad_export;
static void *exported;
static VIOGPU_RESOURCE_SHARED_TEXTURE_OPTIONS observed;
// KERNEL_SHARED_VALIDATOR
static HRESULT allocate_cb(uintptr_t,D3DDDICB_ALLOCATE *cb) {
 auto *allocation=(VIOGPU_CREATE_ALLOCATION_EXCHANGE*)cb->pAllocationInfo->pPrivateDriverData;
 observed=allocation->OptionsShared;
 CHECK(IsValidVistaSharedTexture(&observed,allocation->Size),"kernel accepts actual UMD shared allocation metadata");
 CHECK(observed.sample_count==1,"export describes physical single-sample storage");
 if(!fail_allocate || partial_allocate){cb->pAllocationInfo->hAllocation=++allocations;pending=false;}
 return fail_allocate?E_OUTOFMEMORY:S_OK;
}
static HRESULT deallocate_cb(uintptr_t,D3DDDICB_DEALLOCATE *cb){CHECK(cb->NumAllocations==1&&*cb->HandleList,"deallocation identity");++deallocations;return S_OK;}
static void tritonSetError(PTRITON_DEVICE,HRESULT hr){error=hr;++errors;}
static void tritonPresentEnsureRuntimeCtx(PTRITON_DEVICE){}
static void tritonPresentEnsureKernelContext(PTRITON_DEVICE){}
static void tritonPresentLock(PTRITON_DEVICE){}
static void tritonPresentUnlock(PTRITON_DEVICE){}
static void tritonPresentFlushAndGate(PTRITON_DEVICE d,BOOL,BOOL){d->pCtx1->Flush();}
static bool tritonSharedBridgeReleaseImportRes(ID3D11Device*,UINT,UINT){return true;}
bool tritonSharedBridgeExportBlob(void *r,triton_shared_texture_desc *e){
 if(fail_export)return false;
 D3D11_TEXTURE2D_DESC d;((ID3D11Texture2D*)r)->GetDesc(&d);
 CHECK(d.SampleDesc.Count==1,"shared export uses single-sample texture");
 pending=true;exported=r;e->blob_id=1;e->plane_count=1;
 unsigned bytes=d.Format==DXGI_FORMAT_R16G16B16A16_FLOAT?8:4;
 e->planes[0].pitch=d.Width*bytes;e->allocation_size=d.Width*d.Height*bytes;
 switch(bad_export){case 1:e->plane_count=2;break;case 2:e->planes[0].pitch=0;break;case 3:--e->planes[0].pitch;break;case 4:--e->allocation_size;break;case 5:e->texture_layout=3;break;case 6:e->planes[2].pitch=1;break;case 7:e->allocation_size=UINT64_MAX;break;case 8:e->planes[0].offset=UINT64_MAX;break;}
 return true;
}
bool tritonSharedBridgeCancelExportBlob(void *r,uint64_t id){CHECK(r==exported&&id==1,"cancel exported storage identity");pending=false;return true;}
static HRESULT create2d(ID3D11Device *dev,const D3D11_TEXTURE2D_DESC *d,const D3D11_SUBRESOURCE_DATA *data,ID3D11Texture2D **texture){
 ++create_calls;if(fail_create==create_calls){*texture=nullptr;return E_OUTOFMEMORY;}
 D3D11_TEXTURE2D_DESC actual=*d;
 HRESULT hr=dev->CreateTexture2D(&actual,data,texture);if(SUCCEEDED(hr)&&*texture)++creates;return hr;
}
static void internal_copy(ID3D11DeviceContext *c,ID3D11Resource *dst,UINT dsub,UINT x,UINT y,UINT z,ID3D11Resource *src,UINT ssub,const D3D11_BOX *box){
 ID3D11Predicate *p=nullptr;BOOL value;c->GetPredication(&p,&value);
 CHECK(!p,"internal copy disables application predicate");if(p)p->Release();
 c->CopySubresourceRegion(dst,dsub,x,y,z,src,ssub,box);
}
#define ID3D11DeviceContext1_CopySubresourceRegion internal_copy
#define ID3D11Device1_CreateTexture2D create2d
#define ID3D11Device1_CreateBuffer(d,...) (d)->CreateBuffer(__VA_ARGS__)
#define ID3D11Device1_CreateTexture1D(d,...) (d)->CreateTexture1D(__VA_ARGS__)
#define ID3D11Device1_CreateTexture3D(d,...) (d)->CreateTexture3D(__VA_ARGS__)
#define ID3D11Texture2D_Release(r) (++releases,(r)->Release())
#define ID3D11Resource_Release(r) (++releases,(r)->Release())
#define ID3D11DeviceContext1_GetPredication(c,...) (c)->GetPredication(__VA_ARGS__)
#define ID3D11DeviceContext1_SetPredication(c,...) (c)->SetPredication(__VA_ARGS__)
#define ID3D11DeviceContext1_ResolveSubresource(c,...) (c)->ResolveSubresource(__VA_ARGS__)
#define ID3D11Predicate_Release(p) (p)->Release()
#define GetProcessHeap() nullptr
#define HeapAlloc(h,f,n) malloc(n)
#define HeapFree(h,f,p) free(p)
// HOST_FORMAT_IMPLEMENTATION
// RESOURCE_IMPLEMENTATION
// RESOLVE_IMPLEMENTATION
// BLIT_MACROS
extern "C" void *tritonD3D10BlitBytecode(const void *,SIZE_T,SIZE_T *);
#include "tritonBlitShaders.h"
// BLIT_IMPLEMENTATION
static void rotate_pair(PTRITON_RESOURCE a,PTRITON_RESOURCE b){
 uintptr_t handles[2]={(uintptr_t)a,(uintptr_t)b};
 struct {uintptr_t *pResources;} args={handles},*pArgs=&args;UINT i,n=2;
 // ROTATE_IMPLEMENTATION
}
static D3D11DDIARG_CREATERESOURCE arguments(bool primary,UINT samples,DXGI_FORMAT format){
 static Mip mip={16,16,1};D3D11DDIARG_CREATERESOURCE a={};
 a.pMipInfoList=&mip;a.ResourceDimension=D3D10DDIRESOURCE_TEXTURE2D;a.Format=format;
 a.MipLevels=a.ArraySize=1;a.SampleDesc.Count=samples;a.BindFlags=D3D10_DDI_BIND_RENDER_TARGET|D3D10_DDI_BIND_PRESENT;
 a.pPrimaryDesc=primary?&mip:nullptr;return a;
}
static void destroy(PTRITON_DEVICE d,PTRITON_RESOURCE r){tritonDestroyResource({d},{r});CHECK(!r->pResource&&!r->pPresentResource&&!r->hKMAllocation,"destroy clears ownership");}
static void draw_colour(PTRITON_DEVICE d,PTRITON_RESOURCE r,float red,float green){
 ID3D11RenderTargetView *view=nullptr;CHECK(SUCCEEDED(d->pDev1->CreateRenderTargetView(r->pResource,nullptr,&view)),"MSAA RTV");
 float colour[4]={red,green,0,1};d->pCtx1->ClearRenderTargetView(view,colour);view->Release();
}
static void read_colour(PTRITON_DEVICE d,PTRITON_RESOURCE r,UINT expected,UINT tolerance=0){
 tritonResolveForPresent(d,r);
 D3D11_TEXTURE2D_DESC desc;auto tex=(ID3D11Texture2D*)(r->pPresentResource?r->pPresentResource:r->pResource);tex->GetDesc(&desc);
 desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=desc.MiscFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
 ID3D11Texture2D *read=nullptr;CHECK(SUCCEEDED(d->pDev1->CreateTexture2D(&desc,nullptr,&read)),"staging create");
 d->pCtx1->CopyResource(read,tex);D3D11_MAPPED_SUBRESOURCE map;
 CHECK(SUCCEEDED(d->pCtx1->Map(read,0,D3D11_MAP_READ,0,&map)),"resolved map");
 for(UINT y=0;y<desc.Height;++y)for(UINT x=0;x<desc.Width;++x){
  UINT got=((UINT*)((char*)map.pData+y*map.RowPitch))[x];bool equal=true;
  for(UINT shift=0;shift<32;shift+=8)equal&=unsigned(abs(int((got>>shift)&255)-int((expected>>shift)&255)))<=tolerance;
  if(!equal)fprintf(stderr,"pixel got=%08x expected=%08x format=%u samples=%u\n",got,expected,r->Format,r->SampleDesc.Count);
  CHECK(equal,"resolved pixels");
 }
 d->pCtx1->Unmap(read,0);read->Release();
}
static void half_samples(PTRITON_DEVICE d,PTRITON_RESOURCE target){
 CHECK(tritonBlitEnsureLocked(d),"SM4 sample-mask pipeline");
 D3D11_TEXTURE2D_DESC td={};td.Width=td.Height=1;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;
 td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;UINT white=0xffffffff;
 D3D11_SUBRESOURCE_DATA data={&white,4,4};ID3D11Texture2D *source=nullptr;ID3D11ShaderResourceView *srv=nullptr;ID3D11RenderTargetView *rtv=nullptr;
 CHECK(SUCCEEDED(d->pDev1->CreateTexture2D(&td,&data,&source)),"sample-mask source");
 CHECK(SUCCEEDED(d->pDev1->CreateShaderResourceView(source,nullptr,&srv)),"sample-mask SRV");
 D3D11_RENDER_TARGET_VIEW_DESC vd={};vd.Format=target->Format;vd.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2DMS;
 CHECK(SUCCEEDED(d->pDev1->CreateRenderTargetView(target->pResource,&vd,&rtv)),"sample-mask RTV");
 auto c=d->pCtx1;c->ClearState();float black[4]={0,0,0,1},constants[4]={1,1,0,0};c->ClearRenderTargetView(rtv,black);
 c->UpdateSubresource(d->pBlitCB,0,nullptr,constants,0,0);c->OMSetRenderTargets(1,&rtv,nullptr);c->OMSetBlendState(nullptr,nullptr,3);
 D3D11_VIEWPORT vp={0,0,16,16,0,1};c->RSSetViewports(1,&vp);c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
 c->VSSetShader(d->pBlitVS,nullptr,0);c->PSSetShader(d->pBlitPS,nullptr,0);c->PSSetShaderResources(0,1,&srv);c->PSSetSamplers(0,1,&d->pBlitSampler);c->PSSetConstantBuffers(0,1,&d->pBlitCB);
 c->Draw(3,0);c->ClearState();rtv->Release();srv->Release();source->Release();
}
static ID3D11Predicate *skip_predicate(PTRITON_DEVICE d){
 D3D11_QUERY_DESC desc={D3D11_QUERY_OCCLUSION_PREDICATE,0};ID3D11Predicate *p=nullptr;
 CHECK(SUCCEEDED(d->pDev1->CreatePredicate(&desc,&p)),"predicate create");
 d->pCtx1->Begin(p);d->pCtx1->End(p);d->pCtx1->Flush();BOOL value=TRUE;HRESULT hr=S_FALSE;
 for(unsigned i=0;i<1000000&&hr==S_FALSE;++i)hr=d->pCtx1->GetData(p,&value,sizeof(value),0);
 CHECK(hr==S_OK&&!value,"empty predicate resolves false");d->pCtx1->SetPredication(p,FALSE);return p;
}
static void restore_check(PTRITON_DEVICE d,ID3D11Predicate *expected){
 ID3D11Predicate *p=nullptr;BOOL value=TRUE;d->pCtx1->GetPredication(&p,&value);
 CHECK(p==expected&&!value,"internal present preserves predicate");if(p)p->Release();
 d->pCtx1->SetPredication(nullptr,FALSE);expected->Release();
}
int main(int argc,char **argv){
 TRITON_DEVICE dev={};D3D_FEATURE_LEVEL fl=D3D_FEATURE_LEVEL_10_0,actual;
 CHECK(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,&fl,1,D3D11_SDK_VERSION,&dev.pDev1,&actual,&dev.pCtx1)),"host device");
 dev.KTCallbacks.pfnAllocateCb=allocate_cb;dev.KTCallbacks.pfnDeallocateCb=deallocate_cb;
 bool registration=argc>1&&!strcmp(argv[1],"registration");
 if(argc>1&&!strcmp(argv[1],"shared")){
  for(auto format:{DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,DXGI_FORMAT_B8G8R8X8_UNORM,DXGI_FORMAT_B8G8R8X8_UNORM_SRGB,DXGI_FORMAT_R10G10B10A2_UNORM,DXGI_FORMAT_R16G16B16A16_FLOAT})for(UINT bind:{0u,8u,32u,40u}){
   TRITON_RESOURCE r;memset(&r,0xcd,sizeof(r));auto args=arguments(false,1,format);args.BindFlags=bind;args.MiscFlags=D3D10_DDI_RESOURCE_MISC_SHARED;errors=0;
   tritonCreateResource({&dev},&args,{&r},{nullptr});CHECK(!errors&&r.pResource&&r.IsShared,"ordinary shared create");CHECK(!r.BorrowedKMAllocation&&!r.HostFormat,"create clears poisoned ownership and physical format");
   CHECK(observed.format==format&&observed.bind_flags==40&&observed.misc_flags==2,"shared typed format and physical bind metadata");
   unsigned bpp=format==DXGI_FORMAT_R16G16B16A16_FLOAT?8:4;CHECK(observed.planes[0].pitch==16*bpp,"shared physical row width");
   D3D11_TEXTURE2D_DESC desc;((ID3D11Texture2D*)r.pResource)->GetDesc(&desc);CHECK(desc.Format==format,"shared format identity");
   unsigned char pattern[16*16*8];for(unsigned i=0;i<sizeof(pattern);++i)pattern[i]=(unsigned char)(i*17+3);
   dev.pCtx1->UpdateSubresource(r.pResource,0,nullptr,pattern,16*bpp,0);
   desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;desc.BindFlags=desc.MiscFlags=0;
   ID3D11Texture2D *read=nullptr;CHECK(SUCCEEDED(dev.pDev1->CreateTexture2D(&desc,nullptr,&read)),"shared staging create");dev.pCtx1->CopyResource(read,r.pResource);
   D3D11_MAPPED_SUBRESOURCE mapped={};CHECK(SUCCEEDED(dev.pCtx1->Map(read,0,D3D11_MAP_READ,0,&mapped)),"shared staging map");
   for(unsigned row=0;row<16;++row)CHECK(!memcmp((unsigned char*)mapped.pData+row*mapped.RowPitch,pattern+row*16*bpp,16*bpp),"shared raw row content");
   dev.pCtx1->Unmap(read,0);read->Release();destroy(&dev,&r);
  }
  for(bad_export=1;bad_export<=8;++bad_export){TRITON_RESOURCE r={};auto args=arguments(false,1,DXGI_FORMAT_R16G16B16A16_FLOAT);args.BindFlags=40;args.MiscFlags=2;errors=0;
   tritonCreateResource({&dev},&args,{&r},{nullptr});CHECK(errors==1&&error==E_INVALIDARG&&!r.pResource&&!pending,"malformed export rolls back before kernel allocation");
  }bad_export=0;
  CHECK(creates==releases&&allocations==deallocations,"shared metadata failure lifetime");
 }else if(registration){
  for(UINT mode=0;mode<3;++mode)for(UINT failure=0;failure<6;++failure){
   TRITON_RESOURCE r={};auto a=arguments(mode==2,mode?4:1,DXGI_FORMAT_R8G8B8A8_UNORM);
   if(!mode){a.BindFlags=D3D10_DDI_BIND_RENDER_TARGET;a.MiscFlags=D3D10_DDI_RESOURCE_MISC_SHARED;}
   dev.KTCallbacks.pfnAllocateCb=failure==4?nullptr:allocate_cb;
   dev.KTCallbacks.pfnDeallocateCb=failure==5?nullptr:deallocate_cb;
   fail_export=failure==0;fail_allocate=failure==1||failure==2;partial_allocate=failure==2;
   fail_create=failure==3?create_calls+(mode?2:1):0;errors=0;error=S_OK;
   tritonCreateResource({&dev},&a,{&r},{nullptr});
   CHECK(errors==1&&FAILED(error),"void create publishes registration failure");
   CHECK(!r.pResource&&!r.pPresentResource&&!r.hKMAllocation&&!r.IsShared,"failed create rolled back");
   if(fail_allocate||fail_create)CHECK(error==E_OUTOFMEMORY,"original allocation error retained");
   CHECK(creates==releases&&allocations==deallocations&&!pending,"failed create has no owned allocations or pending export");
  }
 }else{
  for(bool primary:{false,true})for(UINT count:{1u,4u})for(auto format:{DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB}){
   TRITON_RESOURCE a={},b={};auto args=arguments(primary,count,format);errors=0;
   tritonCreateResource({&dev},&args,{&a},{nullptr});tritonCreateResource({&dev},&args,{&b},{nullptr});
   CHECK(errors==0&&a.pResource&&b.pResource,"present create");
   D3D11_TEXTURE2D_DESC real;((ID3D11Texture2D*)a.pResource)->GetDesc(&real);
   CHECK(real.SampleDesc.Count==count&&a.SampleDesc.Count==count,"actual render sample count preserved");
   CHECK(observed.sample_count==1&&observed.primary==primary&&observed.format==DXGI_FORMAT_R8G8B8A8_UNORM,"physical presentation descriptor");
   CHECK(observed.misc_flags==D3D11_RESOURCE_MISC_SHARED,"host export-layout hint is not a shared resource property");
   draw_colour(&dev,&a,1,0);draw_colour(&dev,&b,0,1);read_colour(&dev,&a,0xff0000ff);read_colour(&dev,&b,0xff00ff00);
   if(count==4){half_samples(&dev,&a);read_colour(&dev,&a,format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB?0xffbcbcbc:0xff808080,1);draw_colour(&dev,&a,1,0);}
   auto allocation=a.hKMAllocation;auto render=a.pResource;auto present=a.pPresentResource;
   rotate_pair(&a,&b);CHECK(b.pResource==render&&b.pPresentResource==present&&b.hKMAllocation==allocation,"identity tuple rotation");
   read_colour(&dev,&a,0xff00ff00);read_colour(&dev,&b,0xff0000ff);destroy(&dev,&a);destroy(&dev,&b);
  }
  for(UINT samples:{1u,4u}){
   TRITON_RESOURCE src={},dst={};auto sa=arguments(false,4,DXGI_FORMAT_R8G8B8A8_UNORM);
   auto da=arguments(true,samples,DXGI_FORMAT_B8G8R8A8_UNORM);Mip mip={8,8,1};da.pMipInfoList=&mip;
   tritonCreateResource({&dev},&sa,{&src},{nullptr});tritonCreateResource({&dev},&da,{&dst},{nullptr});
   CHECK(src.pResource&&dst.pResource,"MSAA blit resources");draw_colour(&dev,&src,1,0);
   ID3D11Predicate *predicate=skip_predicate(&dev);
   DXGI_DDI_ARG_BLT_FLAGS flags={TRUE,TRUE,TRUE,TRUE};
   CHECK(SUCCEEDED(tritonDxgiBltCommon((uintptr_t)&dev,(uintptr_t)&dst,0,0,0,8,8,(uintptr_t)&src,0,0,0,16,16,flags)),"MSAA resolve/stretch/format blit");
   restore_check(&dev,predicate);read_colour(&dev,&dst,0xffff0000);destroy(&dev,&src);destroy(&dev,&dst);
  }
  {
   TRITON_RESOURCE src={},dst={};auto sa=arguments(false,4,DXGI_FORMAT_R8G8B8A8_UNORM);sa.BindFlags=D3D10_DDI_BIND_RENDER_TARGET;
   auto da=arguments(false,1,DXGI_FORMAT_R8G8B8A8_UNORM);
   tritonCreateResource({&dev},&sa,{&src},{nullptr});tritonCreateResource({&dev},&da,{&dst},{nullptr});
   CHECK(src.pResource&&dst.pResource&&!src.pPresentResource,"ordinary MSAA source");draw_colour(&dev,&src,0,1);
   ID3D11Predicate *predicate=skip_predicate(&dev);
   DXGI_DDI_ARG_BLT_FLAGS flags={TRUE,FALSE,FALSE,TRUE};
   CHECK(SUCCEEDED(tritonDxgiBltCommon((uintptr_t)&dev,(uintptr_t)&dst,0,0,0,16,16,(uintptr_t)&src,0,0,0,16,16,flags)),"temporary resolve + copy");
   restore_check(&dev,predicate);read_colour(&dev,&dst,0xff00ff00);destroy(&dev,&src);destroy(&dev,&dst);
  }
  CHECK(creates==releases&&allocations==deallocations,"resource/resolve allocation lifetime");
 }
 if(dev.pBlitVS)dev.pBlitVS->Release();if(dev.pBlitPS)dev.pBlitPS->Release();
 if(dev.pBlitSampler)dev.pBlitSampler->Release();if(dev.pBlitCB)dev.pBlitCB->Release();
 dev.pCtx1->ClearState();dev.pCtx1->Release();dev.pDev1->Release();puts(registration?"D3D10 export/allocation failure rollback passed":argc>1&&!strcmp(argv[1],"shared")?"D3D10 ordinary shared creation, 8 typed formats, bindings and malformed export rollback passed":"D3D10 native MSAA render/resolve/rotation/stretch/format pixels passed");return 0;
}
