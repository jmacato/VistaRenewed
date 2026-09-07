/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Minimal output-merger operations used by Vista's D3D9 runtime while the
 * full state cache is brought up. These functions handle only the shapes
 * they can execute exactly and return D3DDDIERR_NOTAVAILABLE for the rest.
 */

#ifdef TRITON9_CLEAR_CONTRACT_PORTABLE
#include <stdint.h>
#include <stddef.h>

typedef int32_t LONG;
typedef uint32_t UINT;
typedef int BOOL;
typedef struct tagRECT {
    LONG left;
    LONG top;
    LONG right;
    LONG bottom;
} RECT;
#define FALSE 0
#define TRUE 1
#else
#include "triton9.h"

HRESULT npt_dispatch_clear_depth_stencil_rects(
    void *context, void *dsv, uint32_t clear_flags, float depth,
    uint8_t stencil, uint32_t rect_count, const int32_t *rect_ltrb);
#endif

#include <limits.h>
#include <string.h>

/* Vista's D3D9 DDI documents this flag although the SDK d3d9types.h that
 * ships with the cross toolchain does not publish the name. */
#ifndef D3DCLEAR_COMPUTERECTS
#define D3DCLEAR_COMPUTERECTS 0x00000008u
#endif

enum triton9_clear_contract_result {
    TRITON9_CLEAR_CONTRACT_OK = 0,
    TRITON9_CLEAR_CONTRACT_INVALID_ARGUMENT = 1,
    TRITON9_CLEAR_CONTRACT_INSUFFICIENT_CAPACITY = 2,
};

static BOOL
triton9ClearRectIsValid(const RECT *rect)
{
    return rect && rect->right > rect->left && rect->bottom > rect->top;
}

static BOOL
triton9IntersectClearRect(RECT *rect, const RECT *clip)
{
    if (rect->left < clip->left)
        rect->left = clip->left;
    if (rect->top < clip->top)
        rect->top = clip->top;
    if (rect->right > clip->right)
        rect->right = clip->right;
    if (rect->bottom > clip->bottom)
        rect->bottom = clip->bottom;
    return triton9ClearRectIsValid(rect);
}

/* Normalize the four PFND3DDDI_CLEAR rectangle shapes.  The caller supplies
 * destination bounds because target and depth resources can differ in size.
 * A zero-count COMPUTERECTS clear uses the viewport but not the scissor.
 * Positive COMPUTERECTS input uses both the viewport and enabled scissor. */
static enum triton9_clear_contract_result
triton9NormalizeClearRects(UINT flags, UINT rectCount, const RECT *rects,
                           const RECT *viewport, BOOL scissorEnabled,
                           const RECT *scissor, const RECT *bounds,
                           RECT *normalized, UINT capacity,
                           UINT *normalizedCount)
{
    const BOOL computeRects = (flags & D3DCLEAR_COMPUTERECTS) != 0;
    UINT sourceCount = rectCount;
    UINT index;
    UINT outputCount = 0;

    if (!bounds || !normalizedCount || !triton9ClearRectIsValid(bounds) ||
        (rectCount && !rects) || (computeRects && !viewport) ||
        (computeRects && scissorEnabled && !scissor))
        return TRITON9_CLEAR_CONTRACT_INVALID_ARGUMENT;

    *normalizedCount = 0;
    if (!rectCount && !computeRects)
        return TRITON9_CLEAR_CONTRACT_OK;

    if (!rectCount)
        sourceCount = 1;
    if (!normalized || capacity < sourceCount)
        return TRITON9_CLEAR_CONTRACT_INSUFFICIENT_CAPACITY;

    for (index = 0; index < sourceCount; ++index) {
        RECT rect = rectCount ? rects[index] : *viewport;

        if (!triton9ClearRectIsValid(&rect))
            return TRITON9_CLEAR_CONTRACT_INVALID_ARGUMENT;

        if (computeRects) {
            if (!triton9IntersectClearRect(&rect, viewport))
                continue;
            if (rectCount && scissorEnabled &&
                !triton9IntersectClearRect(&rect, scissor))
                continue;
            if (!triton9IntersectClearRect(&rect, bounds))
                continue;
        } else {
            if (rect.left < bounds->left || rect.top < bounds->top ||
                rect.right > bounds->right || rect.bottom > bounds->bottom)
                return TRITON9_CLEAR_CONTRACT_INVALID_ARGUMENT;
        }
        normalized[outputCount++] = rect;
    }

    *normalizedCount = outputCount;
    return TRITON9_CLEAR_CONTRACT_OK;
}

static BOOL
triton9ClearRectsCoverResource(const RECT *rects, UINT rectCount,
                               UINT width, UINT height)
{
    return rectCount == 1 && rects && rects[0].left == 0 &&
           rects[0].top == 0 && rects[0].right >= 0 &&
           rects[0].bottom >= 0 && (UINT)rects[0].right == width &&
           (UINT)rects[0].bottom == height;
}

static void
triton9ConvertClearColor(uint32_t packed, float color[4])
{
    color[0] = ((packed >> 16) & 0xffu) / 255.0f;
    color[1] = ((packed >> 8) & 0xffu) / 255.0f;
    color[2] = (packed & 0xffu) / 255.0f;
    color[3] = ((packed >> 24) & 0xffu) / 255.0f;
}

#ifndef TRITON9_CLEAR_CONTRACT_PORTABLE

BOOL
triton9HasCompleteD24S8ClearContract(void)
{
    /* This symbol is the caps owner's link dependency on this complete path.
     * Do not move it to a header or a command-availability macro. */
    return TRUE;
}

static HRESULT
triton9BindOutputs(TRITON9_DEVICE *device)
{
    ID3D11RenderTargetView *renderTarget = NULL;
    ID3D11DepthStencilView *depthStencil = NULL;
    HRESULT hr;

    if (!device || !device->hostContext)
        return E_INVALIDARG;
    if (device->renderTarget) {
        hr = triton9GetRenderTargetView(device, device->renderTarget,
                                        &renderTarget);
        if (FAILED(hr))
            return hr;
    }
    if (device->depthStencil) {
        hr = triton9GetDepthStencilView(device, device->depthStencil,
                                        &depthStencil);
        if (FAILED(hr))
            return hr;
    }
    ID3D11DeviceContext1_OMSetRenderTargets(device->hostContext,
                                             renderTarget ? 1 : 0,
                                             renderTarget ? &renderTarget : NULL,
                                             depthStencil);
    return triton9CheckHostDevice(device);
}

/* The D3D11 proxy cannot bind a subresource as both SRV and RTV.  D3D9
 * applications can change that binding order, so clear the old SRV slots
 * before binding the render target. */
static UINT
triton9UnbindTextureResource(TRITON9_DEVICE *device, TRITON9_RESOURCE *resource)
{
    ID3D11ShaderResourceView *nullView = NULL;
    UINT stageMask = 0;
    UINT stage;

    if (!device || !resource)
        return 0;
    for (stage = 0; stage < TRITON9_MAX_TEXTURE_STAGES; ++stage) {
        if (device->textures[stage] == resource) {
            ID3D11DeviceContext1_PSSetShaderResources(device->hostContext, stage,
                                                       1, &nullView);
            device->textures[stage] = NULL;
            stageMask |= 1u << stage;
        }
    }
    return stageMask;
}

/* The caller holds shaderLock. Restore texture state if a failed output bind
 * rolls back the D3D9 state change. */
static void
triton9RestoreTextureResource(TRITON9_DEVICE *device,
                              TRITON9_RESOURCE *resource, UINT stageMask)
{
    UINT stage;

    if (!device || !resource)
        return;
    for (stage = 0; stage < TRITON9_MAX_TEXTURE_STAGES; ++stage) {
        if (stageMask & (1u << stage)) {
            device->textures[stage] = resource;
            ID3D11DeviceContext1_PSSetShaderResources(
                device->hostContext, stage, 1, &resource->shaderResourceView);
        }
    }
}

static HRESULT
triton9ValidateColorRects(const RECT *rects, UINT rectCount,
                          const TRITON9_RESOURCE *resource)
{
    UINT index;

    if (!resource || (rectCount && !rects))
        return E_INVALIDARG;
    for (index = 0; index < rectCount; ++index) {
        const RECT *rect = &rects[index];

        if (rect->left < 0 || rect->top < 0 || rect->right <= rect->left ||
            rect->bottom <= rect->top || (UINT)rect->right > resource->width ||
            (UINT)rect->bottom > resource->height)
            return D3DDDIERR_INVALIDCALL;
    }
    return S_OK;
}

static void
triton9ClearRenderTargetRects(TRITON9_DEVICE *device,
                              ID3D11RenderTargetView *view,
                              const FLOAT color[4], UINT rectCount,
                              const RECT *rects)
{
    if (rectCount) {
        /* RECT and D3D11_RECT both hold four signed LONG coordinates.  The
         * preceding validation also prevents the proxy from receiving an
         * out-of-bounds rectangle. */
        ID3D11DeviceContext1_ClearView(device->hostContext, (ID3D11View *)view,
                                       color, (const D3D11_RECT *)rects,
                                       rectCount);
    } else {
        ID3D11DeviceContext1_ClearRenderTargetView(device->hostContext, view,
                                                    color);
    }
}

static void
triton9ClearD16Rects(TRITON9_DEVICE *device, ID3D11DepthStencilView *view,
                     FLOAT depth, UINT rectCount, const RECT *rects)
{
    const FLOAT color[4] = { depth, 0.0f, 0.0f, 0.0f };

    /* ClearView supports a depth-only DSV.  The color[0] value becomes the
     * D16 depth value.  The caller validates the rectangles and format. */
    ID3D11DeviceContext1_ClearView(device->hostContext, (ID3D11View *)view,
                                   color, (const D3D11_RECT *)rects, rectCount);
}

static void
triton9CopySystemSurfaceRows(TRITON9_RESOURCE *destination, LONG destinationX,
                             LONG destinationY, TRITON9_RESOURCE *source,
                             const RECT *sourceRect)
{
    SIZE_T rowBytes = (SIZE_T)(sourceRect->right - sourceRect->left) *
                      source->bytesPerPixel;
    LONG rowCount = sourceRect->bottom - sourceRect->top;
    LONG first = 0;
    LONG end = rowCount;
    LONG step = 1;
    LONG row;

    if (source == destination && destinationY > sourceRect->top &&
        destinationY < sourceRect->bottom) {
        first = rowCount - 1;
        end = -1;
        step = -1;
    }
    for (row = first; row != end; row += step) {
        memmove(destination->shadow +
                    (SIZE_T)(destinationY + row) * destination->pitch +
                    (SIZE_T)destinationX * destination->bytesPerPixel,
                source->shadow +
                    (SIZE_T)(sourceRect->top + row) * source->pitch +
                    (SIZE_T)sourceRect->left * source->bytesPerPixel,
                rowBytes);
    }
}

HRESULT APIENTRY
triton9SetRenderTarget(HANDLE hDevice, const D3DDDIARG_SETRENDERTARGET *args)
{
    TRITON9_DEVICE *device = (TRITON9_DEVICE *)hDevice;
    TRITON9_RESOURCE *resource;
    TRITON9_RESOURCE *previous;
    UINT unboundTextureStages = 0;
    HRESULT hr;

    if (!device || !args || args->RenderTargetIndex)
        return E_INVALIDARG;
    if (device->deviceLost)
        return D3DDDIERR_DEVICEREMOVED;
    resource = triton9ResourceSurface((TRITON9_RESOURCE *)args->hRenderTarget,
                                      args->SubResourceIndex);
    if ((args->hRenderTarget && !resource) || (!args->hRenderTarget && args->SubResourceIndex))
        return D3DDDIERR_INVALIDCALL;
    if (resource) {
        if (!triton9ResourceBelongsToDevice(device, resource))
            return D3DDDIERR_INVALIDCALL;
        /* Vista owns SYSTEMMEM bytes.  Do not create a second representation
         * that rendering can make newer than the runtime's pSysMem buffer. */
        if (triton9ResourceIsSystemMemory(resource))
            return D3DDDIERR_INVALIDCALL;
        hr = triton9EnsureResourceHost(device, resource);
        if (FAILED(hr))
            return hr;
    }

    EnterCriticalSection(&device->shaderLock);
    if (resource) {
        ID3D11RenderTargetView *view;
        hr = triton9GetRenderTargetView(device, resource, &view);
        if (FAILED(hr)) {
            LeaveCriticalSection(&device->shaderLock);
            return hr;
        }
        unboundTextureStages = triton9UnbindTextureResource(device, resource);
    }
    previous = device->renderTarget;
    device->renderTarget = resource;
    hr = triton9BindOutputs(device);
    if (FAILED(hr)) {
        device->renderTarget = previous;
        /* Keep the host pipeline consistent with the D3D9 state that the UMD
         * reports after this callback fails. */
        (void)triton9BindOutputs(device);
        triton9RestoreTextureResource(device, resource,
                                      unboundTextureStages);
    }
    LeaveCriticalSection(&device->shaderLock);
    return triton9MapDeviceFailure(device, hr);
}

HRESULT APIENTRY
triton9SetDepthStencil(HANDLE hDevice, const D3DDDIARG_SETDEPTHSTENCIL *args)
{
    TRITON9_DEVICE *device = (TRITON9_DEVICE *)hDevice;
    TRITON9_RESOURCE *resource;
    TRITON9_RESOURCE *previous;
    HRESULT hr;

    if (!device || !args)
        return E_INVALIDARG;
    if (device->deviceLost)
        return D3DDDIERR_DEVICEREMOVED;
    resource = (TRITON9_RESOURCE *)args->hZBuffer;
    if (resource) {
        if (!triton9ResourceBelongsToDevice(device, resource))
            return D3DDDIERR_INVALIDCALL;
        if (triton9ResourceIsSystemMemory(resource))
            return D3DDDIERR_INVALIDCALL;
        hr = triton9EnsureResourceHost(device, resource);
        if (FAILED(hr))
            return hr;
    }

    EnterCriticalSection(&device->shaderLock);
    if (resource) {
        ID3D11DepthStencilView *view;
        hr = triton9GetDepthStencilView(device, resource, &view);
        if (FAILED(hr)) {
            LeaveCriticalSection(&device->shaderLock);
            return hr;
        }
    }
    previous = device->depthStencil;
    device->depthStencil = resource;
    hr = triton9BindOutputs(device);
    if (FAILED(hr)) {
        device->depthStencil = previous;
        /* BindOutputs can fail after it changed OM state. Restore the old
         * D3D9 outputs before returning the original host error. */
        (void)triton9BindOutputs(device);
    }
    LeaveCriticalSection(&device->shaderLock);
    return triton9MapDeviceFailure(device, hr);
}

static HRESULT
triton9BuildClearLimits(const TRITON9_DEVICE *device,
                        const TRITON9_RESOURCE *resource, RECT *bounds,
                        RECT *viewport, BOOL *scissorEnabled, RECT *scissor)
{
    double right;
    double bottom;

    if (!device || !resource || !bounds || !viewport || !scissorEnabled ||
        !scissor || !resource->width || !resource->height ||
        resource->width > INT32_MAX || resource->height > INT32_MAX)
        return E_INVALIDARG;

    bounds->left = 0;
    bounds->top = 0;
    bounds->right = (LONG)resource->width;
    bounds->bottom = (LONG)resource->height;

    if (device->viewportSet) {
        right = (double)device->viewport.TopLeftX + device->viewport.Width;
        bottom = (double)device->viewport.TopLeftY + device->viewport.Height;
        if (device->viewport.TopLeftX != device->viewport.TopLeftX ||
            device->viewport.TopLeftY != device->viewport.TopLeftY ||
            device->viewport.Width != device->viewport.Width ||
            device->viewport.Height != device->viewport.Height ||
            device->viewport.TopLeftX < 0.0f ||
            device->viewport.TopLeftY < 0.0f ||
            device->viewport.Width <= 0.0f ||
            device->viewport.Height <= 0.0f || right > INT32_MAX ||
            bottom > INT32_MAX)
            return D3DDDIERR_INVALIDCALL;
        viewport->left = (LONG)device->viewport.TopLeftX;
        viewport->top = (LONG)device->viewport.TopLeftY;
        viewport->right = (LONG)right;
        viewport->bottom = (LONG)bottom;
    } else {
        *viewport = *bounds;
    }

    *scissorEnabled =
        device->renderStates[D3DRS_SCISSORTESTENABLE] != FALSE;
    *scissor = device->scissorSet ? device->scissorRect : *bounds;
    if (*scissorEnabled && !triton9ClearRectIsValid(scissor))
        return D3DDDIERR_INVALIDCALL;
    return S_OK;
}

static void
triton9TraceClearEntry(HANDLE hDevice, const D3DDDIARG_CLEAR *args,
                       UINT rectCount, const RECT *rects)
{
    const uint64_t deviceValue = (uint64_t)(uintptr_t)hDevice;
    const uint64_t argsValue = (uint64_t)(uintptr_t)args;
    const uint64_t rectValue = (uint64_t)(uintptr_t)rects;

    triton9ProofDiagU32("TRITON9-PROOF-CLEAR-ENTRY-FLAGS",
                        args ? args->Flags : UINT32_MAX);
    triton9ProofDiagU32("TRITON9-PROOF-CLEAR-ENTRY-RECTS", rectCount);
    triton9ProofDiagU32("TRITON9-PROOF-CLEAR-ENTRY-DEVICE-LO",
                        (DWORD)deviceValue);
    triton9ProofDiagU32("TRITON9-PROOF-CLEAR-ENTRY-DEVICE-HI",
                        (DWORD)(deviceValue >> 32));
    triton9ProofDiagU32("TRITON9-PROOF-CLEAR-ENTRY-ARGS-LO",
                        (DWORD)argsValue);
    triton9ProofDiagU32("TRITON9-PROOF-CLEAR-ENTRY-ARGS-HI",
                        (DWORD)(argsValue >> 32));
    triton9ProofDiagU32("TRITON9-PROOF-CLEAR-ENTRY-RECTPTR-LO",
                        (DWORD)rectValue);
    triton9ProofDiagU32("TRITON9-PROOF-CLEAR-ENTRY-RECTPTR-HI",
                        (DWORD)(rectValue >> 32));
}

static HRESULT
triton9TraceClearReturn(DWORD stage, HRESULT hr)
{
    triton9ProofDiagU32("TRITON9-PROOF-CLEAR-RETURN-STAGE", stage);
    triton9ProofDiagU32("TRITON9-PROOF-CLEAR-RETURN-HR", (DWORD)hr);
    return hr;
}

HRESULT APIENTRY
triton9Clear(HANDLE hDevice, const D3DDDIARG_CLEAR *args, UINT rectCount,
             const RECT *rects)
{
    TRITON9_DEVICE *device = (TRITON9_DEVICE *)hDevice;
    RECT stackRects[16];
    RECT *normalized = stackRects;
    UINT normalizedCapacity = rectCount ? rectCount : 1;
    BOOL heapRects = FALSE;
    FLOAT color[4];
    HRESULT hr = S_OK;

    triton9TraceClearEntry(hDevice, args, rectCount, rects);
    if (!device || !args || (rectCount && !rects))
        return triton9TraceClearReturn(1, E_INVALIDARG);
    if (device->deviceLost)
        return triton9TraceClearReturn(2, D3DDDIERR_DEVICEREMOVED);
    if (!(args->Flags & (D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER |
                         D3DCLEAR_STENCIL)) ||
        args->Flags & ~(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER |
                        D3DCLEAR_STENCIL | D3DCLEAR_COMPUTERECTS)) {
        triton9ProofDiagU32("TRITON9-PROOF-CLEAR-BADFLAGS", args->Flags);
        return triton9TraceClearReturn(4, D3DDDIERR_INVALIDCALL);
    }

    if (!rectCount && !(args->Flags & D3DCLEAR_COMPUTERECTS))
        return triton9TraceClearReturn(5, S_OK);
    hr = triton9EnsureHostDevice(device);
    if (FAILED(hr))
        return triton9TraceClearReturn(3, hr);
    if ((args->Flags & D3DCLEAR_ZBUFFER) &&
        (args->FillDepth != args->FillDepth || args->FillDepth < 0.0f ||
         args->FillDepth > 1.0f))
        return triton9TraceClearReturn(6, D3DDDIERR_INVALIDCALL);
    if (normalizedCapacity > ARRAYSIZE(stackRects)) {
        if ((SIZE_T)normalizedCapacity > ((SIZE_T)-1) / sizeof(*normalized))
            return triton9TraceClearReturn(7, E_OUTOFMEMORY);
        normalized = HeapAlloc(GetProcessHeap(), 0,
                               (SIZE_T)normalizedCapacity * sizeof(*normalized));
        if (!normalized)
            return triton9TraceClearReturn(7, E_OUTOFMEMORY);
        heapRects = TRUE;
    }

    triton9ConvertClearColor(args->FillColor, color);

    EnterCriticalSection(&device->shaderLock);
    if (args->Flags & D3DCLEAR_TARGET) {
        ID3D11RenderTargetView *view;
        RECT bounds;
        RECT viewport;
        RECT scissor;
        BOOL scissorEnabled;
        UINT normalizedCount = 0;
        enum triton9_clear_contract_result contractResult;

        if (!device->renderTarget)
            hr = D3DDDIERR_INVALIDCALL;
        else
            hr = triton9BuildClearLimits(device, device->renderTarget, &bounds,
                                         &viewport, &scissorEnabled, &scissor);
        if (SUCCEEDED(hr)) {
            contractResult = triton9NormalizeClearRects(
                args->Flags, rectCount, rects, &viewport, scissorEnabled,
                &scissor, &bounds, normalized, normalizedCapacity,
                &normalizedCount);
            if (contractResult != TRITON9_CLEAR_CONTRACT_OK)
                hr = contractResult ==
                             TRITON9_CLEAR_CONTRACT_INSUFFICIENT_CAPACITY
                         ? E_OUTOFMEMORY
                         : D3DDDIERR_INVALIDCALL;
        }
        if (SUCCEEDED(hr))
            hr = triton9GetRenderTargetView(device, device->renderTarget, &view);
        if (SUCCEEDED(hr) && normalizedCount) {
            if (triton9ClearRectsCoverResource(
                    normalized, normalizedCount, device->renderTarget->width,
                    device->renderTarget->height))
                triton9ClearRenderTargetRects(device, view, color, 0, NULL);
            else
                triton9ClearRenderTargetRects(device, view, color,
                                              normalizedCount, normalized);
        }
    }
    if (SUCCEEDED(hr) && (args->Flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL))) {
        ID3D11DepthStencilView *view;
        RECT bounds;
        RECT viewport;
        RECT scissor;
        BOOL scissorEnabled;
        UINT normalizedCount = 0;
        UINT flags = 0;
        enum triton9_clear_contract_result contractResult;

        if (!device->depthStencil)
            hr = D3DDDIERR_INVALIDCALL;
        else if (device->depthStencil->format != D3DDDIFMT_D16 &&
                 device->depthStencil->format != D3DDDIFMT_D24S8)
            hr = D3DDDIERR_NOTAVAILABLE;
        if (SUCCEEDED(hr) && (args->Flags & D3DCLEAR_STENCIL) &&
            device->depthStencil->format != D3DDDIFMT_D24S8)
            hr = D3DDDIERR_INVALIDCALL;
        if (args->Flags & D3DCLEAR_ZBUFFER)
            flags |= D3D11_CLEAR_DEPTH;
        if (args->Flags & D3DCLEAR_STENCIL)
            flags |= D3D11_CLEAR_STENCIL;
        if (SUCCEEDED(hr))
            hr = triton9BuildClearLimits(device, device->depthStencil,
                                         &bounds, &viewport, &scissorEnabled,
                                         &scissor);
        if (SUCCEEDED(hr)) {
            contractResult = triton9NormalizeClearRects(
                args->Flags, rectCount, rects, &viewport, scissorEnabled,
                &scissor, &bounds, normalized, normalizedCapacity,
                &normalizedCount);
            if (contractResult != TRITON9_CLEAR_CONTRACT_OK)
                hr = contractResult ==
                             TRITON9_CLEAR_CONTRACT_INSUFFICIENT_CAPACITY
                         ? E_OUTOFMEMORY
                         : D3DDDIERR_INVALIDCALL;
        }
        if (SUCCEEDED(hr))
            hr = triton9GetDepthStencilView(device, device->depthStencil, &view);
        triton9ProofDiagU32("TRITON9-PROOF-CLEAR-DSV-HR", (DWORD)hr);
        if (SUCCEEDED(hr) && normalizedCount) {
            if (triton9ClearRectsCoverResource(
                    normalized, normalizedCount, device->depthStencil->width,
                    device->depthStencil->height)) {
                ID3D11DeviceContext1_ClearDepthStencilView(
                    device->hostContext, view, flags, args->FillDepth,
                    (UINT8)args->FillStencil);
            } else if (device->depthStencil->format == D3DDDIFMT_D16) {
                triton9ClearD16Rects(device, view, args->FillDepth,
                                     normalizedCount, normalized);
            } else {
                hr = npt_dispatch_clear_depth_stencil_rects(
                    device->hostContext, view, flags, args->FillDepth,
                    (UINT8)args->FillStencil, normalizedCount,
                    (const int32_t *)normalized);
            }
        }
    }
    if (SUCCEEDED(hr))
        hr = triton9CheckHostDevice(device);
    triton9ProofDiagU32("TRITON9-PROOF-CLEAR-HOST-HR", (DWORD)hr);
    LeaveCriticalSection(&device->shaderLock);
    if (heapRects)
        HeapFree(GetProcessHeap(), 0, normalized);
    hr = triton9MapDeviceFailure(device, hr);
    return triton9TraceClearReturn(8, hr);
}

HRESULT APIENTRY
triton9Blt(HANDLE hDevice, const D3DDDIARG_BLT *args)
{
    TRITON9_DEVICE *device = (TRITON9_DEVICE *)hDevice;
    TRITON9_RESOURCE *source;
    TRITON9_RESOURCE *destination;
    D3D11_BOX box;
    D3D11_BOX destinationBox;
    BOOL sourceSystemMemory;
    BOOL destinationSystemMemory;
    BOOL stretch;
    HRESULT hr;

    if (!device || !args)
        return E_INVALIDARG;
    if (device->deviceLost)
        return D3DDDIERR_DEVICEREMOVED;
    if ((args->Flags.Value & ~3u) || (args->Flags.Point && args->Flags.Linear))
        return D3DDDIERR_NOTAVAILABLE;
    source = triton9ResourceSurface((TRITON9_RESOURCE *)args->hSrcResource,
                                    args->SrcSubResourceIndex);
    destination = triton9ResourceSurface((TRITON9_RESOURCE *)args->hDstResource,
                                         args->DstSubResourceIndex);
    if (!source || !destination)
        return D3DDDIERR_NOTAVAILABLE;
    if (!triton9ResourceBelongsToDevice(device, source) ||
        !triton9ResourceBelongsToDevice(device, destination))
        return D3DDDIERR_INVALIDCALL;
    if (source->locked || destination->locked ||
        source->isBuffer || destination->isBuffer)
        return D3DDDIERR_NOTAVAILABLE;
    sourceSystemMemory = triton9ResourceIsSystemMemory(source);
    destinationSystemMemory = triton9ResourceIsSystemMemory(destination);
    if (source->hostFormat != destination->hostFormat ||
        (sourceSystemMemory && !source->shadow) ||
        (destinationSystemMemory && !destination->shadow) ||
        args->SrcRect.left < 0 ||
        args->SrcRect.top < 0 || args->SrcRect.right <= args->SrcRect.left ||
        args->SrcRect.bottom <= args->SrcRect.top || args->DstRect.left < 0 ||
        args->DstRect.top < 0 || args->DstRect.right <= args->DstRect.left ||
        args->DstRect.bottom <= args->DstRect.top ||
        (UINT)args->SrcRect.right > source->width ||
        (UINT)args->SrcRect.bottom > source->height ||
        (UINT)args->DstRect.right > destination->width ||
        (UINT)args->DstRect.bottom > destination->height)
        return D3DDDIERR_NOTAVAILABLE;

    stretch = args->DstRect.right - args->DstRect.left !=
                  args->SrcRect.right - args->SrcRect.left ||
              args->DstRect.bottom - args->DstRect.top !=
                  args->SrcRect.bottom - args->SrcRect.top;
    if (stretch && (sourceSystemMemory || destinationSystemMemory ||
                    source->isDepthStencil || destination->isDepthStencil))
        return D3DDDIERR_NOTAVAILABLE;

    /* Vista performs the memory copy itself when both allocations are
     * preallocated SYSTEMMEM.  The UMD only has to retire hardware references;
     * Triton never submits either runtime pointer to the host, so there are no
     * such references to drain here. */
    if (sourceSystemMemory && destinationSystemMemory)
        return S_OK;
    hr = triton9EnsureHostDevice(device);
    if (FAILED(hr))
        return hr;
    if (!sourceSystemMemory) {
        hr = triton9EnsureResourceHost(device, source);
        if (FAILED(hr))
            return hr;
    }
    if (!destinationSystemMemory) {
        hr = triton9EnsureResourceHost(device, destination);
        if (FAILED(hr))
            return hr;
    }
    if ((!sourceSystemMemory && !source->hostResource) ||
        (!destinationSystemMemory && !destination->hostResource))
        return D3DDDIERR_NOTAVAILABLE;

    box.left = args->SrcRect.left;
    box.right = args->SrcRect.right;
    box.top = args->SrcRect.top;
    box.bottom = args->SrcRect.bottom;
    box.front = 0;
    box.back = 1;
    EnterCriticalSection(&device->shaderLock);
    if (stretch) {
        hr = triton9StretchBlt(device, source, destination, &args->SrcRect,
                               &args->DstRect, args->Flags.Linear);
    } else if (destinationSystemMemory) {
        /* Complete the GPU readback into Vista's pSysMem allocation now.  A
         * later NotifyOnly Lock cannot repair a deferred private copy because
         * the runtime ignores the pointer returned by that callback. */
        hr = triton9EnsureStagingResource(device, destination);
        if (SUCCEEDED(hr)) {
            ID3D11DeviceContext1_CopySubresourceRegion(
                device->hostContext, destination->stagingResource, 0,
                args->DstRect.left, args->DstRect.top, 0,
                source->hostResource, 0, &box);
            hr = triton9CheckHostDevice(device);
        }
        if (SUCCEEDED(hr))
            hr = triton9CopyStagingSurfaceToShadow(device, destination,
                                                    &args->DstRect);
    } else if (sourceSystemMemory) {
        destinationBox.left = args->DstRect.left;
        destinationBox.right = args->DstRect.right;
        destinationBox.top = args->DstRect.top;
        destinationBox.bottom = args->DstRect.bottom;
        destinationBox.front = 0;
        destinationBox.back = 1;
        ID3D11DeviceContext1_UpdateSubresource(
            device->hostContext, destination->hostResource, 0, &destinationBox,
            source->shadow + (SIZE_T)args->SrcRect.top * source->pitch +
                (SIZE_T)args->SrcRect.left * source->bytesPerPixel,
            source->pitch, source->slicePitch);
        hr = triton9CheckHostDevice(device);
    } else if (source == destination) {
        D3D11_BOX stagedBox;

        hr = triton9EnsureStagingResource(device, source);
        if (SUCCEEDED(hr)) {
            ID3D11DeviceContext1_CopySubresourceRegion(
                device->hostContext, source->stagingResource, 0, 0, 0, 0,
                source->hostResource, 0, &box);
            stagedBox.left = 0;
            stagedBox.right = box.right - box.left;
            stagedBox.top = 0;
            stagedBox.bottom = box.bottom - box.top;
            stagedBox.front = 0;
            stagedBox.back = 1;
            ID3D11DeviceContext1_CopySubresourceRegion(
                device->hostContext, destination->hostResource, 0,
                args->DstRect.left, args->DstRect.top, 0,
                source->stagingResource, 0, &stagedBox);
            hr = triton9CheckHostDevice(device);
        }
    } else {
        ID3D11DeviceContext1_CopySubresourceRegion(device->hostContext,
                                                    destination->hostResource, 0,
                                                    args->DstRect.left,
                                                    args->DstRect.top, 0,
                                                    source->hostResource, 0, &box);
        hr = triton9CheckHostDevice(device);
    }
    LeaveCriticalSection(&device->shaderLock);
    if (SUCCEEDED(hr) && destinationSystemMemory)
        hr = triton9CommitSystemMemoryWrite(device, destination);
    return triton9MapDeviceFailure(device, hr);
}

HRESULT APIENTRY
triton9BufBlt(HANDLE hDevice, const D3DDDIARG_BUFFERBLT *args)
{
    TRITON9_DEVICE *device = (TRITON9_DEVICE *)hDevice;
    TRITON9_RESOURCE *source;
    TRITON9_RESOURCE *destination;
    D3D11_BOX box;
    D3D11_BOX destinationBox;
    BOOL sourceSystemMemory;
    BOOL destinationSystemMemory;
    HRESULT hr;

    if (!device || !args)
        return E_INVALIDARG;
    if (device->deviceLost)
        return D3DDDIERR_DEVICEREMOVED;
    source = (TRITON9_RESOURCE *)args->hSrcResource;
    destination = (TRITON9_RESOURCE *)args->hDstResource;
    if (!source || !destination)
        return D3DDDIERR_INVALIDCALL;
    if (!triton9ResourceBelongsToDevice(device, source) ||
        !triton9ResourceBelongsToDevice(device, destination))
        return D3DDDIERR_INVALIDCALL;
    if (source->locked ||
        destination->locked || !source->isBuffer || !destination->isBuffer)
        return D3DDDIERR_INVALIDCALL;
    sourceSystemMemory = triton9ResourceIsSystemMemory(source);
    destinationSystemMemory = triton9ResourceIsSystemMemory(destination);
    if (!args->SrcRange.Size || args->SrcRange.Offset > source->width ||
        args->SrcRange.Size > source->width - args->SrcRange.Offset ||
        args->Offset > destination->width ||
        args->SrcRange.Size > destination->width - args->Offset)
        return D3DDDIERR_INVALIDCALL;
    if ((sourceSystemMemory && !source->shadow) ||
        (destinationSystemMemory && !destination->shadow))
        return D3DDDIERR_INVALIDUSERBUFFER;
    if (sourceSystemMemory && destinationSystemMemory) {
        memmove(destination->shadow + args->Offset,
                source->shadow + args->SrcRange.Offset,
                args->SrcRange.Size);
        return triton9CommitSystemMemoryWrite(device, destination);
    }
    hr = triton9EnsureHostDevice(device);
    if (FAILED(hr))
        return hr;
    if (!sourceSystemMemory) {
        hr = triton9EnsureResourceHost(device, source);
        if (FAILED(hr))
            return hr;
    }
    if (!destinationSystemMemory) {
        hr = triton9EnsureResourceHost(device, destination);
        if (FAILED(hr))
            return hr;
    }
    if ((!sourceSystemMemory && !source->hostResource) ||
        (!destinationSystemMemory && !destination->hostResource))
        return D3DDDIERR_INVALIDCALL;

    box.left = args->SrcRange.Offset;
    box.right = args->SrcRange.Offset + args->SrcRange.Size;
    box.top = 0;
    box.bottom = 1;
    box.front = 0;
    box.back = 1;
    EnterCriticalSection(&device->shaderLock);
    if (destinationSystemMemory) {
        hr = triton9EnsureStagingResource(device, destination);
        if (SUCCEEDED(hr)) {
            ID3D11DeviceContext1_CopySubresourceRegion(
                device->hostContext, destination->stagingResource, 0,
                args->Offset, 0, 0, source->hostResource, 0, &box);
            hr = triton9CheckHostDevice(device);
        }
        if (SUCCEEDED(hr))
            hr = triton9CopyStagingBufferToShadow(
                device, destination, args->Offset, args->SrcRange.Size);
    } else if (sourceSystemMemory) {
        destinationBox.left = args->Offset;
        destinationBox.right = args->Offset + args->SrcRange.Size;
        destinationBox.top = 0;
        destinationBox.bottom = 1;
        destinationBox.front = 0;
        destinationBox.back = 1;
        ID3D11DeviceContext1_UpdateSubresource(
            device->hostContext, destination->hostResource, 0, &destinationBox,
            source->shadow + args->SrcRange.Offset, 0, 0);
        hr = triton9CheckHostDevice(device);
    } else if (source == destination) {
        D3D11_BOX stagedBox;

        hr = triton9EnsureStagingResource(device, source);
        if (SUCCEEDED(hr)) {
            ID3D11DeviceContext1_CopySubresourceRegion(
                device->hostContext, source->stagingResource, 0, 0, 0, 0,
                source->hostResource, 0, &box);
            stagedBox.left = 0;
            stagedBox.right = args->SrcRange.Size;
            stagedBox.top = 0;
            stagedBox.bottom = 1;
            stagedBox.front = 0;
            stagedBox.back = 1;
            ID3D11DeviceContext1_CopySubresourceRegion(
                device->hostContext, destination->hostResource, 0,
                args->Offset, 0, 0, source->stagingResource, 0, &stagedBox);
            hr = triton9CheckHostDevice(device);
        }
    } else {
        ID3D11DeviceContext1_CopySubresourceRegion(device->hostContext,
                                                    destination->hostResource, 0,
                                                    args->Offset, 0, 0,
                                                    source->hostResource, 0, &box);
        hr = triton9CheckHostDevice(device);
    }
    LeaveCriticalSection(&device->shaderLock);
    if (SUCCEEDED(hr) && destinationSystemMemory)
        hr = triton9CommitSystemMemoryWrite(device, destination);
    return triton9MapDeviceFailure(device, hr);
}

HRESULT APIENTRY
triton9TexBlt(HANDLE hDevice, const D3DDDIARG_TEXBLT *args)
{
    TRITON9_DEVICE *device = (TRITON9_DEVICE *)hDevice;
    TRITON9_RESOURCE *source;
    TRITON9_RESOURCE *destination;
    D3D11_BOX box;
    D3D11_BOX destinationBox;
    RECT destinationRect;
    BOOL sourceSystemMemory;
    BOOL destinationSystemMemory;
    LONG width;
    LONG height;
    HRESULT hr;

    if (!device || !args)
        return E_INVALIDARG;
    if (device->deviceLost)
        return D3DDDIERR_DEVICEREMOVED;
    source = (TRITON9_RESOURCE *)args->hSrcResource;
    destination = (TRITON9_RESOURCE *)args->hDstResource;
    if (!source || !destination)
        return D3DDDIERR_INVALIDCALL;
    if (!triton9ResourceBelongsToDevice(device, source) ||
        !triton9ResourceBelongsToDevice(device, destination))
        return D3DDDIERR_INVALIDCALL;
    if (source->locked ||
        destination->locked || source->isBuffer || destination->isBuffer)
        return D3DDDIERR_INVALIDCALL;
    sourceSystemMemory = triton9ResourceIsSystemMemory(source);
    destinationSystemMemory = triton9ResourceIsSystemMemory(destination);
    if (source->hostFormat != destination->hostFormat || args->CubeMapFace != 0 ||
        args->SrcRect.left < 0 || args->SrcRect.top < 0 ||
        args->SrcRect.right <= args->SrcRect.left ||
        args->SrcRect.bottom <= args->SrcRect.top ||
        args->DstPoint.x < 0 || args->DstPoint.y < 0 ||
        (UINT)args->SrcRect.right > source->width ||
        (UINT)args->SrcRect.bottom > source->height)
        return D3DDDIERR_INVALIDCALL;
    width = args->SrcRect.right - args->SrcRect.left;
    height = args->SrcRect.bottom - args->SrcRect.top;
    if ((UINT)args->DstPoint.x > destination->width ||
        (UINT)width > destination->width - (UINT)args->DstPoint.x ||
        (UINT)args->DstPoint.y > destination->height ||
        (UINT)height > destination->height - (UINT)args->DstPoint.y)
        return D3DDDIERR_INVALIDCALL;

    destinationRect.left = args->DstPoint.x;
    destinationRect.top = args->DstPoint.y;
    destinationRect.right = args->DstPoint.x + width;
    destinationRect.bottom = args->DstPoint.y + height;
    if ((sourceSystemMemory && !source->shadow) ||
        (destinationSystemMemory && !destination->shadow))
        return D3DDDIERR_INVALIDUSERBUFFER;
    if (sourceSystemMemory && destinationSystemMemory) {
        triton9CopySystemSurfaceRows(destination, args->DstPoint.x,
                                     args->DstPoint.y, source, &args->SrcRect);
        return triton9CommitSystemMemoryWrite(device, destination);
    }
    hr = triton9EnsureHostDevice(device);
    if (FAILED(hr))
        return hr;
    if (!sourceSystemMemory) {
        hr = triton9EnsureResourceHost(device, source);
        if (FAILED(hr))
            return hr;
    }
    if (!destinationSystemMemory) {
        hr = triton9EnsureResourceHost(device, destination);
        if (FAILED(hr))
            return hr;
    }
    if ((!sourceSystemMemory && !source->hostResource) ||
        (!destinationSystemMemory && !destination->hostResource))
        return D3DDDIERR_INVALIDCALL;

    box.left = args->SrcRect.left;
    box.right = args->SrcRect.right;
    box.top = args->SrcRect.top;
    box.bottom = args->SrcRect.bottom;
    box.front = 0;
    box.back = 1;
    EnterCriticalSection(&device->shaderLock);
    if (destinationSystemMemory) {
        hr = triton9EnsureStagingResource(device, destination);
        if (SUCCEEDED(hr)) {
            ID3D11DeviceContext1_CopySubresourceRegion(
                device->hostContext, destination->stagingResource, 0,
                args->DstPoint.x, args->DstPoint.y, 0,
                source->hostResource, 0, &box);
            hr = triton9CheckHostDevice(device);
        }
        if (SUCCEEDED(hr))
            hr = triton9CopyStagingSurfaceToShadow(device, destination,
                                                    &destinationRect);
    } else if (sourceSystemMemory) {
        destinationBox.left = destinationRect.left;
        destinationBox.right = destinationRect.right;
        destinationBox.top = destinationRect.top;
        destinationBox.bottom = destinationRect.bottom;
        destinationBox.front = 0;
        destinationBox.back = 1;
        ID3D11DeviceContext1_UpdateSubresource(
            device->hostContext, destination->hostResource, 0, &destinationBox,
            source->shadow + (SIZE_T)args->SrcRect.top * source->pitch +
                (SIZE_T)args->SrcRect.left * source->bytesPerPixel,
            source->pitch, source->slicePitch);
        hr = triton9CheckHostDevice(device);
    } else if (source == destination) {
        D3D11_BOX stagedBox;

        hr = triton9EnsureStagingResource(device, source);
        if (SUCCEEDED(hr)) {
            ID3D11DeviceContext1_CopySubresourceRegion(
                device->hostContext, source->stagingResource, 0, 0, 0, 0,
                source->hostResource, 0, &box);
            stagedBox.left = 0;
            stagedBox.right = width;
            stagedBox.top = 0;
            stagedBox.bottom = height;
            stagedBox.front = 0;
            stagedBox.back = 1;
            ID3D11DeviceContext1_CopySubresourceRegion(
                device->hostContext, destination->hostResource, 0,
                args->DstPoint.x, args->DstPoint.y, 0,
                source->stagingResource, 0, &stagedBox);
            hr = triton9CheckHostDevice(device);
        }
    } else {
        ID3D11DeviceContext1_CopySubresourceRegion(device->hostContext,
                                                    destination->hostResource, 0,
                                                    args->DstPoint.x,
                                                    args->DstPoint.y, 0,
                                                    source->hostResource, 0, &box);
        hr = triton9CheckHostDevice(device);
    }
    LeaveCriticalSection(&device->shaderLock);
    if (SUCCEEDED(hr) && destinationSystemMemory)
        hr = triton9CommitSystemMemoryWrite(device, destination);
    return triton9MapDeviceFailure(device, hr);
}

HRESULT APIENTRY
triton9ColorFill(HANDLE hDevice, const D3DDDIARG_COLORFILL *args)
{
    TRITON9_DEVICE *device = (TRITON9_DEVICE *)hDevice;
    TRITON9_RESOURCE *resource;
    ID3D11RenderTargetView *view;
    const FLOAT color[4] = {
        ((args ? args->Color >> 16 : 0) & 0xff) / 255.0f,
        ((args ? args->Color >> 8 : 0) & 0xff) / 255.0f,
        ((args ? args->Color : 0) & 0xff) / 255.0f,
        ((args ? args->Color >> 24 : 0) & 0xff) / 255.0f,
    };
    HRESULT hr;

    if (!device || !args ||
        (args->Flags.Value & ~1u))
        return E_INVALIDARG;
    if (device->deviceLost)
        return D3DDDIERR_DEVICEREMOVED;
    resource = triton9ResourceSurface((TRITON9_RESOURCE *)args->hResource,
                                      args->SubResourceIndex);
    if (args->hResource && !resource)
        return D3DDDIERR_INVALIDCALL;
    if (!resource)
        return S_OK;
    if (!triton9ResourceBelongsToDevice(device, resource))
        return D3DDDIERR_INVALIDCALL;
    if (triton9ResourceIsSystemMemory(resource))
        return D3DDDIERR_INVALIDCALL;
    hr = triton9EnsureHostDevice(device);
    if (FAILED(hr))
        return hr;
    hr = triton9EnsureResourceHost(device, resource);
    if (FAILED(hr))
        return hr;
    hr = triton9ValidateColorRects(&args->DstRect, 1, resource);
    if (FAILED(hr))
        return hr;
    EnterCriticalSection(&device->shaderLock);
    hr = triton9GetRenderTargetView(device, resource, &view);
    if (FAILED(hr)) {
        LeaveCriticalSection(&device->shaderLock);
        return hr;
    }
    triton9ClearRenderTargetRects(device, view, color, 1, &args->DstRect);
    hr = triton9CheckHostDevice(device);
    LeaveCriticalSection(&device->shaderLock);
    return triton9MapDeviceFailure(device, hr);
}

HRESULT APIENTRY
triton9GenerateMipSubLevels(HANDLE hDevice,
                            const D3DDDIARG_GENERATEMIPSUBLEVELS *args)
{
    TRITON9_DEVICE *device = (TRITON9_DEVICE *)hDevice;
    TRITON9_RESOURCE *resource;
    ID3D11ShaderResourceView *view;
    BOOL restoreRenderTarget;
    HRESULT hr;

    if (!device || !args || !(resource = (TRITON9_RESOURCE *)args->hResource))
        return E_INVALIDARG;
    if (device->deviceLost)
        return D3DDDIERR_DEVICEREMOVED;
    if (!triton9ResourceBelongsToDevice(device, resource))
        return D3DDDIERR_INVALIDCALL;
    if (triton9ResourceIsSystemMemory(resource))
        return D3DDDIERR_NOTAVAILABLE;
    hr = triton9EnsureHostDevice(device);
    if (FAILED(hr))
        return hr;
    hr = triton9EnsureResourceHost(device, resource);
    if (FAILED(hr))
        return hr;
    /* The D3D11 GenerateMips operation has fixed linear filtering. */
    if (args->Filter != D3DDDITEXF_LINEAR)
        return D3DDDIERR_NOTAVAILABLE;
    if (resource->mipLevels < 2 || resource->isBuffer ||
        resource->format == D3DDDIFMT_D16 || resource->format == D3DDDIFMT_D24S8 ||
        !(resource->hostBindFlags & D3D11_BIND_RENDER_TARGET))
        return D3DDDIERR_NOTAVAILABLE;

    EnterCriticalSection(&device->shaderLock);
    hr = triton9GetShaderResourceView(device, resource, &view);
    restoreRenderTarget = device->renderTarget == resource;
    if (SUCCEEDED(hr) && restoreRenderTarget) {
        device->renderTarget = NULL;
        hr = triton9BindOutputs(device);
    }
    if (SUCCEEDED(hr))
        ID3D11DeviceContext1_GenerateMips(device->hostContext, view);
    if (SUCCEEDED(hr))
        hr = triton9CheckHostDevice(device);
    if (restoreRenderTarget) {
        HRESULT restoreHr;

        device->renderTarget = resource;
        restoreHr = triton9BindOutputs(device);
        if (SUCCEEDED(hr) && FAILED(restoreHr))
            hr = restoreHr;
    }
    LeaveCriticalSection(&device->shaderLock);
    return triton9MapDeviceFailure(device, hr);
}

#endif /* !TRITON9_CLEAR_CONTRACT_PORTABLE */
