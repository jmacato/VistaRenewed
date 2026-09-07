/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Vista D3D9 adapter and device entry points.  Unlike the D3D10/11 DDI, the
 * D3D9 runtime does not allocate pDrvPrivate storage.  The in/out HANDLEs in
 * D3DDDIARG_OPENADAPTER and D3DDDIARG_CREATEDEVICE are consequently replaced
 * with heap-owned TRITON9_ADAPTER and TRITON9_DEVICE pointers here.
 */

#include "triton9.h"
#include "triton9_legacy_caps.h"

#include <string.h>

/* Development exception telemetry observes faults; it never handles them or
 * changes Windows runtime code. Remove the callback before this DLL unloads. */
static PVOID triton9ExceptionHandler;
static LONG triton9ExceptionRecorded;

static void
triton9DiagPointer(const char *tag, ULONG_PTR value)
{
    triton9Diag(tag);
    triton9DiagU32("TRITON9-FAULT-PTR-LO", (DWORD)value);
#ifdef _WIN64
    triton9DiagU32("TRITON9-FAULT-PTR-HI", (DWORD)(value >> 32));
#endif
}

#ifdef _WIN64
static void
triton9DiagObject(const char *tag, ULONG_PTR address)
{
    ULONG_PTR words[48];
    SIZE_T bytes = 0;
    unsigned i;
    triton9DiagPointer(tag, address);
    if (!address || !ReadProcessMemory(GetCurrentProcess(), (void *)address,
                                       words, sizeof(words), &bytes))
        return;
    for (i = 0; i < bytes / sizeof(words[0]); ++i) {
        triton9DiagU32("TRITON9-FAULT-OBJECT-OFFSET", i * sizeof(words[0]));
        triton9DiagPointer("TRITON9-FAULT-OBJECT-WORD\n", words[i]);
    }
}
#endif

static LONG CALLBACK
triton9ObserveException(EXCEPTION_POINTERS *exception)
{
    ULONG_PTR stack;
    ULONG_PTR words[48];
    SIZE_T bytes = 0;
    unsigned i;
    if (exception->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        InterlockedCompareExchange(&triton9ExceptionRecorded, 1, 0))
        return EXCEPTION_CONTINUE_SEARCH;
    triton9DiagU32("TRITON9-FAULT-PID", GetCurrentProcessId());
    {
        MEMORY_BASIC_INFORMATION region;
        char module[MAX_PATH];
        if (VirtualQuery(exception->ExceptionRecord->ExceptionAddress,
                         &region, sizeof(region))) {
            triton9DiagPointer("TRITON9-FAULT-MODULE-BASE\n",
                              (ULONG_PTR)region.AllocationBase);
            if (GetModuleFileNameA((HMODULE)region.AllocationBase, module, sizeof(module))) {
                triton9Diag("TRITON9-FAULT-MODULE=");
                triton9Diag(module);
                triton9Diag("\n");
            }
        }
    }
    triton9DiagPointer("TRITON9-FAULT-PC\n",
                      (ULONG_PTR)exception->ExceptionRecord->ExceptionAddress);
    triton9DiagPointer("TRITON9-FAULT-D3D9-BASE\n",
                      (ULONG_PTR)GetModuleHandleA("d3d9.dll"));
#ifdef _WIN64
    triton9DiagPointer("TRITON9-FAULT-RBX\n", exception->ContextRecord->Rbx);
    triton9DiagPointer("TRITON9-FAULT-RDI\n", exception->ContextRecord->Rdi);
    triton9DiagPointer("TRITON9-FAULT-RSI\n", exception->ContextRecord->Rsi);
    triton9DiagPointer("TRITON9-FAULT-R13\n", exception->ContextRecord->R13);
    triton9DiagPointer("TRITON9-FAULT-R15\n", exception->ContextRecord->R15);
    /* For the exact checked runtime fault under investigation, RSI
     * was saved at RSP+0xf0 by D3D9SetDisplayModeLH before being used for
     * the callback HRESULT. Recover EnableFullscreen's swap-chain slot.
     * Copy only bounded objects
     * through ReadProcessMemory, so diagnostic reads cannot cause another AV. */
    if ((ULONG_PTR)exception->ExceptionRecord->ExceptionAddress ==
        (ULONG_PTR)GetModuleHandleA("d3d9.dll") + 0xbc132) {
        ULONG_PTR swapchainSlot = 0;
        ULONG_PTR swapchain = 0;
        ULONG_PTR surfaces[2] = {0, 0};
        SIZE_T copied = 0;
        ReadProcessMemory(GetCurrentProcess(),
                          (void *)(exception->ContextRecord->Rsp + 0xf0),
                          &swapchainSlot, sizeof(swapchainSlot), &copied);
        triton9DiagPointer("TRITON9-FAULT-SAVED-RSI\n", swapchainSlot);
        if (ReadProcessMemory(GetCurrentProcess(),
                              (void *)swapchainSlot,
                              &swapchain, sizeof(swapchain), &copied) && swapchain) {
            triton9DiagObject("TRITON9-FAULT-SWAPCHAIN\n", swapchain);
            {
                ULONG_PTR runtimeDevice = 0, enumeration = 0;
                DWORD ordinal = 0;
                if (ReadProcessMemory(GetCurrentProcess(), (void *)(swapchain + 0x10),
                                      &runtimeDevice, sizeof(runtimeDevice), &copied) &&
                    runtimeDevice) {
                    triton9DiagPointer("TRITON9-FAULT-RUNTIME-DEVICE\n", runtimeDevice);
                    triton9DiagObject("TRITON9-FAULT-DEVICE-TAIL\n", runtimeDevice + 0x3800);
                    ReadProcessMemory(GetCurrentProcess(), (void *)(runtimeDevice + 0x644),
                                      &ordinal, sizeof(ordinal), &copied);
                    triton9DiagU32("TRITON9-FAULT-ORDINAL", ordinal);
                    if (ordinal < 16 && ReadProcessMemory(GetCurrentProcess(),
                            (void *)(runtimeDevice + 0x3900), &enumeration,
                            sizeof(enumeration), &copied) && enumeration) {
                        triton9DiagObject("TRITON9-FAULT-ENUM-DRIVER-CAPS\n",
                                          enumeration + ordinal * 0x308 + 0x1b0);
                        triton9DiagObject("TRITON9-FAULT-ENUM-ADAPTER-TAIL\n",
                                          enumeration + ordinal * 0x308 + 0x300);
                    }
                }
            }
            if (ReadProcessMemory(GetCurrentProcess(), (void *)(swapchain + 0x48),
                                  surfaces, sizeof(surfaces), &copied)) {
                unsigned surfaceIndex;
                for (surfaceIndex = 0; surfaceIndex < 2; ++surfaceIndex) {
                    ULONG_PTR vtable = 0;
                    triton9DiagU32("TRITON9-FAULT-SURFACE-INDEX", surfaceIndex);
                    triton9DiagObject("TRITON9-FAULT-SURFACE\n", surfaces[surfaceIndex]);
                    if (surfaces[surfaceIndex] && ReadProcessMemory(
                            GetCurrentProcess(), (void *)(surfaces[surfaceIndex] + 0x38),
                            &vtable, sizeof(vtable), &copied))
                        triton9DiagObject("TRITON9-FAULT-SURFACE-VTABLE\n", vtable);
                }
            }
        }
    }
    stack = exception->ContextRecord->Rsp;
#else
    stack = exception->ContextRecord->Esp;
#endif
    triton9DiagPointer("TRITON9-FAULT-SP\n", stack);
    if (ReadProcessMemory(GetCurrentProcess(), (void *)stack, words,
                          sizeof(words), &bytes)) {
        for (i = 0; i < bytes / sizeof(words[0]); ++i) {
            triton9DiagU32("TRITON9-FAULT-STACK-INDEX", i);
            triton9DiagPointer("TRITON9-FAULT-STACK-WORD\n", words[i]);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved);

BOOL WINAPI
DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)instance;
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
        triton9ExceptionHandler = AddVectoredExceptionHandler(0, triton9ObserveException);
    else if (reason == DLL_PROCESS_DETACH && triton9ExceptionHandler)
        RemoveVectoredExceptionHandler(triton9ExceptionHandler);
    return TRUE;
}

/* Keep a narrow breadcrumb trail in the checked-kernel capture.  Vista's
 * DWM can reject an adapter before the first Render/Present call, so the
 * absence of KMD traffic alone is not enough to distinguish a UMD contract
 * failure from a later blit problem. */
void
triton9Diag(const char *message)
{
    HANDLE file;
    DWORD written;

    if (!message)
        return;
    OutputDebugStringA(message);
    /* DWM is not elevated on Vista and cannot create a file directly under
     * C:\\ on the stock ACL. Try the root for older test images, then use the
     * writable system temp directory so the breadcrumb survives a reboot. */
    file = CreateFileA("C:\\triton9-ddi.log", GENERIC_WRITE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        file = CreateFileA("C:\\Windows\\Temp\\triton9-ddi.log", GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return;
    SetFilePointer(file, 0, NULL, FILE_END);
    WriteFile(file, message, (DWORD)strlen(message), &written, NULL);
    CloseHandle(file);
}

/* Do not use the CRT formatter for the early D3D9 probe.  Vista can call
 * this entry point before the UMD's normal CRT initialization is complete;
 * fixed-width breadcrumbs keep the rejection reason observable even then. */
void
triton9DiagU32(const char *tag, DWORD value)
{
    static const char digits[] = "0123456789abcdef";
    char message[96];
    SIZE_T length = 0;
    UINT shift;

    if (!tag)
        return;
    while (tag[length] && length + 1 < sizeof(message) - 11)
        message[length] = tag[length], length++;
    message[length++] = '=';
    for (shift = 28; ; shift -= 4) {
        message[length++] = digits[(value >> shift) & 0xf];
        if (!shift)
            break;
    }
    message[length++] = '\n';
    message[length] = '\0';
    triton9Diag(message);
}

void
triton9ProofDiagU32(const char *tag, DWORD value)
{
    static const char digits[] = "0123456789abcdef";
    char message[96];
    HANDLE file;
    DWORD written;
    SIZE_T length = 0;
    UINT shift;

    /* Preserve the normal checked-build breadcrumb too, but keep this
     * short proof record in a separate text file. The guest service tails
     * this file immediately after its D3D9 child exits; unlike the general
     * log it cannot be overwritten by concurrent DWM tracing. */
    triton9DiagU32(tag, value);
    if (!tag)
        return;
    while (tag[length] && length + 1 < sizeof(message) - 11)
        message[length] = tag[length], length++;
    message[length++] = '=';
    for (shift = 28; ; shift -= 4) {
        message[length++] = digits[(value >> shift) & 0xf];
        if (!shift)
            break;
    }
    message[length++] = '\n';
    message[length] = '\0';
    file = CreateFileA("C:\\Windows\\Temp\\triton9-d3d9-proof.log",
                       FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return;
    /* FILE_APPEND_DATA makes each short record an atomic end-of-file write
     * across DWM, WinSAT, and the secure probe.  A SetFilePointer(FILE_END)
     * sequence on separate handles can race and overwrite another process's
     * evidence even though all handles allow write sharing. */
    WriteFile(file, message, (DWORD)length, &written, NULL);
    CloseHandle(file);
}

/* Vista's D3D9 headers do not declare this WDDM capability bit.  milcore
 * uses it to recognize a window-capable LDDM device while it verifies Aero
 * compatibility; later headers only expose the D3D9Ex sharing bit here. */
#ifndef D3DCAPS2_CANRENDERWINDOWED
#define D3DCAPS2_CANRENDERWINDOWED 0x00080000u
#endif
#ifndef D3DDEVCAPS_FLOATTLVERTEX
#define D3DDEVCAPS_FLOATTLVERTEX 0x00000001u
#endif
/* These four names were retired from the D3D9-only SDK headers, but they
 * remain bits in the D3DCAPS9 ABI that Vista checks while admitting a HAL.
 * They describe legacy fixed-function conformance, not a missing D3D9 DDI
 * callback: D3D9 has no ROP2, mask-plane, or CONFORMANT operation to route
 * through this UMD. Keep the values explicit so the Vista contract is not
 * silently reduced by a newer SDK header. */
#ifndef D3DPMISCCAPS_MASKPLANES
#define D3DPMISCCAPS_MASKPLANES 0x00000001u
#endif
#ifndef D3DPMISCCAPS_CONFORMANT
#define D3DPMISCCAPS_CONFORMANT 0x00000008u
#endif
#ifndef D3DPMISCCAPS_FOGINFVF
#define D3DPMISCCAPS_FOGINFVF 0x00002000u
#endif
#ifndef D3DPRASTERCAPS_ROP2
#define D3DPRASTERCAPS_ROP2 0x00000002u
#endif
#ifndef D3DPRASTERCAPS_SUBPIXEL
#define D3DPRASTERCAPS_SUBPIXEL 0x00000020u
#endif
#ifndef D3DPTEXTURECAPS_TRANSPARENCY
#define D3DPTEXTURECAPS_TRANSPARENCY 0x00000008u
#endif

#define TRITON9_VISTA_PRIMITIVE_MISC_CAPS ( \
    D3DPMISCCAPS_MASKPLANES | D3DPMISCCAPS_MASKZ | \
    D3DPMISCCAPS_CONFORMANT | D3DPMISCCAPS_CULLNONE | \
    D3DPMISCCAPS_CULLCW | D3DPMISCCAPS_CULLCCW | \
    D3DPMISCCAPS_FOGINFVF | \
    D3DPMISCCAPS_COLORWRITEENABLE | D3DPMISCCAPS_BLENDOP | \
    D3DPMISCCAPS_SEPARATEALPHABLEND)
#define TRITON9_VISTA_RASTER_CAPS ( \
    D3DPRASTERCAPS_DITHER | D3DPRASTERCAPS_ROP2 | \
    D3DPRASTERCAPS_ZTEST | D3DPRASTERCAPS_FOGVERTEX | \
    D3DPRASTERCAPS_SUBPIXEL | D3DPRASTERCAPS_COLORPERSPECTIVE | \
    D3DPRASTERCAPS_SCISSORTEST)
#define TRITON9_VISTA_TEXTURE_FILTER_CAPS ( \
    D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MINFLINEAR | \
    D3DPTFILTERCAPS_MAGFPOINT | D3DPTFILTERCAPS_MAGFLINEAR | \
    D3DPTFILTERCAPS_MIPFPOINT | D3DPTFILTERCAPS_MIPFLINEAR)

_Static_assert((TRITON9_VISTA_PRIMITIVE_MISC_CAPS & 0x0000002bu) ==
               0x0000002bu,
               "PrimitiveMiscCaps & 0x0000002b == 0x0000002b");
_Static_assert((TRITON9_VISTA_PRIMITIVE_MISC_CAPS &
                D3DPMISCCAPS_FOGINFVF) != 0,
               "Shader Model 2 requires separate fog in FVF");
_Static_assert((TRITON9_VISTA_RASTER_CAPS & 0x00000093u) == 0x00000093u,
               "RasterCaps        & 0x00000093 == 0x00000093");
_Static_assert((TRITON9_VISTA_TEXTURE_FILTER_CAPS &
                (D3DPTFILTERCAPS_MIPFPOINT |
                 D3DPTFILTERCAPS_MIPFLINEAR)) ==
               (D3DPTFILTERCAPS_MIPFPOINT |
                D3DPTFILTERCAPS_MIPFLINEAR),
               "TextureFilterCaps must accept MIL point and linear mip state");
#ifndef D3DSTENCILCAPS_TWOSIDED
#define D3DSTENCILCAPS_TWOSIDED 0x00000100u
#endif

enum {
    TRITON9_DDRAW_INTERFACE = 7u,
    TRITON9_D3D9_INTERFACE = 9u,
    TRITON9_CREATEDEVICE_ALLOWED_FLAGS = 0x00000003u
};

static HRESULT APIENTRY triton9GetCaps(HANDLE hAdapter,
                                       const D3DDDIARG_GETCAPS *args);
static HRESULT APIENTRY triton9CreateDevice(HANDLE hAdapter,
                                            D3DDDIARG_CREATEDEVICE *args);
static HRESULT APIENTRY triton9CloseAdapter(HANDLE hAdapter);
static HRESULT APIENTRY triton9DestroyDevice(HANDLE hDevice);
static HRESULT APIENTRY triton9Flush(HANDLE hDevice);
static HRESULT APIENTRY triton9ValidateDevice(
    HANDLE hDevice, D3DDDIARG_VALIDATETEXTURESTAGESTATE *args);
static HRESULT APIENTRY triton9QueryResourceResidency(
    HANDLE hDevice, const D3DDDIARG_QUERYRESOURCERESIDENCY *args);

static DWORD
triton9MissingDeviceCallbacks(const D3DDDI_DEVICECALLBACKS *callbacks)
{
    DWORD missing = 0;

    if (!callbacks)
        return 0xffffffffu;
    if (!callbacks->pfnAllocateCb)
        missing |= 1u << 0;
    if (!callbacks->pfnDeallocateCb)
        missing |= 1u << 1;
    if (!callbacks->pfnSetDisplayModeCb)
        missing |= 1u << 2;
    if (!callbacks->pfnPresentCb)
        missing |= 1u << 3;
    if (!callbacks->pfnRenderCb)
        missing |= 1u << 4;
    if (!callbacks->pfnLockCb)
        missing |= 1u << 5;
    if (!callbacks->pfnUnlockCb)
        missing |= 1u << 6;
    if (!callbacks->pfnEscapeCb)
        missing |= 1u << 7;
    if (!callbacks->pfnCreateContextCb)
        missing |= 1u << 8;
    if (!callbacks->pfnDestroyContextCb)
        missing |= 1u << 9;
    return missing;
}

/* Neptune keeps a D3D9 runtime binding in process-global renderer state until
 * the first proxy acquire consumes it. A per-device lock cannot stop another
 * device from replacing that table during bootstrap. */
static INIT_ONCE g_triton9ProxyBootstrapOnce = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION g_triton9ProxyBootstrapLock;

static BOOL CALLBACK
triton9InitializeProxyBootstrapLock(PINIT_ONCE once, PVOID parameter,
                                    PVOID *context)
{
    (void)once;
    (void)parameter;
    (void)context;
    InitializeCriticalSection(&g_triton9ProxyBootstrapLock);
    return TRUE;
}

static HRESULT
triton9QueryPrivateAdapterInfo(TRITON9_ADAPTER *adapter)
{
    D3DDDICB_QUERYADAPTERINFO query;
    HRESULT hr;
    DWORD rejectMask = 0;

    if (!adapter || !adapter->callbacks.pfnQueryAdapterInfoCb)
        return D3DDDIERR_NOTAVAILABLE;

    ZeroMemory(&adapter->info, sizeof(adapter->info));
    ZeroMemory(&query, sizeof(query));
    query.pPrivateDriverData = &adapter->info;
    query.PrivateDriverDataSize = sizeof(adapter->info);
    hr = adapter->callbacks.pfnQueryAdapterInfoCb(adapter->hRTAdapter, &query);
    triton9DiagU32("TRITON9-PRIVATE-CB-HR", (DWORD)hr);
    if (FAILED(hr))
        return hr;

    triton9DiagU32("TRITON9-PRIVATE-IAM-LO", (DWORD)adapter->info.V1.IamVioGPU);
    triton9DiagU32("TRITON9-PRIVATE-IAM-HI", (DWORD)(adapter->info.V1.IamVioGPU >> 32));
    triton9DiagU32("TRITON9-PRIVATE-FLAGS",
                   (adapter->info.V1.Flags.Supports3d ? 1u : 0u) |
                       (adapter->info.V1.Flags.HasShmem ? 2u : 0u));
    triton9DiagU32("TRITON9-PRIVATE-CAPSETS-LO",
                   (DWORD)adapter->info.V1.SupportedCapsetIDs);
    triton9DiagU32("TRITON9-PRIVATE-CAPSETS-HI",
                   (DWORD)(adapter->info.V1.SupportedCapsetIDs >> 32));
    triton9DiagU32("TRITON9-PRIVATE-SIZE", adapter->info.StructureSize);
    triton9DiagU32("TRITON9-PRIVATE-ABI", adapter->info.PrivateAbiVersion);
    triton9DiagU32("TRITON9-PRIVATE-FEATURES-LO", (DWORD)adapter->info.FeatureBits);
    triton9DiagU32("TRITON9-PRIVATE-FEATURES-HI",
                   (DWORD)(adapter->info.FeatureBits >> 32));

    /* V2 is deliberately an opt-in ABI.  A legacy KMD can still service the
     * existing D3D10/11 UMD, but it must not be paired with this UMD because
     * ordered host completion is mandatory for Vista composition. */
    if (adapter->info.V1.IamVioGPU != VIOGPU_IAM)
        rejectMask |= 1u << 0;
    if (!adapter->info.V1.Flags.Supports3d)
        rejectMask |= 1u << 1;
    if (!adapter->info.V1.Flags.HasShmem)
        rejectMask |= 1u << 2;
    if (!(adapter->info.V1.SupportedCapsetIDs &
          (1ull << VIOGPU_CAPSET_NEPTUNE)))
        rejectMask |= 1u << 3;
    if (adapter->info.StructureSize < sizeof(adapter->info))
        rejectMask |= 1u << 4;
    if (adapter->info.PrivateAbiVersion != VIOGPU_PRIVATE_ABI_VERSION_V2)
        rejectMask |= 1u << 5;
    if (!(adapter->info.FeatureBits & VIOGPU_FEATURE_RENDER_EVENT))
        rejectMask |= 1u << 6;
    triton9DiagU32("TRITON9-PRIVATE-REJECT", rejectMask);
    if (rejectMask)
        return D3DDDIERR_NOTAVAILABLE;
    return S_OK;
}

static BOOL CALLBACK
triton9InitializePrivateAdapterInfo(PINIT_ONCE once, PVOID parameter,
                                    PVOID *context)
{
    TRITON9_ADAPTER *adapter = (TRITON9_ADAPTER *)parameter;

    (void)once;
    (void)context;
    if (!adapter)
        return FALSE;
    triton9Diag("TRITON9-PRIVATE-DEFERRED-CHECK enter\n");
    adapter->privateInfoHr = triton9QueryPrivateAdapterInfo(adapter);
    triton9DiagU32("TRITON9-PRIVATE-DEFERRED-CHECK-HR",
                   (DWORD)adapter->privateInfoHr);
    /* Cache a failed compatibility check too. Reissuing it from every DDI
     * merely turns a rejected adapter into a callback storm. */
    return TRUE;
}

static HRESULT
triton9EnsurePrivateAdapterInfo(TRITON9_ADAPTER *adapter)
{
    if (!adapter)
        return E_INVALIDARG;
    if (!InitOnceExecuteOnce(&adapter->privateInfoOnce,
                             triton9InitializePrivateAdapterInfo, adapter,
                             NULL)) {
        HRESULT hr = HRESULT_FROM_WIN32(GetLastError());

        return FAILED(hr) ? hr : E_FAIL;
    }
    return adapter->privateInfoHr;
}

static void
triton9FillCaps(D3DCAPS9 *caps)
{
    const DWORD comparisonCaps =
        D3DPCMPCAPS_NEVER | D3DPCMPCAPS_LESS | D3DPCMPCAPS_EQUAL |
        D3DPCMPCAPS_LESSEQUAL | D3DPCMPCAPS_GREATER |
        D3DPCMPCAPS_NOTEQUAL | D3DPCMPCAPS_GREATEREQUAL |
        D3DPCMPCAPS_ALWAYS;
    const DWORD sourceBlendCaps =
        D3DPBLENDCAPS_ZERO | D3DPBLENDCAPS_ONE |
        D3DPBLENDCAPS_SRCCOLOR | D3DPBLENDCAPS_INVSRCCOLOR |
        D3DPBLENDCAPS_SRCALPHA | D3DPBLENDCAPS_INVSRCALPHA |
        D3DPBLENDCAPS_DESTALPHA | D3DPBLENDCAPS_INVDESTALPHA |
        D3DPBLENDCAPS_DESTCOLOR | D3DPBLENDCAPS_INVDESTCOLOR |
        D3DPBLENDCAPS_SRCALPHASAT | D3DPBLENDCAPS_BLENDFACTOR;
    const DWORD destinationBlendCaps =
        D3DPBLENDCAPS_ZERO | D3DPBLENDCAPS_ONE |
        D3DPBLENDCAPS_SRCCOLOR | D3DPBLENDCAPS_INVSRCCOLOR |
        D3DPBLENDCAPS_SRCALPHA | D3DPBLENDCAPS_INVSRCALPHA |
        D3DPBLENDCAPS_DESTALPHA | D3DPBLENDCAPS_INVDESTALPHA |
        D3DPBLENDCAPS_DESTCOLOR | D3DPBLENDCAPS_INVDESTCOLOR |
        D3DPBLENDCAPS_BLENDFACTOR;
    const DWORD stencilCaps =
        D3DSTENCILCAPS_KEEP | D3DSTENCILCAPS_ZERO |
        D3DSTENCILCAPS_REPLACE | D3DSTENCILCAPS_INCRSAT |
        D3DSTENCILCAPS_DECRSAT | D3DSTENCILCAPS_INVERT |
        D3DSTENCILCAPS_INCR | D3DSTENCILCAPS_DECR |
        D3DSTENCILCAPS_TWOSIDED;

    if (!caps)
        return;
    ZeroMemory(caps, sizeof(*caps));

    caps->DeviceType = D3DDEVTYPE_HAL;
    caps->AdapterOrdinal = 0;
    /* Triton has no real scanline counter, gamma ramp, or hardware cursor.
     * Do not turn the KMD's synthetic values into public D3D9 claims. */
    /* Vista QueryLHDDICaps copies this word into D3D9_DRIVERCAPS.
     * GetDX8HALCaps also tests the legacy DDCAPS_BLT bit there before
     * admitting primary surfaces.  Blt is implemented by triton9Blt;
     * omitting this bit leaves DWM's primary without a kernel surface.
     * Use the wire value to avoid mixing ddraw.h with D3D9 type headers. */
    caps->Caps = 0x00000040u; /* DDCAPS_BLT */
    caps->Caps2 = D3DCAPS2_DYNAMICTEXTURES |
                  D3DCAPS2_CANRENDERWINDOWED |
                  D3DCAPS2_CANSHARERESOURCE;
    caps->Caps3 = D3DCAPS3_ALPHA_FULLSCREEN_FLIP_OR_DISCARD |
                  D3DCAPS3_COPY_TO_VIDMEM |
                  D3DCAPS3_COPY_TO_SYSTEMMEM;
    caps->CursorCaps = 0;
    caps->PresentationIntervals = D3DPRESENT_INTERVAL_IMMEDIATE;
    /* Triton owns the hardware-vertex-processing path: it translates both
     * D3D9 vertex shaders and the supported fixed-function transform path to
     * the host D3D11 device.  Vista composition clients select that path by
     * testing HWTRANSFORMANDLIGHT before they create their HAL device.  The
     * runtime enforces pure-device getter restrictions; Triton keeps the same
     * draw implementation for pure and non-pure devices. */
    caps->DevCaps = D3DDEVCAPS_EXECUTESYSTEMMEMORY |
                    D3DDEVCAPS_EXECUTEVIDEOMEMORY |
                    D3DDEVCAPS_TLVERTEXSYSTEMMEMORY |
                    D3DDEVCAPS_TLVERTEXVIDEOMEMORY |
                    D3DDEVCAPS_TEXTURESYSTEMMEMORY |
                    D3DDEVCAPS_TEXTUREVIDEOMEMORY |
                    D3DDEVCAPS_DRAWPRIMTLVERTEX |
                    D3DDEVCAPS_CANRENDERAFTERFLIP |
                    D3DDEVCAPS_DRAWPRIMITIVES2 |
                    D3DDEVCAPS_DRAWPRIMITIVES2EX |
                    D3DDEVCAPS_HWTRANSFORMANDLIGHT |
                    D3DDEVCAPS_HWRASTERIZATION |
                    /* MIL requests a pure hardware device for its normal
                     * composition path.  Pure-device restrictions are
                     * enforced by the D3D9 runtime (state getters are not
                     * exposed); Triton's DDI has no separate pure-device
                     * execution path to emulate. */
                    D3DDEVCAPS_PUREDEVICE |
                    /* Obsolete in D3D9, but required by Vista's WDDM
                     * capability probe and harmless for our translator. */
                    D3DDEVCAPS_FLOATTLVERTEX;
    /* Keep only the primitive and raster states that reach the current host
     * pipeline. The legacy admission bits have no separate D3D9 callback. */
    caps->PrimitiveMiscCaps = TRITON9_VISTA_PRIMITIVE_MISC_CAPS;
    caps->RasterCaps = TRITON9_VISTA_RASTER_CAPS;
    caps->ZCmpCaps = comparisonCaps;
    caps->SrcBlendCaps = sourceBlendCaps;
    caps->DestBlendCaps = destinationBlendCaps;
    caps->AlphaCmpCaps = comparisonCaps;
    caps->ShadeCaps = D3DPSHADECAPS_COLORGOURAUDRGB |
                      D3DPSHADECAPS_SPECULARGOURAUDRGB |
                      D3DPSHADECAPS_ALPHAGOURAUDBLEND;
    caps->TextureCaps = D3DPTEXTURECAPS_PERSPECTIVE |
                        D3DPTEXTURECAPS_ALPHA |
                        D3DPTEXTURECAPS_TRANSPARENCY |
                        D3DPTEXTURECAPS_TEXREPEATNOTSCALEDBYSIZE;
    caps->TextureFilterCaps = TRITON9_VISTA_TEXTURE_FILTER_CAPS;
    caps->CubeTextureFilterCaps = 0;
    caps->VolumeTextureFilterCaps = 0;
    caps->StretchRectFilterCaps = D3DPTFILTERCAPS_MINFPOINT |
        D3DPTFILTERCAPS_MAGFPOINT | D3DPTFILTERCAPS_MINFLINEAR |
        D3DPTFILTERCAPS_MAGFLINEAR;
    caps->TextureAddressCaps = D3DPTADDRESSCAPS_WRAP |
                               D3DPTADDRESSCAPS_MIRROR |
                               D3DPTADDRESSCAPS_CLAMP |
                               D3DPTADDRESSCAPS_BORDER |
                               D3DPTADDRESSCAPS_INDEPENDENTUV |
                               D3DPTADDRESSCAPS_MIRRORONCE;
    caps->VolumeTextureAddressCaps = 0;
    /* D16 has no stencil plane. Publish stencil operations only when the
     * conditional D24S8 format contract is available. */
    caps->StencilCaps = triton9FormatLookup(D3DDDIFMT_D24S8)
        ? stencilCaps : 0;
    caps->VertexTextureFilterCaps = 0;
    caps->LineCaps = D3DLINECAPS_TEXTURE | D3DLINECAPS_ZTEST |
                     D3DLINECAPS_BLEND;
    caps->MaxTextureWidth = 4096;
    caps->MaxTextureHeight = 4096;
    caps->MaxVolumeExtent = 0;
    caps->MaxTextureRepeat = 8192;
    caps->MaxTextureAspectRatio = 4096;
    caps->MaxAnisotropy = 1;
    caps->MaxVertexW = 1.0e10f;
    caps->GuardBandLeft = -8192.0f;
    caps->GuardBandTop = -8192.0f;
    caps->GuardBandRight = 8192.0f;
    caps->GuardBandBottom = 8192.0f;
    caps->FVFCaps = TRITON9_FIXED_TEXTURE_STAGES & D3DFVFCAPS_TEXCOORDCOUNTMASK;
    caps->TextureOpCaps = D3DTEXOPCAPS_DISABLE |
                          D3DTEXOPCAPS_SELECTARG1 |
                          D3DTEXOPCAPS_SELECTARG2 |
                          D3DTEXOPCAPS_MODULATE;
    caps->MaxTextureBlendStages = TRITON9_FIXED_TEXTURE_STAGES;
    caps->MaxSimultaneousTextures = TRITON9_FIXED_TEXTURE_STAGES;
    caps->VertexProcessingCaps = 0;
    caps->MaxActiveLights = 0;
    /* User clip planes are not consumed by the fixed shader path.  The DDI
     * callback below accepts Vista's inert initialization for compatibility,
     * but zero is the truthful public capability. */
    caps->MaxUserClipPlanes = 0;
    caps->MaxVertexBlendMatrices = 0;
    caps->MaxVertexBlendMatrixIndex = 0;
    caps->MaxPointSize = 1.0f;
    caps->MaxPrimitiveCount = 0x00555555u;
    caps->MaxVertexIndex = 0x00ffffffu;
    caps->MaxStreams = TRITON9_MAX_VERTEX_STREAMS;
    caps->MaxStreamStride = 2048;
    /* Vista validates every capability implied by the advertised shader
     * model before it registers the HAL caps.  The current translator has
     * the Shader Model 2.0 feature set needed by DWM, but this compact caps
     * table does not yet describe the complete SM3 contract (for example,
     * the full SM3 stencil/filter/FVF requirements).  Advertising 3.0 here
     * makes IsD3DHALSupported reject the entire adapter and discard the UMD
     * FORMATOP table.  Report the implemented bring-up tier truthfully; move
     * this back to 3.0 only with the corresponding caps and coverage. */
    caps->VertexShaderVersion = D3DVS_VERSION(2, 0);
    caps->MaxVertexShaderConst = 256;
    caps->PixelShaderVersion = D3DPS_VERSION(2, 0);
    caps->PixelShader1xMaxValue = 3.402823466e38F;
    caps->DevCaps2 = D3DDEVCAPS2_STREAMOFFSET |
                     D3DDEVCAPS2_VERTEXELEMENTSCANSHARESTREAMOFFSET;
    caps->NumberOfAdaptersInGroup = 1;
    caps->DeclTypes = D3DDTCAPS_UBYTE4 | D3DDTCAPS_UBYTE4N |
                      D3DDTCAPS_SHORT2N | D3DDTCAPS_SHORT4N |
                      D3DDTCAPS_USHORT2N | D3DDTCAPS_USHORT4N |
                      D3DDTCAPS_UDEC3 | D3DDTCAPS_FLOAT16_2 |
                      D3DDTCAPS_FLOAT16_4;
    caps->NumSimultaneousRTs = 1;
    caps->VS20Caps.Caps = D3DVS20CAPS_PREDICATION;
    caps->VS20Caps.DynamicFlowControlDepth = D3DVS20_MAX_DYNAMICFLOWCONTROLDEPTH;
    caps->VS20Caps.NumTemps = D3DVS20_MAX_NUMTEMPS;
    caps->VS20Caps.StaticFlowControlDepth = D3DVS20_MAX_STATICFLOWCONTROLDEPTH;
    caps->PS20Caps.Caps = D3DPS20CAPS_ARBITRARYSWIZZLE |
                          D3DPS20CAPS_GRADIENTINSTRUCTIONS |
                          D3DPS20CAPS_PREDICATION |
                          D3DPS20CAPS_NODEPENDENTREADLIMIT |
                          D3DPS20CAPS_NOTEXINSTRUCTIONLIMIT;
    caps->PS20Caps.DynamicFlowControlDepth = D3DPS20_MAX_DYNAMICFLOWCONTROLDEPTH;
    caps->PS20Caps.NumTemps = D3DPS20_MAX_NUMTEMPS;
    caps->PS20Caps.StaticFlowControlDepth = D3DPS20_MAX_STATICFLOWCONTROLDEPTH;
    caps->PS20Caps.NumInstructionSlots = D3DPS20_MAX_NUMINSTRUCTIONSLOTS;
    caps->MaxVShaderInstructionsExecuted = 0xffffffffu;
    caps->MaxPShaderInstructionsExecuted = 0xffffffffu;
    /* These fields are part of the shader-model contract, not independent
     * capacity hints.  Vista's checked IsD3DHALSupported requires both to be
     * zero unless the matching 3.0 shader version is advertised. */
    caps->MaxVertexShader30InstructionSlots = 0;
    caps->MaxPixelShader30InstructionSlots = 0;
}

static HRESULT APIENTRY
triton9GetCaps(HANDLE hAdapter, const D3DDDIARG_GETCAPS *args)
{
    TRITON9_ADAPTER *adapter = (TRITON9_ADAPTER *)hAdapter;

    if (!adapter || !args || !args->pData)
        return E_INVALIDARG;

    triton9DiagU32("TRITON9-CAPS-TYPE", (DWORD)args->Type);
    triton9DiagU32("TRITON9-CAPS-DATASIZE", args->DataSize);
    if (args->Type == D3DDDICAPS_GETD3D8CAPS)
        triton9Diag("TRITON9-GETCAPS D3D8\n");
    if (args->Type == D3DDDICAPS_GETD3D9CAPS)
        triton9Diag("TRITON9-GETCAPS D3D9\n");

    switch (args->Type) {
    case D3DDDICAPS_DDRAW:
        if (args->DataSize != sizeof(DDRAW_CAPS))
            return E_INVALIDARG;
        /* Baseline blits/primary surfaces use the existing resource DDI.
         * Do not advertise optional color keys, depth blits, or mirroring. */
        ZeroMemory(args->pData, sizeof(DDRAW_CAPS));
        ((DDRAW_CAPS *)args->pData)->Caps2 = DDRAW_CAPS2_DYNAMICTEXTURES;
        return S_OK;
    case D3DDDICAPS_DDRAW_MODE_SPECIFIC: {
        DDRAW_MODE_SPECIFIC_CAPS *caps = args->pData;
        UINT head;
        if (args->DataSize != sizeof(*caps))
            return E_INVALIDARG;
        head = caps->Head;
        ZeroMemory(caps, sizeof(*caps));
        caps->Head = head;
        return S_OK;
    }
    case D3DDDICAPS_GETFORMATCOUNT:
        triton9Diag("TRITON9-FORMATCOUNT\n");
        if (args->DataSize != sizeof(UINT))
            return E_INVALIDARG;
        *(UINT *)args->pData = triton9FormatCount();
        return S_OK;
    case D3DDDICAPS_GETFORMATDATA: {
        triton9Diag("TRITON9-FORMATDATA\n");
        UINT available = triton9FormatCount();
        SIZE_T required = (SIZE_T)available * sizeof(FORMATOP);
        if ((SIZE_T)args->DataSize != required)
            return E_INVALIDARG;
        triton9CopyFormatOperations((FORMATOP *)args->pData, available);
        return S_OK;
    }
    case D3DDDICAPS_GETD3D3CAPS: {
        TRITON9_LEGACY_GLOBAL_CAPS *legacy = args->pData;
        TRITON9_LEGACY_DEVICE_CAPS *hw = &legacy->hardware;
        D3DCAPS9 caps;
        if (args->DataSize != sizeof(*legacy))
            return E_INVALIDARG;
        triton9FillCaps(&caps);
        ZeroMemory(legacy, sizeof(*legacy));
        legacy->size = sizeof(*legacy);
        hw->size = sizeof(*hw);
        /* D3DDD flags: RGB, device/primitive caps, render and Z bit depths. */
        hw->flags = 0x1e3;
        hw->colorModel = 2; /* D3DCOLOR_RGB */
        hw->devCaps = caps.DevCaps;
        hw->line.size = sizeof(hw->line);
        hw->line.misc = caps.PrimitiveMiscCaps;
        hw->line.raster = caps.RasterCaps;
        hw->line.zCompare = caps.ZCmpCaps;
        hw->line.srcBlend = caps.SrcBlendCaps;
        hw->line.dstBlend = caps.DestBlendCaps;
        hw->line.alphaCompare = caps.AlphaCmpCaps;
        hw->line.shade = caps.ShadeCaps;
        hw->line.texture = caps.TextureCaps;
        /* Legacy NEAREST/LINEAR filters and DECAL/MODULATE/COPY/ADD blends;
         * modern filter bit positions are different. No mip filtering. */
        hw->line.textureFilter = 0x3;
        hw->line.textureBlend = 0xc3;
        hw->line.textureAddress = caps.TextureAddressCaps;
        hw->triangle = hw->line;
        hw->renderDepth = 0x100; /* DDBD_32 */
        hw->zDepth = 0x400; /* DDBD_16 */
        if (triton9FormatLookup(D3DDDIFMT_D24S8))
            hw->zDepth |= 0x200; /* DDBD_24 */
        return S_OK;
    }
    case D3DDDICAPS_GETD3D7CAPS: {
        TRITON9_LEGACY_EXTENDED_CAPS *legacy = args->pData;
        D3DCAPS9 caps;
        if (args->DataSize != sizeof(*legacy))
            return E_INVALIDARG;
        triton9FillCaps(&caps);
        ZeroMemory(legacy, sizeof(*legacy));
        legacy->size = sizeof(*legacy);
        legacy->minTextureWidth = legacy->minTextureHeight = 1;
        legacy->maxTextureWidth = caps.MaxTextureWidth;
        legacy->maxTextureHeight = caps.MaxTextureHeight;
        legacy->maxTextureRepeat = caps.MaxTextureRepeat;
        legacy->maxTextureAspectRatio = caps.MaxTextureAspectRatio;
        legacy->maxAnisotropy = caps.MaxAnisotropy;
        legacy->guardBandLeft = caps.GuardBandLeft;
        legacy->guardBandTop = caps.GuardBandTop;
        legacy->guardBandRight = caps.GuardBandRight;
        legacy->guardBandBottom = caps.GuardBandBottom;
        legacy->extentsAdjust = caps.ExtentsAdjust;
        legacy->stencilCaps = caps.StencilCaps;
        legacy->fvfCaps = caps.FVFCaps;
        legacy->textureOpCaps = caps.TextureOpCaps;
        legacy->maxTextureBlendStages = (WORD)caps.MaxTextureBlendStages;
        legacy->maxSimultaneousTextures = (WORD)caps.MaxSimultaneousTextures;
        legacy->maxActiveLights = caps.MaxActiveLights;
        legacy->maxVertexW = caps.MaxVertexW;
        legacy->maxUserClipPlanes = (WORD)caps.MaxUserClipPlanes;
        legacy->maxVertexBlendMatrices = (WORD)caps.MaxVertexBlendMatrices;
        legacy->vertexProcessingCaps = caps.VertexProcessingCaps;
        return S_OK;
    }
    case D3DDDICAPS_GETD3D8CAPS: {
        D3DCAPS9 caps;
        const UINT d3d8CapsSize = FIELD_OFFSET(D3DCAPS9, DevCaps2);

        /* Vista's D3D9 enum path also requests the D3D8-compatible prefix
         * of D3DCAPS9. The prefix ends at DevCaps2; the Vista-era VBox WDDM
         * D3D9 DDI uses the same layout relationship. Returning NOTAVAILABLE
         * here makes Direct3DCreate9Ex discard an otherwise valid HAL before
         * CreateDeviceEx is reached. */
        triton9DiagU32("TRITON9-D3D8CAPS-NEED", d3d8CapsSize);
        if (args->DataSize != d3d8CapsSize)
            return E_INVALIDARG;
        triton9FillCaps(&caps);
        CopyMemory(args->pData, &caps, d3d8CapsSize);
        return S_OK;
    }
    case D3DDDICAPS_GETD3D9CAPS:
        triton9Diag("TRITON9-CAPS-IN\n");
        triton9DiagU32("TRITON9-CAPS-SIZE", args->DataSize);
        triton9DiagU32("TRITON9-CAPS-NEED", sizeof(D3DCAPS9));
        if (args->DataSize != sizeof(D3DCAPS9))
        {
            triton9Diag("TRITON9-CAPS-SIZE-REJECT\n");
            return E_INVALIDARG;
        }
        triton9FillCaps((D3DCAPS9 *)args->pData);
        {
            D3DCAPS9 *caps = (D3DCAPS9 *)args->pData;
            triton9Diag("TRITON9-CAPS-FILLED\n");
            triton9DiagU32("TRITON9-CAPS-DEV", caps->DeviceType);
            triton9DiagU32("TRITON9-CAPS-ORD", caps->AdapterOrdinal);
            triton9DiagU32("TRITON9-CAPS-CAPS", caps->Caps);
            triton9DiagU32("TRITON9-CAPS-CAPS2", caps->Caps2);
            triton9DiagU32("TRITON9-CAPS-CAPS3", caps->Caps3);
            triton9DiagU32("TRITON9-CAPS-PRIM", caps->PrimitiveMiscCaps);
            triton9DiagU32("TRITON9-CAPS-TEX", caps->TextureCaps);
            triton9DiagU32("TRITON9-CAPS-STAGES", caps->MaxTextureBlendStages);
            triton9DiagU32("TRITON9-CAPS-SIM", caps->MaxSimultaneousTextures);
            triton9DiagU32("TRITON9-CAPS-VS", caps->VertexShaderVersion);
            triton9DiagU32("TRITON9-CAPS-PS", caps->PixelShaderVersion);
            triton9DiagU32("TRITON9-CAPS-VS30-SLOTS",
                           caps->MaxVertexShader30InstructionSlots);
            triton9DiagU32("TRITON9-CAPS-PS30-SLOTS",
                           caps->MaxPixelShader30InstructionSlots);
            triton9DiagU32("TRITON9-CAPS-VPC", caps->VertexProcessingCaps);
        }
        return S_OK;
    case D3DDDICAPS_GETMULTISAMPLEQUALITYLEVELS: {
        DDIMULTISAMPLEQUALITYLEVELSDATA *levels;

        if (args->DataSize != sizeof(*levels))
            return E_INVALIDARG;
        levels = (DDIMULTISAMPLEQUALITYLEVELSDATA *)args->pData;
        /* Resource creation rejects every multisample type.  Report zero
         * quality levels instead of making the runtime infer support from an
         * unavailable capability query. */
        levels->QualityLevels = 0;
        return S_OK;
    }
    case D3DDDICAPS_GETD3DQUERYCOUNT:
        if (args->DataSize != sizeof(UINT))
            return E_INVALIDARG;
        *(UINT *)args->pData = 2;
        return S_OK;
    case D3DDDICAPS_GETD3DQUERYDATA: {
        static const D3DDDIQUERYTYPE queryTypes[] = {
            D3DDDIQUERYTYPE_EVENT,
            D3DDDIQUERYTYPE_OCCLUSION,
        };
        if (args->DataSize != sizeof(queryTypes))
            return E_INVALIDARG;
        CopyMemory(args->pData, queryTypes, sizeof(queryTypes));
        return S_OK;
    }
    case D3DDDICAPS_GETDECODEGUIDCOUNT:
    case D3DDDICAPS_GETVIDEOPROCESSORDEVICEGUIDCOUNT:
    case D3DDDICAPS_GETEXTENSIONGUIDCOUNT:
        if (args->DataSize != sizeof(UINT))
            return E_INVALIDARG;
        *(UINT *)args->pData = 0;
        return S_OK;
    case D3DDDICAPS_GETGAMMARAMPCAPS:
        if (args->DataSize != sizeof(DDIGAMMACAPS))
            return E_INVALIDARG;
        ((DDIGAMMACAPS *)args->pData)->GammaCaps = 0;
        return S_OK;
    case D3DDDICAPS_GETCONTENTPROTECTIONCAPS:
        if (args->DataSize != sizeof(D3DCONTENTPROTECTIONCAPS))
            return E_INVALIDARG;
        ZeroMemory(args->pData, sizeof(D3DCONTENTPROTECTIONCAPS));
        return S_OK;
    default:
        triton9Diag("TRITON9-CAPS-NOTAVAILABLE\n");
        return D3DDDIERR_NOTAVAILABLE;
    }
}

HRESULT
triton9EnsureKernelContext(TRITON9_DEVICE *device)
{
    D3DDDICB_CREATECONTEXT context;
    HRESULT hr = S_OK;

    if (!device || !device->callbacks.pfnCreateContextCb)
        return D3DDDIERR_NOTAVAILABLE;
    if (!device->kmContextLockInitialized)
        return E_FAIL;

    EnterCriticalSection(&device->kmContextLock);
    if (!device->hKMContext) {
        ZeroMemory(&context, sizeof(context));
        hr = device->callbacks.pfnCreateContextCb(device->hRTDevice, &context);
        hr = triton9MapDeviceFailure(device, hr);
        if (SUCCEEDED(hr) && context.hContext)
            device->hKMContext = context.hContext;
        else if (SUCCEEDED(hr))
            hr = E_FAIL;
    }
    LeaveCriticalSection(&device->kmContextLock);
    return hr;
}

HRESULT
triton9Escape(TRITON9_DEVICE *device, VIOGPU_ESCAPE *escape)
{
    D3DDDICB_ESCAPE callback;
    HRESULT hr;

    if (!device || !device->adapter || !escape || !device->callbacks.pfnEscapeCb)
        return D3DDDIERR_NOTAVAILABLE;
    ZeroMemory(&callback, sizeof(callback));
    callback.hDevice = device->hRTDevice;
    callback.pPrivateDriverData = escape;
    callback.PrivateDriverDataSize = sizeof(*escape);
    hr = device->callbacks.pfnEscapeCb(device->adapter->hRTAdapter, &callback);
    return triton9MapDeviceFailure(device, hr);
}

/* The generated Neptune D3D11 proxy creates its transport rings while it is
 * acquired.  That entails Allocate/Render callbacks and a render completion
 * wait.  Vista invokes this UMD's CreateDevice while the runtime is still
 * holding its device-creation serialization, so doing that work there can
 * deadlock CreateDeviceEx before the DDI device is published.  CreateDevice
 * therefore only records the runtime contract; the first real D3D9 operation
 * acquires the proxy through this one-time, per-device gate. */
HRESULT
triton9EnsureHostDevice(TRITON9_DEVICE *device)
{
    ID3D11Device *rawDevice = NULL;
    ID3D11DeviceContext *rawContext = NULL;
    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    HRESULT hr = S_OK;

    if (!device || !device->adapter || !device->hostInitLockInitialized)
        return E_INVALIDARG;
    if (device->deviceLost)
        return D3DDDIERR_DEVICEREMOVED;

    EnterCriticalSection(&device->hostInitLock);
    if (device->hostDevice && device->hostContext)
        goto done;
    triton9ProofDiagU32("TRITON9-PROOF-HOSTPROXY-BEGIN", GetCurrentProcessId());
    if (device->hostDevice || device->hostContext) {
        hr = E_FAIL;
        goto done;
    }

    /* OpenAdapter is part of d3d9.dll's loader-time HAL enumeration.  The
     * private KMD callback used to run there, which can deadlock that loader
     * before Direct3DCreate9Ex is reachable.  This is the first point where
     * the runtime has published a D3D9 device and accepts callbacks. */
    hr = triton9EnsurePrivateAdapterInfo(device->adapter);
    if (FAILED(hr)) {
        triton9DiagU32("TRITON9-PRIVATE-DEFERRED-FAIL", (DWORD)hr);
        goto done;
    }

    if (!InitOnceExecuteOnce(&g_triton9ProxyBootstrapOnce,
                             triton9InitializeProxyBootstrapLock, NULL, NULL)) {
        hr = HRESULT_FROM_WIN32(GetLastError());
        if (SUCCEEDED(hr))
            hr = E_FAIL;
        goto done;
    }

    /* Re-apply this device's copied callbacks immediately before acquisition.
     * Keep the bind and acquire under one process-wide gate: another device
     * must not redirect the renderer to its runtime callbacks mid-bootstrap. */
    EnterCriticalSection(&g_triton9ProxyBootstrapLock);
    npt_renderer_bind_d3d9_runtime(device->adapter->hRTAdapter,
                                   device->hRTDevice,
                                   device->interfaceVersion,
                                   device->runtimeVersion,
                                   &device->adapter->callbacks,
                                   &device->callbacks);
    triton9Diag("TRITON9-HOST-PROXY enter\n");
    hr = npt_d3d11_create_device_internal(NULL, D3D_DRIVER_TYPE_HARDWARE,
                                          NULL, 0, featureLevels,
                                          (UINT)(sizeof(featureLevels) /
                                                 sizeof(featureLevels[0])),
                                          D3D11_SDK_VERSION, &rawDevice,
                                          &device->featureLevel, &rawContext);
    LeaveCriticalSection(&g_triton9ProxyBootstrapLock);
    if (FAILED(hr) || !rawDevice || !rawContext) {
        triton9DiagU32("TRITON9-HOST-PROXY-CREATE-FAIL", (DWORD)hr);
        hr = FAILED(hr) ? hr : E_FAIL;
        goto done;
    }
    hr = ID3D11Device_QueryInterface(rawDevice, &IID_ID3D11Device1,
                                     (void **)&device->hostDevice);
    ID3D11Device_Release(rawDevice);
    rawDevice = NULL;
    if (FAILED(hr) || !device->hostDevice) {
        triton9DiagU32("TRITON9-HOST-PROXY-DEVICE1-FAIL", (DWORD)hr);
        hr = FAILED(hr) ? hr : E_FAIL;
        goto done;
    }
    hr = ID3D11DeviceContext_QueryInterface(rawContext, &IID_ID3D11DeviceContext1,
                                            (void **)&device->hostContext);
    ID3D11DeviceContext_Release(rawContext);
    rawContext = NULL;
    if (FAILED(hr) || !device->hostContext) {
        triton9DiagU32("TRITON9-HOST-PROXY-CONTEXT1-FAIL", (DWORD)hr);
        if (device->hostDevice) {
            ID3D11Device1_Release(device->hostDevice);
            device->hostDevice = NULL;
        }
        hr = FAILED(hr) ? hr : E_FAIL;
        goto done;
    }
    triton9Diag("TRITON9-HOST-PROXY success\n");

done:
    if (rawContext)
        ID3D11DeviceContext_Release(rawContext);
    if (rawDevice)
        ID3D11Device_Release(rawDevice);
    LeaveCriticalSection(&device->hostInitLock);
    if (FAILED(hr))
        triton9ProofDiagU32("TRITON9-PROOF-HOSTPROXY-FAIL", (DWORD)hr);
    return triton9MapDeviceFailure(device, hr);
}

HRESULT
triton9EnsureRuntimeContext(TRITON9_DEVICE *device)
{
    VIOGPU_ESCAPE escape;
    HRESULT hr;

    if (!device)
        return E_INVALIDARG;
    hr = triton9EnsureHostDevice(device);
    if (FAILED(hr))
        return hr;
    if (device->runtimeContextInitialized)
        return device->runtimeContextId ? S_OK : E_FAIL;
    hr = triton9EnsureKernelContext(device);
    if (FAILED(hr)) {
        triton9DiagU32("TRITON9-RUNTIME-CONTEXT-CREATE-FAIL", (DWORD)hr);
        return hr;
    }

    EnterCriticalSection(&device->kmContextLock);
    if (!device->runtimeContextInitialized) {
        ZeroMemory(&escape, sizeof(escape));
        escape.Type = VIOGPU_CTX_INIT;
        escape.DataLength = sizeof(escape.CtxInit);
        escape.CtxInit.CapsetID = 7;
        escape.CtxInit.NumRings = 1;
        hr = triton9Escape(device, &escape);
        if (SUCCEEDED(hr) && escape.CtxInit.CtxId) {
            /* The proxy owns the first CTX_INIT for this D3D9 device.  The
             * KMD returns that same id here so shared blobs are created in
             * the host context that exported them. */
            device->runtimeContextId = escape.CtxInit.CtxId;
            device->runtimeContextInitialized = TRUE;
        } else if (SUCCEEDED(hr)) {
            hr = E_FAIL;
        }
        if (FAILED(hr))
            triton9DiagU32("TRITON9-RUNTIME-CONTEXT-INIT-FAIL", (DWORD)hr);
        else
            triton9DiagU32("TRITON9-RUNTIME-CONTEXT-ID",
                           device->runtimeContextId);
    }
    LeaveCriticalSection(&device->kmContextLock);
    return hr;
}

static HRESULT APIENTRY
triton9Flush(HANDLE hDevice)
{
    TRITON9_DEVICE *device = (TRITON9_DEVICE *)hDevice;
    HRESULT hr;

    if (!device)
        return E_INVALIDARG;
    if (device->deviceLost)
        return D3DDDIERR_DEVICEREMOVED;
    /* Vista is allowed to flush immediately after CreateDevice before the
     * renderer-facing D3D device exists.  VirtualBox's Vista-era Gallium DDI
     * treats that as a successful no-op too.  Do not turn this bookkeeping
     * callback into the first Neptune proxy acquisition: the proxy Allocate/
     * Render path re-enters the runtime before CreateDeviceEx is published.
     * Once an actual resource, non-null shader, or draw created the proxy,
     * Flush has its ordinary D3D11 meaning. */
    if (!device->hostContext)
        return S_OK;
    EnterCriticalSection(&device->shaderLock);
    ID3D11DeviceContext1_Flush(device->hostContext);
    hr = triton9CheckHostDevice(device);
    LeaveCriticalSection(&device->shaderLock);
    return hr;
}

static HRESULT APIENTRY
triton9ValidateDevice(HANDLE hDevice,
                      D3DDDIARG_VALIDATETEXTURESTAGESTATE *args)
{
    TRITON9_DEVICE *device = (TRITON9_DEVICE *)hDevice;

    if (!device || !args)
        return E_INVALIDARG;
    if (device->deviceLost)
        return D3DDDIERR_DEVICEREMOVED;
    /* Each texture-stage combination accepted by the state setters maps to
     * one generated pixel shader. Unsupported combinations fail there. */
    args->NumPasses = 1;
    return S_OK;
}

static HRESULT APIENTRY
triton9QueryResourceResidency(HANDLE hDevice,
                              const D3DDDIARG_QUERYRESOURCERESIDENCY *args)
{
    TRITON9_DEVICE *device = (TRITON9_DEVICE *)hDevice;
    UINT index;

    if (!device || !args || (args->NumResources && !args->pHandleList))
        return E_INVALIDARG;
    if (device->deviceLost)
        return D3DDDIERR_DEVICEREMOVED;
    for (index = 0; index < args->NumResources; ++index) {
        if (!args->pHandleList[index])
            return D3DDDIERR_INVALIDCALL;
    }
    /* Vista's D3D9 DDI has no output residency array here.  Returning
     * success means the resources remain usable; actual allocation residency
     * is tracked by VidMm/KMD rather than guessed in the UMD. */
    return S_OK;
}

static HRESULT APIENTRY
triton9DestroyDevice(HANDLE hDevice)
{
    TRITON9_DEVICE *device = (TRITON9_DEVICE *)hDevice;
    HRESULT result = S_OK;

    if (!device)
        return E_INVALIDARG;

    triton9DestroyAllQueries(device);

    if (device->hKMContext && device->callbacks.pfnDestroyContextCb) {
        D3DDDICB_DESTROYCONTEXT context;
        ZeroMemory(&context, sizeof(context));
        context.hContext = device->hKMContext;
        result = triton9MapDeviceFailure(device,
            device->callbacks.pfnDestroyContextCb(device->hRTDevice, &context));
        device->hKMContext = NULL;
    }
    triton9ReleaseFixedFunctionShaders(device);
    triton9ReleaseUpBuffers(device);
    triton9ReleaseConstantBuffers(device);
    triton9ReleasePipelineState(device);
    triton9ReleaseStretchBlit(device);
    if (device->hostContext)
        ID3D11DeviceContext1_Release(device->hostContext);
    if (device->hostDevice)
        ID3D11Device1_Release(device->hostDevice);
    if (device->shaderLockInitialized)
        DeleteCriticalSection(&device->shaderLock);
    if (device->hostInitLockInitialized)
        DeleteCriticalSection(&device->hostInitLock);
    if (device->kmContextLockInitialized)
        DeleteCriticalSection(&device->kmContextLock);
    HeapFree(GetProcessHeap(), 0, device);
    return result;
}

#include "triton9_failure_trace.h"

static HRESULT APIENTRY
triton9CreateDevice(HANDLE hAdapter, D3DDDIARG_CREATEDEVICE *args)
{
    TRITON9_ADAPTER *adapter = (TRITON9_ADAPTER *)hAdapter;
    TRITON9_DEVICE *device;
    DWORD missingCallbacks;

    triton9Diag("TRITON9-CREATEDEVICE enter\n");
    if (!adapter || !args)
        return E_INVALIDARG;
    triton9DiagU32("TRITON9-CREATEDEVICE-INTERFACE", args->Interface);
    triton9DiagU32("TRITON9-CREATEDEVICE-VERSION", args->Version);
    triton9DiagU32("TRITON9-CREATEDEVICE-FLAGS", args->Flags.Value);
    if (!args->hDevice || !args->pCallbacks || !args->pDeviceFuncs)
        return E_INVALIDARG;
    if (args->Interface != TRITON9_D3D9_INTERFACE &&
        args->Interface != TRITON9_DDRAW_INTERFACE) {
        triton9Diag("TRITON9-CREATEDEVICE-INTERFACE-REJECT\n");
        return D3DDDIERR_NOTAVAILABLE;
    }
    /* Version is an opaque runtime build identifier. Accept every value.
     * Flag bits zero and one grant threading permissions; they do not require
     * Triton to create worker threads. Reserved flag bits must stay clear. */
    if (args->Flags.Value & ~(UINT)TRITON9_CREATEDEVICE_ALLOWED_FLAGS) {
        triton9Diag("TRITON9-CREATEDEVICE-FLAGS-REJECT\n");
        return E_INVALIDARG;
    }
    missingCallbacks = triton9MissingDeviceCallbacks(args->pCallbacks);
    triton9DiagU32("TRITON9-CREATEDEVICE-MISSING-CALLBACKS",
                   missingCallbacks);
    if (missingCallbacks)
        return E_INVALIDARG;
    device = (TRITON9_DEVICE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                         sizeof(*device));
    if (!device)
        return E_OUTOFMEMORY;

    device->hRTDevice = args->hDevice;
    device->adapter = adapter;
    device->callbacks = *args->pCallbacks;
    device->interfaceVersion = args->Interface;
    device->runtimeVersion = args->Version;
    InitializeCriticalSection(&device->kmContextLock);
    device->kmContextLockInitialized = TRUE;
    InitializeCriticalSection(&device->hostInitLock);
    device->hostInitLockInitialized = TRUE;
    InitializeCriticalSection(&device->shaderLock);
    device->shaderLockInitialized = TRUE;
    for (UINT stream = 0; stream < TRITON9_MAX_VERTEX_STREAMS; ++stream)
        device->streamSourceFrequencies[stream] = 1;
    device->streamFrequencyGeneration = 1;
    triton9InitializePipelineState(device);
    ZeroMemory(args->pDeviceFuncs, sizeof(*args->pDeviceFuncs));
    triton9InstallUnsupportedDeviceFuncs(args->pDeviceFuncs);
    args->pDeviceFuncs->pfnValidateDevice = triton9ValidateDevice;
    args->pDeviceFuncs->pfnCreateResource = triton9TraceCreateResource;
    args->pDeviceFuncs->pfnDestroyResource = triton9TraceDestroyResource;
    args->pDeviceFuncs->pfnLock = triton9TraceLock;
    args->pDeviceFuncs->pfnUnlock = triton9TraceUnlock;
    args->pDeviceFuncs->pfnLockAsync = triton9TraceLockAsync;
    args->pDeviceFuncs->pfnUnlockAsync = triton9TraceUnlockAsync;
    args->pDeviceFuncs->pfnRename = triton9TraceRename;
    args->pDeviceFuncs->pfnSetDisplayMode = triton9TraceSetDisplayMode;
    args->pDeviceFuncs->pfnPresent = triton9TracePresent;
    args->pDeviceFuncs->pfnFlush = triton9Flush;
    args->pDeviceFuncs->pfnDestroyDevice = triton9DestroyDevice;
    args->pDeviceFuncs->pfnQueryResourceResidency = triton9QueryResourceResidency;
    args->pDeviceFuncs->pfnOpenResource = triton9TraceOpenResource;
    args->pDeviceFuncs->pfnCreateVertexShaderFunc = triton9CreateVertexShaderFunc;
    args->pDeviceFuncs->pfnDeleteVertexShaderFunc = triton9DeleteVertexShaderFunc;
    args->pDeviceFuncs->pfnSetVertexShaderFunc = triton9SetVertexShaderFunc;
    args->pDeviceFuncs->pfnCreateVertexShaderDecl = triton9CreateVertexShaderDecl;
    args->pDeviceFuncs->pfnDeleteVertexShaderDecl = triton9DeleteVertexShaderDecl;
    args->pDeviceFuncs->pfnSetVertexShaderDecl = triton9SetVertexShaderDecl;
    args->pDeviceFuncs->pfnSetStreamSource = triton9SetStreamSource;
    args->pDeviceFuncs->pfnSetStreamSourceUm = triton9SetStreamSourceUm;
    args->pDeviceFuncs->pfnSetStreamSourceFreq = triton9SetStreamSourceFreq;
    args->pDeviceFuncs->pfnSetIndices = triton9SetIndices;
    args->pDeviceFuncs->pfnSetIndicesUm = triton9SetIndicesUm;
    args->pDeviceFuncs->pfnSetVertexShaderConst = triton9SetVertexShaderConst;
    args->pDeviceFuncs->pfnSetVertexShaderConstI = triton9SetVertexShaderConstI;
    args->pDeviceFuncs->pfnSetVertexShaderConstB = triton9SetVertexShaderConstB;
    args->pDeviceFuncs->pfnCreatePixelShader = triton9CreatePixelShader;
    args->pDeviceFuncs->pfnDeletePixelShader = triton9DeletePixelShader;
    args->pDeviceFuncs->pfnSetPixelShader = triton9SetPixelShader;
    args->pDeviceFuncs->pfnSetPixelShaderConst = triton9SetPixelShaderConst;
    args->pDeviceFuncs->pfnSetPixelShaderConstI = triton9SetPixelShaderConstI;
    args->pDeviceFuncs->pfnSetPixelShaderConstB = triton9SetPixelShaderConstB;
    args->pDeviceFuncs->pfnDrawPrimitive = triton9DrawPrimitive;
    args->pDeviceFuncs->pfnDrawIndexedPrimitive = triton9DrawIndexedPrimitive;
    args->pDeviceFuncs->pfnDrawPrimitive2 = triton9DrawPrimitive2;
    args->pDeviceFuncs->pfnDrawIndexedPrimitive2 = triton9DrawIndexedPrimitive2;
    args->pDeviceFuncs->pfnSetRenderTarget = triton9TraceSetRenderTarget;
    args->pDeviceFuncs->pfnSetDepthStencil = triton9TraceSetDepthStencil;
    args->pDeviceFuncs->pfnClear = triton9TraceClear;
    args->pDeviceFuncs->pfnBlt = triton9TraceBlt;
    args->pDeviceFuncs->pfnBufBlt = triton9TraceBufBlt;
    args->pDeviceFuncs->pfnTexBlt = triton9TraceTexBlt;
    args->pDeviceFuncs->pfnColorFill = triton9TraceColorFill;
    args->pDeviceFuncs->pfnGenerateMipSubLevels = triton9TraceGenerateMipSubLevels;
    args->pDeviceFuncs->pfnSetRenderState = triton9SetRenderState;
    args->pDeviceFuncs->pfnSetTransform = triton9SetTransform;
    args->pDeviceFuncs->pfnMultiplyTransform = triton9MultiplyTransform;
    args->pDeviceFuncs->pfnSetTextureStageState = triton9SetTextureStageState;
    args->pDeviceFuncs->pfnSetTexture = triton9TraceSetTexture;
    args->pDeviceFuncs->pfnSetViewport = triton9SetViewport;
    args->pDeviceFuncs->pfnSetZRange = triton9SetZRange;
    args->pDeviceFuncs->pfnSetScissorRect = triton9SetScissorRect;
    args->pDeviceFuncs->pfnCreateQuery = triton9TraceCreateQuery;
    args->pDeviceFuncs->pfnDestroyQuery = triton9DestroyQuery;
    args->pDeviceFuncs->pfnIssueQuery = triton9TraceIssueQuery;
    args->pDeviceFuncs->pfnGetQueryData = triton9TraceGetQueryData;
    args->hDevice = (HANDLE)device;
    triton9Diag("TRITON9-CREATEDEVICE success\n");
    return S_OK;
}

static HRESULT APIENTRY
triton9CloseAdapter(HANDLE hAdapter)
{
    TRITON9_ADAPTER *adapter = (TRITON9_ADAPTER *)hAdapter;
    if (!adapter)
        return E_INVALIDARG;
    HeapFree(GetProcessHeap(), 0, adapter);
    return S_OK;
}

HRESULT APIENTRY
OpenAdapter(D3DDDIARG_OPENADAPTER *args)
{
    TRITON9_ADAPTER *adapter;

    triton9Diag("TRITON9-OPENADAPTER enter\n");
    triton9ProofDiagU32("TRITON9-PROOF-BUILD", 0x20260816u);
    triton9ProofDiagU32("TRITON9-PROOF-PID", GetCurrentProcessId());
    if (!args)
        return E_INVALIDARG;
    triton9DiagU32("TRITON9-OPENADAPTER-INTERFACE", args->Interface);
    triton9DiagU32("TRITON9-OPENADAPTER-VERSION", args->Version);
    if (!args->hAdapter || !args->pAdapterCallbacks || !args->pAdapterFuncs)
        return E_INVALIDARG;
    /* Vista also opens interface 7 for DirectDraw primary-surface support.
     * Rejecting it leaves CDriverSurface without a kernel object on DWM's
     * fullscreen path, even when ordinary D3D9 rendering works. */
    if (args->Interface != TRITON9_D3D9_INTERFACE &&
        args->Interface != TRITON9_DDRAW_INTERFACE) {
        triton9Diag("TRITON9-OPENADAPTER-INTERFACE-REJECT\n");
        return D3DDDIERR_NOTAVAILABLE;
    }
    /* Version is the runtime build identifier, not the DDI interface version.
     * Return Triton's DDI interface version in DriverVersion on success. */
    if (!args->pAdapterCallbacks->pfnQueryAdapterInfoCb) {
        triton9Diag("TRITON9-OPENADAPTER-CALLBACK-REJECT\n");
        return E_INVALIDARG;
    }
    adapter = (TRITON9_ADAPTER *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                           sizeof(*adapter));
    if (!adapter)
        return E_OUTOFMEMORY;
    adapter->hRTAdapter = args->hAdapter;
    adapter->callbacks = *args->pAdapterCallbacks;
    /* HEAP_ZERO_MEMORY supplied Vista's all-zero INIT_ONCE state. The macro
     * INIT_ONCE_STATIC_INIT is an initializer, not an assignable expression
     * in the MinGW headers used for this UMD. */
    adapter->privateInfoHr = E_UNEXPECTED;

    ZeroMemory(args->pAdapterFuncs, sizeof(*args->pAdapterFuncs));
    args->pAdapterFuncs->pfnGetCaps = triton9GetCaps;
    args->pAdapterFuncs->pfnCreateDevice = triton9CreateDevice;
    args->pAdapterFuncs->pfnCloseAdapter = triton9CloseAdapter;
    args->hAdapter = (HANDLE)adapter;
    args->DriverVersion = D3D_UMD_INTERFACE_VERSION_VISTA;
    triton9Diag("TRITON9-OPENADAPTER success private-info=deferred\n");
    return S_OK;
}
