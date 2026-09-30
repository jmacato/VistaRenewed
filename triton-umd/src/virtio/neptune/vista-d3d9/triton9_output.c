/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * D3D9 output-merger and subresource copy operations. Canonical texture
 * aliases select their own mip/face while sharing the host allocation.
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
#include "triton9_cpu_layout.h"
#include "triton_trace_wire.h"

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

HRESULT
triton9BindOutputs(TRITON9_DEVICE *device)
{
    ID3D11RenderTargetView *views[TRITON9_MAX_RENDER_TARGETS] = { NULL };
    ID3D11DepthStencilView *depth = NULL;
    UINT count = 0;
    HRESULT hr;
    if (!device || !device->hostContext) return E_INVALIDARG;
    device->renderTargets[0] = device->renderTarget;
    for (UINT i = 0; i < TRITON9_MAX_RENDER_TARGETS; ++i) {
        TRITON9_RESOURCE *r = device->renderTargets[i];
        if (!r) continue;
        hr = triton9GetRenderTargetViewEx(device, r,
            device->renderStates[D3DRS_SRGBWRITEENABLE] && triton9FormatSupportsSrgb(r->format), &views[i]);
        if (FAILED(hr)) return hr;
        count = i + 1;
    }
    if (device->depthStencil) {
        hr = triton9GetDepthStencilView(device, device->depthStencil, &depth);
        if (FAILED(hr)) return hr;
    }
    ID3D11DeviceContext1_OMSetRenderTargets(device->hostContext, count, views, depth);
    return triton9CheckHostDevice(device);
}

/* D3D9 retains logical texture bindings while an unused sampler aliases an
 * output. Hide only the backend SRVs; the next draw rebuilds them from the
 * unchanged texture state after the application changes its outputs. */
static void
triton9HideResourceViews(TRITON9_DEVICE *device, TRITON9_RESOURCE *resource)
{
    ID3D11ShaderResourceView *nullView = NULL;
    UINT stage;

    if (!device || !resource)
        return;
    for (stage = 0; stage < TRITON9_MAX_TEXTURE_STAGES; ++stage) {
        if (triton9ResourcesShareBacking(device->textures[stage], resource)) {
            if (stage < TRITON9_MAX_PIXEL_SAMPLERS)
                ID3D11DeviceContext1_PSSetShaderResources(device->hostContext, stage, 1, &nullView);
            else if (stage >= TRITON9_VERTEX_SAMPLER_BASE)
                ID3D11DeviceContext1_VSSetShaderResources(device->hostContext, stage - TRITON9_VERTEX_SAMPLER_BASE, 1, &nullView);
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

HRESULT APIENTRY
triton9SetRenderTarget(HANDLE hDevice, const D3DDDIARG_SETRENDERTARGET *args)
{
    TRITON9_DEVICE *device = (TRITON9_DEVICE *)hDevice;
    TRITON9_RESOURCE *resource;
    TRITON9_RESOURCE *previous;
    HRESULT hr;

    if (!device || !args || args->RenderTargetIndex >= TRITON9_MAX_RENDER_TARGETS)
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
        triton9HideResourceViews(device, resource);
    }
    previous = args->RenderTargetIndex ? device->renderTargets[args->RenderTargetIndex] : device->renderTarget;
    triton9DiagU32("TRITON9-SET-RT-ALLOCATION", resource ? resource->hKMAllocation : 0);
    triton9DiagU32("TRITON9-SET-RT-STANDARD", resource ? resource->isStandardPrimary : 0);
    device->renderTargets[args->RenderTargetIndex] = resource;
    if (!args->RenderTargetIndex) device->renderTarget = resource;
    device->blendStateDirty = TRUE;
    hr = triton9BindOutputs(device);
    if (FAILED(hr)) {
        device->renderTargets[args->RenderTargetIndex] = previous;
        if (!args->RenderTargetIndex) device->renderTarget = previous;
        /* Keep the host pipeline consistent with the D3D9 state that the UMD
         * reports after this callback fails. */
        (void)triton9BindOutputs(device);
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
        triton9HideResourceViews(device, resource);
    }
    previous = device->depthStencil;
    device->depthStencil = resource;
    device->depthStencilStateDirty = TRUE;
    device->rasterizerStateDirty = TRUE;
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
    /* Validate every output and create all views before modifying any plane. */
    for (UINT clearPass = 0; clearPass < 2 && SUCCEEDED(hr); ++clearPass) {
    for (UINT slot = 0; slot < TRITON9_MAX_RENDER_TARGETS && SUCCEEDED(hr) &&
         (args->Flags & D3DCLEAR_TARGET); ++slot) {
        TRITON9_RESOURCE *target = slot ? device->renderTargets[slot] : device->renderTarget;
        if (!target && slot) continue;
        ID3D11RenderTargetView *view;
        RECT bounds;
        RECT viewport;
        RECT scissor;
        BOOL scissorEnabled;
        UINT normalizedCount = 0;
        enum triton9_clear_contract_result contractResult;

        if (!target)
            hr = D3DDDIERR_INVALIDCALL;
        else
            hr = triton9BuildClearLimits(device, target, &bounds,
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
            hr = triton9GetRenderTargetView(device, target, &view);
        if (SUCCEEDED(hr) && normalizedCount && clearPass) {
            FLOAT targetColor[4];
            memcpy(targetColor, color, sizeof(targetColor));
            if (triton9ClearRectsCoverResource(
                    normalized, normalizedCount, target->width,
                    target->height))
                triton9ClearRenderTargetRects(device, view, targetColor, 0, NULL);
            else
                triton9ClearRenderTargetRects(device, view, targetColor,
                                              normalizedCount, normalized);
            triton9ResourceWritten(target);
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
        else if (!device->depthStencil->isDepthStencil)
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
        if (SUCCEEDED(hr) && normalizedCount && clearPass) {
            if (triton9ClearRectsCoverResource(
                    normalized, normalizedCount, device->depthStencil->width,
                    device->depthStencil->height)) {
                ID3D11DeviceContext1_ClearDepthStencilView(
                    device->hostContext, view, flags, args->FillDepth,
                    (UINT8)args->FillStencil);
            } else if (device->depthStencil->hostFormat == DXGI_FORMAT_D16_UNORM) {
                triton9ClearD16Rects(device, view, args->FillDepth,
                                     normalizedCount, normalized);
            } else {
                hr = npt_dispatch_clear_depth_stencil_rects(
                    device->hostContext, view, flags, args->FillDepth,
                    (UINT8)args->FillStencil, normalizedCount,
                    (const int32_t *)normalized);
            }
            if (SUCCEEDED(hr)) triton9ResourceWritten(device->depthStencil);
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

/* A shader samples channels in logical RGBA order and the target view
 * encodes them in its own storage order.  Other format families require
 * explicit conversion semantics before they can use this path. */
static BOOL
triton9BltColorFormat(DXGI_FORMAT format)
{
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_B5G6R5_UNORM:
    case DXGI_FORMAT_B5G5R5A1_UNORM: case DXGI_FORMAT_B4G4R4A4_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16_UNORM: case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        return TRUE;
    default: return FALSE;
    }
}

static BOOL
triton9BltFlagsSupported(D3DDDI_BLTFLAGS flags)
{
    /* The DWM bits mark the first, middle and last copies of a windowed
     * present. They do not request colour keying, rotation or a new filter.
     * One copy may carry both BeginPresentToDwm and EndPresentToDwm. */
    const UINT supported = 0x003u | 0x700u;
    return !(flags.Value & ~supported) && !(flags.Point && flags.Linear);
}

/* Overlapping GPU copies need a snapshot, not CPU-readable staging storage.
 * The selected subresource supplies the dimensions; scratch has one mip and
 * one array slice, and keeps the sample layout for full MSAA copies. */
static HRESULT
triton9CreateCopyScratch(TRITON9_DEVICE *device, TRITON9_RESOURCE *source,
                         ID3D11Resource **scratch)
{
    HRESULT hr;
    *scratch = NULL;
    if (source->isBuffer) {
        D3D11_BUFFER_DESC desc = {0};
        ID3D11Buffer *buffer = NULL;
        desc.ByteWidth = source->width;
        desc.Usage = D3D11_USAGE_DEFAULT;
        hr = ID3D11Device1_CreateBuffer(device->hostDevice, &desc, NULL, &buffer);
        *scratch = (ID3D11Resource *)buffer;
    } else if (source->isVolume) {
        D3D11_TEXTURE3D_DESC desc = {0};
        ID3D11Texture3D *texture = NULL;
        desc.Width = source->width;
        desc.Height = source->height;
        desc.Depth = source->depth;
        desc.MipLevels = 1;
        desc.Format = source->hostFormat;
        desc.Usage = D3D11_USAGE_DEFAULT;
        hr = ID3D11Device1_CreateTexture3D(device->hostDevice, &desc, NULL, &texture);
        *scratch = (ID3D11Resource *)texture;
    } else {
        D3D11_TEXTURE2D_DESC desc = {0};
        ID3D11Texture2D *texture = NULL;
        desc.Width = source->width;
        desc.Height = source->height;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = source->hostFormat;
        desc.SampleDesc.Count = source->sampleCount > 1 ? source->sampleCount : 1;
        desc.SampleDesc.Quality = source->sampleQuality;
        desc.Usage = D3D11_USAGE_DEFAULT;
        hr = ID3D11Device1_CreateTexture2D(device->hostDevice, &desc, NULL, &texture);
        *scratch = (ID3D11Resource *)texture;
    }
    if (FAILED(hr) || !*scratch) {
        if (*scratch) ID3D11Resource_Release(*scratch);
        *scratch = NULL;
        return FAILED(hr) ? triton9MapDeviceFailure(device, hr) : E_FAIL;
    }
    return S_OK;
}

/* Copy one selected subresource. CPU backing and pitches remain logical D3D9
 * bytes; every GPU command names its actual mip/face index. Validate before
 * creating host objects, so a malformed request cannot partially copy data. */
static HRESULT
triton9CopyBox(TRITON9_DEVICE *device, TRITON9_RESOURCE *source,
              TRITON9_RESOURCE *destination, const D3D11_BOX *box,
              UINT x, UINT y, UINT z, BOOL validateOnly)
{
    D3D11_BOX dst;
    BOOL srcCpu, dstCpu, full;
    UINT sourceIndex;
    ID3D11Resource *sourceHost;
    HRESULT hr;
    if (!triton9ResourceBelongsToDevice(device, source) ||
        !triton9ResourceBelongsToDevice(device, destination) ||
        source->locked || destination->locked || source->isBuffer || destination->isBuffer ||
        source->format != destination->format ||
        !triton9ResourceBoxValid(source, box) || x > destination->width ||
        y > destination->height || z > destination->depth ||
        box->right - box->left > destination->width - x ||
        box->bottom - box->top > destination->height - y ||
        box->back - box->front > destination->depth - z)
        return D3DDDIERR_INVALIDCALL;
    dst.left = x; dst.top = y; dst.front = z;
    dst.right = x + box->right - box->left;
    dst.bottom = y + box->bottom - box->top;
    dst.back = z + box->back - box->front;
    if (!triton9ResourceBoxValid(destination, &dst)) return D3DDDIERR_INVALIDCALL;
    srcCpu = triton9ResourceIsSystemMemory(source);
    dstCpu = triton9ResourceIsSystemMemory(destination);
    full = !box->left && !box->top && !box->front &&
        box->right == source->width && box->bottom == source->height && box->back == source->depth &&
        !x && !y && !z && dst.right == destination->width && dst.bottom == destination->height &&
        dst.back == destination->depth;
    if ((srcCpu && !source->shadow) || (dstCpu && !destination->shadow) ||
        (destination->sampleCount > 1 && (srcCpu || source->sampleCount != destination->sampleCount ||
            source->sampleQuality != destination->sampleQuality || !full)) ||
        (source->sampleCount > 1 && !full) ||
        ((source->isDepthStencil || destination->isDepthStencil) && !full))
        return D3DDDIERR_INVALIDCALL;
    if (validateOnly) return S_OK;
    if (srcCpu && dstCpu) {
        UINT bw = source->blockWidth ? source->blockWidth : 1;
        UINT bh = source->blockHeight ? source->blockHeight : 1;
        UINT bytes = source->bytesPerBlock ? source->bytesPerBlock : source->bytesPerPixel;
        UINT rows = (box->bottom + bh - 1) / bh - box->top / bh;
        UINT copy = ((box->right + bw - 1) / bw - box->left / bw) * bytes;
        UINT slices = box->back - box->front;
        SIZE_T size = (SIZE_T)copy * rows * slices;
        BYTE *snapshot = HeapAlloc(GetProcessHeap(), 0, size);
        if (!snapshot) return E_OUTOFMEMORY;
        for (UINT k = 0; k < slices; ++k)
            triton9CpuCopyRows(snapshot + (SIZE_T)k * copy * rows, copy,
                source->shadow + triton9ResourceByteOffset(source, box->left, box->top, box->front + k),
                source->pitch, copy, rows);
        for (UINT k = 0; k < slices; ++k)
            triton9CpuCopyRows(destination->shadow + triton9ResourceByteOffset(destination, x, y, z + k),
                destination->pitch, snapshot + (SIZE_T)k * copy * rows, copy, copy, rows);
        HeapFree(GetProcessHeap(), 0, snapshot);
        return triton9CommitSystemMemoryWrite(device, destination);
    }
    hr = triton9EnsureHostDevice(device);
    if (SUCCEEDED(hr) && !srcCpu) hr = triton9EnsureResourceHost(device, source);
    if (SUCCEEDED(hr) && !dstCpu) hr = triton9EnsureResourceHost(device, destination);
    if (FAILED(hr)) return hr;
    EnterCriticalSection(&device->shaderLock);
    if (srcCpu) {
        hr = triton9UpdateHostTexture(device, destination,
            (ID3D11DeviceContext *)device->hostContext, destination->hostResource,
            destination->subresourceIndex, &dst,
            source->shadow + triton9ResourceByteOffset(source, box->left, box->top, box->front),
            source->pitch, source->slicePitch);
    } else {
        sourceHost = source->hostResource; sourceIndex = source->subresourceIndex;
        if (source->sampleCount > 1 && destination->sampleCount <= 1)
            hr = triton9ResolveResource(device, source, &sourceHost, &sourceIndex);
        if (SUCCEEDED(hr) && dstCpu) {
            hr = triton9EnsureStagingResource(device, destination);
            if (SUCCEEDED(hr)) {
                ID3D11DeviceContext1_CopySubresourceRegion(device->hostContext,
                    destination->stagingResource, 0, x, y, z, sourceHost, sourceIndex,
                    source->isDepthStencil ? NULL : box);
                hr = triton9CopyStagingBoxToShadow(device, destination, &dst);
            }
        } else if (SUCCEEDED(hr) && sourceHost == destination->hostResource &&
                   sourceIndex == destination->subresourceIndex) {
            /* A snapshot is necessary for overlapping copies within a mip. */
            ID3D11Resource *scratch = NULL;
            hr = triton9CreateCopyScratch(device, source, &scratch);
            if (SUCCEEDED(hr)) {
                ID3D11DeviceContext1_CopySubresourceRegion(device->hostContext,
                    scratch, 0, 0, 0, 0, sourceHost, sourceIndex, NULL);
                ID3D11DeviceContext1_CopySubresourceRegion(device->hostContext,
                    destination->hostResource, destination->subresourceIndex, x, y, z,
                    scratch, 0, source->sampleCount > 1 || source->isDepthStencil ? NULL : box);
                ID3D11Resource_Release(scratch);
            }
        } else if (SUCCEEDED(hr)) {
            ID3D11DeviceContext1_CopySubresourceRegion(device->hostContext,
                destination->hostResource, destination->subresourceIndex, x, y, z,
                sourceHost, sourceIndex, source->sampleCount > 1 || source->isDepthStencil ? NULL : box);
        }
    }
    if (SUCCEEDED(hr)) { triton9ResourceWritten(destination); hr = triton9CheckHostDevice(device); }
    LeaveCriticalSection(&device->shaderLock);
    if (SUCCEEDED(hr) && dstCpu) hr = triton9CommitSystemMemoryWrite(device, destination);
    return triton9MapDeviceFailure(device, hr);
}

static HRESULT
triton9BltImpl(HANDLE hDevice, const D3DDDIARG_BLT *args)
{
    TRITON9_DEVICE *device = (TRITON9_DEVICE *)hDevice;
    TRITON9_RESOURCE *source, *destination;
    D3D11_BOX box;
    BOOL stretch, convert, dstCpu;
    HRESULT hr;
    if (!device || !args) return E_INVALIDARG;
    if (device->deviceLost) return D3DDDIERR_DEVICEREMOVED;
    if (!triton9BltFlagsSupported(args->Flags)) return D3DDDIERR_NOTAVAILABLE;
    source = triton9ResourceSurface((TRITON9_RESOURCE *)args->hSrcResource, args->SrcSubResourceIndex);
    destination = triton9ResourceSurface((TRITON9_RESOURCE *)args->hDstResource, args->DstSubResourceIndex);
    if (!triton9ResourceBelongsToDevice(device, source) || !triton9ResourceBelongsToDevice(device, destination) ||
        source->locked || destination->locked || source->isBuffer || destination->isBuffer ||
        source->isVolume || destination->isVolume ||
        args->SrcRect.left < 0 || args->SrcRect.top < 0 || args->SrcRect.right <= args->SrcRect.left ||
        args->SrcRect.bottom <= args->SrcRect.top || args->DstRect.left < 0 || args->DstRect.top < 0 ||
        args->DstRect.right <= args->DstRect.left || args->DstRect.bottom <= args->DstRect.top ||
        (UINT)args->SrcRect.right > source->width || (UINT)args->SrcRect.bottom > source->height ||
        (UINT)args->DstRect.right > destination->width || (UINT)args->DstRect.bottom > destination->height)
        return D3DDDIERR_INVALIDCALL;
    if (triton9ResourceIsSystemMemory(source) && triton9ResourceIsSystemMemory(destination))
        return S_OK; /* Vista performs this particular DDI copy itself. */
    stretch = args->SrcRect.right - args->SrcRect.left != args->DstRect.right - args->DstRect.left ||
        args->SrcRect.bottom - args->SrcRect.top != args->DstRect.bottom - args->DstRect.top;
    hr = triton9EnsureHostDevice(device);
    if (SUCCEEDED(hr) && !triton9ResourceIsSystemMemory(source)) hr = triton9EnsureResourceHost(device, source);
    if (SUCCEEDED(hr) && !triton9ResourceIsSystemMemory(destination)) hr = triton9EnsureResourceHost(device, destination);
    if (FAILED(hr)) return hr;
    /* Standard-primary import can change BGRA to the export's actual RGBA. */
    convert = source->hostFormat != destination->hostFormat || source->format != destination->format;
    if (stretch || convert) {
        if (source->isDepthStencil || destination->isDepthStencil ||
            source->sampleCount > 1 || destination->sampleCount > 1 ||
            !triton9BltColorFormat(source->hostFormat) || !triton9BltColorFormat(destination->hostFormat))
            return D3DDDIERR_INVALIDCALL;
        dstCpu = triton9ResourceIsSystemMemory(destination);
        EnterCriticalSection(&device->shaderLock);
        hr = dstCpu ? triton9EnsureStagingResource(device, destination) : S_OK;
        if (SUCCEEDED(hr)) hr = triton9StretchBlt(device, source, destination, &args->SrcRect,
                                                 &args->DstRect, args->Flags.Linear);
        if (SUCCEEDED(hr) && dstCpu) hr = triton9CopyStagingSurfaceToShadow(device, destination, &args->DstRect);
        LeaveCriticalSection(&device->shaderLock);
        if (SUCCEEDED(hr) && dstCpu) hr = triton9CommitSystemMemoryWrite(device, destination);
        return triton9MapDeviceFailure(device, hr);
    }
    box.left = args->SrcRect.left; box.top = args->SrcRect.top;
    box.right = args->SrcRect.right; box.bottom = args->SrcRect.bottom; box.front = 0; box.back = 1;
    return triton9CopyBox(device, source, destination, &box,
                         args->DstRect.left, args->DstRect.top, 0, FALSE);
}

HRESULT APIENTRY
triton9Blt(HANDLE hDevice, const D3DDDIARG_BLT *args)
{
    TRITON9_DEVICE *device = (TRITON9_DEVICE *)hDevice;
    TRITON9_RESOURCE *source = args ? triton9ResourceSurface((TRITON9_RESOURCE *)args->hSrcResource,
        args->SrcSubResourceIndex) : NULL;
    TRITON9_RESOURCE *destination = args ? triton9ResourceSurface((TRITON9_RESOURCE *)args->hDstResource,
        args->DstSubResourceIndex) : NULL;
    BOOL owned = triton9ResourceBelongsToDevice(device, source) &&
                 triton9ResourceBelongsToDevice(device, destination);
    BOOL trace = owned && device->shaderLockInitialized && (args->Flags.Value & 0x500u) && triton9TraceEnabled(device);
    BOOL traceReadback = FALSE;
    static LONG readbackTraceCount;
    HRESULT hr;
    if (args) {
        triton9DiagU32("TRITON9-BLT-FLAGS", args->Flags.Value);
        if (!triton9BltFlagsSupported(args->Flags))
            triton9DiagU32("TRITON9-BLT-FLAGS-REJECT", args->Flags.Value);
    }
    if (owned) {
        triton9DiagU32("TRITON9-BLT-SRC-FORMAT", source->hostFormat);
        triton9DiagU32("TRITON9-BLT-DST-FORMAT", destination->hostFormat);
        traceReadback = triton9ResourceIsSystemMemory(destination) && destination->width == 1280 &&
            destination->height == 720 && destination->bytesPerPixel == 4 &&
            InterlockedIncrement(&readbackTraceCount) <= 12;
        if (traceReadback) {
            triton9DiagU32("TRITON9-READBACK-BLT-BEGIN-TICK", GetTickCount());
            triton9DiagU32("TRITON9-READBACK-SRC-STD", source->isStandardPrimary);
        }
    }
    if (trace) {
        LARGE_INTEGER ticks;
        EnterCriticalSection(&device->shaderLock);
        QueryPerformanceCounter(&ticks);
        triton9TraceBegin(device, ticks.QuadPart, args->Flags.Value, TT_BLT_BEGIN);
    }
    hr = triton9BltImpl(hDevice, args);
    if (SUCCEEDED(hr) && args->Flags.EndPresentToDwm &&
        !(triton9ResourceIsSystemMemory(source) &&
          triton9ResourceIsSystemMemory(destination)))
        hr = triton9CompleteRedirectedPresent(device);
    if (trace) {
        triton9TraceEvent(device, TT_BLT_END, (UINT)hr, source->hKMAllocation, destination->hKMAllocation);
        device->traceFrame = 0;
        LeaveCriticalSection(&device->shaderLock);
    }
    if (traceReadback && SUCCEEDED(hr) && destination->shadow &&
        destination->shadowSize >= (SIZE_T)360 * destination->pitch + 640 * 4 + 4) {
        DWORD pixel;
        CopyMemory(&pixel, destination->shadow + (SIZE_T)360 * destination->pitch + 640 * 4, 4);
        triton9DiagU32("TRITON9-READBACK-BLT-END-TICK", GetTickCount());
        triton9DiagU32("TRITON9-READBACK-SHADOW-PIXEL", pixel);
    }
    return hr;
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
        ID3D11Resource *scratch = NULL;

        hr = triton9CreateCopyScratch(device, source, &scratch);
        if (SUCCEEDED(hr)) {
            ID3D11DeviceContext1_CopySubresourceRegion(
                device->hostContext, scratch, 0, 0, 0, 0,
                source->hostResource, 0, &box);
            stagedBox.left = 0;
            stagedBox.right = args->SrcRange.Size;
            stagedBox.top = 0;
            stagedBox.bottom = 1;
            stagedBox.front = 0;
            stagedBox.back = 1;
            ID3D11DeviceContext1_CopySubresourceRegion(
                device->hostContext, destination->hostResource, 0,
                args->Offset, 0, 0, scratch, 0, &stagedBox);
            ID3D11Resource_Release(scratch);
            hr = triton9CheckHostDevice(device);
        }
    } else {
        ID3D11DeviceContext1_CopySubresourceRegion(device->hostContext,
                                                    destination->hostResource, 0,
                                                    args->Offset, 0, 0,
                                                    source->hostResource, 0, &box);
        hr = triton9CheckHostDevice(device);
    }
    if (SUCCEEDED(hr)) triton9ResourceWritten(destination);
    LeaveCriticalSection(&device->shaderLock);
    if (SUCCEEDED(hr) && destinationSystemMemory)
        hr = triton9CommitSystemMemoryWrite(device, destination);
    return triton9MapDeviceFailure(device, hr);
}

static HRESULT
triton9CopyTextureChain(TRITON9_DEVICE *device, TRITON9_RESOURCE *source,
                       TRITON9_RESOURCE *destination, UINT face, D3D11_BOX box,
                       UINT x, UINT y, UINT z)
{
    UINT skip = 0, srcLevels, dstLevels, levels, pass;
    if (!triton9ResourceBelongsToDevice(device, source) || !triton9ResourceBelongsToDevice(device, destination) ||
        source->isCube != destination->isCube || source->isVolume != destination->isVolume ||
        source->format != destination->format || (!source->isCube && face) || face >= (source->isCube ? 6u : 1u))
        return D3DDDIERR_INVALIDCALL;
    srcLevels = source->exposedMipLevels ? source->exposedMipLevels : 1;
    dstLevels = destination->exposedMipLevels ? destination->exposedMipLevels : 1;
    while (skip < srcLevels && ((source->width >> skip) > destination->width ||
        (source->height >> skip) > destination->height || (source->depth >> skip) > destination->depth)) ++skip;
    if (skip == srcLevels ||
        (source->wantsAutogenMipmap && !destination->wantsAutogenMipmap))
        return D3DDDIERR_INVALIDCALL;
    {
        TRITON9_RESOURCE *matching = triton9ResourceSurface(source, skip);
        if (!matching || matching->width != destination->width ||
            matching->height != destination->height || matching->depth != destination->depth)
            return D3DDDIERR_INVALIDCALL;
    }
    levels = srcLevels - skip < dstLevels ? srcLevels - skip : dstLevels;
    for (pass = 0; pass < 2; ++pass) for (UINT level = 0; level < levels; ++level) {
        UINT shift = level + skip;
        TRITON9_RESOURCE *src = triton9ResourceSurface(source, face * srcLevels + shift);
        TRITON9_RESOURCE *dst = triton9ResourceSurface(destination, face * dstLevels + level);
        D3D11_BOX scaled;
        UINT dx = x >> level, dy = y >> level, dz = z >> level;
        UINT bw, bh;
        HRESULT hr;
        if (!src || !dst) return D3DDDIERR_INVALIDCALL;
        bw = src->blockWidth ? src->blockWidth : 1; bh = src->blockHeight ? src->blockHeight : 1;
        scaled.left = (box.left >> shift) / bw * bw;
        scaled.top = (box.top >> shift) / bh * bh; scaled.front = box.front >> shift;
        scaled.right = ((box.right + (1u << shift) - 1) >> shift);
        scaled.bottom = ((box.bottom + (1u << shift) - 1) >> shift);
        scaled.back = (box.back + (1u << shift) - 1) >> shift;
        scaled.right = (scaled.right + bw - 1) / bw * bw;
        scaled.bottom = (scaled.bottom + bh - 1) / bh * bh;
        if (scaled.right > src->width) scaled.right = src->width;
        if (scaled.bottom > src->height) scaled.bottom = src->height;
        if (scaled.back > src->depth) scaled.back = src->depth;
        dx = dx / bw * bw; dy = dy / bh * bh;
        hr = triton9CopyBox(device, src, dst, &scaled, dx, dy, dz, pass == 0);
        if (FAILED(hr)) return hr;
    }
    return S_OK;
}

HRESULT APIENTRY
triton9TexBlt(HANDLE hDevice, const D3DDDIARG_TEXBLT *args)
{
    TRITON9_DEVICE *device = (TRITON9_DEVICE *)hDevice;
    D3D11_BOX box;
    TRITON9_RESOURCE *source, *destination;
    if (!device || !args) return E_INVALIDARG;
    if (device->deviceLost) return D3DDDIERR_DEVICEREMOVED;
    source = (TRITON9_RESOURCE *)args->hSrcResource;
    destination = (TRITON9_RESOURCE *)args->hDstResource;
    if (!source || !destination || source->isVolume || destination->isVolume ||
        args->SrcRect.left < 0 || args->SrcRect.top < 0 || args->SrcRect.right <= args->SrcRect.left ||
        args->SrcRect.bottom <= args->SrcRect.top || args->DstPoint.x < 0 || args->DstPoint.y < 0)
        return D3DDDIERR_INVALIDCALL;
    box.left = args->SrcRect.left; box.top = args->SrcRect.top; box.front = 0;
    box.right = args->SrcRect.right; box.bottom = args->SrcRect.bottom; box.back = 1;
    if (!triton9ResourceBoxValid(source, &box)) return D3DDDIERR_INVALIDCALL;
    return triton9CopyTextureChain(device, source, destination, args->CubeMapFace, box,
                                   args->DstPoint.x, args->DstPoint.y, 0);
}

HRESULT APIENTRY
triton9VolBlt(HANDLE hDevice, const D3DDDIARG_VOLUMEBLT *args)
{
    TRITON9_DEVICE *device = (TRITON9_DEVICE *)hDevice;
    TRITON9_RESOURCE *source, *destination;
    D3D11_BOX box;
    if (!device || !args) return E_INVALIDARG;
    if (device->deviceLost) return D3DDDIERR_DEVICEREMOVED;
    source = (TRITON9_RESOURCE *)args->hSrcResource; destination = (TRITON9_RESOURCE *)args->hDstResource;
    if (!source || !destination || !source->isVolume || !destination->isVolume) return D3DDDIERR_INVALIDCALL;
    box.left = args->SrcBox.Left; box.top = args->SrcBox.Top; box.front = args->SrcBox.Front;
    box.right = args->SrcBox.Right; box.bottom = args->SrcBox.Bottom; box.back = args->SrcBox.Back;
    if (!triton9ResourceBoxValid(source, &box)) return D3DDDIERR_INVALIDCALL;
    return triton9CopyTextureChain(device, source, destination, 0, box, args->DstX, args->DstY, args->DstZ);
}

HRESULT APIENTRY
triton9ColorFill(HANDLE hDevice, const D3DDDIARG_COLORFILL *args)
{
    TRITON9_DEVICE *device = (TRITON9_DEVICE *)hDevice;
    TRITON9_RESOURCE *resource;
    ID3D11RenderTargetView *view;
    FLOAT color[4] = {
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
    triton9ResourceWritten(resource);
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
    HRESULT hr;
    if (!device || !args || !(resource = (TRITON9_RESOURCE *)args->hResource)) return E_INVALIDARG;
    resource = triton9ResourceRoot(resource);
    if (!triton9ResourceBelongsToDevice(device, resource)) return D3DDDIERR_INVALIDCALL;
    if (device->deviceLost) return D3DDDIERR_DEVICEREMOVED;
    if (!resource->wantsAutogenMipmap) return S_OK;
    if (args->Filter != D3DDDITEXF_POINT && args->Filter != D3DDDITEXF_LINEAR &&
        args->Filter != D3DDDITEXF_ANISOTROPIC) return D3DDDIERR_INVALIDCALL;
    resource->autogenFilter = args->Filter;
    if (resource->mipLevels < 2) return S_OK;
    if (resource->isVolume || resource->isDepthStencil || triton9ResourceIsSystemMemory(resource))
        return D3DDDIERR_INVALIDCALL;
    hr = triton9EnsureResourceHost(device, resource);
    if (FAILED(hr)) return hr;
    EnterCriticalSection(&device->shaderLock);
    resource->autogenGenerating = TRUE;
    ID3D11DeviceContext1_OMSetRenderTargets(device->hostContext, 0, NULL, NULL);
    if (args->Filter == D3DDDITEXF_POINT) {
        for (UINT face = 0; face < resource->arraySize && SUCCEEDED(hr); ++face) {
            for (UINT mip = 1; mip < resource->mipLevels && SUCCEEDED(hr); ++mip) {
                TRITON9_RESOURCE src = *resource, dst = *resource;
                RECT from, to;
                src.width = resource->width >> (mip - 1); if (!src.width) src.width = 1;
                src.height = resource->height >> (mip - 1); if (!src.height) src.height = 1;
                dst.width = resource->width >> mip; if (!dst.width) dst.width = 1;
                dst.height = resource->height >> mip; if (!dst.height) dst.height = 1;
                src.subresourceIndex = face * resource->mipLevels + mip - 1;
                dst.subresourceIndex = src.subresourceIndex + 1;
                from.left = from.top = to.left = to.top = 0;
                from.right = src.width; from.bottom = src.height;
                to.right = dst.width; to.bottom = dst.height;
                hr = triton9StretchBlt(device, &src, &dst, &from, &to, FALSE);
            }
        }
    } else {
        hr = triton9GetShaderResourceViewEx(device, resource, FALSE, &view);
        if (SUCCEEDED(hr)) ID3D11DeviceContext1_GenerateMips(device->hostContext, view);
    }
    resource->autogenGenerating = FALSE;
    if (SUCCEEDED(hr)) {
        triton9ResourceWritten(resource);
        resource->autogenDirty = FALSE;
    }
    {
        HRESULT restore = triton9BindOutputs(device);
        if (SUCCEEDED(hr)) hr = restore;
    }
    if (SUCCEEDED(hr)) hr = triton9CheckHostDevice(device);
    LeaveCriticalSection(&device->shaderLock);
    return triton9MapDeviceFailure(device, hr);
}

#endif /* !TRITON9_CLEAR_CONTRACT_PORTABLE */
