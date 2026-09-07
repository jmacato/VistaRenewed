/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * State structs and internal declarations shared across the Triton DDI
 * translation units. Each struct sits behind the corresponding
 * D3D10DDI_H* handle's pDrvPrivate; the runtime allocates storage via
 * CalcPrivate*Size and we populate it in the matching pfnCreate*.
 */

#ifndef TRITON_H_INCLUDED
#define TRITON_H_INCLUDED

/* The triton .c files use C-style COM method invocations (e.g.
 * ID3D11Device1_Release(p)); the COBJMACROS define switches d3d*.h
 * into emitting those wrapper macros. */
#define COBJMACROS

#include <windows.h>
/* d3dkmthk.h declares DDI signatures whose return type is NTSTATUS;
 * winternl.h is the standard user-mode source for that typedef. */
#include <winternl.h>
#include <d3dkmthk.h>

/* Suppress d3d10effect.h: Triton doesn't use the D3D10 effects runtime,
 * and the SDK header pulls in slot-count macros lazily in a way that
 * breaks under our include order. */
#define __D3D10EFFECT_H__

#include <d3d10umddi.h>
#include <d3d11_3.h>
/* d3d11_4.h adds ID3D11Device5 (CreateFence), ID3D11DeviceContext4
 * (Signal) and ID3D11Fence (SetEventOnCompletion) — the present GPU-done
 * fence used to gate the flip on real render completion. */
#include <d3d11_4.h>
/* DXGI DDI base-callback / present types (PFNDDXGIDDI_PRESENTCB,
 * DXGIDDICB_PRESENT); must follow d3d10umddi.h. */
#include <dxgiddi.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct TRITON_ADAPTER {
    D3D10DDI_HRTADAPTER          hRTAdapter;
} TRITON_ADAPTER, *PTRITON_ADAPTER;

struct TRITON_SHADER;
struct TRITON_ELEMENTLAYOUT;

typedef struct TRITON_DEVICE {
    D3D10DDI_HRTDEVICE              hRTDevice;
    UINT                            uIfVersion;
    PTRITON_ADAPTER                 pAdapter;
    D3DDDI_DEVICECALLBACKS          KTCallbacks;

    /* pDev1/pCtx1 come from the Neptune factory. pDev2/pDev3/pCtx2/pCtx3
     * are QI'd in tritonCreateDevice and may be NULL when the host stack
     * does not expose the newer interfaces. WDDM 1.3+ handlers check for
     * NULL and fall back to TR_STUB. */
    ID3D11Device1                  *pDev1;
    ID3D11Device2                  *pDev2;
    ID3D11Device3                  *pDev3;
    ID3D11DeviceContext1           *pCtx1;
    ID3D11DeviceContext2           *pCtx2;
    ID3D11DeviceContext3           *pCtx3;
    D3D_FEATURE_LEVEL               FeatureLevel;

    /* Currently-bound VS and element layout. Used to lazily create the
     * ID3D11InputLayout the first time both are known. Weak refs: the
     * owning TRITON_SHADER / TRITON_ELEMENTLAYOUT may be destroyed while
     * still referenced here; we clear from those destroy paths. */
    struct TRITON_SHADER           *pCurrentVS;
    struct TRITON_ELEMENTLAYOUT    *pCurrentLayout;

    /* The reconciled VS currently bound on the context, or NULL when the app's
     * own VS is bound.  A reconciled VS belongs to one layout; if a resolve
     * does not complete, the previous layout's copy would otherwise stay bound
     * and feed the draw mistyped vertex data. */
    ID3D11VertexShader             *pBoundReconVS;

    /* Monotonic per-device shader-instance counter. Each shader gets a
     * unique cookie at create; the input-layout cache keys on it so a
     * freed VS bytecode pointer reused at the same heap address cannot
     * alias a layout built against the old VS's input signature. */
    UINT64                          nextShaderCookie;

    /* Runtime callback handle + table. The DDI is mostly VOID-returning;
     * to signal asynchronous failures we route through pfnSetErrorCb. */
    D3D10DDI_HRTCORELAYER                            hRTCoreLayer;
    const D3D11DDI_CORELAYER_DEVICECALLBACKS        *pUMCallbacks;

    /* Kernel context for present submission, created lazily via
     * KTCallbacks.pfnCreateContextCb on the first primary-create or present
     * (tritonPresentEnsureKernelContext). pfnPresentCb (recorded from
     * DXGIBaseDDI.pDXGIBaseCallbacks) consumes it to reach the KMD's
     * DxgkDdiPresent / DxgkDdiSetVidPnSourceAddress. NULL until created:
     * present is then unavailable but rendering still works. */
    HANDLE                                           hKMContext;
    PFNDDXGIDDI_PRESENTCB                            pfnPresentCb;

    /* Set once tritonPresentEnsureRuntimeCtx has SUCCESSFULLY issued
     * VIOGPU_CTX_INIT for this device, giving its KMD context a valid virtio
     * context for RESOURCE_CREATE_BLOB / CTX_ATTACH_RESOURCE.  A failed
     * escape leaves it clear so the next resource create retries. */
    BOOL                                             RuntimeCtxInited;

    /* Present GPU-done fence: gates the flip on the producing context's
     * actual GPU completion.  The presentable blob and the host's render
     * target alias the same shmem across a process boundary, and Metal has
     * no implicit cross-device sync, so without an explicit fence the
     * KMD/DWM scans out a half-rendered backbuffer.  Lazily created on
     * first present; *Disabled if the host stack lacks
     * ID3D11Device5/ID3D11Fence, in which case present falls back to a bare
     * Flush. */
    ID3D11DeviceContext4           *pCtx4;
    ID3D11Fence                    *pPresentFence;
    UINT64                          presentFenceValue;
    /* Size of the flip chain's rotation set, recorded by
     * pfnRotateResourceIdentities; 0 until the first rotation.  Feeds the
     * run-ahead depth (see TRITON_PRESENT_MAX_AHEAD).  Plain UINT store:
     * advisory, single-writer per swapchain, torn reads impossible. */
    UINT                            lastFlipChainLength;
    /* Last pacing depth reported to TR_LOG, so the depth line is emitted
     * only when it changes. */
    UINT                            lastPaceDepthLogged;
    BOOL                            presentFenceReady;
    BOOL                            presentFenceDisabled;
    /* Reusable auto-reset events for the GPU-done waits, guarded by
     * presentLock.  Each concurrent present waiter takes its own event: a
     * shared auto-reset event releases only one waiter per signal, so one
     * present thread would consume another's completion and strand it until
     * the timeout. */
#define TRITON_PRESENT_EVENT_POOL 8
    HANDLE                          presentEventPool[TRITON_PRESENT_EVENT_POOL];
    UINT                            presentEventPoolCount;
    BOOL                            presentEventPoolArmed[TRITON_PRESENT_EVENT_POOL];
    /* Serialises the present-fence protocol and every use of the shared
     * internal immediate context (lazy fence init/teardown, the fence-value
     * increment, Signal/Flush, SetEventOnCompletion, the event pool): DWM
     * composites several flip swapchains through one device, so the DXGI
     * present DDI runs concurrently on multiple present threads, and the
     * internal npt immediate context has no locking of its own.  The long
     * GPU-done wait runs outside this lock so concurrent presents overlap
     * their waits. */
    CRITICAL_SECTION                presentLock;
    /* Serialises pfnPresentCb and the lazy kernel-/runtime-context inits.
     * MSDN ("Changes from Direct3D 10"): only a single thread may work
     * against an HCONTEXT at a time, and when multiple threads call
     * pfnPresentCb with the same HCONTEXT "the driver must synchronize the
     * calls to the callback functions".  Separate from presentLock so a
     * kernel present blocked in dxgkrnl doesn't stall other presents' fence
     * signalling; never held together with presentLock. */
    CRITICAL_SECTION                kmCtxLock;
    /* TRUE once both critical sections above are initialised. */
    BOOL                            presentLockInit;
    /* DXGI DDI calls currently driving the present path (pfnPresent,
     * pfnPresent1, pfnBlt, pfnBlt1).  Such a call drops presentLock for the
     * GPU wait and then re-takes it, and drives pfnPresentCb under kmCtxLock,
     * so DestroyDevice must wait for this to reach zero before touching
     * anything it uses. */
    volatile LONG                   presentInFlight;

    /* Stretch/convert blit rig for the DXGI Blt DDI (tritonDxgiBltStretch):
     * a plain CopySubresourceRegion cannot stretch and the host silently
     * copies ZEROS across formats, so Convert/Stretch blts draw a
     * fullscreen triangle instead.  Lazily created under presentLock;
     * released by DestroyDevice.  blitInitFailed latches a failed init so
     * it is attempted once, falling back to the clamped copy. */
    ID3D11VertexShader             *pBlitVS;
    ID3D11PixelShader              *pBlitPS;
    ID3D11SamplerState             *pBlitSampler;
    ID3D11Buffer                   *pBlitCB;
    BOOL                            blitInitFailed;
} TRITON_DEVICE, *PTRITON_DEVICE;

/* Present GPU-done gate timing.  DestroyDevice's drain must outlast the gate's
 * own worst case, so both are derived from these.
 *
 * The deadline distinguishes "GPU is slow" from "device is gone"; only the
 * second justifies scanning out an unfinished frame.  A cold host shader
 * compile can legitimately outlast one budget, so it may be extended while the
 * device still reports healthy, bounded so a real hang still surfaces.  The
 * grace period is granted to one fresh registration once the extensions run
 * out with the fence still short: long enough to catch a merely late
 * completion, short enough not to add a second visible stall. */
#define TRITON_PRESENT_DEADLINE_MS    1000
/* Bounded run-ahead for KMD-gated (scanout-primary) presents: present N
 * sleeps until frame N-depth completes on the GPU.  Nothing else paces such
 * an app (sync-interval-0 flips complete on supersede, and the render
 * stream travels the shared-memory ring, invisible to dxgkrnl/DXGI
 * accounting); unpaced it runs seconds ahead of GPU execution -- the
 * pending-flip queue then never holds a retired frame and the display
 * freezes.  The depth is min(lastFlipChainLength, TRITON_PRESENT_MAX_AHEAD):
 * pfnRotateResourceIdentities' Resources count is the runtime's rotation
 * set for the flip chain -- all rotated back buffers, one extra in windowed
 * flip-sequential, doubled for stereo -- so it is always >= the app's
 * BufferCount and the min() can only tighten the bound when the chain
 * itself is shallower than the cap.  The cap matches DXGI's default
 * MaximumFrameLatency (which has no DDI channel): deep enough for full
 * CPU/GPU overlap, shallow enough that queued flips retire within a couple
 * of frames.  Mirrors npt_swapchain.c's latency ring
 * (cap = min(max_frame_latency, NPT_SC_PRESENT_LATENCY)). */
#define TRITON_PRESENT_MAX_AHEAD      3
#define TRITON_PRESENT_MAX_EXTENSIONS 4
#define TRITON_PRESENT_GRACE_MS       100

/* Bound on how long the gate can hold one present, plus a margin for the
 * pfnPresentCb that follows it. */
#define TRITON_PRESENT_DRAIN_MS \
    (TRITON_PRESENT_DEADLINE_MS * (TRITON_PRESENT_MAX_EXTENSIONS + 1) + \
     TRITON_PRESENT_GRACE_MS + 1000)

/* dxvk-native vendor MiscFlags bit (DXVK_D3D11_RESOURCE_MISC_LINEAR_EXPORT
 * in dxvk_shared_resource.h): with MISC_SHARED, forces the dmabuf export to
 * DRM_FORMAT_MOD_LINEAR so scanout consumers without modifier plumbing can
 * import it.  Value is ABI with the host dxvk build. */
#define TRITON_D3D11_MISC_LINEAR_EXPORT 0x40000000u

static inline void tritonSetError(PTRITON_DEVICE pD, HRESULT hr)
{
    if (pD && pD->pUMCallbacks && pD->pUMCallbacks->pfnSetErrorCb)
        pD->pUMCallbacks->pfnSetErrorCb(pD->hRTCoreLayer, hr);
}

/* Map the DDI's D3DWDDM2_0DDI_CONTEXTTYPE_FLAG bit flag to the sequential
 * D3D11_CONTEXT_TYPE enum (they diverge at COPY/VIDEO). Defined in
 * tritonQuery.c; shared with the WDDM 2.0 Flush path in tritonView.c. */
D3D11_CONTEXT_TYPE tritonContextFlagToEnum(UINT flag);

/* Intrusive per-resource view list.  RotateResourceIdentities must
 * recreate every host view wrapping a rotated resource, so each view
 * links itself onto its owning resource at create and unlinks at
 * destroy. */
typedef enum TRITON_VIEW_KIND {
    TRITON_VIEW_RTV = 0,
    TRITON_VIEW_SRV,
    TRITON_VIEW_DSV,
    TRITON_VIEW_UAV,
} TRITON_VIEW_KIND;

typedef struct TRITON_VIEWLINK {
    struct TRITON_VIEWLINK  *pNext;
    struct TRITON_VIEWLINK **ppPrev;  /* NULL when not linked */
    TRITON_VIEW_KIND         kind;
} TRITON_VIEWLINK;

typedef struct TRITON_RESOURCE {
    D3D10DDI_HRTRESOURCE   hRTResource;
    DXGI_FORMAT            Format;
    UINT                   Width;
    UINT                   Height;
    UINT                   Depth;
    UINT                   MipLevels;
    UINT                   ArraySize;
    UINT                   BindFlags;
    UINT                   MiscFlags;
    UINT                   MapFlags;     /* D3D10_DDI_CPU_ACCESS_* */
    UINT                   Usage;        /* D3D10_DDI_USAGE_* */
    DXGI_SAMPLE_DESC       SampleDesc;
    UINT                   ByteStride;

    /* TRUE when this resource is a display primary (BIND_PRESENT with
     * pPrimaryDesc): its host texture is a linear dmabuf export and its
     * KM allocation is a flippable scanout primary the KMD scans out
     * via SET_SCANOUT_BLOB. */
    BOOL                   IsPresentable;

    /* KM allocation backing this resource's virtio-gpu blob (shared
     * textures and primaries; 0 otherwise).  Owned by the creator;
     * consumers leave it 0 (the runtime owns their opened allocation). */
    D3DKMT_HANDLE          hKMAllocation;

    /* TRUE when the resource is shared (creator) or opened (consumer). */
    BOOL                   IsShared;

    /* Consumer-side import rig: WDDM handles of the transport-device
     * TYPE_IMPORT allocation that keeps the blob attached to this
     * process's transport context (0 when not a consumer). */
    D3DKMT_HANDLE          hImportAlloc;
    D3DKMT_HANDLE          hImportResKmt;

    ID3D11Resource        *pResource;

    /* Head of the intrusive list of live views wrapping this resource. */
    TRITON_VIEWLINK       *pViewList;
} TRITON_RESOURCE, *PTRITON_RESOURCE;

typedef struct TRITON_RTVIEW {
    PTRITON_RESOURCE                pResource;
    ID3D11RenderTargetView         *pRTV;
    TRITON_VIEWLINK                 link;
} TRITON_RTVIEW, *PTRITON_RTVIEW;

typedef struct TRITON_SRVIEW {
    PTRITON_RESOURCE                pResource;
    ID3D11ShaderResourceView       *pSRV;
    TRITON_VIEWLINK                 link;
} TRITON_SRVIEW, *PTRITON_SRVIEW;

typedef struct TRITON_DSVIEW {
    PTRITON_RESOURCE                pResource;
    ID3D11DepthStencilView         *pDSV;
    TRITON_VIEWLINK                 link;
} TRITON_DSVIEW, *PTRITON_DSVIEW;

typedef struct TRITON_UAVIEW {
    PTRITON_RESOURCE                pResource;
    ID3D11UnorderedAccessView      *pUAV;
    TRITON_VIEWLINK                 link;
} TRITON_UAVIEW, *PTRITON_UAVIEW;

typedef struct TRITON_BLENDSTATE {
    ID3D11BlendState1       *pState;
} TRITON_BLENDSTATE, *PTRITON_BLENDSTATE;

typedef struct TRITON_DEPTHSTENCILSTATE {
    ID3D11DepthStencilState        *pState;
} TRITON_DEPTHSTENCILSTATE, *PTRITON_DEPTHSTENCILSTATE;

typedef struct TRITON_RASTERIZERSTATE {
    ID3D11RasterizerState1       *pState;
} TRITON_RASTERIZERSTATE, *PTRITON_RASTERIZERSTATE;

typedef struct TRITON_SAMPLER {
    ID3D11SamplerState   *pState;
} TRITON_SAMPLER, *PTRITON_SAMPLER;

typedef struct TRITON_ELEMENTLAYOUT {
    UINT                            NumElements;
    D3D10DDIARG_INPUT_ELEMENT_DESC *pElements;  /* heap copy (driver-owned) */
    ID3D11InputLayout              *pLayout;    /* NULL until first VS is known */
    UINT64                          LayoutVsCookie; /* cookie of the VS pLayout was built against; 0 = none */
    /* Reconciled VS: a private copy of the bound VS whose input-signature
     * component types are retyped from this layout's vertex formats, so
     * D3DMetal's shader-side fetch reads integer inputs correctly. Bound in
     * place of the app's VS. NULL when no retyping is needed; keyed by
     * LayoutVsCookie alongside pLayout. */
    ID3D11VertexShader             *pReconVS;
} TRITON_ELEMENTLAYOUT, *PTRITON_ELEMENTLAYOUT;

typedef enum TRITON_SHADER_KIND {
    TRITON_SHADER_VS = 0,
    TRITON_SHADER_PS,
    TRITON_SHADER_GS,
    TRITON_SHADER_HS,
    TRITON_SHADER_DS,
    TRITON_SHADER_CS,
} TRITON_SHADER_KIND;

typedef struct TRITON_QUERY {
    ID3D11Query       *pQuery;
} TRITON_QUERY, *PTRITON_QUERY;

typedef struct TRITON_SHADER {
    TRITON_SHADER_KIND     kind;
    void                  *pBytecode;   /* heap copy; freed on destroy */
    SIZE_T                 cbBytecode;
    UINT64                 cookie;       /* unique instance id; input-layout cache key */
    union {
        ID3D11DeviceChild       *pDeviceChild;
        ID3D11VertexShader      *pVS;
        ID3D11PixelShader       *pPS;
        ID3D11GeometryShader    *pGS;
        ID3D11HullShader        *pHS;
        ID3D11DomainShader      *pDS;
        ID3D11ComputeShader     *pCS;
    } u;
} TRITON_SHADER, *PTRITON_SHADER;

/* View-list plumbing + rotation support (tritonView.c). */
void tritonResourceLinkView(PTRITON_RESOURCE r, TRITON_VIEWLINK *l,
                            TRITON_VIEW_KIND kind);
void tritonViewUnlink(TRITON_VIEWLINK *l);
HRESULT tritonResourceRecreateViews(PTRITON_DEVICE pD, PTRITON_RESOURCE r);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* TRITON_H_INCLUDED */
