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
#include "triton_trace_wire.h"

/* Defined in triton9_ddi.c; kept deliberately tiny so early DDI failures
 * can be inspected after a QMP-only reboot into Safe Mode. */
extern void triton9Diag(const char *message);

#include "../triton/tritonSharedBridge.h"
#include "npt_shared_texture.h"

#include <limits.h>
#include <stdio.h>
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
    case DXGI_FORMAT_R8G8B8A8_UNORM:
        value = 67; /* VIRGL_FORMAT_R8G8B8A8_UNORM */
        break;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        value = 8; /* VIRGL_FORMAT_R10G10B10A2_UNORM */
        break;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        value = 94; /* VIRGL_FORMAT_R16G16B16A16_FLOAT */
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
    const TRITON9_FORMAT *format = triton9FormatLookup(resource->format);
    UINT bw = format ? format->blockWidth : 1;
    UINT bh = format ? format->blockHeight : 1;
    UINT bytes = format ? format->bytesPerBlock : 1;
    uint64_t row, rows, pitch, slice, size;
    BOOL systemMemory = triton9ResourceIsSystemMemory(resource);
    UINT z;
    if (!resource->width || !resource->height || !resource->depth || !bw || !bh || !bytes)
        return D3DDDIERR_INVALIDCALL;
    resource->blockWidth = bw; resource->blockHeight = bh; resource->bytesPerBlock = bytes;
    row = ((uint64_t)resource->width + bw - 1) / bw * bytes;
    rows = ((uint64_t)resource->height + bh - 1) / bh;
    pitch = systemMemory && surface && !resource->isBuffer ? surface->SysMemPitch : row;
    slice = systemMemory && surface && surface->SysMemSlicePitch
        ? surface->SysMemSlicePitch : pitch * rows;
    size = slice * resource->depth;
    if (row > UINT_MAX || pitch < row || pitch > UINT_MAX ||
        slice < pitch * rows || slice > UINT_MAX ||
        size > (SIZE_T)-1 || !size)
        return systemMemory ? D3DDDIERR_INVALIDUSERBUFFER : E_OUTOFMEMORY;
    resource->rowBytes = (UINT)row; resource->pitch = (UINT)pitch;
    resource->slicePitch = (UINT)slice; resource->shadowSize = (SIZE_T)size;
    if (systemMemory) {
        if (!surface || !surface->pSysMem)
            return D3DDDIERR_INVALIDUSERBUFFER;
        resource->shadow = (BYTE *)(uintptr_t)surface->pSysMem;
        resource->ownsShadow = FALSE;
        return S_OK;
    }
    resource->shadow = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, resource->shadowSize);
    if (!resource->shadow) return E_OUTOFMEMORY;
    resource->ownsShadow = TRUE;
    if (surface && surface->pSysMem) {
        uint64_t srcPitch = surface->SysMemPitch ? surface->SysMemPitch : row;
        uint64_t srcSlice = surface->SysMemSlicePitch ? surface->SysMemSlicePitch : srcPitch * rows;
        if (srcPitch < row || srcSlice < srcPitch * rows ||
            srcSlice > (SIZE_T)-1 / resource->depth)
            return D3DDDIERR_INVALIDUSERBUFFER;
        for (z = 0; z < resource->depth; ++z)
            triton9CpuCopyRows(resource->shadow + (SIZE_T)z * resource->slicePitch,
                resource->pitch, (const BYTE *)surface->pSysMem + (SIZE_T)z * srcSlice,
                (SIZE_T)srcPitch, (SIZE_T)row, (UINT)rows);
    }
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
static void
triton9ReleaseResourceViews(TRITON9_RESOURCE *resource);

/* The public A2R10G10B10 CPU layout stores red in bits 20..29. Host
 * R10G10B10A2 storage is canonical RGBA, with red in bits 0..9. Convert only
 * at CPU boundaries; shader channels, blending and MRT masks stay uniform.
 * memcpy keeps unaligned runtime buffers valid and leaves row padding alone. */
static void
triton9SwapPacked10Rows(BYTE *destination, SIZE_T destinationPitch,
                        const BYTE *source, SIZE_T sourcePitch,
                        SIZE_T rowBytes, UINT rows)
{
    UINT y;
    for (y = 0; y < rows; ++y) {
        SIZE_T x;
        for (x = 0; x < rowBytes; x += 4) {
            UINT pixel;
            memcpy(&pixel, source + x, sizeof(pixel));
            pixel = (pixel & 0xc00ffc00u) | ((pixel & 0x3ffu) << 20) |
                    ((pixel >> 20) & 0x3ffu);
            memcpy(destination + x, &pixel, sizeof(pixel));
        }
        destination += destinationPitch;
        source += sourcePitch;
    }
}

HRESULT
triton9UpdateHostTexture(TRITON9_DEVICE *device, TRITON9_RESOURCE *resource,
                         ID3D11DeviceContext *context, ID3D11Resource *destination,
                         UINT subresource, const D3D11_BOX *box,
                         const BYTE *source, UINT rowPitch, UINT slicePitch)
{
    BYTE *converted = NULL;
    const BYTE *upload = source;
    UINT uploadPitch = rowPitch, uploadSlice = slicePitch;
    if (!triton9ResourceBelongsToDevice(device, resource) || !context ||
        !destination || !source)
        return E_INVALIDARG;
    if (!resource->isBuffer && resource->format == D3DDDIFMT_A2R10G10B10) {
        UINT width = box ? box->right - box->left : resource->width;
        UINT height = box ? box->bottom - box->top : resource->height;
        UINT depth = box ? box->back - box->front : resource->depth;
        uint64_t pitch = (uint64_t)width * 4;
        uint64_t slice = pitch * height;
        uint64_t sourceSlice = slicePitch ? slicePitch : (uint64_t)rowPitch * height;
        UINT z;
        if (!width || !height || !depth || pitch > rowPitch || pitch > UINT_MAX ||
            slice > UINT_MAX || slice > (SIZE_T)-1 / depth ||
            sourceSlice < (uint64_t)rowPitch * height || sourceSlice > (SIZE_T)-1 / depth)
            return E_INVALIDARG;
        converted = HeapAlloc(GetProcessHeap(), 0, (SIZE_T)slice * depth);
        if (!converted) return E_OUTOFMEMORY;
        for (z = 0; z < depth; ++z)
            triton9SwapPacked10Rows(converted + (SIZE_T)z * slice, (SIZE_T)pitch,
                source + (SIZE_T)z * sourceSlice, rowPitch, (SIZE_T)pitch, height);
        upload = converted; uploadPitch = (UINT)pitch; uploadSlice = (UINT)slice;
    }
    ID3D11DeviceContext_UpdateSubresource(context, destination, subresource, box,
                                          upload, uploadPitch, uploadSlice);
    if (converted) HeapFree(GetProcessHeap(), 0, converted);
    return S_OK;
}

static void
triton9ClearLockRegion(TRITON9_RESOURCE *resource)
{
    resource->lockRangeValid = FALSE;
    resource->lockAreaValid = FALSE;
    resource->lockBoxValid = FALSE;
    ZeroMemory(&resource->lockRange, sizeof(resource->lockRange));
    ZeroMemory(&resource->lockArea, sizeof(resource->lockArea));
    ZeroMemory(&resource->lockBox, sizeof(resource->lockBox));
}

static HRESULT
triton9UploadShadowRegion(TRITON9_DEVICE *device, TRITON9_RESOURCE *resource,
                           const D3D11_BOX *box, const BYTE *source,
                           UINT rowPitch, UINT slicePitch)
{
    BOOL track = resource->isBuffer && triton9ResourceIsSystemMemory(resource) &&
        resource->systemMemorySnapshot &&
        resource->systemMemorySnapshotSize == resource->width &&
        (!box || (resource->systemMemorySnapshotHost == resource->hostResource &&
                  resource->systemMemorySnapshotSerial == resource->contentSerial));
    HRESULT hr;

    if (resource->isBuffer && box &&
        (box->left >= box->right || box->right > resource->width))
        return D3DDDIERR_INVALIDCALL;
    if (track) {
        UINT first = box ? box->left : 0;
        UINT end = box ? box->right : resource->width;
        /* Preserve bytes outside a partial write as last submitted, even if
         * Vista changed its canonical alias there without a notification. */
        resource->systemMemorySnapshotHost = NULL;
        memcpy(resource->systemMemorySnapshot + first, source, end - first);
        source = resource->systemMemorySnapshot + first;
    }
    hr = triton9UpdateHostTexture(device, resource,
        (ID3D11DeviceContext *)device->hostContext, resource->hostResource,
        resource->subresourceIndex, box, source, rowPitch, slicePitch);
    if (FAILED(hr)) return hr;
    triton9ResourceWritten(resource);
    hr = triton9CheckHostDevice(device);
    if (track && SUCCEEDED(hr)) {
        resource->systemMemorySnapshotHost = resource->hostResource;
        resource->systemMemorySnapshotSerial = resource->contentSerial;
    }
    return hr;
}

static HRESULT
triton9UploadLockedShadow(TRITON9_DEVICE *device, TRITON9_RESOURCE *resource)
{
    D3D11_BOX box;
    const BYTE *source;
    if (!triton9ResourceBelongsToDevice(device, resource) || !resource->hostResource || !resource->shadow)
        return E_INVALIDARG;
    if (!resource->lockRangeValid && !resource->lockAreaValid && !resource->lockBoxValid)
        return triton9UploadShadow(device, resource);
    box.left = 0; box.right = resource->width; box.top = 0; box.bottom = resource->height;
    box.front = 0; box.back = resource->depth;
    if (resource->isBuffer && resource->lockRangeValid) {
        box.left = resource->lockRange.Offset;
        box.right = box.left + resource->lockRange.Size;
        source = resource->shadow + box.left;
    } else {
        if (resource->lockAreaValid) {
            box.left = resource->lockArea.left; box.top = resource->lockArea.top;
            box.right = resource->lockArea.right; box.bottom = resource->lockArea.bottom;
        } else if (resource->lockBoxValid) {
            box.left = resource->lockBox.Left; box.top = resource->lockBox.Top;
            box.right = resource->lockBox.Right; box.bottom = resource->lockBox.Bottom;
            box.front = resource->lockBox.Front; box.back = resource->lockBox.Back;
        }
        if (!triton9ResourceBoxValid(resource, &box)) return D3DDDIERR_INVALIDCALL;
        source = resource->shadow + triton9ResourceByteOffset(resource, box.left, box.top, box.front);
    }
    return triton9UploadShadowRegion(device, resource, &box, source,
        resource->pitch, resource->slicePitch);
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
    const TRITON9_FORMAT *format = triton9FormatLookup(resource->format);
    UINT bind = resource->isDepthStencil ? D3D11_BIND_DEPTH_STENCIL : D3D11_BIND_SHADER_RESOURCE;
    UINT misc = resource->isCube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
    HRESULT hr;
    if (!format) return D3DDDIERR_INVALIDCALL;
    if (!resource->isDepthStencil && (resource->wantsRenderTarget ||
        resource->wantsAutogenMipmap || (!resource->isTexture &&
        (format->operations & FORMATOP_OFFSCREENPLAIN))))
        bind |= D3D11_BIND_RENDER_TARGET;
    if (resource->wantsAutogenMipmap && resource->mipLevels > 1)
        misc |= D3D11_RESOURCE_MISC_GENERATE_MIPS;
    if (resource->isVolume) {
        D3D11_TEXTURE3D_DESC desc;
        ID3D11Texture3D *texture = NULL;
        ZeroMemory(&desc, sizeof(desc));
        desc.Width = resource->width; desc.Height = resource->height; desc.Depth = resource->depth;
        desc.MipLevels = resource->mipLevels; desc.Format = format->resourceFormat;
        desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = bind; desc.MiscFlags = misc;
        hr = ID3D11Device1_CreateTexture3D(device->hostDevice, &desc, NULL, &texture);
        resource->hostResource = (ID3D11Resource *)texture;
    } else {
        D3D11_TEXTURE2D_DESC desc;
        ID3D11Texture2D *texture = NULL;
        ZeroMemory(&desc, sizeof(desc));
        desc.Width = resource->width; desc.Height = resource->height;
        desc.MipLevels = resource->mipLevels; desc.ArraySize = resource->arraySize ? resource->arraySize : 1;
        desc.Format = format->resourceFormat;
        desc.SampleDesc.Count = resource->sampleCount ? resource->sampleCount : 1;
        desc.SampleDesc.Quality = resource->sampleQuality;
        desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = bind; desc.MiscFlags = misc;
        if (desc.SampleDesc.Count > 1) {
            UINT quality = 0;
            hr = ID3D11Device1_CheckMultisampleQualityLevels(device->hostDevice,
                resource->hostFormat, desc.SampleDesc.Count, &quality);
            if (FAILED(hr) || !quality || desc.SampleDesc.Quality >= quality)
                return D3DDDIERR_INVALIDCALL;
            desc.BindFlags &= ~D3D11_BIND_SHADER_RESOURCE;
        }
        hr = ID3D11Device1_CreateTexture2D(device->hostDevice, &desc, NULL, &texture);
        resource->hostResource = (ID3D11Resource *)texture;
        bind = desc.BindFlags;
    }
    if (FAILED(hr) || !resource->hostResource)
        return FAILED(hr) ? triton9MapDeviceFailure(device, hr) : E_FAIL;
    resource->hostBindFlags = bind;
    return S_OK;
}

static HRESULT
triton9OpenStandardPrimaryHost(TRITON9_DEVICE *device,
                               TRITON9_RESOURCE *resource)
{
    VIOGPU_ESCAPE escape;
    struct triton_shared_texture_desc desc;
    HRESULT hr;

    if (!resource->hKMAllocation)
        return E_INVALIDARG;
    hr = triton9EnsureRuntimeContext(device);
    if (FAILED(hr))
        return hr;
    ZeroMemory(&escape, sizeof(escape));
    escape.Type = VIOGPU_RES_INFO;
    escape.DataLength = sizeof(escape.ResourceInfo);
    escape.ResourceInfo.ResHandle = resource->hKMAllocation;
    hr = triton9Escape(device, &escape);
    if (FAILED(hr) || !escape.ResourceInfo.IsCreated || !escape.ResourceInfo.Id)
        return FAILED(hr) ? hr : E_FAIL;

    /* Import handles own only the transport attachment.  Keep the runtime's
     * original primary allocation handle and its ownership unchanged.
     * Retain an attachment across failed opens; DestroyResource drains and
     * releases it, and a retry must not overwrite its handles. */
    if (!resource->hImportAllocation && !resource->hImportResource &&
        !tritonSharedBridgeImportRes(device->hostDevice, escape.ResourceInfo.Id,
                                     (uint64_t)resource->width * resource->height * 4,
                                     &resource->hImportAllocation,
                                     &resource->hImportResource))
        return E_FAIL;
    ZeroMemory(&desc, sizeof(desc));
    if (!tritonSharedBridgeQueryRes(device->hostDevice, escape.ResourceInfo.Id,
                                    &desc) ||
        desc.width != resource->width || desc.height != resource->height)
        return D3DDDIERR_NOTAVAILABLE;
    resource->hostResource = (ID3D11Resource *)tritonSharedBridgeOpenRes(
        device->hostDevice, escape.ResourceInfo.Id, &desc);
    if (!resource->hostResource)
        return E_FAIL;
    /* GL can export logical BGRA as an RGBA image.  Views must use the real
     * host format; the application-facing D3D9 format remains unchanged. */
    resource->hostFormat = (DXGI_FORMAT)desc.format;
    resource->hostBindFlags = desc.bind_flags;
    return S_OK;
}

HRESULT
triton9EnsureResourceHost(TRITON9_DEVICE *device, TRITON9_RESOURCE *resource)
{
    HRESULT hr;

    if (!triton9ResourceBelongsToDevice(device, resource))
        return E_INVALIDARG;
    if (resource->textureOwner) {
        TRITON9_RESOURCE *root = resource->textureOwner;
        hr = triton9EnsureResourceHost(device, root);
        if (FAILED(hr)) return hr;
        return resource->hostReady && resource->hostResource ? S_OK : E_FAIL;
    }
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
        /* Render into the allocation Windows actually scans out. */
        hr = triton9OpenStandardPrimaryHost(device, resource);
    } else if (resource->isShared || resource->needsPresentAllocation) {
        ID3D11Resource *exported = (ID3D11Resource *)
            npt_shared_texture_create_exportable(device->hostDevice, resource->width,
                resource->height, npt_shared_texture_host_format(resource->hostFormat));
        if (!exported) { hr = E_FAIL; goto fail; }
        if (resource->sampleCount > 1) {
            /* VidMm scans out a single-sample export. Rendering and depth
             * testing retain the actual multisample allocation. */
            resource->resolveResource = exported;
            hr = triton9CreateHostTexture(device, resource);
            if (SUCCEEDED(hr)) {
                ID3D11Resource *multisampled = resource->hostResource;
                resource->hostResource = exported;
                hr = triton9RegisterSharedTexture(device, resource);
                resource->hostResource = multisampled;
            }
        } else {
            resource->hostResource = exported;
            resource->hostBindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            hr = triton9RegisterSharedTexture(device, resource);
        }
    } else {
        hr = triton9CreateHostTexture(device, resource);
    }
    if (SUCCEEDED(hr))
        hr = triton9CheckHostDevice(device);
    if (FAILED(hr))
        goto fail;
    {
        UINT i, count = resource->textureSurfaces ? resource->surfaceCount : 1;
        EnterCriticalSection(&device->shaderLock);
        for (i = 0; i < count; ++i) {
            TRITON9_RESOURCE *surface = resource->textureSurfaces ? resource->textureSurfaces[i] : resource;
            if (surface != resource) {
                if (surface->hostResource) ID3D11Resource_Release(surface->hostResource);
                surface->hostResource = resource->hostResource;
                ID3D11Resource_AddRef(surface->hostResource);
                surface->hostBindFlags = resource->hostBindFlags;
            }
            if (surface->hasInitialData && resource->sampleCount <= 1)
                hr = triton9UploadShadow(device, surface);
            if (FAILED(hr)) break;
        }
        if (SUCCEEDED(hr)) {
            for (i = 0; i < count; ++i)
                (resource->textureSurfaces ? resource->textureSurfaces[i] : resource)->hostReady = TRUE;
        }
        LeaveCriticalSection(&device->shaderLock);
    }
    if (FAILED(hr)) goto fail;
    resource->hostReady = TRUE;
    return S_OK;

fail:
    resource->hostReady = FALSE;
    if (resource->textureSurfaces) {
        UINT i;
        for (i = 1; i < resource->surfaceCount; ++i) {
            TRITON9_RESOURCE *surface = resource->textureSurfaces[i];
            triton9ReleaseResourceViews(surface);
            if (surface->hostResource) ID3D11Resource_Release(surface->hostResource);
            surface->hostResource = NULL; surface->hostReady = FALSE;
        }
    }
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
    if (!resource->hostResource) triton9ReleaseResourceViews(resource);
    return triton9MapDeviceFailure(device, hr);
}

static DXGI_FORMAT
triton9SrgbViewFormat(DXGI_FORMAT format)
{
    switch (format) {
    case DXGI_FORMAT_B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    case DXGI_FORMAT_B8G8R8X8_UNORM: return DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
    case DXGI_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case DXGI_FORMAT_BC1_UNORM: return DXGI_FORMAT_BC1_UNORM_SRGB;
    case DXGI_FORMAT_BC2_UNORM: return DXGI_FORMAT_BC2_UNORM_SRGB;
    case DXGI_FORMAT_BC3_UNORM: return DXGI_FORMAT_BC3_UNORM_SRGB;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

HRESULT
triton9GetRenderTargetViewEx(TRITON9_DEVICE *device, TRITON9_RESOURCE *resource,
                             BOOL srgb, ID3D11RenderTargetView **view)
{
    D3D11_RENDER_TARGET_VIEW_DESC desc;
    ID3D11RenderTargetView **slot;
    const TRITON9_FORMAT *format;
    HRESULT hr;
    if (!triton9ResourceBelongsToDevice(device, resource) || !view ||
        resource->isBuffer || resource->isDepthStencil ||
        triton9ResourceIsSystemMemory(resource)) return D3DDDIERR_INVALIDCALL;
    hr = triton9EnsureResourceHost(device, resource);
    if (FAILED(hr)) return hr;
    if (!(resource->hostBindFlags & D3D11_BIND_RENDER_TARGET)) return D3DDDIERR_INVALIDCALL;
    format = triton9FormatLookup(resource->format);
    if (srgb && (!format || format->srgbFormat == DXGI_FORMAT_UNKNOWN))
        return D3DDDIERR_INVALIDCALL;
    slot = srgb ? &resource->srgbRenderTargetView : &resource->renderTargetView;
    if (!*slot) {
        ZeroMemory(&desc, sizeof(desc));
        desc.Format = srgb ? triton9SrgbViewFormat(resource->hostFormat) : resource->hostFormat;
        if (resource->isVolume) {
            desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE3D;
            desc.Texture3D.MipSlice = resource->mipLevel;
            desc.Texture3D.WSize = resource->depth;
        } else if (resource->sampleCount > 1) {
            desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;
        } else if (resource->isCube) {
            desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
            desc.Texture2DArray.MipSlice = resource->mipLevel;
            desc.Texture2DArray.FirstArraySlice = resource->arraySlice;
            desc.Texture2DArray.ArraySize = 1;
        } else {
            desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            desc.Texture2D.MipSlice = resource->mipLevel;
        }
        hr = ID3D11Device1_CreateRenderTargetView(device->hostDevice,
            resource->hostResource, &desc, slot);
        if (FAILED(hr) || !*slot) return FAILED(hr) ? triton9MapDeviceFailure(device, hr) : E_FAIL;
    }
    *view = *slot;
    return S_OK;
}

HRESULT
triton9GetRenderTargetView(TRITON9_DEVICE *device, TRITON9_RESOURCE *resource,
                           ID3D11RenderTargetView **view)
{
    return triton9GetRenderTargetViewEx(device, resource, FALSE, view);
}

HRESULT
triton9GetDepthStencilView(TRITON9_DEVICE *device, TRITON9_RESOURCE *resource,
                           ID3D11DepthStencilView **view)
{
    D3D11_DEPTH_STENCIL_VIEW_DESC desc;
    HRESULT hr;
    if (!triton9ResourceBelongsToDevice(device, resource) || !view ||
        !resource->isDepthStencil || resource->isBuffer || resource->isVolume ||
        triton9ResourceIsSystemMemory(resource)) return D3DDDIERR_INVALIDCALL;
    hr = triton9EnsureResourceHost(device, resource);
    if (FAILED(hr)) return hr;
    if (!(resource->hostBindFlags & D3D11_BIND_DEPTH_STENCIL)) return D3DDDIERR_INVALIDCALL;
    if (!resource->depthStencilView) {
        ZeroMemory(&desc, sizeof(desc));
        desc.Format = resource->hostFormat;
        if (resource->sampleCount > 1) desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMS;
        else if (resource->isCube) {
            desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
            desc.Texture2DArray.MipSlice = resource->mipLevel;
            desc.Texture2DArray.FirstArraySlice = resource->arraySlice;
            desc.Texture2DArray.ArraySize = 1;
        } else {
            desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
            desc.Texture2D.MipSlice = resource->mipLevel;
        }
        hr = ID3D11Device1_CreateDepthStencilView(device->hostDevice,
            resource->hostResource, &desc, &resource->depthStencilView);
        if (FAILED(hr) || !resource->depthStencilView)
            return FAILED(hr) ? triton9MapDeviceFailure(device, hr) : E_FAIL;
    }
    *view = resource->depthStencilView;
    return S_OK;
}

HRESULT
triton9GetShaderResourceViewEx(TRITON9_DEVICE *device, TRITON9_RESOURCE *resource,
                               BOOL srgb, ID3D11ShaderResourceView **view)
{
    D3D11_SHADER_RESOURCE_VIEW_DESC desc;
    ID3D11ShaderResourceView **slot;
    const TRITON9_FORMAT *format;
    HRESULT hr;
    resource = triton9ResourceRoot(resource);
    if (!triton9ResourceBelongsToDevice(device, resource) || !view || resource->isBuffer ||
        resource->isDepthStencil || resource->sampleCount > 1) return D3DDDIERR_INVALIDCALL;
    hr = triton9PrepareResourceForHostRead(device, resource);
    if (FAILED(hr)) return hr;
    if (!(resource->hostBindFlags & D3D11_BIND_SHADER_RESOURCE)) return D3DDDIERR_INVALIDCALL;
    format = triton9FormatLookup(resource->format);
    if (srgb && (!format || format->srgbFormat == DXGI_FORMAT_UNKNOWN))
        return D3DDDIERR_INVALIDCALL;
    slot = srgb ? &resource->srgbShaderResourceView : &resource->shaderResourceView;
    if (!*slot) {
        ZeroMemory(&desc, sizeof(desc));
        desc.Format = srgb ? triton9SrgbViewFormat(resource->hostFormat) : resource->hostFormat;
        if (resource->isCube) {
            desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
            desc.TextureCube.MipLevels = resource->mipLevels;
        } else if (resource->isVolume) {
            desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
            desc.Texture3D.MipLevels = resource->mipLevels;
        } else {
            desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            desc.Texture2D.MipLevels = resource->mipLevels;
        }
        hr = ID3D11Device1_CreateShaderResourceView(device->hostDevice,
            resource->hostResource, &desc, slot);
        if (FAILED(hr) || !*slot) return FAILED(hr) ? triton9MapDeviceFailure(device, hr) : E_FAIL;
    }
    if (resource->wantsAutogenMipmap && resource->mipLevels > 1 &&
        resource->autogenDirty && !resource->autogenGenerating) {
        D3DDDIARG_GENERATEMIPSUBLEVELS args;
        ZeroMemory(&args, sizeof(args));
        args.hResource = (HANDLE)resource;
        args.Filter = resource->autogenFilter ? resource->autogenFilter : D3DDDITEXF_LINEAR;
        hr = triton9GenerateMipSubLevels((HANDLE)device, &args);
        if (FAILED(hr)) return hr;
    }
    *view = *slot;
    return S_OK;
}

HRESULT
triton9GetShaderResourceView(TRITON9_DEVICE *device, TRITON9_RESOURCE *resource,
                             ID3D11ShaderResourceView **view)
{
    return triton9GetShaderResourceViewEx(device, resource, FALSE, view);
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
        !device->callbacks.pfnAllocateCb || !device->callbacks.pfnDeallocateCb ||
        resource->hKMAllocation || resource->pendingExportResource)
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
    resource->pendingExportBlob = exported.blob_id;
    resource->pendingExportResource = resource->hostResource;
    ID3D11Resource_AddRef(resource->pendingExportResource);
    triton9DiagU32("TRITON9-SHARED-EXPORT-CTX", exported.create_ctx_id);
    /* Treat export metadata as untrusted transport input. In particular,
     * allocation.Size is later converted to SIZE_T by the KMD. Reject a
     * value that would wrap either the page alignment or a Vista x86
     * SIZE_T. */
    if (!exported.blob_id || exported.plane_count != 1 ||
        exported.texture_layout > 2 ||
        exported.planes[1].offset || exported.planes[1].pitch ||
        exported.planes[2].offset || exported.planes[2].pitch ||
        exported.planes[3].offset || exported.planes[3].pitch ||
        !exported.allocation_size ||
        exported.allocation_size > (uint64_t)((SIZE_T)-1) - 4095ull) {
        hr = E_FAIL;
        goto rollback;
    }
    if (!triton9ValidateSharedPlane(exported.planes[0].offset,
                                    exported.planes[0].pitch,
                                    exported.allocation_size,
                                    resource->width, resource->height,
                                    resource->bytesPerPixel)) {
        hr = E_FAIL;
        goto rollback;
    }

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
    /* A failed callback may still return a live allocation. Retain that
     * handle before inspecting HRESULT, including on rollback failure. */
    resource->hKMAllocation = allocationInfo.hAllocation;
    resource->ownsKMAllocation = allocationInfo.hAllocation != 0;
    resource->kmResourceAssociated = callback.hResource != NULL;
    if (FAILED(hr) || !allocationInfo.hAllocation) {
        hr = FAILED(hr) ? hr : E_FAIL;
        triton9DiagU32("TRITON9-SHARED-ALLOCATE-FAIL",
                       (DWORD)hr);
        goto rollback;
    }
    /* KMD consumed the export; release our extra wrapper reference. */
    ID3D11Resource_Release(resource->pendingExportResource);
    resource->pendingExportResource = NULL;
    resource->pendingExportBlob = 0;
    triton9DiagU32("TRITON9-SHARED-ALLOC", allocationInfo.hAllocation);
    return S_OK;

rollback:
    if (FAILED(triton9DeallocateResource(device, resource))) {
        device->deviceLost = TRUE;
        return D3DDDIERR_DEVICEREMOVED;
    }
    return hr;
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
    resource->hKMAllocation = allocationInfo.hAllocation;
    resource->ownsKMAllocation = allocationInfo.hAllocation != 0;
    if (FAILED(hr) || !allocationInfo.hAllocation) {
        triton9DiagU32("TRITON9-PRIMARY-ALLOCATE-FAIL",
                       FAILED(hr) ? (DWORD)hr : (DWORD)E_FAIL);
        return FAILED(hr) ? hr : E_FAIL;
    }
    triton9DiagU32("TRITON9-PRIMARY-ALLOC", allocationInfo.hAllocation);
    return S_OK;
}

/* Destroy associated allocations with their runtime resource; use allocation
 * handles only for allocations that were attached directly to the device. */
static HRESULT
triton9DeallocateResource(TRITON9_DEVICE *device, TRITON9_RESOURCE *resource)
{
    D3DDDICB_DEALLOCATE deallocate;
    HRESULT hr = S_OK;

    if (!triton9ResourceBelongsToDevice(device, resource))
        return E_INVALIDARG;
    if (resource->ownsKMAllocation && resource->hKMAllocation) {
        ZeroMemory(&deallocate, sizeof(deallocate));
        if (resource->kmResourceAssociated) {
            deallocate.hResource = resource->hRTResource;
        } else {
            deallocate.NumAllocations = 1;
            deallocate.HandleList = &resource->hKMAllocation;
        }
        hr = device->callbacks.pfnDeallocateCb ?
            device->callbacks.pfnDeallocateCb(device->hRTDevice, &deallocate) : E_FAIL;
        if (SUCCEEDED(hr)) {
            resource->hKMAllocation = 0;
            resource->ownsKMAllocation = FALSE;
            resource->kmResourceAssociated = FALSE;
        }
    }
    /* Idempotent even if AllocateCb already consumed the pending blob. Keep
     * its owning wrapper ref if transport cleanup cannot complete. */
    if (resource->pendingExportResource) {
        if (tritonSharedBridgeCancelExportBlob(resource->pendingExportResource,
                                               resource->pendingExportBlob)) {
            ID3D11Resource_Release(resource->pendingExportResource);
            resource->pendingExportResource = NULL;
            resource->pendingExportBlob = 0;
        } else {
            hr = E_FAIL;
        }
    }
    if (FAILED(hr)) {
        device->deviceLost = TRUE;
        return D3DDDIERR_DEVICEREMOVED;
    }
    return S_OK;
}

static void
triton9FreeUnpublishedResource(TRITON9_DEVICE *device, TRITON9_RESOURCE *resource)
{
    triton9ReleaseResourceViews(resource);
    if (resource->fvfDeclaration)
        triton9DestroyFvfDeclaration(device, resource->fvfDeclaration);
    if (resource->pendingExportResource)
        ID3D11Resource_Release(resource->pendingExportResource);
    if (resource->hostResource)
        ID3D11Resource_Release(resource->hostResource);
    if (resource->ownsShadow && resource->shadow)
        HeapFree(GetProcessHeap(), 0, resource->shadow);
    if (resource->systemMemorySnapshot)
        HeapFree(GetProcessHeap(), 0, resource->systemMemorySnapshot);
    HeapFree(GetProcessHeap(), 0, resource);
}

/* Failed creation has no public handle for a later DestroyResource. Retain
 * unsuccessful rollback records until final device teardown can retry. */
static HRESULT
triton9DisposeFailedResource(TRITON9_DEVICE *device, TRITON9_RESOURCE *resource,
                              HRESULT originalFailure)
{
    HRESULT hr = triton9DeallocateResource(device, resource);
    if (FAILED(hr)) {
        resource->failedNext = device->failedResources;
        device->failedResources = resource;
        device->deviceLost = TRUE;
        return D3DDDIERR_DEVICEREMOVED;
    }
    triton9FreeUnpublishedResource(device, resource);
    return originalFailure;
}

HRESULT
triton9DestroyFailedResources(TRITON9_DEVICE *device)
{
    HRESULT result = S_OK;
    if (!device) return E_INVALIDARG;
    while (device->failedResources) {
        TRITON9_RESOURCE *resource = device->failedResources;
        HRESULT hr = triton9DeallocateResource(device, resource);
        if (FAILED(hr) && SUCCEEDED(result)) result = hr;
        device->failedResources = resource->failedNext;
        /* DestroyDevice is final: runtime/KMD device teardown owns any kernel
         * handles that still could not be released. No borrowed callbacks or
         * process-local COM/heap records may survive this boundary. */
        triton9FreeUnpublishedResource(device, resource);
    }
    return result;
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
    if (resource->srgbShaderResourceView)
        ID3D11ShaderResourceView_Release(resource->srgbShaderResourceView);
    if (resource->srgbRenderTargetView)
        ID3D11RenderTargetView_Release(resource->srgbRenderTargetView);
    if (resource->resolveResource) ID3D11Resource_Release(resource->resolveResource);
    resource->srgbShaderResourceView = NULL;
    resource->srgbRenderTargetView = NULL;
    resource->resolveResource = NULL;
    if (resource->readbackQuery) ID3D11Query_Release(resource->readbackQuery);
    resource->readbackQuery = NULL;
    resource->readbackPending = resource->readbackReady = FALSE;
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
    return triton9UploadShadowRegion(device, resource, NULL, resource->shadow,
        resource->isBuffer ? 0 : resource->pitch, resource->isBuffer ? 0 : resource->slicePitch);
}

/* The caller holds shaderLock. Keep an exact copy of submitted bytes instead
 * of treating Lock/Unlock notifications as ownership of Vista's alias. */
static HRESULT
triton9RefreshSystemMemoryBuffer(TRITON9_DEVICE *device,
                                 TRITON9_RESOURCE *resource,
                                 UINT first, UINT end)
{
    BYTE *snapshot = resource->systemMemorySnapshot;
    BOOL valid;
    D3D11_BOX box;
    HRESULT hr;

    if (!resource->width || resource->shadowSize < resource->width)
        return D3DDDIERR_INVALIDUSERBUFFER;
    if (first >= end || end > resource->width)
        return D3DDDIERR_INVALIDCALL;
    valid = snapshot && resource->systemMemorySnapshotSize == resource->width &&
        resource->systemMemorySnapshotHost == resource->hostResource &&
        resource->systemMemorySnapshotSerial == resource->contentSerial;
    if (!valid) {
        /* Establish the entire submitted baseline before publishing a valid
         * cache. Later draws may read any other range of this same buffer. */
        first = 0;
        end = resource->width;
    }
    if (!snapshot || resource->systemMemorySnapshotSize != resource->width) {
        BYTE *replacement = HeapAlloc(GetProcessHeap(), 0, resource->width);
        resource->systemMemorySnapshotHost = NULL;
        if (!replacement)
            return triton9UploadShadow(device, resource);
        if (snapshot) HeapFree(GetProcessHeap(), 0, snapshot);
        resource->systemMemorySnapshot = snapshot = replacement;
        resource->systemMemorySnapshotSize = resource->width;
    }
    if (valid) {
        /* Compare large equal spans with the CRT, then locate exact byte
         * boundaries without reading beyond either allocation. */
        while (end - first >= 4096 &&
               !memcmp(snapshot + first, resource->shadow + first, 4096))
            first += 4096;
        while (first < end && snapshot[first] == resource->shadow[first])
            ++first;
        if (first == end) {
            hr = triton9CheckHostDevice(device);
            if (FAILED(hr)) resource->systemMemorySnapshotHost = NULL;
            return hr;
        }
        while (end - first >= 4096 &&
               !memcmp(snapshot + end - 4096, resource->shadow + end - 4096, 4096))
            end -= 4096;
        while (end > first && snapshot[end - 1] == resource->shadow[end - 1])
            --end;
    }
    resource->systemMemorySnapshotHost = NULL;
    memcpy(snapshot + first, resource->shadow + first, end - first);
    box.left = first; box.right = end;
    box.top = box.front = 0; box.bottom = box.back = 1;
    hr = triton9UpdateHostTexture(device, resource,
        (ID3D11DeviceContext *)device->hostContext, resource->hostResource, 0,
        first || end != resource->width ? &box : NULL, snapshot + first, 0, 0);
    if (FAILED(hr)) return hr;
    triton9ResourceWritten(resource);
    hr = triton9CheckHostDevice(device);
    if (SUCCEEDED(hr)) {
        resource->systemMemorySnapshotHost = resource->hostResource;
        resource->systemMemorySnapshotSerial = resource->contentSerial;
    }
    return hr;
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
    if (resource->textureSurfaces) {
        UINT i;
        for (i = 0; i < resource->surfaceCount && SUCCEEDED(hr); ++i)
            hr = triton9UploadShadow(device, resource->textureSurfaces[i]);
    } else if (resource->isBuffer)
        hr = triton9RefreshSystemMemoryBuffer(device, resource, 0, resource->width);
    else hr = triton9UploadShadow(device, resource);
    LeaveCriticalSection(&device->shaderLock);
    return triton9MapDeviceFailure(device, hr);
}

/* Vertex/index bindings do not consume data. Refresh their validated byte
 * range at each draw, including alias writes made since an earlier draw. */
HRESULT
triton9PrepareBufferRangeForHostRead(TRITON9_DEVICE *device,
                                     TRITON9_RESOURCE *resource,
                                     UINT first, UINT end)
{
    HRESULT hr;

    if (!triton9ResourceBelongsToDevice(device, resource) || !resource->isBuffer ||
        first >= end || end > resource->width)
        return D3DDDIERR_INVALIDCALL;
    hr = triton9EnsureResourceHost(device, resource);
    if (FAILED(hr) || !triton9ResourceIsSystemMemory(resource))
        return hr;
    if (!resource->hostResource || !resource->shadow)
        return D3DDDIERR_INVALIDUSERBUFFER;
    EnterCriticalSection(&device->shaderLock);
    hr = triton9RefreshSystemMemoryBuffer(device, resource, first, end);
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
    } else if (resource->isVolume) {
        D3D11_TEXTURE3D_DESC desc;
        ID3D11Texture3D *texture = NULL;
        ZeroMemory(&desc, sizeof(desc));
        desc.Width = resource->width; desc.Height = resource->height; desc.Depth = resource->depth;
        desc.MipLevels = 1; desc.Format = resource->hostFormat;
        desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        hr = ID3D11Device1_CreateTexture3D(device->hostDevice, &desc, NULL, &texture);
        if (FAILED(hr) || !texture) return FAILED(hr) ? triton9MapDeviceFailure(device, hr) : E_FAIL;
        resource->stagingResource = (ID3D11Resource *)texture;
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
    if (!resource->readbackReady) {
        ID3D11DeviceContext1_Flush(device->hostContext);
        if (!tritonSharedBridgeDrain(device->hostContext,
                                     TRITON9_HOST_DRAIN_TIMEOUT_MS)) {
            device->deviceLost = TRUE;
            return D3DDDIERR_DEVICEREMOVED;
        }
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
                                  0, D3D11_MAP_READ,
                                  resource->readbackReady ? D3D11_MAP_FLAG_DO_NOT_WAIT : 0, mapped);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return D3DDDIERR_WASSTILLDRAWING;
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
triton9CopyStagingBoxToShadow(TRITON9_DEVICE *device, TRITON9_RESOURCE *resource,
                              const D3D11_BOX *box)
{
    D3D11_MAPPED_SUBRESOURCE mapped;
    UINT bw = resource->blockWidth ? resource->blockWidth : 1;
    UINT bh = resource->blockHeight ? resource->blockHeight : 1;
    UINT bytes = resource->bytesPerBlock ? resource->bytesPerBlock : resource->bytesPerPixel;
    UINT firstRow, rows, z;
    SIZE_T xBytes, copyBytes, depthPitch;
    HRESULT hr;
    if (!triton9ResourceBelongsToDevice(device, resource) || resource->isBuffer ||
        !triton9ResourceBoxValid(resource, box)) return E_INVALIDARG;
    firstRow = box->top / bh;
    rows = (box->bottom + bh - 1) / bh - firstRow;
    xBytes = (SIZE_T)(box->left / bw) * bytes;
    copyBytes = (SIZE_T)((box->right + bw - 1) / bw - box->left / bw) * bytes;
    hr = triton9MapStagingForRead(device, resource, &mapped);
    if (FAILED(hr)) return hr;
    depthPitch = resource->isVolume ? mapped.DepthPitch : (SIZE_T)mapped.RowPitch * ((resource->height + bh - 1) / bh);
    if (mapped.RowPitch < resource->rowBytes ||
        depthPitch < (SIZE_T)mapped.RowPitch * ((resource->height + bh - 1) / bh)) {
        hr = E_FAIL;
    } else {
        for (z = box->front; z < box->back; ++z) {
            BYTE *destination = resource->shadow + (SIZE_T)z * resource->slicePitch +
                (SIZE_T)firstRow * resource->pitch + xBytes;
            const BYTE *source = (const BYTE *)mapped.pData + (SIZE_T)z * depthPitch +
                (SIZE_T)firstRow * mapped.RowPitch + xBytes;
            if (resource->format == D3DDDIFMT_A2R10G10B10)
                triton9SwapPacked10Rows(destination, resource->pitch, source,
                    mapped.RowPitch, copyBytes, rows);
            else triton9CpuCopyRows(destination, resource->pitch, source,
                    mapped.RowPitch, copyBytes, rows);
        }
    }
    ID3D11DeviceContext1_Unmap(device->hostContext, resource->stagingResource, 0);
    return FAILED(hr) ? hr : triton9CheckHostDevice(device);
}

HRESULT
triton9CopyStagingSurfaceToShadow(TRITON9_DEVICE *device, TRITON9_RESOURCE *resource,
                                  const RECT *region)
{
    D3D11_BOX box;
    if (!region || region->left < 0 || region->top < 0) return E_INVALIDARG;
    box.left = region->left; box.top = region->top; box.right = region->right;
    box.bottom = region->bottom; box.front = 0; box.back = 1;
    return triton9CopyStagingBoxToShadow(device, resource, &box);
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
HRESULT
triton9ResolveResource(TRITON9_DEVICE *device, TRITON9_RESOURCE *resource,
                       ID3D11Resource **host, UINT *subresource)
{
    HRESULT hr;
    if (!triton9ResourceBelongsToDevice(device, resource) || !host || !subresource)
        return E_INVALIDARG;
    hr = triton9EnsureResourceHost(device, resource);
    if (FAILED(hr)) return hr;
    *host = resource->hostResource; *subresource = resource->subresourceIndex;
    if (resource->sampleCount <= 1) return S_OK;
    if (resource->isDepthStencil) return D3DDDIERR_INVALIDCALL;
    if (!resource->resolveResource) {
        D3D11_TEXTURE2D_DESC desc;
        ID3D11Texture2D *texture = NULL;
        ZeroMemory(&desc, sizeof(desc));
        desc.Width = resource->width; desc.Height = resource->height;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = resource->hostFormat; desc.Usage = D3D11_USAGE_DEFAULT;
        hr = ID3D11Device1_CreateTexture2D(device->hostDevice, &desc, NULL, &texture);
        if (FAILED(hr) || !texture) return FAILED(hr) ? triton9MapDeviceFailure(device, hr) : E_FAIL;
        resource->resolveResource = (ID3D11Resource *)texture;
    }
    ID3D11DeviceContext1_ResolveSubresource(device->hostContext, resource->resolveResource,
        0, resource->hostResource, resource->subresourceIndex, resource->hostFormat);
    *host = resource->resolveResource; *subresource = 0;
    return triton9CheckHostDevice(device);
}

static HRESULT
triton9ReadbackShadow(TRITON9_DEVICE *device, TRITON9_RESOURCE *resource)
{
    ID3D11Resource *host;
    UINT subresource;
    HRESULT hr;
    if (!triton9ResourceBelongsToDevice(device, resource) || !resource->hostResource)
        return E_INVALIDARG;
    hr = triton9EnsureStagingResource(device, resource);
    if (FAILED(hr)) return hr;
    hr = triton9ResolveResource(device, resource, &host, &subresource);
    if (FAILED(hr)) return hr;
    ID3D11DeviceContext1_CopySubresourceRegion(device->hostContext,
        resource->stagingResource, 0, 0, 0, 0, host, subresource, NULL);
    if (resource->isBuffer)
        return triton9CopyStagingBufferToShadow(device, resource, 0, (UINT)resource->shadowSize);
    {
        D3D11_BOX full = { 0, 0, 0, resource->width, resource->height, resource->depth };
        return triton9CopyStagingBoxToShadow(device, resource, &full);
    }
}

/* An event fences the copy, so a later DONOTWAIT attempt can map without
 * draining the transport or waiting for GPU work. A write since the earlier
 * attempt invalidates that snapshot, even if it used another mip/face alias. */
static HRESULT
triton9ReadbackShadowAsync(TRITON9_DEVICE *device, TRITON9_RESOURCE *resource)
{
    TRITON9_RESOURCE *root = triton9ResourceRoot(resource);
    HRESULT hr;
    BOOL complete = FALSE;
    if (!resource->readbackQuery) {
        D3D11_QUERY_DESC desc = { D3D11_QUERY_EVENT, 0 };
        hr = ID3D11Device1_CreateQuery(device->hostDevice, &desc, &resource->readbackQuery);
        if (FAILED(hr) || !resource->readbackQuery) return FAILED(hr) ? hr : E_FAIL;
    }
    hr = triton9EnsureStagingResource(device, resource);
    if (FAILED(hr)) return hr;
    if (!resource->readbackPending || resource->readbackSerial != root->contentSerial) {
        ID3D11DeviceContext1_CopySubresourceRegion(device->hostContext,
            resource->stagingResource, 0, 0, 0, 0,
            resource->hostResource, resource->subresourceIndex, NULL);
        ID3D11DeviceContext1_End(device->hostContext, (ID3D11Asynchronous *)resource->readbackQuery);
        ID3D11DeviceContext1_Flush(device->hostContext);
        resource->readbackPending = TRUE;
        resource->readbackSerial = root->contentSerial;
        return D3DDDIERR_WASSTILLDRAWING;
    }
    hr = ID3D11DeviceContext1_GetData(device->hostContext,
        (ID3D11Asynchronous *)resource->readbackQuery, &complete, sizeof(complete), D3D11_ASYNC_GETDATA_DONOTFLUSH);
    if (hr == S_FALSE || (SUCCEEDED(hr) && !complete)) return D3DDDIERR_WASSTILLDRAWING;
    if (FAILED(hr)) return hr;
    resource->readbackReady = TRUE;
    if (resource->isBuffer) {
        hr = triton9CopyStagingBufferToShadow(device, resource, 0, (UINT)resource->shadowSize);
    } else {
        D3D11_BOX box = { 0, 0, 0, resource->width, resource->height, resource->depth };
        hr = triton9CopyStagingBoxToShadow(device, resource, &box);
    }
    resource->readbackReady = FALSE;
    if (SUCCEEDED(hr)) resource->readbackPending = FALSE;
    return hr;
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
    if ((args->Flags.RenderTarget || args->Flags.ZBuffer) &&
        ((args->MultisampleType == D3DDDIMULTISAMPLE_NONMASKABLE &&
          args->MultisampleQuality >= triton9FormatMultisampleQuality(args->Format, D3DDDIMULTISAMPLE_NONMASKABLE)) ||
         args->MultisampleType > D3DDDIMULTISAMPLE_16_SAMPLES ||
         (args->MultisampleType == D3DDDIMULTISAMPLE_NONE && args->MultisampleQuality)))
        return D3DDDIERR_INVALIDCALL;
    if (args->Rotation && args->Rotation != D3DDDI_ROTATION_IDENTITY)
        return D3DDDIERR_INVALIDCALL;
    /* Video is a content hint on an ordinary render target (including RGB
     * swap chains created with D3DPRESENTFLAG_VIDEO). It does not
     * request DXVA decoding, video processing, or protected allocation.
     * Validate its format and render-target usage through the normal path. */
    if (args->Flags.Video && (!args->Flags.RenderTarget || args->Flags.ZBuffer))
        return D3DDDIERR_INVALIDCALL;
    if (args->Flags.Overlay || args->Flags.Volume ||
        args->Flags.CubeMap || args->Flags.DecodeRenderTarget ||
        args->Flags.DecodeCompressedBuffer || args->Flags.VideoProcessRenderTarget ||
        args->Flags.DMap || args->Flags.Points || args->Flags.RtPatches ||
        args->Flags.NPatches || args->Flags.CaptureBuffer ||
        args->Flags.InterlacedRefresh ||
        args->Flags.TextApi || args->Flags.RestrictedContent ||
        args->Flags.RestrictSharedAccess)
        return D3DDDIERR_INVALIDCALL;
    /* Explicit multi-mip resources expose every subresource to the DDI. They
     * remain unsupported until lock, copy, and views cover each level. Auto
     * mipmaps are different: Vista exposes only mip zero and lets the driver
     * create the hidden chain. */
    if (args->SurfCount != 1 || ((args->Flags.Texture || args->Flags.CubeMap || args->Flags.Volume) && args->MipLevels > 1))
        return D3DDDIERR_INVALIDCALL;

    resource = (TRITON9_RESOURCE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                              sizeof(*resource));
    if (!resource)
        return E_OUTOFMEMORY;
    resource->hOwnerDevice = (HANDLE)(uintptr_t)device;
    resource->hRTResource = args->hResource;
    resource->independentAllocation = independentAllocation;
    resource->format = args->Format;
    resource->fvf = args->Flags.VertexBuffer ? args->Fvf : 0;
    resource->isTexture = args->Flags.Texture;
    resource->arraySize = 1; resource->exposedMipLevels = 1;
    resource->sampleCount = (args->Flags.RenderTarget || args->Flags.ZBuffer) && args->MultisampleType
        ? (UINT)args->MultisampleType : 1;
    resource->sampleQuality = resource->sampleCount > 1 ? args->MultisampleQuality : 0;
    if ((args->Flags.RenderTarget || args->Flags.ZBuffer) &&
        args->MultisampleType == D3DDDIMULTISAMPLE_NONMASKABLE) {
        /* Public quality indexes select the two guaranteed sample counts;
         * they are not native DXGI sample-pattern quality indexes. */
        resource->nonMaskable = TRUE;
        resource->sampleCount = args->MultisampleQuality ? 4 : 2;
        resource->sampleQuality = 0;
    }
    resource->dynamic = args->Flags.Dynamic;
    resource->mipLevels = 1;
    resource->surfaceCount = 1;
    resource->vidPnSourceId = args->VidPnSourceId;
    resource->isPrimary = args->Flags.Primary;
    /* Every UMD-created colour render surface may become a Present source,
     * including a single MatchGdiPrimary render target. Its KMD allocation
     * must describe the texture we actually render into. KMD-created desktop
     * primaries are handled separately by OpenResource. */
    resource->isShared = args->Flags.SharedResource;
    resource->pool = args->Pool;
    resource->isDepthStencil = args->Flags.ZBuffer;
    resource->wantsRenderTarget = args->Flags.RenderTarget;
    resource->needsPresentAllocation =
        !args->Flags.Texture && resource->wantsRenderTarget &&
        !resource->isDepthStencil && (args->Format == D3DDDIFMT_A8R8G8B8 ||
                                     args->Format == D3DDDIFMT_X8R8G8B8);
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
            hr = D3DDDIERR_INVALIDCALL;
            goto fail;
        }
        hr = triton9CreateShadow(resource, &args->pSurfList[0]);
        if (FAILED(hr))
            goto fail;
    } else {
        format = triton9FormatLookup(args->Format);
        if (!format) {
            hr = D3DDDIERR_INVALIDCALL;
            goto fail;
        }
        if (resource->isShared && (resource->sampleCount != 1 || args->Flags.AutogenMipmap)) {
            hr = D3DDDIERR_INVALIDCALL; goto fail;
        }
        if ((resource->wantsRenderTarget && !(format->operations & FORMATOP_OFFSCREEN_RENDERTARGET)) ||
            (!resource->isTexture && !resource->isDepthStencil && !resource->wantsRenderTarget &&
             !(format->operations & FORMATOP_OFFSCREENPLAIN)) ||
            (resource->sampleCount > 1 &&
             resource->sampleQuality >= triton9FormatMultisampleQuality(resource->format, resource->sampleCount))) {
            hr = D3DDDIERR_INVALIDCALL; goto fail;
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
            hr = D3DDDIERR_INVALIDCALL;
            goto fail;
        }
        if ((resource->isPrimary || resource->isShared ||
             resource->needsPresentAllocation) &&
            !triton9SharedFormat(resource->hostFormat, NULL)) {
            hr = D3DDDIERR_INVALIDCALL;
            goto fail;
        }
        if (resource->isPrimary && resource->hostFormat != DXGI_FORMAT_B8G8R8A8_UNORM &&
            resource->hostFormat != DXGI_FORMAT_B8G8R8X8_UNORM) {
            hr = D3DDDIERR_INVALIDCALL;
            goto fail;
        }
        if (args->Flags.AutogenMipmap) {
            if (format->depthStencil || resource->isPrimary || resource->isShared ||
                resource->needsPresentAllocation) {
                hr = D3DDDIERR_INVALIDCALL;
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
            hr = D3DDDIERR_INVALIDCALL;
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
    if (resource->needsPresentAllocation || resource->isShared) {
        /* Fullscreen setup and shared-handle publication query the allocation
         * before the first draw or Present. A shared texture must already
         * have its runtime-associated KMD allocation when CreateResource
         * returns, or CreateTexture succeeds with a null shared handle and
         * DWM's window-surface handshake cannot finish. */
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
            device->depthStencilStateDirty = TRUE;
        }
        LeaveCriticalSection(&device->shaderLock);
    }
    return triton9DisposeFailedResource(device, resource, hr);
}

/* Texture handles own one allocation and one CPU/lock/view record per DDI
 * surface. A volume mip contains all Z slices; a cube has six mip chains. */
static HRESULT
triton9CreateTextureResource(TRITON9_DEVICE *device, D3DDDIARG_CREATERESOURCE *args)
{
    const TRITON9_FORMAT *format;
    TRITON9_RESOURCE *root = NULL;
    TRITON9_RESOURCE **surfaces = NULL;
    UINT faces, exposed, levels, largest, i;
    HRESULT hr = D3DDDIERR_INVALIDCALL;
    SIZE_T total = 0;
    if (!device || !args || !args->pSurfList || !args->SurfCount) return E_INVALIDARG;
    if (device->deviceLost) return D3DDDIERR_DEVICEREMOVED;
    format = triton9FormatLookup(args->Format);
    if (!format || args->Pool < D3DDDIPOOL_SYSTEMMEM || args->Pool > D3DDDIPOOL_NONLOCALVIDMEM ||
        (args->Flags.CubeMap && args->Flags.Volume) || args->Flags.Primary ||
        args->Flags.VertexBuffer || args->Flags.IndexBuffer || args->Flags.Overlay ||
        args->Flags.DecodeRenderTarget || args->Flags.DecodeCompressedBuffer ||
        args->Flags.VideoProcessRenderTarget || args->Flags.DMap || args->Flags.RestrictedContent ||
        args->Flags.RestrictSharedAccess || args->Flags.CaptureBuffer || args->Flags.TextApi ||
        args->Flags.SharedResource || (args->Flags.Volume && args->Flags.RenderTarget))
        return D3DDDIERR_INVALIDCALL;
    if ((args->Flags.CubeMap && !(format->operations & FORMATOP_CUBETEXTURE)) ||
        (args->Flags.Volume && !(format->operations & FORMATOP_VOLUMETEXTURE)) ||
        (!args->Flags.CubeMap && !args->Flags.Volume && !(format->operations & FORMATOP_TEXTURE)) ||
        (args->Flags.RenderTarget && !(format->operations & FORMATOP_OFFSCREEN_RENDERTARGET)) ||
        (format->depthStencil != (args->Flags.ZBuffer != 0)))
        return D3DDDIERR_INVALIDCALL;
    faces = args->Flags.CubeMap ? 6 : 1;
    if (args->SurfCount % faces) return D3DDDIERR_INVALIDCALL;
    exposed = args->SurfCount / faces;
    largest = args->pSurfList[0].Width;
    if (args->pSurfList[0].Height > largest) largest = args->pSurfList[0].Height;
    if (args->Flags.Volume && args->pSurfList[0].Depth > largest) largest = args->pSurfList[0].Depth;
    levels = triton9FullMipCount(largest, 1);
    if (!largest || exposed > levels || (args->MipLevels && args->MipLevels != exposed) ||
        (args->Flags.AutogenMipmap && (args->Flags.Volume || args->Pool == D3DDDIPOOL_SYSTEMMEM ||
         exposed != 1)))
        return D3DDDIERR_INVALIDCALL;
    if (!args->Flags.AutogenMipmap || !(format->operations & FORMATOP_AUTOGENMIPMAP)) levels = exposed;
    if (args->Flags.RenderTarget && (args->MultisampleType != D3DDDIMULTISAMPLE_NONE || args->MultisampleQuality))
        return D3DDDIERR_INVALIDCALL;
    surfaces = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, args->SurfCount * sizeof(*surfaces));
    if (!surfaces) return E_OUTOFMEMORY;
    for (i = 0; i < args->SurfCount; ++i) {
        const D3DDDI_SURFACEINFO *info = &args->pSurfList[i];
        UINT mip = i % exposed, w = args->pSurfList[0].Width >> mip;
        UINT h = args->pSurfList[0].Height >> mip;
        UINT d = args->Flags.Volume ? args->pSurfList[0].Depth >> mip : 1;
        TRITON9_RESOURCE *r;
        if (!w) w = 1;
        if (!h) h = 1;
        if (!d) d = 1;
        if (info->Width != w || info->Height != h ||
            (args->Flags.Volume ? info->Depth != d : info->Depth > 1) ||
            w > 4096 || h > 4096 || d > 2048 || (args->Flags.CubeMap && w != h)) goto fail;
        r = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*r));
        if (!r) { hr = E_OUTOFMEMORY; goto fail; }
        surfaces[i] = r;
        if (!root) root = r;
        r->hOwnerDevice = (HANDLE)device; r->hRTResource = args->hResource;
        r->textureOwner = i ? root : NULL;
        r->format = args->Format; r->hostFormat = format->hostFormat;
        r->width = w; r->height = h; r->depth = d;
        r->mipLevels = levels; r->exposedMipLevels = exposed;
        r->mipLevel = mip; r->arraySlice = i / exposed;
        r->subresourceIndex = r->arraySlice * levels + mip;
        r->arraySize = faces; r->surfaceCount = 1;
        r->sampleCount = 1;
        r->isTexture = TRUE; r->isCube = args->Flags.CubeMap; r->isVolume = args->Flags.Volume;
        r->bytesPerPixel = format->bytesPerPixel;
        r->pool = args->Pool; r->isDepthStencil = args->Flags.ZBuffer;
        r->wantsRenderTarget = args->Flags.RenderTarget; r->wantsAutogenMipmap = args->Flags.AutogenMipmap && (format->operations & FORMATOP_AUTOGENMIPMAP);
        r->notLockable = args->Flags.NotLockable; r->writeOnly = args->Flags.WriteOnly;
        r->dynamic = args->Flags.Dynamic; r->hasInitialData = info->pSysMem != NULL;
        hr = triton9CreateShadow(r, info);
        if (FAILED(hr)) goto fail;
        if (r->shadowSize > (SIZE_T)-1 - total) { hr = E_OUTOFMEMORY; goto fail; }
        total += r->shadowSize;
    }
    root->textureSurfaces = surfaces; root->surfaceCount = args->SurfCount;
    args->hResource = (HANDLE)root;
    return S_OK;
fail:
    for (i = 0; i < args->SurfCount; ++i) {
        if (surfaces[i]) {
            if (surfaces[i]->ownsShadow) HeapFree(GetProcessHeap(), 0, surfaces[i]->shadow);
            HeapFree(GetProcessHeap(), 0, surfaces[i]);
        }
    }
    HeapFree(GetProcessHeap(), 0, surfaces);
    return FAILED(hr) ? hr : D3DDDIERR_INVALIDCALL;
}

HRESULT APIENTRY
triton9CreateResource(HANDLE hDevice, D3DDDIARG_CREATERESOURCE *args)
{
    TRITON9_RESOURCE **surfaces;
    D3DDDIARG_CREATERESOURCE single;
    HRESULT hr;
    UINT i;

    if (args) {
        triton9DiagU32("TRITON9-CREATE-COUNT", args->SurfCount);
        triton9DiagU32("TRITON9-CREATE-FLAGS", args->Flags.Value);
        triton9DiagU32("TRITON9-CREATE-FORMAT", args->Format);
        triton9DiagU32("TRITON9-CREATE-MIPS", args->MipLevels);
    }

    if (args && (args->Flags.Texture || args->Flags.CubeMap || args->Flags.Volume) &&
        !args->Flags.SharedResource)
        return triton9CreateTextureResource((TRITON9_DEVICE *)hDevice, args);
    if (!args || args->SurfCount <= 1)
        return triton9CreateSingleResource(hDevice, args, FALSE);
    /* Flip chains are independent allocations, unlike the texture aliases
     * handled above. Preserve the standard primary sharing contract. */
    if (!args->pSurfList || args->SurfCount > 4 || !args->Flags.Primary ||
        !args->Flags.RenderTarget || args->Flags.Texture || args->Flags.SharedResource ||
        args->Flags.ZBuffer)
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
            while (i) {
                HRESULT cleanupHr = triton9DisposeFailedResource(
                    (TRITON9_DEVICE *)hDevice, surfaces[--i], hr);
                if (FAILED(cleanupHr)) hr = cleanupHr;
            }
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
    if (resource->textureSurfaces) {
        UINT i;
        for (i = 1; i < resource->surfaceCount; ++i) {
            if (!resource->textureSurfaces[i]) continue;
            result = triton9DestroyResource(hDevice, (HANDLE)resource->textureSurfaces[i]);
            if (FAILED(result)) return result;
            resource->textureSurfaces[i] = NULL;
        }
        HeapFree(GetProcessHeap(), 0, resource->textureSurfaces);
        resource->textureSurfaces = NULL; resource->surfaceCount = 1;
    }
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
        if (device->renderTarget == resource) { device->renderTarget = NULL; rebindOutputs = TRUE; }
        for (UINT rt = 0; rt < TRITON9_MAX_RENDER_TARGETS; ++rt) {
            if (device->renderTargets[rt] == resource) { device->renderTargets[rt] = NULL; rebindOutputs = TRUE; }
        }
        if (device->depthStencil == resource) {
            device->depthStencil = NULL;
            device->depthStencilStateDirty = TRUE;
            rebindOutputs = TRUE;
        }
        if (rebindOutputs && device->hostContext)
            (void)triton9BindOutputs(device);
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
                if (texture < TRITON9_MAX_PIXEL_SAMPLERS)
                    ID3D11DeviceContext1_PSSetShaderResources(device->hostContext, texture, 1, &nullShaderResource);
                else if (texture >= TRITON9_VERTEX_SAMPLER_BASE)
                    ID3D11DeviceContext1_VSSetShaderResources(device->hostContext,
                        texture - TRITON9_VERTEX_SAMPLER_BASE, 1, &nullShaderResource);
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
    if (resource->srgbShaderResourceView) ID3D11ShaderResourceView_Release(resource->srgbShaderResourceView);
    if (resource->srgbRenderTargetView) ID3D11RenderTargetView_Release(resource->srgbRenderTargetView);
    if (resource->resolveResource) ID3D11Resource_Release(resource->resolveResource);
    resource->srgbShaderResourceView = NULL; resource->srgbRenderTargetView = NULL;
    resource->resolveResource = NULL;
    if (resource->readbackQuery) ID3D11Query_Release(resource->readbackQuery);
    resource->readbackQuery = NULL;
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
    if (resource->systemMemorySnapshot)
        HeapFree(GetProcessHeap(), 0, resource->systemMemorySnapshot);
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
        !resource->shadow || resource->notLockable || resource->sampleCount > 1)
        return D3DDDIERR_INVALIDCALL;
    {
        TRITON9_RESOURCE *root = triton9ResourceRoot(resource);
        if (root->textureSurfaces)
            for (UINT i = 0; i < root->surfaceCount; ++i)
                if (root->textureSurfaces[i]->pendingRename)
                    return D3DDDIERR_INVALIDCALL;
    }
    systemMemory = triton9ResourceIsSystemMemory(resource);
    if (systemMemory && resource->width == 1280 && resource->height == 720) {
        static LONG readbackLockTraceCount;
        if (InterlockedIncrement(&readbackLockTraceCount) <= 12) {
            triton9DiagU32("TRITON9-READBACK-LOCK-TICK", GetTickCount());
            triton9DiagU32("TRITON9-READBACK-LOCK-FLAGS", args->Flags.Value);
        }
    }
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
        D3D11_BOX box = { (UINT)area->left, (UINT)area->top, 0,
            (UINT)area->right, (UINT)area->bottom, 1 };
        if (resource->isVolume || !triton9ResourceBoxValid(resource, &box))
            return D3DDDIERR_INVALIDCALL;
        offset = triton9ResourceByteOffset(resource, box.left, box.top, 0);
    } else if (args->Flags.BoxValid) {
        D3D11_BOX box = { args->Box.Left, args->Box.Top, args->Box.Front,
            args->Box.Right, args->Box.Bottom, args->Box.Back };
        if (!resource->isVolume || !triton9ResourceBoxValid(resource, &box))
            return D3DDDIERR_INVALIDCALL;
        offset = triton9ResourceByteOffset(resource, box.left, box.top, box.front);
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
    if (needsReadback) {
        EnterCriticalSection(&device->shaderLock);
        hr = args->Flags.DoNotWait ? triton9ReadbackShadowAsync(device, resource)
                                  : triton9ReadbackShadow(device, resource);
        LeaveCriticalSection(&device->shaderLock);
        if (FAILED(hr))
            return triton9MapDeviceFailure(device, hr);
    }
    resource->locked = TRUE;
    resource->lockRangeValid = args->Flags.RangeValid;
    resource->lockAreaValid = args->Flags.AreaValid;
    resource->lockBoxValid = args->Flags.BoxValid;
    if (resource->lockBoxValid) resource->lockBox = args->Box;
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
         !args->Flags.Discard && !args->Flags.NoOverwrite))
        return D3DDDIERR_INVALIDCALL;
    /* The runtime owns SYSTEMMEM storage. An asynchronous DISCARD needs a
     * separate snapshot until queued worker references have been consumed;
     * uploading that storage at UnlockAsync would overwrite those references.
     * Let the runtime fall back to synchronized Lock for this case. */
    if (args->Flags.Discard && triton9ResourceIsSystemMemory(resource))
        return E_NOTIMPL;
    if (args->Flags.Discard) {
        if (args->Flags.NotifyOnly || resource->isPrimary || resource->isShared ||
            resource->needsPresentAllocation ||
            !resource->shadow || resource->pendingRename)
            return D3DDDIERR_NOTAVAILABLE;
        TRITON9_RESOURCE *root = triton9ResourceRoot(resource);
        if (root->textureSurfaces) {
            for (UINT i = 0; i < root->surfaceCount; ++i)
                if (root->textureSurfaces[i]->locked || root->textureSurfaces[i]->pendingRename)
                    return D3DDDIERR_INVALIDCALL;
        }
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
    if (args->Flags.BoxValid) lock.Box = args->Box;
    else if (args->Flags.AreaValid) lock.Area = args->Area;
    else lock.Range = args->Range;
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

/* Discard changes one canonical texture allocation. Build every alias and view
 * before publishing any pointer; each published alias owns its COM reference. */
static HRESULT
triton9RenameTexture(TRITON9_DEVICE *device, TRITON9_RESOURCE *selected,
                     TRITON9_RENAME_COOKIE *cookie)
{
    TRITON9_RESOURCE *root = triton9ResourceRoot(selected);
    TRITON9_RESOURCE *copies = NULL, **table = NULL;
    UINT i, count = root->surfaceCount;
    HRESULT hr = E_OUTOFMEMORY;
    copies = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, count * sizeof(*copies));
    table = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, count * sizeof(*table));
    if (!copies || !table) goto done;
    for (i = 0; i < count; ++i) {
        copies[i] = *root->textureSurfaces[i]; table[i] = &copies[i];
        copies[i].systemMemorySnapshot = NULL;
        copies[i].systemMemorySnapshotSize = 0;
        copies[i].systemMemorySnapshotHost = NULL;
        copies[i].hostResource = NULL; copies[i].stagingResource = NULL;
        copies[i].resolveResource = NULL; copies[i].renderTargetView = NULL;
        copies[i].readbackQuery = NULL; copies[i].readbackPending = copies[i].readbackReady = FALSE;
        copies[i].srgbRenderTargetView = NULL; copies[i].depthStencilView = NULL;
        copies[i].shaderResourceView = NULL; copies[i].srgbShaderResourceView = NULL;
        copies[i].textureOwner = i ? &copies[0] : NULL;
        copies[i].textureSurfaces = i ? NULL : table;
        copies[i].hostReady = TRUE; copies[i].autogenGenerating = TRUE;
    }
    hr = triton9CreateHostTexture(device, &copies[0]);
    if (FAILED(hr)) goto done;
    for (i = 1; i < count; ++i) {
        copies[i].hostResource = copies[0].hostResource;
        ID3D11Resource_AddRef(copies[i].hostResource);
        copies[i].hostBindFlags = copies[0].hostBindFlags;
    }
    for (i = 0; i < count; ++i) {
        TRITON9_RESOURCE *old = root->textureSurfaces[i], *copy = &copies[i];
        ID3D11RenderTargetView *rtv;
        ID3D11DepthStencilView *dsv;
        ID3D11ShaderResourceView *srv;
        if (old == selected) hr = triton9UploadLockedShadow(device, copy);
        if (SUCCEEDED(hr) && old->renderTargetView) hr = triton9GetRenderTargetViewEx(device, copy, FALSE, &rtv);
        if (SUCCEEDED(hr) && old->srgbRenderTargetView) hr = triton9GetRenderTargetViewEx(device, copy, TRUE, &rtv);
        if (SUCCEEDED(hr) && old->depthStencilView) hr = triton9GetDepthStencilView(device, copy, &dsv);
        if (SUCCEEDED(hr) && old->shaderResourceView) hr = triton9GetShaderResourceViewEx(device, copy, FALSE, &srv);
        if (SUCCEEDED(hr) && old->srgbShaderResourceView) hr = triton9GetShaderResourceViewEx(device, copy, TRUE, &srv);
        if (FAILED(hr)) goto done;
    }
    for (i = 0; i < count; ++i) {
        TRITON9_RESOURCE *old = root->textureSurfaces[i], *copy = &copies[i];
        triton9ReleaseResourceViews(old);
        if (old->hostResource) ID3D11Resource_Release(old->hostResource);
        old->hostResource = copy->hostResource; copy->hostResource = NULL;
        old->hostBindFlags = copy->hostBindFlags; old->hostReady = TRUE;
        old->renderTargetView = copy->renderTargetView; copy->renderTargetView = NULL;
        old->srgbRenderTargetView = copy->srgbRenderTargetView; copy->srgbRenderTargetView = NULL;
        old->depthStencilView = copy->depthStencilView; copy->depthStencilView = NULL;
        old->shaderResourceView = copy->shaderResourceView; copy->shaderResourceView = NULL;
        old->srgbShaderResourceView = copy->srgbShaderResourceView; copy->srgbShaderResourceView = NULL;
    }
    triton9ResourceWritten(root);
    /* All draw-time shader bindings are rebuilt from device->textures. */
    hr = triton9BindOutputs(device);
done:
    if (copies) for (i = 0; i < count; ++i) {
        triton9ReleaseResourceViews(&copies[i]);
        if (copies[i].hostResource) ID3D11Resource_Release(copies[i].hostResource);
    }
    if (copies) HeapFree(GetProcessHeap(), 0, copies);
    if (table) HeapFree(GetProcessHeap(), 0, table);
    selected->pendingRename = NULL;
    triton9ClearLockRegion(selected);
    HeapFree(GetProcessHeap(), 0, cookie);
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

    if (triton9ResourceRoot(resource)->textureSurfaces) {
        EnterCriticalSection(&device->shaderLock);
        hr = triton9RenameTexture(device, resource, cookie);
        LeaveCriticalSection(&device->shaderLock);
        return triton9MapDeviceFailure(device, hr);
    }
    ZeroMemory(&replacement, sizeof(replacement));
    EnterCriticalSection(&device->shaderLock);
    hr = triton9CloneHostResource(device, resource, &newHost);
    if (FAILED(hr))
        goto done;
    replacement = *resource;
    replacement.systemMemorySnapshot = NULL;
    replacement.systemMemorySnapshotSize = 0;
    replacement.systemMemorySnapshotHost = NULL;
    replacement.hostResource = newHost;
    replacement.stagingResource = NULL;
    replacement.renderTargetView = NULL;
    replacement.depthStencilView = NULL;
    replacement.shaderResourceView = NULL;
    replacement.srgbShaderResourceView = NULL;
    replacement.srgbRenderTargetView = NULL;
    replacement.resolveResource = NULL;
    replacement.readbackQuery = NULL;
    replacement.readbackPending = replacement.readbackReady = FALSE;
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
    retired.systemMemorySnapshot = NULL;
    retired.systemMemorySnapshotSize = 0;
    retired.systemMemorySnapshotHost = NULL;
    resource->hostResource = replacement.hostResource;
    resource->stagingResource = replacement.stagingResource;
    resource->renderTargetView = replacement.renderTargetView;
    resource->depthStencilView = replacement.depthStencilView;
    resource->shaderResourceView = replacement.shaderResourceView;
    resource->srgbShaderResourceView = replacement.srgbShaderResourceView;
    resource->srgbRenderTargetView = replacement.srgbRenderTargetView;
    resource->resolveResource = replacement.resolveResource;
    resource->readbackQuery = NULL;
    resource->readbackPending = resource->readbackReady = FALSE;
    triton9ResourceWritten(resource);
    replacement.srgbShaderResourceView = NULL;
    replacement.srgbRenderTargetView = NULL;
    replacement.resolveResource = NULL;
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
            if (texture < TRITON9_MAX_PIXEL_SAMPLERS)
                ID3D11DeviceContext1_PSSetShaderResources(device->hostContext, texture, 1, &resource->shaderResourceView);
            else if (texture >= TRITON9_VERTEX_SAMPLER_BASE)
                ID3D11DeviceContext1_VSSetShaderResources(device->hostContext,
                    texture - TRITON9_VERTEX_SAMPLER_BASE, 1, &resource->shaderResourceView);
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
            hr = triton9PollHostDevice(device);
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

HRESULT
triton9CompleteRedirectedPresent(TRITON9_DEVICE *device)
{
    HRESULT hr;

    if (!device || !device->hostDevice || !device->hostContext)
        return E_INVALIDARG;
    if (device->deviceLost)
        return D3DDDIERR_DEVICEREMOVED;

    EnterCriticalSection(&device->shaderLock);
    hr = triton9WaitForPresentGpuCompletion(device);
    if (SUCCEEDED(hr)) {
        /* Vista supplies the present-history token only while calling the
         * final redirected Blt and its following Flush. A busy Neptune ring
         * consumes proxy calls without entering RenderCb, so flushing that
         * ring alone can lose the token. Submit an explicit runtime marker
         * inside this callback, after the copied pixels are complete. */
        if (!tritonSharedBridgeDrain(device->hostContext,
                                    TRITON9_HOST_DRAIN_TIMEOUT_MS)) {
            device->deviceLost = TRUE;
            hr = D3DDDIERR_DEVICEREMOVED;
        } else {
            hr = triton9CheckHostDevice(device);
        }
    }
    LeaveCriticalSection(&device->shaderLock);
    return hr;
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

    if (device->deviceLost)
        return D3DDDIERR_DEVICEREMOVED;
    if (!device->presentConsumptionEvent)
        device->presentConsumptionEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    event = device->presentConsumptionEvent;
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
    return triton9MapDeviceFailure(device, hr);
}

static BOOL triton9PresentProfileEnabled(TRITON9_DEVICE *device)
{
    if (!device->presentProfileState) {
        char value[2] = {0};
        DWORD length = GetEnvironmentVariableA("TRITON9_PRESENT_PROFILE", value, sizeof(value));
        device->presentProfileState = length == 1 && value[0] == '1' ? 2 : 1;
    }
    return device->presentProfileState == 2;
}

static void triton9PresentProfileRecord(TRITON9_DEVICE *device, LARGE_INTEGER *ticks)
{
    char line[256];
    LARGE_INTEGER frequency;
    UINT i;
    for (i = 0; i < 4; ++i) {
        UINT64 elapsed = ticks[i + 1].QuadPart - ticks[i].QuadPart;
        device->presentProfileTicks[i] += elapsed;
        if (elapsed > device->presentProfileMaxTicks[i])
            device->presentProfileMaxTicks[i] = elapsed;
    }
    if (++device->presentProfileFrames != 120)
        return;
    QueryPerformanceFrequency(&frequency);
    for (i = 0; i < 4; ++i) {
        snprintf(line, sizeof(line), "TRITON9-PERF pid=%lu stage=%u frames=120 mean_us=%.3f max_us=%.3f\n",
            (unsigned long)GetCurrentProcessId(), i,
            device->presentProfileTicks[i] * 1000000.0 / frequency.QuadPart / 120,
            device->presentProfileMaxTicks[i] * 1000000.0 / frequency.QuadPart);
        triton9Diag(line);
        device->presentProfileTicks[i] = device->presentProfileMaxTicks[i] = 0;
    }
    device->presentProfileFrames = 0;
}

HRESULT APIENTRY
triton9Present(HANDLE hDevice, const D3DDDIARG_PRESENT *args)
{
    TRITON9_DEVICE *device = (TRITON9_DEVICE *)hDevice;
    TRITON9_RESOURCE *src;
    TRITON9_RESOURCE *dst;
    D3DDDICB_PRESENT callback;
    HRESULT hr;
    LARGE_INTEGER ticks[5];
    BOOL profile;
    LARGE_INTEGER entryTicks = {0};

    triton9Diag("TRITON9-PRESENT enter\n");
    if (!device || !args || !(src = (TRITON9_RESOURCE *)args->hSrcResource) ||
        !triton9ResourceBelongsToDevice(device, src))
        return E_INVALIDARG;
    if (triton9TraceEnabled(device)) QueryPerformanceCounter(&entryTicks);
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
    if (src->sampleCount > 1) {
        ID3D11Resource *resolved; UINT index;
        hr = triton9ResolveResource(device, src, &resolved, &index);
        if (FAILED(hr)) { LeaveCriticalSection(&device->shaderLock); return hr; }
    }
    if (entryTicks.QuadPart) triton9TraceBegin(device, entryTicks.QuadPart, args->Flags.Value, TT_PRESENT_BEGIN);
    else device->traceFrame = 0;
    profile = triton9PresentProfileEnabled(device);
    if (profile) QueryPerformanceCounter(&ticks[0]);
    triton9TraceGpuFinish(device);
    triton9TraceEvent(device, TT_GPU_WAIT_BEGIN, 0, 0, 0);
    hr = triton9WaitForPresentGpuCompletion(device);
    triton9TraceEvent(device, TT_GPU_WAIT_END, (UINT)hr, device->presentFenceValue, 0);
    if (profile) QueryPerformanceCounter(&ticks[1]);
    if (FAILED(hr)) {
        triton9TraceEvent(device, TT_PRESENT_END, (UINT)hr, 0, 0);
        LeaveCriticalSection(&device->shaderLock);
        return hr;
    }
    /* Keep this post-completion drain for the existing KMD/transport ordering
     * contract.  It does not provide the GPU completion guarantee above. */
    triton9TraceEvent(device, TT_DRAIN_BEGIN, 0, 0, 0);
    if (!tritonSharedBridgeDrain(device->hostContext,
                                 TRITON9_HOST_DRAIN_TIMEOUT_MS)) {
        device->deviceLost = TRUE;
        triton9TraceEvent(device, TT_PRESENT_END, D3DDDIERR_DEVICEREMOVED, 0, 0);
        LeaveCriticalSection(&device->shaderLock);
        return D3DDDIERR_DEVICEREMOVED;
    }
    hr = triton9CheckHostDevice(device);
    if (FAILED(hr)) {
        triton9TraceEvent(device, TT_PRESENT_END, (UINT)hr, 0, 0);
        LeaveCriticalSection(&device->shaderLock);
        return hr;
    }
    hr = triton9EnsureKernelContext(device);
    if (FAILED(hr)) {
        triton9TraceEvent(device, TT_PRESENT_END, (UINT)hr, 0, 0);
        LeaveCriticalSection(&device->shaderLock);
        return hr;
    }
    ZeroMemory(&callback, sizeof(callback));
    callback.hSrcAllocation = src->hKMAllocation;
    callback.hDstAllocation = dst ? dst->hKMAllocation : 0;
    callback.hContext = device->hKMContext;
    triton9TraceEvent(device, TT_DRAIN_END, 0, 0, 0);
    if (profile) QueryPerformanceCounter(&ticks[2]);
    triton9DiagU32("TRITON9-PRESENT-FLAGS", args->Flags.Value);
    triton9DiagU32("TRITON9-PRESENT-SRC", callback.hSrcAllocation);
    triton9DiagU32("TRITON9-PRESENT-SRC-STANDARD", src->isStandardPrimary);
    triton9DiagU32("TRITON9-PRESENT-DST", callback.hDstAllocation);
    triton9TraceEvent(device, TT_CALLBACK_BEGIN, callback.hSrcAllocation, callback.hDstAllocation, 0);
    hr = triton9MapDeviceFailure(device,
        device->callbacks.pfnPresentCb(device->hRTDevice, &callback));
    triton9TraceEvent(device, TT_CALLBACK_END, (UINT)hr, 0, 0);
    if (profile) QueryPerformanceCounter(&ticks[3]);
    if (SUCCEEDED(hr)) {
        triton9TraceEvent(device, TT_CONSUME_BEGIN, 0, 0, 0);
        hr = triton9WaitForPresentConsumption(device);
        triton9TraceEvent(device, TT_CONSUME_END, (UINT)hr, 0, 0);
    }
    if (profile && SUCCEEDED(hr)) {
        QueryPerformanceCounter(&ticks[4]);
        triton9PresentProfileRecord(device, ticks);
    }
    if (SUCCEEDED(hr)) triton9TraceGpuResolve(device);
    triton9TraceEvent(device, TT_PRESENT_END, (UINT)hr, 0, 0);
    if (SUCCEEDED(hr)) triton9TraceGpuStart(device);
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
        resource->sampleCount = resource->arraySize = resource->exposedMipLevels = 1;
        resource->blockWidth = resource->blockHeight = 1;
        resource->bytesPerBlock = format->bytesPerBlock;
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
        (options->primary && options->format != DXGI_FORMAT_B8G8R8A8_UNORM &&
         options->format != DXGI_FORMAT_B8G8R8X8_UNORM) ||
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
    resource->sampleCount = resource->arraySize = resource->exposedMipLevels = 1;
    resource->blockWidth = resource->blockHeight = 1;
    resource->bytesPerBlock = format->bytesPerBlock;
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
