/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Exact-signature fallbacks for Vista D3D9 callbacks that are outside the
 * current Aero milestone.  The D3D9 UMD ABI is stdcall on x86.  Assigning a
 * generic, cast callback would corrupt the stack when the runtime calls it.
 */

#include "triton9.h"

static HRESULT
triton9UnsupportedResult(HANDLE hDevice, const void *args)
{
    TRITON9_DEVICE *device = (TRITON9_DEVICE *)hDevice;

    if (!device || !args)
        return E_INVALIDARG;
    if (device->deviceLost)
        return D3DDDIERR_DEVICEREMOVED;
    return D3DDDIERR_NOTAVAILABLE;
}

#define TRITON9_UNSUPPORTED(name, parameters) \
    static HRESULT APIENTRY name parameters \
    { \
        triton9Diag("TRITON9-UNSUPPORTED " #name "\n"); \
        return triton9UnsupportedResult(hDevice, args); \
    }

/* Vista calls UpdateWInfo while it publishes a newly-created D3D9 device.
 * Triton presents through the WDDM Present DDI and does not retain a separate
 * window binding, so this is intentionally a validated no-op.  Treating this
 * required bookkeeping callback as NOTAVAILABLE makes the public runtime
 * abandon CreateDeviceEx before it can issue its first resource operation. */
static HRESULT APIENTRY
triton9UpdateWInfo(HANDLE hDevice, const D3DDDIARG_WINFO *args)
{
    TRITON9_DEVICE *device = (TRITON9_DEVICE *)hDevice;

    if (!device || !args)
        return E_INVALIDARG;
    if (device->deviceLost)
        return D3DDDIERR_DEVICEREMOVED;
    return S_OK;
}
TRITON9_UNSUPPORTED(triton9DrawRectPatch,
                    (HANDLE hDevice, const D3DDDIARG_DRAWRECTPATCH *args,
                     const D3DDDIRECTPATCH_INFO *info, const FLOAT *data))
TRITON9_UNSUPPORTED(triton9DrawTriPatch,
                    (HANDLE hDevice, const D3DDDIARG_DRAWTRIPATCH *args,
                     const D3DDDITRIPATCH_INFO *info, const FLOAT *data))
TRITON9_UNSUPPORTED(triton9VolBlt,
                    (HANDLE hDevice, const D3DDDIARG_VOLUMEBLT *args))
#ifndef D3DHAL_STATESETCREATE
#define D3DHAL_STATESETCREATE 5u
#endif

static HRESULT APIENTRY
triton9StateSet(HANDLE hDevice, D3DDDIARG_STATESET *args)
{
    TRITON9_DEVICE *device = (TRITON9_DEVICE *)hDevice;

    if (!device || !args)
        return E_INVALIDARG;
    if (device->deviceLost)
        return D3DDDIERR_DEVICEREMOVED;
    if (args->Operation > D3DHAL_STATESETCREATE)
        return D3DDDIERR_INVALIDCALL;
    triton9DiagU32("TRITON9-STATESET-OP", args->Operation);
    /* Standard state-block commands are runtime bookkeeping. Triton exposes
     * no private or extended render state to append, so the runtime owns the
     * block and its handle while the existing DDI setters maintain Triton's
     * state shadow. This is the same no-private-state contract used by
     * Microsoft's D3D9on12 UMD. */
    return S_OK;
}

/* WDDM 1.0 priority changes are advisory for allocations that VidMm owns.
 * Triton does not evict resources in its single host context, so accepting a
 * valid hint has no state or output to emulate. */
static HRESULT APIENTRY
triton9SetPriority(HANDLE hDevice, const D3DDDIARG_SETPRIORITY *args)
{
    TRITON9_DEVICE *device = (TRITON9_DEVICE *)hDevice;

    if (!device || !args || !args->hResource)
        return E_INVALIDARG;
    if (device->deviceLost)
        return D3DDDIERR_DEVICEREMOVED;
    return S_OK;
}
TRITON9_UNSUPPORTED(triton9UpdatePalette,
                    (HANDLE hDevice, const D3DDDIARG_UPDATEPALETTE *args,
                     const PALETTEENTRY *entries))
TRITON9_UNSUPPORTED(triton9SetPalette,
                    (HANDLE hDevice, const D3DDDIARG_SETPALETTE *args))
/* MIL programs a material as part of the canonical hardware-device state
 * block, even though that block immediately disables lighting for its 2D
 * composition draws.  Preserve the value rather than treating the callback
 * as an unsupported rendering feature; this is required device state and is
 * the natural storage point for the eventual fixed-function lighting path. */
static HRESULT APIENTRY
triton9SetMaterial(HANDLE hDevice, const D3DDDIARG_SETMATERIAL *args)
{
    TRITON9_DEVICE *device = (TRITON9_DEVICE *)hDevice;

    if (!device || !args || !device->shaderLockInitialized)
        return E_INVALIDARG;
    if (device->deviceLost)
        return D3DDDIERR_DEVICEREMOVED;
    EnterCriticalSection(&device->shaderLock);
    device->material = *args;
    LeaveCriticalSection(&device->shaderLock);
    return S_OK;
}
TRITON9_UNSUPPORTED(triton9SetLight,
                    (HANDLE hDevice, const D3DDDIARG_SETLIGHT *args,
                     const D3DDDI_LIGHT *light))
TRITON9_UNSUPPORTED(triton9CreateLight,
                    (HANDLE hDevice, const D3DDDIARG_CREATELIGHT *args))
TRITON9_UNSUPPORTED(triton9DestroyLight,
                    (HANDLE hDevice, const D3DDDIARG_DESTROYLIGHT *args))
/* Do not make public device creation depend on the old clip-plane callback.
 * Triton reports no usable user clip planes, and SetRenderState rejects any
 * nonzero CLIPPLANEENABLE mask, but Vista still initializes this ABI field
 * while publishing a device.  Validate the device and plane slot, then keep
 * it a state-only no-op; it cannot affect a draw on the current path. */
static HRESULT APIENTRY
triton9SetClipPlane(HANDLE hDevice, const D3DDDIARG_SETCLIPPLANE *args)
{
    TRITON9_DEVICE *device = (TRITON9_DEVICE *)hDevice;

    if (!device || !args || args->Index >= 6)
        return E_INVALIDARG;
    if (device->deviceLost)
        return D3DDDIERR_DEVICEREMOVED;
    return S_OK;
}

/* The public Vista D3D9 runtime asks every HAL for its vertex-cache model
 * as it finishes creating a software-vertex-processing device.  This query
 * is not optional: returning NOTAVAILABLE after accepting the dynamic
 * internal buffers makes CreateDeviceEx fail with a generic E_FAIL.
 *
 * Triton does not expose a host-specific post-transform cache.  The
 * documented "longest strips" result accurately tells the runtime not to
 * apply cache-size-specific triangle reordering.  This is the same narrow
 * Vista DDI contract used by the contemporary VirtualBox WDDM driver, but
 * remains wholly within Triton's own D3D9 UMD architecture. */
static HRESULT APIENTRY
triton9GetInfo(HANDLE hDevice, UINT id, VOID *data, UINT dataSize)
{
    TRITON9_DEVICE *device = (TRITON9_DEVICE *)hDevice;
    D3DDDIDEVINFO_VCACHE *cache;

    if (!device || !data)
        return E_INVALIDARG;
    if (device->deviceLost)
        return D3DDDIERR_DEVICEREMOVED;
    triton9DiagU32("TRITON9-GETINFO-ID", id);
    triton9DiagU32("TRITON9-GETINFO-SIZE", dataSize);
    if (id != D3DDDIDEVINFOID_VCACHE) {
        triton9Diag("TRITON9-GETINFO-NOTAVAILABLE\n");
        return D3DDDIERR_NOTAVAILABLE;
    }
    if (dataSize != sizeof(D3DDDIDEVINFO_VCACHE))
        return E_INVALIDARG;
    cache = (D3DDDIDEVINFO_VCACHE *)data;
    cache->Pattern = 0x48434143u; /* MAKEFOURCC('C', 'A', 'C', 'H') */
    cache->OptMethod = 0;         /* optimize for longest strips */
    cache->CacheSize = 0;
    cache->MagicNumber = 0;
    triton9Diag("TRITON9-GETINFO-VCACHE success\n");
    return S_OK;
}
TRITON9_UNSUPPORTED(triton9SetConvolutionKernelMono,
                    (HANDLE hDevice, const D3DDDIARG_SETCONVOLUTIONKERNELMONO *args))
TRITON9_UNSUPPORTED(triton9ComposeRects,
                    (HANDLE hDevice, const D3DDDIARG_COMPOSERECTS *args))
TRITON9_UNSUPPORTED(triton9DepthFill,
                    (HANDLE hDevice, const D3DDDIARG_DEPTHFILL *args))
TRITON9_UNSUPPORTED(triton9GetCaptureAllocationHandle,
                    (HANDLE hDevice, D3DDDIARG_GETCAPTUREALLOCATIONHANDLE *args))
TRITON9_UNSUPPORTED(triton9CaptureToSysMem,
                    (HANDLE hDevice, const D3DDDIARG_CAPTURETOSYSMEM *args))

void
triton9InstallUnsupportedDeviceFuncs(D3DDDI_DEVICEFUNCS *funcs)
{
    if (!funcs)
        return;

    funcs->pfnUpdateWInfo = triton9UpdateWInfo;
    funcs->pfnDrawRectPatch = triton9DrawRectPatch;
    funcs->pfnDrawTriPatch = triton9DrawTriPatch;
    funcs->pfnVolBlt = triton9VolBlt;
    funcs->pfnStateSet = triton9StateSet;
    funcs->pfnSetPriority = triton9SetPriority;
    funcs->pfnUpdatePalette = triton9UpdatePalette;
    funcs->pfnSetPalette = triton9SetPalette;
    funcs->pfnSetMaterial = triton9SetMaterial;
    funcs->pfnSetLight = triton9SetLight;
    funcs->pfnCreateLight = triton9CreateLight;
    funcs->pfnDestroyLight = triton9DestroyLight;
    funcs->pfnSetClipPlane = triton9SetClipPlane;
    funcs->pfnGetInfo = triton9GetInfo;
    funcs->pfnSetConvolutionKernelMono = triton9SetConvolutionKernelMono;
    funcs->pfnComposeRects = triton9ComposeRects;
    funcs->pfnDepthFill = triton9DepthFill;
    funcs->pfnGetCaptureAllocationHandle = triton9GetCaptureAllocationHandle;
    funcs->pfnCaptureToSysMem = triton9CaptureToSysMem;
}
