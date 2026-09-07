/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Compile this source once for x86 and once for x64.  It protects the
 * Vista-only D3D9 ABI from a later global WDDM interface-version change.
 */

#include "../triton9.h"
#include "../../triton/tritonDxbcSignature.h"

#include <stddef.h>

_Static_assert(D3D_UMD_INTERFACE_VERSION == D3D_UMD_INTERFACE_VERSION_VISTA,
               "Vista D3D9 UMD must use the Vista callback table");
_Static_assert(DXGKDDI_INTERFACE_VERSION == DXGKDDI_INTERFACE_VERSION_VISTA,
               "Vista D3D9 UMD must use the Vista KMD ABI");
_Static_assert(D3D_UMD_INTERFACE_VERSION_VISTA == 0x000c,
               "Vista UMD DriverVersion changed");
_Static_assert(sizeof(D3DCAPS9) == 0x130,
               "Vista D3DCAPS9 layout changed");
_Static_assert(sizeof(FORMATOP) == 20,
               "Vista FORMATOP layout changed");
_Static_assert(D3DDDICAPS_GETFORMATCOUNT == 3,
               "Vista format-count query value changed");
_Static_assert(D3DDDICAPS_GETFORMATDATA == 4,
               "Vista format-data query value changed");
_Static_assert(D3DDDICAPS_GETD3D8CAPS == 12,
               "Vista D3D8-caps query value changed");
_Static_assert(D3DDDICAPS_GETD3D9CAPS == 13,
               "Vista D3D9-caps query value changed");
_Static_assert(FIELD_OFFSET(D3DCAPS9, DevCaps2) < sizeof(D3DCAPS9),
               "Vista D3D8-compatible caps prefix changed");
_Static_assert(sizeof(D3DDDI_ADAPTERFUNCS) == 3 * sizeof(void *),
               "Vista adapter callback table changed");
_Static_assert(offsetof(D3DDDI_ADAPTERFUNCS, pfnGetCaps) == 0,
               "Vista GetCaps function offset changed");
_Static_assert(offsetof(D3DDDI_ADAPTERFUNCS, pfnCreateDevice) ==
               sizeof(void *),
               "Vista CreateDevice function offset changed");
_Static_assert(offsetof(D3DDDI_ADAPTERFUNCS, pfnCloseAdapter) ==
               2 * sizeof(void *),
               "Vista CloseAdapter function offset changed");
_Static_assert(sizeof(D3DDDI_ADAPTERCALLBACKS) == 2 * sizeof(void *),
               "Vista adapter runtime-callback table changed");
_Static_assert(offsetof(D3DDDI_ADAPTERCALLBACKS, pfnQueryAdapterInfoCb) == 0,
               "Vista adapter-info callback offset changed");
_Static_assert(offsetof(D3DDDI_ADAPTERCALLBACKS,
                        pfnGetMultisampleMethodListCb) == sizeof(void *),
               "Vista multisample callback offset changed");
_Static_assert(sizeof(D3DDDI_DEVICEFUNCS) == 99 * sizeof(void *),
               "Vista D3D9 device function table changed");
_Static_assert(offsetof(D3DDDI_DEVICEFUNCS, pfnUpdateWInfo) ==
               1 * sizeof(void *),
               "Vista UpdateWInfo function offset changed");
_Static_assert(offsetof(D3DDDI_DEVICEFUNCS, pfnDrawPrimitive) ==
               10 * sizeof(void *),
               "Vista DrawPrimitive function offset changed");
_Static_assert(offsetof(D3DDDI_DEVICEFUNCS, pfnDrawIndexedPrimitive) ==
               11 * sizeof(void *),
               "Vista DrawIndexedPrimitive function offset changed");
_Static_assert(offsetof(D3DDDI_DEVICEFUNCS, pfnDrawPrimitive2) ==
               14 * sizeof(void *),
               "Vista DrawPrimitive2 function offset changed");
_Static_assert(offsetof(D3DDDI_DEVICEFUNCS, pfnDrawIndexedPrimitive2) ==
               15 * sizeof(void *),
               "Vista DrawIndexedPrimitive2 function offset changed");
_Static_assert(offsetof(D3DDDI_DEVICEFUNCS, pfnStateSet) ==
               19 * sizeof(void *),
               "Vista StateSet function offset changed");
_Static_assert(offsetof(D3DDDI_DEVICEFUNCS, pfnSetPriority) ==
               20 * sizeof(void *),
               "Vista SetPriority function offset changed");
_Static_assert(offsetof(D3DDDI_DEVICEFUNCS, pfnClear) ==
               21 * sizeof(void *),
               "Vista Clear function offset changed");
_Static_assert(offsetof(D3DDDI_DEVICEFUNCS, pfnGetInfo) ==
               34 * sizeof(void *),
               "Vista GetInfo function offset changed");
_Static_assert(offsetof(D3DDDI_DEVICEFUNCS, pfnPresent) ==
               40 * sizeof(void *),
               "Vista Present function offset changed");
_Static_assert(offsetof(D3DDDI_DEVICEFUNCS, pfnColorFill) ==
               56 * sizeof(void *),
               "Vista ColorFill function offset changed");
_Static_assert(offsetof(D3DDDI_DEVICEFUNCS, pfnDepthFill) ==
               57 * sizeof(void *),
               "Vista DepthFill function offset changed");
_Static_assert(offsetof(D3DDDI_DEVICEFUNCS, pfnSetRenderTarget) ==
               62 * sizeof(void *),
               "Vista render-target function offset changed");
_Static_assert(offsetof(D3DDDI_DEVICEFUNCS, pfnSetDepthStencil) ==
               63 * sizeof(void *),
               "Vista depth-stencil function offset changed");
_Static_assert(offsetof(D3DDDI_DEVICEFUNCS, pfnGenerateMipSubLevels) ==
               64 * sizeof(void *),
               "Vista GenerateMipSubLevels function offset changed");
_Static_assert(offsetof(D3DDDI_DEVICEFUNCS, pfnDestroyDevice) ==
               91 * sizeof(void *),
               "Vista DestroyDevice function offset changed");
_Static_assert(offsetof(D3DDDI_DEVICEFUNCS, pfnOpenResource) ==
               93 * sizeof(void *),
               "Vista OpenResource function offset changed");
_Static_assert(offsetof(D3DDDI_DEVICEFUNCS, pfnGetCaptureAllocationHandle) ==
               94 * sizeof(void *),
               "Vista capture-handle function offset changed");
_Static_assert(offsetof(D3DDDI_DEVICEFUNCS, pfnCaptureToSysMem) ==
               95 * sizeof(void *),
               "Vista capture-to-system-memory function offset changed");
_Static_assert(offsetof(D3DDDI_DEVICEFUNCS, pfnLockAsync) ==
               96 * sizeof(void *),
               "Vista LockAsync function offset changed");
_Static_assert(offsetof(D3DDDI_DEVICEFUNCS, pfnRename) ==
               98 * sizeof(void *),
               "Vista Rename function offset changed");
/* The private Neptune transport receives this table through CreateDevice.
 * Pin the Vista prefix we consume: an offset error here reads a different
 * callback and is especially destructive on x86, where it also corrupts the
 * stdcall stack.  Vista's table ends after SetDisplayPrivateDriverFormatCb. */
_Static_assert(offsetof(D3DDDI_DEVICECALLBACKS, pfnAllocateCb) ==
               0 * sizeof(void *),
               "Vista Allocate callback offset changed");
_Static_assert(offsetof(D3DDDI_DEVICECALLBACKS, pfnDeallocateCb) ==
               1 * sizeof(void *),
               "Vista Deallocate callback offset changed");
_Static_assert(offsetof(D3DDDI_DEVICECALLBACKS, pfnSetDisplayModeCb) ==
               4 * sizeof(void *),
               "Vista SetDisplayMode callback offset changed");
_Static_assert(offsetof(D3DDDI_DEVICECALLBACKS, pfnPresentCb) ==
               5 * sizeof(void *),
               "Vista Present callback offset changed");
_Static_assert(offsetof(D3DDDI_DEVICECALLBACKS, pfnRenderCb) ==
               6 * sizeof(void *),
               "Vista Render callback offset changed");
_Static_assert(offsetof(D3DDDI_DEVICECALLBACKS, pfnLockCb) ==
               7 * sizeof(void *),
               "Vista Lock callback offset changed");
_Static_assert(offsetof(D3DDDI_DEVICECALLBACKS, pfnUnlockCb) ==
               8 * sizeof(void *),
               "Vista Unlock callback offset changed");
_Static_assert(offsetof(D3DDDI_DEVICECALLBACKS, pfnEscapeCb) ==
               9 * sizeof(void *),
               "Vista Escape callback offset changed");
_Static_assert(offsetof(D3DDDI_DEVICECALLBACKS, pfnCreateContextCb) ==
               14 * sizeof(void *),
               "Vista CreateContext callback offset changed");
_Static_assert(offsetof(D3DDDI_DEVICECALLBACKS, pfnDestroyContextCb) ==
               15 * sizeof(void *),
               "Vista DestroyContext callback offset changed");
_Static_assert(sizeof(D3DDDI_DEVICECALLBACKS) == 22 * sizeof(void *),
               "Vista device callback table changed");
_Static_assert(sizeof(D3DDDI_CREATEDEVICEFLAGS) == sizeof(UINT),
               "Vista CreateDevice flag layout changed");
_Static_assert(sizeof(D3DDDIARG_OPENADAPTER) ==
               (sizeof(void *) == 8 ? 40 : 24),
               "Vista OpenAdapter argument layout changed");
_Static_assert(offsetof(D3DDDIARG_OPENADAPTER, Interface) == sizeof(void *),
               "Vista OpenAdapter interface offset changed");
_Static_assert(offsetof(D3DDDIARG_OPENADAPTER, Version) ==
               sizeof(void *) + sizeof(UINT),
               "Vista OpenAdapter runtime-version offset changed");
_Static_assert(offsetof(D3DDDIARG_OPENADAPTER, DriverVersion) ==
               (sizeof(void *) == 8 ? 32 : 20),
               "Vista OpenAdapter driver-version offset changed");
_Static_assert(sizeof(D3DDDIARG_CREATEDEVICE) ==
               (sizeof(void *) == 8 ? 88 : 48),
               "Vista CreateDevice argument layout changed");
_Static_assert(offsetof(D3DDDIARG_CREATEDEVICE, Interface) == sizeof(void *),
               "Vista CreateDevice interface offset changed");
_Static_assert(offsetof(D3DDDIARG_CREATEDEVICE, Version) ==
               sizeof(void *) + sizeof(UINT),
               "Vista CreateDevice runtime-version offset changed");
_Static_assert(offsetof(D3DDDIARG_CREATEDEVICE, Flags) ==
               (sizeof(void *) == 8 ? 80 : 44),
               "Vista CreateDevice flags offset changed");
_Static_assert(sizeof(D3DDDIARG_GETCAPS) ==
               (sizeof(void *) == 8 ? 32 : 16),
               "Vista GetCaps argument layout changed");
_Static_assert(sizeof(DDIMULTISAMPLEQUALITYLEVELSDATA) == 16,
               "Vista multisample-caps layout changed");
_Static_assert(sizeof(DDIGAMMACAPS) == sizeof(UINT),
               "Vista gamma-caps layout changed");
_Static_assert(sizeof(D3DCONTENTPROTECTIONCAPS) ==
               (sizeof(void *) == 8 ? 40 : 36),
               "Vista content-protection caps layout changed");
_Static_assert(sizeof(D3DDDIARG_CLEAR) == 16,
               "Vista Clear argument layout changed");
_Static_assert(sizeof(D3DDDIARG_PRESENT) ==
               (sizeof(void *) == 8 ? 40 : 24),
               "Vista Present argument layout changed");
_Static_assert(offsetof(D3DDDIARG_OPENADAPTER, DriverVersion) >
               offsetof(D3DDDIARG_OPENADAPTER, pAdapterFuncs),
               "Vista OpenAdapter fields changed");
_Static_assert(offsetof(D3DDDIARG_CREATEDEVICE, pDeviceFuncs) >
               offsetof(D3DDDIARG_CREATEDEVICE, pCallbacks),
               "Vista CreateDevice fields changed");
_Static_assert(offsetof(D3DDDICB_ALLOCATE, pAllocationInfo) >
               offsetof(D3DDDICB_ALLOCATE, NumAllocations),
               "Vista allocation callback changed");
_Static_assert(D3DDDIPOOL_SYSTEMMEM == 1,
               "Vista SYSTEMMEM pool value changed");
_Static_assert(sizeof(D3DDDI_LOCKFLAGS) == sizeof(UINT),
               "Vista lock flags layout changed");
_Static_assert(offsetof(D3DDDI_SURFACEINFO, pSysMem) ==
               (sizeof(void *) == 8 ? 16 : 12),
               "Vista system-memory pointer offset changed");
_Static_assert(offsetof(D3DDDI_SURFACEINFO, SysMemPitch) ==
               offsetof(D3DDDI_SURFACEINFO, pSysMem) + sizeof(void *),
               "Vista system-memory pitch offset changed");
_Static_assert(offsetof(D3DDDI_SURFACEINFO, SysMemSlicePitch) ==
               offsetof(D3DDDI_SURFACEINFO, SysMemPitch) + sizeof(UINT),
               "Vista system-memory slice-pitch offset changed");
_Static_assert(sizeof(D3DDDI_SURFACEINFO) ==
               (sizeof(void *) == 8 ? 32 : 24),
               "Vista system-memory surface layout changed");
_Static_assert(offsetof(TRITON_DXBC_SIGNATURE, semanticName) == 0,
               "normalized signature name offset changed");
_Static_assert(offsetof(TRITON_DXBC_SIGNATURE, semanticIndex) == sizeof(void *),
               "normalized signature semantic index offset changed");
_Static_assert(offsetof(TRITON_DXBC_SIGNATURE, registerIdx) ==
               sizeof(void *) + 2 * sizeof(UINT),
               "normalized signature register offset changed");
_Static_assert(offsetof(TRITON_DXBC_SIGNATURE, componentType) ==
               sizeof(void *) + 4 * sizeof(UINT),
               "normalized signature component type offset changed");

/* These assignments make both cross-builds type-check the complete stdcall
 * signature.  A generic unsupported callback would corrupt Vista x86's stack
 * when the runtime invokes one of these UP paths. */
typedef BOOL (*TRITON9_D24S8_CONTRACT_FN)(void);

static PFND3DDDI_OPENADAPTER const gOpenAdapter = OpenAdapter;
static PFND3DDDI_SETSTREAMSOURCEUM const gSetStreamSourceUm =
    triton9SetStreamSourceUm;
static PFND3DDDI_SETSTREAMSOURCEFREQ const gSetStreamSourceFreq =
    triton9SetStreamSourceFreq;
static PFND3DDDI_CREATERESOURCE const gCreateResource = triton9CreateResource;
static PFND3DDDI_LOCK const gLock = triton9Lock;
static PFND3DDDI_UNLOCK const gUnlock = triton9Unlock;
static PFND3DDDI_BLT const gBlt = triton9Blt;
static PFND3DDDI_BUFBLT const gBufBlt = triton9BufBlt;
static PFND3DDDI_TEXBLT const gTexBlt = triton9TexBlt;
static PFND3DDDI_LOCKASYNC const gLockAsync = triton9LockAsync;
static PFND3DDDI_UNLOCKASYNC const gUnlockAsync = triton9UnlockAsync;
static PFND3DDDI_RENAME const gRename = triton9Rename;
static PFND3DDDI_SETINDICESUM const gSetIndicesUm = triton9SetIndicesUm;
static PFND3DDDI_DRAWPRIMITIVE2 const gDrawPrimitive2 = triton9DrawPrimitive2;
static PFND3DDDI_DRAWINDEXEDPRIMITIVE2 const gDrawIndexedPrimitive2 =
    triton9DrawIndexedPrimitive2;
static PFND3DDDI_DRAWPRIMITIVE const gDrawPrimitive = triton9DrawPrimitive;
static PFND3DDDI_DRAWINDEXEDPRIMITIVE const gDrawIndexedPrimitive =
    triton9DrawIndexedPrimitive;
static PFND3DDDI_CLEAR const gClear = triton9Clear;
static PFND3DDDI_SETDISPLAYMODE const gSetDisplayMode = triton9SetDisplayMode;
static PFND3DDDI_PRESENT const gPresent = triton9Present;
static PFND3DDDI_SETRENDERTARGET const gSetRenderTarget =
    triton9SetRenderTarget;
static PFND3DDDI_SETDEPTHSTENCIL const gSetDepthStencil =
    triton9SetDepthStencil;
static PFND3DDDI_COLORFILL const gColorFill = triton9ColorFill;
static PFND3DDDI_GENERATEMIPSUBLEVELS const gGenerateMipSubLevels =
    triton9GenerateMipSubLevels;
static TRITON9_D24S8_CONTRACT_FN const gD24S8Contract =
    triton9HasCompleteD24S8ClearContract;

static int triton9AbiCompileProbe(void)
{
    TRITON_DXBC_SIGNATURE normalized = {
        "POSITION", 0, 0, 3, 0x0f, 0, 3, 0
    };
    TRITON_DXBC_SIGNATURE decoded;

    if (!gOpenAdapter || !gSetStreamSourceUm || !gSetStreamSourceFreq ||
        !gCreateResource ||
        !gLock || !gUnlock || !gBlt || !gBufBlt || !gTexBlt || !gLockAsync ||
        !gUnlockAsync || !gRename || !gSetIndicesUm || !gDrawPrimitive2 ||
        !gDrawIndexedPrimitive2 || !gDrawPrimitive || !gDrawIndexedPrimitive ||
        !gClear || !gSetDisplayMode || !gPresent || !gSetRenderTarget ||
        !gSetDepthStencil || !gColorFill || !gGenerateMipSubLevels ||
        !gD24S8Contract)
        return 1;
    if (!tritonDxbcSignatureFromDdi(&normalized, sizeof(normalized), 0,
                                    &decoded))
        return 2;
    if (decoded.semanticName != normalized.semanticName ||
        decoded.registerIdx != normalized.registerIdx ||
        decoded.componentType != normalized.componentType)
        return 3;
    if (tritonDxbcSignatureFromDdi(NULL, sizeof(normalized), 0, &decoded))
        return 4;
    return 0;
}
