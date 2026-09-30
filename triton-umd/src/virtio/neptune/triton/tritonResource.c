/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Resource lifecycle, Map/Unmap, UpdateSubresource, copies, hazards,
 * constant-buffer binds, tiled-resource forwarders, and the various
 * unimplementable WDDM 2.0+ stubs.
 */

#include "triton.h"
#include "triton_log.h"
#include "tritonPresent.h"
#include "tritonSharedBridge.h"
#include "npt_shared_texture.h"          /* shared format + export helpers */

#include "virtio/virtio-gpu/wddm_hw.h"   /* VIOGPU resource / shared-allocation ABI */

SIZE_T APIENTRY
tritonCalcPrivateResourceSize(D3D10DDI_HDEVICE hDevice,
                              const D3D11DDIARG_CREATERESOURCE *pArgs)
{
    (void)hDevice;
    (void)pArgs;
    return sizeof(TRITON_RESOURCE);
}

SIZE_T APIENTRY
tritonCalcPrivateOpenedResourceSize(D3D10DDI_HDEVICE hDevice,
                                    const D3D10DDIARG_OPENRESOURCE *pArgs)
{
    (void)hDevice;
    (void)pArgs;
    return sizeof(TRITON_RESOURCE);
}

/* Translate the DDI's combined Usage/CpuAccess/Bind flag salad into the
 * corresponding D3D11 enums.
 *
 * BindFlags bit layout is NOT 1:1 between the DDI and D3D11:
 *   - DDI 0x080 = D3D10_DDI_BIND_PRESENT (no D3D11 BindFlag equivalent)
 *   - DDI 0x100 = D3D11_DDI_BIND_UNORDERED_ACCESS, but D3D11_BIND_UNORDERED_ACCESS
 *     is at 0x080. A verbatim copy would (a) drop the UAV bit and (b)
 *     spuriously enable UAV for any present-bound texture.
 *
 * MiscFlags has its own DDI-only bits (DISCARD_ON_PRESENT 0x08, REMOTE
 * 0x400, ...) that have no D3D11 meaning; we drop them. TEXTURECUBE
 * (0x4) is signaled via ResourceDimension, not MiscFlags, so it is
 * re-added in the cube create path. */
static void tritonTranslateUsage(const D3D11DDIARG_CREATERESOURCE *a,
                                 D3D11_USAGE *pUsage,
                                 UINT *pCPUAccessFlags,
                                 UINT *pBindFlags,
                                 UINT *pMiscFlags)
{
    switch (a->Usage) {
    case D3D10_DDI_USAGE_DEFAULT:    *pUsage = D3D11_USAGE_DEFAULT;   break;
    case D3D10_DDI_USAGE_IMMUTABLE:  *pUsage = D3D11_USAGE_IMMUTABLE; break;
    case D3D10_DDI_USAGE_DYNAMIC:    *pUsage = D3D11_USAGE_DYNAMIC;   break;
    case D3D10_DDI_USAGE_STAGING:    *pUsage = D3D11_USAGE_STAGING;   break;
    default:                          *pUsage = D3D11_USAGE_DEFAULT;   break;
    }

    *pCPUAccessFlags = 0;
    if (a->MapFlags & D3D10_DDI_CPU_ACCESS_READ)  *pCPUAccessFlags |= D3D11_CPU_ACCESS_READ;
    if (a->MapFlags & D3D10_DDI_CPU_ACCESS_WRITE) *pCPUAccessFlags |= D3D11_CPU_ACCESS_WRITE;

    UINT bf = 0;
    const UINT dbf = a->BindFlags;
    if (dbf & D3D10_DDI_BIND_VERTEX_BUFFER)        bf |= D3D11_BIND_VERTEX_BUFFER;
    if (dbf & D3D10_DDI_BIND_INDEX_BUFFER)         bf |= D3D11_BIND_INDEX_BUFFER;
    if (dbf & D3D10_DDI_BIND_CONSTANT_BUFFER)      bf |= D3D11_BIND_CONSTANT_BUFFER;
    if (dbf & D3D10_DDI_BIND_SHADER_RESOURCE)      bf |= D3D11_BIND_SHADER_RESOURCE;
    if (dbf & D3D10_DDI_BIND_STREAM_OUTPUT)        bf |= D3D11_BIND_STREAM_OUTPUT;
    if (dbf & D3D10_DDI_BIND_RENDER_TARGET)        bf |= D3D11_BIND_RENDER_TARGET;
    if (dbf & D3D10_DDI_BIND_DEPTH_STENCIL)        bf |= D3D11_BIND_DEPTH_STENCIL;
    if (dbf & D3D11_DDI_BIND_UNORDERED_ACCESS)     bf |= D3D11_BIND_UNORDERED_ACCESS;
    if (dbf & D3D11_DDI_BIND_DECODER)              bf |= D3D11_BIND_DECODER;
    if (dbf & D3D11_DDI_BIND_VIDEO_ENCODER)        bf |= D3D11_BIND_VIDEO_ENCODER;
    /* BIND_PRESENT (0x80) for swap-chain back buffers has no D3D11
     * equivalent; RENDER_TARGET covers the only access we need. */
    *pBindFlags = bf;

    UINT mf = 0;
    const UINT dmf = a->MiscFlags;
    if (dmf & D3D10_DDI_RESOURCE_AUTO_GEN_MIP_MAP)         mf |= D3D11_RESOURCE_MISC_GENERATE_MIPS;
    if (dmf & D3D10_DDI_RESOURCE_MISC_SHARED)              mf |= D3D11_RESOURCE_MISC_SHARED;
    if (dmf & D3D11_DDI_RESOURCE_MISC_DRAWINDIRECT_ARGS)   mf |= D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
    if (dmf & D3D11_DDI_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS) mf |= D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    if (dmf & D3D11_DDI_RESOURCE_MISC_BUFFER_STRUCTURED)   mf |= D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    if (dmf & D3D11_DDI_RESOURCE_MISC_RESOURCE_CLAMP)      mf |= D3D11_RESOURCE_MISC_RESOURCE_CLAMP;
    *pMiscFlags = mf;
}

/* Assemble D3D11_SUBRESOURCE_DATA[] from pInitialDataUP for the
 * CreateBuffer/CreateTexture* initial-data forms. The caller owns the
 * returned heap buffer. NULL when pInitialDataUP is NULL. */
static D3D11_SUBRESOURCE_DATA *
tritonBuildInitData(const D3D11DDIARG_CREATERESOURCE *a)
{
    if (!a->pInitialDataUP) return NULL;
    /* Subresource count is MipLevels * ArraySize. A cube may arrive with
     * ArraySize==0 meaning one 6-face cube; the create path below
     * substitutes 6 for the desc, so mirror that here or the faces would
     * be created without their supplied initial data. */
    UINT arraySize = a->ArraySize;
    if (a->ResourceDimension == D3D10DDIRESOURCE_TEXTURECUBE && arraySize == 0)
        arraySize = 6;
    const UINT count = a->MipLevels * arraySize;
    if (count == 0) return NULL;
    D3D11_SUBRESOURCE_DATA *p = (D3D11_SUBRESOURCE_DATA *)(
        HeapAlloc(GetProcessHeap(), 0, count * sizeof(D3D11_SUBRESOURCE_DATA)));
    if (!p) return NULL;
    for (UINT i = 0; i < count; ++i) {
        p[i].pSysMem          = a->pInitialDataUP[i].pSysMem;
        p[i].SysMemPitch      = a->pInitialDataUP[i].SysMemPitch;
        p[i].SysMemSlicePitch = a->pInitialDataUP[i].SysMemSlicePitch;
    }
    return p;
}

/* Exporter side of the shared/presentable texture plumbing.  The host
 * texture r->pResource was created with exportable (dmabuf) storage;
 * stage its export as this context's pending blob and create the shared
 * KM allocation whose private data round-trips the texture description
 * to an opening process, and whose KMD-side blob create binds a
 * VM-global res_id to the dmabuf.  primary marks a flippable scanout
 * primary (segment-1 residency + scanout promotion in the KMD). */
static HRESULT
tritonRegisterSharedBlob(PTRITON_DEVICE pD, PTRITON_RESOURCE r,
                         UINT d3d11Usage, UINT d3d11Bind, UINT d3d11Cpu,
                         UINT d3d11Misc, DXGI_FORMAT hostFmt, BOOL primary)
{
    if (!pD->KTCallbacks.pfnAllocateCb || !pD->KTCallbacks.pfnDeallocateCb)
        return E_FAIL;

    const UINT physicalSamples = r->pPresentResource ? 1 : r->SampleDesc.Count;
    const UINT physicalMisc = d3d11Misc &
        ~(TRITON_D3D11_MISC_LINEAR_EXPORT | TRITON_D3D11_MISC_SINGLE_PLANE_EXPORT);
    const UINT sharedFormat = npt_shared_texture_virgl_format(hostFmt);
    const UINT bytesPerPixel = hostFmt == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4;
    if (!r->Width || !r->Height || !sharedFormat)
        return E_INVALIDARG;
#if defined(NPT_D3D10_RUNTIME_DDI)
    if (!r->Width || !r->Height || r->Width > (primary ? 4096u : 8192u) ||
        r->Height > (primary ? 4096u : 8192u) || !sharedFormat ||
        r->MipLevels != 1 || r->ArraySize != 1 || physicalSamples != 1 ||
        d3d11Usage != D3D11_USAGE_DEFAULT || d3d11Cpu ||
        d3d11Bind != (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET) ||
        physicalMisc != D3D11_RESOURCE_MISC_SHARED ||
        (primary && hostFmt != DXGI_FORMAT_R8G8B8A8_UNORM &&
         hostFmt != DXGI_FORMAT_B8G8R8A8_UNORM && hostFmt != DXGI_FORMAT_B8G8R8X8_UNORM))
        return E_INVALIDARG;
#else
    (void)physicalSamples;
#endif

    /* The KMD's deferred blob create / ctx attach need this device's
     * virtio context. */
    tritonPresentEnsureRuntimeCtx(pD);

    VIOGPU_CREATE_ALLOCATION_EXCHANGE ax;
    memset(&ax, 0, sizeof(ax));
    ax.Type = VIOGPU_RESOURCE_TYPE_SHARED;
    VIOGPU_RESOURCE_SHARED_TEXTURE_OPTIONS *o = &ax.OptionsShared;

    struct triton_shared_texture_desc exp;
    memset(&exp, 0, sizeof(exp));
    ID3D11Resource *exportResource = r->pPresentResource ? r->pPresentResource : r->pResource;
    if (!tritonSharedBridgeExportBlob(exportResource, &exp)) {
        TR_LOG("shared: export failed %ux%u fmt=%d", r->Width, r->Height,
               hostFmt);
        return E_FAIL;
    }
    const ULONGLONG rowBytes = (ULONGLONG)r->Width * bytesPerPixel;
    if (!exp.blob_id || exp.plane_count != 1 || exp.texture_layout > 2 ||
        !exp.allocation_size || exp.allocation_size > (ULONGLONG)(SIZE_T)-1 - 4095ull ||
        !exp.planes[0].pitch || exp.planes[0].pitch > UINT32_MAX ||
        exp.planes[0].pitch < rowBytes ||
        exp.planes[0].offset > UINT32_MAX || exp.planes[0].offset >= exp.allocation_size ||
        rowBytes > exp.allocation_size - exp.planes[0].offset ||
        (ULONGLONG)(r->Height - 1) >
            (exp.allocation_size - exp.planes[0].offset - rowBytes) / exp.planes[0].pitch ||
        exp.planes[1].offset || exp.planes[1].pitch ||
        exp.planes[2].offset || exp.planes[2].pitch ||
        exp.planes[3].offset || exp.planes[3].pitch) {
        return tritonSharedBridgeCancelExportBlob(exportResource, exp.blob_id)
            ? E_INVALIDARG : DXGI_ERROR_DEVICE_REMOVED;
    }
    o->blob_id         = exp.blob_id;
    o->create_ctx_id   = exp.create_ctx_id;
    o->plane_count     = exp.plane_count;
    o->texture_layout  = exp.texture_layout;
    o->modifier        = exp.modifier;
    o->allocation_size = exp.allocation_size;
    for (UINT i = 0; i < exp.plane_count && i < 4; i++) {
        o->planes[i].offset = exp.planes[i].offset;
        o->planes[i].pitch  = exp.planes[i].pitch;
    }

    o->primary          = primary ? 1u : 0u;
    o->width            = r->Width;
    o->height           = r->Height;
    o->mip_levels       = r->MipLevels ? r->MipLevels : 1;
    o->array_size       = r->ArraySize ? r->ArraySize : 1;
    o->format           = (ULONG)hostFmt;
    o->sample_count     = r->pPresentResource ? 1 : (r->SampleDesc.Count ? r->SampleDesc.Count : 1);
    o->usage            = d3d11Usage;
    o->bind_flags       = d3d11Bind;
    o->cpu_access_flags = d3d11Cpu;
    /* Export layout flags are consumed by host texture creation, not D3D
     * resource property and must not survive in the allocation description
     * that the kernel validates and another process imports. */
    o->misc_flags       = physicalMisc;

    /* Publish the texture description for EVERY shared blob, not just scanout
     * primaries: the KMD keys its blob-info-valid flag off ScanoutInfo.width,
     * and without it DescribeAllocation returns STATUS_INVALID_PARAMETER and
     * GetTransferLayout fails, so a windowed blt present emits
     * VIRGL_CCMD_PIPE_RESOURCE_SET_TYPE with width/height/stride 0 and the
     * host rejects both it and the resource_copy_region that follows.  This
     * is pure description; claiming the scanout stays gated on `primary` via
     * o->primary and ai.Flags.Primary below. */
    o->ScanoutInfo.width      = r->Width;
    o->ScanoutInfo.height     = r->Height;
    o->ScanoutInfo.format     = sharedFormat;
    o->ScanoutInfo.strides[0] = (ULONG)o->planes[0].pitch;
    o->ScanoutInfo.offsets[0] = (ULONG)o->planes[0].offset;

    ax.Size = (o->allocation_size + 4095ull) & ~4095ull;

#if defined(NPT_D3D10_RUNTIME_DDI)
    D3DDDI_ALLOCATIONINFO ai;
#else
    D3DDDI_ALLOCATIONINFO2 ai;
#endif
    memset(&ai, 0, sizeof(ai));
    ai.pPrivateDriverData    = &ax;
    ai.PrivateDriverDataSize = sizeof(ax);
    if (primary) {
        ai.Flags.Primary = 1;
        ai.VidPnSourceId = 0;
    }

    D3DDDICB_ALLOCATE cb;
    memset(&cb, 0, sizeof(cb));
    cb.hResource        = r->hRTResource.handle;
    cb.NumAllocations   = 1;
#if defined(NPT_D3D10_RUNTIME_DDI)
    cb.pAllocationInfo = &ai;
#else
    cb.pAllocationInfo2 = &ai;
#endif

    HRESULT hr = pD->KTCallbacks.pfnAllocateCb(pD->hRTDevice.handle, &cb);
    if (FAILED(hr) || !ai.hAllocation) {
        TR_LOG("shared: pfnAllocateCb failed hr=0x%08lx", hr);
        /* A callback may return an allocation alongside a failing status.
         * Do not strand that allocation on a failed void CreateResource. */
        HRESULT cleanup = S_OK;
        if (ai.hAllocation) {
            D3DDDICB_DEALLOCATE da = {0};
            da.NumAllocations = 1;
            da.HandleList = &ai.hAllocation;
            cleanup = pD->KTCallbacks.pfnDeallocateCb(pD->hRTDevice.handle, &da);
        }
        /* Export owns a separate pending host fd. Releasing the texture
         * alone cannot roll it back. Cancellation is idempotent when the
         * KMD already consumed the pending blob before failing allocation. */
        if (!tritonSharedBridgeCancelExportBlob(exportResource, exp.blob_id) || FAILED(cleanup))
            return DXGI_ERROR_DEVICE_REMOVED;
        return FAILED(hr) ? hr : E_FAIL;
    }
    r->hKMAllocation = ai.hAllocation;
    r->IsShared      = TRUE;
    TR_LOG("shared: exporter blob_id=0x%llx alloc=0x%x %ux%u primary=%d "
           "pitch=%llu", o->blob_id, ai.hAllocation, r->Width, r->Height,
           primary, o->planes[0].pitch);
    return S_OK;
}

void APIENTRY
tritonCreateResource(D3D10DDI_HDEVICE hDevice,
                     const D3D11DDIARG_CREATERESOURCE *pArgs,
                     D3D10DDI_HRESOURCE hResource,
                     D3D10DDI_HRTRESOURCE hRTResource)
{
    PTRITON_DEVICE   pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_RESOURCE r  = (PTRITON_RESOURCE)(hResource.pDrvPrivate);
    if (!pD || !r) {
        TR_LOG("CreateResource: missing pDrvPrivate");
        return;
    }

    r->hRTResource = hRTResource;
    r->Format      = pArgs->Format;
    r->MipLevels   = pArgs->MipLevels;
    r->ArraySize   = pArgs->ArraySize;
    r->BindFlags   = pArgs->BindFlags;
    r->MiscFlags   = pArgs->MiscFlags;
    r->MapFlags    = pArgs->MapFlags;
    r->Usage       = pArgs->Usage;
    r->SampleDesc  = pArgs->SampleDesc;
    r->ByteStride  = pArgs->ByteStride;
    r->pResource        = NULL;
    r->pPresentResource = NULL;
    r->PresentFormat    = DXGI_FORMAT_UNKNOWN;
    r->pViewList        = NULL;
    /* A resource is a display primary only when the runtime supplies
     * pPrimaryDesc (d3d10umddi.h: "If pPrimaryDesc absent, blt/copy style
     * is implied when used with Present").  BIND_PRESENT alone marks any
     * swapchain back buffer, including windowed ones that DWM composites
     * as ordinary shared textures and that must never claim the scanout. */
    r->IsPresentable    = !!(pArgs->BindFlags & D3D10_DDI_BIND_PRESENT) &&
                          pArgs->pPrimaryDesc != NULL;
    r->hKMAllocation    = 0;
    r->BorrowedKMAllocation = FALSE;
    r->HostFormat       = DXGI_FORMAT_UNKNOWN;
    r->IsShared         = FALSE;
    r->hImportAlloc     = 0;
    r->hImportResKmt    = 0;

    /* Non-primary TEXTURE2D with MISC_SHARED: emulate the shared handle.
     * Display primaries take the host-swapchain route below.
     *
     * BIND_PRESENT joins them even without MISC_SHARED.  A windowed
     * blt-model swapchain (DXGI_SWAP_EFFECT_DISCARD) creates a plain back
     * buffer and presents it with Flags.Blt and hDstResource 0, so the only
     * handle dxgkrnl ever receives is the source allocation; with no KM
     * allocation tritonDxgiPresent has nothing to submit and the window is
     * never composed.  Blob-backed shared storage supplies that allocation;
     * primary=FALSE keeps it off the scanout, exactly like a flip-model back
     * buffer. */
    BOOL isShared = !r->IsPresentable &&
                    pArgs->ResourceDimension == D3D10DDIRESOURCE_TEXTURE2D &&
                    ((pArgs->MiscFlags & D3D10_DDI_RESOURCE_MISC_SHARED) != 0 ||
                     (pArgs->BindFlags & D3D10_DDI_BIND_PRESENT) != 0);


    /* Top-level dimensions come from the mip-0 entry of pMipInfoList. */
    if (pArgs->pMipInfoList) {
        r->Width  = pArgs->pMipInfoList[0].TexelWidth;
        r->Height = pArgs->pMipInfoList[0].TexelHeight;
        r->Depth  = pArgs->pMipInfoList[0].TexelDepth;
    } else {
        r->Width = r->Height = r->Depth = 0;
    }

    /* Presentation storage must be exportable and single-sampled. Keep a
     * genuine multisampled render resource for application views and resolve
     * it to a separate shared texture at the presentation boundary. */
    if ((pArgs->BindFlags & D3D10_DDI_BIND_PRESENT) &&
        pArgs->ResourceDimension == D3D10DDIRESOURCE_TEXTURE2D) {
        if (!r->Width || !r->Height || r->MipLevels != 1 ||
            r->ArraySize != 1 || !r->SampleDesc.Count ||
            pArgs->pInitialDataUP || pArgs->Usage != D3D10_DDI_USAGE_DEFAULT) {
            tritonSetError(pD, E_INVALIDARG);
            return;
        }
        if (r->IsPresentable) tritonPresentEnsureKernelContext(pD);
        r->PresentFormat = (DXGI_FORMAT)npt_shared_texture_host_format(r->Format);
        if (r->PresentFormat != r->Format)
            r->HostFormat = r->PresentFormat;
        D3D11_TEXTURE2D_DESC d = {0};
        d.Width = r->Width;
        d.Height = r->Height;
        d.MipLevels = d.ArraySize = 1;
        d.Format = r->PresentFormat;
        d.SampleDesc = r->SampleDesc;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        const UINT sharedMisc = D3D11_RESOURCE_MISC_SHARED |
            TRITON_D3D11_MISC_SINGLE_PLANE_EXPORT;
        if (d.SampleDesc.Count == 1) d.MiscFlags = sharedMisc;
        ID3D11Texture2D *tex = NULL;
        HRESULT hr = ID3D11Device1_CreateTexture2D(pD->pDev1, &d, NULL, &tex);
        if (SUCCEEDED(hr) && tex) {
            r->pResource = (ID3D11Resource *)tex;
            if (d.SampleDesc.Count > 1) {
                d.SampleDesc.Count = 1;
                d.SampleDesc.Quality = 0;
                d.MiscFlags = sharedMisc;
                tex = NULL;
                hr = ID3D11Device1_CreateTexture2D(pD->pDev1, &d, NULL, &tex);
                if (SUCCEEDED(hr) && tex) r->pPresentResource = (ID3D11Resource *)tex;
                else if (SUCCEEDED(hr)) hr = E_FAIL;
            }
            if (SUCCEEDED(hr))
                hr = tritonRegisterSharedBlob(pD, r, d.Usage, d.BindFlags, 0,
                                               sharedMisc, d.Format, r->IsPresentable);
        } else if (SUCCEEDED(hr)) hr = E_FAIL;
        if (FAILED(hr)) {
            if (r->pPresentResource) ID3D11Resource_Release(r->pPresentResource);
            if (r->pResource) ID3D11Resource_Release(r->pResource);
            r->pPresentResource = r->pResource = NULL;
            tritonSetError(pD, hr);
        }
        return;
    }

    D3D11_USAGE usage;
    UINT cpuAccess, bind, misc;
    tritonTranslateUsage(pArgs, &usage, &cpuAccess, &bind, &misc);

    /* Cross-process sharing is carried by the virtio res_id bound to the KM
     * allocation, not by Win32 shared handles: keep MISC_SHARED so the host
     * backs the texture with exportable memory, and guarantee consumers (DWM
     * samples every composited surface) can create SRVs against it. */
    if (isShared) {
#if defined(NPT_D3D10_RUNTIME_DDI)
        if (usage != D3D11_USAGE_DEFAULT || cpuAccess || r->MipLevels != 1 ||
            r->ArraySize != 1 || r->SampleDesc.Count != 1 || r->SampleDesc.Quality ||
            misc != D3D11_RESOURCE_MISC_SHARED ||
            (bind & ~(D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET)) ||
            !npt_shared_texture_virgl_format(r->Format)) {
            tritonSetError(pD, E_INVALIDARG);
            return;
        }
#endif
        bind |= D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        /* A back buffer that reaches here only for BIND_PRESENT never asked
         * for MISC_SHARED, so its host storage would not be exportable and
         * the export below would fail; force it on as the primary path
         * above does. */
        misc |= D3D11_RESOURCE_MISC_SHARED | TRITON_D3D11_MISC_SINGLE_PLANE_EXPORT;
    }

    D3D11_SUBRESOURCE_DATA *initData = tritonBuildInitData(pArgs);
    if (pArgs->pInitialDataUP && !initData) {
        tritonSetError(pD, E_OUTOFMEMORY);
        return;
    }
    HRESULT hr = E_NOTIMPL;

    switch (pArgs->ResourceDimension) {
    case D3D10DDIRESOURCE_TEXTURE2D: {
        D3D11_TEXTURE2D_DESC d = {};
        d.Width            = r->Width;
        d.Height           = r->Height;
        d.MipLevels        = r->MipLevels;
        d.ArraySize        = r->ArraySize;
        d.Format           = r->Format;
        d.SampleDesc       = r->SampleDesc;
        d.Usage            = usage;
        d.BindFlags        = bind;
        d.CPUAccessFlags   = cpuAccess;
        d.MiscFlags        = misc;
        ID3D11Texture2D *tex = NULL;
        hr = ID3D11Device1_CreateTexture2D(pD->pDev1, &d, initData, &tex);
        if (SUCCEEDED(hr) && tex)
            r->pResource = (ID3D11Resource *)tex;
        break;
    }
    case D3D10DDIRESOURCE_BUFFER:
    case D3D11DDIRESOURCE_BUFFEREX: {
        D3D11_BUFFER_DESC d = {};
        /* Buffer size: pMipInfoList[0].TexelWidth is bytes for buffers. */
        d.ByteWidth           = r->Width;
        d.Usage               = usage;
        d.BindFlags           = bind;
        d.CPUAccessFlags      = cpuAccess;
        d.MiscFlags           = misc;
        d.StructureByteStride = r->ByteStride;
        ID3D11Buffer *buf = NULL;
        hr = ID3D11Device1_CreateBuffer(pD->pDev1, &d, initData, &buf);
        if (SUCCEEDED(hr) && buf)
            r->pResource = (ID3D11Resource *)buf;
        break;
    }
    case D3D10DDIRESOURCE_TEXTURE1D: {
        D3D11_TEXTURE1D_DESC d = {};
        d.Width          = r->Width;
        d.MipLevels      = r->MipLevels;
        d.ArraySize      = r->ArraySize;
        d.Format         = r->Format;
        d.Usage          = usage;
        d.BindFlags      = bind;
        d.CPUAccessFlags = cpuAccess;
        d.MiscFlags      = misc;
        ID3D11Texture1D *tex = NULL;
        hr = ID3D11Device1_CreateTexture1D(pD->pDev1, &d, initData, &tex);
        if (SUCCEEDED(hr) && tex) r->pResource = (ID3D11Resource *)tex;
        break;
    }
    case D3D10DDIRESOURCE_TEXTURE3D: {
        D3D11_TEXTURE3D_DESC d = {};
        d.Width          = r->Width;
        d.Height         = r->Height;
        d.Depth          = r->Depth;
        d.MipLevels      = r->MipLevels;
        d.Format         = r->Format;
        d.Usage          = usage;
        d.BindFlags      = bind;
        d.CPUAccessFlags = cpuAccess;
        d.MiscFlags      = misc;
        ID3D11Texture3D *tex = NULL;
        hr = ID3D11Device1_CreateTexture3D(pD->pDev1, &d, initData, &tex);
        if (SUCCEEDED(hr) && tex) r->pResource = (ID3D11Resource *)tex;
        break;
    }
    case D3D10DDIRESOURCE_TEXTURECUBE: {
        /* Cubes are tex2d arrays with MiscFlags |= TEXTURECUBE. */
        D3D11_TEXTURE2D_DESC d = {};
        d.Width            = r->Width;
        d.Height           = r->Height;
        d.MipLevels        = r->MipLevels;
        d.ArraySize        = r->ArraySize ? r->ArraySize : 6;
        d.Format           = r->Format;
        d.SampleDesc       = r->SampleDesc;
        d.Usage            = usage;
        d.BindFlags        = bind;
        d.CPUAccessFlags   = cpuAccess;
        d.MiscFlags        = misc | D3D11_RESOURCE_MISC_TEXTURECUBE;
        ID3D11Texture2D *tex = NULL;
        hr = ID3D11Device1_CreateTexture2D(pD->pDev1, &d, initData, &tex);
        if (SUCCEEDED(hr) && tex) r->pResource = (ID3D11Resource *)tex;
        break;
    }
    default:
        TR_LOG("CreateResource: unsupported resource dimension");
        hr = E_INVALIDARG;
        break;
    }

    if (initData) HeapFree(GetProcessHeap(), 0, initData);

    if (FAILED(hr) || !r->pResource) {
        TR_LOG("CreateResource: failed dim=%d hr=0x%08lx", pArgs->ResourceDimension, hr);
        tritonSetError(pD, FAILED(hr) ? hr : E_OUTOFMEMORY);
        return;
    }

    if (isShared) {
        hr = tritonRegisterSharedBlob(pD, r, (UINT)usage, bind, cpuAccess,
                                       misc, r->Format, FALSE);
        if (FAILED(hr)) {
            ID3D11Resource_Release(r->pResource);
            r->pResource = NULL;
            tritonSetError(pD, hr);
        }
    }
}

void APIENTRY
tritonDestroyResource(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE hResource)
{
    PTRITON_RESOURCE r = (PTRITON_RESOURCE)(hResource.pDrvPrivate);
    if (!r) return;
    PTRITON_DEVICE pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);

    /* Consumer: drop the transport-context attach rig before the host
     * texture; the imported texture keeps the dmabuf alive on its own. */
    if ((r->hImportAlloc || r->hImportResKmt) && pD && pD->pDev1) {
        if (tritonSharedBridgeReleaseImportRes(pD->pDev1, r->hImportAlloc,
                                               r->hImportResKmt)) {
            r->hImportAlloc = 0;
            r->hImportResKmt = 0;
        } else {
            TR_LOG("shared: import release failed alloc=%u resource=%u",
                   r->hImportAlloc, r->hImportResKmt);
            tritonSetError(pD, E_FAIL);
        }
    }

    if (r->pPresentResource) {
        ID3D11Resource_Release(r->pPresentResource);
        r->pPresentResource = NULL;
    }
    if (r->pResource) {
        ID3D11Resource_Release(r->pResource);
        r->pResource = NULL;
    }

    /* Exporter: the KM allocation owns the virtio blob; deallocating it
     * unrefs the host resource once every opener has closed. */
    if (r->hKMAllocation && !r->BorrowedKMAllocation && pD &&
        pD->KTCallbacks.pfnDeallocateCb) {
        D3DDDICB_DEALLOCATE da;
        memset(&da, 0, sizeof(da));
        da.NumAllocations = 1;
        da.HandleList     = &r->hKMAllocation;
        pD->KTCallbacks.pfnDeallocateCb(pD->hRTDevice.handle, &da);
        r->hKMAllocation = 0;
    }
    if (r->BorrowedKMAllocation)
        r->hKMAllocation = 0;
}

#if defined(NPT_D3D10_RUNTIME_DDI)
/* Validate the private allocation description again at the importing UMD.
 * Opening must not turn a corrupt pitch or a scanout-only interpretation into
 * a host resource merely because a kernel handle was supplied. */
static BOOL
tritonValidateSharedOpen(const VIOGPU_RESOURCE_SHARED_TEXTURE_OPTIONS *so,
                         ULONGLONG allocationSize)
{
    if (!so || !so->blob_id || so->primary > 1 || !so->width || !so->height ||
        so->width > (so->primary ? 4096u : 8192u) ||
        so->height > (so->primary ? 4096u : 8192u) ||
        so->mip_levels != 1 || so->array_size != 1 || so->sample_count != 1 ||
        so->usage != D3D11_USAGE_DEFAULT || so->cpu_access_flags ||
        so->bind_flags != (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET) ||
        so->misc_flags != D3D11_RESOURCE_MISC_SHARED ||
        so->plane_count != 1 || so->texture_layout > 2 || !so->allocation_size ||
        so->allocation_size > (ULONGLONG)(SIZE_T)-1 - 4095ull ||
        ((so->allocation_size + 4095ull) & ~4095ull) != allocationSize)
        return FALSE;
    const UINT format = npt_shared_texture_virgl_format(so->format);
    const UINT bytesPerPixel = so->format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4;
    if (!format || (so->primary && so->format != DXGI_FORMAT_R8G8B8A8_UNORM &&
        so->format != DXGI_FORMAT_B8G8R8A8_UNORM &&
        so->format != DXGI_FORMAT_B8G8R8X8_UNORM))
        return FALSE;
    const ULONGLONG rowBytes = (ULONGLONG)so->width * bytesPerPixel;
    const ULONGLONG pitch = so->planes[0].pitch, offset = so->planes[0].offset;
    if (pitch < rowBytes || pitch > UINT32_MAX || offset > UINT32_MAX ||
        offset >= so->allocation_size || rowBytes > so->allocation_size - offset ||
        (ULONGLONG)(so->height - 1) > (so->allocation_size - offset - rowBytes) / pitch ||
        so->ScanoutInfo.width != so->width || so->ScanoutInfo.height != so->height ||
        so->ScanoutInfo.format != format || so->ScanoutInfo.strides[0] != pitch ||
        so->ScanoutInfo.offsets[0] != offset)
        return FALSE;
    for (UINT i = 1; i < 4; ++i) {
        if (so->planes[i].offset || so->planes[i].pitch ||
            so->ScanoutInfo.strides[i] || so->ScanoutInfo.offsets[i])
            return FALSE;
    }
    return TRUE;
}
#endif

/* Standard Vista shadow/staging allocations are linear WDDM surfaces. They
 * are distinct from standard primaries, whose queried host storage and borrowed
 * allocation ownership require a separate import path. */
static BOOL
tritonSynthesizeStandardTexture(const VIOGPU_RESOURCE_3D_OPTIONS *options,
                                ULONGLONG allocationSize,
                                VIOGPU_RESOURCE_SHARED_TEXTURE_OPTIONS *out)
{
    if (!options || !out)
        return FALSE;
    memset(out, 0, sizeof(*out));
    ULONG format;
    switch (options->format) {
    case 1: format = DXGI_FORMAT_B8G8R8A8_UNORM; break;
    case 2: format = DXGI_FORMAT_B8G8R8X8_UNORM; break;
    case 67: case 134: format = DXGI_FORMAT_R8G8B8A8_UNORM; break;
    default: return FALSE;
    }
#if defined(NPT_D3D10_RUNTIME_DDI)
    if (!options->width || !options->height || options->width > 4096 ||
        options->height > 4096 || options->target != 2 || options->depth != 1 ||
        options->array_size != 1 || options->last_level || options->nr_samples ||
        options->bind != ((1u << 1) | (1u << 3) | (1u << 7) | (1u << 18)) ||
        (options->flags != (1u << 2) &&
         options->flags != VIOGPU_RESOURCE_FLAG_STANDARD_PRIMARY))
        return FALSE;
    if (options->flags == VIOGPU_RESOURCE_FLAG_STANDARD_PRIMARY &&
        options->format != 1 && options->format != 2)
        return FALSE;
    const ULONGLONG stride = (ULONGLONG)options->width * 4ull;
    if (allocationSize != stride * options->height || allocationSize > (SIZE_T)-1)
        return FALSE;
#else
    /* Modern metal-backed standard resources retain their established layout. */
    const ULONGLONG stride = ((ULONGLONG)options->width * 4ull + 255ull) & ~255ull;
    (void)allocationSize;
#endif
    out->width = options->width;
    out->height = options->height;
    out->mip_levels = out->array_size = out->sample_count = 1;
    out->format = format;
    out->usage = D3D11_USAGE_DEFAULT;
    out->bind_flags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    out->plane_count = 1;
    out->allocation_size = stride * options->height;
    out->planes[0].pitch = stride;
    return TRUE;
}

/* A standard primary is a KMD-created non-blob resource. Its actual export
 * layout is supplied by the host after attachment, rather than synthesized
 * from the guest's linear CPU shadow. */
static BOOL
tritonQueryStandardPrimary(PTRITON_DEVICE pD, PTRITON_RESOURCE r,
                           UINT res_id, struct triton_shared_texture_desc *desc)
{
    memset(desc, 0, sizeof(*desc));
    if (!tritonSharedBridgeQueryRes(pD->pDev1, res_id, desc) ||
        desc->width != r->Width || desc->height != r->Height ||
        desc->mip_levels != 1 || desc->array_size != 1 ||
        desc->sample_count != 1 || desc->usage != D3D11_USAGE_DEFAULT ||
        desc->cpu_access_flags || desc->plane_count != 1 ||
        !desc->allocation_size ||
        (desc->bind_flags & (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET)) !=
            (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET))
        return FALSE;
    switch (desc->format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
        return TRUE;
    default:
        return FALSE;
    }
}

/* OpenResource imports a cross-process shared resource.  The opening
 * process receives the exporter's per-allocation private data (the
 * VIOGPU_CREATE_ALLOCATION_EXCHANGE it passed to pfnAllocateCb),
 * round-tripped by the runtime, plus the KM allocation handle.  The
 * virtio res_id bound to the exporter's dmabuf is queried from the KMD
 * (VIOGPU_RES_INFO), attached to this process's transport context, and
 * imported on this device -- the imported texture aliases the
 * exporter's memory.  PFND3D10DDI_OPENRESOURCE is VOID, so failure MUST
 * be signalled via pfnSetErrorCb or the runtime leaves a NULL resource
 * that later no-ops. */
void APIENTRY
tritonOpenResource(D3D10DDI_HDEVICE hDevice, const D3D10DDIARG_OPENRESOURCE *pArgs,
                   D3D10DDI_HRESOURCE hResource, D3D10DDI_HRTRESOURCE hRTResource)
{
    PTRITON_DEVICE   pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_RESOURCE r  = (PTRITON_RESOURCE)(hResource.pDrvPrivate);
    if (!pD || !r) {
        tritonSetError(pD, E_INVALIDARG);
        return;
    }

    if (!pArgs || pArgs->NumAllocations != 1 || !pArgs->pOpenAllocationInfo ||
        !pArgs->pOpenAllocationInfo[0].hAllocation) {
        TR_STUB("OpenResource: no allocation info");
        tritonSetError(pD, E_NOTIMPL);
        return;
    }
    const VIOGPU_CREATE_ALLOCATION_EXCHANGE *ax =
        (const VIOGPU_CREATE_ALLOCATION_EXCHANGE *)
            pArgs->pOpenAllocationInfo[0].pPrivateDriverData;
    if (!ax ||
        pArgs->pOpenAllocationInfo[0].PrivateDriverDataSize < sizeof(*ax) ||
        (ax->Type != VIOGPU_RESOURCE_TYPE_SHARED &&
         ax->Type != VIOGPU_RESOURCE_TYPE_3D)) {
        TR_LOG("OpenResource: not an openable texture (type=%d)",
               ax ? (int)ax->Type : -1);
        tritonSetError(pD, E_NOTIMPL);
        return;
    }

    VIOGPU_RESOURCE_SHARED_TEXTURE_OPTIONS synth3d;
    const VIOGPU_RESOURCE_SHARED_TEXTURE_OPTIONS *so;
    if (ax->Type == VIOGPU_RESOURCE_TYPE_3D) {
        if (!tritonSynthesizeStandardTexture(&ax->Options3D, ax->Size, &synth3d)) {
            tritonSetError(pD, E_INVALIDARG);
            return;
        }
        so = &synth3d;
    } else {
        so = &ax->OptionsShared;
    }

#if defined(NPT_D3D10_RUNTIME_DDI)
    if (ax->Type == VIOGPU_RESOURCE_TYPE_SHARED &&
        !tritonValidateSharedOpen(so, ax->Size)) {
        tritonSetError(pD, E_INVALIDARG);
        return;
    }
#endif

    memset(r, 0, sizeof(*r));
    r->hRTResource        = hRTResource;
    r->Format             = (DXGI_FORMAT)so->format;
    r->Width              = so->width;
    r->Height             = so->height;
    r->Depth              = 1;
    r->MipLevels          = so->mip_levels ? so->mip_levels : 1;
    r->ArraySize          = so->array_size ? so->array_size : 1;
    r->BindFlags          = so->bind_flags;
    r->MiscFlags          = so->misc_flags;
    r->SampleDesc.Count   = so->sample_count ? so->sample_count : 1;
    r->SampleDesc.Quality = 0;
    r->IsShared           = TRUE;
    /* Ordinary shared consumers keep only their import attachment. Standard
     * primary consumers also need the runtime allocation as a Present target,
     * but must never deallocate that borrowed handle. */
#if defined(NPT_D3D10_RUNTIME_DDI)
    if (ax->Type == VIOGPU_RESOURCE_TYPE_3D &&
        ax->Options3D.flags == VIOGPU_RESOURCE_FLAG_STANDARD_PRIMARY) {
        r->BorrowedKMAllocation = TRUE;
        r->hKMAllocation = pArgs->pOpenAllocationInfo[0].hAllocation;
    }
#endif

    /* The KMD attaches opened allocations to this device's virtio
     * context; make sure it exists first. */
    tritonPresentEnsureRuntimeCtx(pD);

    /* The res_id was minted by the KMD when the exporter's allocation
     * bound the blob; recover it from the opened allocation. */
    VIOGPU_ESCAPE esc;
    memset(&esc, 0, sizeof(esc));
    esc.Type                   = VIOGPU_RES_INFO;
    esc.DataLength             = sizeof(esc.ResourceInfo);
    esc.ResourceInfo.ResHandle = pArgs->pOpenAllocationInfo[0].hAllocation;
    HRESULT hr = tritonPresentEscape(pD, &esc);
    if (FAILED(hr) || !esc.ResourceInfo.IsCreated || !esc.ResourceInfo.Id) {
        TR_LOG("OpenResource: RES_INFO failed hr=0x%08lx created=%d id=%u",
               hr, esc.ResourceInfo.IsCreated, esc.ResourceInfo.Id);
        tritonSetError(pD, E_FAIL);
        return;
    }
    const uint32_t res_id = esc.ResourceInfo.Id;

    /* Attach the resource to this process's transport context before the
     * open names it.  Same-process opens resolve without the attach, so a
     * failure here is not fatal. */
    if (!tritonSharedBridgeImportRes(pD->pDev1, res_id, so->allocation_size,
                                     &r->hImportAlloc, &r->hImportResKmt))
        TR_LOG("OpenResource: transport attach failed for res_id=%u "
               "(same-process open may still succeed)", res_id);

    struct triton_shared_texture_desc desc;
    memset(&desc, 0, sizeof(desc));
    desc.blob_id          = so->blob_id;
    desc.create_ctx_id    = so->create_ctx_id;
    desc.plane_count      = so->plane_count;
    desc.texture_layout   = so->texture_layout;
    desc.modifier         = so->modifier;
    desc.allocation_size  = so->allocation_size;
    for (UINT i = 0; i < so->plane_count && i < TRITON_SHARED_MAX_PLANES; i++) {
        desc.planes[i].offset = so->planes[i].offset;
        desc.planes[i].pitch  = so->planes[i].pitch;
    }
    desc.width            = so->width;
    desc.height           = so->height;
    desc.mip_levels       = so->mip_levels;
    desc.array_size       = so->array_size;
    desc.format           = so->format;
    desc.sample_count     = so->sample_count;
    desc.usage            = so->usage;
    desc.bind_flags       = so->bind_flags;
    desc.cpu_access_flags = so->cpu_access_flags;
    desc.misc_flags       = so->misc_flags;

    void *imported = NULL;
    if (!r->BorrowedKMAllocation ||
        tritonQueryStandardPrimary(pD, r, res_id, &desc))
        imported = tritonSharedBridgeOpenRes(pD->pDev1, res_id, &desc);
    if (!imported) {
        TR_LOG("shared: open res_id=%u failed", res_id);
        if (r->hImportAlloc || r->hImportResKmt) {
            if (tritonSharedBridgeReleaseImportRes(pD->pDev1,
                                                   r->hImportAlloc,
                                                   r->hImportResKmt)) {
                r->hImportAlloc = 0;
                r->hImportResKmt = 0;
            } else {
                TR_LOG("shared: failed-open import release failed alloc=%u resource=%u",
                       r->hImportAlloc, r->hImportResKmt);
            }
        }
        tritonSetError(pD, E_FAIL);
        return;
    }
    r->pResource = (ID3D11Resource *)imported;
    if (r->BorrowedKMAllocation) {
        r->HostFormat = (DXGI_FORMAT)desc.format;
        r->BindFlags = desc.bind_flags;
    }
    TR_LOG("shared: consumer opened res_id=%u %ux%u", res_id,
           r->Width, r->Height);
}

/* D3D10_DDI_MAP enum values match D3D11_MAP (READ=1, WRITE=2,
 * READWRITE=3, WRITE_DISCARD=4, WRITE_NOOVERWRITE=5) and
 * D3D10DDI_MAPPED_SUBRESOURCE has the same layout as
 * D3D11_MAPPED_SUBRESOURCE -- so we forward directly. */

void APIENTRY
tritonResourceMap(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE hResource,
                  UINT Subresource, D3D10_DDI_MAP DDIMap, UINT Flags,
                  D3D10DDI_MAPPED_SUBRESOURCE *pMapped)
{
    PTRITON_DEVICE   pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_RESOURCE r  = (PTRITON_RESOURCE)(hResource.pDrvPrivate);
    if (!pD || !r || !r->pResource || !pMapped) return;

    UINT mapFlags = 0;
    if (Flags & D3D10_DDI_MAP_FLAG_DONOTWAIT) mapFlags |= D3D11_MAP_FLAG_DO_NOT_WAIT;

    D3D11_MAPPED_SUBRESOURCE m = {};
    HRESULT hr = ID3D11DeviceContext1_Map(pD->pCtx1, r->pResource, Subresource,
                                          (D3D11_MAP)DDIMap, mapFlags, &m);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
        /* DO_NOT_WAIT + GPU busy: propagate to runtime so the app gets the
         * DXGI status from the next API call instead of reading garbage. */
        tritonSetError(pD, DXGI_DDI_ERR_WASSTILLDRAWING);
        pMapped->pData = NULL;
        pMapped->RowPitch = pMapped->DepthPitch = 0;
        return;
    }
    if (FAILED(hr)) {
        tritonSetError(pD, hr);
        pMapped->pData = NULL;
        pMapped->RowPitch = pMapped->DepthPitch = 0;
        return;
    }
    pMapped->pData      = m.pData;
    pMapped->RowPitch   = m.RowPitch;
    pMapped->DepthPitch = m.DepthPitch;
}

void APIENTRY
tritonResourceUnmap(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE hResource, UINT Subresource)
{
    PTRITON_DEVICE   pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_RESOURCE r  = (PTRITON_RESOURCE)(hResource.pDrvPrivate);
    if (!pD || !r || !r->pResource) return;
    ID3D11DeviceContext1_Unmap(pD->pCtx1, r->pResource, Subresource);
}

BOOL APIENTRY
tritonResourceIsStagingBusy(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE hResource)
{
    PTRITON_DEVICE   pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_RESOURCE r  = (PTRITON_RESOURCE)(hResource.pDrvPrivate);
    /* Conservative on missing inputs: returning FALSE would let the
     * runtime read garbage from a non-existent / bad staging map;
     * TRUE is always a legal, if pessimistic, answer. */
    if (!pD || !r || !r->pResource) return TRUE;

    /* D3D11 has no side-effect-free "is busy" query, so probe with a
     * non-blocking Map/Unmap. The map type MUST match the resource's CPU
     * access: a MAP_READ probe on a write-only (CPU_ACCESS_WRITE) staging
     * resource fails E_INVALIDARG, which would report "busy" forever and
     * defeat the runtime's non-blocking fast path. Prefer READ when the
     * resource is readable, else WRITE; no CPU access → not mappable. */
    D3D11_MAP mapType;
    if (r->MapFlags & D3D10_DDI_CPU_ACCESS_READ)
        mapType = D3D11_MAP_READ;
    else if (r->MapFlags & D3D10_DDI_CPU_ACCESS_WRITE)
        mapType = D3D11_MAP_WRITE;
    else
        return TRUE;

    D3D11_MAPPED_SUBRESOURCE m = {};
    HRESULT hr = ID3D11DeviceContext1_Map(pD->pCtx1, r->pResource, 0,
                                          mapType, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return TRUE;
    if (SUCCEEDED(hr)) {
        ID3D11DeviceContext1_Unmap(pD->pCtx1, r->pResource, 0);
        return FALSE;
    }
    return TRUE;
}

/* Byte order only: SRGB uploads retain their encoded values. */
static int
tritonUploadColorOrder(DXGI_FORMAT format)
{
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return 0;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return 1;
    default:
        return -1;
    }
}

/* Standard-primary imports can expose RGBA storage for logical BGRA. Preserve
 * the caller's pitches and destination box, without reading row padding or
 * applying the destination offset to the source. UpdateSubresource snapshots
 * the transformed memory before it returns, so the caller can free it then. */
static HRESULT
tritonPrepareUpload(PTRITON_RESOURCE r, UINT subresource,
                     const D3D10_DDI_BOX *box, const VOID *source,
                     UINT rowPitch, const VOID **upload, VOID **allocation)
{
    *upload = source;
    *allocation = NULL;
    if (box && (box->left >= box->right || box->top >= box->bottom ||
                box->front >= box->back))
        return S_FALSE;
    const int logicalOrder = tritonUploadColorOrder(r->Format);
    const int hostOrder = tritonUploadColorOrder(r->HostFormat);
    if (!r->HostFormat || r->Format == r->HostFormat)
        return S_OK;
    if (logicalOrder < 0 || hostOrder < 0)
        return E_INVALIDARG;
    if (logicalOrder == hostOrder)
        return S_OK;

    D3D11_RESOURCE_DIMENSION dimension;
    ID3D11Resource_GetType(r->pResource, &dimension);
    if (dimension != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
        return E_INVALIDARG;
    D3D11_TEXTURE2D_DESC desc;
    ID3D11Texture2D_GetDesc((ID3D11Texture2D *)r->pResource, &desc);
    if (!desc.Width || !desc.Height || !desc.MipLevels || desc.MipLevels > 32 ||
        !desc.ArraySize ||
        (UINT64)subresource >= (UINT64)desc.MipLevels * desc.ArraySize ||
        desc.SampleDesc.Count != 1 ||
        desc.Usage == D3D11_USAGE_IMMUTABLE || desc.Usage == D3D11_USAGE_DYNAMIC ||
        tritonUploadColorOrder(desc.Format) != hostOrder)
        return E_INVALIDARG;
    const UINT mip = subresource % desc.MipLevels;
    UINT width = desc.Width >> mip, height = desc.Height >> mip;
    if (!width) width = 1;
    if (!height) height = 1;
    if (box) {
        if (box->front || box->back != 1 || box->right > width || box->bottom > height)
            return E_INVALIDARG;
        width = box->right - box->left;
        height = box->bottom - box->top;
    }
    if (!source || width > UINT32_MAX / 4u || rowPitch < width * 4u)
        return E_INVALIDARG;
    const UINT rowBytes = width * 4u;
    const UINT64 extent = (UINT64)(height - 1u) * rowPitch + rowBytes;
    if (extent > (SIZE_T)-1)
        return E_OUTOFMEMORY;
    if ((uintptr_t)source > UINTPTR_MAX - (extent - 1u))
        return E_INVALIDARG;
    unsigned char *copy = (unsigned char *)HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, (SIZE_T)extent);
    if (!copy)
        return E_OUTOFMEMORY;
    for (UINT row = 0; row < height; ++row) {
        const unsigned char *in = (const unsigned char *)source + (SIZE_T)row * rowPitch;
        unsigned char *out = copy + (SIZE_T)row * rowPitch;
        for (UINT col = 0; col < width; ++col) {
            out[0] = in[2]; out[1] = in[1];
            out[2] = in[0]; out[3] = in[3];
            in += 4; out += 4;
        }
    }
    *upload = copy;
    *allocation = copy;
    return S_OK;
}

void APIENTRY
tritonResourceUpdateSubresourceUP(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE hResource,
                                  UINT DstSubresource, const D3D10_DDI_BOX *pDstBox,
                                  const VOID *pSrc, UINT SrcRowPitch, UINT SrcDepthPitch)
{
    PTRITON_DEVICE   pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_RESOURCE r  = (PTRITON_RESOURCE)(hResource.pDrvPrivate);
    if (!pD || !r || !r->pResource) return;
    const VOID *upload;
    VOID *allocation;
    HRESULT hr = tritonPrepareUpload(r, DstSubresource, pDstBox, pSrc,
                                      SrcRowPitch, &upload, &allocation);
    if (hr == S_FALSE) return;
    if (FAILED(hr)) { tritonSetError(pD, hr); return; }
    ID3D11DeviceContext1_UpdateSubresource(
        pD->pCtx1, r->pResource, DstSubresource,
        (const D3D11_BOX *)(pDstBox), upload, SrcRowPitch, SrcDepthPitch);
    if (allocation) HeapFree(GetProcessHeap(), 0, allocation);
}

void APIENTRY
tritonResourceUpdateSubresourceUP_11_1(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE hResource,
                                       UINT DstSubresource, const D3D10_DDI_BOX *pDstBox,
                                       const VOID *pSrc, UINT SrcRowPitch, UINT SrcDepthPitch,
                                       UINT CopyFlags)
{
    PTRITON_DEVICE   pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_RESOURCE r  = (PTRITON_RESOURCE)(hResource.pDrvPrivate);
    if (!pD || !r || !r->pResource) return;
    const VOID *upload;
    VOID *allocation;
    HRESULT hr = tritonPrepareUpload(r, DstSubresource, pDstBox, pSrc,
                                      SrcRowPitch, &upload, &allocation);
    if (hr == S_FALSE) return;
    if (FAILED(hr)) { tritonSetError(pD, hr); return; }
    ID3D11DeviceContext1_UpdateSubresource1(
        pD->pCtx1, r->pResource, DstSubresource,
        (const D3D11_BOX *)(pDstBox), upload, SrcRowPitch, SrcDepthPitch, CopyFlags);
    if (allocation) HeapFree(GetProcessHeap(), 0, allocation);
}

static BOOL
tritonResourceNeedsColorConversion(PTRITON_RESOURCE dst, PTRITON_RESOURCE src)
{
    return (dst->HostFormat || src->HostFormat) &&
        tritonResourceHostViewFormat(dst, dst->Format) !=
            tritonResourceHostViewFormat(src, src->Format);
}

void APIENTRY
tritonResourceCopy(D3D10DDI_HDEVICE hDevice,
                   D3D10DDI_HRESOURCE hDst, D3D10DDI_HRESOURCE hSrc)
{
    PTRITON_DEVICE   pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_RESOURCE d  = (PTRITON_RESOURCE)(hDst.pDrvPrivate);
    PTRITON_RESOURCE s  = (PTRITON_RESOURCE)(hSrc.pDrvPrivate);
    if (!pD || !d || !s || !d->pResource || !s->pResource) return;
    if (tritonResourceNeedsColorConversion(d, s)) {
        HRESULT hr = tritonResourceCopyConverted(pD, d, 0, 0, 0, 0, s, 0,
                                                  NULL, DXGI_FORMAT_UNKNOWN);
        if (FAILED(hr)) tritonSetError(pD, hr);
        return;
    }
    ID3D11DeviceContext1_CopyResource(pD->pCtx1, d->pResource, s->pResource);
}

void APIENTRY
tritonResourceCopyRegion(D3D10DDI_HDEVICE hDevice,
                         D3D10DDI_HRESOURCE hDst, UINT DstSubresource,
                         UINT DstX, UINT DstY, UINT DstZ,
                         D3D10DDI_HRESOURCE hSrc, UINT SrcSubresource,
                         const D3D10_DDI_BOX *pSrcBox)
{
    PTRITON_DEVICE   pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_RESOURCE d  = (PTRITON_RESOURCE)(hDst.pDrvPrivate);
    PTRITON_RESOURCE s  = (PTRITON_RESOURCE)(hSrc.pDrvPrivate);
    if (!pD || !d || !s || !d->pResource || !s->pResource) return;
    if (tritonResourceNeedsColorConversion(d, s)) {
        HRESULT hr = tritonResourceCopyConverted(pD, d, DstSubresource,
            DstX, DstY, DstZ, s, SrcSubresource,
            (const D3D11_BOX *)pSrcBox, DXGI_FORMAT_UNKNOWN);
        if (FAILED(hr)) tritonSetError(pD, hr);
        return;
    }
    ID3D11DeviceContext1_CopySubresourceRegion(
        pD->pCtx1, d->pResource, DstSubresource, DstX, DstY, DstZ,
        s->pResource, SrcSubresource, (const D3D11_BOX *)(pSrcBox));
}

void APIENTRY
tritonResourceCopyRegion_11_1(D3D10DDI_HDEVICE hDevice,
                              D3D10DDI_HRESOURCE hDst, UINT DstSubresource,
                              UINT DstX, UINT DstY, UINT DstZ,
                              D3D10DDI_HRESOURCE hSrc, UINT SrcSubresource,
                              const D3D10_DDI_BOX *pSrcBox, UINT CopyFlags)
{
    PTRITON_DEVICE   pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_RESOURCE d  = (PTRITON_RESOURCE)(hDst.pDrvPrivate);
    PTRITON_RESOURCE s  = (PTRITON_RESOURCE)(hSrc.pDrvPrivate);
    if (!pD || !d || !s || !d->pResource || !s->pResource) return;
    if (tritonResourceNeedsColorConversion(d, s)) {
        HRESULT hr = tritonResourceCopyConverted(pD, d, DstSubresource,
            DstX, DstY, DstZ, s, SrcSubresource,
            (const D3D11_BOX *)pSrcBox, DXGI_FORMAT_UNKNOWN);
        if (FAILED(hr)) tritonSetError(pD, hr);
        return;
    }
    ID3D11DeviceContext1_CopySubresourceRegion1(
        pD->pCtx1, d->pResource, DstSubresource, DstX, DstY, DstZ,
        s->pResource, SrcSubresource, (const D3D11_BOX *)(pSrcBox), CopyFlags);
}

void APIENTRY
tritonResourceResolveSubresource(D3D10DDI_HDEVICE hDevice,
                                 D3D10DDI_HRESOURCE hDst, UINT DstSubresource,
                                 D3D10DDI_HRESOURCE hSrc, UINT SrcSubresource,
                                 DXGI_FORMAT Format)
{
    PTRITON_DEVICE   pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_RESOURCE d  = (PTRITON_RESOURCE)(hDst.pDrvPrivate);
    PTRITON_RESOURCE s  = (PTRITON_RESOURCE)(hSrc.pDrvPrivate);
    if (!pD || !d || !s || !d->pResource || !s->pResource) return;
    if (tritonResourceNeedsColorConversion(d, s)) {
        HRESULT hr = tritonResourceCopyConverted(pD, d, DstSubresource,
            0, 0, 0, s, SrcSubresource, NULL, Format);
        if (FAILED(hr)) tritonSetError(pD, hr);
        return;
    }
    Format = tritonResourceHostViewFormat(s, Format);
    ID3D11DeviceContext1_ResolveSubresource(
        pD->pCtx1, d->pResource, DstSubresource, s->pResource, SrcSubresource, Format);
}

/* D3D11 tracks hazards on the immediate context; nothing to do here. */

void APIENTRY
tritonResourceReadAfterWriteHazard(D3D10DDI_HDEVICE hDev, D3D10DDI_HRESOURCE hRes) {}

void APIENTRY
tritonShaderResourceViewReadAfterWriteHazard(D3D10DDI_HDEVICE hDev, D3D10DDI_HSHADERRESOURCEVIEW hSRV, D3D10DDI_HRESOURCE hRes) {}

/* Per-stage constant-buffer setters. D3D11_0 takes only handles
 * (whole-buffer bind); D3D11_1 adds FirstConstant + NumConstants for
 * partial bind. */

/* Fill `out` with at most `cap` entries. Returns FALSE if any slot-range
 * check fails (StartSlot < cap, NumBuffers <= cap, StartSlot + NumBuffers
 * <= cap); callers must NOT issue the D3D11 bind in that case (would
 * OOB-read out). */
static BOOL tritonFillConstantBuffers(UINT Start, UINT N, const D3D10DDI_HRESOURCE *phB,
                                      ID3D11Buffer **out, UINT cap)
{
    if (Start >= cap || N > cap || Start + N > cap) return FALSE;
    for (UINT i = 0; i < N; ++i) {
        PTRITON_RESOURCE r = phB
            ? (PTRITON_RESOURCE)(phB[i].pDrvPrivate)
            : NULL;
        out[i] = r ? (ID3D11Buffer *)r->pResource : NULL;
    }
    return TRUE;
}

#define TR_CB_SETTER_10(stage, ctx_method)                                                  \
void APIENTRY                                                                    \
tritonCs_ ## stage ## _Set10(D3D10DDI_HDEVICE hDevice, UINT Start, UINT N,                  \
                             const D3D10DDI_HRESOURCE *phB)                                 \
{                                                                                           \
    PTRITON_DEVICE pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);              \
    if (!pD) return;                                                                        \
    ID3D11Buffer *aB[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT] = {};               \
    if (!tritonFillConstantBuffers(Start, N, phB, aB,                                       \
                       D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT)) {                \
        tritonSetError(pD, E_INVALIDARG);                                                   \
        return;                                                                             \
    }                                                                                       \
    ID3D11DeviceContext1_##ctx_method(pD->pCtx1, Start, N, aB);                             \
}

TR_CB_SETTER_10(VS, VSSetConstantBuffers)
TR_CB_SETTER_10(PS, PSSetConstantBuffers)
TR_CB_SETTER_10(GS, GSSetConstantBuffers)
TR_CB_SETTER_10(HS, HSSetConstantBuffers)
TR_CB_SETTER_10(DS, DSSetConstantBuffers)
TR_CB_SETTER_10(CS, CSSetConstantBuffers)

#undef TR_CB_SETTER_10

#define TR_CB_SETTER_11_1(stage, ctx_method)                                                \
void APIENTRY                                                                    \
tritonCs_ ## stage ## _Set11_1(D3D10DDI_HDEVICE hDevice, UINT Start, UINT N,                \
                               const D3D10DDI_HRESOURCE *phB,                               \
                               const UINT *pFirst, const UINT *pNum)                        \
{                                                                                           \
    PTRITON_DEVICE pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);              \
    if (!pD) return;                                                                        \
    ID3D11Buffer *aB[D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT] = {};               \
    if (!tritonFillConstantBuffers(Start, N, phB, aB,                                       \
                       D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT)) {                \
        tritonSetError(pD, E_INVALIDARG);                                                   \
        return;                                                                             \
    }                                                                                       \
    ID3D11DeviceContext1_##ctx_method(pD->pCtx1, Start, N, aB, pFirst, pNum);               \
}

TR_CB_SETTER_11_1(VS, VSSetConstantBuffers1)
TR_CB_SETTER_11_1(PS, PSSetConstantBuffers1)
TR_CB_SETTER_11_1(GS, GSSetConstantBuffers1)
TR_CB_SETTER_11_1(HS, HSSetConstantBuffers1)
TR_CB_SETTER_11_1(DS, DSSetConstantBuffers1)
TR_CB_SETTER_11_1(CS, CSSetConstantBuffers1)

#undef TR_CB_SETTER_11_1

/* DefaultConstantBufferUpdateSubresourceUP: runtime hint that the given
 * subresource is a constant buffer being updated in full. Route to
 * UpdateSubresource(1). */

void APIENTRY
tritonDefaultCbUpdateSubresourceUP(D3D10DDI_HDEVICE hDevice,
                                   D3D10DDI_HRESOURCE hResource, UINT DstSubresource,
                                   const D3D10_DDI_BOX *pDstBox, const VOID *pSrc,
                                   UINT SrcRowPitch, UINT SrcDepthPitch)
{
    tritonResourceUpdateSubresourceUP(hDevice, hResource, DstSubresource,
                                      pDstBox, pSrc, SrcRowPitch, SrcDepthPitch);
}

void APIENTRY
tritonDefaultCbUpdateSubresourceUP_11_1(D3D10DDI_HDEVICE hDevice,
                                        D3D10DDI_HRESOURCE hResource, UINT DstSubresource,
                                        const D3D10_DDI_BOX *pDstBox, const VOID *pSrc,
                                        UINT SrcRowPitch, UINT SrcDepthPitch,
                                        UINT CopyFlags)
{
    tritonResourceUpdateSubresourceUP_11_1(hDevice, hResource, DstSubresource,
                                           pDstBox, pSrc, SrcRowPitch, SrcDepthPitch, CopyFlags);
}

/* WDDM 1.3 tiled-resources.
 *
 * D3DWDDM1_3DDI_TILED_RESOURCE_COORDINATE / TILE_REGION_SIZE are
 * layout-compatible with D3D11_TILED_RESOURCE_COORDINATE /
 * D3D11_TILE_REGION_SIZE (same field types in the same order). The DDI
 * flag enums match the D3D11 ones bit-for-bit. Forward verbatim. */

void APIENTRY
tritonUpdateTileMappings(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE hTiledRes,
                         UINT NumRegions,
                         const D3DWDDM1_3DDI_TILED_RESOURCE_COORDINATE *pRegionCoords,
                         const D3DWDDM1_3DDI_TILE_REGION_SIZE *pRegionSizes,
                         D3D10DDI_HRESOURCE hTilePool, UINT NumRanges,
                         const UINT *pRangeFlags, const UINT *pTilePoolStartOffsets,
                         const UINT *pRangeTileCounts, UINT Flags)
{
    PTRITON_DEVICE   pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_RESOURCE r  = (PTRITON_RESOURCE)(hTiledRes.pDrvPrivate);
    PTRITON_RESOURCE pool = (PTRITON_RESOURCE)(hTilePool.pDrvPrivate);
    if (!pD || !pD->pCtx2 || !r || !r->pResource) {
        if (!pD || !pD->pCtx2) TR_STUB("UpdateTileMappings (no Device2)");
        return;
    }
    HRESULT hr = ID3D11DeviceContext2_UpdateTileMappings(
        pD->pCtx2, r->pResource, NumRegions,
        (const D3D11_TILED_RESOURCE_COORDINATE *)pRegionCoords,
        (const D3D11_TILE_REGION_SIZE *)pRegionSizes,
        pool ? (ID3D11Buffer *)pool->pResource : NULL,
        NumRanges, pRangeFlags, pTilePoolStartOffsets, pRangeTileCounts, Flags);
    if (FAILED(hr)) tritonSetError(pD, hr);
}

void APIENTRY
tritonCopyTileMappings(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE hDstRes,
                       const D3DWDDM1_3DDI_TILED_RESOURCE_COORDINATE *pDstCoord,
                       D3D10DDI_HRESOURCE hSrcRes,
                       const D3DWDDM1_3DDI_TILED_RESOURCE_COORDINATE *pSrcCoord,
                       const D3DWDDM1_3DDI_TILE_REGION_SIZE *pRegionSize, UINT Flags)
{
    PTRITON_DEVICE   pD  = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_RESOURCE rD  = (PTRITON_RESOURCE)(hDstRes.pDrvPrivate);
    PTRITON_RESOURCE rS  = (PTRITON_RESOURCE)(hSrcRes.pDrvPrivate);
    if (!pD || !pD->pCtx2 || !rD || !rS || !rD->pResource || !rS->pResource) {
        if (!pD || !pD->pCtx2) TR_STUB("CopyTileMappings (no Device2)");
        return;
    }
    HRESULT hr = ID3D11DeviceContext2_CopyTileMappings(
        pD->pCtx2, rD->pResource, (const D3D11_TILED_RESOURCE_COORDINATE *)pDstCoord,
        rS->pResource, (const D3D11_TILED_RESOURCE_COORDINATE *)pSrcCoord,
        (const D3D11_TILE_REGION_SIZE *)pRegionSize, Flags);
    if (FAILED(hr)) tritonSetError(pD, hr);
}

void APIENTRY
tritonCopyTiles(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE hTiledRes,
                const D3DWDDM1_3DDI_TILED_RESOURCE_COORDINATE *pCoord,
                const D3DWDDM1_3DDI_TILE_REGION_SIZE *pSize,
                D3D10DDI_HRESOURCE hBuffer, UINT64 BufferStartOffsetBytes, UINT Flags)
{
    PTRITON_DEVICE   pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_RESOURCE r  = (PTRITON_RESOURCE)(hTiledRes.pDrvPrivate);
    PTRITON_RESOURCE b  = (PTRITON_RESOURCE)(hBuffer.pDrvPrivate);
    if (!pD || !pD->pCtx2 || !r || !b || !r->pResource || !b->pResource) {
        if (!pD || !pD->pCtx2) TR_STUB("CopyTiles (no Device2)");
        return;
    }
    ID3D11DeviceContext2_CopyTiles(pD->pCtx2, r->pResource,
        (const D3D11_TILED_RESOURCE_COORDINATE *)pCoord,
        (const D3D11_TILE_REGION_SIZE *)pSize,
        (ID3D11Buffer *)b->pResource, BufferStartOffsetBytes, Flags);
}

void APIENTRY
tritonUpdateTiles(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE hDstRes,
                  const D3DWDDM1_3DDI_TILED_RESOURCE_COORDINATE *pDstCoord,
                  const D3DWDDM1_3DDI_TILE_REGION_SIZE *pDstSize,
                  const VOID *pSourceTileData, UINT Flags)
{
    PTRITON_DEVICE   pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_RESOURCE r  = (PTRITON_RESOURCE)(hDstRes.pDrvPrivate);
    if (!pD || !pD->pCtx2 || !r || !r->pResource) {
        if (!pD || !pD->pCtx2) TR_STUB("UpdateTiles (no Device2)");
        return;
    }
    ID3D11DeviceContext2_UpdateTiles(pD->pCtx2, r->pResource,
        (const D3D11_TILED_RESOURCE_COORDINATE *)pDstCoord,
        (const D3D11_TILE_REGION_SIZE *)pDstSize, pSourceTileData, Flags);
}

/* Convert (HandleType, void*) → ID3D11DeviceChild* using the leading
 * PTRITON_*VIEW / PTRITON_RESOURCE layouts. NULL handle stays NULL. */
static ID3D11DeviceChild *tritonHandleToDeviceChild(D3D11DDI_HANDLETYPE t, void *h)
{
    if (!h) return NULL;
    switch (t) {
    case D3D10DDI_HT_RESOURCE: {
        PTRITON_RESOURCE r = (PTRITON_RESOURCE)(h);
        return r ? (ID3D11DeviceChild *)r->pResource : NULL;
    }
    case D3D10DDI_HT_SHADERRESOURCEVIEW: {
        PTRITON_SRVIEW v = (PTRITON_SRVIEW)(h);
        return v ? (ID3D11DeviceChild *)v->pSRV : NULL;
    }
    case D3D10DDI_HT_RENDERTARGETVIEW: {
        PTRITON_RTVIEW v = (PTRITON_RTVIEW)(h);
        return v ? (ID3D11DeviceChild *)v->pRTV : NULL;
    }
    case D3D10DDI_HT_DEPTHSTENCILVIEW: {
        PTRITON_DSVIEW v = (PTRITON_DSVIEW)(h);
        return v ? (ID3D11DeviceChild *)v->pDSV : NULL;
    }
    case D3D11DDI_HT_UNORDEREDACCESSVIEW: {
        PTRITON_UAVIEW v = (PTRITON_UAVIEW)(h);
        return v ? (ID3D11DeviceChild *)v->pUAV : NULL;
    }
    default:
        return NULL;
    }
}

void APIENTRY
tritonTiledResourceBarrier(D3D10DDI_HDEVICE hDevice,
                           D3D11DDI_HANDLETYPE tBefore, VOID *hBefore,
                           D3D11DDI_HANDLETYPE tAfter,  VOID *hAfter)
{
    PTRITON_DEVICE pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    if (!pD || !pD->pCtx2) { TR_STUB("TiledResourceBarrier (no Device2)"); return; }
    ID3D11DeviceContext2_TiledResourceBarrier(pD->pCtx2,
        tritonHandleToDeviceChild(tBefore, hBefore),
        tritonHandleToDeviceChild(tAfter,  hAfter));
}

void APIENTRY
tritonGetMipPacking(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE hTiledRes,
                    UINT *pNumPackedMips, UINT *pNumTilesForPackedMips)
{
    if (pNumPackedMips)         *pNumPackedMips = 0;
    if (pNumTilesForPackedMips) *pNumTilesForPackedMips = 0;

    PTRITON_DEVICE   pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_RESOURCE r  = (PTRITON_RESOURCE)(hTiledRes.pDrvPrivate);
    if (!pD || !pD->pDev2 || !r || !r->pResource) {
        if (!pD || !pD->pDev2) TR_STUB("GetMipPacking (no Device2)");
        return;
    }
    /* The DDI only asks for the packed-mip subset of GetResourceTiling's
     * output; pass NULLs for the rest. */
    D3D11_PACKED_MIP_DESC mipDesc = {};
    UINT numSubresourceTilings = 0;
    ID3D11Device2_GetResourceTiling(pD->pDev2, r->pResource,
        NULL /* total tiles */, &mipDesc,
        NULL /* tile shape */, &numSubresourceTilings,
        0 /* first */, NULL /* per-subresource tilings */);
    if (pNumPackedMips)         *pNumPackedMips         = mipDesc.NumPackedMips;
    if (pNumTilesForPackedMips) *pNumTilesForPackedMips = mipDesc.NumTilesForPackedMips;
}

void APIENTRY
tritonResizeTilePool(D3D10DDI_HDEVICE hDevice, D3D10DDI_HRESOURCE hTilePool,
                     UINT64 NewSizeInBytes)
{
    PTRITON_DEVICE   pD = (PTRITON_DEVICE)(hDevice.pDrvPrivate);
    PTRITON_RESOURCE r  = (PTRITON_RESOURCE)(hTilePool.pDrvPrivate);
    if (!pD || !pD->pCtx2 || !r || !r->pResource) {
        if (!pD || !pD->pCtx2) TR_STUB("ResizeTilePool (no Device2)");
        return;
    }
    HRESULT hr = ID3D11DeviceContext2_ResizeTilePool(
        pD->pCtx2, (ID3D11Buffer *)r->pResource, NewSizeInBytes);
    if (FAILED(hr)) tritonSetError(pD, hr);
}

/* WDDM 2.0: GetResourceLayout returns the kernel allocation handle plus
 * driver-internal texture swizzle / row-major layout metadata. D3D11
 * exposes none of this at the UMD level. */

void APIENTRY
tritonGetResourceLayout(D3D10DDI_HDEVICE hDev, D3D10DDI_HRESOURCE hRes, UINT SubresourceCount,
                        D3DKMT_HANDLE *pAllocation,
                        D3DWDDM2_0DDI_TEXTURE_LAYOUT *pLayout,
                        UINT *pMipLevelSwizzleTransition,
                        D3DWDDM2_0DDI_SUBRESOURCE_LAYOUT *pSubresources)
{
    TR_STUB("GetResourceLayout (no D3D11 equivalent)");
    if (pAllocation)                *pAllocation = 0;
    if (pLayout)                    *pLayout = D3DWDDM2_0DDI_TL_UNDEFINED;
    if (pMipLevelSwizzleTransition) *pMipLevelSwizzleTransition = 0;
    if (pSubresources)
        ZeroMemory(pSubresources, SubresourceCount * sizeof(*pSubresources));
}

/* WDDM 2.0: hardware content protection. D3D11 exposes this only via
 * the video-DDI surfaces; the non-video pfnSetHardwareProtection{,State}
 * DDI entries have no public Device-level API equivalent. */

void APIENTRY
tritonSetHardwareProtection(D3D10DDI_HDEVICE hDev, D3D10DDI_HRESOURCE hRes, BOOL flag)
{
    TR_STUB("SetHardwareProtection (no D3D11 equivalent)");
}

void APIENTRY
tritonSetHardwareProtectionState(D3D10DDI_HDEVICE hDev, BOOL flag)
{
    TR_STUB("SetHardwareProtectionState (no D3D11 equivalent)");
}

/* WDDM 2.1: sync-token acquire/release for cross-process shared-resource
 * handoff. D3D11's analogue is IDXGIKeyedMutex / ID3D11Multithread, which
 * apps drive directly through DXGI, not the UMD. */

void APIENTRY
tritonAcquireResource(D3D10DDI_HDEVICE hDev, D3D10DDI_HRESOURCE hRes, HANDLE h)
{
    TR_STUB("AcquireResource (no D3D11 equivalent)");
}

void APIENTRY
tritonReleaseResource(D3D10DDI_HDEVICE hDev, D3D10DDI_HRESOURCE hRes, HANDLE h)
{
    TR_STUB("ReleaseResource (no D3D11 equivalent)");
}
