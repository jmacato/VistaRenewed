/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Private declarations for the Vista-only D3D9 UMD.  This target is kept
 * separate from Triton's D3D10/11 UMD on purpose: d3dumddi.h lays out both
 * the device table and several callback structures from the compile-time
 * D3D_UMD_INTERFACE_VERSION.  Building these TUs at a newer version would
 * hand Vista a longer, incompatible table.
 */

#ifndef TRITON9_H_INCLUDED
#define TRITON9_H_INCLUDED

#ifndef DXGKDDI_INTERFACE_VERSION
#define DXGKDDI_INTERFACE_VERSION 0x1052 /* DXGKDDI_INTERFACE_VERSION_VISTA */
#endif
#ifndef D3D_UMD_INTERFACE_VERSION
#define D3D_UMD_INTERFACE_VERSION 0x000C /* D3D_UMD_INTERFACE_VERSION_VISTA */
#endif

#define COBJMACROS

#include <windows.h>
/* d3dkmthk.h uses NTSTATUS in its user-mode callback declarations. */
#include <winternl.h>
#include <d3d9types.h>
#include <d3dumddi.h>
#include <d3d9caps.h>
#include <d3d11_4.h>
#include <dxgiformat.h>

#include <stdbool.h>
#include <stdint.h>

#include "virtio/virtio-gpu/wddm_hw.h"

#define TRITON9_MAX_VERTEX_STREAMS 16u
#define TRITON9_MAX_CONSTANT_BUFFERS 6u
#define TRITON9_MAX_PIXEL_SAMPLERS 16u
#define TRITON9_MAX_VERTEX_SAMPLERS 4u
#define TRITON9_VERTEX_SAMPLER_BASE 17u
#define TRITON9_MAX_TEXTURE_STAGES 21u
#define TRITON9_MAX_RENDER_TARGETS 4u
#define TRITON9_FIXED_TEXTURE_STAGES 8u
#define TRITON9_RENDER_STATE_COUNT 210u
#define TRITON9_TEXTURE_STAGE_STATE_COUNT 35u

typedef struct TRITON9_CONSTANT_BUFFER {
    BYTE                       *data;
    UINT                        byteCount;
    ID3D11Buffer               *hostBuffer;
    BOOL                        dirty;
} TRITON9_CONSTANT_BUFFER;

#ifdef __cplusplus
extern "C" {
#endif

void triton9Diag(const char *message);
void triton9DiagU32(const char *tag, DWORD value);
/* A bounded, text-only probe channel. Unlike the general checked-build DDI
 * trace it cannot be displaced by DWM's steady-state resource traffic. */
void triton9ProofDiagU32(const char *tag, DWORD value);

HRESULT APIENTRY
OpenAdapter(D3DDDIARG_OPENADAPTER *args);

/* Bind Neptune to the D3D9 runtime device already being created.  Reopening
 * it through D3DKMTCreateDevice from this callback deadlocks Vista. */
void npt_renderer_bind_d3d9_runtime(
    HANDLE hRTAdapter, HANDLE hRTDevice,
    UINT interfaceVersion, UINT runtimeVersion,
    const D3DDDI_ADAPTERCALLBACKS *adapterCallbacks,
    const D3DDDI_DEVICECALLBACKS *deviceCallbacks);

/* The Vista UMD creates its own opaque handles.  Never cast a runtime handle
 * to a resource/device pointer after replacing it in a DDI argument. */
typedef struct TRITON9_ADAPTER {
    HANDLE                      hRTAdapter;
    D3DDDI_ADAPTERCALLBACKS     callbacks;
    VIOGPU_ADAPTERINFO_V2       info;
    /* Do not query the private KMD contract from OpenAdapter.  Vista loads
     * d3d9.dll while holding its adapter-enumeration lock, and a synchronous
     * callback at that point can re-enter dxgkrnl before the HAL is published.
     * Cache that validation at the first real device operation instead. */
    INIT_ONCE                    privateInfoOnce;
    HRESULT                      privateInfoHr;
} TRITON9_ADAPTER;

typedef struct TRITON9_RESOURCE {
    /* The D3D9 runtime handle is a process pointer, but it is valid only for
     * the device that created or opened this resource. */
    HANDLE                      hOwnerDevice;
    HANDLE                      hRTResource;
    D3DKMT_HANDLE               hKMAllocation;
    /* Export ownership survives failed allocation callbacks and cleanup.
     * This reference can be the resolve wrapper rather than hostResource. */
    UINT64                      pendingExportBlob;
    ID3D11Resource             *pendingExportResource;
    struct TRITON9_RESOURCE    *failedNext;
    D3DKMT_HANDLE               hImportAllocation;
    D3DKMT_HANDLE               hImportResource;
    D3DDDIFORMAT                format;
    DXGI_FORMAT                 hostFormat;
    UINT                        width;
    UINT                        height;
    UINT                        depth;
    UINT                        mipLevels;
    UINT                        surfaceCount;
    /* Primary flip-chain entries have independent backing and lifetimes. */
    struct TRITON9_RESOURCE    **chainSurfaces;
    BOOL                        independentAllocation;
    /* All texture aliases share the root's single host allocation. Each
     * materialized alias owns a COM reference; primary chains remain separate. */
    struct TRITON9_RESOURCE     *textureOwner;
    struct TRITON9_RESOURCE    **textureSurfaces;
    UINT                        subresourceIndex, mipLevel, arraySlice;
    UINT                        exposedMipLevels, arraySize;
    UINT                        sampleCount, sampleQuality;
    BOOL                        nonMaskable;
    UINT                        blockWidth, blockHeight, bytesPerBlock;
    BOOL                        isTexture, isCube, isVolume;
    BOOL                        dynamic;
    BOOL                        autogenGenerating, autogenDirty;
    UINT                        autogenFilter;
    ID3D11ShaderResourceView    *srgbShaderResourceView;
    ID3D11RenderTargetView      *srgbRenderTargetView;
    ID3D11Resource              *resolveResource;
    /* The D3D runtime assigns this resource to a VidPn source.  Preserve it
     * in every later allocation callback instead of assuming source zero. */
    UINT                        vidPnSourceId;
    UINT                        bytesPerPixel;
    UINT                        rowBytes;
    UINT                        pitch;
    UINT                        slicePitch;
    UINT                        fvf;
    SIZE_T                      shadowSize;
    BYTE                       *shadow;
    /* A SYSTEMMEM buffer's last submitted bytes. Vista can modify its alias
     * without advancing contentSerial, so every refresh compares the bytes. */
    BYTE                       *systemMemorySnapshot;
    SIZE_T                      systemMemorySnapshotSize;
    ID3D11Resource             *systemMemorySnapshotHost;
    UINT64                      systemMemorySnapshotSerial;
    ID3D11Resource             *hostResource;
    /* hostResource can exist while shared-allocation registration or an
     * initial upload is incomplete.  Only hostReady proves the full lazy
     * materialization transaction completed. */
    BOOL                        hostReady;
    ID3D11Resource             *stagingResource;
    void                       *fvfDeclaration;
    ID3D11RenderTargetView     *renderTargetView;
    ID3D11DepthStencilView     *depthStencilView;
    ID3D11ShaderResourceView   *shaderResourceView;
    UINT                        hostBindFlags;
    BOOL                        isPrimary;
    BOOL                        isShared;
    D3DDDI_POOL                 pool;
    /* For SYSTEMMEM, shadow aliases Vista's pSysMem and must never be freed.
     * Other lockable resources own their private CPU backing store. */
    BOOL                        ownsShadow;
    BOOL                        isDepthStencil;
    BOOL                        wantsRenderTarget;
    BOOL                        wantsAutogenMipmap;
    /* A Present source must have a KMD allocation even when Vista declares
     * it as an ordinary swap-chain back buffer. Flip-chain primaries also
     * belong here: each rendered allocation must be directly scannable.
     * A standalone standard primary remains a separate non-blob destination. */
    BOOL                        needsPresentAllocation;
    /* A primary opened from the KMD's standard-allocation path has a KMD
     * handle but no D3D11/Neptune texture.  It is the scanout destination,
     * never the exported DWM image. */
    BOOL                        isStandardPrimary;
    BOOL                        hasInitialData;
    BOOL                        ownsKMAllocation;
    BOOL                        kmResourceAssociated;
    BOOL                        isBuffer;
    BOOL                        notLockable;
    BOOL                        writeOnly;
    /* This D3D9 resource declaration permits a subsequent lock to remain
     * bound for drawing.  The CPU shadow remains authoritative while locked,
     * so draw preparation explicitly pushes it to the D3D11 buffer. */
    BOOL                        canDrawWhileLocked;
    BOOL                        locked;
    BOOL                        uploadOnUnlock;
    BOOL                        lockRangeValid;
    BOOL                        lockAreaValid;
    BOOL                        lockBoxValid;
    D3DDDIBOX                   lockBox;
    D3DDDIRANGE                 lockRange;
    RECT                        lockArea;
    void                       *pendingRename;
    /* Event-backed DONOTWAIT readback snapshots are invalidated by any write
     * to the canonical texture, including writes through another alias. */
    UINT64                      contentSerial, readbackSerial;
    ID3D11Query                 *readbackQuery;
    BOOL                        readbackPending, readbackReady;
} TRITON9_RESOURCE;

static inline TRITON9_RESOURCE *
triton9ResourceSurface(TRITON9_RESOURCE *resource, UINT index)
{
    if (!resource || index >= resource->surfaceCount)
        return NULL;
    if (resource->textureSurfaces)
        return resource->textureSurfaces[index];
    return resource->chainSurfaces ? resource->chainSurfaces[index] :
                                    (index == 0 ? resource : NULL);
}

static inline TRITON9_RESOURCE *
triton9ResourceRoot(TRITON9_RESOURCE *resource)
{
    return resource && resource->textureOwner ? resource->textureOwner : resource;
}

static inline void
triton9ResourceWritten(TRITON9_RESOURCE *resource)
{
    TRITON9_RESOURCE *root = triton9ResourceRoot(resource);
    if (root) {
        if (!++root->contentSerial) ++root->contentSerial;
        if (root->wantsAutogenMipmap && !root->autogenGenerating) root->autogenDirty = TRUE;
    }
}

static inline BOOL
triton9ResourcesShareBacking(TRITON9_RESOURCE *a, TRITON9_RESOURCE *b)
{
    return a && b && triton9ResourceRoot(a) == triton9ResourceRoot(b);
}

static inline SIZE_T
triton9ResourceByteOffset(const TRITON9_RESOURCE *r, UINT x, UINT y, UINT z)
{
    UINT bw = r->blockWidth ? r->blockWidth : 1;
    UINT bh = r->blockHeight ? r->blockHeight : 1;
    UINT bytes = r->bytesPerBlock ? r->bytesPerBlock : r->bytesPerPixel;
    return (SIZE_T)z * r->slicePitch + (SIZE_T)(y / bh) * r->pitch + (SIZE_T)(x / bw) * bytes;
}

static inline BOOL
triton9ResourceBoxValid(const TRITON9_RESOURCE *r, const D3D11_BOX *box)
{
    UINT bw = r->blockWidth ? r->blockWidth : 1;
    UINT bh = r->blockHeight ? r->blockHeight : 1;
    return box && box->left < box->right && box->top < box->bottom &&
        box->front < box->back && box->right <= r->width &&
        box->bottom <= r->height && box->back <= r->depth &&
        !(box->left % bw) && !(box->top % bh) &&
        (!(box->right % bw) || box->right == r->width) &&
        (!(box->bottom % bh) || box->bottom == r->height);
}

typedef struct TRITON9_LIGHT {
    UINT index;
    D3DDDI_LIGHT data;
    BOOL enabled;
    struct TRITON9_LIGHT *next;
} TRITON9_LIGHT;

static inline BOOL
triton9ResourceIsSystemMemory(const TRITON9_RESOURCE *resource)
{
    return resource && resource->pool == D3DDDIPOOL_SYSTEMMEM;
}

typedef struct TRITON9_DEVICE {
    TRITON9_RESOURCE *failedResources;
    HANDLE                      hRTDevice;
    TRITON9_ADAPTER            *adapter;
    D3DDDI_DEVICECALLBACKS     callbacks;
    /* The D3D9 runtime owns CreateDevice.  Do not start the Neptune proxy
     * from that callback: its transport bootstrap submits runtime callbacks
     * and can only run after the runtime has published this device. */
    UINT                        interfaceVersion;
    UINT                        runtimeVersion;
    ID3D11Device1             *hostDevice;
    ID3D11DeviceContext1      *hostContext;
    BOOL                      hostDitherEnabled;
    D3D_FEATURE_LEVEL          featureLevel;
    HANDLE                      hKMContext;
    void                       *kmCommandBuffer;
    UINT                        kmCommandBufferSize;
    D3DDDI_ALLOCATIONLIST       *kmAllocationList;
    UINT                        kmAllocationListSize;
    D3DDDI_PATCHLOCATIONLIST    *kmPatchLocationList;
    UINT                        kmPatchLocationListSize;
    BOOL                        presentConsumptionReported;
    /* Auto-reset event reused under shaderLock; one outstanding Present. */
    HANDLE                      presentConsumptionEvent;
    BOOL                        runtimeContextInitialized;
    UINT                        runtimeContextId;
    BOOL                        deviceLost;
    CRITICAL_SECTION            kmContextLock;
    BOOL                        kmContextLockInitialized;
    CRITICAL_SECTION            hostInitLock;
    BOOL                        hostInitLockInitialized;
    CRITICAL_SECTION            shaderLock;
    BOOL                        shaderLockInitialized;
    void                       *activeVertexShader;
    void                       *activePixelShader;
    void                       *activeVertexDeclaration;
    /* These pointers describe the shaders actually bound for the most recent
     * draw preparation.  They can be application shaders or the guarded
     * fixed-function fallback shaders. */
    void                       *drawVertexShader;
    void                       *drawVertexDeclaration;
    void                       *fixedFunctionShaders;
    void                       *stretchBlitState;
    TRITON9_RESOURCE           *streamResources[TRITON9_MAX_VERTEX_STREAMS];
    UINT                        streamOffsets[TRITON9_MAX_VERTEX_STREAMS];
    UINT                        streamStrides[TRITON9_MAX_VERTEX_STREAMS];
    /* D3D9's stream divider is state, not resource metadata.  Keep the
     * unmodified D3DSTREAMSOURCE_* value so a layout can map it to D3D11's
     * input-slot classification when the next draw is prepared. */
    UINT                        streamSourceFrequencies[TRITON9_MAX_VERTEX_STREAMS];
    UINT64                      streamFrequencyGeneration;
    /* The runtime supplies UP data as transient user-memory pointers.  Keep
     * only the pointer until the matching draw callback copies it into the
     * reusable D3D11 dynamic buffer. */
    const VOID                 *upVertexData[TRITON9_MAX_VERTEX_STREAMS];
    ID3D11Buffer               *upVertexBuffers[TRITON9_MAX_VERTEX_STREAMS];
    UINT                        upVertexCapacities[TRITON9_MAX_VERTEX_STREAMS];
    UINT                        upVertexBytes[TRITON9_MAX_VERTEX_STREAMS];
    UINT                        drawStreamOffsets[TRITON9_MAX_VERTEX_STREAMS];
    BOOL                        drawStreamOffsetOverrides[TRITON9_MAX_VERTEX_STREAMS];
    TRITON9_RESOURCE           *indexResource;
    UINT                        indexStride;
    const VOID                 *upIndexData;
    UINT                        upIndexStride;
    ID3D11Buffer               *upIndexBuffer;
    UINT                        upIndexCapacity;
    TRITON9_CONSTANT_BUFFER     vertexConstants[TRITON9_MAX_CONSTANT_BUFFERS];
    TRITON9_CONSTANT_BUFFER     pixelConstants[TRITON9_MAX_CONSTANT_BUFFERS];
    TRITON9_CONSTANT_BUFFER     fixedVertexFloatConstants;
    TRITON9_CONSTANT_BUFFER     fixedPixelFloatConstants;
    TRITON9_RESOURCE           *renderTarget; /* RT0 compatibility alias */
    TRITON9_RESOURCE           *renderTargets[TRITON9_MAX_RENDER_TARGETS];
    TRITON9_RESOURCE           *depthStencil;
    UINT                        renderStates[TRITON9_RENDER_STATE_COUNT];
    /* Keep the material supplied by the D3D9 runtime even while the current
     * fixed-function translator is using its unlit composition path.  Vista
     * MIL initializes this state before it issues any draw, and retaining it
     * also makes a later lighting implementation a state-extension rather
     * than a device-contract change. */
    D3DDDIARG_SETMATERIAL       material;
    UINT                        textureStageStates[TRITON9_MAX_TEXTURE_STAGES]
                                              [TRITON9_TEXTURE_STAGE_STATE_COUNT];
    TRITON9_RESOURCE           *textures[TRITON9_MAX_TEXTURE_STAGES];
    ID3D11SamplerState         *samplerStates[TRITON9_MAX_TEXTURE_STAGES];
    ID3D11BlendState           *blendState;
    ID3D11DepthStencilState    *depthStencilState;
    ID3D11RasterizerState      *rasterizerState;
    D3D11_VIEWPORT              viewport;
    RECT                        scissorRect;
    BOOL                        viewportSet;
    BOOL                        zRangeSet;
    BOOL                        scissorSet;
    BOOL                        blendStateDirty;
    BOOL                        depthStencilStateDirty;
    BOOL                        rasterizerStateDirty;
    BOOL                        samplerStatesDirty[TRITON9_MAX_TEXTURE_STAGES];
    D3DMATRIX                   worldTransform;
    D3DMATRIX                   worldTransforms[256];
    FLOAT                       clipPlanes[6][4];
    BOOL                        wFogEnable;
    TRITON9_LIGHT              *lights;
    void                       *pointState;
    D3DMATRIX                   viewTransform;
    D3DMATRIX                   projectionTransform;
    D3DMATRIX                   textureTransforms[TRITON9_FIXED_TEXTURE_STAGES];
    /* Internal Present completion gate.  It is separate from application
     * D3D9 query handles and is reused for the lifetime of this device. */
    ID3D11Fence                 *presentFence;
    ID3D11DeviceContext4        *presentContext;
    UINT64                      presentFenceValue;
    UINT                        presentProfileState;
    UINT                        presentProfileFrames;
    UINT64                      presentProfileTicks[4];
    UINT64                      presentProfileMaxTicks[4];
    void                       *traceMap;
    HANDLE                      traceMapHandle;
    DWORD                       traceLastOpen;
    BOOL                        traceTriedOpen;
    UINT64                      traceRun, traceFrame;
    UINT                        traceContext;
    ID3D11Query                 *traceGpuBegin, *traceGpuEnd, *traceGpuDisjoint;
    UINT64                      traceGpuRun;
    BOOL                        traceGpuArmed;
    UINT                        traceGpuEvery, traceGpuSampleCount;
    UINT64                      traceGpuSampleRun;
    void                       *queries;
} TRITON9_DEVICE;

BOOL triton9TraceEnabled(TRITON9_DEVICE *device);
void triton9TraceBegin(TRITON9_DEVICE *device, UINT64 ticks, UINT flags, UINT kind);
void triton9TraceEventAt(TRITON9_DEVICE *device, UINT kind, UINT64 ticks, UINT64 a, UINT64 b, UINT64 c);
void triton9TraceEvent(TRITON9_DEVICE *device, UINT kind, UINT64 a, UINT64 b, UINT64 c);
void triton9TraceClose(TRITON9_DEVICE *device);
void triton9TraceGpuStart(TRITON9_DEVICE *device);
void triton9TraceGpuFinish(TRITON9_DEVICE *device);
void triton9TraceGpuResolve(TRITON9_DEVICE *device);
void triton9TraceGpuRelease(TRITON9_DEVICE *device);

static inline BOOL
triton9ResourceBelongsToDevice(const TRITON9_DEVICE *device,
                               const TRITON9_RESOURCE *resource)
{
    return device && resource &&
           resource->hOwnerDevice == (HANDLE)(uintptr_t)device;
}

typedef struct TRITON9_FORMAT {
    D3DDDIFORMAT                d3dFormat;
    DXGI_FORMAT                 hostFormat;
    UINT                        bytesPerPixel;
    UINT                        operations;
    BOOL                        depthStencil;
    UINT                        blockWidth, blockHeight, bytesPerBlock;
    DXGI_FORMAT                 resourceFormat, srgbFormat;
} TRITON9_FORMAT;

/* The format table calls this symbol before it exposes D24S8.  The clear
 * implementation returns TRUE only when creation, binding, plane-preserving
 * clears, ordered completion, and CPU readback form one complete contract. */
BOOL triton9HasCompleteD24S8ClearContract(void);

/* Neptune sends COM_RELEASE on the primary ring.  Imported-resource teardown
 * must drain that exact ring before it asks the KMD to detach the allocation. */
bool triton9DrainPrimaryTransport(void *pWrapper, uint32_t timeout_ms);
bool triton9ReleaseImportTransport(void *pWrapper, uint32_t alloc,
                                   uint32_t res_kmt);
bool triton9TransportHealthy(void *pWrapper);

const TRITON9_FORMAT *triton9FormatLookup(D3DDDIFORMAT format);
const TRITON9_FORMAT *triton9FormatLookupHost(DXGI_FORMAT format);
UINT triton9FormatCount(void);
void triton9CopyFormatOperations(FORMATOP *destination, UINT count);
BOOL triton9FormatSupportsSrgb(D3DDDIFORMAT format);
UINT triton9FormatMultisampleQuality(D3DDDIFORMAT format, UINT samples);
HRESULT triton9GetShaderResourceViewEx(TRITON9_DEVICE *device, TRITON9_RESOURCE *resource,
                                     BOOL srgb, ID3D11ShaderResourceView **view);
HRESULT triton9GetRenderTargetViewEx(TRITON9_DEVICE *device, TRITON9_RESOURCE *resource,
                                   BOOL srgb, ID3D11RenderTargetView **view);
HRESULT triton9BindOutputs(TRITON9_DEVICE *device);
HRESULT triton9CopyStagingBoxToShadow(TRITON9_DEVICE *device, TRITON9_RESOURCE *resource,
                                    const D3D11_BOX *box);
HRESULT triton9ResolveResource(TRITON9_DEVICE *device, TRITON9_RESOURCE *resource,
                              ID3D11Resource **host, UINT *subresource);
HRESULT APIENTRY triton9VolBlt(HANDLE hDevice, const D3DDDIARG_VOLUMEBLT *args);
void triton9DestroyLights(TRITON9_DEVICE *device);

/* Neptune's internal D3D11 proxy creator.  It deliberately has no import
 * from d3d11.dll; it talks to Neptune's generated guest transport. */
HRESULT npt_d3d11_create_device_internal(IDXGIAdapter *pAdapter,
                                         D3D_DRIVER_TYPE driverType,
                                         HMODULE software,
                                         UINT flags,
                                         const D3D_FEATURE_LEVEL *featureLevels,
                                         UINT featureLevelCount,
                                         UINT sdkVersion,
                                         ID3D11Device **outDevice,
                                         D3D_FEATURE_LEVEL *outFeatureLevel,
                                         ID3D11DeviceContext **outContext);

HRESULT triton9EnsureKernelContext(TRITON9_DEVICE *device);
HRESULT triton9EnsureRuntimeContext(TRITON9_DEVICE *device);
HRESULT triton9Escape(TRITON9_DEVICE *device, VIOGPU_ESCAPE *escape);
HRESULT triton9EnsureHostDevice(TRITON9_DEVICE *device);

HRESULT APIENTRY triton9CreateResource(HANDLE hDevice,
                                       D3DDDIARG_CREATERESOURCE *args);
HRESULT triton9UpdateHostTexture(TRITON9_DEVICE *device, TRITON9_RESOURCE *resource,
    ID3D11DeviceContext *context, ID3D11Resource *destination, UINT subresource,
    const D3D11_BOX *box, const BYTE *source, UINT rowPitch, UINT slicePitch);
HRESULT APIENTRY triton9DestroyResource(HANDLE hDevice, HANDLE hResource);
HRESULT triton9DestroyFailedResources(TRITON9_DEVICE *device);
/* CreateDeviceEx allocates its bootstrap resources before it returns the
 * runtime device to the caller.  Materialize their Neptune backing only from
 * a later operational callback, after Vista has published that device. */
HRESULT triton9EnsureResourceHost(TRITON9_DEVICE *device,
                                  TRITON9_RESOURCE *resource);
HRESULT triton9PrepareResourceForHostRead(TRITON9_DEVICE *device,
                                          TRITON9_RESOURCE *resource);
HRESULT triton9PrepareBufferRangeForHostRead(TRITON9_DEVICE *device,
                                             TRITON9_RESOURCE *resource,
                                             UINT first, UINT end);
/* Propagate a completed write to Vista's SYSTEMMEM backing into an existing
 * host mirror.  This never creates a mirror for a CPU-only resource. */
HRESULT triton9CommitSystemMemoryWrite(TRITON9_DEVICE *device,
                                       TRITON9_RESOURCE *resource);
HRESULT triton9EnsureStagingResource(TRITON9_DEVICE *device,
                                     TRITON9_RESOURCE *resource);
HRESULT triton9CopyStagingSurfaceToShadow(TRITON9_DEVICE *device,
                                          TRITON9_RESOURCE *resource,
                                          const RECT *region);
HRESULT triton9CopyStagingBufferToShadow(TRITON9_DEVICE *device,
                                         TRITON9_RESOURCE *resource,
                                         UINT offset, UINT size);
HRESULT APIENTRY triton9Lock(HANDLE hDevice, D3DDDIARG_LOCK *args);
HRESULT APIENTRY triton9Unlock(HANDLE hDevice,
                               const D3DDDIARG_UNLOCK *args);
HRESULT APIENTRY triton9LockAsync(HANDLE hDevice,
                                  D3DDDIARG_LOCKASYNC *args);
HRESULT APIENTRY triton9UnlockAsync(HANDLE hDevice,
                                    const D3DDDIARG_UNLOCKASYNC *args);
HRESULT APIENTRY triton9Rename(HANDLE hDevice, const D3DDDIARG_RENAME *args);
/* The caller holds shaderLock.  Make CPU-shadow writes visible when Vista
 * legally draws from a buffer before its matching unlock. */
HRESULT triton9SynchronizeLockedBuffers(TRITON9_DEVICE *device);
HRESULT APIENTRY triton9SetDisplayMode(HANDLE hDevice,
                                       const D3DDDIARG_SETDISPLAYMODE *args);
HRESULT APIENTRY triton9Present(HANDLE hDevice,
                                const D3DDDIARG_PRESENT *args);
HRESULT APIENTRY triton9OpenResource(HANDLE hDevice,
                                     D3DDDIARG_OPENRESOURCE *args);
HRESULT triton9GetRenderTargetView(TRITON9_DEVICE *device,
                                   TRITON9_RESOURCE *resource,
                                   ID3D11RenderTargetView **view);
HRESULT triton9GetDepthStencilView(TRITON9_DEVICE *device,
                                   TRITON9_RESOURCE *resource,
                                   ID3D11DepthStencilView **view);
HRESULT triton9GetShaderResourceView(TRITON9_DEVICE *device,
                                     TRITON9_RESOURCE *resource,
                                     ID3D11ShaderResourceView **view);
HRESULT APIENTRY triton9SetRenderTarget(HANDLE hDevice,
                                        const D3DDDIARG_SETRENDERTARGET *args);
HRESULT APIENTRY triton9SetDepthStencil(HANDLE hDevice,
                                        const D3DDDIARG_SETDEPTHSTENCIL *args);
HRESULT APIENTRY triton9Clear(HANDLE hDevice, const D3DDDIARG_CLEAR *args,
                              UINT rectCount, const RECT *rects);
HRESULT APIENTRY triton9Blt(HANDLE hDevice, const D3DDDIARG_BLT *args);
HRESULT triton9CompleteRedirectedPresent(TRITON9_DEVICE *device);
HRESULT triton9StretchBlt(TRITON9_DEVICE *device, TRITON9_RESOURCE *source,
                          TRITON9_RESOURCE *destination,
                          const RECT *sourceRect, const RECT *destinationRect,
                          BOOL linear);
void triton9ReleaseStretchBlit(TRITON9_DEVICE *device);
HRESULT APIENTRY triton9ColorFill(HANDLE hDevice,
                                  const D3DDDIARG_COLORFILL *args);
HRESULT APIENTRY triton9GenerateMipSubLevels(
    HANDLE hDevice, const D3DDDIARG_GENERATEMIPSUBLEVELS *args);
HRESULT APIENTRY triton9SetRenderState(HANDLE hDevice,
                                       const D3DDDIARG_RENDERSTATE *args);
HRESULT APIENTRY triton9SetTransform(HANDLE hDevice,
                                     const D3DDDIARG_SETTRANSFORM *args);
HRESULT APIENTRY triton9MultiplyTransform(
    HANDLE hDevice, const D3DDDIARG_MULTIPLYTRANSFORM *args);
HRESULT APIENTRY triton9SetTextureStageState(
    HANDLE hDevice, const D3DDDIARG_TEXTURESTAGESTATE *args);
HRESULT APIENTRY triton9SetTexture(HANDLE hDevice, UINT stage, HANDLE resource);
HRESULT APIENTRY triton9SetViewport(HANDLE hDevice,
                                    const D3DDDIARG_VIEWPORTINFO *args);
HRESULT APIENTRY triton9SetZRange(HANDLE hDevice,
                                  const D3DDDIARG_ZRANGE *args);
HRESULT APIENTRY triton9SetScissorRect(HANDLE hDevice, const RECT *rect);
HRESULT APIENTRY triton9CreateQuery(HANDLE hDevice,
                                    D3DDDIARG_CREATEQUERY *args);
HRESULT APIENTRY triton9DestroyQuery(HANDLE hDevice, const HANDLE query);
HRESULT APIENTRY triton9IssueQuery(HANDLE hDevice,
                                   const D3DDDIARG_ISSUEQUERY *args);
HRESULT APIENTRY triton9GetQueryData(HANDLE hDevice,
                                     const D3DDDIARG_GETQUERYDATA *args);
void triton9DestroyAllQueries(TRITON9_DEVICE *device);
void triton9InitializePipelineState(TRITON9_DEVICE *device);
void triton9ReleasePipelineState(TRITON9_DEVICE *device);
HRESULT triton9PreparePipelineState(TRITON9_DEVICE *device);
HRESULT APIENTRY triton9CreateVertexShaderFunc(
    HANDLE hDevice, D3DDDIARG_CREATEVERTEXSHADERFUNC *args,
    const UINT *tokens);
HRESULT APIENTRY triton9DeleteVertexShaderFunc(HANDLE hDevice, HANDLE shader);
HRESULT APIENTRY triton9SetVertexShaderFunc(HANDLE hDevice, HANDLE shader);
HRESULT APIENTRY triton9CreateVertexShaderDecl(
    HANDLE hDevice, D3DDDIARG_CREATEVERTEXSHADERDECL *args,
    const D3DDDIVERTEXELEMENT *elements);
HRESULT APIENTRY triton9DeleteVertexShaderDecl(HANDLE hDevice,
                                                HANDLE declaration);
HRESULT APIENTRY triton9SetVertexShaderDecl(HANDLE hDevice,
                                             HANDLE declaration);
HRESULT triton9CreateFvfDeclaration(TRITON9_DEVICE *device, UINT fvf,
                                    void **outDeclaration);
void triton9DestroyFvfDeclaration(TRITON9_DEVICE *device, void *declaration);
HRESULT APIENTRY triton9SetStreamSource(HANDLE hDevice,
                                        const D3DDDIARG_SETSTREAMSOURCE *args);
HRESULT APIENTRY triton9SetStreamSourceUm(
    HANDLE hDevice, const D3DDDIARG_SETSTREAMSOURCEUM *args, const VOID *data);
HRESULT APIENTRY triton9SetStreamSourceFreq(
    HANDLE hDevice, const D3DDDIARG_SETSTREAMSOURCEFREQ *args);
HRESULT APIENTRY triton9SetIndices(HANDLE hDevice,
                                   const D3DDDIARG_SETINDICES *args);
HRESULT APIENTRY triton9SetIndicesUm(HANDLE hDevice, UINT stride,
                                     const VOID *data);
HRESULT APIENTRY triton9SetVertexShaderConst(
    HANDLE hDevice, const D3DDDIARG_SETVERTEXSHADERCONST *args,
    const VOID *data);
HRESULT APIENTRY triton9SetVertexShaderConstI(
    HANDLE hDevice, const D3DDDIARG_SETVERTEXSHADERCONSTI *args,
    const INT *data);
HRESULT APIENTRY triton9SetVertexShaderConstB(
    HANDLE hDevice, const D3DDDIARG_SETVERTEXSHADERCONSTB *args,
    const BOOL *data);
HRESULT APIENTRY triton9CreatePixelShader(HANDLE hDevice,
                                          D3DDDIARG_CREATEPIXELSHADER *args,
                                          const UINT *tokens);
HRESULT APIENTRY triton9DeletePixelShader(HANDLE hDevice, HANDLE shader);
HRESULT APIENTRY triton9SetPixelShader(HANDLE hDevice, HANDLE shader);
HRESULT APIENTRY triton9SetPixelShaderConst(
    HANDLE hDevice, const D3DDDIARG_SETPIXELSHADERCONST *args,
    const FLOAT *data);
HRESULT APIENTRY triton9SetPixelShaderConstI(
    HANDLE hDevice, const D3DDDIARG_SETPIXELSHADERCONSTI *args,
    const INT *data);
HRESULT APIENTRY triton9SetPixelShaderConstB(
    HANDLE hDevice, const D3DDDIARG_SETPIXELSHADERCONSTB *args,
    const BOOL *data);
HRESULT APIENTRY triton9DrawPrimitive(
    HANDLE hDevice, const D3DDDIARG_DRAWPRIMITIVE *args,
    const UINT *flags);
HRESULT APIENTRY triton9DrawIndexedPrimitive(
    HANDLE hDevice, const D3DDDIARG_DRAWINDEXEDPRIMITIVE *args);
HRESULT APIENTRY triton9DrawPrimitive2(
    HANDLE hDevice, const D3DDDIARG_DRAWPRIMITIVE2 *args);
HRESULT APIENTRY triton9DrawIndexedPrimitive2(
    HANDLE hDevice, const D3DDDIARG_DRAWINDEXEDPRIMITIVE2 *args,
    UINT stride, const VOID *data, const UINT *flags);
HRESULT APIENTRY triton9BufBlt(HANDLE hDevice,
                               const D3DDDIARG_BUFFERBLT *args);
HRESULT APIENTRY triton9TexBlt(HANDLE hDevice,
                               const D3DDDIARG_TEXBLT *args);
void triton9ReleaseConstantBuffers(TRITON9_DEVICE *device);
void triton9ReleaseFixedFunctionShaders(TRITON9_DEVICE *device);
void triton9ReleaseUpBuffers(TRITON9_DEVICE *device);

/* Install exact-signature error handlers for the regular D3D9 functions that
 * are not implemented yet.  Do not use casts from one generic handler here:
 * the Vista x86 DDI uses stdcall and each callback has a different stack
 * footprint.  Decode, video process, and overlay callbacks intentionally
 * remain NULL because the adapter reports none of those optional families. */
void triton9InstallUnsupportedDeviceFuncs(D3DDDI_DEVICEFUNCS *funcs);

static inline HRESULT triton9DeviceRemoved(TRITON9_DEVICE *device)
{
    if (device)
        device->deviceLost = TRUE;
    return D3DDDIERR_DEVICEREMOVED;
}

static inline HRESULT triton9MapDeviceFailure(TRITON9_DEVICE *device,
                                              HRESULT hr)
{
    if (hr == D3DDDIERR_DEVICEREMOVED || hr == DXGI_ERROR_DEVICE_REMOVED ||
        hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG)
        return triton9DeviceRemoved(device);
    return hr;
}

static inline HRESULT triton9CheckHostDevice(TRITON9_DEVICE *device)
{
    HRESULT hr;
    IUnknown *transportObject;

    hr = triton9EnsureHostDevice(device);
    if (FAILED(hr))
        return hr;
    transportObject = device->hostContext ? (IUnknown *)device->hostContext
                                          : (IUnknown *)device->hostDevice;
    if (!triton9TransportHealthy(transportObject))
        return triton9DeviceRemoved(device);
    return S_OK;
}

/* The corrected GetDeviceRemovedReason override is a synchronous RPC. Calling it
 * after every state change/draw drains the command stream and defeats async
 * submission. Poll at Present/Flush boundaries; hot DDIs still detect a
 * poisoned transport and propagate failures returned by their host calls. */
static inline HRESULT triton9PollHostDevice(TRITON9_DEVICE *device)
{
    HRESULT hr = triton9CheckHostDevice(device);
    if (FAILED(hr))
        return hr;
#ifdef __cplusplus
    hr = device->hostDevice->GetDeviceRemovedReason();
#else
    hr = ID3D11Device1_GetDeviceRemovedReason(device->hostDevice);
#endif
    if (FAILED(hr))
        return triton9DeviceRemoved(device);
    return triton9CheckHostDevice(device);
}

#ifdef __cplusplus
}
#endif

#endif /* TRITON9_H_INCLUDED */
