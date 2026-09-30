#ifdef TRITON9_SHARED_ROLLBACK_TEST
/* Execute extracted production registration/rollback branches with injected
 * transport and runtime callbacks. Kernel ownership is modeled independently
 * of COM references: releasing a wrapper never erases a pending export. */
#include <cstdint>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <algorithm>
#include <new>
using UINT = unsigned; using ULONG = unsigned; using DWORD = unsigned;
using UINT64 = uint64_t; using ULONGLONG = uint64_t; using SIZE_T = size_t;
using BOOL = int; using HANDLE = void *; using HRESULT = int32_t;
using DXGI_FORMAT = unsigned; using D3DKMT_HANDLE = unsigned;
constexpr BOOL TRUE=1, FALSE=0;
constexpr HRESULT S_OK=0, E_FAIL=-1, E_INVALIDARG=-2, E_OUTOFMEMORY=-3;
constexpr HRESULT D3DDDIERR_NOTAVAILABLE=-4, D3DDDIERR_DEVICEREMOVED=-5;
constexpr DXGI_FORMAT DXGI_FORMAT_B8G8R8A8_UNORM=87, DXGI_FORMAT_B8G8R8X8_UNORM=88,
 DXGI_FORMAT_R8G8B8A8_UNORM=28,DXGI_FORMAT_R10G10B10A2_UNORM=24,DXGI_FORMAT_R16G16B16A16_FLOAT=10;
constexpr UINT D3D11_USAGE_DEFAULT=0, D3D11_BIND_SHADER_RESOURCE=8,
 D3D11_BIND_RENDER_TARGET=32, D3D11_RESOURCE_MISC_SHARED=2,
 VIOGPU_RESOURCE_TYPE_SHARED=10,VIOGPU_RESOURCE_TYPE_3D=3,
 VIOGPU_RESOURCE_FLAG_STANDARD_PRIMARY=4;
#define FAILED(hr) ((hr)<0)
#define SUCCEEDED(hr) ((hr)>=0)
#define ZeroMemory(p,n) memset(p,0,n)
#define GetProcessHeap() nullptr
static int frees=0;
static std::vector<char> events;
#define HeapFree(h,f,p) (++frees, events.push_back('F'), free(p))
struct ID3D11Resource { int refs=1; };
#define ID3D11Resource_AddRef(p) (++(p)->refs)
#define ID3D11Resource_Release(p) (events.push_back('R'), --(p)->refs)
struct Plane { uint64_t offset,pitch; };
struct triton_shared_texture_desc {
 uint64_t blob_id,modifier,allocation_size; UINT create_ctx_id,plane_count,texture_layout;
 Plane planes[4];
};
struct VIOGPU_RESOURCE_SHARED_TEXTURE_OPTIONS : triton_shared_texture_desc {
 UINT primary,width,height,mip_levels,array_size,format,sample_count,usage,bind_flags,misc_flags;
 struct { UINT width,height,format,strides[4],offsets[4]; } ScanoutInfo;
};
struct VIOGPU_CREATE_ALLOCATION_EXCHANGE {
 UINT Type; uint64_t Size; VIOGPU_RESOURCE_SHARED_TEXTURE_OPTIONS OptionsShared;
 struct { UINT target,format,bind,width,height,depth,array_size,last_level,nr_samples,flags; } Options3D;
};
struct D3DDDI_ALLOCATIONINFO {
 void *pPrivateDriverData; UINT PrivateDriverDataSize,VidPnSourceId;
 struct { UINT Primary; } Flags; D3DKMT_HANDLE hAllocation;
};
struct D3DDDICB_ALLOCATE { HANDLE hResource; UINT NumAllocations; D3DDDI_ALLOCATIONINFO *pAllocationInfo; };
struct D3DDDICB_DEALLOCATE { HANDLE hResource; UINT NumAllocations; D3DKMT_HANDLE *HandleList; };
struct TRITON9_RESOURCE {
 HANDLE hOwnerDevice=nullptr,hRTResource=(HANDLE)0x12;
 D3DKMT_HANDLE hKMAllocation=0;
 UINT64 pendingExportBlob=0;
 ID3D11Resource *pendingExportResource=nullptr,*hostResource=nullptr,*resolveResource=nullptr;
 TRITON9_RESOURCE *failedNext=nullptr;
 BOOL isPrimary=0,needsPresentAllocation=0,isShared=1,independentAllocation=0,
      ownsKMAllocation=0,kmResourceAssociated=0,ownsShadow=0;
 DXGI_FORMAT hostFormat=DXGI_FORMAT_B8G8R8A8_UNORM;
 UINT width=4,height=4,bytesPerPixel=4,mipLevels=1,pitch=16,rowBytes=16,vidPnSourceId=0;
 void *fvfDeclaration=nullptr,*shadow=nullptr,*systemMemorySnapshot=nullptr;
};
struct TRITON9_DEVICE {
 HANDLE hRTDevice=(HANDLE)0x23; BOOL deviceLost=0;
 struct { HRESULT (*pfnAllocateCb)(HANDLE,D3DDDICB_ALLOCATE*);
          HRESULT (*pfnDeallocateCb)(HANDLE,D3DDDICB_DEALLOCATE*); } callbacks;
 TRITON9_RESOURCE *failedResources=nullptr;
};
struct Scenario {
 triton_shared_texture_desc metadata{123,0,64,1,1,0,{{0,16}}};
 HRESULT allocateResult=S_OK,deallocateResult=S_OK;
 D3DKMT_HANDLE allocation=77; bool consume=true,cancelFails=false,exportFails=false;
 bool pending=false,allocated=false,associated=false;
 ID3D11Resource *exportWrapper=nullptr;
 unsigned exports=0,allocates=0,deallocates=0,cancels=0;
};
static Scenario sc;
static unsigned checks=0,failures=0;
#define CHECK(c) do { ++checks; if(!(c)){++failures;printf("[FAIL] rollback line %u: %s\n",__LINE__,#c);} }while(0)
static HRESULT allocate(HANDLE,D3DDDICB_ALLOCATE *cb) {
 ++sc.allocates; events.push_back('A');
 CHECK(cb->NumAllocations==1); CHECK(!sc.allocated);
 cb->pAllocationInfo->hAllocation=sc.allocation;
 sc.allocated=sc.allocation!=0;sc.associated=cb->hResource!=nullptr;
 if(sc.consume)sc.pending=false;
 return sc.allocateResult;
}
static HRESULT deallocate(HANDLE,D3DDDICB_DEALLOCATE *cb) {
 ++sc.deallocates;events.push_back('D'); CHECK(sc.allocated);
 if(sc.associated){CHECK(cb->hResource==(HANDLE)0x12);CHECK(!cb->HandleList&&!cb->NumAllocations);}
 else {CHECK(!cb->hResource);CHECK(cb->NumAllocations==1&&cb->HandleList&&*cb->HandleList==sc.allocation);}
 if(SUCCEEDED(sc.deallocateResult))sc.allocated=false;
 return sc.deallocateResult;
}
static bool tritonSharedBridgeExportBlob(ID3D11Resource *r,triton_shared_texture_desc *out) {
 ++sc.exports;events.push_back('E'); if(sc.exportFails)return false;
 CHECK(!sc.pending);sc.pending=true;sc.exportWrapper=r;*out=sc.metadata;return true;
}
static bool tritonSharedBridgeCancelExportBlob(ID3D11Resource *r,uint64_t blob) {
 ++sc.cancels;events.push_back('C');CHECK(r==sc.exportWrapper);
 if(sc.cancelFails||!blob)return false;
 CHECK(blob==sc.metadata.blob_id);sc.pending=false;return true;
}
static BOOL triton9ResourceBelongsToDevice(TRITON9_DEVICE *d,TRITON9_RESOURCE *r){return d&&r&&r->hOwnerDevice==d;}
static HRESULT triton9EnsureRuntimeContext(TRITON9_DEVICE*){return S_OK;}
static void triton9Diag(const char*){}
static void triton9DiagU32(const char*,DWORD){}
static void triton9ReleaseResourceViews(TRITON9_RESOURCE *r){if(r->resolveResource)ID3D11Resource_Release(r->resolveResource);}
static void triton9DestroyFvfDeclaration(TRITON9_DEVICE*,void*){}
// GENERATED_ROLLBACK_PRODUCTION
static TRITON9_RESOURCE *make(TRITON9_DEVICE &d,ID3D11Resource &wrapper) {
 sc=Scenario{};events.clear();frees=0;wrapper.refs=1;d={};d.callbacks={allocate,deallocate};
 auto r=new (malloc(sizeof(TRITON9_RESOURCE))) TRITON9_RESOURCE; r->hOwnerDevice=&d;r->hostResource=&wrapper;return r;
}
static void disposed(TRITON9_DEVICE &d,TRITON9_RESOURCE *r,ID3D11Resource &w,HRESULT expected) {
 CHECK(triton9DisposeFailedResource(&d,r,expected)==expected);
 CHECK(w.refs==0&&frees==1&&!d.failedResources&&!sc.pending&&!sc.allocated);
}
int main(){
 TRITON9_DEVICE d{};ID3D11Resource wrapper;
 for(unsigned f:{87u,88u,28u,24u,10u})for(bool shortRow:{false,true}){
  auto r=make(d,wrapper);r->hostFormat=f;r->bytesPerPixel=f==10?8:4;
  sc.metadata.planes[0].pitch=4*r->bytesPerPixel;sc.metadata.allocation_size=16*r->bytesPerPixel;
  if(shortRow)--sc.metadata.planes[0].pitch;
  CHECK(triton9RegisterSharedTexture(&d,r)==(shortRow?E_FAIL:S_OK));
  CHECK(shortRow?sc.cancels==1:sc.allocates==1);disposed(d,r,wrapper,E_FAIL);
 }

 for(unsigned metadata=0;metadata<10;++metadata){
  auto r=make(d,wrapper);
  switch(metadata){case 0:sc.metadata.plane_count=0;break;case 1:sc.metadata.plane_count=2;break;
   case 2:sc.metadata.texture_layout=3;break;case 3:sc.metadata.allocation_size=0;break;
   case 4:sc.metadata.allocation_size=UINT64_MAX;break;case 5:sc.metadata.planes[0].pitch=0;break;
   case 6:sc.metadata.planes[0].pitch=15;break;case 7:sc.metadata.planes[0].offset=64;break;
   case 8:sc.metadata.allocation_size=63;break;case 9:sc.metadata.planes[3].pitch=1;break;}
  CHECK(triton9RegisterSharedTexture(&d,r)==E_FAIL);
  CHECK(sc.exports==1&&sc.cancels==1&&!sc.allocates&&!sc.pending&&!d.deviceLost);
  CHECK(!r->pendingExportResource&&!r->pendingExportBlob&&wrapper.refs==1);
  disposed(d,r,wrapper,E_FAIL);
 }
 for(bool partial:{false,true})for(bool consumed:{false,true})for(bool independent:{false,true}){
  auto r=make(d,wrapper);r->independentAllocation=independent;
  sc.allocateResult=E_OUTOFMEMORY;sc.allocation=partial?77:0;sc.consume=consumed;
  CHECK(triton9RegisterSharedTexture(&d,r)==E_OUTOFMEMORY);
  CHECK(sc.deallocates==unsigned(partial)&&sc.cancels==1&&!sc.pending&&!sc.allocated);
  CHECK(!r->hKMAllocation&&!r->ownsKMAllocation&&!r->pendingExportResource&&wrapper.refs==1);
  if(partial)CHECK(std::find(events.begin(),events.end(),'D')<std::find(events.begin(),events.end(),'C'));
  CHECK(std::find(events.begin(),events.end(),'C')<std::find(events.begin(),events.end(),'R'));
  disposed(d,r,wrapper,E_OUTOFMEMORY);
 }
 {auto r=make(d,wrapper);sc.allocation=0;sc.consume=false;
  CHECK(triton9RegisterSharedTexture(&d,r)==E_FAIL);disposed(d,r,wrapper,E_FAIL);}
 {auto r=make(d,wrapper);sc.exportFails=true;
  CHECK(triton9RegisterSharedTexture(&d,r)==E_FAIL);CHECK(!sc.cancels);disposed(d,r,wrapper,E_FAIL);}
 {auto r=make(d,wrapper);CHECK(triton9RegisterSharedTexture(&d,r)==S_OK);
  CHECK(r->hKMAllocation==77&&r->ownsKMAllocation&&!sc.pending&&wrapper.refs==1&&!sc.cancels);
  CHECK(triton9RegisterSharedTexture(&d,r)==D3DDDIERR_NOTAVAILABLE&&sc.exports==1);
  CHECK(tritonSharedBridgeCancelExportBlob(&wrapper,123));CHECK(tritonSharedBridgeCancelExportBlob(&wrapper,123));
  disposed(d,r,wrapper,E_FAIL);}
 for(bool recover:{false,true}){
  auto r=make(d,wrapper);sc.allocateResult=E_OUTOFMEMORY;sc.deallocateResult=E_FAIL;
  CHECK(triton9RegisterSharedTexture(&d,r)==D3DDDIERR_DEVICEREMOVED);
  CHECK(d.deviceLost&&r->hKMAllocation==77&&r->ownsKMAllocation&&sc.allocated&&!sc.pending&&wrapper.refs==1);
  CHECK(triton9DisposeFailedResource(&d,r,E_FAIL)==D3DDDIERR_DEVICEREMOVED);
  CHECK(d.failedResources==r&&r->hKMAllocation==77&&frees==0&&wrapper.refs==1);
  if(recover)sc.deallocateResult=S_OK;
  CHECK(triton9DestroyFailedResources(&d)==(recover?S_OK:D3DDDIERR_DEVICEREMOVED));
  CHECK(!d.failedResources&&frees==1&&wrapper.refs==0&&sc.allocated==!recover);
  sc.allocated=false; // final runtime/KMD device teardown owns this handle
 }
 for(bool recover:{false,true}){
  auto r=make(d,wrapper);sc.metadata.plane_count=2;sc.cancelFails=true;
  CHECK(triton9RegisterSharedTexture(&d,r)==D3DDDIERR_DEVICEREMOVED);
  CHECK(r->pendingExportBlob==123&&r->pendingExportResource==&wrapper&&wrapper.refs==2&&sc.pending);
  CHECK(triton9DisposeFailedResource(&d,r,E_FAIL)==D3DDDIERR_DEVICEREMOVED&&d.failedResources==r);
  if(recover)sc.cancelFails=false;
  CHECK(triton9DestroyFailedResources(&d)==(recover?S_OK:D3DDDIERR_DEVICEREMOVED));
  CHECK(!d.failedResources&&frees==1&&wrapper.refs==0&&sc.pending==!recover);
  sc.pending=false; // renderer/device loss must retire host pending table
 }
 {auto r=make(d,wrapper);sc.metadata.blob_id=0;
  CHECK(triton9RegisterSharedTexture(&d,r)==D3DDDIERR_DEVICEREMOVED&&d.deviceLost);
  CHECK(triton9DisposeFailedResource(&d,r,E_FAIL)==D3DDDIERR_DEVICEREMOVED);
  CHECK(triton9DestroyFailedResources(&d)==D3DDDIERR_DEVICEREMOVED);CHECK(wrapper.refs==0&&!d.failedResources);}
 {auto r=make(d,wrapper);sc.allocateResult=E_OUTOFMEMORY;sc.cancelFails=true;sc.consume=false;
  CHECK(triton9RegisterSharedTexture(&d,r)==D3DDDIERR_DEVICEREMOVED);
  ID3D11Resource msaa;r->hostResource=&msaa;r->resolveResource=&wrapper;
  CHECK(triton9DisposeFailedResource(&d,r,E_FAIL)==D3DDDIERR_DEVICEREMOVED);
  sc.cancelFails=false;CHECK(triton9DestroyFailedResources(&d)==S_OK);
  CHECK(wrapper.refs==0&&msaa.refs==0&&!sc.pending&&!sc.allocated);}
 {auto r=make(d,wrapper);r->isPrimary=true;r->isShared=false;
  sc.allocateResult=E_OUTOFMEMORY;sc.deallocateResult=E_FAIL;
  CHECK(triton9AllocateStandardPrimary(&d,r)==E_OUTOFMEMORY&&r->hKMAllocation==77);
  CHECK(triton9DisposeFailedResource(&d,r,E_OUTOFMEMORY)==D3DDDIERR_DEVICEREMOVED);
  CHECK(d.failedResources==r&&r->hKMAllocation==77);
  sc.deallocateResult=S_OK;CHECK(triton9DestroyFailedResources(&d)==S_OK&&wrapper.refs==0&&!sc.allocated);}
 printf("D3D9 shared rollback: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
#else
/* Production DDI resource functions, real DXVK textures, independent byte/pixel
 * expectations. */
#define TRITON_LINUX_D3D11_NO_MAIN
#include "tests/linux-d3d11-test.cpp"
#include "triton-umd/src/virtio/neptune/triton/tritonBlitShaders.h"
#include "triton-umd/src/virtio/neptune/vista-d3d9/triton9_cpu_layout.h"
#include <d3d11_1.h>
#include <d3d9.h>
#include <algorithm>
#define D3D_UMD_INTERFACE_VERSION 12
#define D3D_UMD_INTERFACE_VERSION_WIN7 0x2000
#define D3D_UMD_INTERFACE_VERSION_WIN8 0x3000
#define D3DDDIERR_WASSTILLDRAWING D3DERR_WASSTILLDRAWING
#define D3DDDIERR_INVALIDCALL D3DERR_INVALIDCALL
#define D3DDDIERR_INVALIDUSERBUFFER D3DERR_INVALIDCALL
#define D3DDDIERR_DEVICEREMOVED D3DERR_DEVICELOST
#define D3DDDIERR_NOTAVAILABLE D3DERR_NOTAVAILABLE
#define D3DDDIMULTISAMPLE_NONE D3DMULTISAMPLE_NONE
#define D3DDDIMULTISAMPLE_NONMASKABLE D3DMULTISAMPLE_NONMASKABLE
#define D3DDDIMULTISAMPLE_16_SAMPLES D3DMULTISAMPLE_16_SAMPLES
#define D3DDDITEXF_LINEAR D3DTEXF_LINEAR
#define D3DDDITEXF_POINT D3DTEXF_POINT
#define D3DDDITEXF_ANISOTROPIC D3DTEXF_ANISOTROPIC
#define D3DCLEAR_COMPUTERECTS 8
#define APIENTRY
enum triton9_clear_contract_result {
  TRITON9_CLEAR_CONTRACT_OK,
  TRITON9_CLEAR_CONTRACT_INVALID_ARGUMENT,
  TRITON9_CLEAR_CONTRACT_INSUFFICIENT_CAPACITY
};
#define TRITON9_MAX_RENDER_TARGETS 4
#define TRITON9_MAX_TEXTURE_STAGES 21
#define TRITON9_MAX_PIXEL_SAMPLERS 16
#define TRITON9_MAX_VERTEX_SAMPLERS 4
#define TRITON9_VERTEX_SAMPLER_BASE 17
#define D3DDDITSS_SRGBTEXTURE 11
#define D3DDDIRS_DITHERENABLE D3DRS_DITHERENABLE
#define D3DDDIRS_BLENDFACTOR D3DRS_BLENDFACTOR
#define D3DDDIRS_MULTISAMPLEMASK D3DRS_MULTISAMPLEMASK
#define D3DDDIRS_STENCILREF D3DRS_STENCILREF
#define TRITON9_HOST_DRAIN_TIMEOUT_MS 15000
#define D3DDDIRS_SRGBWRITEENABLE D3DRS_SRGBWRITEENABLE
using D3DDDIFORMAT = D3DFORMAT;
using D3DKMT_HANDLE = UINT;
using D3DDDI_POOL = UINT;
using D3DDDIMULTISAMPLE_TYPE = D3DMULTISAMPLE_TYPE;
using D3DDDI_VIDEO_PRESENT_SOURCE_ID = UINT;
struct D3DDDI_RATIONAL {
  UINT Numerator, Denominator;
};
enum {
  D3DDDIPOOL_SYSTEMMEM = 1,
  D3DDDIPOOL_VIDEOMEMORY = 2,
  D3DDDIPOOL_LOCALVIDMEM = 3,
  D3DDDIPOOL_NONLOCALVIDMEM = 4
};
struct FORMATOP {
  D3DDDIFORMAT Format;
  UINT Operations, FlipMsTypes, BltMsTypes, PrivateFormatBitCount;
};
struct D3DDDIARG_GENERATEMIPSUBLEVELS {
  HANDLE hResource;
  UINT Filter;
};
// GENERATED_CONSTANTS
// GENERATED_TYPES
struct TRITON9_DEVICE {
  TRITON9_RESOURCE *failedResources = nullptr;
  TRITON9_RESOURCE *textures[21] = {};
  UINT textureStageStates[21][35] = {};
  ID3D11BlendState *blendState = nullptr;
  ID3D11DepthStencilState *depthStencilState = nullptr;
  ID3D11RasterizerState *rasterizerState = nullptr;
  ID3D11SamplerState *samplerStates[21] = {};
  BOOL blendStateDirty = FALSE, rasterizerStateDirty = FALSE;
  ID3D11Device1 *hostDevice;
  ID3D11DeviceContext1 *hostContext;
  BOOL hostDitherEnabled = FALSE;
  BOOL deviceLost = FALSE;
  int shaderLock = 0;
  TRITON9_RESOURCE *renderTarget = nullptr, *renderTargets[4] = {},
                   *depthStencil = nullptr;
  DWORD renderStates[256] = {};
  void *stretchBlitState = nullptr;
  BOOL shaderLockInitialized = TRUE, depthStencilStateDirty = FALSE,
       viewportSet = FALSE, scissorSet = FALSE;
  D3D11_VIEWPORT viewport = {};
  RECT scissorRect = {};
};
struct TRITON9_RENAME_COOKIE {
  TRITON9_RESOURCE *resource;
};
// GENERATED_HELPERS
static int allocationCountdown = -1;
struct Allocation {
  void *p;
  template <class T> operator T *() const { return (T *)p; }
};
static Allocation testHeap(size_t size) {
  if (allocationCountdown == 0)
    return {nullptr};
  if (allocationCountdown > 0)
    --allocationCountdown;
  return {calloc(1, size)};
}
#define GetProcessHeap() nullptr
#define HEAP_ZERO_MEMORY 0
#define HeapAlloc(h, f, s) testHeap(s)
#define HeapFree(h, f, p) free(p)
#define EnterCriticalSection(x) ((void)0)
#define LeaveCriticalSection(x) ((void)0)

static HRESULT injectedHostStatus = S_OK;
static HRESULT triton9CheckHostDevice(TRITON9_DEVICE *d) {
  if (FAILED(injectedHostStatus)) return injectedHostStatus;
  return d->hostDevice->GetDeviceRemovedReason();
}
static HRESULT triton9MapDeviceFailure(TRITON9_DEVICE *, HRESULT h) {
  return h;
}
static HRESULT triton9EnsureHostDevice(TRITON9_DEVICE *) { return S_OK; }
static HRESULT triton9CreateFvfDeclaration(TRITON9_DEVICE *, UINT, void **) {
  return E_NOTIMPL;
}
static void triton9DestroyFvfDeclaration(TRITON9_DEVICE *, void *) {}
static HRESULT triton9DeallocateResource(TRITON9_DEVICE *, TRITON9_RESOURCE *) {
  return S_OK;
}
static HRESULT triton9OpenStandardPrimaryHost(TRITON9_DEVICE *,
                                              TRITON9_RESOURCE *) {
  return E_FAIL;
}
static HRESULT triton9RegisterSharedTexture(TRITON9_DEVICE *,
                                            TRITON9_RESOURCE *) {
  return E_FAIL;
}
static void *npt_shared_texture_create_exportable(ID3D11Device1 *, UINT, UINT,
                                                  DXGI_FORMAT) {
  return nullptr;
}
static DXGI_FORMAT npt_shared_texture_host_format(DXGI_FORMAT f) { return f; }
#include "triton-umd/src/virtio/neptune/triton/tritonDitherControl.h"
static int32_t tritonSharedBridgeSetDither(void *context, uint32_t enabled) {
  auto *ctx = static_cast<ID3D11DeviceContext1 *>(context);
  const GUID capsGuid = TRITON_DITHER_CAPS_GUID, stateGuid = TRITON_DITHER_STATE_GUID;
  TritonDitherCaps caps = {}; UINT bytes = sizeof(caps);
  HRESULT hr = ctx->GetPrivateData(capsGuid, &bytes, &caps);
  if (FAILED(hr) || caps.version != TRITON_DITHER_VERSION || !(caps.flags & TRITON_DITHER_NATIVE))
    return enabled ? DXGI_ERROR_UNSUPPORTED : S_OK;
  TritonDitherState state = {TRITON_DITHER_VERSION, enabled};
  return ctx->SetPrivateData(stateGuid, sizeof(state), &state);
}

static bool tritonSharedBridgeDrain(ID3D11DeviceContext1 *, UINT) {
  return true;
}
static void triton9Diag(const char *) {}
static HRESULT triton9AllocateStandardPrimary(TRITON9_DEVICE *,
                                              TRITON9_RESOURCE *) {
  return E_FAIL;
}
static void triton9DiagU32(const char *, DWORD) {}
#define GetTickCount() 0
#define InterlockedIncrement(p) (++*(p))
static void triton9ProofDiagU32(const char *, DWORD) {}
static HRESULT
npt_dispatch_clear_depth_stencil_rects(void *context, void *dsv, uint32_t flags,
                                       float depth, uint8_t stencil,
                                       uint32_t count, const int32_t *rects) {
  return dxvk_d3d11_clear_depth_stencil_rects(context, dsv, flags, depth,
                                              stencil, count, rects);
}
const TRITON9_FORMAT *triton9FormatLookup(D3DDDIFORMAT);
UINT triton9FormatCount(void);
UINT triton9FormatMultisampleQuality(D3DDDIFORMAT, UINT);
static HRESULT triton9CreateBlendState(TRITON9_DEVICE *) { return S_OK; }
static HRESULT triton9CreateDepthStencilState(TRITON9_DEVICE *) { return S_OK; }
static HRESULT triton9CreateRasterizerState(TRITON9_DEVICE *) { return S_OK; }
static HRESULT triton9CreateSamplerState(TRITON9_DEVICE *d, UINT stage) {
  if(d->samplerStates[stage]) return S_OK;
  D3D11_SAMPLER_DESC desc={};desc.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;
  desc.AddressU=desc.AddressV=desc.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
  desc.MaxLOD=D3D11_FLOAT32_MAX;
  return d->hostDevice->CreateSamplerState(&desc,&d->samplerStates[stage]);
}
static HRESULT triton9ValidateViewport(TRITON9_DEVICE *d,D3D11_VIEWPORT *v,RECT *r) {
  *v={0,0,(FLOAT)d->renderTarget->width,(FLOAT)d->renderTarget->height,0,1};
  *r={0,0,(LONG)d->renderTarget->width,(LONG)d->renderTarget->height};return S_OK;
}
static int triton9RenderTargetColorLayout(TRITON9_RESOURCE *) { return 0; }
static BOOL triton9CpuDitherStateSupported(UINT,int) { return TRUE; }
struct BufferUpload { ID3D11Resource *resource; UINT left, right; };
static std::vector<BufferUpload> bufferUploads;
static bool suppressBufferForward;
static void recordUpdate(ID3D11DeviceContext *context, ID3D11Resource *resource,
                         UINT subresource, const D3D11_BOX *box, const void *source,
                         UINT rowPitch, UINT depthPitch) {
  D3D11_RESOURCE_DIMENSION dimension;
  resource->GetType(&dimension);
  if (dimension == D3D11_RESOURCE_DIMENSION_BUFFER) {
    D3D11_BUFFER_DESC desc;
    static_cast<ID3D11Buffer *>(resource)->GetDesc(&desc);
    bufferUploads.push_back({resource, box ? box->left : 0, box ? box->right : desc.ByteWidth});
    if (suppressBufferForward) return;
  }
  context->UpdateSubresource(resource, subresource, box, source, rowPitch, depthPitch);
}
static bool auditGpuCopies;
static UINT auditedCopies, cpuCopyIntermediates;
static void recordCopy(ID3D11DeviceContext1 *context, ID3D11Resource *destination,
                       UINT destinationSubresource, UINT x, UINT y, UINT z,
                       ID3D11Resource *source, UINT sourceSubresource,
                       const D3D11_BOX *box) {
  if (auditGpuCopies) {
    ++auditedCopies;
    for (auto resource : {source, destination}) {
      D3D11_RESOURCE_DIMENSION dimension;
      resource->GetType(&dimension);
      UINT usage = UINT_MAX, access = UINT_MAX;
      if (dimension == D3D11_RESOURCE_DIMENSION_BUFFER) {
        D3D11_BUFFER_DESC desc;
        static_cast<ID3D11Buffer *>(resource)->GetDesc(&desc);
        usage = desc.Usage; access = desc.CPUAccessFlags;
      } else if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC desc;
        static_cast<ID3D11Texture2D *>(resource)->GetDesc(&desc);
        usage = desc.Usage; access = desc.CPUAccessFlags;
      } else if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D) {
        D3D11_TEXTURE3D_DESC desc;
        static_cast<ID3D11Texture3D *>(resource)->GetDesc(&desc);
        usage = desc.Usage; access = desc.CPUAccessFlags;
      }
      if (usage != D3D11_USAGE_DEFAULT || access) ++cpuCopyIntermediates;
    }
  }
  context->CopySubresourceRegion(destination, destinationSubresource, x, y, z,
                                 source, sourceSubresource, box);
}
// GENERATED_PRODUCTION

static void releaseTexture(TRITON9_RESOURCE *r) {
  if (!r)
    return;
  auto table = r->textureSurfaces;
  UINT count = table ? r->surfaceCount : 1;
  for (UINT i = count; i-- > 0;) {
    auto t = table ? table[i] : r;
    triton9ReleaseResourceViews(t);
    if (t->systemMemorySnapshot) free(t->systemMemorySnapshot);
    if (t->hostResource)
      t->hostResource->Release();
    if (t->ownsShadow)
      free(t->shadow);
    free(t);
  }
  free(table);
}
static TRITON9_RESOURCE *texture(TRITON9_DEVICE &d, D3DFORMAT format, UINT w,
                                 UINT h, UINT depth, UINT mips, UINT faces,
                                 bool rt = false) {
  std::vector<D3DDDI_SURFACEINFO> info(mips * faces);
  for (UINT f = 0; f < faces; f++)
    for (UINT m = 0; m < mips; m++)
      info[f * mips + m] = {std::max(1u, w >> m),
                            std::max(1u, h >> m),
                            std::max(1u, depth >> m),
                            nullptr,
                            0,
                            0};
  D3DDDIARG_CREATERESOURCE a = {};
  a.Format = format;
  a.Pool = D3DDDIPOOL_VIDEOMEMORY;
  a.pSurfList = info.data();
  a.SurfCount = info.size();
  a.MipLevels = mips;
  a.Flags.Texture = 1;
  a.Flags.CubeMap = faces == 6;
  a.Flags.Volume = depth > 1;
  a.Flags.RenderTarget = rt;
  HRESULT hr = triton9CreateTextureResource(&d, &a);
  CHECK("create", SUCCEEDED(hr));
  return SUCCEEDED(hr) ? (TRITON9_RESOURCE *)a.hResource : nullptr;
}
static void shapes(TRITON9_DEVICE &d) {
  const char *T = "subresources";
  for (UINT faces : {1u, 6u}) {
    auto r = texture(d, D3DFMT_A8R8G8B8, 8, 8, 1, 4, faces, true);
    CHECK(T, r, return);
    CHECK_HR(T, triton9EnsureResourceHost(&d, r));
    for (UINT i = 0; i < r->surfaceCount; i++) {
      auto a = r->textureSurfaces[i];
      CHECK(T, a->hostResource == r->hostResource && a->subresourceIndex == i);
      uint32_t expected = 0xff000000 | ((i + 1) * 0x010307);
      for (UINT y = 0; y < a->height; y++)
        for (UINT x = 0; x < a->width; x++)
          memcpy(a->shadow + y * a->pitch + x * 4, &expected, 4);
      CHECK_HR(T, triton9UploadShadow(&d, a));
    }
    for (UINT i = 0; i < r->surfaceCount; i++) {
      auto a = r->textureSurfaces[i];
      memset(a->shadow, 0, a->shadowSize);
      CHECK_HR(T, triton9ReadbackShadow(&d, a));
      uint32_t expected = 0xff000000 | ((i + 1) * 0x010307);
      bool ok = true;
      for (UINT y = 0; y < a->height; y++)
        for (UINT x = 0; x < a->width; x++) {
          uint32_t p;
          memcpy(&p, a->shadow + y * a->pitch + x * 4, 4);
          if (p != expected)
            ok = false;
        }
      CHECK(T, ok);
      ID3D11RenderTargetView *v = nullptr;
      CHECK_HR(T, triton9GetRenderTargetView(&d, a, &v));
      const float green[] = {0, 1, 0, 1};
      d.hostContext->ClearRenderTargetView(v, green);
      CHECK_HR(T, triton9ReadbackShadow(&d, a));
      uint32_t p;
      memcpy(&p, a->shadow, 4);
      CHECK(T, p == 0xff00ff00u);
    }
    ID3D11ShaderResourceView *v = nullptr;
    CHECK_HR(T, triton9GetShaderResourceViewEx(&d, r, FALSE, &v));
    D3D11_SHADER_RESOURCE_VIEW_DESC vd;
    v->GetDesc(&vd);
    CHECK(T, vd.ViewDimension == (faces == 6 ? D3D11_SRV_DIMENSION_TEXTURECUBE
                                             : D3D11_SRV_DIMENSION_TEXTURE2D));
    CHECK_HR(T, triton9GetShaderResourceViewEx(&d, r, TRUE, &v));
    v->GetDesc(&vd);
    CHECK(T, vd.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
    releaseTexture(r);
  }
}
static void volume(TRITON9_DEVICE &d) {
  const char *T = "volume";
  auto r = texture(d, D3DFMT_A8B8G8R8, 8, 4, 4, 4, 1);
  CHECK(T, r, return);
  CHECK_HR(T, triton9EnsureResourceHost(&d, r));
  for (UINT i = 0; i < 4; i++) {
    auto a = r->textureSurfaces[i];
    memset(a->shadow, 0x20 + i, a->shadowSize);
    CHECK_HR(T, triton9UploadShadow(&d, a));
    memset(a->shadow, 0, a->shadowSize);
    CHECK_HR(T, triton9ReadbackShadow(&d, a));
    bool ok = true;
    for (size_t b = 0; b < a->shadowSize; b++)
      ok &= a->shadow[b] == 0x20 + i;
    CHECK(T, ok);
  }
  D3D11_BOX box = {2, 1, 1, 6, 3, 3};
  memset(r->shadow, 0xa5, r->shadowSize);
  r->lockBoxValid = TRUE;
  r->lockBox = {2, 1, 6, 3, 1, 3};
  CHECK_HR(T, triton9UploadLockedShadow(&d, r));
  CHECK_HR(T, triton9ReadbackShadow(&d, r));
  bool ok = true;
  for (UINT z = 0; z < 4; z++)
    for (UINT y = 0; y < 4; y++)
      for (UINT x = 0; x < 8; x++)
        ok &= r->shadow[z * r->slicePitch + y * r->pitch + x * 4] ==
              ((z >= 1 && z < 3 && y >= 1 && y < 3 && x >= 2 && x < 6) ? 0xa5
                                                                       : 0x20);
  CHECK(T, ok);
  releaseTexture(r);
}
static void compressed(TRITON9_DEVICE &d) {
  const char *T = "block_layout";
  for (auto f : {D3DFMT_DXT1, D3DFMT_DXT3, D3DFMT_DXT5}) {
    auto r = texture(d, f, 8, 8, 1, 4, 6);
    CHECK(T, r, return);
    CHECK_HR(T, triton9EnsureResourceHost(&d, r));
    UINT bytes = f == D3DFMT_DXT1 ? 8 : 16;
    for (UINT i = 0; i < r->surfaceCount; i++) {
      auto a = r->textureSurfaces[i];
      UINT row = ((a->width + 3) / 4) * bytes;
      CHECK(T,
            a->rowBytes == row && a->shadowSize == row * ((a->height + 3) / 4));
      memset(a->shadow, 0x32 + i, a->shadowSize);
      CHECK_HR(T, triton9UploadShadow(&d, a));
      memset(a->shadow, 0, a->shadowSize);
      CHECK_HR(T, triton9ReadbackShadow(&d, a));
      bool ok = true;
      for (size_t k = 0; k < a->shadowSize; k++)
        ok &= a->shadow[k] == 0x32 + i;
      CHECK(T, ok);
    }
    D3D11_BOX bad = {1, 0, 0, 4, 4, 1}, good = {4, 4, 0, 8, 8, 1};
    CHECK(T, !triton9ResourceBoxValid(r, &bad) &&
                 triton9ResourceBoxValid(r, &good));
    releaseTexture(r);
  }
}
static void copying(TRITON9_DEVICE &d) {
  const char *T = "copy_mip";
  auto a = texture(d, D3DFMT_A8R8G8B8, 8, 8, 1, 4, 1);
  auto b = texture(d, D3DFMT_A8R8G8B8, 8, 8, 1, 4, 1);
  CHECK(T, a && b, return);
  CHECK_HR(T, triton9EnsureResourceHost(&d, a));
  CHECK_HR(T, triton9EnsureResourceHost(&d, b));
  auto src = a->textureSurfaces[1], dst = b->textureSurfaces[2];
  memset(src->shadow, 0x73, src->shadowSize);
  CHECK_HR(T, triton9UploadShadow(&d, src));
  D3D11_BOX box = {1, 1, 0, 3, 3, 1};
  CHECK_HR(T, triton9CopyBox(&d, src, dst, &box, 0, 0, 0, FALSE));
  CHECK_HR(T, triton9ReadbackShadow(&d, dst));
  bool ok = true;
  for (size_t i = 0; i < dst->shadowSize; i++)
    ok &= dst->shadow[i] == 0x73;
  CHECK(T, ok);
  CHECK(T, FAILED(triton9CopyBox(&d, src, dst, &box, 1, 1, 0, FALSE)));
  releaseTexture(a);
  releaseTexture(b);
}
static void overlappingCopies(TRITON9_DEVICE &d) {
  const char *T = "gpu_overlap_copy";
  // Mip/face selection and both overlap directions must preserve all peers.
  for (UINT shape : {0u, 1u, 2u}) {
    auto root = texture(d, D3DFMT_A8B8G8R8, 8, 8,
                        shape == 2 ? 4 : 1, 3, shape == 1 ? 6 : 1);
    CHECK(T, root, return);
    CHECK_HR(T, triton9EnsureResourceHost(&d, root));
    std::vector<std::vector<BYTE>> expected(root->surfaceCount);
    for (UINT i = 0; i < root->surfaceCount; ++i) {
      auto r = root->textureSurfaces[i];
      expected[i].resize(r->shadowSize);
      for (SIZE_T b = 0; b < r->shadowSize; ++b)
        expected[i][b] = BYTE(b * 17 + i * 31);
      memcpy(r->shadow, expected[i].data(), r->shadowSize);
      CHECK_HR(T, triton9UploadShadow(&d, r));
    }
    const UINT selected = shape == 1 ? 7 : 1;
    auto r = root->textureSurfaces[selected];
    for (UINT reverse : {0u, 1u}) {
      const UINT sz = r->depth > 1 ? reverse : 0;
      const UINT dz = r->depth > 1 ? 1 - reverse : 0;
      D3D11_BOX box = {reverse, reverse, sz, r->width - 1 + reverse,
                       r->height - 1 + reverse, r->depth > 1 ? sz + 1 : 1};
      const UINT x = 1 - reverse, y = 1 - reverse;
      auto prior = expected[selected];
      for (UINT z = 0; z < box.back - box.front; ++z)
        for (UINT row = 0; row < box.bottom - box.top; ++row)
          memcpy(expected[selected].data() + (dz + z) * r->slicePitch +
                     (y + row) * r->pitch + x * 4,
                 prior.data() + (box.front + z) * r->slicePitch +
                     (box.top + row) * r->pitch + box.left * 4,
                 (box.right - box.left) * 4);
      auditedCopies = cpuCopyIntermediates = 0; auditGpuCopies = true;
      HRESULT hr = triton9CopyBox(&d, r, r, &box, x, y, dz, FALSE);
      auditGpuCopies = false;
      CHECK_HR(T, hr);
      CHECK(T, auditedCopies == 2 && cpuCopyIntermediates == 0);
      // Only the oracle may read the GPU result back to CPU memory.
      for (UINT i = 0; i < root->surfaceCount; ++i) {
        auto surface = root->textureSurfaces[i];
        CHECK_HR(T, triton9ReadbackShadow(&d, surface));
        CHECK(T, !memcmp(surface->shadow, expected[i].data(), surface->shadowSize));
      }
    }
    D3D11_BOX bad = {0, 0, 0, r->width, r->height, 1};
    auditedCopies = 0; auditGpuCopies = true;
    HRESULT hr = triton9CopyBox(&d, r, r, &bad, 1, 0, 0, FALSE);
    auditGpuCopies = false;
    CHECK(T, FAILED(hr) && auditedCopies == 0);
    releaseTexture(root);
  }
  auto compressed = texture(d, D3DFMT_DXT1, 16, 8, 1, 1, 1);
  CHECK(T, compressed, return);
  CHECK_HR(T, triton9EnsureResourceHost(&d, compressed));
  std::vector<BYTE> blocks(compressed->shadowSize);
  for (SIZE_T i = 0; i < blocks.size(); ++i) blocks[i] = BYTE(i * 13 + 41);
  memcpy(compressed->shadow, blocks.data(), blocks.size());
  CHECK_HR(T, triton9UploadShadow(&d, compressed));
  for (UINT reverse : {0u, 1u}) {
    const UINT sourceX = reverse ? 4 : 0, destinationX = reverse ? 0 : 4;
    auto prior = blocks;
    for (UINT row = 0; row < 2; ++row)
      memcpy(blocks.data() + row * compressed->pitch + destinationX * 2,
             prior.data() + row * compressed->pitch + sourceX * 2, 24);
    D3D11_BOX box = {sourceX, 0, 0, sourceX + 12, 8, 1};
    auditedCopies = cpuCopyIntermediates = 0; auditGpuCopies = true;
    HRESULT hr = triton9CopyBox(&d, compressed, compressed, &box,
                               destinationX, 0, 0, FALSE);
    auditGpuCopies = false;
    CHECK_HR(T, hr);
    CHECK(T, auditedCopies == 2 && cpuCopyIntermediates == 0);
    CHECK_HR(T, triton9ReadbackShadow(&d, compressed));
    CHECK(T, !memcmp(compressed->shadow, blocks.data(), blocks.size()));
  }
  releaseTexture(compressed);
  D3DDDI_SURFACEINFO info = {96, 1, 1, nullptr, 0, 0};
  D3DDDIARG_CREATERESOURCE create = {};
  create.Format = D3DFMT_VERTEXDATA; create.Pool = D3DDDIPOOL_VIDEOMEMORY;
  create.pSurfList = &info; create.SurfCount = 1; create.Flags.VertexBuffer = 1;
  CHECK_HR(T, triton9CreateSingleResource(&d, &create, FALSE));
  auto buffer = (TRITON9_RESOURCE *)create.hResource;
  CHECK(T, buffer, return);
  CHECK_HR(T, triton9EnsureResourceHost(&d, buffer));
  std::vector<BYTE> expected(buffer->width);
  for (UINT i = 0; i < buffer->width; ++i) expected[i] = BYTE(i * 7 + 19);
  memcpy(buffer->shadow, expected.data(), expected.size());
  CHECK_HR(T, triton9UploadShadow(&d, buffer));
  for (UINT reverse : {0u, 1u}) {
    D3DDDIARG_BUFFERBLT copy = {};
    copy.hSrcResource = copy.hDstResource = buffer;
    copy.SrcRange.Offset = reverse ? 16 : 3; copy.SrcRange.Size = 63;
    copy.Offset = reverse ? 3 : 16;
    memmove(expected.data() + copy.Offset, expected.data() + copy.SrcRange.Offset, 63);
    auditedCopies = cpuCopyIntermediates = 0; auditGpuCopies = true;
    HRESULT hr = triton9BufBlt(&d, &copy);
    auditGpuCopies = false;
    CHECK_HR(T, hr);
    CHECK(T, auditedCopies == 2 && cpuCopyIntermediates == 0);
    CHECK_HR(T, triton9ReadbackShadow(&d, buffer));
    CHECK(T, !memcmp(buffer->shadow, expected.data(), expected.size()));
    copy.Offset = 90;
    auditedCopies = 0; auditGpuCopies = true;
    hr = triton9BufBlt(&d, &copy);
    auditGpuCopies = false;
    CHECK(T, FAILED(hr) && auditedCopies == 0);
  }
  releaseTexture(buffer);
}
static void multisample(TRITON9_DEVICE &d) {
  const char *T = "msaa_resolve";
  for (UINT samples : {2u, 4u}) {
    TRITON9_RESOURCE r = {};
    r.hOwnerDevice = &d;
    r.format = D3DFMT_A8R8G8B8;
    r.hostFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    r.width = r.height = 8;
    r.depth = r.mipLevels = r.arraySize = r.surfaceCount = 1;
    r.sampleCount = samples;
    r.pool = D3DDDIPOOL_VIDEOMEMORY;
    r.wantsRenderTarget = TRUE;
    r.bytesPerPixel = 4;
    CHECK_HR(T, triton9CreateShadow(&r, nullptr));
    CHECK_HR(T, triton9EnsureResourceHost(&d, &r));
    ID3D11RenderTargetView *v = nullptr;
    CHECK_HR(T, triton9GetRenderTargetView(&d, &r, &v));
    const float c[] = {1, 0, 0, 1};
    d.hostContext->ClearRenderTargetView(v, c);
    D3D11_BOX full = {0, 0, 0, r.width, r.height, 1};
    auditedCopies = cpuCopyIntermediates = 0; auditGpuCopies = true;
    HRESULT hr = triton9CopyBox(&d, &r, &r, &full, 0, 0, 0, FALSE);
    auditGpuCopies = false;
    CHECK_HR(T, hr);
    CHECK(T, auditedCopies == 2 && cpuCopyIntermediates == 0);
    CHECK_HR(T, triton9ReadbackShadow(&d, &r));
    bool ok = true;
    for (UINT y = 0; y < 8; y++)
      for (UINT x = 0; x < 8; x++) {
        uint32_t p;
        memcpy(&p, r.shadow + y * r.pitch + x * 4, 4);
        ok &= p == 0xffff0000u;
      }
    CHECK(T, ok);
    triton9ReleaseResourceViews(&r);
    r.hostResource->Release();
    free(r.shadow);
  }
}
static void formats(TRITON9_DEVICE &d) {
  const char *T = "formats";
  for (UINT i = 0; i < triton9FormatCount(); i++) {
    const auto &f = g_formats[i];
    if (f.depthStencil)
      continue;
    auto r = texture(d, f.d3dFormat, 8, 8, 1, 1, 1,
                     (f.operations & FORMATOP_OFFSCREEN_RENDERTARGET) != 0);
    CHECK(T, r, continue);
    HRESULT hr = triton9EnsureResourceHost(&d, r);
    if (FAILED(hr))
      emit("[FORMAT] %u hr=%08x", f.d3dFormat, (unsigned)hr);
    CHECK(T, SUCCEEDED(hr));
    if (SUCCEEDED(hr)) {
      ID3D11ShaderResourceView *v;
      CHECK_HR(T, triton9GetShaderResourceView(&d, r, &v));
    }
    releaseTexture(r);
  }
}
static void typedSrgb(TRITON9_DEVICE &d) {
  const char *T = "typed_srgb";
  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = desc.Height = 8;
  desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  Com<ID3D11Texture2D> t;
  CHECK_HR(T, d.hostDevice->CreateTexture2D(&desc, nullptr, &t));
  D3D11_RENDER_TARGET_VIEW_DESC vd = {};
  vd.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
  vd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
  Com<ID3D11RenderTargetView> v;
  HRESULT hr = d.hostDevice->CreateRenderTargetView(t, &vd, &v);
  emit("[TYPED-SRGB] %08x", (unsigned)hr);
  CHECK(T, SUCCEEDED(hr));
}
static void renameAndFailures(TRITON9_DEVICE &d) {
  const char *T = "rename_transaction";
  auto r = texture(d, D3DFMT_A8R8G8B8, 8, 8, 1, 4, 6, true);
  CHECK(T, r, return);
  CHECK_HR(T, triton9EnsureResourceHost(&d, r));
  auto selected = r->textureSurfaces[9];
  ID3D11RenderTargetView *view = nullptr;
  ID3D11ShaderResourceView *srv = nullptr;
  CHECK_HR(T, triton9GetRenderTargetView(&d, selected, &view));
  CHECK_HR(T, triton9GetShaderResourceView(&d, r, &srv));
  auto before = r->hostResource;
  before->AddRef();
  memset(selected->shadow, 0x63, selected->shadowSize);
  auto cookie =
      (TRITON9_RENAME_COOKIE *)calloc(1, sizeof(TRITON9_RENAME_COOKIE));
  cookie->resource = selected;
  selected->pendingRename = cookie;
  CHECK_HR(T, triton9RenameTexture(&d, selected, cookie));
  CHECK(T, r->hostResource != before);
  for (UINT i = 0; i < r->surfaceCount; i++)
    CHECK(T, r->textureSurfaces[i]->hostResource == r->hostResource);
  CHECK_HR(T, triton9ReadbackShadow(&d, selected));
  bool ok = true;
  for (size_t i = 0; i < selected->shadowSize; i++)
    ok &= selected->shadow[i] == 0x63;
  CHECK(T, ok);
  before->Release();
  before = r->hostResource;
  auto oldView = selected->renderTargetView;
  cookie = (TRITON9_RENAME_COOKIE *)calloc(1, sizeof(TRITON9_RENAME_COOKIE));
  cookie->resource = selected;
  selected->pendingRename = cookie;
  allocationCountdown = 1;
  HRESULT hr = triton9RenameTexture(&d, selected, cookie);
  allocationCountdown = -1;
  CHECK(T, hr == E_OUTOFMEMORY && r->hostResource == before &&
               selected->renderTargetView == oldView &&
               !selected->pendingRename);
  CHECK_HR(T, triton9ReadbackShadow(&d, selected));
  CHECK(T, selected->shadow[0] == 0x63);
  releaseTexture(r);
  D3DDDI_SURFACEINFO info[4] = {{8, 8, 1, nullptr, 0, 0},
                                {4, 4, 1, nullptr, 0, 0},
                                {2, 2, 1, nullptr, 0, 0},
                                {1, 1, 1, nullptr, 0, 0}};
  D3DDDIARG_CREATERESOURCE a = {};
  a.Format = D3DFMT_A8R8G8B8;
  a.Pool = D3DDDIPOOL_VIDEOMEMORY;
  a.pSurfList = info;
  a.SurfCount = a.MipLevels = 4;
  a.Flags.Texture = 1;
  for (int fault = 0; fault < 9; fault++) {
    allocationCountdown = fault;
    a.hResource = (HANDLE)0x1234;
    hr = triton9CreateTextureResource(&d, &a);
    allocationCountdown = -1;
    CHECK(T, hr == E_OUTOFMEMORY && a.hResource == (HANDLE)0x1234);
  }
  info[2].Width = 3;
  CHECK(T, triton9CreateTextureResource(&d, &a) == D3DDDIERR_INVALIDCALL);
}
static void textureChains(TRITON9_DEVICE &d) {
  const char *T = "dirty_mip_chain";
  auto a = texture(d, D3DFMT_A8R8G8B8, 8, 8, 1, 4, 1);
  auto b = texture(d, D3DFMT_A8R8G8B8, 4, 4, 1, 3, 1);
  CHECK(T, a && b, return);
  CHECK_HR(T, triton9EnsureResourceHost(&d, a));
  CHECK_HR(T, triton9EnsureResourceHost(&d, b));
  for (UINT i = 0; i < 4; i++) {
    memset(a->textureSurfaces[i]->shadow, 0x50 + i,
           a->textureSurfaces[i]->shadowSize);
    CHECK_HR(T, triton9UploadShadow(&d, a->textureSurfaces[i]));
  }
  D3D11_BOX box = {0, 0, 0, 8, 8, 1};
  CHECK_HR(T, triton9CopyTextureChain(&d, a, b, 0, box, 0, 0, 0));
  for (UINT i = 0; i < 3; i++) {
    CHECK_HR(T, triton9ReadbackShadow(&d, b->textureSurfaces[i]));
    CHECK(T, b->textureSurfaces[i]->shadow[0] == 0x51 + i);
  }
  releaseTexture(a);
  releaseTexture(b);
}
static void mrtClear(TRITON9_DEVICE &d) {
  const char *T = "mrt_clear";
  TRITON9_RESOURCE *r[4] = {};
  for (UINT i = 0; i < 4; i++) {
    r[i] = texture(d, D3DFMT_A8R8G8B8, 8, 8, 1, 1, 1, true);
    CHECK(T, r[i], return);
    d.renderTargets[i] = r[i];
  }
  d.renderTarget = r[0];
  CHECK_HR(T, triton9BindOutputs(&d));
  D3DDDIARG_CLEAR args = {};
  args.Flags = D3DCLEAR_TARGET | D3DCLEAR_COMPUTERECTS;
  args.FillColor = 0xff194b72;
  CHECK_HR(T, triton9Clear(&d, &args, 0, nullptr));
  for (UINT i = 0; i < 4; i++) {
    CHECK_HR(T, triton9ReadbackShadow(&d, r[i]));
    uint32_t color;
    memcpy(&color, r[i]->shadow, 4);
    CHECK(T, color == args.FillColor);
  }
  args.Flags |= D3DCLEAR_ZBUFFER;
  args.FillColor = 0xffabcdef;
  CHECK(T, triton9Clear(&d, &args, 0, nullptr) == D3DDDIERR_INVALIDCALL);
  CHECK_HR(T, triton9ReadbackShadow(&d, r[0]));
  uint32_t unchanged;
  memcpy(&unchanged, r[0]->shadow, 4);
  CHECK(T, unchanged == 0xff194b72u);
  args.Flags &= ~D3DCLEAR_ZBUFFER;
  RECT rect = {2, 2, 5, 5};
  args.FillColor = 0xffee1100;
  CHECK_HR(T, triton9Clear(&d, &args, 1, &rect));
  for (UINT i = 0; i < 4; i++) {
    CHECK_HR(T, triton9ReadbackShadow(&d, r[i]));
    bool ok = true;
    for (UINT y = 0; y < 8; y++)
      for (UINT x = 0; x < 8; x++) {
        uint32_t color;
        memcpy(&color, r[i]->shadow + y * r[i]->pitch + x * 4, 4);
        ok &= color ==
              ((x >= 2 && x < 5 && y >= 2 && y < 5) ? 0xffee1100 : 0xff194b72);
      }
    CHECK(T, ok);
  }
  d.renderTarget = nullptr;
  for (UINT i = 0; i < 4; i++) {
    d.renderTargets[i] = nullptr;
    releaseTexture(r[i]);
  }
  CHECK_HR(T, triton9BindOutputs(&d));
}
static void autoMips(TRITON9_DEVICE &d) {
  const char *T = "autogen_mips";
  for (UINT filter : {UINT(D3DTEXF_POINT), UINT(D3DTEXF_LINEAR)}) {
    D3DDDI_SURFACEINFO info = {8, 8, 1, nullptr, 0, 0};
    D3DDDIARG_CREATERESOURCE a = {};
    a.Format = D3DFMT_A8R8G8B8;
    a.Pool = D3DDDIPOOL_VIDEOMEMORY;
    a.pSurfList = &info;
    a.SurfCount = a.MipLevels = 1;
    a.Flags.Texture = 1;
    a.Flags.AutogenMipmap = 1;
    CHECK_HR(T, triton9CreateTextureResource(&d, &a));
    auto r = (TRITON9_RESOURCE *)a.hResource;
    CHECK_HR(T, triton9EnsureResourceHost(&d, r));
    memset(r->shadow, 0x6a, r->shadowSize);
    CHECK_HR(T, triton9UploadShadow(&d, r));
    D3DDDIARG_GENERATEMIPSUBLEVELS gen = {a.hResource, filter};
    CHECK_HR(T, triton9GenerateMipSubLevels(&d, &gen));
    TRITON9_RESOURCE last = *r;
    last.textureOwner = r;
    last.textureSurfaces = nullptr;
    last.width = last.height = last.depth = 1;
    last.mipLevel = last.subresourceIndex = 3;
    last.stagingResource = nullptr;
    last.shadow = (BYTE *)calloc(1, 4);
    last.pitch = last.slicePitch = last.rowBytes = last.shadowSize = 4;
    CHECK_HR(T, triton9ReadbackShadow(&d, &last));
    CHECK(T, last.shadow[0] == 0x6a && last.shadow[3] == 0x6a);
    if (last.stagingResource)
      last.stagingResource->Release();
    free(last.shadow);
    releaseTexture(r);
  }
  triton9ReleaseStretchBlit(&d);
}
static void packedBlit(TRITON9_DEVICE &d) {
  const char *T = "packed_channel_blit";
  auto a = texture(d, D3DFMT_A2R10G10B10, 4, 4, 1, 1, 1, true);
  auto b = texture(d, D3DFMT_A8R8G8B8, 4, 4, 1, 1, 1, true);
  CHECK(T, a && b, return);
  CHECK_HR(T, triton9EnsureResourceHost(&d, a));
  CHECK_HR(T, triton9EnsureResourceHost(&d, b));
  uint32_t packed = (3u << 30) | (1023u << 20) | (512u << 10) | 256;
  for (UINT i = 0; i < 16; i++)
    memcpy(a->shadow + i * 4, &packed, 4);
  CHECK_HR(T, triton9UploadShadow(&d, a));
  RECT rect = {0, 0, 4, 4};
  CHECK_HR(T, triton9StretchBlt(&d, a, b, &rect, &rect, FALSE));
  CHECK_HR(T, triton9ReadbackShadow(&d, b));
  uint32_t p;
  memcpy(&p, b->shadow, 4);
  CHECK(T, p == 0xffff8040u);
  CHECK_HR(T, triton9StretchBlt(&d, b, a, &rect, &rect, FALSE));
  CHECK_HR(T, triton9ReadbackShadow(&d, a));
  memcpy(&p, a->shadow, 4);
  CHECK(T, ((p >> 20) & 1023) == 1023 && ((p >> 10) & 1023) >= 512 &&
               ((p >> 10) & 1023) <= 516 && (p & 1023) >= 256 &&
               (p & 1023) <= 259);
  releaseTexture(a);
  releaseTexture(b);
  triton9ReleaseStretchBlit(&d);
}
static void allShapes(TRITON9_DEVICE &d) {
  const char *T = "format_shape_views";
  for (UINT i = 0; i < triton9FormatCount(); i++) {
    const auto &f = g_formats[i];
    for (UINT kind : {3u, 4u}) {
      UINT op = kind == 3 ? FORMATOP_CUBETEXTURE : FORMATOP_VOLUMETEXTURE;
      if (!(f.operations & op))
        continue;
      auto r = texture(d, f.d3dFormat, 8, 8, kind == 4 ? 4 : 1, 4,
                       kind == 3 ? 6 : 1);
      CHECK(T, r, continue);
      HRESULT hr = triton9EnsureResourceHost(&d, r);
      if (FAILED(hr))
        emit("[FORMAT-SHAPE] format=%u kind=%u hr=%08x", f.d3dFormat, kind,
             (unsigned)hr);
      CHECK(T, SUCCEEDED(hr));
      if (SUCCEEDED(hr)) {
        ID3D11ShaderResourceView *v;
        CHECK_HR(T, triton9GetShaderResourceView(&d, r, &v));
      }
      releaseTexture(r);
    }
  }
}
static void locks(TRITON9_DEVICE &d) {
  const char *T = "lock_subresources";
  auto r = texture(d, D3DFMT_A8R8G8B8, 8, 8, 1, 4, 6);
  CHECK(T, r, return);
  CHECK_HR(T, triton9EnsureResourceHost(&d, r));
  auto a = r->textureSurfaces[9];
  memset(a->shadow, 0x21, a->shadowSize);
  CHECK_HR(T, triton9UploadShadow(&d, a));
  D3DDDIARG_LOCK lock = {};
  lock.hResource = r;
  lock.SubResourceIndex = 9;
  lock.Flags.AreaValid = 1;
  lock.Flags.WriteOnly = 1;
  lock.Area = {1, 1, 3, 3};
  CHECK_HR(T, triton9Lock(&d, &lock));
  CHECK(T, lock.hResource == r && lock.SubResourceIndex == 9 &&
               lock.pSurfData == a->shadow + a->pitch + 4);
  for (UINT y = 0; y < 2; y++)
    memset((BYTE *)lock.pSurfData + y * lock.Pitch, 0x78, 8);
  D3DDDIARG_UNLOCK unlock = {};
  unlock.hResource = r;
  unlock.SubResourceIndex = 9;
  CHECK_HR(T, triton9Unlock(&d, &unlock));
  CHECK_HR(T, triton9ReadbackShadow(&d, a));
  bool ok = true;
  for (UINT y = 0; y < 4; y++)
    for (UINT x = 0; x < 4; x++)
      ok &= a->shadow[y * a->pitch + x * 4] ==
            ((x >= 1 && x < 3 && y >= 1 && y < 3) ? 0x78 : 0x21);
  CHECK(T, ok);
  lock.Flags.Value = 0;
  lock.Flags.AreaValid = 1;
  lock.Area = {0, 0, 5, 4};
  CHECK(T, triton9Lock(&d, &lock) == D3DDDIERR_INVALIDCALL && !a->locked);
  D3DDDIARG_LOCKASYNC async = {};
  async.hResource = r;
  async.SubResourceIndex = 9;
  async.Flags.Discard = 1;
  CHECK_HR(T, triton9LockAsync(&d, &async));
  CHECK(T, async.hCookie != nullptr && a->pendingRename == async.hCookie);
  memset(async.pSurfData, 0x49, a->shadowSize);
  D3DDDIARG_UNLOCKASYNC end = {};
  end.hResource = r;
  end.SubResourceIndex = 9;
  CHECK_HR(T, triton9UnlockAsync(&d, &end));
  CHECK_HR(T,
           triton9RenameTexture(&d, a, (TRITON9_RENAME_COOKIE *)async.hCookie));
  CHECK_HR(T, triton9ReadbackShadow(&d, a));
  CHECK(T, a->shadow[0] == 0x49);
  releaseTexture(r);
  auto volume = texture(d, D3DFMT_A8R8G8B8, 4, 4, 4, 3, 1);
  CHECK(T, volume, return);
  lock = {};
  lock.hResource = volume;
  lock.Flags.BoxValid = 1;
  lock.Flags.WriteOnly = 1;
  lock.Box = {1, 1, 3, 3, 1, 3};
  CHECK_HR(T, triton9Lock(&d, &lock));
  CHECK(T, lock.pSurfData ==
               volume->shadow + volume->slicePitch + volume->pitch + 4);
  unlock = {};
  unlock.hResource = volume;
  CHECK_HR(T, triton9Unlock(&d, &unlock));
  releaseTexture(volume);
}
static void systemMemoryAsyncLocks(TRITON9_DEVICE &d) {
  const char *T = "system_memory_async_locks";
  BYTE bytes[96];
  memset(bytes, 0x31, sizeof(bytes));
  D3DDDI_SURFACEINFO info = {sizeof(bytes), 1, 1, bytes, 0, 0};
  D3DDDIARG_CREATERESOURCE create = {};
  create.Format = D3DFMT_VERTEXDATA;
  create.Pool = D3DDDIPOOL_SYSTEMMEM;
  create.pSurfList = &info;
  create.SurfCount = 1;
  create.Flags.VertexBuffer = create.Flags.WriteOnly = 1;
  CHECK_HR(T, triton9CreateSingleResource(&d, &create, FALSE));
  auto r = (TRITON9_RESOURCE *)create.hResource;
  CHECK(T, r && r->shadow == bytes && !r->ownsShadow, return);
  CHECK(T, !r->hostResource && !r->pendingRename);
  // Actual Vista 3DMark06 flags: NotifyOnly, NoOverwrite, RangeValid, with
  // or without NoExistingReferences.
  for (UINT flags : {0x65u, 0x45u, 0x41u}) {
    D3DDDIARG_LOCKASYNC lock = {};
    lock.hResource = r;
    lock.Flags.Value = flags;
    lock.Range = {16, 32};
    HRESULT hr = triton9LockAsync(&d, &lock);
    CHECK(T, hr == S_OK, continue);
    const bool range = (flags & 4) != 0;
    CHECK(T, lock.pSurfData == bytes + (range ? 16 : 0));
    CHECK(T, !lock.hCookie && !r->pendingRename && !r->hostResource);
    memset(bytes + (range ? 16 : 0), flags, range ? 32 : sizeof(bytes));
    D3DDDIARG_UNLOCKASYNC unlock = {};
    unlock.hResource = r;
    unlock.Flags.NotifyOnly = 1;
    CHECK_HR(T, triton9UnlockAsync(&d, &unlock));
    CHECK(T, !r->locked && !r->pendingRename && !r->hostResource);
  }
  // Once a host buffer exists, upload only the appended range and retain
  // unrelated host bytes even when the CPU shadow differs outside that range.
  memset(bytes, 0x31, sizeof(bytes));
  CHECK_HR(T, triton9EnsureResourceHost(&d, r));
  CHECK_HR(T, triton9UploadShadow(&d, r));
  memset(bytes, 0x77, sizeof(bytes));
  D3DDDIARG_LOCKASYNC lock = {};
  lock.hResource = r;
  lock.Flags.Value = 0x65;
  lock.Range = {16, 32};
  HRESULT hr = triton9LockAsync(&d, &lock);
  CHECK(T, hr == S_OK);
  if (SUCCEEDED(hr)) {
    memset(bytes + 16, 0x52, 32);
    D3DDDIARG_UNLOCKASYNC unlock = {};
    unlock.hResource = r;
    unlock.Flags.NotifyOnly = 1;
    CHECK_HR(T, triton9UnlockAsync(&d, &unlock));
    CHECK_HR(T, triton9ReadbackShadow(&d, r));
    for (UINT i = 0; i < sizeof(bytes); ++i)
      CHECK(T, bytes[i] == (i >= 16 && i < 48 ? 0x52 : 0x31));
  }
  for (UINT flags : {0x47u, 0xc5u, 0x05u}) {
    lock.Flags.Value = flags;
    CHECK(T, triton9LockAsync(&d, &lock) == D3DDDIERR_INVALIDCALL);
    CHECK(T, !r->locked && !r->pendingRename);
  }
  // DISCARD must fall back before writes can overwrite a queued worker's
  // older reference. Failure changes no bytes, lock state, or host identity.
  BYTE prior[sizeof(bytes)];
  memcpy(prior, bytes, sizeof(bytes));
  auto oldHost = r->hostResource;
  for (UINT flags : {0x42u, 0x46u, 0x62u, 0x66u}) {
    lock.Flags.Value = flags;
    lock.pSurfData = nullptr;
    CHECK(T, triton9LockAsync(&d, &lock) == E_NOTIMPL);
    CHECK(T, !lock.hCookie && !lock.pSurfData && !r->locked &&
             !r->pendingRename && r->hostResource == oldHost);
    CHECK_HR(T, triton9ReadbackShadow(&d, r));
    CHECK(T, !memcmp(bytes, prior, sizeof(bytes)));
  }
  releaseTexture(r);
}
static void systemMemoryRefresh(TRITON9_DEVICE &d) {
  const char *T = "system_memory_refresh";
  // Cross the chunk boundaries and exercise both VB and IB byte offsets.
  for (D3DFORMAT format : {D3DFMT_VERTEXDATA, D3DFMT_INDEX16}) {
    std::vector<BYTE> bytes(8194, 0x31), expected(bytes);
    D3DDDI_SURFACEINFO info = {UINT(bytes.size()), 1, 1, bytes.data(), 0, 0};
    D3DDDIARG_CREATERESOURCE create = {};
    create.Format = format; create.Pool = D3DDDIPOOL_SYSTEMMEM;
    create.pSurfList = &info; create.SurfCount = 1;
    create.Flags.VertexBuffer = format == D3DFMT_VERTEXDATA;
    create.Flags.IndexBuffer = format == D3DFMT_INDEX16;
    CHECK_HR(T, triton9CreateSingleResource(&d, &create, FALSE));
    auto r = (TRITON9_RESOURCE *)create.hResource;
    CHECK(T, r && r->shadow == bytes.data(), continue);
    CHECK_HR(T, triton9EnsureResourceHost(&d, r));
    auto refresh = [&](UINT left, UINT right) {
      bufferUploads.clear(); expected = bytes;
      CHECK_HR(T, triton9PrepareResourceForHostRead(&d, r));
      CHECK(T, bufferUploads.size() == (left == right ? 0u : 1u));
      if (!bufferUploads.empty()) {
        CHECK(T, bufferUploads[0].resource == r->hostResource);
        CHECK(T, bufferUploads[0].left == left && bufferUploads[0].right == right);
      }
      CHECK_HR(T, triton9ReadbackShadow(&d, r));
      CHECK(T, bytes == expected);
    };
    refresh(0, bytes.size());
    refresh(0, 0);
    for (UINT position : {0u, 1u, 4095u, 4096u, 8191u, 8193u}) {
      bytes[position] ^= 0xff;
      refresh(position, position + 1);
      refresh(0, 0);
    }
    bytes[17] ^= 0xff; bytes[8180] ^= 0xff;
    refresh(17, 8181);

    // A host write invalidates equality even when the alias is unchanged.
    BYTE other = 0x99;
    D3D11_BOX box = {0, 0, 0, 1, 1, 1};
    d.hostContext->UpdateSubresource(r->hostResource, 0, &box, &other, 0, 0);
    triton9ResourceWritten(r);
    refresh(0, bytes.size());

    // Resource identity independently prevents reuse of an old host mirror.
    auto oldHost = r->hostResource;
    D3D11_BUFFER_DESC desc;
    static_cast<ID3D11Buffer *>(oldHost)->GetDesc(&desc);
    ID3D11Buffer *replacement = nullptr;
    CHECK_HR(T, d.hostDevice->CreateBuffer(&desc, nullptr, &replacement));
    r->hostResource = replacement;
    refresh(0, bytes.size());
    oldHost->Release();

    // Failed health checks cannot publish newly staged bytes as committed.
    bytes[64] ^= 0xff;
    injectedHostStatus = E_FAIL;
    CHECK(T, FAILED(triton9PrepareResourceForHostRead(&d, r)));
    CHECK(T, !r->systemMemorySnapshotHost);
    injectedHostStatus = S_OK;
    refresh(0, bytes.size());
    injectedHostStatus = E_FAIL;
    CHECK(T, FAILED(triton9PrepareResourceForHostRead(&d, r)));
    CHECK(T, !r->systemMemorySnapshotHost);
    injectedHostStatus = S_OK;
    refresh(0, bytes.size());

    // OOM preserves the old full-upload behavior and remains retryable.
    free(r->systemMemorySnapshot); r->systemMemorySnapshot = nullptr;
    r->systemMemorySnapshotSize = 0;
    allocationCountdown = 0;
    bufferUploads.clear();
    CHECK_HR(T, triton9PrepareResourceForHostRead(&d, r));
    allocationCountdown = -1;
    CHECK(T, !r->systemMemorySnapshot && !r->systemMemorySnapshotHost);
    CHECK(T, bufferUploads.size() == 1 && bufferUploads[0].right == bytes.size());
    refresh(0, bytes.size());

    // A ranged Unlock transmits only that range. Rebinding must still see
    // independently changed alias bytes outside it.
    memset(bytes.data(), 0x77, bytes.size());
    D3DDDIARG_LOCKASYNC lock = {};
    lock.hResource = r; lock.Flags.Value = 0x65; lock.Range = {16, 32};
    CHECK_HR(T, triton9LockAsync(&d, &lock));
    memset(bytes.data() + 16, 0x52, 32);
    D3DDDIARG_UNLOCKASYNC unlock = {};
    unlock.hResource = r; unlock.Flags.NotifyOnly = 1;
    bufferUploads.clear();
    CHECK_HR(T, triton9UnlockAsync(&d, &unlock));
    CHECK(T, bufferUploads.size() == 1 && bufferUploads[0].left == 16 &&
             bufferUploads[0].right == 48);
    refresh(0, bytes.size());
    refresh(0, 0);

    // Successful partial writes update only their submitted snapshot span.
    // The next bind must not upload it again or hide independent alias writes.
    for (UINT outside : {UINT(bytes.size()), 1u, 8193u}) {
      CHECK_HR(T, triton9LockAsync(&d, &lock));
      bytes[23] ^= 0xff;
      if (outside < bytes.size()) bytes[outside] ^= 0xff;
      bufferUploads.clear();
      CHECK_HR(T, triton9UnlockAsync(&d, &unlock));
      CHECK(T, bufferUploads.size() == 1 && bufferUploads[0].left == 16 &&
               bufferUploads[0].right == 48);
      CHECK(T, r->systemMemorySnapshotHost == r->hostResource &&
               r->systemMemorySnapshotSerial == r->contentSerial);
      if (outside < bytes.size()) refresh(outside, outside + 1);
      else refresh(0, 0);
    }

    // A partial write cannot validate a snapshot made stale by a host write.
    d.hostContext->UpdateSubresource(r->hostResource, 0, &box, &other, 0, 0);
    triton9ResourceWritten(r);
    CHECK_HR(T, triton9LockAsync(&d, &lock));
    bytes[24] ^= 0xff;
    CHECK_HR(T, triton9UnlockAsync(&d, &unlock));
    refresh(0, bytes.size());

    // Failed partial writes and full writes have the same publication rule.
    CHECK_HR(T, triton9LockAsync(&d, &lock));
    bytes[25] ^= 0xff;
    injectedHostStatus = E_FAIL;
    CHECK(T, FAILED(triton9UnlockAsync(&d, &unlock)));
    CHECK(T, !r->systemMemorySnapshotHost);
    injectedHostStatus = S_OK;
    refresh(0, bytes.size());
    bytes[36] ^= 0xff;
    CHECK_HR(T, triton9UploadShadow(&d, r));
    refresh(0, 0);
    bytes[37] ^= 0xff;
    injectedHostStatus = E_FAIL;
    CHECK(T, FAILED(triton9UploadShadow(&d, r)));
    CHECK(T, !r->systemMemorySnapshotHost);
    injectedHostStatus = S_OK;
    refresh(0, bytes.size());

    // Draws consume only their validated spans. Changes outside one draw must
    // remain pending and appear when a later draw reads that part of the alias.
    auto hostBytes = bytes;
    bytes[17] ^= 0x80; bytes[4095] ^= 0x40; bytes[8180] ^= 0x20;
    auto refreshRange = [&](UINT first, UINT end, UINT changedFirst, UINT changedEnd) {
      auto aliasBytes = bytes;
      bufferUploads.clear();
      CHECK_HR(T, triton9PrepareBufferRangeForHostRead(&d, r, first, end));
      CHECK(T, bufferUploads.size() == (changedFirst == changedEnd ? 0u : 1u));
      if (!bufferUploads.empty()) {
        CHECK(T, bufferUploads[0].left == changedFirst &&
                 bufferUploads[0].right == changedEnd);
      }
      std::copy(bytes.begin() + changedFirst, bytes.begin() + changedEnd,
                hostBytes.begin() + changedFirst);
      CHECK_HR(T, triton9ReadbackShadow(&d, r));
      CHECK(T, bytes == hostBytes);
      bytes = aliasBytes;
    };
    refreshRange(4090, 4100, 4095, 4096);
    refreshRange(4090, 4100, 0, 0);
    refreshRange(16, 18, 17, 18);
    refreshRange(8000, 8194, 8180, 8181);
    refreshRange(1, 8193, 0, 0);
    bytes[0] ^= 0x10; bytes[8193] ^= 0x08;
    r->systemMemorySnapshotHost = nullptr;
    refreshRange(16, 18, 0, bytes.size());
    CHECK(T, triton9PrepareBufferRangeForHostRead(&d, r, 16, 16) == D3DDDIERR_INVALIDCALL);
    CHECK(T, triton9PrepareBufferRangeForHostRead(&d, r, 0, bytes.size() + 1) == D3DDDIERR_INVALIDCALL);
    releaseTexture(r);

    // A padded CPU allocation does not enlarge the host buffer. Provide
    // physical guard storage so the negative control stays within allocations.
    std::vector<BYTE> padded(96, 0x31);
    info.Width = 64; info.pSysMem = padded.data(); info.SysMemSlicePitch = 96;
    create.hResource = nullptr;
    CHECK_HR(T, triton9CreateSingleResource(&d, &create, FALSE));
    r = (TRITON9_RESOURCE *)create.hResource;
    CHECK(T, r && r->shadowSize == 96 && r->width == 64, continue);
    CHECK_HR(T, triton9PrepareResourceForHostRead(&d, r));
    free(r->systemMemorySnapshot);
    r->systemMemorySnapshot = (BYTE *)malloc(96);
    memset(r->systemMemorySnapshot, 0x31, 64);
    memset(r->systemMemorySnapshot + 64, 0xa5, 32);
    lock.hResource = r; lock.Range = {64, 8};
    unlock.hResource = r;
    CHECK_HR(T, triton9LockAsync(&d, &lock));
    memset(padded.data() + 64, 0x52, 8);
    bufferUploads.clear();
    auto serial = r->contentSerial;
    suppressBufferForward = true;
    HRESULT rejected = triton9UnlockAsync(&d, &unlock);
    suppressBufferForward = false;
    CHECK(T, rejected == D3DDDIERR_INVALIDCALL);
    CHECK(T, bufferUploads.empty() && r->contentSerial == serial);
    CHECK(T, std::all_of(r->systemMemorySnapshot + 64,
                        r->systemMemorySnapshot + 96,
                        [](BYTE b) { return b == 0xa5; }));
    CHECK(T, !r->locked);
    releaseTexture(r);
  }
}
static void admission(TRITON9_DEVICE &d) {
  const char *T = "surface_admission";
  D3DDDI_SURFACEINFO info = {8, 8, 1, nullptr, 0, 0};
  D3DDDIARG_CREATERESOURCE a = {};
  a.Format = D3DFMT_A16B16G16R16F;
  a.Pool = D3DDDIPOOL_VIDEOMEMORY;
  a.pSurfList = &info;
  a.SurfCount = 1;
  a.MipLevels = 0xdeadbeef;
  a.Fvf = 0xbad;
  a.Flags.RenderTarget = 1;
  CHECK_HR(T, triton9CreateSingleResource(&d, &a, FALSE));
  auto r = (TRITON9_RESOURCE *)a.hResource;
  CHECK(T, !r->needsPresentAllocation && r->fvf == 0 && r->mipLevels == 1);
  CHECK_HR(T, triton9EnsureResourceHost(&d, r));
  ID3D11RenderTargetView *v;
  CHECK_HR(T, triton9GetRenderTargetView(&d, r, &v));
  const float c[] = {1, .5f, .25f, 1};
  d.hostContext->ClearRenderTargetView(v, c);
  CHECK_HR(T, triton9ReadbackShadow(&d, r));
  uint16_t pixel[4];
  memcpy(pixel, r->shadow, 8);
  CHECK(T, pixel[0] == 0x3c00 && pixel[1] == 0x3800 && pixel[2] == 0x3400 &&
               pixel[3] == 0x3c00);
  releaseTexture(r);
  a.Format = D3DFMT_A8;
  a.Flags.RenderTarget = 1;
  a.hResource = (HANDLE)0x1234;
  CHECK(T,
        triton9CreateSingleResource(&d, &a, FALSE) == D3DDDIERR_INVALIDCALL &&
            a.hResource == (HANDLE)0x1234);
  a.Format = D3DFMT_A8R8G8B8;
  a.MultisampleType = D3DMULTISAMPLE_4_SAMPLES;
  a.MultisampleQuality = 1;
  CHECK(T, triton9CreateSingleResource(&d, &a, FALSE) == D3DDDIERR_INVALIDCALL);
  a.Format = D3DFMT_A8B8G8R8;
  a.MultisampleType = D3DMULTISAMPLE_NONMASKABLE;
  for (UINT quality=0;quality<2;++quality) {
    a.MultisampleQuality = quality;
    CHECK_HR(T,triton9CreateSingleResource(&d,&a,FALSE));
    r=(TRITON9_RESOURCE *)a.hResource;
    CHECK(T,r->nonMaskable && r->sampleCount==(quality?4u:2u) && r->sampleQuality==0);
    // A8B8G8R8 is a native render surface, without the KMD scanout bridge.
    CHECK_HR(T,triton9EnsureResourceHost(&d,r));
    D3D11_TEXTURE2D_DESC desc;((ID3D11Texture2D*)r->hostResource)->GetDesc(&desc);
    CHECK(T,desc.SampleDesc.Count==(quality?4u:2u) && desc.SampleDesc.Quality==0);
    CHECK_HR(T,triton9GetRenderTargetView(&d,r,&v));
    const float blue[4]={0,0,1,1};d.hostContext->ClearRenderTargetView(v,blue);
    CHECK_HR(T,triton9ReadbackShadow(&d,r));
    uint32_t resolved;memcpy(&resolved,r->shadow,4);CHECK(T,resolved==0xffff0000u);
    releaseTexture(r);
  }
  a.hResource=(HANDLE)0x1234;a.MultisampleQuality=2;
  CHECK(T,triton9CreateSingleResource(&d,&a,FALSE)==D3DDDIERR_INVALIDCALL && a.hResource==(HANDLE)0x1234);
  a.MultisampleQuality=~0u;
  CHECK(T,triton9CreateSingleResource(&d,&a,FALSE)==D3DDDIERR_INVALIDCALL);
  a.Format=D3DFMT_A16B16G16R16F;a.MultisampleQuality=0;
  CHECK(T,triton9CreateSingleResource(&d,&a,FALSE)==D3DDDIERR_INVALIDCALL);
  a.Flags.Value = 0;
  a.Flags.ZBuffer = 1;
  a.Format = D3DFMT_D16_LOCKABLE;
  a.MultisampleType = D3DMULTISAMPLE_NONE;
  a.MultisampleQuality = 0;
  CHECK_HR(T, triton9CreateSingleResource(&d, &a, FALSE));
  r = (TRITON9_RESOURCE *)a.hResource;
  CHECK_HR(T, triton9EnsureResourceHost(&d, r));
  ID3D11DepthStencilView *ds;
  CHECK_HR(T, triton9GetDepthStencilView(&d, r, &ds));
  d.hostContext->ClearDepthStencilView(ds, D3D11_CLEAR_DEPTH, .25f, 0);
  CHECK_HR(T, triton9ReadbackShadow(&d, r));
  uint16_t depth;
  memcpy(&depth, r->shadow, 2);
  CHECK(T, depth == 16384);
  releaseTexture(r);
}
static void defaultChannels(TRITON9_DEVICE &d) {
  const char *T = "blit_default_channels";
  struct Item {
    D3DFORMAT format;
    UINT bytes;
    uint64_t bits;
    uint32_t expected;
  };
  Item items[] = {{D3DFMT_R16F, 2, 0x3800, 0xff80ffff},
                  {D3DFMT_G16R16F, 4, 0x38003c00, 0xffff80ff},
                  {D3DFMT_X8B8G8R8, 4, 0x00563412, 0xff123456},
                  {D3DFMT_X1R5G5B5, 2, 0x7c00, 0xffff0000},
                  {D3DFMT_X4R4G4B4, 2, 0x0f00, 0xffff0000}};
  RECT rect = {0, 0, 4, 4};
  for (auto item : items) {
    auto a = texture(d, item.format, 4, 4, 1, 1, 1, true),
         b = texture(d, D3DFMT_A8R8G8B8, 4, 4, 1, 1, 1, true),
         packed = texture(d, D3DFMT_A2R10G10B10, 4, 4, 1, 1, 1, true);
    CHECK(T, a && b && packed, return);
    CHECK_HR(T, triton9EnsureResourceHost(&d, a));
    CHECK_HR(T, triton9EnsureResourceHost(&d, b));
    CHECK_HR(T, triton9EnsureResourceHost(&d, packed));
    for (UINT y = 0; y < 4; y++)
      for (UINT x = 0; x < 4; x++)
        memcpy(a->shadow + y * a->pitch + x * item.bytes, &item.bits,
               item.bytes);
    CHECK_HR(T, triton9UploadShadow(&d, a));
    for (int mode = 0; mode < 2; mode++) {
      if (mode) {
        CHECK_HR(T, triton9StretchBlt(&d, a, packed, &rect, &rect, FALSE));
        CHECK_HR(T, triton9StretchBlt(&d, packed, b, &rect, &rect, FALSE));
      } else
        CHECK_HR(T, triton9StretchBlt(&d, a, b, &rect, &rect, FALSE));
      CHECK_HR(T, triton9ReadbackShadow(&d, b));
      uint32_t pixel;
      memcpy(&pixel, b->shadow, 4);
      if (pixel != item.expected)
        emit("[DEFAULT-CHANNELS] format=%u mode=%d got=%08x expected=%08x",
             item.format, mode, pixel, item.expected);
      CHECK(T, pixel == item.expected);
    }
    releaseTexture(a);
    releaseTexture(b);
    releaseTexture(packed);
  }
  triton9ReleaseStretchBlit(&d);
}
static void nonblockingRead(TRITON9_DEVICE &d) {
  const char *T = "nonblocking_lock_progress";
  auto r = texture(d, D3DFMT_A8R8G8B8, 8, 8, 1, 4, 1);
  CHECK(T, r, return);
  CHECK_HR(T, triton9EnsureResourceHost(&d, r));
  memset(r->shadow, 0x12, r->shadowSize);
  CHECK_HR(T, triton9UploadShadow(&d, r));
  D3DDDIARG_LOCK lock = {};
  lock.hResource = r;
  lock.Flags.ReadOnly = 1;
  lock.Flags.DoNotWait = 1;
  CHECK(T, triton9Lock(&d, &lock) == D3DDDIERR_WASSTILLDRAWING && !r->locked);
  memset(r->shadow, 0x7c, r->shadowSize);
  CHECK_HR(T, triton9UploadShadow(&d, r));
  CHECK(T, triton9Lock(&d, &lock) == D3DDDIERR_WASSTILLDRAWING);
  HRESULT hr = D3DDDIERR_WASSTILLDRAWING;
  for (UINT i = 0; i < 10000 && hr == D3DDDIERR_WASSTILLDRAWING; i++) {
    hr = triton9Lock(&d, &lock);
    if (hr == D3DDDIERR_WASSTILLDRAWING)
      usleep(100);
  }
  CHECK(T, hr == S_OK && r->locked);
  if (SUCCEEDED(hr)) {
    bool ok = true;
    for (size_t i = 0; i < r->shadowSize; i++)
      ok &= ((BYTE *)lock.pSurfData)[i] == 0x7c;
    CHECK(T, ok);
    D3DDDIARG_UNLOCK end = {};
    end.hResource = r;
    CHECK_HR(T, triton9Unlock(&d, &end));
  }
  releaseTexture(r);
}
static void logicalFormatBlt(TRITON9_DEVICE &d) {
  const char *T = "logical_format_blt";
  auto source = texture(d,D3DFMT_A2R10G10B10,4,4,1,1,1,true);
  auto destination = texture(d,D3DFMT_A2B10G10R10,4,4,1,1,1,true);
  CHECK(T, source && destination, return);
  CHECK_HR(T,triton9EnsureResourceHost(&d,source));
  CHECK_HR(T,triton9EnsureResourceHost(&d,destination));
  uint32_t input=(3u<<30)|(1023u<<20)|(512u<<10)|256;
  for (UINT i=0;i<16;i++) memcpy(source->shadow+i*4,&input,4);
  CHECK_HR(T,triton9UploadShadow(&d,source));
  D3DDDIARG_BLT args={}; args.hSrcResource=source;args.hDstResource=destination;
  args.SrcRect=args.DstRect={0,0,4,4};
  CHECK_HR(T,triton9BltImpl(&d,&args));
  CHECK_HR(T,triton9ReadbackShadow(&d,destination));
  uint32_t actual; memcpy(&actual,destination->shadow,4);
  CHECK(T,actual==((3u<<30)|(256u<<20)|(512u<<10)|1023));
  releaseTexture(source);releaseTexture(destination);triton9ReleaseStretchBlit(&d);
  CHECK(T,triton9FormatMultisampleQuality(D3DFMT_UNKNOWN,0)==0);
  CHECK(T,triton9FormatMultisampleQuality(D3DFMT_A8,0)==0);
  CHECK(T,triton9FormatMultisampleQuality(D3DFMT_A8R8G8B8,0)==1);
}
static void packedStorage(TRITON9_DEVICE &d) {
  const char *T="canonical_packed10_storage";
  const UINT32 logical=(2u<<30)|(913u<<20)|(537u<<10)|117u;
  const UINT32 canonical=(2u<<30)|(117u<<20)|(537u<<10)|913u;
  auto checkStorage=[&](TRITON9_RESOURCE *r,UINT32 host,UINT32 cpu) {
    CHECK_HR(T,triton9EnsureStagingResource(&d,r));
    d.hostContext->CopySubresourceRegion(r->stagingResource,0,0,0,0,r->hostResource,r->subresourceIndex,nullptr);
    D3D11_MAPPED_SUBRESOURCE map={};CHECK_HR(T,triton9MapStagingForRead(&d,r,&map));
    for(UINT z=0;z<r->depth;++z)for(UINT y=0;y<r->height;++y)for(UINT x=0;x<r->width;++x){
      UINT32 actual;memcpy(&actual,(BYTE*)map.pData+z*map.DepthPitch+y*map.RowPitch+x*4,4);
      CHECK(T,actual==host);
    }
    d.hostContext->Unmap(r->stagingResource,0);
    CHECK_HR(T,triton9ReadbackShadow(&d,r));
    for(UINT z=0;z<r->depth;++z)for(UINT y=0;y<r->height;++y)for(UINT x=0;x<r->width;++x){
      UINT32 actual;memcpy(&actual,r->shadow+z*r->slicePitch+y*r->pitch+x*4,4);
      CHECK(T,actual==cpu);
    }
  };
  // Distinct mip/face aliases and a volume exercise canonical backing indices.
  for(UINT faces:{1u,6u}) {
    auto r=texture(d,D3DFMT_A2R10G10B10,4,4,faces==1?2:1,3,faces);
    CHECK(T,r,continue);CHECK_HR(T,triton9EnsureResourceHost(&d,r));
    for(UINT i=0;i<r->surfaceCount;++i){
      auto sub=r->textureSurfaces[i];
      for(SIZE_T p=0;p<sub->shadowSize;p+=4)memcpy(sub->shadow+p,&logical,4);
      CHECK_HR(T,triton9UploadShadow(&d,sub));checkStorage(sub,canonical,logical);
    }
    releaseTexture(r);
  }
  // Runtime bytes can be unaligned, row-padded and slice-padded.
  std::vector<BYTE> bytes(257,0xcd);
  D3DDDI_SURFACEINFO info={4,4,2,bytes.data()+1,24,128};
  D3DDDIARG_CREATERESOURCE args={};args.Format=D3DFMT_A2R10G10B10;
  args.Pool=D3DDDIPOOL_SYSTEMMEM;args.pSurfList=&info;args.SurfCount=args.MipLevels=1;
  args.Flags.Texture=1;args.Flags.Volume=1;
  for(UINT z=0;z<2;++z)for(UINT y=0;y<4;++y)for(UINT x=0;x<4;++x)
    memcpy(bytes.data()+1+z*128+y*24+x*4,&logical,4);
  const auto original=bytes;
  CHECK_HR(T,triton9CreateTextureResource(&d,&args));
  auto padded=(TRITON9_RESOURCE*)args.hResource;
  CHECK_HR(T,triton9EnsureResourceHost(&d,padded));
  checkStorage(padded,canonical,logical);CHECK(T,bytes==original);
  releaseTexture(padded);
  // CPU copy, partial locked upload, staging copy and clear all meet at the
  // same canonical GPU storage, preserving bytes outside the selected region.
  info.Depth=1;args.Flags.Volume=0;args.hResource=nullptr;
  CHECK_HR(T,triton9CreateTextureResource(&d,&args));padded=(TRITON9_RESOURCE*)args.hResource;
  auto r=texture(d,D3DFMT_A2R10G10B10,4,4,1,1,1,true);
  auto rgb=texture(d,D3DFMT_A8R8G8B8,4,4,1,1,1,true);
  CHECK(T,r&&rgb,return);CHECK_HR(T,triton9EnsureResourceHost(&d,r));
  D3D11_BOX full={0,0,0,4,4,1};
  CHECK_HR(T,triton9CopyBox(&d,padded,r,&full,0,0,0,FALSE));checkStorage(r,canonical,logical);
  const UINT32 red=(3u<<30)|(1023u<<20), redHost=(3u<<30)|1023u;
  D3DDDIARG_LOCK lock={};lock.hResource=r;lock.Flags.AreaValid=lock.Flags.WriteOnly=1;lock.Area={1,1,3,3};
  CHECK_HR(T,triton9Lock(&d,&lock));
  for(UINT y=0;y<2;++y)for(UINT x=0;x<2;++x)memcpy((BYTE*)lock.pSurfData+y*lock.Pitch+x*4,&red,4);
  D3DDDIARG_UNLOCK unlock={};unlock.hResource=r;CHECK_HR(T,triton9Unlock(&d,&unlock));
  CHECK_HR(T,triton9ReadbackShadow(&d,r));
  for(UINT y=0;y<4;++y)for(UINT x=0;x<4;++x){UINT32 actual;memcpy(&actual,r->shadow+y*r->pitch+x*4,4);CHECK(T,actual==((x>=1&&x<3&&y>=1&&y<3)?red:logical));}
  D3D11_BOX middle={1,1,0,3,3,1};
  CHECK_HR(T,triton9CopyBox(&d,r,padded,&middle,1,1,0,FALSE));
  for(UINT i=0;i<bytes.size();++i){
    const UINT offset=i?i-1:UINT_MAX,y=offset/24,x=offset%24;
    if(i&&offset<96&&y>=1&&y<3&&x>=4&&x<12)continue;
    CHECK(T,bytes[i]==original[i]);
  }
  RECT rect={0,0,4,4};CHECK_HR(T,triton9EnsureResourceHost(&d,rgb));
  CHECK_HR(T,triton9StretchBlt(&d,padded,rgb,&rect,&rect,FALSE));
  CHECK_HR(T,triton9ReadbackShadow(&d,rgb));UINT32 actual=0;
  memcpy(&actual,rgb->shadow+rgb->pitch+4,4);CHECK(T,actual==0xffff0000);
  D3DDDIARG_SETRENDERTARGET rt={0,r,0};CHECK_HR(T,triton9SetRenderTarget(&d,&rt));
  D3DDDIARG_CLEAR clear={};clear.Flags=D3DCLEAR_TARGET;clear.FillColor=0xffff0000;
  CHECK_HR(T,triton9Clear(&d,&clear,1,&rect));checkStorage(r,redHost,red);
  // Failing the conversion allocation must submit no upload or write serial.
  memset(r->shadow,0,r->shadowSize);UINT64 serial=r->contentSerial;
  allocationCountdown=0;HRESULT failure=triton9UploadShadow(&d,r);allocationCountdown=-1;
  CHECK(T,failure==E_OUTOFMEMORY&&r->contentSerial==serial);checkStorage(r,redHost,red);
  D3DDDIARG_COLORFILL fill={};fill.hResource=r;fill.Color=0xff0000ff;fill.DstRect=rect;
  CHECK_HR(T,triton9ColorFill(&d,&fill));
  checkStorage(r,(3u<<30)|(1023u<<20),(3u<<30)|1023u);
  rt.hRenderTarget=nullptr;CHECK_HR(T,triton9SetRenderTarget(&d,&rt));
  releaseTexture(padded);releaseTexture(r);releaseTexture(rgb);triton9ReleaseStretchBlit(&d);
  for(UINT filter:{UINT(D3DTEXF_POINT),UINT(D3DTEXF_LINEAR)}){
    D3DDDI_SURFACEINFO mipInfo={4,4,1,nullptr,0,0};
    D3DDDIARG_CREATERESOURCE mipArgs={};mipArgs.Format=D3DFMT_A2R10G10B10;
    mipArgs.Pool=D3DDDIPOOL_VIDEOMEMORY;mipArgs.pSurfList=&mipInfo;mipArgs.SurfCount=mipArgs.MipLevels=1;
    mipArgs.Flags.Texture=mipArgs.Flags.AutogenMipmap=1;
    CHECK_HR(T,triton9CreateTextureResource(&d,&mipArgs));auto mip=(TRITON9_RESOURCE*)mipArgs.hResource;
    CHECK_HR(T,triton9EnsureResourceHost(&d,mip));
    for(SIZE_T p=0;p<mip->shadowSize;p+=4)memcpy(mip->shadow+p,&logical,4);
    CHECK_HR(T,triton9UploadShadow(&d,mip));
    D3DDDIARG_GENERATEMIPSUBLEVELS gen={mip,filter};CHECK_HR(T,triton9GenerateMipSubLevels(&d,&gen));
    TRITON9_RESOURCE last=*mip;last.textureOwner=mip;last.textureSurfaces=nullptr;
    last.width=last.height=last.depth=1;last.mipLevel=last.subresourceIndex=2;
    last.stagingResource=nullptr;last.shadow=(BYTE*)calloc(1,4);
    last.pitch=last.slicePitch=last.rowBytes=last.shadowSize=4;
    checkStorage(&last,canonical,logical);
    if(last.stagingResource)last.stagingResource->Release();free(last.shadow);releaseTexture(mip);
  }
  triton9ReleaseStretchBlit(&d);
}

static void retainedTextureBinding(TRITON9_DEVICE &d) {
  const char *T="retained_texture_binding";
  auto a=texture(d,D3DFMT_A8R8G8B8,4,4,1,1,1,true);
  auto b=texture(d,D3DFMT_A8R8G8B8,4,4,1,1,1,true);
  CHECK(T,a&&b,return);
  CHECK_HR(T,triton9EnsureResourceHost(&d,a));CHECK_HR(T,triton9EnsureResourceHost(&d,b));
  const UINT32 color=0xff123456;
  for(UINT i=0;i<16;++i)memcpy(a->shadow+i*4,&color,4);
  CHECK_HR(T,triton9UploadShadow(&d,a));
  CHECK_HR(T,triton9SetTexture(&d,0,a));
  CHECK_HR(T,triton9SetTexture(&d,D3DVERTEXTEXTURESAMPLER0,a));
  D3DDDIARG_SETRENDERTARGET rt={0,a,0};
  CHECK_HR(T,triton9SetRenderTarget(&d,&rt));
  CHECK(T,d.textures[0]==a&&d.textures[17]==a&&d.blendStateDirty);
  CHECK_HR(T,triton9PreparePipelineState(&d));
  ID3D11ShaderResourceView *ps=nullptr,*vs=nullptr;
  d.hostContext->PSGetShaderResources(0,1,&ps);d.hostContext->VSGetShaderResources(0,1,&vs);
  CHECK(T,!ps&&!vs);
  if(ps)ps->Release();if(vs)vs->Release();
  // No shader reads either sampler in this pass. Issue an actual draw with
  // rasterization disabled, leaving A's independently initialized pixels.
  ID3D11VertexShader *vertex=nullptr;
  CHECK_HR(T,d.hostDevice->CreateVertexShader(g_tritonBlitVS,sizeof(g_tritonBlitVS),nullptr,&vertex));
  d.hostContext->VSSetShader(vertex,nullptr,0);d.hostContext->PSSetShader(nullptr,nullptr,0);
  d.hostContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  d.hostContext->Draw(3,0);
  CHECK_HR(T,triton9CheckHostDevice(&d));
  rt.hRenderTarget=b;CHECK_HR(T,triton9SetRenderTarget(&d,&rt));
  CHECK_HR(T,triton9PreparePipelineState(&d));
  CHECK(T,d.textures[0]==a&&d.textures[17]==a);
  d.hostContext->PSGetShaderResources(0,1,&ps);d.hostContext->VSGetShaderResources(0,1,&vs);
  CHECK(T,ps==a->shaderResourceView&&vs==a->shaderResourceView);
  if(ps)ps->Release();if(vs)vs->Release();
  CHECK_HR(T,triton9EnsureStretchBlit(&d));
  auto state=(TRITON9_STRETCH_STATE *)d.stretchBlitState;
  d.hostContext->PSSetShader(state->ps,nullptr,0);
  d.hostContext->PSSetConstantBuffers(0,1,&state->constants);
  d.renderStates[D3DDDIRS_MULTISAMPLEMASK]=~0u;
  CHECK_HR(T,triton9PreparePipelineState(&d));
  d.hostContext->Draw(3,0);
  CHECK_HR(T,triton9ReadbackShadow(&d,b));
  UINT32 actual=0;memcpy(&actual,b->shadow+2*b->pitch+2*4,4);CHECK(T,actual==color);
  CHECK_HR(T,triton9SetTexture(&d,0,nullptr));
  CHECK_HR(T,triton9SetTexture(&d,D3DVERTEXTEXTURESAMPLER0,nullptr));
  rt.hRenderTarget=nullptr;CHECK_HR(T,triton9SetRenderTarget(&d,&rt));
  d.hostContext->ClearState();vertex->Release();
  for(auto &sampler:d.samplerStates){if(sampler)sampler->Release();sampler=nullptr;}
  releaseTexture(a);releaseTexture(b);triton9ReleaseStretchBlit(&d);
}
int main(int argc, char **argv) {
  g_out = stdout;
  Ctx c;
  if (!create_device(c))
    return 1;
  TRITON9_DEVICE d = {};
  if (FAILED(c.device->QueryInterface(__uuidof(ID3D11Device1),
                                      (void **)&d.hostDevice)) ||
      FAILED(c.ctx->QueryInterface(__uuidof(ID3D11DeviceContext1),
                                   (void **)&d.hostContext)))
    return 1;
  if (argc == 2 && !strcmp(argv[1], "--system-memory-refresh")) {
    systemMemoryRefresh(d);
    d.hostContext->Release();
    d.hostDevice->Release();
    emit("[RESOURCE-SUMMARY] passed=%d failed=%d", g_passes, g_fails);
    return g_fails ? 1 : 0;
  }
  if (argc == 2 && !strcmp(argv[1], "--gpu-overlap")) {
    overlappingCopies(d);
    multisample(d);
    d.hostContext->Release();
    d.hostDevice->Release();
    emit("[RESOURCE-SUMMARY] passed=%d failed=%d", g_passes, g_fails);
    return g_fails ? 1 : 0;
  }
  typedSrgb(d);
  shapes(d);
  volume(d);
  compressed(d);
  copying(d);
  overlappingCopies(d);
  multisample(d);
  formats(d);
  renameAndFailures(d);
  textureChains(d);
  mrtClear(d);
  autoMips(d);
  packedBlit(d);
  allShapes(d);
  locks(d);
  systemMemoryAsyncLocks(d);
  systemMemoryRefresh(d);
  admission(d);
  defaultChannels(d);
  nonblockingRead(d);
  logicalFormatBlt(d);
  retainedTextureBinding(d);
  packedStorage(d);
  d.hostContext->Release();
  d.hostDevice->Release();
  emit("[RESOURCE-SUMMARY] passed=%d failed=%d", g_passes, g_fails);
  return g_fails ? 1 : 0;
}

#endif // TRITON9_SHARED_ROLLBACK_TEST
