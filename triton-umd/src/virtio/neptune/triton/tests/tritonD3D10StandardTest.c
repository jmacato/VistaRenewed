/* SPDX-License-Identifier: MIT
 * CPU execution of production OpenResource, DestroyResource, rotation and
 * descriptor/view helpers. Bridge/KMT calls are fault-injected test doubles. */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
typedef unsigned UINT,ULONG,DXGI_FORMAT,D3DKMT_HANDLE;typedef uint64_t ULONGLONG;typedef SIZE_TYPE SIZE_T;typedef int BOOL;typedef int32_t HRESULT;
#define TRUE 1
#define FALSE 0
#define S_OK 0
#define E_FAIL (-1)
#define E_INVALIDARG (-2)
#define E_NOTIMPL (-3)
#define FAILED(x) ((x)<0)
#define APIENTRY
#define TR_LOG(...) ((void)0)
#define npt_log(...) ((void)0)
#define TR_STUB(...) ((void)0)
#define TRITON_SHARED_MAX_PLANES 4
#define VIOGPU_RESOURCE_TYPE_3D 0
#define VIOGPU_RESOURCE_TYPE_SHARED 3
#define VIOGPU_RESOURCE_FLAG_STANDARD_PRIMARY 0x80000000u
#define D3D11_USAGE_DEFAULT 0
#define D3D11_BIND_SHADER_RESOURCE 8
#define D3D11_BIND_RENDER_TARGET 32
#define D3D11_RESOURCE_MISC_SHARED 2
#define D3D11_0_DDI_INTERFACE_VERSION 0x100
#define D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT 128
#define D3D11_PS_CS_UAV_REGISTER_COUNT 8
enum {DXGI_FORMAT_UNKNOWN=0,DXGI_FORMAT_R8G8B8A8_UNORM=28,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB=29,DXGI_FORMAT_B8G8R8A8_UNORM=87,DXGI_FORMAT_B8G8R8X8_UNORM=88,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB=91,DXGI_FORMAT_B8G8R8X8_UNORM_SRGB=93,DXGI_FORMAT_R10G10B10A2_UNORM=24,DXGI_FORMAT_R16G16B16A16_FLOAT=10};
static unsigned checks,opens,queries,imports,attachments,releases,escapes,deallocations,host_releases,recreated;
static HRESULT last_error;static bool fail_open,fail_query,fail_release;static unsigned desc_mutation;
#define CHECK(x) do {++checks;if(!(x)){fprintf(stderr,"FAIL standard line %u: %s\n",__LINE__,#x);exit(1);}}while(0)
// PACKED_TYPES
// BRIDGE_DESC
typedef struct {void *pDrvPrivate;} D3D10DDI_HDEVICE,D3D10DDI_HRESOURCE;
typedef uintptr_t D3D10DDI_HRTRESOURCE;
typedef void ID3D11Resource;
typedef struct {UINT Count,Quality;} DXGI_SAMPLE_DESC;
typedef struct {UINT NumAllocations;D3DKMT_HANDLE *HandleList;} D3DDDICB_DEALLOCATE;
typedef struct {UINT Type;ULONGLONG Size;union {VIOGPU_RESOURCE_3D_OPTIONS Options3D;VIOGPU_RESOURCE_SHARED_TEXTURE_OPTIONS OptionsShared;};} VIOGPU_CREATE_ALLOCATION_EXCHANGE;
typedef struct {D3DKMT_HANDLE hAllocation;UINT PrivateDriverDataSize;void *pPrivateDriverData;} OPENINFO;
typedef struct {UINT NumAllocations;OPENINFO *pOpenAllocationInfo;} D3D10DDIARG_OPENRESOURCE;
typedef struct {UINT Type,DataLength;struct {UINT ResHandle,IsCreated,Id;} ResourceInfo;} VIOGPU_ESCAPE;
#define VIOGPU_RES_INFO 1
typedef struct {void (*pfnStateVsSrvCb)(void*,UINT,UINT),(*pfnStateGsSrvCb)(void*,UINT,UINT),(*pfnStatePsSrvCb)(void*,UINT,UINT),(*pfnStateHsSrvCb)(void*,UINT,UINT),(*pfnStateDsSrvCb)(void*,UINT,UINT),(*pfnStateCsSrvCb)(void*,UINT,UINT),(*pfnStateCsUavCb)(void*,UINT,UINT);void (*pfnStateOmRenderTargetsCb)(void*);} D3D11DDI_CORELAYER_DEVICECALLBACKS;
typedef struct {void *pDev1;struct {HRESULT(*pfnDeallocateCb)(uintptr_t,D3DDDICB_DEALLOCATE*);} KTCallbacks;struct {uintptr_t handle;} hRTDevice;UINT lastFlipChainLength,uIfVersion;void *hRTCoreLayer;const D3D11DDI_CORELAYER_DEVICECALLBACKS *pUMCallbacks;} DEVICE,*PTRITON_DEVICE;
typedef struct {D3D10DDI_HRTRESOURCE hRTResource;DXGI_FORMAT Format,HostFormat,PresentFormat;UINT Width,Height,Depth,MipLevels,ArraySize,BindFlags,MiscFlags;DXGI_SAMPLE_DESC SampleDesc;BOOL IsShared,IsPresentable,BorrowedKMAllocation;D3DKMT_HANDLE hKMAllocation,hImportAlloc,hImportResKmt;ID3D11Resource *pResource,*pPresentResource;} RESOURCE,TRITON_RESOURCE,*PTRITON_RESOURCE;
typedef struct {uintptr_t hDevice;UINT Resources;uintptr_t *pResources;} DXGI_DDI_ARG_ROTATE_RESOURCE_IDENTITIES;
static int object;
static void tritonSetError(PTRITON_DEVICE p,HRESULT h){last_error=h;}
static void tritonPresentEnsureRuntimeCtx(PTRITON_DEVICE p){}
static HRESULT tritonPresentEscape(PTRITON_DEVICE p,VIOGPU_ESCAPE *e){++escapes;CHECK(e->ResourceInfo.ResHandle==77);e->ResourceInfo.IsCreated=1;e->ResourceInfo.Id=99;return S_OK;}
static bool tritonSharedBridgeImportRes(void *dev,UINT id,uint64_t size,UINT *a,UINT *r){CHECK(id==99&&size==1920000);++imports;++attachments;*a=91;*r=92;return true;}
static bool tritonSharedBridgeReleaseImportRes(void *dev,UINT a,UINT r){CHECK(a==91&&r==92);++releases;if(fail_release)return false;CHECK(attachments);--attachments;return true;}
static bool tritonSharedBridgeQueryRes(void *dev,UINT id,struct triton_shared_texture_desc *d){
 ++queries;CHECK(id==99);if(fail_query)return false;*d=(struct triton_shared_texture_desc){0};d->width=800;d->height=600;d->mip_levels=d->array_size=d->sample_count=d->plane_count=1;d->format=28;d->allocation_size=1996800;d->planes[0].pitch=3328;d->bind_flags=40;
 switch(desc_mutation){case 1:d->width=801;break;case 2:d->sample_count=2;break;case 3:d->format=24;break;case 4:d->plane_count=2;break;case 5:d->bind_flags=8;break;}return true;
}
static void *tritonSharedBridgeOpenRes(void *dev,UINT id,const struct triton_shared_texture_desc *d){++opens;CHECK(id==99);if(queries)CHECK(d->planes[0].pitch==3328&&d->allocation_size==1996800&&d->format==28);else CHECK(d->planes[0].pitch==3200);return fail_open?NULL:&object;}
static void ID3D11Resource_Release(void *r){CHECK(r==&object);++host_releases;}
static HRESULT deallocate(uintptr_t h,D3DDDICB_DEALLOCATE *d){CHECK(d->NumAllocations==1&&*d->HandleList==55);++deallocations;return S_OK;}
static HRESULT tritonResourceRecreateViews(PTRITON_DEVICE p,PTRITON_RESOURCE r){CHECK(r->pResource);++recreated;return S_OK;}
// PRODUCTION
static void reset(void){CHECK(!attachments);opens=queries=imports=releases=escapes=deallocations=host_releases=recreated=0;last_error=S_OK;fail_open=fail_query=fail_release=false;desc_mutation=0;}
static VIOGPU_RESOURCE_3D_OPTIONS standard(UINT flags){return (VIOGPU_RESOURCE_3D_OPTIONS){2,1,(1u<<1)|(1u<<3)|(1u<<7)|(1u<<18),800,600,1,1,0,0,flags};}
int main(void){
 RESOURCE poisoned;memset(&poisoned,0xcd,sizeof(poisoned));create_initialize_flags(&poisoned);CHECK(!poisoned.BorrowedKMAllocation&&!poisoned.HostFormat);
 VIOGPU_RESOURCE_SHARED_TEXTURE_OPTIONS so;VIOGPU_RESOURCE_3D_OPTIONS options=standard(4);
 CHECK(tritonSynthesizeStandardTexture(&options,1920000,&so));
#ifdef NPT_D3D10_RUNTIME_DDI
 CHECK(so.planes[0].pitch==3200&&so.allocation_size==1920000);
 for(unsigned i=0;i<13;++i){VIOGPU_RESOURCE_3D_OPTIONS bad=options;ULONGLONG size=1920000;switch(i){case 0:bad.width=0;break;case 1:bad.height=4097;break;case 2:bad.target=3;break;case 3:bad.depth=2;break;case 4:bad.array_size=2;break;case 5:bad.last_level=1;break;case 6:bad.nr_samples=1;break;case 7:bad.bind=0;break;case 8:bad.flags=5;break;case 9:--size;break;case 10:++size;break;case 11:bad.format=999;break;case 12:bad.flags=VIOGPU_RESOURCE_FLAG_STANDARD_PRIMARY;bad.format=67;break;}CHECK(!tritonSynthesizeStandardTexture(&bad,size,&so));}
#else
 CHECK(so.planes[0].pitch==3328&&so.allocation_size==1996800);
 puts("Standard import modern shadow layout preserved");return 0;
#endif
 DEVICE device={0};device.pDev1=&device;device.KTCallbacks.pfnDeallocateCb=deallocate;D3D10DDI_HDEVICE hd={&device};
 VIOGPU_CREATE_ALLOCATION_EXCHANGE ax={0};ax.Type=0;ax.Size=1920000;ax.Options3D=standard(VIOGPU_RESOURCE_FLAG_STANDARD_PRIMARY);OPENINFO info={77,sizeof(ax),&ax};D3D10DDIARG_OPENRESOURCE args={1,&info};RESOURCE r={0};D3D10DDI_HRESOURCE hr={&r};
 reset();tritonOpenResource(hd,&args,hr,7);CHECK(last_error==S_OK&&opens==1&&queries==1&&r.pResource==&object);CHECK(r.hKMAllocation==77&&r.BorrowedKMAllocation&&r.HostFormat==28&&r.Format==87&&r.hRTResource==7);
 CHECK(tritonResourceHostViewFormat(&r,87)==28);CHECK(tritonResourceHostViewFormat(&r,91)==29);CHECK(tritonResourceHostViewFormat(&r,24)==24);
 r.HostFormat=0;CHECK(tritonResourceRecreatedViewFormat(&r,28)==87);CHECK(tritonResourceRecreatedViewFormat(&r,29)==91);CHECK(tritonResourceHostViewFormat(&r,28)==28);r.HostFormat=28;CHECK(tritonResourceRecreatedViewFormat(&r,87)==28);
 tritonDestroyResource(hd,hr);CHECK(!deallocations&&!r.hKMAllocation&&!attachments&&host_releases==1);
 for(unsigned i=1;i<=7;++i){reset();memset(&r,0,sizeof(r));desc_mutation=i<=5?i:0;fail_query=i==6;fail_open=i==7;tritonOpenResource(hd,&args,hr,7);CHECK(last_error==E_FAIL&&!r.pResource&&!attachments&&r.hKMAllocation==77);tritonDestroyResource(hd,hr);CHECK(!deallocations);}
 reset();memset(&r,0,sizeof(r));fail_open=fail_release=true;tritonOpenResource(hd,&args,hr,7);CHECK(last_error==E_FAIL&&attachments==1&&r.hImportAlloc==91);fail_release=false;tritonDestroyResource(hd,hr);CHECK(!attachments&&!deallocations);
 reset();memset(&r,0,sizeof(r));ax.Options3D=standard(4);tritonOpenResource(hd,&args,hr,7);CHECK(last_error==S_OK&&!queries&&!r.BorrowedKMAllocation&&!r.hKMAllocation);tritonDestroyResource(hd,hr);CHECK(!deallocations);
 reset();memset(&r,0,sizeof(r));ax.Options3D=standard(5);tritonOpenResource(hd,&args,hr,7);CHECK(last_error==E_INVALIDARG&&!escapes&&!imports);
 /* Allocation ownership and physical format travel with backing identities. */
 reset();RESOURCE a={0},b={0};a.pResource=b.pResource=&object;a.hKMAllocation=77;a.BorrowedKMAllocation=1;a.HostFormat=28;b.hKMAllocation=55;b.HostFormat=87;uintptr_t resources[]={(uintptr_t)&a,(uintptr_t)&b};DXGI_DDI_ARG_ROTATE_RESOURCE_IDENTITIES rotation={(uintptr_t)&device,2,resources};
 CHECK(tritonDxgiRotateResourceIdentities(&rotation)==S_OK&&recreated==2);CHECK(a.hKMAllocation==55&&!a.BorrowedKMAllocation&&a.HostFormat==87);CHECK(b.hKMAllocation==77&&b.BorrowedKMAllocation&&b.HostFormat==28);
 tritonDestroyResource(hd,(D3D10DDI_HRESOURCE){&a});tritonDestroyResource(hd,(D3D10DDI_HRESOURCE){&b});CHECK(deallocations==1);
 printf("Standard import %u checks passed\n",checks);return 0;
}
