/*
 * Copyright (C) 2019-2020 Red Hat, Inc.
 *
 * Written By: Vadim Rozenfeld <vrozenfe@redhat.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met :
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and / or other materials provided with the distribution.
 * 3. Neither the names of the copyright holders nor the names of their contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "driver.h"
#include "helper.h"
#include "baseobj.h"
#include "viogpu_adapter.h"
#include "viogpu_device.h"
#include "viogpu_pnp_fixup.h"
#include "viogpu_trace.h"
#if !DBG
#include "driver.tmh"
#endif

#pragma code_seg(push)
#pragma code_seg("INIT")

int nDebugLevel;
int virtioDebugLevel;
int bDebugPrint;
int bBreakAlways;

tDebugPrintFunc VirtioDebugPrintProc;

#ifdef DBG
void InitializeDebugPrints(IN PDRIVER_OBJECT DriverObject, IN PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(RegistryPath);
    bDebugPrint = 0;
    virtioDebugLevel = 0;
    nDebugLevel = TRACE_LEVEL_NONE;
    bBreakAlways = 0;

#if defined(VIOGPU_KD_TRACE) || defined(VIOGPU_SERIAL_TRACE)
    bDebugPrint = 1;
#if defined(VIOGPU_TARGET_VISTA) && !defined(VIOGPU_VERBOSE_TRACE)
    // Each serial byte causes a VM exit. Per-command/queue/interrupt traces
    // dominated interactive rendering after bring-up; retain warnings and
    // errors by default, with verbose capture an explicit diagnostic build.
    virtioDebugLevel = 0;
    nDebugLevel = TRACE_LEVEL_WARNING;
#else
    virtioDebugLevel = 0x5;
    nDebugLevel = TRACE_LEVEL_VERBOSE;
#endif
#endif

#if defined(DBG_VERBOSE)
    bDebugPrint = 1;
    virtioDebugLevel = 0x5;
    bBreakAlways = 1;
    nDebugLevel = TRACE_LEVEL_INFORMATION;
#endif
#if defined(COM_DEBUG)
    VirtioDebugPrintProc = DebugPrintFuncSerial;
#elif defined(PRINT_DEBUG)
    VirtioDebugPrintProc = DebugPrintFuncKdPrint;
#endif
}
#endif

#include <ntddk.h>
#include "viogpu_device.h"

#pragma code_seg(push)
#pragma code_seg("PAGE")
extern "C" NTSTATUS DriverEntry(_In_ DRIVER_OBJECT *pDriverObject, _In_ UNICODE_STRING *pRegistryPath)
{
    PAGED_CODE();
    VioGpuTraceInitialize();
    WPP_INIT_TRACING(pDriverObject, pRegistryPath)
    DbgPrint(TRACE_LEVEL_FATAL, ("---> VIOGPU FULL build on on %s %s\n", __DATE__, __TIME__));
    DRIVER_INITIALIZATION_DATA InitialData = {0};

    // Keep the modern WDDM 1.3 registration intact.  The Vista package is a
    // separate binary and must present the original WDDM 1.0 callback table;
    // merely lowering this version while leaving later members populated makes
    // dxgkrnl interpret the table with the wrong layout.
#if defined(VIOGPU_TARGET_VISTA)
    // Runtime validation on the Vista SP2 guest rejects the RTM revision
    // (0x1052) with STATUS_REVISION_MISMATCH.  SP1's 0x1053 revision retains
    // the same WDDM 1.0 callback-table ABI and is the required registration.
    InitialData.Version = DXGKDDI_INTERFACE_VERSION_VISTA_SP1;
#if defined(VIOGPU_KD_TRACE)
    DbgPrint(TRACE_LEVEL_FATAL,
             ("Vista DDI compile=%#x register=%#x init-data=%Iu\n",
              DXGKDDI_INTERFACE_VERSION,
              InitialData.Version,
              sizeof(InitialData)));
#endif
#else
    InitialData.Version = DXGKDDI_INTERFACE_VERSION_WDDM1_3;
#endif

    InitialData.DxgkDdiAddDevice = VioGpu3DAddDevice;
    InitialData.DxgkDdiStartDevice = VioGpu3DStartDevice;
    InitialData.DxgkDdiStopDevice = VioGpu3DStopDevice;
    InitialData.DxgkDdiRemoveDevice = VioGpu3DRemoveDevice;

    InitialData.DxgkDdiDispatchIoRequest = VioGpu3DDispatchIoRequest;
    InitialData.DxgkDdiInterruptRoutine = VioGpu3DInterruptRoutine;
    InitialData.DxgkDdiDpcRoutine = VioGpu3DDpcRoutine;

    InitialData.DxgkDdiQueryChildRelations = VioGpu3DQueryChildRelations;
    InitialData.DxgkDdiQueryChildStatus = VioGpu3DQueryChildStatus;
    InitialData.DxgkDdiQueryDeviceDescriptor = VioGpu3DQueryDeviceDescriptor;
    InitialData.DxgkDdiSetPowerState = VioGpu3DSetPowerState;
    InitialData.DxgkDdiNotifyAcpiEvent = VioGpu3DNotifyAcpiEvent;
    InitialData.DxgkDdiResetDevice = VioGpu3DResetDevice;
    InitialData.DxgkDdiUnload = VioGpu3DUnload;
    InitialData.DxgkDdiQueryInterface = VioGpu3DQueryInterface;
    InitialData.DxgkDdiControlEtwLogging = VioGpu3DControlEtwLogging;

    InitialData.DxgkDdiQueryAdapterInfo = VioGpu3DQueryAdapterInfo;
    InitialData.DxgkDdiEscape = VioGpu3DEscape;
    InitialData.DxgkDdiCreateAllocation = VioGpu3DCreateAllocation;
    InitialData.DxgkDdiOpenAllocation = VioGpu3DOpenAllocation;
    InitialData.DxgkDdiCloseAllocation = VioGpu3DCloseAllocation;
    InitialData.DxgkDdiDescribeAllocation = VioGpu3DDescribeAllocation;
    InitialData.DxgkDdiDestroyAllocation = VioGpu3DDestroyAllocation;
    InitialData.DxgkDdiGetStandardAllocationDriverData = VioGpu3DGetStandardAllocationDriverData;
    InitialData.DxgkDdiBuildPagingBuffer = VioGpu3DBuildPagingBuffer;
    // These baseline WDDM 1.0 slots are validated before dxgkrnl enters
    // StartDevice.  The functions below explicitly return NOT_SUPPORTED for
    // the features outside this driver’s Aero scope.
    InitialData.DxgkDdiAcquireSwizzlingRange = VioGpu3DAcquireSwizzlingRange;
    InitialData.DxgkDdiReleaseSwizzlingRange = VioGpu3DReleaseSwizzlingRange;
    InitialData.DxgkDdiSetPalette = VioGpu3DSetPalette;

    InitialData.DxgkDdiCreateContext = VioGpu3DDdiCreateContext;
    InitialData.DxgkDdiDestroyContext = VioGpu3DDdiDestroyContext;
    InitialData.DxgkDdiSetDisplayPrivateDriverFormat = VioGpu3DSetDisplayPrivateDriverFormat;

    InitialData.DxgkDdiPresent = VioGpu3DPresent;
    InitialData.DxgkDdiRender = VioGpu3DRender;
    InitialData.DxgkDdiPatch = VioGpu3DPatch;
    InitialData.DxgkDdiSubmitCommand = VioGpu3DSubmitCommand;

    InitialData.DxgkDdiSetPointerPosition = VioGpu3DSetPointerPosition;
    InitialData.DxgkDdiSetPointerShape = VioGpu3DSetPointerShape;
    InitialData.DxgkDdiIsSupportedVidPn = VioGpu3DIsSupportedVidPn;
    InitialData.DxgkDdiRecommendFunctionalVidPn = VioGpu3DRecommendFunctionalVidPn;
    InitialData.DxgkDdiEnumVidPnCofuncModality = VioGpu3DEnumVidPnCofuncModality;
    InitialData.DxgkDdiSetVidPnSourceVisibility = VioGpu3DSetVidPnSourceVisibility;
    InitialData.DxgkDdiCommitVidPn = VioGpu3DCommitVidPn;
    InitialData.DxgkDdiUpdateActiveVidPnPresentPath = VioGpu3DUpdateActiveVidPnPresentPath;
    InitialData.DxgkDdiSetVidPnSourceAddress = VioGpu3DSetVidPnSourceAddress;
    InitialData.DxgkDdiRecommendMonitorModes = VioGpu3DRecommendMonitorModes;
    // This is a WDDM 1.0 member, not a later hardware-capability extension.
    // Leaving it null makes the Vista dxgkrnl reject the otherwise valid
    // VidPn callback set before composition can select a topology.
    InitialData.DxgkDdiRecommendVidPnTopology = VioGpu3DRecommendVidPnTopology;
    InitialData.DxgkDdiStopCapture = VioGpu3DStopCapture;
    InitialData.DxgkDdiCreateOverlay = VioGpu3DCreateOverlay;
#if !defined(VIOGPU_TARGET_VISTA)
    InitialData.DxgkDdiQueryVidPnHWCapability = VioGpu3DQueryVidPnHWCapability;
    InitialData.DxgkDdiSystemDisplayEnable = VioGpu3DSystemDisplayEnable;
    InitialData.DxgkDdiSystemDisplayWrite = VioGpu3DSystemDisplayWrite;

    InitialData.DxgkDdiStopDeviceAndReleasePostDisplayOwnership = VioGpu3DStopDeviceAndReleasePostDisplayOwnership;
#endif

    InitialData.DxgkDdiCreateDevice = VioGpu3DCreateDevice;
    InitialData.DxgkDdiDestroyDevice = VioGpu3DDestroyDevice;
    InitialData.DxgkDdiUpdateOverlay = VioGpu3DUpdateOverlay;
    InitialData.DxgkDdiFlipOverlay = VioGpu3DFlipOverlay;
    InitialData.DxgkDdiDestroyOverlay = VioGpu3DDestroyOverlay;

    InitialData.DxgkDdiPreemptCommand = VioGpu3DDdiPreemptCommand;
    InitialData.DxgkDdiResetFromTimeout = VioGpu3DDdiResetFromTimeout;
    InitialData.DxgkDdiRestartFromTimeout = VioGpu3DDdiRestartFromTimeout;
    InitialData.DxgkDdiCollectDbgInfo = VioGpu3DDdiCollectDbgInfo;
    InitialData.DxgkDdiQueryCurrentFence = VioGpu3DDdiQueryCurrentFence;
#if !defined(VIOGPU_TARGET_VISTA)
    InitialData.DxgkDdiCancelCommand = VioGpu3DDdiCancelCommand;
    InitialData.DxgkDdiQueryEngineStatus = VioGpu3DDdiQueryEngineStatus;
    InitialData.DxgkDdiResetEngine = VioGpu3DDdiResetEngine;

    InitialData.DxgkDdiGetNodeMetadata = VioGpu3DDdiGetNodeMetadata;
#endif
    InitialData.DxgkDdiControlInterrupt = VioGpu3DDdiControlInterrupt;
    InitialData.DxgkDdiGetScanLine = VioGpu3DDdiGetScanLine;

    NTSTATUS Status = DxgkInitialize(pDriverObject, pRegistryPath, &InitialData);

    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("DxgkInitialize failed with Status: 0x%X\n", Status));
    }

#if defined(VIOGPU_TARGET_VISTA)
    // The synthetic display needs sub-ms scheduling. Own the request for
    // the driver lifetime, outside power IRPs and flip-thread stop waits.
    if (NT_SUCCESS(Status))
        ExSetTimerResolution(5000, TRUE);
#endif

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return Status;
}
// END: Init Code
#pragma code_seg(pop)

#pragma code_seg(push)
#pragma code_seg("PAGE")

//
// PnP DDIs
//

VOID VioGpu3DUnload(VOID)
{
    VioGpuTraceShutdown();
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_INFORMATION, ("<--> %s\n", __FUNCTION__));
#if defined(VIOGPU_TARGET_VISTA)
    ExSetTimerResolution(0, FALSE);
#endif
    WPP_CLEANUP(NULL);
}

NTSTATUS
VioGpu3DAddDevice(_In_ DEVICE_OBJECT *pPhysicalDeviceObject, _Outptr_ PVOID *ppDeviceContext)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    if ((pPhysicalDeviceObject == NULL) || (ppDeviceContext == NULL))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("One of pPhysicalDeviceObject (%p), ppDeviceContext (%p) is NULL",
                  pPhysicalDeviceObject,
                  ppDeviceContext));
        return STATUS_INVALID_PARAMETER;
    }
    *ppDeviceContext = NULL;

    VioGpuAdapter *pAdapter = new (VIOGPU_NONPAGED_POOL) VioGpuAdapter(pPhysicalDeviceObject);
    if (pAdapter == NULL)
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("pAdapter failed to be allocated"));
        return STATUS_NO_MEMORY;
    }

    *ppDeviceContext = pAdapter->ToHandle();

    VioGpuInstallDisplayFixup(pPhysicalDeviceObject);

    DbgPrint(TRACE_LEVEL_FATAL, ("<--- %s ppDeviceContext = %p\n", __FUNCTION__, pAdapter));
    return STATUS_SUCCESS;
}

NTSTATUS
VioGpu3DRemoveDevice(_In_ VOID *pDeviceContext)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_FATAL, ("---> %s 0x%p\n", __FUNCTION__, pDeviceContext));

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(pDeviceContext);

    if (pAdapter)
    {
        VioGpuRemoveDisplayFixup(pAdapter->GetPhysicalDevice());
        delete pAdapter;
    }

    DbgPrint(TRACE_LEVEL_FATAL, ("<--- %s\n", __FUNCTION__));
    return STATUS_SUCCESS;
}

NTSTATUS
VioGpu3DStartDevice(_In_ VOID *pDeviceContext,
                    _In_ DXGK_START_INFO *pDxgkStartInfo,
                    _In_ DXGKRNL_INTERFACE *pDxgkInterface,
                    _Out_ ULONG *pNumberOfViews,
                    _Out_ ULONG *pNumberOfChildren)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_FATAL, ("<---> %s context=%p\n", __FUNCTION__, pDeviceContext));
    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(pDeviceContext);
    if (pAdapter == NULL || pDxgkStartInfo == NULL || pDxgkInterface == NULL ||
        pNumberOfViews == NULL || pNumberOfChildren == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    NTSTATUS Status = pAdapter->StartDevice(pDxgkStartInfo, pDxgkInterface, pNumberOfViews, pNumberOfChildren);
    DbgPrint(TRACE_LEVEL_FATAL, ("<--- %s status=%#x views=%lu children=%lu\n",
                                 __FUNCTION__,
                                 Status,
                                 pNumberOfViews ? *pNumberOfViews : 0,
                                 pNumberOfChildren ? *pNumberOfChildren : 0));
    return Status;
}

NTSTATUS
APIENTRY
VioGpu3DSetDisplayPrivateDriverFormat(
    _In_ CONST HANDLE hAdapter,
    _In_ DXGKARG_SETDISPLAYPRIVATEDRIVERFORMAT *pSetDisplayPrivateDriverFormat)
{
    PAGED_CODE();

    if (pSetDisplayPrivateDriverFormat == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(hAdapter);
    if (pAdapter == NULL || !pAdapter->IsDriverActive())
    {
        return STATUS_UNSUCCESSFUL;
    }

    if (pSetDisplayPrivateDriverFormat->VidPnSourceId >= MAX_VIEWS)
    {
        return STATUS_INVALID_PARAMETER;
    }

    // Neptune scanout uses the public format selected by the committed VidPn.
    // A nonzero private attribute would require separate scanout/swizzle
    // programming, which this Vista driver deliberately does not expose.
    if (pSetDisplayPrivateDriverFormat->PrivateDriverFormatAttribute != 0)
    {
        DbgPrint(TRACE_LEVEL_WARNING,
                 ("%s rejected private format %#x for source %u\n",
                  __FUNCTION__,
                  pSetDisplayPrivateDriverFormat->PrivateDriverFormatAttribute,
                  pSetDisplayPrivateDriverFormat->VidPnSourceId));
        return STATUS_NOT_SUPPORTED;
    }

    DbgPrint(TRACE_LEVEL_VERBOSE,
             ("%s source=%u primary=%p public format\n",
              __FUNCTION__,
              pSetDisplayPrivateDriverFormat->VidPnSourceId,
              pSetDisplayPrivateDriverFormat->PrimaryAllocation));
    return STATUS_SUCCESS;
}

NTSTATUS
APIENTRY
VioGpu3DSetPalette(_In_ CONST HANDLE hAdapter,
                   _In_ CONST DXGKARG_SETPALETTE *pSetPalette)
{
    PAGED_CODE();
    if (hAdapter == NULL || pSetPalette == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    // Palette scanout is intentionally outside the Vista composition scope.
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS
APIENTRY
VioGpu3DStopCapture(_In_ CONST HANDLE hAdapter,
                    _In_ CONST DXGKARG_STOPCAPTURE *pStopCapture)
{
    PAGED_CODE();
    if (hAdapter == NULL || pStopCapture == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    return STATUS_NOT_SUPPORTED;
}

NTSTATUS
APIENTRY
VioGpu3DCreateOverlay(_In_ CONST HANDLE hAdapter,
                      _Inout_ DXGKARG_CREATEOVERLAY *pCreateOverlay)
{
    PAGED_CODE();
    if (hAdapter == NULL || pCreateOverlay == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    pCreateOverlay->hOverlay = NULL;
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS
APIENTRY
VioGpu3DUpdateOverlay(_In_ CONST HANDLE hOverlay,
                      _In_ CONST DXGKARG_UPDATEOVERLAY *pUpdateOverlay)
{
    PAGED_CODE();
    if (hOverlay == NULL || pUpdateOverlay == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    return STATUS_NOT_SUPPORTED;
}

NTSTATUS
APIENTRY
VioGpu3DFlipOverlay(_In_ CONST HANDLE hOverlay,
                    _In_ CONST DXGKARG_FLIPOVERLAY *pFlipOverlay)
{
    PAGED_CODE();
    if (hOverlay == NULL || pFlipOverlay == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    return STATUS_NOT_SUPPORTED;
}

NTSTATUS
APIENTRY
VioGpu3DDestroyOverlay(_In_ CONST HANDLE hOverlay)
{
    PAGED_CODE();
    if (hOverlay == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    return STATUS_NOT_SUPPORTED;
}

NTSTATUS
VioGpu3DNotifyAcpiEvent(_In_ VOID *pDeviceContext,
                         _In_ DXGK_EVENT_TYPE EventType,
                         _In_ ULONG Event,
                         _In_ VOID *Argument,
                         _Out_ ULONG *pAcpiFlags)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(pDeviceContext);
    UNREFERENCED_PARAMETER(EventType);
    UNREFERENCED_PARAMETER(Event);
    UNREFERENCED_PARAMETER(Argument);

    if (pAcpiFlags == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    // The virtual adapter exposes no ACPI-specific display controls.  Zero
    // flags explicitly tell dxgkrnl that this notification did not alter a
    // monitor or a VidPn; returning NOT_SUPPORTED lets any child stack handle
    // an interface it owns instead of treating the event as consumed.
    *pAcpiFlags = 0;
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS
VioGpu3DQueryInterface(_In_ VOID *pDeviceContext,
                        _In_ PQUERY_INTERFACE pQueryInterface)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(pDeviceContext);
    UNREFERENCED_PARAMETER(pQueryInterface);

    // No child-device, OPM, or vendor functional interface is exported by
    // this adapter.  This is the documented result for an unsupported query.
    return STATUS_NOT_SUPPORTED;
}

VOID
VioGpu3DControlEtwLogging(_In_ BOOLEAN Enable,
                           _In_ ULONG Flags,
                           _In_ UCHAR Level)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(Flags);
    // The Vista diagnostic binary does not register an ETW provider.  Keep
    // its KD trace verbosity in step with dxgkrnl's enable/disable request.
    nDebugLevel = Enable
                      ? ((Level < TRACE_LEVEL_VERBOSE) ? (int)Level : TRACE_LEVEL_VERBOSE)
                      : TRACE_LEVEL_NONE;
}

NTSTATUS
VioGpu3DStopDevice(_In_ VOID *pDeviceContext)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_INFORMATION, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(pDeviceContext);
    if (pAdapter == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }
    return pAdapter->StopDevice();
}

NTSTATUS
VioGpu3DDispatchIoRequest(_In_ VOID *pDeviceContext,
                          _In_ ULONG VidPnSourceId,
                          _In_ VIDEO_REQUEST_PACKET *pVideoRequestPacket)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(pDeviceContext);
    if (pAdapter == NULL || pVideoRequestPacket == NULL ||
        VidPnSourceId >= MAX_VIEWS)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (!pAdapter->IsDriverActive())
    {
        VIOGPU_LOG_ASSERTION1("VioGpuAdapter (0x%I64x) is being called when not active!", pAdapter);
        return STATUS_UNSUCCESSFUL;
    }
    return pAdapter->DispatchIoRequest(VidPnSourceId, pVideoRequestPacket);
}

NTSTATUS
VioGpu3DSetPowerState(_In_ VOID *pDeviceContext,
                      _In_ ULONG HardwareUid,
                      _In_ DEVICE_POWER_STATE DevicePowerState,
                      _In_ POWER_ACTION ActionType)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(pDeviceContext);
    if (pAdapter == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (!pAdapter->IsDriverActive())
    {
        return STATUS_SUCCESS;
    }
    return pAdapter->SetPowerState(HardwareUid, DevicePowerState, ActionType);
}

NTSTATUS
VioGpu3DQueryChildRelations(_In_ VOID *pDeviceContext,
                            _Out_writes_bytes_(ChildRelationsSize) DXGK_CHILD_DESCRIPTOR *pChildRelations,
                            _In_ ULONG ChildRelationsSize)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(pDeviceContext);
    if (pAdapter == NULL || pChildRelations == NULL || ChildRelationsSize == 0)
    {
        return STATUS_INVALID_PARAMETER;
    }

    return pAdapter->QueryChildRelations(pChildRelations, ChildRelationsSize);
}

NTSTATUS
VioGpu3DQueryChildStatus(_In_ VOID *pDeviceContext,
                         _Inout_ DXGK_CHILD_STATUS *pChildStatus,
                         _In_ BOOLEAN NonDestructiveOnly)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(pDeviceContext);
    if (pAdapter == NULL || pChildStatus == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    return pAdapter->QueryChildStatus(pChildStatus, NonDestructiveOnly);
}

NTSTATUS
VioGpu3DQueryDeviceDescriptor(_In_ VOID *pDeviceContext,
                              _In_ ULONG ChildUid,
                              _Inout_ DXGK_DEVICE_DESCRIPTOR *pDeviceDescriptor)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(pDeviceContext);
    if (pAdapter == NULL || pDeviceDescriptor == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }
    if (!pAdapter->IsDriverActive())
    {
        DbgPrint(TRACE_LEVEL_WARNING, ("VIOGPU (%p) is being called when not active!", pAdapter));
        return STATUS_UNSUCCESSFUL;
    }
    return pAdapter->QueryDeviceDescriptor(ChildUid, pDeviceDescriptor);
}

NTSTATUS
APIENTRY
VioGpu3DQueryAdapterInfo(_In_ CONST HANDLE hAdapter, _In_ CONST DXGKARG_QUERYADAPTERINFO *pQueryAdapterInfo)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(hAdapter);
    if (pAdapter == NULL || pQueryAdapterInfo == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    return pAdapter->QueryAdapterInfo(pQueryAdapterInfo);
}

#if !defined(VIOGPU_TARGET_VISTA)
NTSTATUS
APIENTRY
VioGpu3DDdiGetNodeMetadata(_In_ CONST HANDLE hAdapter,
                           UINT NodeOrdinal,
                           _Out_ DXGKARG_GETNODEMETADATA *pGetNodeMetadata)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(NodeOrdinal);

    if (NodeOrdinal >= 1)
    {
        return STATUS_INVALID_PARAMETER;
    }

    pGetNodeMetadata->EngineType = DXGK_ENGINE_TYPE_3D;
    pGetNodeMetadata->Flags.Value = 0;

    return STATUS_SUCCESS;
};

#endif

NTSTATUS
APIENTRY
VioGpu3DSetPointerPosition(_In_ CONST HANDLE hAdapter, _In_ CONST DXGKARG_SETPOINTERPOSITION *pSetPointerPosition)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(hAdapter);
    if (pAdapter == NULL || pSetPointerPosition == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (!pAdapter->IsDriverActive())
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("VioGpu (%p) is being called when not active!", pAdapter));
        VioGpuDbgBreak();
        return STATUS_UNSUCCESSFUL;
    }

    return pAdapter->SetPointerPosition(pSetPointerPosition);
}

NTSTATUS
APIENTRY
VioGpu3DSetPointerShape(_In_ CONST HANDLE hAdapter, _In_ CONST DXGKARG_SETPOINTERSHAPE *pSetPointerShape)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(hAdapter);
    if (pAdapter == NULL || pSetPointerShape == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (!pAdapter->IsDriverActive())
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("<---> %s VioGpu (%p) is being called when not active!\n", __FUNCTION__, pAdapter));
        return STATUS_UNSUCCESSFUL;
    }
    return pAdapter->SetPointerShape(pSetPointerShape);
}

NTSTATUS
APIENTRY
VioGpu3DEscape(_In_ CONST HANDLE hAdapter, _In_ CONST DXGKARG_ESCAPE *pEscape)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(hAdapter);
    if ((pAdapter == NULL) || (pEscape == NULL) ||
        (pEscape->pPrivateDriverData == NULL))
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (!pAdapter->IsDriverActive())
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("<---> %s VioGpu (%p) is being called when not active!\n", __FUNCTION__, pAdapter));
        return STATUS_UNSUCCESSFUL;
    }
    return pAdapter->Escape(pEscape);
}

NTSTATUS
APIENTRY
VioGpu3DCreateAllocation(_In_ CONST HANDLE hAdapter, _Inout_ DXGKARG_CREATEALLOCATION *pCreateAllocation)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(hAdapter);
    if (pAdapter == NULL || pCreateAllocation == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (!pAdapter->IsDriverActive())
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("<---> %s VioGpu (%p) is being called when not active!\n", __FUNCTION__, pAdapter));
        return STATUS_UNSUCCESSFUL;
    }
    return VioGpuAllocation::DxgkCreateAllocation(pAdapter, pCreateAllocation);
}

NTSTATUS
APIENTRY
VioGpu3DDescribeAllocation(_In_ CONST HANDLE hAdapter, _Inout_ DXGKARG_DESCRIBEALLOCATION *pDescribeAllocation)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    if (VioGpuAdapter::FromHandle(hAdapter) == NULL ||
        pDescribeAllocation == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    VioGpuAllocation *pAllocation = VioGpuAllocation::FromHandle(pDescribeAllocation->hAllocation);
    if (pAllocation == NULL)
    {
        return STATUS_INVALID_HANDLE;
    }

    return pAllocation->DescribeAllocation(pDescribeAllocation);
}

NTSTATUS
APIENTRY
VioGpu3DOpenAllocation(_In_ CONST HANDLE hDevice, _In_ CONST DXGKARG_OPENALLOCATION *pOpenAllocation)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuDevice *pDxContext = VioGpuDevice::FromHandle(hDevice);
    if (pDxContext == NULL || pOpenAllocation == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    return pDxContext->OpenAllocation(pOpenAllocation);
}

NTSTATUS
APIENTRY
VioGpu3DCloseAllocation(_In_ CONST HANDLE hDevice, _In_ CONST DXGKARG_CLOSEALLOCATION *pCloseAllocation)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuDevice *pDevice = VioGpuDevice::FromHandle(hDevice);
    if (pDevice == NULL || pCloseAllocation == NULL ||
        (pCloseAllocation->NumAllocations != 0 &&
         pCloseAllocation->pOpenHandleList == NULL))
    {
        return STATUS_INVALID_PARAMETER;
    }

    for (ULONG i = 0; i < pCloseAllocation->NumAllocations; i++)
    {
        VioGpuDeviceAllocation *pDeviceAllocation =
            VioGpuDeviceAllocation::FromHandle(pCloseAllocation->pOpenHandleList[i]);
        if (pDeviceAllocation == NULL || pDeviceAllocation->GetDevice() != pDevice)
        {
            return STATUS_INVALID_HANDLE;
        }
        for (ULONG j = 0; j < i; j++)
        {
            if (pCloseAllocation->pOpenHandleList[i] ==
                pCloseAllocation->pOpenHandleList[j])
            {
                return STATUS_INVALID_PARAMETER;
            }
        }
    }

    for (ULONG i = 0; i < pCloseAllocation->NumAllocations; i++)
    {
        VioGpuDeviceAllocation *pDeviceAllocation = VioGpuDeviceAllocation::FromHandle(pCloseAllocation->pOpenHandleList[i]);
        if (pDeviceAllocation != NULL)
        {
            pDeviceAllocation->GetAllocation()->Close(pDeviceAllocation);
        }
    }

    return STATUS_SUCCESS;
}

NTSTATUS
APIENTRY
VioGpu3DDestroyAllocation(_In_ CONST HANDLE hAdapter, _In_ CONST DXGKARG_DESTROYALLOCATION *pDestroyAllocation)
{
    PAGED_CODE();
    if (VioGpuAdapter::FromHandle(hAdapter) == NULL ||
        pDestroyAllocation == NULL ||
        (pDestroyAllocation->NumAllocations != 0 &&
         pDestroyAllocation->pAllocationList == NULL))
    {
        return STATUS_INVALID_PARAMETER;
    }

    for (ULONG i = 0; i < pDestroyAllocation->NumAllocations; i++)
    {
        if (VioGpuAllocation::FromHandle(pDestroyAllocation->pAllocationList[i]) == NULL)
        {
            return STATUS_INVALID_HANDLE;
        }
        for (ULONG j = 0; j < i; j++)
        {
            if (pDestroyAllocation->pAllocationList[i] ==
                pDestroyAllocation->pAllocationList[j])
            {
                return STATUS_INVALID_PARAMETER;
            }
        }
    }
    if (pDestroyAllocation->Flags.DestroyResource &&
        VioGpuResource::FromHandle(pDestroyAllocation->hResource) == NULL)
    {
        return STATUS_INVALID_HANDLE;
    }

    for (ULONG i = 0; i < pDestroyAllocation->NumAllocations; i++)
    {
        VioGpuAllocation *allocation = VioGpuAllocation::FromHandle(pDestroyAllocation->pAllocationList[i]);
        if (allocation != NULL)
        {
            // Release the DXGK reference. Async paths that took an extra
            // ref (e.g. the vsync DPC) complete the deletion.
            allocation->Release();
        }
    }

    if (pDestroyAllocation->Flags.DestroyResource)
    {
        VioGpuResource *resource = VioGpuResource::FromHandle(pDestroyAllocation->hResource);
        if (resource != NULL)
        {
            delete resource;
        }
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s \n", __FUNCTION__));
    return STATUS_SUCCESS;
}

NTSTATUS
APIENTRY
VioGpu3DGetStandardAllocationDriverData(_In_ CONST HANDLE hAdapter,
                                        _Inout_ DXGKARG_GETSTANDARDALLOCATIONDRIVERDATA *pStandardAllocation)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(hAdapter);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));
    if (pStandardAllocation == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }
    return VioGpuAllocation::GetStandardAllocationDriverData(pStandardAllocation);
}

NTSTATUS
APIENTRY
VioGpu3DBuildPagingBuffer(_In_ CONST HANDLE hAdapter, _In_ DXGKARG_BUILDPAGINGBUFFER *pBuildPagingBuffer)
{
    PAGED_CODE();
    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(hAdapter);
    if (pAdapter == NULL || pBuildPagingBuffer == NULL ||
        ((pBuildPagingBuffer->pDmaBufferPrivateData != NULL) &&
         pBuildPagingBuffer->DmaBufferPrivateDataSize < sizeof(void *)))
    {
        return STATUS_INVALID_PARAMETER;
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s operation=%d\n", __FUNCTION__, pBuildPagingBuffer->Operation));

    // Paging DMA buffers are recycled with their private-data area; no
    // path below stores a VioGpuCommand*, so a stale pointer from the
    // buffer's previous user would reach SubmitCommand and corrupt an
    // unrelated in-flight command's fence (see VioGpuDevice::Present).
    if (pBuildPagingBuffer->pDmaBufferPrivateData)
    {
        *(void **)pBuildPagingBuffer->pDmaBufferPrivateData = NULL;
    }

    switch (pBuildPagingBuffer->Operation)
    {
        case DXGK_OPERATION_MAP_APERTURE_SEGMENT:
            {
                if (pBuildPagingBuffer->MapApertureSegment.hAllocation == NULL)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("<--- %s (map aperture segment) no allocation specified\n", __FUNCTION__));
                    return STATUS_INVALID_PARAMETER;
                }

                VioGpuAllocation *allocation = VioGpuAllocation::FromHandle(pBuildPagingBuffer->MapApertureSegment.hAllocation);
                if (allocation == NULL)
                    return STATUS_INVALID_HANDLE;
                NTSTATUS Status = allocation->MapApertureSegment(pBuildPagingBuffer);
                DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s (map aperture segment)\n", __FUNCTION__));
                return Status;
            }
        case DXGK_OPERATION_UNMAP_APERTURE_SEGMENT:
            {
                if (pBuildPagingBuffer->UnmapApertureSegment.hAllocation == NULL)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("<--- %s (map aperture segment) no allocation specified\n", __FUNCTION__));
                    return STATUS_INVALID_PARAMETER;
                }

                VioGpuAllocation *allocation = VioGpuAllocation::FromHandle(pBuildPagingBuffer->UnmapApertureSegment.hAllocation);
                if (allocation == NULL)
                    return STATUS_INVALID_HANDLE;
                NTSTATUS Status = allocation->UnmapApertureSegment(pBuildPagingBuffer);
                DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s (unmap aperture segment)\n", __FUNCTION__));
                return Status;
            }
        case DXGK_OPERATION_FILL:
            {
                if (pBuildPagingBuffer->Fill.hAllocation == NULL)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("<--- %s (fill) no allocation specified\n", __FUNCTION__));
                    return STATUS_INVALID_PARAMETER;
                }


                VioGpuAllocation *allocation = VioGpuAllocation::FromHandle(pBuildPagingBuffer->Fill.hAllocation);
                if (allocation == NULL)
                    return STATUS_INVALID_HANDLE;
                DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s (fill size=%Iu pattern=%x segment=%d addr=%I64x) res_id=%d isBlob=%d primary=%d\n",
                                               __FUNCTION__,
                                               pBuildPagingBuffer->Fill.FillSize,
                                               pBuildPagingBuffer->Fill.FillPattern,
                                               pBuildPagingBuffer->Fill.Destination.SegmentId,
                                               pBuildPagingBuffer->Fill.Destination.SegmentAddress.QuadPart,
                                               allocation->GetId(),
                                               allocation->IsBlob(),
                                               allocation->IsPrimary()));
                return allocation->PagingFill(pBuildPagingBuffer);
            }
        case DXGK_OPERATION_DISCARD_CONTENT:
            {
                if (pBuildPagingBuffer->DiscardContent.hAllocation == NULL)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("<--- %s (discard) no allocation specified\n", __FUNCTION__));
                    return STATUS_INVALID_PARAMETER;
                }


                VioGpuAllocation *allocation = VioGpuAllocation::FromHandle(pBuildPagingBuffer->DiscardContent.hAllocation);
                if (allocation == NULL)
                    return STATUS_INVALID_HANDLE;
                DbgPrint(TRACE_LEVEL_WARNING, ("<--- %s (discard segment=%d addr=%p) res_id=%d isBlob=%d\n",
                                               __FUNCTION__,
                                               pBuildPagingBuffer->DiscardContent.SegmentId,
                                               pBuildPagingBuffer->DiscardContent.SegmentAddress.QuadPart,
                                               allocation->GetId(),
                                               allocation->IsBlob()));

                // VidMm is freeing this allocation's segment range for reuse.
                // A mappable blob's HOST mapping ideally dies with it (dead
                // processes never send their UMD-side unmap), but no
                // UNMAP_BLOB is emitted from the paging DMA: QEMU's unmap
                // completion is asynchronous (RCU-deferred region teardown)
                // and under process churn a suspended unmap parks the paging
                // fence past dxgkrnl's TDR budget (a ResetFromTimeout within
                // seconds of DISCARD bursts).  The stale-window concern is
                // better fixed host-side by making RES_UNREF drop any live
                // mapping when the blob is destroyed.

                return STATUS_SUCCESS;
            }
#if !defined(VIOGPU_TARGET_VISTA)
        case DXGK_OPERATION_NOTIFY_RESIDENCY:
            {
                if (pBuildPagingBuffer->NotifyResidency.hAllocation == NULL)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("<--- %s (residency) no allocation specified\n", __FUNCTION__));
                    return STATUS_SUCCESS;
                }

                VioGpuAllocation *allocation = VioGpuAllocation::FromHandle(pBuildPagingBuffer->NotifyResidency.hAllocation);
                if (allocation == NULL)
                    return STATUS_INVALID_HANDLE;
                DbgPrint(TRACE_LEVEL_WARNING, ("<--- %s (residency segment=%u padding=%u off=%p resident=%d) res_id=%d isBlob=%d\n",
                                               __FUNCTION__,
                                               pBuildPagingBuffer->NotifyResidency.PhysicalAddress.SegmentId,
                                               pBuildPagingBuffer->NotifyResidency.PhysicalAddress.Padding,
                                               pBuildPagingBuffer->NotifyResidency.PhysicalAddress.SegmentOffset,
                                               pBuildPagingBuffer->NotifyResidency.Resident,
                                               allocation->GetId(),
                                               allocation->IsBlob()));

                return STATUS_SUCCESS;
            }
#endif
        case DXGK_OPERATION_TRANSFER:
            {
                if (pBuildPagingBuffer->Transfer.hAllocation == NULL)
                {
                    return STATUS_INVALID_PARAMETER;
                }
                VioGpuAllocation *allocation = VioGpuAllocation::FromHandle(
                    pBuildPagingBuffer->Transfer.hAllocation);
                if (allocation == NULL)
                {
                    return STATUS_INVALID_HANDLE;
                }
                return allocation->PagingTransfer(pBuildPagingBuffer);
            }
        case DXGK_OPERATION_READ_PHYSICAL:
        case DXGK_OPERATION_WRITE_PHYSICAL:
            {
                // Read / write of a physical page through the
                // adapter is used by the scheduler for diagnostic
                // access; no virtio-gpu equivalent. Report success
                // so the diagnostic does not abort the scheduler.
                DbgPrint(TRACE_LEVEL_WARNING,
                         ("<--- %s (rw_physical op=%d, no-op)\n",
                          __FUNCTION__, pBuildPagingBuffer->Operation));
                return STATUS_SUCCESS;
            }
#if !defined(VIOGPU_TARGET_VISTA)
        case DXGK_OPERATION_SIGNAL_MONITORED_FENCE:
            {
                // The fence-signal operations are paged through the
                // DMA buffer in some scheduler paths; we don't track
                // them but the scheduler does. SUCCESS keeps the
                // pipeline moving; the fence itself is still managed
                // by the normal submit path.
                DbgPrint(TRACE_LEVEL_VERBOSE,
                         ("<--- %s (fence op=%d, deferred to submit path)\n",
                          __FUNCTION__, pBuildPagingBuffer->Operation));
                return STATUS_SUCCESS;
            }
#endif
        default:
            {
                DbgPrint(TRACE_LEVEL_WARNING,
                         ("<--- %s unhandled operation=%d\n",
                          __FUNCTION__, pBuildPagingBuffer->Operation));
                return STATUS_NOT_SUPPORTED;
            }
    };
}

NTSTATUS
APIENTRY
VioGpu3DAcquireSwizzlingRange(_In_ CONST HANDLE hAdapter, _Inout_ DXGKARG_ACQUIRESWIZZLINGRANGE *pAcquireSwizzlingRange)
{
    PAGED_CODE();
    if (hAdapter == NULL || pAcquireSwizzlingRange == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    return STATUS_NOT_SUPPORTED;
}

NTSTATUS
APIENTRY
VioGpu3DReleaseSwizzlingRange(_In_ CONST HANDLE hAdapter, _In_ CONST DXGKARG_RELEASESWIZZLINGRANGE *pReleaseSwizzlingRange)
{
    PAGED_CODE();
    if (hAdapter == NULL || pReleaseSwizzlingRange == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    return STATUS_NOT_SUPPORTED;
}

// DxgkDdiPatch / DxgkDdiSubmitCommand run at DISPATCH_LEVEL and must be
// nonpageable: in the PAGE section they page out under memory pressure and
// the next submission bugchecks D1 (EXECUTE fault at IRQL 2).
#pragma code_seg(push)
#pragma code_seg()
NTSTATUS
APIENTRY
VioGpu3DPatch(_In_ CONST HANDLE hAdapter, _In_ CONST DXGKARG_PATCH *pPatch)
{

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(hAdapter);
    if (pAdapter == NULL || pPatch == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (!pAdapter->IsDriverActive())
    {
        return STATUS_UNSUCCESSFUL;
    }
    return pAdapter->commander.Patch(pPatch);
};

_IRQL_requires_(DISPATCH_LEVEL)
NTSTATUS
APIENTRY
VioGpu3DSubmitCommand(_In_ CONST HANDLE hAdapter, _In_ CONST DXGKARG_SUBMITCOMMAND *pSubmitCommand)
{
    // DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));
    // DbgPrint(TRACE_LEVEL_ERROR, ("Fake imp %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(hAdapter);
    if (pAdapter == NULL || pSubmitCommand == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (!pAdapter->IsDriverActive())
    {
        // DbgPrint(TRACE_LEVEL_ERROR, ("<---> %s VioGpu (%p) is being called when not active!\n", __FUNCTION__,
        // pAdapter));
        return STATUS_UNSUCCESSFUL;
    }
    return pAdapter->commander.SubmitCommand(pSubmitCommand);
};
#pragma code_seg(pop)

NTSTATUS
APIENTRY
VioGpu3DCreateDevice(_In_ CONST HANDLE hAdapter, _Inout_ DXGKARG_CREATEDEVICE *pCreateDevice)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(hAdapter);
    if (pAdapter == NULL || pCreateDevice == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    // Flags and pInfo share the same union in DXGKARG_CREATEDEVICE.  Flags is
    // input, while pInfo is the obsolete pre-context output contract.  Clear
    // pInfo before returning so Vista uses DxgkDdiCreateContext and obtains
    // the DMA/allocation/patch-list sizes from DXGK_CONTEXTINFO.  Leaving the
    // SystemDevice/GdiDevice flag bits in the union makes checked dxgkrnl
    // interpret them as a non-NULL legacy pInfo pointer and then assert on a
    // zero-sized context without ever calling our CreateContext callback.
    pCreateDevice->pInfo = NULL;
    pCreateDevice->hDevice = NULL;

    if (!pAdapter->IsDriverActive())
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("<---> %s VioGpu (%p) is being called when not active!\n", __FUNCTION__, pAdapter));
        return STATUS_UNSUCCESSFUL;
    }

    VioGpuDevice *pDevice = new (VIOGPU_NONPAGED_POOL) VioGpuDevice(pAdapter);
    if (!pDevice)
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("<---> %s failed to allocate VioGpuDevice\n", __FUNCTION__));
        return STATUS_NO_MEMORY;
    }
#if defined(VIOGPU_TARGET_VISTA)
    DbgPrint(TRACE_LEVEL_FATAL,
             ("VISTA-CREATEDEVICE: object constructed device=%p; publishing handle\n",
              pDevice));
#endif
    pCreateDevice->hDevice = pDevice->ToHandle();
#if defined(VIOGPU_TARGET_VISTA)
    DbgPrint(TRACE_LEVEL_FATAL,
             ("VISTA-CREATEDEVICE: handle published handle=%p; returning success\n",
              pCreateDevice->hDevice));
#endif
    return STATUS_SUCCESS;
}

NTSTATUS
APIENTRY
VioGpu3DDestroyDevice(_In_ VOID *pDeviceContext)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_FATAL, ("---> %s 0x%p\n", __FUNCTION__, pDeviceContext));

    VioGpuDevice *pDxContext = VioGpuDevice::FromHandle(pDeviceContext);

    if (pDxContext)
    {
        delete pDxContext;
    }

    DbgPrint(TRACE_LEVEL_FATAL, ("<--- %s\n", __FUNCTION__));
    return STATUS_SUCCESS;
}

NTSTATUS
APIENTRY
VioGpu3DDdiCreateContext(_In_ CONST HANDLE hDevice, _Inout_ DXGKARG_CREATECONTEXT *pCreateContext)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuDevice *pDevice = VioGpuDevice::FromHandle(hDevice);
    if ((pDevice == NULL) || (pCreateContext == NULL))
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (pCreateContext->Flags.GdiContext || pCreateContext->Flags.SystemContext)
    {
        DbgPrint(TRACE_LEVEL_WARNING, ("<---> %s context type: System(%d) GDI(%d) \n",
                                       __FUNCTION__,
                                       pCreateContext->Flags.SystemContext,
                                       pCreateContext->Flags.GdiContext));
    }

    VioGpuDxContext *context = new (VIOGPU_NONPAGED_POOL) VioGpuDxContext(pDevice);
    if (context == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    pCreateContext->hContext = context->ToHandle();

    pCreateContext->ContextInfo.DmaBufferSegmentSet = 0;
    pCreateContext->ContextInfo.DmaBufferSize = 1024 * 1024;
    // Per-command side-band: each submission stores one VioGpuCommand*
    // at offset 0 (see Present/Render).
    pCreateContext->ContextInfo.DmaBufferPrivateDataSize = sizeof(VioGpuCommand *);

#if defined(VIOGPU_TARGET_VISTA)
    pCreateContext->ContextInfo.AllocationListSize = 1024;
    pCreateContext->ContextInfo.PatchLocationListSize = 1024;
#else
    if (pCreateContext->Flags.GdiContext || pCreateContext->Flags.SystemContext)
    {
        pCreateContext->ContextInfo.AllocationListSize = DXGK_ALLOCATION_LIST_SIZE_GDICONTEXT;
        pCreateContext->ContextInfo.PatchLocationListSize = DXGK_ALLOCATION_LIST_SIZE_GDICONTEXT;
    }
    else
    {
        pCreateContext->ContextInfo.AllocationListSize = 1024;
        pCreateContext->ContextInfo.PatchLocationListSize = 1024;
    }
#endif
    return STATUS_SUCCESS;
};

NTSTATUS
APIENTRY
VioGpu3DDdiDestroyContext(_In_ CONST HANDLE hContext)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    VioGpuDxContext *context = VioGpuDxContext::FromHandle(hContext);
    if (context == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }
    delete context;
    return STATUS_SUCCESS;
};

NTSTATUS
APIENTRY
VioGpu3DPresent(_In_ CONST HANDLE hContext, _Inout_ DXGKARG_PRESENT *pPresent)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuDxContext *context = VioGpuDxContext::FromHandle(hContext);
    VioGpuDevice *pDevice = context ? context->GetDevice() : NULL;
    if ((pDevice == NULL) || (pPresent == NULL))
    {
        return STATUS_INVALID_PARAMETER;
    }

    return pDevice->Present(pPresent);
}

NTSTATUS
APIENTRY
VioGpu3DRender(_In_ CONST HANDLE hContext, _Inout_ DXGKARG_RENDER *pRender)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuDxContext *context = VioGpuDxContext::FromHandle(hContext);
    VioGpuDevice *pDevice = context ? context->GetDevice() : NULL;
    if ((pDevice == NULL) || (pRender == NULL))
    {
        return STATUS_INVALID_PARAMETER;
    }

    return pDevice->Render(pRender);
}

#if !defined(VIOGPU_TARGET_VISTA)
NTSTATUS
APIENTRY
VioGpu3DStopDeviceAndReleasePostDisplayOwnership(_In_ VOID *pDeviceContext,
                                                 _In_ D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
                                                 _Out_ DXGK_DISPLAY_INFORMATION *DisplayInfo)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_INFORMATION, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(pDeviceContext);
    if (pAdapter == NULL)
    {
        return;
    }

    return pAdapter->StopDeviceAndReleasePostDisplayOwnership(TargetId, DisplayInfo);
}
#endif

NTSTATUS
APIENTRY
VioGpu3DIsSupportedVidPn(_In_ CONST HANDLE hAdapter, _Inout_ DXGKARG_ISSUPPORTEDVIDPN *pIsSupportedVidPn)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(hAdapter);
    if (pAdapter == NULL || pIsSupportedVidPn == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (!pAdapter->IsDriverActive())
    {
        DbgPrint(TRACE_LEVEL_WARNING, ("VIOGPU (%p) is being called when not active!", pAdapter));
        return STATUS_UNSUCCESSFUL;
    }
    return pAdapter->vidpn.IsSupportedVidPn(pIsSupportedVidPn);
}

NTSTATUS
APIENTRY
VioGpu3DRecommendFunctionalVidPn(_In_ CONST HANDLE hAdapter,
                                 _In_ CONST DXGKARG_RECOMMENDFUNCTIONALVIDPN *CONST pRecommendFunctionalVidPn)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(hAdapter);
    if (pAdapter == NULL || pRecommendFunctionalVidPn == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (!pAdapter->IsDriverActive())
    {
        VIOGPU_LOG_ASSERTION1("VIOGPU (%p) is being called when not active!", pAdapter);
        return STATUS_UNSUCCESSFUL;
    }
    return pAdapter->vidpn.RecommendFunctionalVidPn(pRecommendFunctionalVidPn);
}

NTSTATUS
APIENTRY
VioGpu3DRecommendVidPnTopology(_In_ CONST HANDLE hAdapter,
                               _In_ CONST DXGKARG_RECOMMENDVIDPNTOPOLOGY *CONST pRecommendVidPnTopology)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(hAdapter);
    if (pAdapter == NULL || pRecommendVidPnTopology == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (!pAdapter->IsDriverActive())
    {
        VIOGPU_LOG_ASSERTION1("VIOGPU (%p) is being called when not active!", pAdapter);
        return STATUS_UNSUCCESSFUL;
    }
    return pAdapter->vidpn.RecommendVidPnTopology(pRecommendVidPnTopology);
}

NTSTATUS
APIENTRY
VioGpu3DRecommendMonitorModes(_In_ CONST HANDLE hAdapter,
                              _In_ CONST DXGKARG_RECOMMENDMONITORMODES *CONST pRecommendMonitorModes)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(hAdapter);
    if (pAdapter == NULL || pRecommendMonitorModes == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (!pAdapter->IsDriverActive())
    {
        VIOGPU_LOG_ASSERTION1("VIOGPU (%p) is being called when not active!", pAdapter);
        return STATUS_UNSUCCESSFUL;
    }
    return pAdapter->vidpn.RecommendMonitorModes(pRecommendMonitorModes);
}

NTSTATUS
APIENTRY
VioGpu3DEnumVidPnCofuncModality(_In_ CONST HANDLE hAdapter,
                                _In_ CONST DXGKARG_ENUMVIDPNCOFUNCMODALITY *CONST pEnumCofuncModality)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(hAdapter);
    if (pAdapter == NULL || pEnumCofuncModality == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (!pAdapter->IsDriverActive())
    {
        VIOGPU_LOG_ASSERTION1("VIOGPU (%p) is being called when not active!", pAdapter);
        return STATUS_UNSUCCESSFUL;
    }
    return pAdapter->vidpn.EnumVidPnCofuncModality(pEnumCofuncModality);
}

NTSTATUS
APIENTRY
VioGpu3DSetVidPnSourceVisibility(_In_ CONST HANDLE hAdapter,
                                 _In_ CONST DXGKARG_SETVIDPNSOURCEVISIBILITY *pSetVidPnSourceVisibility)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(hAdapter);
    if (pAdapter == NULL || pSetVidPnSourceVisibility == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (!pAdapter->IsDriverActive())
    {
        VIOGPU_LOG_ASSERTION1("VIOGPU (%p) is being called when not active!", pAdapter);
        return STATUS_UNSUCCESSFUL;
    }
    return pAdapter->vidpn.SetVidPnSourceVisibility(pSetVidPnSourceVisibility);
}

NTSTATUS
APIENTRY
VioGpu3DCommitVidPn(_In_ CONST HANDLE hAdapter, _In_ CONST DXGKARG_COMMITVIDPN *CONST pCommitVidPn)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(hAdapter);
    if (pAdapter == NULL || pCommitVidPn == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (!pAdapter->IsDriverActive())
    {
        VIOGPU_LOG_ASSERTION1("VIOGPU (%p) is being called when not active!", pAdapter);
        return STATUS_UNSUCCESSFUL;
    }
    return pAdapter->vidpn.CommitVidPn(pCommitVidPn);
}

NTSTATUS
APIENTRY
VioGpu3DUpdateActiveVidPnPresentPath(_In_ CONST HANDLE hAdapter,
                                     _In_ CONST DXGKARG_UPDATEACTIVEVIDPNPRESENTPATH *CONST pUpdateActiveVidPnPresentPath)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(hAdapter);
    if (pAdapter == NULL || pUpdateActiveVidPnPresentPath == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (!pAdapter->IsDriverActive())
    {
        VIOGPU_LOG_ASSERTION1("VIOGPU (%p) is being called when not active!", pAdapter);
        return STATUS_UNSUCCESSFUL;
    }
    return pAdapter->vidpn.UpdateActiveVidPnPresentPath(pUpdateActiveVidPnPresentPath);
}

#if !defined(VIOGPU_TARGET_VISTA)
NTSTATUS
APIENTRY
VioGpu3DQueryVidPnHWCapability(_In_ CONST HANDLE hAdapter, _Inout_ DXGKARG_QUERYVIDPNHWCAPABILITY *pVidPnHWCaps)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(hAdapter);
    VIOGPU_ASSERT_CHK(pAdapter != NULL);

    if (!pAdapter->IsDriverActive())
    {
        VIOGPU_LOG_ASSERTION1("VIOGPU (%p) is being called when not active!", pAdapter);
        return STATUS_UNSUCCESSFUL;
    }
    return pAdapter->vidpn.QueryVidPnHWCapability(pVidPnHWCaps);
}

#endif

NTSTATUS
APIENTRY
VioGpu3DDdiControlInterrupt(_In_ CONST HANDLE hAdapter,
                            _In_ CONST DXGK_INTERRUPT_TYPE InterruptType,
                            _In_ BOOLEAN EnableInterrupt)
{
    PAGED_CODE();

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(hAdapter);
    if (pAdapter == NULL || !pAdapter->IsDriverActive())
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (InterruptType != DXGK_INTERRUPT_CRTC_VSYNC)
    {
        // DMA completion and fault notifications come from the virtqueue
        // interrupt path. They are not optional software-generated events.
        return STATUS_NOT_IMPLEMENTED;
    }

    pAdapter->vidpn.SetVsyncEnabled(EnableInterrupt);
    return STATUS_SUCCESS;
};

NTSTATUS
APIENTRY
VioGpu3DDdiGetScanLine(_In_ CONST HANDLE hAdapter, _Inout_ DXGKARG_GETSCANLINE *pGetScanLine)
{
    PAGED_CODE();
    if (VioGpuAdapter::FromHandle(hAdapter) == NULL || pGetScanLine == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    return VioGpuAdapter::FromHandle(hAdapter)->vidpn.GetScanLine(pGetScanLine);
}

// END: Paged Code
#pragma code_seg(pop)

#pragma code_seg(push)
#pragma code_seg()
// BEGIN: Non-Paged Code

VOID VioGpu3DDpcRoutine(_In_ VOID *pDeviceContext)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(pDeviceContext);
    if (pAdapter == NULL)
    {
        return;
    }

    if (!pAdapter->IsHardwareInit())
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("VioGpu (%p) is being called when not active!", pAdapter));
        return;
    }
    pAdapter->DpcRoutine();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

BOOLEAN
VioGpu3DInterruptRoutine(_In_ VOID *pDeviceContext, _In_ ULONG MessageNumber)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(pDeviceContext);
    if (pAdapter == NULL)
    {
        return FALSE;
    }

    return pAdapter->InterruptRoutine(MessageNumber);
}

NTSTATUS
APIENTRY
VioGpu3DSetVidPnSourceAddress(_In_ CONST HANDLE hAdapter,
                               _In_ CONST DXGKARG_SETVIDPNSOURCEADDRESS *pSetVidPnSourceAddress)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(hAdapter);
    if ((pAdapter == NULL) || (pSetVidPnSourceAddress == NULL) ||
        (pSetVidPnSourceAddress->VidPnSourceId >= MAX_VIEWS))
    {
        return STATUS_INVALID_PARAMETER;
    }

    return pAdapter->vidpn.SetVidPnSourceAddress(pSetVidPnSourceAddress);
}

VOID VioGpu3DResetDevice(_In_ VOID *pDeviceContext)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(pDeviceContext);
    if (pAdapter == NULL)
    {
        return;
    }

    pAdapter->ResetDevice();
}

#if !defined(VIOGPU_TARGET_VISTA)
NTSTATUS
APIENTRY
VioGpu3DSystemDisplayEnable(_In_ VOID *pDeviceContext,
                            _In_ D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
                            _In_ PDXGKARG_SYSTEM_DISPLAY_ENABLE_FLAGS Flags,
                            _Out_ UINT *Width,
                            _Out_ UINT *Height,
                            _Out_ D3DDDIFORMAT *ColorFormat)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(pDeviceContext);
    VIOGPU_ASSERT_CHK(pAdapter != NULL);

    return pAdapter->vidpn.SystemDisplayEnable(TargetId, Flags, Width, Height, ColorFormat);
}

VOID APIENTRY VioGpu3DSystemDisplayWrite(_In_ VOID *pDeviceContext,
                                         _In_ VOID *Source,
                                         _In_ UINT SourceWidth,
                                         _In_ UINT SourceHeight,
                                         _In_ UINT SourceStride,
                                         _In_ UINT PositionX,
                                         _In_ UINT PositionY)
{
    DbgPrint(TRACE_LEVEL_INFORMATION, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(pDeviceContext);
    VIOGPU_ASSERT_CHK(pAdapter != NULL);

    pAdapter->vidpn.SystemDisplayWrite(Source, SourceWidth, SourceHeight, SourceStride, PositionX, PositionY);
}

#endif

NTSTATUS
APIENTRY
VioGpu3DDdiPreemptCommand(_In_ CONST HANDLE hAdapter, _In_ CONST DXGKARG_PREEMPTCOMMAND *pPreemptCommand)
{
    // DxgkDdiPreemptCommand documents that any error return triggers
    // bugcheck 0x119 (arg1=2). A NULL deref here would also AV-crash
    // the host. Guard the argument and return SUCCESS.
    if (!pPreemptCommand)
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("<---> %s null pPreemptCommand\n", __FUNCTION__));
        return STATUS_SUCCESS;
    }

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(hAdapter);
    if (pAdapter == NULL)
        return STATUS_INVALID_PARAMETER;

    // Host work cannot be interrupted. Put a fence-only marker behind the
    // outstanding DMA instead of claiming preemption while callbacks and
    // command objects are still live. The worker reports the actual final
    // completed fence when it reaches the marker.
    return pAdapter->commander.QueuePreemption(pPreemptCommand);
};

NTSTATUS
APIENTRY
VioGpu3DDdiRestartFromTimeout(_In_ CONST HANDLE hAdapter)
{
    VioGpuAdapter *adapter = VioGpuAdapter::FromHandle(hAdapter);
    if (adapter == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }
    return adapter->RestartFromTimeout();
};

#if !defined(VIOGPU_TARGET_VISTA)
NTSTATUS
APIENTRY
VioGpu3DDdiCancelCommand(_In_ CONST HANDLE hAdapter, _In_ CONST DXGKARG_CANCELCOMMAND *pCancelCommand)
{
    VioGpuAdapter *adapter = VioGpuAdapter::FromHandle(hAdapter);
    if (adapter == NULL || pCancelCommand == NULL)
    {
        return STATUS_SUCCESS;
    }

    // Dxgkrnl supplies the per-DMA private data we filled in Render.  Do not
    // delete the command here: a virtqueue DPC can still hold its callback
    // pointer.  Releasing the event reference wakes the finite UMD wait, and
    // normal completion retains ownership of the command object.
    if (pCancelCommand->pDmaBufferPrivateData != NULL &&
        pCancelCommand->DmaBufferPrivateDataSize >= sizeof(VioGpuCommand *))
    {
        VioGpuCommand *cmd = VioGpuCommand::FromHandle(
            *(void **)pCancelCommand->pDmaBufferPrivateData);
        if (cmd != NULL)
        {
            cmd->SignalRenderEvent();
        }
    }
    adapter->commander.CancelRenderEvents();

    return STATUS_SUCCESS;
};
#endif

NTSTATUS
APIENTRY
VioGpu3DDdiQueryCurrentFence(_In_ CONST HANDLE hAdapter, _Inout_ DXGKARG_QUERYCURRENTFENCE *pCurrentFence)
{
    // UNREFERENCED_PARAMETER(hAdapter);
    // UNREFERENCED_PARAMETER(pCurrentFence);
    // DbgPrint(TRACE_LEVEL_ERROR, ("<---> %s UNSUPPORTED PREEMPTION FUNCTION\n", __FUNCTION__));

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    VioGpuAdapter *pAdapter = VioGpuAdapter::FromHandle(hAdapter);
    if (pAdapter == NULL || pCurrentFence == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }
    pCurrentFence->CurrentFence = InterlockedOr(&pAdapter->m_LastCompletedFenceId, 0);

    return STATUS_SUCCESS;
};

#if !defined(VIOGPU_TARGET_VISTA)
NTSTATUS
APIENTRY
VioGpu3DDdiResetEngine(_In_ CONST HANDLE hAdapter, _Inout_ DXGKARG_RESETENGINE *pResetEngine)
{
    UNREFERENCED_PARAMETER(pResetEngine);

    VioGpuAdapter *adapter = VioGpuAdapter::FromHandle(hAdapter);
    if (adapter)
    {
        adapter->commander.CancelRenderEvents();
    }

    DbgPrint(TRACE_LEVEL_ERROR, ("<---> %s UNSUPPORTED PREEMPTION FUNCTION\n", __FUNCTION__));

    return STATUS_SUCCESS;
};

NTSTATUS
APIENTRY
VioGpu3DDdiQueryEngineStatus(_In_ CONST HANDLE hAdapter, _Inout_ DXGKARG_QUERYENGINESTATUS *pQueryEngineStatus)
{
    UNREFERENCED_PARAMETER(hAdapter);

    // The scheduler calls this when it suspects a node has stopped making
    // progress (before declaring a TDR). The out parameter MUST be filled:
    // leaving EngineStatus untouched lets the scheduler read an
    // uninitialized Responsive bit and treat the engine as hung.
    //
    // This engine is paravirtual -- a worker thread draining a virtqueue
    // against the host renderer, not real hardware that can wedge. Command
    // completion is driven by host fence responses, so the engine is always
    // able to report progress. Report Responsive.
    if (pQueryEngineStatus)
    {
        pQueryEngineStatus->EngineStatus.Value = 0;
        pQueryEngineStatus->EngineStatus.Responsive = 1;
    }

    return STATUS_SUCCESS;
};

#endif

NTSTATUS
APIENTRY
VioGpu3DDdiCollectDbgInfo(_In_ CONST HANDLE hAdapter, _In_ CONST DXGKARG_COLLECTDBGINFO *pCollectDbgInfo)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pCollectDbgInfo);

    DbgPrint(TRACE_LEVEL_ERROR, ("<---> %s UNSUPPORTED PREEMPTION FUNCTION\n", __FUNCTION__));
    {
        VioGpuAdapter *adapter = VioGpuAdapter::FromHandle(hAdapter);
        if (adapter)
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("<---> %s fence submitted=%d completed=%d\n", __FUNCTION__,
                      adapter->m_LastSubmittedFenceId, adapter->m_LastCompletedFenceId));
        }
    }

    return STATUS_SUCCESS;
};

NTSTATUS
APIENTRY
VioGpu3DDdiResetFromTimeout(_In_ CONST HANDLE hAdapter)
{
    VioGpuAdapter *adapter = VioGpuAdapter::FromHandle(hAdapter);
    if (adapter)
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("<---> %s fence submitted=%d completed=%d\n", __FUNCTION__,
                  adapter->m_LastSubmittedFenceId, adapter->m_LastCompletedFenceId));
        return adapter->ResetFromTimeout();
    }
    return STATUS_INVALID_PARAMETER;
};

#if defined(DBG)

#if defined(COM_DEBUG)

#define RHEL_DEBUG_PORT  ((PUCHAR)0x3F8)
#if defined(VIOGPU_SERIAL_TRACE)
// QEMU's debug console is host-captured and remains available even when the
// guest COM UART is unavailable during early display-driver initialization.
#define QEMU_DEBUGCON_PORT ((PUCHAR)0xE9)
#endif
#define TEMP_BUFFER_SIZE 256

void DebugPrintFuncSerial(CONST char *format, ...)
{
    char buf[TEMP_BUFFER_SIZE];
    NTSTATUS status;
    size_t len;
    va_list list;
    va_start(list, format);
    status = RtlStringCbVPrintfA(buf, sizeof(buf), format, list);
    if (status == STATUS_SUCCESS)
    {
        len = strlen(buf);
    }
    else
    {
        len = 2;
        buf[0] = 'O';
        buf[1] = '\n';
    }
    if (len)
    {
#if !defined(VIOGPU_SERIAL_TRACE)
        WRITE_PORT_BUFFER_UCHAR(RHEL_DEBUG_PORT, (PUCHAR)buf, (ULONG)len);
        WRITE_PORT_UCHAR(RHEL_DEBUG_PORT, '\r');
#else
        // The Vista QEMU diagnostic build captures port E9 directly.  Do not
        // touch COM1 first: with no serial client, HAL's port writer can spin
        // forever polling the UART line-status register at 0x3FD.  That froze
        // dxgkrnl immediately after DxgkDdiCreateDevice returned.
        for (size_t i = 0; i < len; ++i)
        {
            WRITE_PORT_UCHAR(QEMU_DEBUGCON_PORT, (UCHAR)buf[i]);
        }
        WRITE_PORT_UCHAR(QEMU_DEBUGCON_PORT, '\r');
#endif
    }
    va_end(list);
}
#endif

#if defined(PRINT_DEBUG)
void DebugPrintFuncKdPrint(CONST char *format, ...)
{
    va_list list;
    va_start(list, format);
    // This trace-only Vista build is used before a component filter has been
    // configured in KD.  A mask-class message (the normal Triton setting)
    // can therefore be discarded by the Vista checked kernel.  Error level
    // is unconditionally visible to a connected kernel debugger, which makes
    // the first StartDevice failure diagnosable without touching the guest's
    // signature or code-integrity policy.
    vDbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL, format, list);
    va_end(list);
}
#endif

#endif
#pragma code_seg(pop) // End Non-Paged Code
