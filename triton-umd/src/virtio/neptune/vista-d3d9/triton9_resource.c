/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Vista D3D9 resource, lock, sharing and present plumbing.  DEFAULT memory
 * lives in the Neptune D3D11 proxy.  For D3DDDIPOOL_SYSTEMMEM, Vista owns the
 * pSysMem buffer and its pitch; the UMD retains that exact backing store and
 * synchronizes it at Blt/Lock/Unlock boundaries.  Other lockable resources
 * use a UMD-owned CPU shadow.
 */

#include "triton9.h"
#include "triton9_cpu_layout.h"

/* Defined in triton9_ddi.c; kept deliberately tiny so early DDI failures
 * can be inspected after a QMP-only reboot into Safe Mode. */
extern void triton9Diag(const char *message);

#include "../triton/tritonSharedBridge.h"
#include "npt_shared_texture.h"

#include <limits.h>
#include <string.h>

#define TRITON9_HOST_DRAIN_TIMEOUT_MS 15000u

typedef struct TRITON9_RENAME_COOKIE {
    TRITON9_RESOURCE *resource;
} TRITON9_RENAME_COOKIE;

static BOOL
triton9ValidateSharedPlane(uint64_t offset, uint64_t pitch,
                           uint64_t allocationSize, UINT width, UINT height,
                           UINT bytesPerPixel)
{
    uint64_t rowBytes;

    if (!pitch || !allocationSize || !width || !height || !bytesPerPixel ||
        width > UINT64_MAX / bytesPerPixel)
        return FALSE;
    rowBytes = (uint64_t)width * bytesPerPixel;
    if (pitch < rowBytes || pitch > UINT_MAX || offset > UINT_MAX ||
        offset >= allocationSize || rowBytes > allocationSize - offset)
        return FALSE;
    return (uint64_t)(height - 1u) <=
           (allocationSize - offset - rowBytes) / pitch;
}

static BOOL
triton9SharedFormat(DXGI_FORMAT format, ULONG *virglFormat)
{
    ULONG value;

    switch (format) {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        value = 1; /* VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM */
        break;
    case DXGI_FORMAT_B8G8R8X8_UNORM:
        value = 2; /* VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM */
        break;
    default:
        return FALSE;
    }
    if (virglFormat)
        *virglFormat = value;
    return TRUE;
}

static UINT
triton9FullMipCount(UINT width, UINT height)
{
    UINT largest = width > height ? width : height;
    UINT count = 1;

    while (largest > 1) {
        largest >>= 1;
        ++count;
    }
    return count;
}

static HRESULT
triton9CreateShadow(TRITON9_RESOURCE *resource,
                    const D3DDDI_SURFACEINFO *surface)
{
    TRITON9_CPU_BACKING backing;
    TRITON9_CPU_LAYOUT layout;
    SIZE_T sourcePitch;
    BOOL systemMemory;

    if (!resource || !resource->bytesPerPixel)
        return E_INVALIDARG;
    systemMemory = triton9ResourceIsSystemMemory(resource);
    if (!triton9CpuBackingInit(&backing,
                               surface ? (void *)(uintptr_t)surface->pSysMem : NULL,
                               systemMemory))
        return D3DDDIERR_INVALIDUSERBUFFER;

    if (resource->isBuffer) {
        if (!triton9CpuLayoutLinear(&layout, resource->width))
            return E_OUTOFMEMORY;
    } else {
        if (systemMemory && !surface->SysMemPitch)
            return D3DDDIERR_INVALIDUSERBUFFER;
        if (!triton9CpuLayout2D(&layout, resource->width, resource->height,
                                resource->bytesPerPixel,
                                systemMemory ? surface->SysMemPitch : 0,
                                systemMemory ? surface->SysMemSlicePitch : 0))
            return systemMemory ? D3DDDIERR_INVALIDUSERBUFFER : E_OUTOFMEMORY;
    }
    if (layout.rowBytes > UINT_MAX || layout.rowPitch > UINT_MAX ||
        layout.slicePitch > UINT_MAX)
        return E_OUTOFMEMORY;

    resource->rowBytes = (UINT)layout.rowBytes;
    resource->pitch = (UINT)layout.rowPitch;
    resource->slicePitch = (UINT)layout.slicePitch;
    resource->shadowSize = layout.dataSize;
    if (systemMemory) {
        /* pSysMem is declared const because CreateResource receives initial
         * contents.  Vista subsequently exposes this same allocation to the
         * application for writes and sends NotifyOnly Lock/Unlock calls so the
         * UMD can synchronize its host mirror. */
        resource->shadow = backing.data;
        resource->ownsShadow = backing.driverOwned;
        return S_OK;
    }

    resource->shadow = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                         resource->shadowSize ? resource->shadowSize : 1);
    if (!resource->shadow)
        return E_OUTOFMEMORY;
    resource->ownsShadow = backing.driverOwned;

    if (!surface || !surface->pSysMem)
        return S_OK;
    if (resource->isBuffer) {
        memcpy(resource->shadow, surface->pSysMem, resource->shadowSize);
        return S_OK;
    }
    sourcePitch = surface->SysMemPitch ? surface->SysMemPitch : resource->rowBytes;
    if (sourcePitch < resource->rowBytes)
        return D3DDDIERR_INVALIDUSERBUFFER;
    triton9CpuCopyRows(resource->shadow, resource->pitch, surface->pSysMem,
                       sourcePitch, resource->rowBytes, resource->height);
    return S_OK;
}

static HRESULT
triton9RegisterSharedTexture(TRITON9_DEVICE *device, TRITON9_RESOURCE *resource);
static HRESULT
triton9AllocateStandardPrimary(TRITON9_DEVICE *device,
                                TRITON9_RESOURCE *resource);
static HRESULT
triton9DeallocateResource(TRITON9_DEVICE *device,
                          TRITON9_RESOURCE *resource);
static HRESULT
triton9UploadShadow(TRITON9_DEVICE *device, TRITON9_RESOURCE *resource);
static HRESULT
triton9UploadLockedShadow(TRITON9_DEVICE *device, TRITON9_RESOURCE *resource);
static void
triton9ReleaseResourceViews(TRITON9_RESOURCE *resource);

static void
triton9ClearLockRegion(TRITON9_RESOURCE *resource)
{
    if (!resource)
        return;
    resource->lockRangeValid = FALSE;
    resource->lockAreaValid = FALSE;
    ZeroMemory(&resource->lockRange, sizeof(resource->lockRange));
    ZeroMemory(&resource->lockArea, sizeof(resource->lockArea));
}

static HRESULT
triton9CreateHostBuffer(TRITON9_DEVICE *device, TRITON9_RESOURCE *resource)
{
    D3D11_BUFFER_DESC desc;
    D3D11_SUBRESOURCE_DATA initial;
    D3D11_SUBRESOURCE_DATA *initialPtr = NULL;
    ID3D11Buffer *buffer = NULL;
    HRESULT hr;

    ZeroMemory(&desc, sizeof(desc));
    desc.ByteWidth = resource->width;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = resource->format == D3DDDIFMT_INDEX16 ||
                     resource->format == D3DDDIFMT_INDEX32
        ? D3D11_BIND_INDEX_BUFFER : D3D11_BIND_VERTEX_BUFFER;
    if (resource->hasInitialData) {
        ZeroMemory(&initial, sizeof(initial));
        initial.pSysMem = resource->shadow;
        initialPtr = &initial;
    }
    hr = ID3D11Device1_CreateBuffer(device->hostDevice, &desc, initialPtr, &buffer);
    if (FAILED(hr) || !buffer)
        return FAILED(hr) ? triton9MapDeviceFailure(device, hr) : E_FAIL;
    resource->hostResource = (ID3D11Resource *)buffer;
    resource->hostBindFlags = desc.BindFlags;
    return S_OK;
}

static HRESULT
triton9CreateHostTexture(TRITON9_DEVICE *device, TRITON9_RESOURCE *resource)
{
    D3D11_TEXTURE2D_DESC desc;
    D3D11_SUBRESOURCE_DATA initial;
    D3D11_SUBRESOURCE_DATA *initialPtr = NULL;
    ID3D11Texture2D *texture = NULL;
    HRESULT hr;

    ZeroMemory(&desc, sizeof(desc));
    desc.Width = resource->width;
    desc.Height = resource->height;
    desc.MipLevels = resource->mipLevels;
    desc.ArraySize = 1;
    desc.Format = resource->hostFormat;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    if (resource->isDepthStencil)
        desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    else {
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        /* OFFSCREENPLAIN is advertised for the two 32-bit colour formats.
         * Their DEFAULT-pool textures must therefore support ColorFill even
         * when the declaration did not include RenderTarget. */
        if (resource->wantsRenderTarget || resource->isPrimary ||
            resource->wantsAutogenMipmap ||
            resource->hostFormat == DXGI_FORMAT_B8G8R8A8_UNORM ||
            resource->hostFormat == DXGI_FORMAT_B8G8R8X8_UNORM)
            desc.BindFlags |= D3D11_BIND_RENDER_TARGET;
        if (resource->wantsAutogenMipmap)
            desc.MiscFlags |= D3D11_RESOURCE_MISC_GENERATE_MIPS;
    }
    /* D3D11 requires data for every mip when it is supplied at creation.
     * Auto-mipmap resources expose only mip zero to the Vista runtime, so
     * upload that level after creation instead. */
    if (resource->hasInitialData && resource->mipLevels == 1) {
        ZeroMemory(&initial, sizeof(initial));
        initial.pSysMem = resource->shadow;
        initial.SysMemPitch = resource->pitch;
        initial.SysMemSlicePitch = resource->slicePitch;
        initialPtr = &initial;
    }
    hr = ID3D11Device1_CreateTexture2D(device->hostDevice, &desc, initialPtr,
                                        &texture);
    if (FAILED(hr) || !texture)
        return FAILED(hr) ? triton9MapDeviceFailure(device, hr) : E_FAIL;
    resource->hostResource = (ID3D11Resource *)texture;
    resource->hostBindFlags = desc.BindFlags;
    return S_OK;
}

HRESULT
triton9EnsureResourceHost(TRITON9_DEVICE *device, TRITON9_RESOURCE *resource)
{
    HRESULT hr;

    if (!triton9ResourceBelongsToDevice(device, resource))
        return E_INVALIDARG;
    if (resource->hostReady && resource->hostResource)
        return S_OK;
    /* A prior lazy-materialization attempt can leave a host object alive when
     * its KMD registration or initial upload fails.  Retire that incomplete
     * transaction before a retry.  If deallocation is not proven, preserve
     * every dependent object and fail closed. */
    if (resource->hostResource) {
        hr = triton9DeallocateResource(device, resource);
        if (FAILED(hr))
            return hr;
        triton9ReleaseResourceViews(resource);
        if (resource->fvfDeclaration) {
            triton9DestroyFvfDeclaration(device, resource->fvfDeclaration);
            resource->fvfDeclaration = NULL;
        }
        ID3D11Resource_Release(resource->hostResource);
        resource->hostResource = NULL;
        resource->hostBindFlags = 0;
    }
    hr = triton9EnsureHostDevice(device);
    if (FAILED(hr))
        return hr;

    if (resource->isBuffer) {
        hr = triton9CreateHostBuffer(device, resource);
        if (SUCCEEDED(hr) && resource->fvf && !resource->fvfDeclaration)
            hr = triton9CreateFvfDeclaration(device, resource->fvf,
                                              &resource->fvfDeclaration);
    } else if (resource->isPrimary && !resource->needsPresentAllocation) {
        /* The KMD standard primary is deliberately separate from this
         * D3D11 texture.  Keep a local texture only so legacy D3D9 state
         * setup can bind a primary; never export it or use it for scanout. */
        hr = triton9CreateHostTexture(device, resource);
    } else if (resource->isShared || resource->needsPresentAllocation) {
        resource->hostResource = (ID3D11Resource *)
            npt_shared_texture_create_exportable(device->hostDevice,
                                                 resource->width,
                                                 resource->height,
                                                 npt_shared_texture_host_format(
                                                     resource->hostFormat));
        if (!resource->hostResource) {
            hr = E_FAIL;
            goto fail;
        }
        resource->hostBindFlags = D3D11_BIND_SHADER_RESOURCE |
                                  D3D11_BIND_RENDER_TARGET;
        hr = triton9RegisterSharedTexture(device, resource);
    } else {
        hr = triton9CreateHostTexture(device, resource);
    }
    if (SUCCEEDED(hr))
        hr = triton9CheckHostDevice(device);
    if (FAILED(hr))
        goto fail;
    if (resource->hasInitialData) {
        EnterCriticalSection(&device->shaderLock);
        hr = triton9UploadShadow(device, resource);
        if (SUCCEEDED(hr))
            hr = triton9CheckHostDevice(device);
        LeaveCriticalSection(&device->shaderLock);
    }
    if (FAILED(hr))
        goto fail;
    resource->hostReady = TRUE;
    return S_OK;

fail:
    resource->hostReady = FALSE;
    /* Do not release an exported host object until its KMD allocation is
     * retired.  A failed cleanup remains visible to the next retry. */
    if (resource->hostResource) {
        HRESULT cleanupHr = triton9DeallocateResource(device, resource);
        if (SUCCEEDED(cleanupHr)) {
            triton9ReleaseResourceViews(resource);
            if (resource->fvfDeclaration) {
                triton9DestroyFvfDeclaration(device, resource->fvfDeclaration);
                resource->fvfDeclaration = NULL;
            }
            ID3D11Resource_Release(resource->hostResource);
            resource->hostResource = NULL;
            resource->hostBindFlags = 0;
        } else {
            device->deviceLost = TRUE;
            hr = cleanupHr;
        }
    }
    return triton9MapDeviceFailure(device, hr);
}

HRESULT
triton9GetRenderTargetView(TRITON9_DEVICE *device, TRITON9_RESOURCE *resource,
                           ID3D11RenderTargetView **view)
{
    HRESULT hr;

    if (!triton9ResourceBelongsToDevice(device, resource) || !view ||
        !resource->hostReady || !resource->hostResource ||
        resource->isBuffer || triton9ResourceIsSystemMemory(resource) ||
        resource->format == D3DDDIFMT_D16 ||
        resource->format == D3DDDIFMT_D24S8 ||
        !(resource->hostBindFlags & D3D11_BIND_RENDER_TARGET))
        return D3DDDIERR_NOTAVAILABLE;
    if (!resource->renderTargetView) {
        hr = ID3D11Device1_CreateRenderTargetView(device->hostDevice,
                                                    resource->hostResource,
                                                    NULL,
                                                    &resource->renderTargetView);
        if (FAILED(hr) || !resource->renderTargetView)
            return FAILED(hr) ? triton9MapDeviceFailure(device, hr) : E_FAIL;
    }
    *view = resource->renderTargetView;
    return S_OK;
}

HRESULT
triton9GetDepthStencilView(TRITON9_DEVICE *device, TRITON9_RESOURCE *resource,
                           ID3D11DepthStencilView **view)
{
    HRESULT hr;

    if (!triton9ResourceBelongsToDevice(device, resource) || !view ||
        !resource->hostReady || !resource->hostResource ||
        resource->isBuffer || triton9ResourceIsSystemMemory(resource) ||
        (resource->format != D3DDDIFMT_D16 &&
        resource->format != D3DDDIFMT_D24S8) ||
        !(resource->hostBindFlags & D3D11_BIND_DEPTH_STENCIL))
        return D3DDDIERR_NOTAVAILABLE;
    if (!resource->depthStencilView) {
        hr = ID3D11Device1_CreateDepthStencilView(device->hostDevice,
                                                    resource->hostResource,
                                                    NULL,
                                                    &resource->depthStencilView);
        if (FAILED(hr) || !resource->depthStencilView)
            return FAILED(hr) ? triton9MapDeviceFailure(device, hr) : E_FAIL;
    }
    *view = resource->depthStencilView;
    return S_OK;
}

HRESULT
triton9GetShaderResourceView(TRITON9_DEVICE *device, TRITON9_RESOURCE *resource,
                             ID3D11ShaderResourceView **view)
{
    HRESULT hr;

    if (!triton9ResourceBelongsToDevice(device, resource) || !view ||
        !resource->hostReady || !resource->hostResource ||
        resource->isBuffer || resource->format == D3DDDIFMT_D16 ||
        resource->format == D3DDDIFMT_D24S8 ||
        !(resource->hostBindFlags & D3D11_BIND_SHADER_RESOURCE))
        return D3DDDIERR_NOTAVAILABLE;
    if (!resource->shaderResourceView) {
        hr = ID3D11Device1_CreateShaderResourceView(device->hostDevice,
                                                      resource->hostResource,
                                                      NULL,
                                                      &resource->shaderResourceView);
        if (FAILED(hr) || !resource->shaderResourceView)
            return FAILED(hr) ? triton9MapDeviceFailure(device, hr) : E_FAIL;
    }
    *view = resource->shaderResourceView;
    return S_OK;
}

static HRESULT
triton9RegisterSharedTexture(TRITON9_DEVICE *device, TRITON9_RESOURCE *resource)
{
    struct triton_shared_texture_desc exported;
    VIOGPU_CREATE_ALLOCATION_EXCHANGE allocation;
    VIOGPU_RESOURCE_SHARED_TEXTURE_OPTIONS *options;
    D3DDDI_ALLOCATIONINFO allocationInfo;
    D3DDDICB_ALLOCATE callback;
    ULONG scanoutFormat;
    HRESULT hr;
    UINT i;

    if (!triton9ResourceBelongsToDevice(device, resource) ||
        (resource->isPrimary && !resource->needsPresentAllocation) || !resource->hostResource ||
        !triton9SharedFormat(resource->hostFormat, &scanoutFormat) ||
        !device->callbacks.pfnAllocateCb)
        return D3DDDIERR_NOTAVAILABLE;
    triton9Diag("TRITON9-SHARED-REGISTER enter\n");
    hr = triton9EnsureRuntimeContext(device);
    if (FAILED(hr))
        return hr;
    ZeroMemory(&exported, sizeof(exported));
    if (!tritonSharedBridgeExportBlob(resource->hostResource, &exported)) {
        triton9Diag("TRITON9-SHARED-EXPORT fail\n");
        return E_FAIL;
    }
    triton9DiagU32("TRITON9-SHARED-EXPORT-CTX", exported.create_ctx_id);
    /* Treat export metadata as untrusted transport input. In particular,
     * allocation.Size is later converted to SIZE_T by the KMD. Reject a
     * value that would wrap either the page alignment or a Vista x86
     * SIZE_T. */
    if (!exported.blob_id || exported.plane_count != 1 ||
        exported.texture_layout > 2 ||
        !exported.allocation_size ||
        exported.allocation_size > (uint64_t)((SIZE_T)-1) - 4095ull)
        return E_FAIL;
    if (!triton9ValidateSharedPlane(exported.planes[0].offset,
                                    exported.planes[0].pitch,
                                    exported.allocation_size,
                                    resource->width, resource->height,
                                    resource->bytesPerPixel))
        return E_FAIL;

    ZeroMemory(&allocation, sizeof(allocation));
    allocation.Type = VIOGPU_RESOURCE_TYPE_SHARED;
    options = &allocation.OptionsShared;
    options->blob_id = exported.blob_id;
    options->create_ctx_id = exported.create_ctx_id;
    options->plane_count = exported.plane_count;
    options->texture_layout = exported.texture_layout;
    options->modifier = exported.modifier;
    options->allocation_size = exported.allocation_size;
    /* Every flip-chain member is both renderable and directly scannable.
     * Ordinary blt sources remain non-primary exported textures. */
    options->primary = resource->isPrimary;
    options->width = resource->width;
    options->height = resource->height;
    options->mip_levels = resource->mipLevels;
    options->array_size = 1;
    options->format = resource->hostFormat;
    options->sample_count = 1;
    options->usage = D3D11_USAGE_DEFAULT;
    options->bind_flags = D3D11_BIND_SHADER_RESOURCE |
                          D3D11_BIND_RENDER_TARGET;
    options->misc_flags = D3D11_RESOURCE_MISC_SHARED;
    for (i = 0; i < exported.plane_count && i < 4; ++i) {
        options->planes[i].offset = exported.planes[i].offset;
        options->planes[i].pitch = exported.planes[i].pitch;
    }
    /* The KMD's shared-resource describe path needs these for every shared
     * texture, not just a scanout primary. */
    options->ScanoutInfo.width = resource->width;
    options->ScanoutInfo.height = resource->height;
    options->ScanoutInfo.format = scanoutFormat;
    if (exported.plane_count) {
        options->ScanoutInfo.strides[0] = (ULONG)exported.planes[0].pitch;
        options->ScanoutInfo.offsets[0] = (ULONG)exported.planes[0].offset;
    }
    allocation.Size = (exported.allocation_size + 4095ull) & ~4095ull;
    if (!allocation.Size)
        allocation.Size = ((ULONGLONG)resource->pitch * resource->height + 4095ull) & ~4095ull;

    ZeroMemory(&allocationInfo, sizeof(allocationInfo));
    allocationInfo.pPrivateDriverData = &allocation;
    allocationInfo.PrivateDriverDataSize = sizeof(allocation);
    allocationInfo.VidPnSourceId = resource->vidPnSourceId;
    allocationInfo.Flags.Primary = resource->isPrimary;
    ZeroMemory(&callback, sizeof(callback));
    /* A Present allocation belongs to this runtime surface even when the
     * surface is not shared. Vista's fullscreen path needs that association
     * before it asks the surface for its primary allocation. */
    callback.hResource = resource->independentAllocation ? NULL : resource->hRTResource;
    callback.NumAllocations = 1;
    callback.pAllocationInfo = &allocationInfo;
    hr = device->callbacks.pfnAllocateCb(device->hRTDevice, &callback);
    if (FAILED(hr) || !allocationInfo.hAllocation) {
        triton9DiagU32("TRITON9-SHARED-ALLOCATE-FAIL",
                       FAILED(hr) ? (DWORD)hr : (DWORD)E_FAIL);
        return FAILED(hr) ? hr : E_FAIL;
    }
    resource->hKMAllocation = allocationInfo.hAllocation;
    resource->ownsKMAllocation = TRUE;
    resource->kmResourceAssociated = callback.hResource != NULL;
    triton9DiagU32("TRITON9-SHARED-ALLOC", allocationInfo.hAllocation);
    return S_OK;
}

/* Vista's display Primary is not DWM's rendered image.  It is the legacy
 * non-blob target that the KMD selects for scanout after Present.  Allocate
 * it as one ordinary allocation (hResource == NULL), mirroring the Vista
 * WDDM lifetime rule used by VBox's D3D9 path.  In contrast, the DWM colour
 * render target is allocated later as an exported shared blob. */
static HRESULT
triton9AllocateStandardPrimary(TRITON9_DEVICE *device,
                                TRITON9_RESOURCE *resource)
{
    VIOGPU_CREATE_ALLOCATION_EXCHANGE allocation;
    D3DDDI_ALLOCATIONINFO allocationInfo;
    D3DDDICB_ALLOCATE callback;
    ULONG virglFormat;
    uint64_t byteSize;
    HRESULT hr;

    if (!triton9ResourceBelongsToDevice(device, resource) ||
        !resource->isPrimary ||
        resource->hKMAllocation)
        return E_INVALIDARG;
    if (!device->callbacks.pfnAllocateCb || !resource->width ||
        !resource->height || resource->bytesPerPixel != 4 ||
        resource->pitch != resource->rowBytes ||
        !triton9SharedFormat(resource->hostFormat, &virglFormat))
        return D3DDDIERR_NOTAVAILABLE;
    byteSize = (uint64_t)resource->rowBytes * resource->height;
    if (!byteSize || byteSize > (uint64_t)((SIZE_T)-1))
        return E_OUTOFMEMORY;

    ZeroMemory(&allocation, sizeof(allocation));
    allocation.Type = VIOGPU_RESOURCE_TYPE_3D;
    allocation.Options3D.target = 2; /* PIPE_TEXTURE_2D */
    allocation.Options3D.format = virglFormat;
    allocation.Options3D.bind =
        (1u << 1) |  /* VIRGL_BIND_RENDER_TARGET */
        (1u << 3) |  /* VIRGL_BIND_SAMPLER_VIEW */
        (1u << 7) |  /* VIRGL_BIND_DISPLAY_TARGET */
        (1u << 18);  /* VIRGL_BIND_SCANOUT */
    allocation.Options3D.width = resource->width;
    allocation.Options3D.height = resource->height;
    allocation.Options3D.depth = 1;
    allocation.Options3D.array_size = 1;
    allocation.Options3D.last_level = 0;
    allocation.Options3D.nr_samples = 0;
    allocation.Options3D.flags = VIOGPU_RESOURCE_FLAG_STANDARD_PRIMARY;
    allocation.Size = byteSize;

    ZeroMemory(&allocationInfo, sizeof(allocationInfo));
    allocationInfo.pPrivateDriverData = &allocation;
    allocationInfo.PrivateDriverDataSize = sizeof(allocation);
    allocationInfo.VidPnSourceId = resource->vidPnSourceId;
    allocationInfo.Flags.Primary = 1;
    ZeroMemory(&callback, sizeof(callback));
    callback.hResource = NULL;
    callback.NumAllocations = 1;
    callback.pAllocationInfo = &allocationInfo;
    hr = device->callbacks.pfnAllocateCb(device->hRTDevice, &callback);
    if (FAILED(hr) || !allocationInfo.hAllocation) {
        triton9DiagU32("TRITON9-PRIMARY-ALLOCATE-FAIL",
                       FAILED(hr) ? (DWORD)hr : (DWORD)E_FAIL);
        return FAILED(hr) ? hr : E_FAIL;
    }
    resource->hKMAllocation = allocationInfo.hAllocation;
    resource->ownsKMAllocation = TRUE;
    triton9DiagU32("TRITON9-PRIMARY-ALLOC", allocationInfo.hAllocation);
    return S_OK;
}

/* Destroy associated allocations with their runtime resource; use allocation
 * handles only for allocations that were attached directly to the device. */
static HRESULT
triton9DeallocateResource(TRITON9_DEVICE *device, TRITON9_RESOURCE *resource)
{
    D3DDDICB_DEALLOCATE deallocate;
    HRESULT hr;

    if (!triton9ResourceBelongsToDevice(device, resource) ||
        !resource->ownsKMAllocation ||
        !resource->hKMAllocation || !device->callbacks.pfnDeallocateCb)
        return S_OK;
    ZeroMemory(&deallocate, sizeof(deallocate));
    if (resource->isShared || resource->kmResourceAssociated) {
        deallocate.hResource = resource->hRTResource;
    } else {
        deallocate.NumAllocations = 1;
        deallocate.HandleList = &resource->hKMAllocation;
    }
    hr = triton9MapDeviceFailure(device,
        device->callbacks.pfnDeallocateCb(device->hRTDevice, &deallocate));
    if (SUCCEEDED(hr)) {
        resource->hKMAllocation = 0;
        resource->ownsKMAllocation = FALSE;
        resource->kmResourceAssociated = FALSE;
    }
    return hr;
}

static HRESULT
triton9CloneHostResource(TRITON9_DEVICE *device, TRITON9_RESOURCE *resource,
                         ID3D11Resource **outResource)
{
    HRESULT hr;
    void *object = NULL;

    if (!triton9ResourceBelongsToDevice(device, resource) ||
        !resource->hostResource || !outResource)
        return E_INVALIDARG;
    *outResource = NULL;
    if (resource->isBuffer) {
        ID3D11Buffer *oldBuffer;
        ID3D11Buffer *newBuffer = NULL;
        D3D11_BUFFER_DESC desc;

        hr = ID3D11Resource_QueryInterface(resource->hostResource, &IID_ID3D11Buffer,
                                            &object);
        if (FAILED(hr) || !object)
            return FAILED(hr) ? hr : E_FAIL;
        oldBuffer = (ID3D11Buffer *)object;
        ID3D11Buffer_GetDesc(oldBuffer, &desc);
        ID3D11Buffer_Release(oldBuffer);
        hr = ID3D11Device1_CreateBuffer(device->hostDevice, &desc, NULL,
                                        &newBuffer);
        if (FAILED(hr) || !newBuffer)
            return FAILED(hr) ? triton9MapDeviceFailure(device, hr) : E_FAIL;
        *outResource = (ID3D11Resource *)newBuffer;
        return S_OK;
    }
    {
        ID3D11Texture2D *oldTexture;
        ID3D11Texture2D *newTexture = NULL;
        D3D11_TEXTURE2D_DESC desc;

        hr = ID3D11Resource_QueryInterface(resource->hostResource,
                                            &IID_ID3D11Texture2D, &object);
        if (FAILED(hr) || !object)
            return FAILED(hr) ? hr : E_FAIL;
        oldTexture = (ID3D11Texture2D *)object;
        ID3D11Texture2D_GetDesc(oldTexture, &desc);
        ID3D11Texture2D_Release(oldTexture);
        hr = ID3D11Device1_CreateTexture2D(device->hostDevice, &desc, NULL,
                                            &newTexture);
        if (FAILED(hr) || !newTexture)
            return FAILED(hr) ? triton9MapDeviceFailure(device, hr) : E_FAIL;
        *outResource = (ID3D11Resource *)newTexture;
        return S_OK;
    }
}

static HRESULT
triton9GetHostBuffer(TRITON9_RESOURCE *resource, UINT requiredBind,
                     ID3D11Buffer **outBuffer)
{
    void *object = NULL;
    HRESULT hr;

    if (!resource || !outBuffer || !resource->isBuffer || !resource->hostResource ||
        !(resource->hostBindFlags & requiredBind))
        return D3DDDIERR_INVALIDCALL;
    *outBuffer = NULL;
    hr = ID3D11Resource_QueryInterface(resource->hostResource, &IID_ID3D11Buffer,
                                        &object);
    if (FAILED(hr) || !object)
        return FAILED(hr) ? hr : E_FAIL;
    *outBuffer = (ID3D11Buffer *)object;
    return S_OK;
}

static void
triton9ReleaseResourceViews(TRITON9_RESOURCE *resource)
{
    if (!resource)
        return;
    if (resource->shaderResourceView)
        ID3D11ShaderResourceView_Release(resource->shaderResourceView);
    if (resource->depthStencilView)
        ID3D11DepthStencilView_Release(resource->depthStencilView);
    if (resource->renderTargetView)
        ID3D11RenderTargetView_Release(resource->renderTargetView);
    if (resource->stagingResource)
        ID3D11Resource_Release(resource->stagingResource);
    resource->shaderResourceView = NULL;
    resource->depthStencilView = NULL;
    resource->renderTargetView = NULL;
    resource->stagingResource = NULL;
}

static HRESULT
triton9UploadShadow(TRITON9_DEVICE *device, TRITON9_RESOURCE *resource)
{
    if (!triton9ResourceBelongsToDevice(device, resource) ||
        !resource->hostResource || !resource->shadow)
        return E_INVALIDARG;
    if (resource->isBuffer)
        ID3D11DeviceContext1_UpdateSubresource(device->hostContext,
                                                resource->hostResource, 0,
                                                NULL, resource->shadow, 0, 0);
    else
        ID3D11DeviceContext1_UpdateSubresource(device->hostContext,
                                                resource->hostResource, 0,
                                                NULL, resource->shadow,
                                                resource->pitch,
                                                resource->slicePitch);
    return triton9CheckHostDevice(device);
}

/* Upload only the bytes exposed by the active lock.  This preserves host
 * bytes outside a WriteOnly or NoOverwrite region without fabricating them
 * from a stale CPU shadow.  The caller owns shaderLock. */
static HRESULT
triton9UploadLockedShadow(TRITON9_DEVICE *device, TRITON9_RESOURCE *resource)
{
    D3D11_BOX box;
    const BYTE *source;

    if (!triton9ResourceBelongsToDevice(device, resource) ||
        !resource->hostResource || !resource->shadow)
        return E_INVALIDARG;
    if (!resource->lockRangeValid && !resource->lockAreaValid)
        return triton9UploadShadow(device, resource);

    ZeroMemory(&box, sizeof(box));
    box.front = 0;
    box.back = 1;
    if (resource->isBuffer && resource->lockRangeValid) {
        box.left = resource->lockRange.Offset;
        box.right = resource->lockRange.Offset + resource->lockRange.Size;
        box.top = 0;
        box.bottom = 1;
        source = resource->shadow + resource->lockRange.Offset;
        ID3D11DeviceContext1_UpdateSubresource(device->hostContext,
            resource->hostResource, 0, &box, source, 0, 0);
    } else if (!resource->isBuffer && resource->lockAreaValid) {
        box.left = (UINT)resource->lockArea.left;
        box.right = (UINT)resource->lockArea.right;
        box.top = (UINT)resource->lockArea.top;
        box.bottom = (UINT)resource->lockArea.bottom;
        source = resource->shadow +
                 (SIZE_T)resource->lockArea.top * resource->pitch +
                 (SIZE_T)resource->lockArea.left * resource->bytesPerPixel;
        ID3D11DeviceContext1_UpdateSubresource(device->hostContext,
            resource->hostResource, 0, &box, source, resource->pitch,
            resource->slicePitch);
    } else {
        return D3DDDIERR_INVALIDCALL;
    }
    return triton9CheckHostDevice(device);
}

/* SYSTEMMEM's Vista-owned buffer is canonical.  Before a persistent D3D11
 * binding reads its mirror, refresh that mirror from pSysMem.  Blt/TexBlt use
 * the CPU buffer directly and therefore do not need this path. */
HRESULT
triton9PrepareResourceForHostRead(TRITON9_DEVICE *device,
                                  TRITON9_RESOURCE *resource)
{
    HRESULT hr;

    if (!triton9ResourceBelongsToDevice(device, resource))
        return E_INVALIDARG;
    hr = triton9EnsureResourceHost(device, resource);
    if (FAILED(hr) || !triton9ResourceIsSystemMemory(resource))
        return hr;
    if (!resource->hostResource || !resource->shadow)
        return D3DDDIERR_INVALIDUSERBUFFER;
    EnterCriticalSection(&device->shaderLock);
    hr = triton9UploadShadow(device, resource);
    LeaveCriticalSection(&device->shaderLock);
    return triton9MapDeviceFailure(device, hr);
}

HRESULT
triton9CommitSystemMemoryWrite(TRITON9_DEVICE *device,
                               TRITON9_RESOURCE *resource)
{
    HRESULT hr;

    if (!triton9ResourceBelongsToDevice(device, resource) ||
        !triton9ResourceIsSystemMemory(resource))
        return E_INVALIDARG;
    if (!resource->hostResource)
        return S_OK;
    if (!resource->shadow)
        return D3DDDIERR_INVALIDUSERBUFFER;
    EnterCriticalSection(&device->shaderLock);
    hr = triton9UploadShadow(device, resource);
    LeaveCriticalSection(&device->shaderLock);
    return triton9MapDeviceFailure(device, hr);
}

/* The caller holds shaderLock.  Vista creates an internal dynamic vertex
 * buffer during device bring-up with MightDrawFromLocked set.  This is not an
 * unsupported resource class: the flag says that a later lock is permitted
 * to remain live across a draw.  Our D3D11 DEFAULT buffer is backed by the
 * CPU shadow, so make that shadow visible at each such draw rather than
 * pretending that the flag is absent or relying on an unsafe user pointer. */
HRESULT
triton9SynchronizeLockedBuffers(TRITON9_DEVICE *device)
{
    TRITON9_RESOURCE *seen[TRITON9_MAX_VERTEX_STREAMS + 1];
    UINT seenCount = 0;
    UINT stream;
    HRESULT hr;

    if (!device || !device->hostContext)
        return E_INVALIDARG;
    for (stream = 0; stream < TRITON9_MAX_VERTEX_STREAMS + 1; ++stream) {
        TRITON9_RESOURCE *resource =
            stream < TRITON9_MAX_VERTEX_STREAMS ?
                device->streamResources[stream] : device->indexResource;
        UINT prior;

        if (!resource || !resource->locked || !resource->canDrawWhileLocked)
            continue;
        for (prior = 0; prior < seenCount; ++prior) {
            if (seen[prior] == resource)
                break;
        }
        if (prior != seenCount)
            continue;
        seen[seenCount++] = resource;
        if (!triton9ResourceBelongsToDevice(device, resource) ||
            !resource->isBuffer || !resource->hostResource ||
            !resource->shadow)
            return D3DDDIERR_INVALIDCALL;
        hr = triton9UploadLockedShadow(device, resource);
        if (FAILED(hr))
            return triton9MapDeviceFailure(device, hr);
    }
    return S_OK;
}

HRESULT
triton9EnsureStagingResource(TRITON9_DEVICE *device, TRITON9_RESOURCE *resource)
{
    HRESULT hr;

    if (!triton9ResourceBelongsToDevice(device, resource) ||
        !device->hostDevice)
        return E_INVALIDARG;
    if (resource->stagingResource)
        return S_OK;
    if (resource->isBuffer) {
        D3D11_BUFFER_DESC desc;
        ID3D11Buffer *buffer = NULL;

        ZeroMemory(&desc, sizeof(desc));
        desc.ByteWidth = resource->width;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        hr = ID3D11Device1_CreateBuffer(device->hostDevice, &desc, NULL, &buffer);
        if (FAILED(hr) || !buffer)
            return FAILED(hr) ? triton9MapDeviceFailure(device, hr) : E_FAIL;
        resource->stagingResource = (ID3D11Resource *)buffer;
    } else {
        D3D11_TEXTURE2D_DESC desc;
        ID3D11Texture2D *texture = NULL;

        ZeroMemory(&desc, sizeof(desc));
        desc.Width = resource->width;
        desc.Height = resource->height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = resource->hostFormat;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        hr = ID3D11Device1_CreateTexture2D(device->hostDevice, &desc, NULL,
                                            &texture);
        if (FAILED(hr) || !texture)
            return FAILED(hr) ? triton9MapDeviceFailure(device, hr) : E_FAIL;
        resource->stagingResource = (ID3D11Resource *)texture;
    }
    return S_OK;
}

static HRESULT
triton9MapStagingForRead(TRITON9_DEVICE *device, TRITON9_RESOURCE *resource,
                         D3D11_MAPPED_SUBRESOURCE *mapped)
{
    HRESULT hr;

    if (!triton9ResourceBelongsToDevice(device, resource) || !mapped ||
        !resource->shadow ||
        !resource->shadowSize || !resource->stagingResource)
        return E_INVALIDARG;
    /* The Neptune D3D11 wrapper returns from Clear/Copy after queuing host
     * work.  A staging Map is not a completion fence for that transport, so
     * explicitly retire an ordered marker before reading CPU-visible bytes. */
    ID3D11DeviceContext1_Flush(device->hostContext);
    if (!tritonSharedBridgeDrain(device->hostContext,
                                 TRITON9_HOST_DRAIN_TIMEOUT_MS)) {
        device->deviceLost = TRUE;
        return D3DDDIERR_DEVICEREMOVED;
    }
    /* The Vista runtime-DDI renderer cannot call D3DKMTGetDeviceState with
     * the runtime's opaque device handle.  Query the host device immediately
     * after the ordered event.  This distinguishes a reset wake before Map
     * can expose CPU bytes. */
    hr = triton9CheckHostDevice(device);
    if (FAILED(hr))
        return hr;
    ZeroMemory(mapped, sizeof(*mapped));
    hr = ID3D11DeviceContext1_Map(device->hostContext, resource->stagingResource,
                                  0, D3D11_MAP_READ, 0, mapped);
    if (FAILED(hr))
        return triton9MapDeviceFailure(device, hr);
    if (!mapped->pData) {
        ID3D11DeviceContext1_Unmap(device->hostContext, resource->stagingResource,
                                    0);
        return E_FAIL;
    }
    return S_OK;
}

/* The caller holds shaderLock.  Blt must complete video-memory to SYSTEMMEM
 * copies into Vista's pSysMem allocation before returning. */
HRESULT
triton9CopyStagingSurfaceToShadow(TRITON9_DEVICE *device,
                                  TRITON9_RESOURCE *resource,
                                  const RECT *region)
{
    TRITON9_CPU_LAYOUT destinationLayout;
    TRITON9_CPU_LAYOUT sourceLayout;
    D3D11_MAPPED_SUBRESOURCE mapped;
    SIZE_T xBytes;
    SIZE_T copyBytes;
    UINT rows;
    HRESULT hr;

    if (!triton9ResourceBelongsToDevice(device, resource) || !region ||
        resource->isBuffer ||
        region->left < 0 || region->top < 0 ||
        region->right <= region->left || region->bottom <= region->top ||
        (UINT)region->right > resource->width ||
        (UINT)region->bottom > resource->height)
        return E_INVALIDARG;
    xBytes = (SIZE_T)region->left * resource->bytesPerPixel;
    copyBytes = (SIZE_T)(region->right - region->left) *
                resource->bytesPerPixel;
    rows = (UINT)(region->bottom - region->top);
    destinationLayout.rowBytes = resource->rowBytes;
    destinationLayout.rowPitch = resource->pitch;
    destinationLayout.rowCount = resource->height;
    destinationLayout.slicePitch = resource->slicePitch;
    destinationLayout.dataSize = resource->shadowSize;
    if (!triton9CpuRegionFits(&destinationLayout, xBytes, region->top,
                              copyBytes, rows))
        return D3DDDIERR_INVALIDUSERBUFFER;

    hr = triton9MapStagingForRead(device, resource, &mapped);
    if (FAILED(hr))
        return hr;
    if (!triton9CpuLayout2D(&sourceLayout, resource->width, resource->height,
                            resource->bytesPerPixel, mapped.RowPitch,
                            mapped.DepthPitch) ||
        !triton9CpuRegionFits(&sourceLayout, xBytes, region->top,
                              copyBytes, rows)) {
        ID3D11DeviceContext1_Unmap(device->hostContext,
                                    resource->stagingResource, 0);
        return E_FAIL;
    }
    triton9CpuCopyRows(resource->shadow + (SIZE_T)region->top * resource->pitch +
                           xBytes,
                       resource->pitch,
                       (const BYTE *)mapped.pData +
                           (SIZE_T)region->top * mapped.RowPitch + xBytes,
                       mapped.RowPitch, copyBytes, rows);
    ID3D11DeviceContext1_Unmap(device->hostContext, resource->stagingResource,
                                0);
    return triton9CheckHostDevice(device);
}

/* The caller holds shaderLock. */
HRESULT
triton9CopyStagingBufferToShadow(TRITON9_DEVICE *device,
                                 TRITON9_RESOURCE *resource,
                                 UINT offset, UINT size)
{
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr;

    if (!triton9ResourceBelongsToDevice(device, resource) ||
        !resource->isBuffer || !size ||
        offset > resource->shadowSize || size > resource->shadowSize - offset)
        return E_INVALIDARG;
    hr = triton9MapStagingForRead(device, resource, &mapped);
    if (FAILED(hr))
        return hr;
    memcpy(resource->shadow + offset, (const BYTE *)mapped.pData + offset, size);
    ID3D11DeviceContext1_Unmap(device->hostContext, resource->stagingResource,
                                0);
    return triton9CheckHostDevice(device);
}

/* The caller holds shaderLock.  A CPU shadow is authoritative for CPU
 * writes, but GPU rendering can make it stale.  Refresh it before a lock that
 * promises current bytes to the D3D9 runtime. */
static HRESULT
triton9ReadbackShadow(TRITON9_DEVICE *device, TRITON9_RESOURCE *resource)
{
    HRESULT hr;

    if (!triton9ResourceBelongsToDevice(device, resource) ||
        !resource->hostResource)
        return E_INVALIDARG;
    hr = triton9EnsureStagingResource(device, resource);
    if (FAILED(hr))
        return hr;
    ID3D11DeviceContext1_CopySubresourceRegion(device->hostContext,
                                                resource->stagingResource, 0,
                                                0, 0, 0, resource->hostResource,
                                                0, NULL);
    if (resource->isBuffer)
        return triton9CopyStagingBufferToShadow(device, resource, 0,
                                                (UINT)resource->shadowSize);
    {
        RECT full = { 0, 0, (LONG)resource->width, (LONG)resource->height };
        return triton9CopyStagingSurfaceToShadow(device, resource, &full);
    }
}

static HRESULT
triton9CreateSingleResource(HANDLE hDevice, D3DDDIARG_CREATERESOURCE *args,
                            BOOL independentAllocation)
{
    TRITON9_DEVICE *device = (TRITON9_DEVICE *)hDevice;
    TRITON9_RESOURCE *resource;
    const TRITON9_FORMAT *format;
    BOOL vertexBuffer;
    BOOL indexBuffer;
    HRESULT hr;

    triton9Diag("TRITON9-CREATERESOURCE enter\n");
    if (!device || !args || !args->pSurfList || !args->SurfCount)
        return E_INVALIDARG;
    triton9DiagU32("TRITON9-RESOURCE-FORMAT", args->Format);
    triton9DiagU32("TRITON9-RESOURCE-POOL", args->Pool);
    triton9DiagU32("TRITON9-RESOURCE-FLAGS", args->Flags.Value);
    triton9DiagU32("TRITON9-RESOURCE-SURFACES", args->SurfCount);
    triton9DiagU32("TRITON9-RESOURCE-WIDTH", args->pSurfList[0].Width);
    triton9DiagU32("TRITON9-RESOURCE-HEIGHT", args->pSurfList[0].Height);
    if (device->deviceLost)
        return D3DDDIERR_DEVICEREMOVED;
    /* CreateDeviceEx creates bootstrap resources before it returns the
     * runtime device.  Starting Neptune here can submit callbacks into that
     * still-unpublished device and strand the public call.  Preserve the D3D9
     * declaration and CPU shadow now; the first operational use materializes
     * the host resource after the runtime has returned. */
    if (args->Pool < D3DDDIPOOL_SYSTEMMEM ||
        args->Pool > D3DDDIPOOL_NONLOCALVIDMEM)
        return E_INVALIDARG;
    if (args->MultisampleType != D3DDDIMULTISAMPLE_NONE ||
        args->MultisampleQuality)
        return D3DDDIERR_NOTAVAILABLE;
    if (args->Rotation && args->Rotation != D3DDDI_ROTATION_IDENTITY)
        return D3DDDIERR_NOTAVAILABLE;
    if (args->Flags.Video || args->Flags.Overlay || args->Flags.Volume ||
        args->Flags.CubeMap || args->Flags.DecodeRenderTarget ||
        args->Flags.DecodeCompressedBuffer || args->Flags.VideoProcessRenderTarget ||
        args->Flags.DMap || args->Flags.Points || args->Flags.RtPatches ||
        args->Flags.NPatches || args->Flags.CaptureBuffer ||
        args->Flags.InterlacedRefresh ||
        args->Flags.TextApi || args->Flags.RestrictedContent ||
        args->Flags.RestrictSharedAccess)
        return D3DDDIERR_NOTAVAILABLE;
    /* Explicit multi-mip resources expose every subresource to the DDI. They
     * remain unsupported until lock, copy, and views cover each level. Auto
     * mipmaps are different: Vista exposes only mip zero and lets the driver
     * create the hidden chain. */
    if (args->SurfCount != 1 || (args->MipLevels && args->MipLevels != 1))
        return D3DDDIERR_NOTAVAILABLE;

    resource = (TRITON9_RESOURCE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                              sizeof(*resource));
    if (!resource)
        return E_OUTOFMEMORY;
    resource->hOwnerDevice = (HANDLE)(uintptr_t)device;
    resource->hRTResource = args->hResource;
    resource->independentAllocation = independentAllocation;
    resource->format = args->Format;
    resource->fvf = args->Fvf;
    resource->mipLevels = 1;
    resource->surfaceCount = 1;
    resource->vidPnSourceId = args->VidPnSourceId;
    resource->isPrimary = args->Flags.Primary;
    /* DWM flip-chain primaries and ordinary colour back buffers both need
     * exported present allocations. The standalone standard primary keeps
     * its existing non-blob destination allocation. */
    resource->isShared = args->Flags.SharedResource;
    resource->pool = args->Pool;
    resource->isDepthStencil = args->Flags.ZBuffer;
    resource->wantsRenderTarget = args->Flags.RenderTarget;
    resource->needsPresentAllocation =
        (!resource->isPrimary || independentAllocation) && !args->Flags.Texture &&
        resource->wantsRenderTarget && !resource->isDepthStencil;
    resource->wantsAutogenMipmap = args->Flags.AutogenMipmap;
    resource->hasInitialData = args->pSurfList[0].pSysMem != NULL;
    resource->notLockable = args->Flags.NotLockable;
    resource->writeOnly = args->Flags.WriteOnly;
    resource->canDrawWhileLocked = args->Flags.MightDrawFromLocked;
    vertexBuffer = args->Flags.VertexBuffer || args->Format == D3DDDIFMT_VERTEXDATA;
    indexBuffer = args->Flags.IndexBuffer || args->Format == D3DDDIFMT_INDEX16 ||
                  args->Format == D3DDDIFMT_INDEX32;
    if (vertexBuffer && indexBuffer) {
        hr = D3DDDIERR_INVALIDCALL;
        goto fail;
    }
    resource->isBuffer = vertexBuffer || indexBuffer;

    if (resource->isBuffer) {
        resource->width = args->pSurfList[0].Width;
        resource->height = 1;
        resource->depth = 1;
        resource->bytesPerPixel = 1;
        if (!resource->width || resource->isPrimary || resource->isShared ||
            args->Flags.RenderTarget || args->Flags.ZBuffer ||
            args->Flags.AutogenMipmap || args->Flags.Texture ||
            (vertexBuffer && args->Format != D3DDDIFMT_VERTEXDATA) ||
            (indexBuffer && args->Format != D3DDDIFMT_INDEX16 &&
             args->Format != D3DDDIFMT_INDEX32) || (indexBuffer && resource->fvf)) {
            hr = D3DDDIERR_NOTAVAILABLE;
            goto fail;
        }
        hr = triton9CreateShadow(resource, &args->pSurfList[0]);
        if (FAILED(hr))
            goto fail;
    } else if (resource->fvf) {
        hr = D3DDDIERR_INVALIDCALL;
        goto fail;
    } else {
        format = triton9FormatLookup(args->Format);
        if (!format) {
            hr = D3DDDIERR_NOTAVAILABLE;
            goto fail;
        }
        resource->hostFormat = format->hostFormat;
        resource->bytesPerPixel = format->bytesPerPixel;
        resource->width = args->pSurfList[0].Width;
        resource->height = args->pSurfList[0].Height;
        resource->depth = args->pSurfList[0].Depth ? args->pSurfList[0].Depth : 1;
        if (!resource->width || !resource->height || resource->width > 4096 ||
            resource->height > 4096 || resource->depth != 1 ||
            (format->depthStencil != (args->Flags.ZBuffer != 0)) ||
            (format->depthStencil && (args->Flags.RenderTarget ||
                                      resource->isPrimary || resource->isShared))) {
            hr = D3DDDIERR_NOTAVAILABLE;
            goto fail;
        }
        if ((resource->isPrimary || resource->isShared ||
             resource->needsPresentAllocation) &&
            !triton9SharedFormat(resource->hostFormat, NULL)) {
            hr = D3DDDIERR_NOTAVAILABLE;
            goto fail;
        }
        if (args->Flags.AutogenMipmap) {
            if (format->depthStencil || resource->isPrimary || resource->isShared ||
                resource->needsPresentAllocation) {
                hr = D3DDDIERR_NOTAVAILABLE;
                goto fail;
            }
            resource->mipLevels = triton9FullMipCount(resource->width,
                                                       resource->height);
        }
        hr = triton9CreateShadow(resource, &args->pSurfList[0]);
        if (FAILED(hr))
            goto fail;
        if ((resource->isPrimary || resource->isShared ||
             resource->needsPresentAllocation) && format->depthStencil) {
            hr = D3DDDIERR_NOTAVAILABLE;
            goto fail;
        }
    }
    /* Do this while the runtime is creating the primary.  It only asks the
     * KMD for the standard non-blob allocation; unlike the exported DWM
     * source, it neither starts Neptune nor re-enters the unpublished UMD
     * device. */
    if (resource->isPrimary && !resource->needsPresentAllocation) {
        hr = triton9AllocateStandardPrimary(device, resource);
        if (FAILED(hr))
            goto fail;
    }
    if (resource->needsPresentAllocation) {
        /* Fullscreen setup can query the allocation before the first draw
         * or Present, so delaying this until resource use leaves it absent. */
        EnterCriticalSection(&device->shaderLock);
        hr = triton9EnsureResourceHost(device, resource);
        LeaveCriticalSection(&device->shaderLock);
        if (FAILED(hr))
            goto fail;
    }
    args->hResource = (HANDLE)resource;
    triton9Diag("TRITON9-CREATERESOURCE success\n");
    return S_OK;

fail:
    triton9DiagU32("TRITON9-CREATERESOURCE-FAIL", (DWORD)hr);
    if (device->shaderLockInitialized) {
        EnterCriticalSection(&device->shaderLock);
        if (device->renderTarget == resource) {
            ID3D11DeviceContext1_OMSetRenderTargets(device->hostContext, 0,
                                                     NULL, NULL);
            device->renderTarget = NULL;
        }
        if (device->depthStencil == resource) {
            ID3D11DeviceContext1_OMSetRenderTargets(device->hostContext,
                device->renderTarget ? 1 : 0,
                device->renderTarget ? &device->renderTarget->renderTargetView : NULL,
                NULL);
            device->depthStencil = NULL;
        }
        LeaveCriticalSection(&device->shaderLock);
    }
    (void)triton9DeallocateResource(device, resource);
    if (resource->shaderResourceView)
        ID3D11ShaderResourceView_Release(resource->shaderResourceView);
    if (resource->depthStencilView)
        ID3D11DepthStencilView_Release(resource->depthStencilView);
    if (resource->renderTargetView)
        ID3D11RenderTargetView_Release(resource->renderTargetView);
    if (resource->stagingResource)
        ID3D11Resource_Release(resource->stagingResource);
    if (resource->fvfDeclaration)
        triton9DestroyFvfDeclaration(device, resource->fvfDeclaration);
    if (resource->hostResource)
        ID3D11Resource_Release(resource->hostResource);
    if (resource->ownsShadow && resource->shadow)
        HeapFree(GetProcessHeap(), 0, resource->shadow);
    HeapFree(GetProcessHeap(), 0, resource);
    return hr;
}

HRESULT APIENTRY
triton9CreateResource(HANDLE hDevice, D3DDDIARG_CREATERESOURCE *args)
{
    TRITON9_RESOURCE **surfaces;
    D3DDDIARG_CREATERESOURCE single;
    HRESULT hr;
    UINT i;

    if (!args || args->SurfCount <= 1)
        return triton9CreateSingleResource(hDevice, args, FALSE);
    /* Only equal-sized primary flip chains are multi-surface resources here.
     * Texture mip/array resources still require their own implementation. */
    if (!args->pSurfList || args->SurfCount > 4 || !args->Flags.Primary ||
        !args->Flags.RenderTarget || args->Flags.Texture || args->Flags.SharedResource ||
        args->Flags.ZBuffer || args->MipLevels > 1)
        return D3DDDIERR_NOTAVAILABLE;
    for (i = 1; i < args->SurfCount; ++i) {
        if (args->pSurfList[i].Width != args->pSurfList[0].Width ||
            args->pSurfList[i].Height != args->pSurfList[0].Height)
            return D3DDDIERR_NOTAVAILABLE;
    }
    surfaces = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                         args->SurfCount * sizeof(*surfaces));
    if (!surfaces)
        return E_OUTOFMEMORY;
    for (i = 0; i < args->SurfCount; ++i) {
        single = *args;
        single.SurfCount = 1;
        single.pSurfList = &args->pSurfList[i];
        /* Flip can scan out any entry, including the initial front buffer.
         * Preserve Primary on every independently exported allocation. */
        hr = triton9CreateSingleResource(hDevice, &single, TRUE);
        if (FAILED(hr)) {
            while (i)
                triton9DestroyResource(hDevice, (HANDLE)surfaces[--i]);
            HeapFree(GetProcessHeap(), 0, surfaces);
            return hr;
        }
        surfaces[i] = (TRITON9_RESOURCE *)single.hResource;
    }
    surfaces[0]->chainSurfaces = surfaces;
    surfaces[0]->surfaceCount = args->SurfCount;
    args->hResource = (HANDLE)surfaces[0];
    triton9DiagU32("TRITON9-PRIMARY-CHAIN-SURFACES", args->SurfCount);
    return S_OK;
}

HRESULT APIENTRY
triton9DestroyResource(HANDLE hDevice, HANDLE hResource)
{
    TRITON9_DEVICE *device = (TRITON9_DEVICE *)hDevice;
    TRITON9_RESOURCE *resource = (TRITON9_RESOURCE *)hResource;
    ID3D11Buffer *nullBuffer = NULL;
    ID3D11ShaderResourceView *nullShaderResource = NULL;
    UINT zero = 0;
    UINT stream;
    UINT texture;
    BOOL rebindOutputs = FALSE;
    HRESULT result = S_OK;

    if (!triton9ResourceBelongsToDevice(device, resource))
        return E_INVALIDARG;
    if (resource->chainSurfaces) {
        UINT i;
        for (i = 1; i < resource->surfaceCount; ++i) {
            if (!resource->chainSurfaces[i])
                continue;
            result = triton9DestroyResource(hDevice, (HANDLE)resource->chainSurfaces[i]);
            if (FAILED(result))
                return result;
            resource->chainSurfaces[i] = NULL;
        }
        HeapFree(GetProcessHeap(), 0, resource->chainSurfaces);
        resource->chainSurfaces = NULL;
        resource->surfaceCount = 1;
    }
    if (device->shaderLockInitialized) {
        EnterCriticalSection(&device->shaderLock);
        if (device->renderTarget == resource) {
            device->renderTarget = NULL;
            rebindOutputs = TRUE;
        }
        if (device->depthStencil == resource) {
            device->depthStencil = NULL;
            rebindOutputs = TRUE;
        }
        if (rebindOutputs) {
            ID3D11DeviceContext1_OMSetRenderTargets(
                device->hostContext, device->renderTarget ? 1 : 0,
                device->renderTarget ? &device->renderTarget->renderTargetView : NULL,
                device->depthStencil ? device->depthStencil->depthStencilView : NULL);
        }
        for (stream = 0; stream < TRITON9_MAX_VERTEX_STREAMS; ++stream) {
            if (device->streamResources[stream] == resource) {
                ID3D11DeviceContext1_IASetVertexBuffers(device->hostContext, stream, 1,
                                                         &nullBuffer, &zero, &zero);
                device->streamResources[stream] = NULL;
                device->streamOffsets[stream] = 0;
                device->streamStrides[stream] = 0;
            }
        }
        if (device->indexResource == resource) {
            ID3D11DeviceContext1_IASetIndexBuffer(device->hostContext, NULL,
                                                   DXGI_FORMAT_UNKNOWN, 0);
            device->indexResource = NULL;
            device->indexStride = 0;
        }
        for (texture = 0; texture < TRITON9_MAX_TEXTURE_STAGES; ++texture) {
            if (device->textures[texture] == resource) {
                ID3D11DeviceContext1_PSSetShaderResources(device->hostContext,
                                                           texture, 1,
                                                           &nullShaderResource);
                device->textures[texture] = NULL;
            }
        }
        LeaveCriticalSection(&device->shaderLock);
    }
    result = triton9DeallocateResource(device, resource);
    if (FAILED(result)) {
        /* dxgkrnl can still own the allocation.  Keep the imported wrapper,
         * host object, and CPU backing alive so no transport user observes a
         * freed allocation after a failed destruction callback. */
        return result;
    }
    if (resource->fvfDeclaration) {
        triton9DestroyFvfDeclaration(device, resource->fvfDeclaration);
        resource->fvfDeclaration = NULL;
    }
    if (resource->shaderResourceView) {
        ID3D11ShaderResourceView_Release(resource->shaderResourceView);
        resource->shaderResourceView = NULL;
    }
    if (resource->depthStencilView) {
        ID3D11DepthStencilView_Release(resource->depthStencilView);
        resource->depthStencilView = NULL;
    }
    if (resource->renderTargetView) {
        ID3D11RenderTargetView_Release(resource->renderTargetView);
        resource->renderTargetView = NULL;
    }
    if (resource->stagingResource) {
        ID3D11Resource_Release(resource->stagingResource);
        resource->stagingResource = NULL;
    }
    if (resource->hostResource) {
        ID3D11Resource_Release(resource->hostResource);
        resource->hostResource = NULL;
        resource->hostReady = FALSE;
    }
    if (resource->hImportAllocation || resource->hImportResource) {
        /* SHARED_OPEN_RES uses the imported allocation to attach the resource
         * to this transport context.  Retire every host COM release before the
         * KMD detaches that allocation.  On an uncertain drain, keep the import
         * handles and the UMD record so a retry cannot double-release objects. */
        if (!device->hostDevice ||
            !triton9DrainPrimaryTransport(device->hostDevice,
                                          TRITON9_HOST_DRAIN_TIMEOUT_MS)) {
            device->deviceLost = TRUE;
            return D3DDDIERR_DEVICEREMOVED;
        }
        if (!triton9ReleaseImportTransport(device->hostDevice,
                                           resource->hImportAllocation,
                                           resource->hImportResource)) {
            device->deviceLost = TRUE;
            return D3DDDIERR_DEVICEREMOVED;
        }
        resource->hImportAllocation = 0;
        resource->hImportResource = 0;
    }
    if (device->hostDevice &&
        !triton9TransportHealthy(device->hostContext
                                     ? (IUnknown *)device->hostContext
                                     : (IUnknown *)device->hostDevice)) {
        device->deviceLost = TRUE;
        return D3DDDIERR_DEVICEREMOVED;
    }
    if (resource->ownsShadow && resource->shadow)
        HeapFree(GetProcessHeap(), 0, resource->shadow);
    if (resource->pendingRename)
        HeapFree(GetProcessHeap(), 0, resource->pendingRename);
    HeapFree(GetProcessHeap(), 0, resource);
    return result;
}

HRESULT APIENTRY
triton9Lock(HANDLE hDevice, D3DDDIARG_LOCK *args)
{
    TRITON9_DEVICE *device = (TRITON9_DEVICE *)hDevice;
    TRITON9_RESOURCE *resource;
    SIZE_T offset = 0;
    BOOL systemMemory;
    BOOL needsReadback;
    UINT regionCount;
    HRESULT hr;

    if (!device || !args || !(resource = (TRITON9_RESOURCE *)args->hResource) ||
        !triton9ResourceBelongsToDevice(device, resource))
        return E_INVALIDARG;
    if (args->SubResourceIndex) {
        D3DDDIARG_LOCK selected = *args;
        TRITON9_RESOURCE *surface = triton9ResourceSurface(resource, args->SubResourceIndex);
        if (!surface)
            return D3DDDIERR_INVALIDCALL;
        selected.hResource = (HANDLE)surface;
        selected.SubResourceIndex = 0;
        hr = triton9Lock(hDevice, &selected);
        selected.hResource = args->hResource;
        selected.SubResourceIndex = args->SubResourceIndex;
        *args = selected;
        return hr;
    }

    if (device->deviceLost)
        return D3DDDIERR_DEVICEREMOVED;
    if (args->SubResourceIndex || resource->locked || resource->pendingRename ||
        !resource->shadow || resource->notLockable)
        return D3DDDIERR_INVALIDCALL;
    systemMemory = triton9ResourceIsSystemMemory(resource);
    if (args->Flags.Value & ~0x3ffu ||
        (args->Flags.ReadOnly && args->Flags.WriteOnly) ||
        (args->Flags.Discard && (args->Flags.ReadOnly || args->Flags.NoOverwrite)) ||
        (resource->writeOnly && args->Flags.ReadOnly) ||
        ((args->Flags.NotifyOnly != 0) != (systemMemory != 0)))
        return D3DDDIERR_INVALIDCALL;
    if (args->Flags.MightDrawFromLocked && !resource->canDrawWhileLocked)
        return D3DDDIERR_INVALIDCALL;
    regionCount = (args->Flags.RangeValid ? 1u : 0u) +
                  (args->Flags.AreaValid ? 1u : 0u) +
                  (args->Flags.BoxValid ? 1u : 0u);
    if (regionCount > 1 ||
        (resource->isBuffer && (args->Flags.AreaValid || args->Flags.BoxValid)) ||
        (!resource->isBuffer && args->Flags.RangeValid))
        return D3DDDIERR_INVALIDCALL;
    if (resource->isBuffer && args->Flags.RangeValid) {
        if (!args->Range.Size || args->Range.Offset > resource->shadowSize ||
            args->Range.Size > resource->shadowSize - args->Range.Offset)
            return D3DDDIERR_INVALIDCALL;
        offset = args->Range.Offset;
    } else if (!resource->isBuffer && args->Flags.AreaValid) {
        const RECT *area = &args->Area;
        if (area->left < 0 || area->top < 0 || area->right <= area->left ||
            area->bottom <= area->top || (UINT)area->right > resource->width ||
            (UINT)area->bottom > resource->height)
            return D3DDDIERR_INVALIDCALL;
        offset = (SIZE_T)area->top * resource->pitch +
                 (SIZE_T)area->left * resource->bytesPerPixel;
    } else if (args->Flags.BoxValid) {
        return D3DDDIERR_NOTAVAILABLE;
    }
    if (args->Flags.NoOverwrite &&
        (!resource->isBuffer || args->Flags.ReadOnly))
        return D3DDDIERR_INVALIDCALL;
    /* Vista DWM appends vertices to runtime-owned SYSTEMMEM buffers with
     * NotifyOnly | NoOverwrite, with either a subrange (0x94) or the whole
     * buffer (0x84). The CPU backing already aliases pSysMem; keep any
     * validated range for the upload at Unlock. An omitted range means the
     * whole buffer and does not invalidate the no-overwrite promise.
     * NoOverwrite does not require a driver-owned hardware allocation.
     * DWM also wraps this buffer with NotifyOnly | Discard | RangeValid
     * (0x98). Runtime-owned CPU storage remains authoritative: discard only
     * permits old contents to be ignored. The ordered upload at Unlock
     * supplies the written range without a driver-side CPU allocation rename. */
    /* Vista already owns the backing allocation of a NotifyOnly SYSTEMMEM
     * lock.  Do not create a hardware allocation merely to return a mapping
     * that the runtime explicitly ignores. */
    if (!systemMemory) {
        hr = triton9EnsureResourceHost(device, resource);
        if (FAILED(hr))
            return hr;
    }
    /* Readback is necessary only when the CPU can consume prior contents.
     * WriteOnly and NoOverwrite locks upload their exact region at unlock, so
     * stale shadow bytes outside that region never overwrite real host data. */
    needsReadback = !systemMemory && !args->Flags.Discard &&
                    !args->Flags.WriteOnly && !args->Flags.NoOverwrite;
    if (needsReadback && args->Flags.DoNotWait)
        return D3DDDIERR_WASSTILLDRAWING;
    if (needsReadback) {
        EnterCriticalSection(&device->shaderLock);
        hr = triton9ReadbackShadow(device, resource);
        LeaveCriticalSection(&device->shaderLock);
        if (FAILED(hr))
            return triton9MapDeviceFailure(device, hr);
    }
    resource->locked = TRUE;
    resource->lockRangeValid = args->Flags.RangeValid;
    resource->lockAreaValid = args->Flags.AreaValid;
    if (resource->lockRangeValid)
        resource->lockRange = args->Range;
    if (resource->lockAreaValid)
        resource->lockArea = args->Area;
    /* Unlock must observe runtime writes even for NotifyOnly.  Vista ignores
     * the pointer returned below and writes the original pSysMem allocation,
     * which is why SYSTEMMEM aliases that allocation instead of a private
     * shadow. */
    resource->uploadOnUnlock = !args->Flags.ReadOnly;
    args->pSurfData = resource->shadow + offset;
    args->Pitch = resource->pitch;
    args->SlicePitch = resource->slicePitch;
    return S_OK;
}

HRESULT APIENTRY
triton9Unlock(HANDLE hDevice, const D3DDDIARG_UNLOCK *args)
{
    TRITON9_DEVICE *device = (TRITON9_DEVICE *)hDevice;
    TRITON9_RESOURCE *resource;
    HRESULT hr = S_OK;

    if (!device || !args || !(resource = (TRITON9_RESOURCE *)args->hResource) ||
        !triton9ResourceBelongsToDevice(device, resource))
        return E_INVALIDARG;
    if (args->SubResourceIndex) {
        D3DDDIARG_UNLOCK selected = *args;
        TRITON9_RESOURCE *surface = triton9ResourceSurface(resource, args->SubResourceIndex);
        if (!surface)
            return D3DDDIERR_INVALIDCALL;
        selected.hResource = (HANDLE)surface;
        selected.SubResourceIndex = 0;
        return triton9Unlock(hDevice, &selected);
    }

    if ((args->Flags.Value & ~1u) || args->SubResourceIndex || !resource->locked ||
        ((args->Flags.NotifyOnly != 0) !=
         (triton9ResourceIsSystemMemory(resource) != 0)))
        return D3DDDIERR_INVALIDCALL;
    if (device->deviceLost) {
        resource->locked = FALSE;
        resource->uploadOnUnlock = FALSE;
        triton9ClearLockRegion(resource);
        return D3DDDIERR_DEVICEREMOVED;
    }
    /* A discard LockAsync writes the CPU shadow for a replacement resource.
     * Uploading it here would overwrite the old resource while earlier GPU
     * work can still reference it. Rename uploads the shadow after it creates
     * the replacement. */
    if (resource->uploadOnUnlock && !resource->pendingRename &&
        resource->hostResource) {
        EnterCriticalSection(&device->shaderLock);
        hr = triton9UploadLockedShadow(device, resource);
        if (SUCCEEDED(hr))
            hr = triton9CheckHostDevice(device);
        LeaveCriticalSection(&device->shaderLock);
    }
    resource->locked = FALSE;
    resource->uploadOnUnlock = FALSE;
    if (!resource->pendingRename)
        triton9ClearLockRegion(resource);
    return hr;
}

HRESULT APIENTRY
triton9LockAsync(HANDLE hDevice, D3DDDIARG_LOCKASYNC *args)
{
    TRITON9_DEVICE *device = (TRITON9_DEVICE *)hDevice;
    TRITON9_RESOURCE *resource;
    TRITON9_RENAME_COOKIE *cookie = NULL;
    D3DDDIARG_LOCK lock;
    HRESULT hr;

    if (!device || !args || !(resource = (TRITON9_RESOURCE *)args->hResource) ||
        !triton9ResourceBelongsToDevice(device, resource))
        return E_INVALIDARG;
    if (args->SubResourceIndex) {
        D3DDDIARG_LOCKASYNC selected = *args;
        TRITON9_RESOURCE *surface = triton9ResourceSurface(resource, args->SubResourceIndex);
        if (!surface)
            return D3DDDIERR_INVALIDCALL;
        selected.hResource = (HANDLE)surface;
        selected.SubResourceIndex = 0;
        hr = triton9LockAsync(hDevice, &selected);
        selected.hResource = args->hResource;
        selected.SubResourceIndex = args->SubResourceIndex;
        *args = selected;
        return hr;
    }

    if (device->deviceLost)
        return D3DDDIERR_DEVICEREMOVED;
    args->hCookie = NULL;
    if ((args->Flags.Value & ~0x7fu) ||
        (args->Flags.Discard && args->Flags.NoOverwrite) ||
        (!triton9ResourceIsSystemMemory(resource) &&
         !args->Flags.Discard && !args->Flags.NoOverwrite) ||
        (triton9ResourceIsSystemMemory(resource) &&
         (args->Flags.Discard || args->Flags.NoOverwrite)))
        return D3DDDIERR_INVALIDCALL;
    if (args->Flags.Discard) {
        if (args->Flags.NotifyOnly || resource->isPrimary || resource->isShared ||
            resource->needsPresentAllocation ||
            resource->mipLevels != 1 ||
            !resource->shadow || resource->pendingRename)
            return D3DDDIERR_NOTAVAILABLE;
        cookie = (TRITON9_RENAME_COOKIE *)HeapAlloc(GetProcessHeap(),
                                                     HEAP_ZERO_MEMORY,
                                                     sizeof(*cookie));
        if (!cookie)
            return E_OUTOFMEMORY;
        cookie->resource = resource;
    }
    ZeroMemory(&lock, sizeof(lock));
    lock.hResource = args->hResource;
    lock.SubResourceIndex = args->SubResourceIndex;
    lock.Range = args->Range;
    lock.Flags.NoOverwrite = args->Flags.NoOverwrite;
    lock.Flags.Discard = args->Flags.Discard;
    lock.Flags.RangeValid = args->Flags.RangeValid;
    lock.Flags.AreaValid = args->Flags.AreaValid;
    lock.Flags.BoxValid = args->Flags.BoxValid;
    lock.Flags.NotifyOnly = args->Flags.NotifyOnly;
    /* LockAsync is a write-only D3D9 contract.  This also prevents the common
     * Lock path from synchronizing and blocking an asynchronous lock. */
    lock.Flags.WriteOnly = TRUE;
    hr = triton9Lock(hDevice, &lock);
    if (FAILED(hr)) {
        if (cookie)
            HeapFree(GetProcessHeap(), 0, cookie);
        return hr;
    }
    args->pSurfData = lock.pSurfData;
    args->Pitch = lock.Pitch;
    args->SlicePitch = lock.SlicePitch;
    if (cookie) {
        resource->pendingRename = cookie;
        args->hCookie = (HANDLE)cookie;
    }
    return S_OK;
}

HRESULT APIENTRY
triton9UnlockAsync(HANDLE hDevice, const D3DDDIARG_UNLOCKASYNC *args)
{
    TRITON9_RESOURCE *resource;
    D3DDDIARG_UNLOCK unlock;
    HRESULT hr;

    if (!hDevice || !args ||
        !(resource = (TRITON9_RESOURCE *)args->hResource) ||
        !triton9ResourceBelongsToDevice((TRITON9_DEVICE *)hDevice, resource))
        return E_INVALIDARG;
    if (args->SubResourceIndex) {
        D3DDDIARG_UNLOCKASYNC selected = *args;
        TRITON9_RESOURCE *surface = triton9ResourceSurface(resource, args->SubResourceIndex);
        if (!surface)
            return D3DDDIERR_INVALIDCALL;
        selected.hResource = (HANDLE)surface;
        selected.SubResourceIndex = 0;
        return triton9UnlockAsync(hDevice, &selected);
    }

    if (args->Flags.Value & ~1u)
        return D3DDDIERR_INVALIDCALL;
    ZeroMemory(&unlock, sizeof(unlock));
    unlock.hResource = args->hResource;
    unlock.SubResourceIndex = args->SubResourceIndex;
    unlock.Flags.NotifyOnly = args->Flags.NotifyOnly;
    hr = triton9Unlock(hDevice, &unlock);
    if (FAILED(hr) && resource->pendingRename) {
        HeapFree(GetProcessHeap(), 0, resource->pendingRename);
        resource->pendingRename = NULL;
        triton9ClearLockRegion(resource);
    }
    return hr;
}

HRESULT APIENTRY
triton9Rename(HANDLE hDevice, const D3DDDIARG_RENAME *args)
{
    TRITON9_DEVICE *device = (TRITON9_DEVICE *)hDevice;
    TRITON9_RESOURCE *resource;
    TRITON9_RENAME_COOKIE *cookie;
    TRITON9_RESOURCE replacement;
    TRITON9_RESOURCE retired;
    ID3D11Resource *newHost = NULL;
    ID3D11Buffer *vertexBuffers[TRITON9_MAX_VERTEX_STREAMS] = { NULL };
    ID3D11Buffer *indexBuffer = NULL;
    HRESULT hr;
    UINT stream;
    UINT texture;

    if (!device || !args || !(resource = (TRITON9_RESOURCE *)args->hResource) ||
        !triton9ResourceBelongsToDevice(device, resource))
        return E_INVALIDARG;
    if (args->SubResourceIndex) {
        D3DDDIARG_RENAME selected = *args;
        TRITON9_RESOURCE *surface = triton9ResourceSurface(resource, args->SubResourceIndex);
        if (!surface)
            return D3DDDIERR_INVALIDCALL;
        selected.hResource = (HANDLE)surface;
        selected.SubResourceIndex = 0;
        return triton9Rename(hDevice, &selected);
    }

    if (device->deviceLost)
        return D3DDDIERR_DEVICEREMOVED;
    cookie = (TRITON9_RENAME_COOKIE *)args->hCookie;
    if (args->SubResourceIndex || !cookie || cookie != resource->pendingRename ||
        cookie->resource != resource || resource->locked)
        return D3DDDIERR_INVALIDCALL;

    ZeroMemory(&replacement, sizeof(replacement));
    EnterCriticalSection(&device->shaderLock);
    hr = triton9CloneHostResource(device, resource, &newHost);
    if (FAILED(hr))
        goto done;
    replacement = *resource;
    replacement.hostResource = newHost;
    replacement.stagingResource = NULL;
    replacement.renderTargetView = NULL;
    replacement.depthStencilView = NULL;
    replacement.shaderResourceView = NULL;
    hr = triton9UploadLockedShadow(device, &replacement);
    if (FAILED(hr))
        goto done;

    if (device->renderTarget == resource) {
        ID3D11RenderTargetView *view;
        hr = triton9GetRenderTargetView(device, &replacement, &view);
        if (FAILED(hr))
            goto done;
    }
    if (device->depthStencil == resource) {
        ID3D11DepthStencilView *view;
        hr = triton9GetDepthStencilView(device, &replacement, &view);
        if (FAILED(hr))
            goto done;
    }
    for (texture = 0; texture < TRITON9_MAX_TEXTURE_STAGES; ++texture) {
        if (device->textures[texture] == resource) {
            ID3D11ShaderResourceView *view;
            hr = triton9GetShaderResourceView(device, &replacement, &view);
            if (FAILED(hr))
                goto done;
        }
    }
    for (stream = 0; stream < TRITON9_MAX_VERTEX_STREAMS; ++stream) {
        if (device->streamResources[stream] == resource) {
            hr = triton9GetHostBuffer(&replacement, D3D11_BIND_VERTEX_BUFFER,
                                      &vertexBuffers[stream]);
            if (FAILED(hr))
                goto done;
        }
    }
    if (device->indexResource == resource) {
        hr = triton9GetHostBuffer(&replacement, D3D11_BIND_INDEX_BUFFER,
                                  &indexBuffer);
        if (FAILED(hr))
            goto done;
    }

    retired = *resource;
    resource->hostResource = replacement.hostResource;
    resource->stagingResource = replacement.stagingResource;
    resource->renderTargetView = replacement.renderTargetView;
    resource->depthStencilView = replacement.depthStencilView;
    resource->shaderResourceView = replacement.shaderResourceView;
    replacement.hostResource = NULL;
    replacement.stagingResource = NULL;
    replacement.renderTargetView = NULL;
    replacement.depthStencilView = NULL;
    replacement.shaderResourceView = NULL;

    if (device->renderTarget == resource || device->depthStencil == resource) {
        ID3D11RenderTargetView *renderTarget = device->renderTarget
            ? device->renderTarget->renderTargetView : NULL;
        ID3D11DepthStencilView *depthStencil = device->depthStencil
            ? device->depthStencil->depthStencilView : NULL;
        ID3D11DeviceContext1_OMSetRenderTargets(device->hostContext,
                                                 renderTarget ? 1 : 0,
                                                 renderTarget ? &renderTarget : NULL,
                                                 depthStencil);
    }
    for (stream = 0; stream < TRITON9_MAX_VERTEX_STREAMS; ++stream) {
        if (vertexBuffers[stream]) {
            UINT stride = device->streamStrides[stream];
            UINT offset = device->streamOffsets[stream];
            ID3D11DeviceContext1_IASetVertexBuffers(device->hostContext, stream, 1,
                                                     &vertexBuffers[stream],
                                                     &stride, &offset);
        }
    }
    if (indexBuffer) {
        DXGI_FORMAT format = resource->format == D3DDDIFMT_INDEX16
            ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
        ID3D11DeviceContext1_IASetIndexBuffer(device->hostContext, indexBuffer,
                                               format, 0);
    }
    for (texture = 0; texture < TRITON9_MAX_TEXTURE_STAGES; ++texture) {
        if (device->textures[texture] == resource) {
            ID3D11DeviceContext1_PSSetShaderResources(device->hostContext, texture, 1,
                                                       &resource->shaderResourceView);
        }
    }
    triton9ReleaseResourceViews(&retired);
    if (retired.hostResource)
        ID3D11Resource_Release(retired.hostResource);
    resource->pendingRename = NULL;
    triton9ClearLockRegion(resource);
    cookie->resource = NULL;
    HeapFree(GetProcessHeap(), 0, cookie);
    hr = S_OK;

done:
    for (stream = 0; stream < TRITON9_MAX_VERTEX_STREAMS; ++stream) {
        if (vertexBuffers[stream])
            ID3D11Buffer_Release(vertexBuffers[stream]);
    }
    if (indexBuffer)
        ID3D11Buffer_Release(indexBuffer);
    if (FAILED(hr)) {
        triton9ReleaseResourceViews(&replacement);
        if (newHost)
            ID3D11Resource_Release(newHost);
        resource->pendingRename = NULL;
        triton9ClearLockRegion(resource);
        cookie->resource = NULL;
        HeapFree(GetProcessHeap(), 0, cookie);
    }
    if (SUCCEEDED(hr))
        hr = triton9CheckHostDevice(device);
    LeaveCriticalSection(&device->shaderLock);
    return triton9MapDeviceFailure(device, hr);
}

HRESULT APIENTRY
triton9SetDisplayMode(HANDLE hDevice, const D3DDDIARG_SETDISPLAYMODE *args)
{
    TRITON9_DEVICE *device = (TRITON9_DEVICE *)hDevice;
    TRITON9_RESOURCE *resource;
    D3DDDICB_SETDISPLAYMODE callback;

    if (!device || !args || !(resource = (TRITON9_RESOURCE *)args->hResource) ||
        !triton9ResourceBelongsToDevice(device, resource))
        return E_INVALIDARG;
    resource = triton9ResourceSurface(resource, args->SubResourceIndex);
    if (!resource)
        return D3DDDIERR_INVALIDCALL;
    if (device->deviceLost)
        return D3DDDIERR_DEVICEREMOVED;
    if (!resource->isPrimary || !resource->hKMAllocation ||
        !device->callbacks.pfnSetDisplayModeCb)
        return D3DDDIERR_INVALIDCALL;
    ZeroMemory(&callback, sizeof(callback));
    callback.hPrimaryAllocation = resource->hKMAllocation;
    return triton9MapDeviceFailure(device,
        device->callbacks.pfnSetDisplayModeCb(device->hRTDevice, &callback));
}

/* Caller holds shaderLock across rendering completion, Present and the
 * consumption marker. Each frame has a unique, increasing GPU fence value;
 * a cached value from an earlier frame cannot satisfy this frame's wait. */
static HRESULT
triton9WaitForPresentGpuCompletion(TRITON9_DEVICE *device)
{
    ID3D11Device5 *device5 = NULL;
    ID3D11DeviceContext4 *context4 = NULL;
    ID3D11Fence *fence = NULL;
    UINT64 target, completed;
    DWORD start;
    HRESULT hr;

    if (!device || !device->hostDevice || !device->hostContext)
        return E_INVALIDARG;
    if (!device->presentFence) {
        hr = ID3D11Device1_QueryInterface(device->hostDevice,
                                          &IID_ID3D11Device5, (void **)&device5);
        if (FAILED(hr) || !device5)
            return FAILED(hr) ? triton9MapDeviceFailure(device, hr) : E_FAIL;
        hr = ID3D11DeviceContext1_QueryInterface(device->hostContext,
                    &IID_ID3D11DeviceContext4, (void **)&context4);
        if (SUCCEEDED(hr) && context4)
            hr = ID3D11Device5_CreateFence(device5, 0, D3D11_FENCE_FLAG_NONE,
                                            &IID_ID3D11Fence, (void **)&fence);
        ID3D11Device5_Release(device5);
        if (FAILED(hr) || !context4 || !fence) {
            if (fence)
                ID3D11Fence_Release(fence);
            if (context4)
                ID3D11DeviceContext4_Release(context4);
            return FAILED(hr) ? triton9MapDeviceFailure(device, hr) : E_FAIL;
        }
        device->presentFence = fence;
        device->presentContext = context4;
        device->presentFenceValue = 0;
    }
    /* UINT64_MAX is the D3D11 device-removal sentinel, never a frame ID. */
    if (device->presentFenceValue >= UINT64_MAX - 1) {
        device->deviceLost = TRUE;
        return D3DDDIERR_DEVICEREMOVED;
    }
    target = ++device->presentFenceValue;
    hr = ID3D11DeviceContext4_Signal(device->presentContext,
                                     device->presentFence, target);
    if (FAILED(hr))
        return triton9MapDeviceFailure(device, hr);
    ID3D11DeviceContext1_Flush(device->hostContext);
    start = GetTickCount();
    for (;;) {
        completed = ID3D11Fence_GetCompletedValue(device->presentFence);
        if (completed == UINT64_MAX) {
            device->deviceLost = TRUE;
            return D3DDDIERR_DEVICEREMOVED;
        }
        if (completed >= target) {
            hr = triton9CheckHostDevice(device);
            if (SUCCEEDED(hr) && target == 1)
                triton9Diag("TRITON9-PRESENT-TIMELINE-COMPLETE\n");
            return triton9MapDeviceFailure(device, hr);
        }
        if ((DWORD)(GetTickCount() - start) >= TRITON9_HOST_DRAIN_TIMEOUT_MS) {
            device->deviceLost = TRUE;
            return D3DDDIERR_DEVICEREMOVED;
        }
        Sleep(1);
    }
}

/* Caller holds shaderLock. This marker uses the same scheduler context as
 * Present, so its signal follows the KMD's synchronous source copy. */
static HRESULT
triton9WaitForPresentConsumption(TRITON9_DEVICE *device)
{
    D3DDDICB_RENDER render;
    VIOGPU_COMMAND_HDR header;
    VIOGPU_SIGNAL_EVENT_CMD signal;
    HANDLE event;
    DWORD wait;
    HRESULT hr;
    const UINT packetSize = sizeof(header) + sizeof(signal);

    event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!event) {
        device->deviceLost = TRUE;
        return E_FAIL;
    }
    EnterCriticalSection(&device->kmContextLock);
    if (!device->hKMContext || !device->callbacks.pfnRenderCb ||
        !device->kmCommandBuffer || device->kmCommandBufferSize < packetSize) {
        hr = E_FAIL;
        goto fail;
    }
    ZeroMemory(&header, sizeof(header));
    header.type = VIOGPU_CMD_SIGNAL_EVENT;
    header.size = sizeof(signal);
    signal.Event = VioGpuUmHandle(event);
    CopyMemory(device->kmCommandBuffer, &header, sizeof(header));
    CopyMemory((BYTE *)device->kmCommandBuffer + sizeof(header),
               &signal, sizeof(signal));
    ZeroMemory(&render, sizeof(render));
    render.CommandLength = packetSize;
    render.hContext = device->hKMContext;
    hr = device->callbacks.pfnRenderCb(device->hRTDevice, &render);
    if (FAILED(hr))
        goto fail;
    if (!render.pNewCommandBuffer || render.NewCommandBufferSize < packetSize ||
        !render.pNewAllocationList || !render.NewAllocationListSize ||
        !render.pNewPatchLocationList || !render.NewPatchLocationListSize) {
        hr = E_FAIL;
        goto fail;
    }
    device->kmCommandBuffer = render.pNewCommandBuffer;
    device->kmCommandBufferSize = render.NewCommandBufferSize;
    device->kmAllocationList = render.pNewAllocationList;
    device->kmAllocationListSize = render.NewAllocationListSize;
    device->kmPatchLocationList = render.pNewPatchLocationList;
    device->kmPatchLocationListSize = render.NewPatchLocationListSize;
    LeaveCriticalSection(&device->kmContextLock);
    wait = WaitForSingleObject(event, TRITON9_HOST_DRAIN_TIMEOUT_MS);
    CloseHandle(event);
    if (wait != WAIT_OBJECT_0) {
        device->deviceLost = TRUE;
        return D3DDDIERR_DEVICEREMOVED;
    }
    if (!device->presentConsumptionReported) {
        device->presentConsumptionReported = TRUE;
        triton9Diag("TRITON9-PRESENT-CONSUMED\n");
    }
    return S_OK;

fail:
    device->kmCommandBuffer = NULL;
    device->kmCommandBufferSize = 0;
    device->kmAllocationList = NULL;
    device->kmAllocationListSize = 0;
    device->kmPatchLocationList = NULL;
    device->kmPatchLocationListSize = 0;
    device->deviceLost = TRUE;
    LeaveCriticalSection(&device->kmContextLock);
    CloseHandle(event);
    return triton9MapDeviceFailure(device, hr);
}

HRESULT APIENTRY
triton9Present(HANDLE hDevice, const D3DDDIARG_PRESENT *args)
{
    TRITON9_DEVICE *device = (TRITON9_DEVICE *)hDevice;
    TRITON9_RESOURCE *src;
    TRITON9_RESOURCE *dst;
    D3DDDICB_PRESENT callback;
    HRESULT hr;

    triton9Diag("TRITON9-PRESENT enter\n");
    if (!device || !args || !(src = (TRITON9_RESOURCE *)args->hSrcResource) ||
        !triton9ResourceBelongsToDevice(device, src))
        return E_INVALIDARG;
    dst = (TRITON9_RESOURCE *)args->hDstResource;
    if (dst && !triton9ResourceBelongsToDevice(device, dst))
        return D3DDDIERR_INVALIDCALL;
    src = triton9ResourceSurface(src, args->SrcSubResourceIndex);
    dst = triton9ResourceSurface(dst, args->DstSubResourceIndex);
    if (!src || (args->hDstResource && !dst) ||
        (!args->hDstResource && args->DstSubResourceIndex))
        return D3DDDIERR_INVALIDCALL;
    if (device->deviceLost)
        return D3DDDIERR_DEVICEREMOVED;
    hr = triton9EnsureHostDevice(device);
    if (FAILED(hr))
        return hr;
    /* CreateResource intentionally defers the host-backed DWM source until
     * the device is published.  Present is the first callback that requires
     * its KMD allocation, so materialize it before testing the handle.  The
     * destination standard primary already has its KMD allocation and must
     * remain a non-blob object. */
    hr = triton9EnsureResourceHost(device, src);
    if (FAILED(hr))
        return hr;
    if (dst && !dst->hKMAllocation) {
        hr = triton9EnsureResourceHost(device, dst);
        if (FAILED(hr))
            return hr;
    }
    if (!src->hKMAllocation || (dst && !dst->hKMAllocation) ||
        !device->callbacks.pfnPresentCb || !device->hostContext ||
        !device->hostDevice) {
        DWORD rejected = 0;

        rejected |= !src->hKMAllocation ? 0x04u : 0u;
        rejected |= dst && !dst->hKMAllocation ? 0x08u : 0u;
        rejected |= !device->callbacks.pfnPresentCb ? 0x10u : 0u;
        rejected |= !device->hostContext ? 0x20u : 0u;
        rejected |= !device->hostDevice ? 0x40u : 0u;
        triton9DiagU32("TRITON9-PRESENT-REJECT", rejected);
        return D3DDDIERR_INVALIDCALL;
    }
    EnterCriticalSection(&device->shaderLock);
    hr = triton9WaitForPresentGpuCompletion(device);
    if (FAILED(hr)) {
        LeaveCriticalSection(&device->shaderLock);
        return hr;
    }
    /* Keep this post-completion drain for the existing KMD/transport ordering
     * contract.  It does not provide the GPU completion guarantee above. */
    if (!tritonSharedBridgeDrain(device->hostContext,
                                 TRITON9_HOST_DRAIN_TIMEOUT_MS)) {
        device->deviceLost = TRUE;
        LeaveCriticalSection(&device->shaderLock);
        return D3DDDIERR_DEVICEREMOVED;
    }
    hr = triton9CheckHostDevice(device);
    if (FAILED(hr)) {
        LeaveCriticalSection(&device->shaderLock);
        return hr;
    }
    hr = triton9EnsureKernelContext(device);
    if (FAILED(hr)) {
        LeaveCriticalSection(&device->shaderLock);
        return hr;
    }
    ZeroMemory(&callback, sizeof(callback));
    callback.hSrcAllocation = src->hKMAllocation;
    callback.hDstAllocation = dst ? dst->hKMAllocation : 0;
    callback.hContext = device->hKMContext;
    hr = triton9MapDeviceFailure(device,
        device->callbacks.pfnPresentCb(device->hRTDevice, &callback));
    if (SUCCEEDED(hr))
        hr = triton9WaitForPresentConsumption(device);
    LeaveCriticalSection(&device->shaderLock);
    triton9Diag(SUCCEEDED(hr) ? "TRITON9-PRESENT success\n" :
                                "TRITON9-PRESENT fail\n");
    return hr;
}

HRESULT APIENTRY
triton9OpenResource(HANDLE hDevice, D3DDDIARG_OPENRESOURCE *args)
{
    TRITON9_DEVICE *device = (TRITON9_DEVICE *)hDevice;
    TRITON9_RESOURCE *resource;
    const VIOGPU_CREATE_ALLOCATION_EXCHANGE *allocation;
    const VIOGPU_RESOURCE_SHARED_TEXTURE_OPTIONS *options;
    VIOGPU_RESOURCE_SHARED_TEXTURE_OPTIONS synth3d;
    struct triton_shared_texture_desc desc;
    VIOGPU_ESCAPE escape;
    void *opened;
    const TRITON9_FORMAT *format;
    ULONG scanoutFormat;
    uint64_t roundedSize;
    HRESULT hr;
    UINT i;

    triton9Diag("TRITON9-OPENRESOURCE enter\n");
    if (!device || !args || (args->Flags.Value & ~3u) ||
        args->NumAllocations != 1 ||
        !args->pOpenAllocationInfo || !args->pOpenAllocationInfo[0].pPrivateDriverData)
        return E_INVALIDARG;
    if (device->deviceLost)
        return D3DDDIERR_DEVICEREMOVED;
    allocation = (const VIOGPU_CREATE_ALLOCATION_EXCHANGE *)
        args->pOpenAllocationInfo[0].pPrivateDriverData;
    if (args->pOpenAllocationInfo[0].PrivateDriverDataSize < sizeof(*allocation) ||
        (allocation->Type != VIOGPU_RESOURCE_TYPE_SHARED &&
         allocation->Type != VIOGPU_RESOURCE_TYPE_3D))
        return D3DDDIERR_NOTAVAILABLE;

    /* The KMD-created standard primary is an ordinary RESOURCE_CREATE_3D
     * allocation, marked with the private flag shared by the two driver
     * halves.  It is the *destination* of a DWM Present.  Do not try to
     * import it through the dmabuf bridge: it has no blob_id, and turning it
     * into a host blob would make the KMD's non-blob-primary scanout path
     * unreachable. */
    if (allocation->Type == VIOGPU_RESOURCE_TYPE_3D &&
        (allocation->Options3D.flags & VIOGPU_RESOURCE_FLAG_STANDARD_PRIMARY)) {
        const VIOGPU_RESOURCE_3D_OPTIONS *primary = &allocation->Options3D;
        ULONG dxgiFormat;
        uint64_t primarySize;

        switch (primary->format) {
        case 1: /* VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM */
            dxgiFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
            break;
        case 2: /* VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM */
            dxgiFormat = DXGI_FORMAT_B8G8R8X8_UNORM;
            break;
        default:
            return D3DDDIERR_NOTAVAILABLE;
        }
        if (!args->pOpenAllocationInfo[0].hAllocation ||
            primary->target != 2 || primary->width == 0 ||
            primary->height == 0 || primary->width > 4096 ||
            primary->height > 4096 || primary->depth != 1 ||
            primary->array_size != 1 || primary->last_level != 0 ||
            primary->nr_samples != 0 ||
            primary->flags != VIOGPU_RESOURCE_FLAG_STANDARD_PRIMARY ||
            primary->bind != ((1u << 1) | (1u << 3) | (1u << 7) |
                              (1u << 18)))
            return D3DDDIERR_INVALIDCALL;
        primarySize = (uint64_t)primary->width * 4ull * primary->height;
        if (!primarySize || primarySize != allocation->Size ||
            primarySize > (uint64_t)((SIZE_T)-1))
            return D3DDDIERR_INVALIDCALL;
        format = triton9FormatLookupHost((DXGI_FORMAT)dxgiFormat);
        if (!format || format->depthStencil)
            return D3DDDIERR_NOTAVAILABLE;
        resource = (TRITON9_RESOURCE *)HeapAlloc(GetProcessHeap(),
                                                  HEAP_ZERO_MEMORY,
                                                  sizeof(*resource));
        if (!resource)
            return E_OUTOFMEMORY;
        resource->hOwnerDevice = (HANDLE)(uintptr_t)device;
        resource->hRTResource = args->hResource;
        resource->hKMAllocation = args->pOpenAllocationInfo[0].hAllocation;
        resource->format = format->d3dFormat;
        resource->hostFormat = format->hostFormat;
        resource->width = primary->width;
        resource->height = primary->height;
        resource->depth = 1;
        resource->mipLevels = 1;
        resource->surfaceCount = 1;
        resource->pool = D3DDDIPOOL_VIDEOMEMORY;
        resource->bytesPerPixel = format->bytesPerPixel;
        resource->rowBytes = primary->width * format->bytesPerPixel;
        resource->pitch = resource->rowBytes;
        resource->slicePitch = (UINT)primarySize;
        resource->isPrimary = TRUE;
        resource->isStandardPrimary = TRUE;
        resource->notLockable = TRUE;
        args->hResource = (HANDLE)resource;
        triton9Diag("TRITON9-OPEN-STANDARD-PRIMARY success\n");
        return S_OK;
    }

    /* Non-primary 3D allocations are staging/shadow resources.  They remain
     * bridge-imported for the legacy cross-process resource path; the branch
     * above is deliberately separate because a standard primary is not a
     * dmabuf-backed render texture. */
    if (allocation->Type == VIOGPU_RESOURCE_TYPE_3D) {
        const VIOGPU_RESOURCE_3D_OPTIONS *options3d = &allocation->Options3D;
        ULONG dxgiFormat;
        ULONGLONG stride;
        switch (options3d->format) {
        case 1: /* VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM */
            dxgiFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
            break;
        case 2: /* VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM */
            dxgiFormat = DXGI_FORMAT_B8G8R8X8_UNORM;
            break;
        default:
            return D3DDDIERR_NOTAVAILABLE;
        }
        if (!options3d->width || !options3d->height ||
            options3d->width > 4096 || options3d->height > 4096 ||
            options3d->target != 2 || options3d->depth != 1 ||
            options3d->array_size != 1 || options3d->last_level != 0 ||
            options3d->nr_samples != 0 ||
            options3d->bind != ((1u << 1) | (1u << 3) | (1u << 7) |
                               (1u << 18)) ||
            options3d->flags != (1u << 2)) /* MAP_COHERENT */
            return D3DDDIERR_INVALIDCALL;
        /* Standard Vista 3D/shadow allocations are backed by the KMD's
         * linear WDDM allocation, whose pitch is width*4 and whose Size is
         * exactly pitch*height.  Do not invent a 256-byte host pitch here:
         * at 800x600 it makes the synthesized descriptor larger than the
         * allocation and causes every DWM OpenResource to fail. */
        stride = (ULONGLONG)options3d->width * 4ull;
        if (allocation->Size != stride * options3d->height)
            return D3DDDIERR_INVALIDCALL;
        ZeroMemory(&synth3d, sizeof(synth3d));
        synth3d.width = options3d->width;
        synth3d.height = options3d->height;
        synth3d.mip_levels = 1;
        synth3d.array_size = 1;
        synth3d.format = dxgiFormat;
        synth3d.sample_count = 1;
        synth3d.usage = D3D11_USAGE_DEFAULT;
        synth3d.bind_flags = D3D11_BIND_SHADER_RESOURCE |
                             D3D11_BIND_RENDER_TARGET;
        synth3d.plane_count = 1;
        synth3d.allocation_size = allocation->Size;
        synth3d.planes[0].pitch = stride;
        options = &synth3d;
    } else {
        options = &allocation->OptionsShared;
    }
    format = triton9FormatLookupHost((DXGI_FORMAT)options->format);
    if (!args->pOpenAllocationInfo[0].hAllocation ||
        (allocation->Type == VIOGPU_RESOURCE_TYPE_SHARED && !options->blob_id) ||
        !options->width || !options->height || options->width > 4096 ||
        options->height > 4096 || options->plane_count != 1 || !format ||
        format->depthStencil ||
        !triton9SharedFormat((DXGI_FORMAT)options->format, &scanoutFormat) ||
        !options->allocation_size ||
        options->primary > 1 ||
        options->allocation_size > allocation->Size ||
        options->allocation_size > (uint64_t)((SIZE_T)-1) - 4095ull ||
        options->mip_levels != 1 || options->array_size != 1 ||
        options->sample_count != 1 || options->usage != D3D11_USAGE_DEFAULT ||
        options->bind_flags != (D3D11_BIND_SHADER_RESOURCE |
                                D3D11_BIND_RENDER_TARGET) ||
        options->cpu_access_flags ||
        (allocation->Type == VIOGPU_RESOURCE_TYPE_SHARED &&
         options->misc_flags != D3D11_RESOURCE_MISC_SHARED) ||
        options->texture_layout > 2)
        return D3DDDIERR_INVALIDCALL;
    roundedSize = (options->allocation_size + 4095ull) & ~4095ull;
    if (allocation->Type == VIOGPU_RESOURCE_TYPE_SHARED) {
        if (!roundedSize || allocation->Size != roundedSize ||
            options->ScanoutInfo.width != options->width ||
            options->ScanoutInfo.height != options->height ||
            options->ScanoutInfo.format != scanoutFormat ||
            options->ScanoutInfo.strides[0] != (ULONG)options->planes[0].pitch ||
            options->ScanoutInfo.offsets[0] != (ULONG)options->planes[0].offset)
            return D3DDDIERR_INVALIDCALL;
    }
    for (i = 1; i < 4; ++i) {
        if (options->planes[i].offset || options->planes[i].pitch ||
            options->ScanoutInfo.strides[i] || options->ScanoutInfo.offsets[i])
            return D3DDDIERR_INVALIDCALL;
    }
    if (!triton9ValidateSharedPlane(options->planes[0].offset,
                                    options->planes[0].pitch,
                                    options->allocation_size,
                                    options->width, options->height,
                                    format->bytesPerPixel))
        return D3DDDIERR_INVALIDCALL;

    resource = (TRITON9_RESOURCE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                              sizeof(*resource));
    if (!resource)
        return E_OUTOFMEMORY;
    resource->hOwnerDevice = (HANDLE)(uintptr_t)device;
    resource->hRTResource = args->hResource;
    resource->hKMAllocation = args->pOpenAllocationInfo[0].hAllocation;
    resource->format = format->d3dFormat;
    resource->hostFormat = format->hostFormat;
    resource->width = options->width;
    resource->height = options->height;
    resource->depth = 1;
    resource->mipLevels = options->mip_levels ? options->mip_levels : 1;
    resource->surfaceCount = 1;
    resource->pool = D3DDDIPOOL_VIDEOMEMORY;
    resource->bytesPerPixel = format->bytesPerPixel;
    resource->rowBytes = resource->width * resource->bytesPerPixel;
    resource->pitch = (UINT)options->planes[0].pitch;
    if ((uint64_t)resource->pitch * resource->height > UINT_MAX) {
        hr = D3DDDIERR_NOTAVAILABLE;
        goto fail;
    }
    resource->slicePitch = resource->pitch * resource->height;
    resource->shadowSize = resource->slicePitch;
    resource->isPrimary = options->primary != 0;
    resource->isShared = TRUE;
    /* An opened allocation has no UMD-owned or runtime-owned CPU backing.
     * Locking it would return a fabricated pointer, so keep it host-only. */
    resource->notLockable = TRUE;
    resource->hostBindFlags = options->bind_flags;
    hr = triton9EnsureRuntimeContext(device);
    if (FAILED(hr))
        goto fail;
    ZeroMemory(&escape, sizeof(escape));
    escape.Type = VIOGPU_RES_INFO;
    escape.DataLength = sizeof(escape.ResourceInfo);
    escape.ResourceInfo.ResHandle = resource->hKMAllocation;
    hr = triton9Escape(device, &escape);
    if (FAILED(hr) || !escape.ResourceInfo.IsCreated || !escape.ResourceInfo.Id) {
        hr = FAILED(hr) ? hr : E_FAIL;
        goto fail;
    }
    if (!tritonSharedBridgeImportRes(device->hostDevice, escape.ResourceInfo.Id,
                                     options->allocation_size,
                                     &resource->hImportAllocation,
                                     &resource->hImportResource)) {
        hr = E_FAIL;
        goto fail;
    }
    ZeroMemory(&desc, sizeof(desc));
    desc.blob_id = options->blob_id;
    desc.create_ctx_id = options->create_ctx_id;
    desc.plane_count = options->plane_count;
    desc.texture_layout = options->texture_layout;
    desc.modifier = options->modifier;
    desc.allocation_size = options->allocation_size;
    for (i = 0; i < options->plane_count; ++i) {
        desc.planes[i].offset = options->planes[i].offset;
        desc.planes[i].pitch = options->planes[i].pitch;
    }
    desc.width = options->width;
    desc.height = options->height;
    desc.mip_levels = resource->mipLevels;
    desc.array_size = options->array_size ? options->array_size : 1;
    desc.format = options->format;
    desc.sample_count = options->sample_count ? options->sample_count : 1;
    desc.usage = options->usage;
    desc.bind_flags = options->bind_flags;
    desc.cpu_access_flags = options->cpu_access_flags;
    desc.misc_flags = options->misc_flags;
    opened = tritonSharedBridgeOpenRes(device->hostDevice, escape.ResourceInfo.Id,
                                       &desc);
    if (!opened) {
        hr = E_FAIL;
        goto fail;
    }
    resource->hostResource = (ID3D11Resource *)opened;
    hr = triton9CheckHostDevice(device);
    if (FAILED(hr))
        goto fail;
    resource->hostReady = TRUE;
    args->hResource = (HANDLE)resource;
    triton9Diag("TRITON9-OPENRESOURCE success\n");
    return S_OK;

fail:
    if (resource->hostResource) {
        ID3D11Resource_Release(resource->hostResource);
        resource->hostResource = NULL;
    }
    if (resource->hImportAllocation || resource->hImportResource) {
        /* OpenRes can mint and then release a host object before it reports a
         * wrapper-allocation failure.  Drain that primary-ring release before
         * detaching the import allocation.  Leak the kernel handles on an
         * uncertain failure; no public UMD resource exists to retry safely. */
        if (!device->hostDevice ||
            !triton9DrainPrimaryTransport(device->hostDevice,
                                          TRITON9_HOST_DRAIN_TIMEOUT_MS) ||
            !triton9ReleaseImportTransport(device->hostDevice,
                                           resource->hImportAllocation,
                                           resource->hImportResource)) {
            device->deviceLost = TRUE;
            hr = D3DDDIERR_DEVICEREMOVED;
        }
    }
    HeapFree(GetProcessHeap(), 0, resource);
    return hr;
}
