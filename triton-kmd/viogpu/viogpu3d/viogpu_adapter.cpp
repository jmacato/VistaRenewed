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

#include "helper.h"
#include "driver.h"
#include "viogpu_adapter.h"
#include "baseobj.h"
#include "bitops.h"
#include "viogpum.h"
#include "viogpu_device.h"
#if !DBG
#include "viogpudo.tmh"
#endif

static UINT g_InstanceId = 0;

struct NOTIFY_CONTEXT
{
    DXGKRNL_INTERFACE *pDxgkInterface;
    DXGKARGCB_NOTIFY_INTERRUPT_DATA *interrupt;
    BOOL triggerDpc;
};

BOOLEAN NotifyRoutine(PVOID ctx_void)
{
    // DbgPrint(TRACE_LEVEL_ERROR, ("<---> %s\n", __FUNCTION__));
    NOTIFY_CONTEXT *ctx = (NOTIFY_CONTEXT *)ctx_void;
    DXGKRNL_INTERFACE *pDxgkInterface = ctx->pDxgkInterface;
    pDxgkInterface->DxgkCbNotifyInterrupt(pDxgkInterface->DeviceHandle, ctx->interrupt);
    if (ctx->triggerDpc)
    {
        pDxgkInterface->DxgkCbQueueDpc(pDxgkInterface->DeviceHandle);
    }

    return TRUE;
}

NTSTATUS VioGpuAdapter::NotifyInterrupt(DXGKARGCB_NOTIFY_INTERRUPT_DATA *interruptData, BOOL triggerDpc)
{
    NOTIFY_CONTEXT notify;
    notify.pDxgkInterface = &m_DxgkInterface;
    notify.interrupt = interruptData;
    notify.triggerDpc = triggerDpc;
    BOOLEAN bRet;
    return m_DxgkInterface.DxgkCbSynchronizeExecution(m_DxgkInterface.DeviceHandle, NotifyRoutine, &notify, 0, &bRet);
}

BOOLEAN TryColorFormat(UINT format, virtio_gpu_formats *pColorFormat)
{
    if (pColorFormat == NULL)
    {
        return FALSE;
    }

    switch (format)
    {
        case D3DDDIFMT_A8R8G8B8:
            *pColorFormat = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
            return TRUE;
        case D3DDDIFMT_X8R8G8B8:
            *pColorFormat = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
            return TRUE;
        case D3DDDIFMT_A8B8G8R8:
            *pColorFormat = VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM;
            return TRUE;
        case D3DDDIFMT_X8B8G8R8:
            *pColorFormat = VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM;
            return TRUE;
    }
    return FALSE;
}

virtio_gpu_formats ColorFormat(UINT format)
{
    virtio_gpu_formats colorFormat;
    if (TryColorFormat(format, &colorFormat))
    {
        return colorFormat;
    }
    DbgPrint(TRACE_LEVEL_ERROR, ("---> %s Unsupported color format %d\n", __FUNCTION__, format));
    return VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
}

PAGED_CODE_SEG_BEGIN

VioGpuAdapter::VioGpuAdapter(_In_ DEVICE_OBJECT *pPhysicalDeviceObject)
    : m_pPhysicalDevice(pPhysicalDeviceObject), m_MonitorPowerState(PowerDeviceD0), m_AdapterPowerState(PowerDeviceD0),
      commander(this), vidpn(this)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    *((UINT *)&m_Flags) = 0;
    RtlZeroMemory(&m_DxgkInterface, sizeof(m_DxgkInterface));
    RtlZeroMemory(&m_DeviceInfo, sizeof(m_DeviceInfo));
    RtlZeroMemory(&m_PointerShape, sizeof(m_PointerShape));
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    RtlZeroMemory(&m_VioDev, sizeof(m_VioDev));
    m_Id = g_InstanceId++;
    m_PendingWorks = 0;
    KeInitializeEvent(&m_ConfigUpdateEvent, SynchronizationEvent, FALSE);
    m_bStopWorkThread = FALSE;
    m_pWorkThread = NULL;
    m_ResolutionEvent = NULL;
    m_ResolutionEventHandle = NULL;
    m_u32NumCapsets = 0;
    m_u32NumScanouts = 0;
    m_supportedCapsetIDs = 0;
    m_u64HostFeatures = 0;
    m_u64GuestFeatures = 0;
    m_LastCompletedFenceId = 0;
    m_LastSubmittedFenceId = 0;
    m_TdrResetPending = 0;
    m_PciBus = 0;
    m_PciDev = 0;
    m_PciFunc = 0;
    m_pCursorBuf = NULL;
    m_PointerResource = 0;
    m_PointerX = m_PointerY = 0;
    m_PointerVisible = m_PointerFailed = FALSE;
    // Present-fence completion contexts: fixed-size, allocated at Escape
    // (PASSIVE) and freed from the response DPC (DISPATCH), which is exactly
    // the lookaside contract.
    ExInitializeNPagedLookasideList(&m_PresentFenceLookaside,
                                    NULL,
                                    NULL,
                                    VIOGPU_NPAGED_LOOKASIDE_POOL,
                                    sizeof(PRESENT_FENCE_CTX),
                                    'fPgV',
                                    0);
}

VioGpuAdapter::~VioGpuAdapter(void)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    CloseResolutionEvent();
    m_Flags.DriverStarted = FALSE;
    VioGpuAdapterClose();
    commander.Stop();
    HWClose();
    ExDeleteNPagedLookasideList(&m_PresentFenceLookaside);
    m_Id = 0;
}

BOOLEAN VioGpuAdapter::CheckHardware()
{
    PAGED_CODE();

    NTSTATUS Status = STATUS_GRAPHICS_DRIVER_MISMATCH;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    PCI_COMMON_HEADER Header = {0};
    ULONG BytesRead;

    Status = m_DxgkInterface.DxgkCbReadDeviceSpace(m_DxgkInterface.DeviceHandle,
                                                   DXGK_WHICHSPACE_CONFIG,
                                                   &Header,
                                                   0,
                                                   sizeof(Header),
                                                   &BytesRead);

    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("DxgkCbReadDeviceSpace failed with status 0x%X\n", Status));
        return FALSE;
    }
    DbgPrint(TRACE_LEVEL_INFORMATION,
             ("<--- %s VendorId = 0x%04X DeviceId = 0x%04X\n", __FUNCTION__, Header.VendorID, Header.DeviceID));
    if (Header.VendorID == REDHAT_PCI_VENDOR_ID && Header.DeviceID == 0x1050)
    {
        SetVgaDevice(Header.SubClass == PCI_SUBCLASS_VID_VGA_CTLR);
        return TRUE;
    }

    return FALSE;
}

#pragma warning(disable : 4702)
NTSTATUS VioGpuAdapter::StartDevice(_In_ DXGK_START_INFO *pDxgkStartInfo,
                                    _In_ DXGKRNL_INTERFACE *pDxgkInterface,
                                    _Out_ ULONG *pNumberOfViews,
                                    _Out_ ULONG *pNumberOfChildren)
{
    PAGED_CODE();

    NTSTATUS Status;
    VIOGPU_ASSERT(pDxgkStartInfo != NULL);
    VIOGPU_ASSERT(pDxgkInterface != NULL);
    VIOGPU_ASSERT(pNumberOfViews != NULL);
    VIOGPU_ASSERT(pNumberOfChildren != NULL);
    RtlCopyMemory(&m_DxgkInterface, pDxgkInterface, sizeof(m_DxgkInterface));

    Status = m_DxgkInterface.DxgkCbGetDeviceInformation(m_DxgkInterface.DeviceHandle, &m_DeviceInfo);
    if (!NT_SUCCESS(Status))
    {
        VIOGPU_LOG_ASSERTION1("DxgkCbGetDeviceInformation failed with status 0x%X\n", Status);
        return Status;
    }

    DbgPrint(TRACE_LEVEL_FATAL,
             ("VISTA-START: device information acquired; beginning hardware initialization\n"));

    if (!CheckHardware())
    {
        Status = STATUS_GRAPHICS_DRIVER_MISMATCH;
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("StartDevice found an unsupported PCI display device\n"));
        return Status;
    }

    Status = GetRegisterInfo();
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_WARNING, ("GetRegisterInfo failed with status 0x%X\n", Status));
    }

    Status = GetPCIInfo();
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_WARNING, ("GetPCIInfo failed with status 0x%X\n", Status));
    }

    Status = HWInit(m_DeviceInfo.TranslatedResourceList, FALSE);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("HWInit failed with status 0x%X\n", Status));
        VioGpuAdapterClose();
        HWClose();
        return Status;
    }

    DbgPrint(TRACE_LEVEL_FATAL,
             ("VISTA-START: hardware initialization and capset discovery complete\n"));

    if (!AckFeature(VIRTIO_GPU_F_VIRGL))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("VioGpu3D cannot start because virgl is not enabled\n"));
        VioGpuAdapterClose();
        HWClose();
        return STATUS_UNSUCCESSFUL;
    }

    if (!AckFeature(VIRTIO_GPU_F_RESOURCE_BLOB))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("VioGpu3D cannot start because blob resources are not enabled\n"));
        VioGpuAdapterClose();
        HWClose();
        return STATUS_UNSUCCESSFUL;
    }

    if (!AckFeature(VIRTIO_GPU_F_CONTEXT_INIT))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("VioGpu3D cannot start because context init is not enabled\n"));
        VioGpuAdapterClose();
        HWClose();
        return STATUS_UNSUCCESSFUL;
    }

    // Vista's milcore uses HardwareInformation.MemorySize as an input to
    // GraphicsAccelerationTier::GetTier before it creates the D3D9 device.
    // Zero selects the unaccelerated tier even when the WDDM adapter and UMD
    // otherwise satisfy the Aero contract.  Neptune's host-memory aperture
    // is 1 GiB in the Vista launch profile; expose a conservative 128 MiB
    // budget so Vista reaches tier 2 (the checked binary's threshold is
    // 120 MiB) without claiming the whole host allocation.
    static const DWORD vistaAeroMemorySize = 128u * 1024u * 1024u;
    Status = SetRegisterInfo(GetInstanceId(), vistaAeroMemorySize);
    if (!NT_SUCCESS(Status))
    {
        VIOGPU_LOG_ASSERTION1("RegisterHWInfo failed with status 0x%X\n", Status);
        VioGpuAdapterClose();
        HWClose();
        return Status;
    }

    Status = commander.Start();
    if (!NT_SUCCESS(Status))
    {
        VioGpuAdapterClose();
        commander.Stop();
        HWClose();
        return Status;
    }
    Status = vidpn.Start(pNumberOfViews, pNumberOfChildren);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("VioGpuVidPN::Start failed with status 0x%X\n", Status));
        VioGpuAdapterClose();
        commander.Stop();
        HWClose();
        VioGpuDbgBreak();
        return Status;
    }

    m_Flags.DriverStarted = TRUE;

#if defined(VIOGPU_TARGET_VISTA)
    // DXGK_START_INFO did not provide an adapter LUID on WDDM 1.0.  The
    // private UMD ABI keeps this field for layout compatibility only.
    RtlZeroMemory(&m_AdapterLuid, sizeof(m_AdapterLuid));
#else
    m_AdapterLuid = pDxgkStartInfo->AdapterLuid;
#endif

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return STATUS_SUCCESS;
}

NTSTATUS VioGpuAdapter::StopDevice(VOID)
{
    PAGED_CODE();
    m_Flags.DriverStarted = FALSE;
    // Close the queues first. VioGpuAdapterClose fires every pending vbuf
    // callback while the command worker and its allocation state still
    // exist. Commander::Stop then drains the commands those callbacks
    // re-queued against the now-closed queue, where submission fails
    // synchronously and cannot strand another host callback.
    VioGpuAdapterClose();
    commander.Stop();
    HWClose();
    return STATUS_SUCCESS;
}

NTSTATUS VioGpuAdapter::ResetFromTimeout(VOID)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_ERROR,
             ("---> %s: stopping hung virtio generation\n", __FUNCTION__));
    InterlockedExchange(&m_TdrResetPending, TRUE);
    m_Flags.DriverStarted = FALSE;

    // This is the scheduler-reset path, not DxgkDdiResetDevice's bugcheck
    // display callback.  It must close all queues before returning so no DMA
    // from the timed-out generation can reach the host afterward.
    VioGpuAdapterClose();
    commander.Stop();
    HWClose();

    InterlockedExchange(&m_LastCompletedFenceId, 0);
    InterlockedExchange(&m_LastSubmittedFenceId, 0);
    m_PendingWorks = 0;
    return STATUS_SUCCESS;
}

NTSTATUS VioGpuAdapter::RestartFromTimeout(VOID)
{
    PAGED_CODE();
    ULONG numberOfViews = 0;
    ULONG numberOfChildren = 0;
    NTSTATUS status;

    if (InterlockedCompareExchange(&m_TdrResetPending, TRUE, TRUE) == FALSE)
    {
        return IsHardwareInit() ? STATUS_SUCCESS : STATUS_DEVICE_NOT_READY;
    }

    DbgPrint(TRACE_LEVEL_ERROR,
             ("---> %s: creating fresh virtio generation\n", __FUNCTION__));
    status = HWInit(m_DeviceInfo.TranslatedResourceList, TRUE);
    if (!NT_SUCCESS(status))
    {
        goto RestartFailed;
    }
    if (!AckFeature(VIRTIO_GPU_F_VIRGL) ||
        !AckFeature(VIRTIO_GPU_F_RESOURCE_BLOB) ||
        !AckFeature(VIRTIO_GPU_F_CONTEXT_INIT))
    {
        status = STATUS_GRAPHICS_DRIVER_MISMATCH;
        goto RestartFailed;
    }

    status = commander.Start();
    if (!NT_SUCCESS(status))
    {
        goto RestartFailed;
    }

    status = vidpn.Start(&numberOfViews, &numberOfChildren);
    if (!NT_SUCCESS(status) || numberOfViews != MAX_VIEWS ||
        numberOfChildren != MAX_CHILDREN)
    {
        if (NT_SUCCESS(status))
        {
            status = STATUS_DEVICE_CONFIGURATION_ERROR;
        }
        goto RestartFailed;
    }

    m_Flags.DriverStarted = TRUE;
    InterlockedExchange(&m_TdrResetPending, FALSE);
    DbgPrint(TRACE_LEVEL_ERROR,
             ("<--- %s: transport restarted\n", __FUNCTION__));
    return STATUS_SUCCESS;

RestartFailed:
    DbgPrint(TRACE_LEVEL_ERROR,
             ("<--- %s failed status=0x%x\n", __FUNCTION__, status));
    VioGpuAdapterClose();
    commander.Stop();
    HWClose();
    return status;
}

NTSTATUS VioGpuAdapter::DispatchIoRequest(_In_ ULONG VidPnSourceId, _In_ VIDEO_REQUEST_PACKET *pVideoRequestPacket)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(VidPnSourceId);
    UNREFERENCED_PARAMETER(pVideoRequestPacket);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--> %s\n", __FUNCTION__));

    // The 3D driver does not implement any video IOCTLs; reporting
    // STATUS_SUCCESS would let callers read uninitialized response
    // data as if it had been populated.
    return STATUS_NOT_SUPPORTED;
}

PCHAR
DbgDevicePowerString(__in DEVICE_POWER_STATE Type)
{
    PAGED_CODE();

    switch (Type)
    {
        case PowerDeviceUnspecified:
            return "PowerDeviceUnspecified";
        case PowerDeviceD0:
            return "PowerDeviceD0";
        case PowerDeviceD1:
            return "PowerDeviceD1";
        case PowerDeviceD2:
            return "PowerDeviceD2";
        case PowerDeviceD3:
            return "PowerDeviceD3";
        case PowerDeviceMaximum:
            return "PowerDeviceMaximum";
        default:
            return "UnKnown Device Power State";
    }
}

PCHAR
DbgPowerActionString(__in POWER_ACTION Type)
{
    PAGED_CODE();

    switch (Type)
    {
        case PowerActionNone:
            return "PowerActionNone";
        case PowerActionReserved:
            return "PowerActionReserved";
        case PowerActionSleep:
            return "PowerActionSleep";
        case PowerActionHibernate:
            return "PowerActionHibernate";
        case PowerActionShutdown:
            return "PowerActionShutdown";
        case PowerActionShutdownReset:
            return "PowerActionShutdownReset";
        case PowerActionShutdownOff:
            return "PowerActionShutdownOff";
        case PowerActionWarmEject:
            return "PowerActionWarmEject";
        default:
            return "UnKnown Device Power State";
    }
}

NTSTATUS VioGpuAdapter::SetPowerState(_In_ ULONG HardwareUid,
                                      _In_ DEVICE_POWER_STATE DevicePowerState,
                                      _In_ POWER_ACTION ActionType)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(ActionType);

    DbgPrint(TRACE_LEVEL_FATAL,
             ("---> %s HardwareUid = 0x%x ActionType = %s DevicePowerState = %s AdapterPowerState = %s\n",
              __FUNCTION__,
              HardwareUid,
              DbgPowerActionString(ActionType),
              DbgDevicePowerString(DevicePowerState),
              DbgDevicePowerString(m_AdapterPowerState)));

    if (DevicePowerState == PowerDeviceUnspecified)
    {
        return STATUS_SUCCESS;
    }
    if (DevicePowerState < PowerDeviceD0 ||
        DevicePowerState > PowerDeviceD3)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (HardwareUid == DISPLAY_ADAPTER_HW_ID)
    {
        if (DevicePowerState == PowerDeviceD0 && !IsHardwareInit())
        {
            // Rebuilding only the virtqueues here would leave the buffer
            // pool, worker, contexts, resources, and scanout in the old
            // generation.  PnP StartDevice is the only full initializer.
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("%s cannot resume an uninitialized adapter\n",
                      __FUNCTION__));
            return STATUS_DEVICE_NOT_READY;
        }

        // virtio-gpu has no device-power command.  Keep the transport and
        // VidPn state intact across Dx power transitions; tearing down only
        // part of the adapter here made D3 -> D0 resume irrecoverable.  PnP
        // StopDevice remains the complete teardown path.
        if (DevicePowerState == PowerDeviceD3)
        {
            commander.CancelRenderEvents();
        }
        m_AdapterPowerState = DevicePowerState;
        DbgPrint(TRACE_LEVEL_INFORMATION,
                 ("%s adapter entering D%d with virtio transport retained\n",
                  __FUNCTION__, DevicePowerState - PowerDeviceD0));
        return STATUS_SUCCESS;
    }

    if (HardwareUid < MAX_CHILDREN)
    {
        m_MonitorPowerState = DevicePowerState;
        return STATUS_SUCCESS;
    }

    return STATUS_INVALID_PARAMETER;
}

NTSTATUS
VioGpuAdapter::QueryChildRelations(_Out_writes_bytes_(ChildRelationsSize) DXGK_CHILD_DESCRIPTOR *pChildRelations,
                                   _In_ ULONG ChildRelationsSize)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    if (pChildRelations == NULL ||
        (ChildRelationsSize % sizeof(DXGK_CHILD_DESCRIPTOR)) != 0)
    {
        return STATUS_INVALID_PARAMETER;
    }

    const ULONG requiredSize =
        (MAX_CHILDREN + 1) * sizeof(DXGK_CHILD_DESCRIPTOR);
    if (ChildRelationsSize < requiredSize)
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    // The final zero descriptor terminates the relation list.
    RtlZeroMemory(pChildRelations, ChildRelationsSize);

    for (UINT ChildIndex = 0; ChildIndex < MAX_CHILDREN; ++ChildIndex)
    {
        pChildRelations[ChildIndex].ChildDeviceType = TypeVideoOutput;
        pChildRelations[ChildIndex].ChildCapabilities.HpdAwareness = IsVgaDevice() ? HpdAwarenessAlwaysConnected
                                                                                   : HpdAwarenessInterruptible;
        // Virtual virtio-gpu scanouts have no physical connector.
        // VOT_OTHER is the documented catch-all; HD15 would identify
        // the output as analog VGA D-Sub and gate off HDR/VRR via
        // connector-type heuristics in the shell.
        pChildRelations[ChildIndex].ChildCapabilities.Type.VideoOutput.InterfaceTechnology = IsVgaDevice() ? D3DKMDT_VOT_INTERNAL
                                                                                                           : D3DKMDT_VOT_OTHER;
        pChildRelations[ChildIndex].ChildCapabilities.Type.VideoOutput.MonitorOrientationAwareness = D3DKMDT_MOA_NONE;
        pChildRelations[ChildIndex].ChildCapabilities.Type.VideoOutput.SupportsSdtvModes = FALSE;
        pChildRelations[ChildIndex].AcpiUid = 0;
        pChildRelations[ChildIndex].ChildUid = ChildIndex;
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return STATUS_SUCCESS;
}

NTSTATUS VioGpuAdapter::QueryChildStatus(_Inout_ DXGK_CHILD_STATUS *pChildStatus, _In_ BOOLEAN NonDestructiveOnly)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    UNREFERENCED_PARAMETER(NonDestructiveOnly);
    if (pChildStatus == NULL || pChildStatus->ChildUid >= MAX_CHILDREN)
    {
        return STATUS_INVALID_PARAMETER;
    }

    switch (pChildStatus->Type)
    {
        case StatusConnection:
            {
                pChildStatus->HotPlug.Connected = IsDriverActive();
                return STATUS_SUCCESS;
            }

        case StatusRotation:
            {
                // Vista queries rotation while it builds the monitor object
                // even when QueryChildRelations reports D3DKMDT_MOA_NONE.
                // Returning STATUS_NOT_SUPPORTED makes checked dxgkrnl call
                // watchdog!WdLogEvent5 and break in
                // DXGMONITOR::DetermineMonitorConnectivityInfo.  This virtual
                // output is not rotated, so report the required zero angle.
                pChildStatus->Rotation.Angle = 0;
                return STATUS_SUCCESS;
            }

        default:
            {
                DbgPrint(TRACE_LEVEL_WARNING, ("Unknown pChildStatus->Type (0x%I64x) requested.", pChildStatus->Type));
                return STATUS_NOT_SUPPORTED;
            }
    }
}

NTSTATUS VioGpuAdapter::QueryDeviceDescriptor(_In_ ULONG ChildUid, _Inout_ DXGK_DEVICE_DESCRIPTOR *pDeviceDescriptor)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    if (pDeviceDescriptor == NULL || ChildUid >= MAX_CHILDREN ||
        (pDeviceDescriptor->DescriptorLength != 0 &&
         pDeviceDescriptor->DescriptorBuffer == NULL))
    {
        return STATUS_INVALID_PARAMETER;
    }
    PBYTE edid = vidpn.GetEdidData(ChildUid);

    if (!edid)
    {
        return STATUS_GRAPHICS_CHILD_DESCRIPTOR_NOT_SUPPORTED;
    }
    else if (pDeviceDescriptor->DescriptorOffset < EDID_RAW_BLOCK_SIZE)
    {
        ULONG len = min(pDeviceDescriptor->DescriptorLength,
                        (EDID_RAW_BLOCK_SIZE - pDeviceDescriptor->DescriptorOffset));
        RtlCopyMemory(pDeviceDescriptor->DescriptorBuffer, (edid + pDeviceDescriptor->DescriptorOffset), len);
        pDeviceDescriptor->DescriptorLength = len;
        return STATUS_SUCCESS;
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return STATUS_MONITOR_NO_MORE_DESCRIPTOR_DATA;
}

// The cursor queue is independent of rendering. Wait only for shape uploads,
// never for pointer motion. A timed-out upload retains its backing until reset.
NTSTATUS VioGpuAdapter::PointerCommand(const void *data, UINT size, BOOLEAN cursor,
                                      PGPU_MEM_ENTRY entries, UINT count)
{
    PGPU_VBUFFER buffer = NULL;
    PVOID command = cursor ? m_CursorQueue.AllocCursor(&buffer) : ctrlQueue.AllocCmd(&buffer, size);
    VioGpuQueue *queue = cursor ? (VioGpuQueue *)&m_CursorQueue : (VioGpuQueue *)&ctrlQueue;
    if (!command) {
        delete[] reinterpret_cast<PBYTE>(entries);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlCopyMemory(command, data, size);
    buffer->data_buf = entries;
    buffer->data_size = count * sizeof(*entries);
    PVIOGPU_WAIT_CTX wait = VioGpuAllocWaitCtx();
    if (!wait) { queue->ReleaseBuffer(buffer); return STATUS_INSUFFICIENT_RESOURCES; }
    wait->vbuf = buffer;
    InterlockedIncrement(&wait->refCount);
    InterlockedIncrement(&buffer->ref_count);
    buffer->complete_cb = VioGpuWaitCtxCompleteCB;
    buffer->complete_ctx = wait;
    UINT submitted = cursor ? m_CursorQueue.QueueCursor(buffer) : ctrlQueue.QueueBuffer(buffer);
    if (submitted == (UINT)-1) {
        VioGpuWaitCtxFinish(wait, buffer, queue, STATUS_SUCCESS);
        queue->ReleaseBuffer(buffer);
        return STATUS_DEVICE_NOT_READY;
    }
    LARGE_INTEGER timeout;
    timeout.QuadPart = -20000000; // two seconds; late callbacks own heap state
    NTSTATUS status = KeWaitForSingleObject(&wait->event, Executive, KernelMode, FALSE, &timeout);
    if (!VioGpuWaitCtxFinish(wait, buffer, queue, status)) return STATUS_IO_TIMEOUT;
    if (NT_SUCCESS(status) && !cursor &&
        (!buffer->resp_buf || ((PGPU_CTRL_HDR)buffer->resp_buf)->type != VIRTIO_GPU_RESP_OK_NODATA))
        status = STATUS_UNSUCCESSFUL;
    queue->ReleaseBuffer(buffer);
    return status;
}

NTSTATUS VioGpuAdapter::SetPointerPosition(const DXGKARG_SETPOINTERPOSITION *position)
{
    PAGED_CODE();
    if (position->VidPnSourceId != 0) return STATUS_INVALID_PARAMETER;
    m_PointerX = position->X;
    m_PointerY = position->Y;
    m_PointerVisible = position->Flags.Visible;
    if (!m_PointerResource || !IsHardwareInit()) return STATUS_SUCCESS;
    PGPU_VBUFFER buffer = NULL;
    PGPU_UPDATE_CURSOR cmd = (PGPU_UPDATE_CURSOR)m_CursorQueue.AllocCursor(&buffer);
    if (!cmd) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(cmd, sizeof(*cmd));
    cmd->hdr.type = VIRTIO_GPU_CMD_MOVE_CURSOR;
    // WDDM positions the bitmap origin; virtio/QEMU positions its hotspot.
    cmd->pos.x = (UINT)((LONGLONG)m_PointerX + m_PointerShape.XHot);
    cmd->pos.y = (UINT)((LONGLONG)m_PointerY + m_PointerShape.YHot);
    cmd->resource_id = m_PointerVisible && !m_PointerFailed ? m_PointerResource : 0;
    return m_CursorQueue.QueueCursor(buffer) == (UINT)-1 ? STATUS_DEVICE_NOT_READY : STATUS_SUCCESS;
}

NTSTATUS VioGpuAdapter::SetPointerShape(const DXGKARG_SETPOINTERSHAPE *shape)
{
    PAGED_CODE();
    if (shape->VidPnSourceId != 0 || shape->Flags.Value != 2 ||
        !shape->pPixels || !shape->Width || !shape->Height ||
        shape->Width > 64 || shape->Height > 64 || shape->Pitch < shape->Width * 4 ||
        shape->XHot >= shape->Width || shape->YHot >= shape->Height)
        return STATUS_NOT_SUPPORTED;
    if (!IsHardwareInit() || m_PointerFailed) return STATUS_DEVICE_NOT_READY;
    NTSTATUS status = STATUS_SUCCESS;
    if (!m_PointerResource) {
        if (!m_CursorSegment.Init(64 * 64 * 4, NULL)) return STATUS_INSUFFICIENT_RESOURCES;
        m_PointerResource = resourceIdr.GetId();
        GPU_RES_CREATE_2D create = {};
        create.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
        create.resource_id = m_PointerResource;
        create.format = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
        create.width = create.height = 64;
        status = PointerCommand(&create, sizeof(create), FALSE, NULL, 0);
        if (!NT_SUCCESS(status)) goto fail;
        {
            PSCATTER_GATHER_LIST sg = m_CursorSegment.GetSGList();
            PGPU_MEM_ENTRY entries = new (VIOGPU_NONPAGED_POOL) GPU_MEM_ENTRY[sg->NumberOfElements];
            if (!entries) { status = STATUS_INSUFFICIENT_RESOURCES; goto fail; }
            for (UINT i = 0; i < sg->NumberOfElements; ++i) {
                entries[i].addr = sg->Elements[i].Address.QuadPart;
                entries[i].length = sg->Elements[i].Length;
                entries[i].padding = 0;
            }
            GPU_RES_ATTACH_BACKING attach = {};
            attach.hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
            attach.resource_id = m_PointerResource;
            attach.nr_entries = sg->NumberOfElements;
            status = PointerCommand(&attach, sizeof(attach), FALSE, entries, attach.nr_entries);
            if (!NT_SUCCESS(status)) goto fail;
        }
    }
    // The previous UPDATE_CURSOR has been consumed before this backing is reused.
    RtlZeroMemory(m_CursorSegment.GetVirtualAddress(), 64 * 64 * 4);
    for (UINT y = 0; y < shape->Height; ++y)
        RtlCopyMemory((PBYTE)m_CursorSegment.GetVirtualAddress() + y * 256,
                      (const BYTE *)shape->pPixels + (SIZE_T)y * shape->Pitch, shape->Width * 4);
    {
        GPU_RES_TRANSF_TO_HOST_2D transfer = {};
        transfer.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
        transfer.resource_id = m_PointerResource;
        transfer.r.width = transfer.r.height = 64;
        status = PointerCommand(&transfer, sizeof(transfer), FALSE, NULL, 0);
        if (!NT_SUCCESS(status)) goto fail;
        GPU_UPDATE_CURSOR update = {};
        update.hdr.type = VIRTIO_GPU_CMD_UPDATE_CURSOR;
        update.resource_id = m_PointerResource;
        update.pos.x = (UINT)((LONGLONG)m_PointerX + shape->XHot);
        update.pos.y = (UINT)((LONGLONG)m_PointerY + shape->YHot);
        update.hot_x = shape->XHot;
        update.hot_y = shape->YHot;
        status = PointerCommand(&update, sizeof(update), TRUE, NULL, 0);
        if (!NT_SUCCESS(status)) goto fail;
        m_PointerShape.XHot = shape->XHot;
        m_PointerShape.YHot = shape->YHot;
        if (!m_PointerVisible) {
            DXGKARG_SETPOINTERPOSITION position = {};
            position.X = m_PointerX; position.Y = m_PointerY;
            return SetPointerPosition(&position);
        }
        return STATUS_SUCCESS;
    }
fail:
    m_PointerFailed = TRUE;
    // Do not free/rewrite a buffer that an outstanding transfer can still read.
    // Device reset releases the host resource before the backing is freed.
    return status;
}

NTSTATUS VioGpuAdapter::QueryAdapterInfo(_In_ CONST DXGKARG_QUERYADAPTERINFO *pQueryAdapterInfo)
{
    PAGED_CODE();

    if (pQueryAdapterInfo == NULL || pQueryAdapterInfo->pOutputData == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }
    DbgPrint(TRACE_LEVEL_VERBOSE,
             ("---> %s type=%u outputSize=%u\n",
              __FUNCTION__,
              (UINT)pQueryAdapterInfo->Type,
              pQueryAdapterInfo->OutputDataSize));

    switch (pQueryAdapterInfo->Type)
    {
        case DXGKQAITYPE_UMDRIVERPRIVATE:
            {
                if (pQueryAdapterInfo->OutputDataSize < sizeof(VIOGPU_ADAPTERINFO))
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("pQueryAdapterInfo->OutputDataSize (0x%u) is smaller than sizeof(VIOGPU_ADAPTERINFO) "
                              "(0x%u)\n",
                              pQueryAdapterInfo->OutputDataSize,
                              sizeof(VIOGPU_ADAPTERINFO))) return STATUS_BUFFER_TOO_SMALL;
                }
                const ULONG privateInfoSize =
                    (pQueryAdapterInfo->OutputDataSize >= sizeof(VIOGPU_ADAPTERINFO_V2))
                        ? sizeof(VIOGPU_ADAPTERINFO_V2)
                        : sizeof(VIOGPU_ADAPTERINFO);
                RtlZeroMemory(pQueryAdapterInfo->pOutputData, privateInfoSize);
                VIOGPU_ADAPTERINFO *info = (VIOGPU_ADAPTERINFO *)pQueryAdapterInfo->pOutputData;
                info->IamVioGPU = VIOGPU_IAM;
                // Report against m_u64GuestFeatures (what was actually
                // negotiated) so UMD never sees a flag we did not ack.
                // virtio-gpu silently ignores unset feature bits, so an
                // over-claimed hint would let UMD send fields the host
                // disregards.
                info->Flags.Supports3d = virtio_is_feature_enabled(m_u64GuestFeatures, VIRTIO_GPU_F_VIRGL) &&
                                         virtio_is_feature_enabled(m_u64GuestFeatures, VIRTIO_GPU_F_RESOURCE_BLOB) &&
                                         virtio_is_feature_enabled(m_u64GuestFeatures, VIRTIO_GPU_F_CONTEXT_INIT);
                info->Flags.HasShmem = HasUsableShmem();
                info->Flags.Reserved = 0;
                info->SupportedCapsetIDs = m_supportedCapsetIDs;
                info->AdapterLuid = m_AdapterLuid;

                // V2 is an append-only extension of the original UMD private
                // data.  Do not require it: modern UMDs still send a V1-sized
                // buffer and must continue to work unchanged.  Vista's D3D9
                // transport requests V2 and refuses to use render-event
                // completion unless this feature is present.
                if (pQueryAdapterInfo->OutputDataSize >= sizeof(VIOGPU_ADAPTERINFO_V2))
                {
                    VIOGPU_ADAPTERINFO_V2 *infoV2 =
                        (VIOGPU_ADAPTERINFO_V2 *)pQueryAdapterInfo->pOutputData;
                    infoV2->StructureSize = sizeof(*infoV2);
                    infoV2->PrivateAbiVersion = VIOGPU_PRIVATE_ABI_VERSION_V2;
                    infoV2->FeatureBits = VIOGPU_FEATURE_RENDER_EVENT;

                    DbgPrint(TRACE_LEVEL_INFORMATION,
                             ("<--- %s UMDRIVERPRIVATE size=%u iam=%I64x flags=%#x "
                              "capsets=%I64x abiSize=%u abiVersion=%u features=%I64x\n",
                              __FUNCTION__,
                              pQueryAdapterInfo->OutputDataSize,
                              infoV2->V1.IamVioGPU,
                              (infoV2->V1.Flags.Supports3d ? 1u : 0u) |
                                  (infoV2->V1.Flags.HasShmem ? 2u : 0u),
                              infoV2->V1.SupportedCapsetIDs,
                              infoV2->StructureSize,
                              infoV2->PrivateAbiVersion,
                              infoV2->FeatureBits));
                }
                else
                {
                    DbgPrint(TRACE_LEVEL_INFORMATION,
                             ("<--- %s UMDRIVERPRIVATE-V1 size=%u iam=%I64x flags=%#x "
                              "capsets=%I64x\n",
                              __FUNCTION__,
                              pQueryAdapterInfo->OutputDataSize,
                              info->IamVioGPU,
                              (info->Flags.Supports3d ? 1u : 0u) |
                                  (info->Flags.HasShmem ? 2u : 0u),
                              info->SupportedCapsetIDs));
                }
                return STATUS_SUCCESS;
            }
        case DXGKQAITYPE_DRIVERCAPS:
            {
                // DXGK_DRIVERCAPS grew in Win7 and again in Win8.  A Vista
                // dxgkrnl supplies only the original prefix, whose last
                // member is GpuEngineTopology.  Do not use sizeof() for the
                // Vista build: a newer build header would make us reject the
                // valid WDDM 1.0 buffer before it reaches the OS.
#if defined(VIOGPU_TARGET_VISTA)
                const ULONG driverCapsSize =
                    FIELD_OFFSET(DXGK_DRIVERCAPS, GpuEngineTopology) +
                    sizeof(DXGK_GPUENGINETOPOLOGY);
#else
                const ULONG driverCapsSize = sizeof(DXGK_DRIVERCAPS);
#endif
                if (pQueryAdapterInfo->OutputDataSize < driverCapsSize)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("pQueryAdapterInfo->OutputDataSize (0x%u) is smaller than sizeof(DXGK_DRIVERCAPS) "
                              "(0x%u)\n",
                              pQueryAdapterInfo->OutputDataSize,
                              driverCapsSize));
                    return STATUS_BUFFER_TOO_SMALL;
                }

                DXGK_DRIVERCAPS *pDriverCaps = (DXGK_DRIVERCAPS *)pQueryAdapterInfo->pOutputData;
#if !defined(VIOGPU_TARGET_VISTA)
                DbgPrint(TRACE_LEVEL_ERROR,
                         ("InterruptMessageNumber = %d, WDDMVersion = %d\n",
                          pDriverCaps->InterruptMessageNumber,
                          pDriverCaps->WDDMVersion));
#endif
                RtlZeroMemory(pDriverCaps, driverCapsSize);
#if !defined(VIOGPU_TARGET_VISTA)
                pDriverCaps->WDDMVersion = DXGKDDI_WDDMv1_3;
#endif
                pDriverCaps->HighestAcceptableAddress.QuadPart = (ULONG64)-1;

#if !defined(VIOGPU_TARGET_VISTA)
                pDriverCaps->PreemptionCaps.GraphicsPreemptionGranularity = D3DKMDT_GRAPHICS_PREEMPTION_NONE;
                pDriverCaps->PreemptionCaps.ComputePreemptionGranularity = D3DKMDT_COMPUTE_PREEMPTION_NONE;
#endif

#if defined(VIOGPU_TARGET_VISTA)
                /* Match the WDDM 1.0 contract used by the old VirtualBox
                 * Vista display driver.  Vista's compositor uses blt
                 * presents here; advertising MMIO flips changes the DWM
                 * scheduling path without providing the old SVGA flip
                 * semantics. */
                pDriverCaps->PresentationCaps.NoScreenToScreenBlt = 1;
                pDriverCaps->PresentationCaps.NoOverlapScreenBlt = 1;
                pDriverCaps->PresentationCaps.AlignmentShift = 2;
                pDriverCaps->PresentationCaps.MaxTextureWidthShift = 2;
                pDriverCaps->PresentationCaps.MaxTextureHeightShift = 2;
                pDriverCaps->MaxQueuedFlipOnVSync = 0;
                pDriverCaps->FlipCaps.Value = 0;
                pDriverCaps->SchedulingCaps.Value = 0;
#else
                // Flip-model presents arrive via DxgkDdiPresent with Flags.Flip
                // (no MMIO PhysicalAddress is required); VioGpuDevice::Present
                // latches the new primary in m_sourceRes and the vsync Flip
                // scans it out by res_id.
                pDriverCaps->FlipCaps.FlipOnVSyncMmIo = TRUE;
                pDriverCaps->MaxQueuedFlipOnVSync = 1;
#endif

                // WDDM 1.0 needs a usable allocation-list range and one
                // scheduling node.  Zero nodes prevents device contexts from
                // being assigned to the command engine.
                pDriverCaps->MaxAllocationListSlotId = 16;
                // On WDDM 1.0 this bit selects the context-aware scheduling
                // contract.  Vista calls DxgkDdiCreateContext only when it is
                // set; clearing it selects the obsolete per-device DMA-info
                // path even though CreateDevice returns pInfo == NULL.  This
                // driver submits through per-context handles, so advertise the
                // one implemented engine and its context callbacks together.
                pDriverCaps->SchedulingCaps.MultiEngineAware = 1;
#if defined(VIOGPU_TARGET_VISTA)
                pDriverCaps->MemoryManagementCaps.Value = 0;
                pDriverCaps->MemoryManagementCaps.PagingNode = 0;
#endif
                pDriverCaps->GpuEngineTopology.NbAsymetricProcessingNodes = 1;

#if !defined(VIOGPU_TARGET_VISTA)
                pDriverCaps->MemoryManagementCaps.SectionBackedPrimary = TRUE;
                pDriverCaps->SupportDirectFlip = 0;
                pDriverCaps->SchedulingCaps.PreemptionAware = 1;
                pDriverCaps->SupportSmoothRotation = FALSE;
                pDriverCaps->SupportNonVGA = IsVgaDevice();
#endif

                pDriverCaps->MaxPointerWidth = 64;
                pDriverCaps->MaxPointerHeight = 64;
                pDriverCaps->PointerCaps.Color = 1;

                // Surely this is enough...
                // pDriverCaps->NumberOfSwizzlingRanges = 1024;

                DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s Driver caps return\n", __FUNCTION__));
                return STATUS_SUCCESS;
            }
        case VIOGPU_QUERY_SEGMENT_TYPE:
            {
                if (pQueryAdapterInfo->OutputDataSize < sizeof(VIOGPU_QUERYSEGMENTOUT))
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("pQueryAdapterInfo->OutputDataSize (0x%u) is smaller than sizeof(VIOGPU_QUERYSEGMENTOUT) "
                              "(0x%u)\n",
                              pQueryAdapterInfo->OutputDataSize,
                              sizeof(VIOGPU_QUERYSEGMENTOUT)));
                    return STATUS_BUFFER_TOO_SMALL;
                }

                DbgPrint(TRACE_LEVEL_ERROR, ("QUERY SEG\n"));
                VIOGPU_QUERYSEGMENTOUT *pSegmentInfo =
                    (VIOGPU_QUERYSEGMENTOUT *)pQueryAdapterInfo->pOutputData;
                VIOGPU_SEGMENTDESCRIPTOR *pSegmentDesc =
                    pSegmentInfo->pSegmentDescriptor;

                // The query is two-phase.  Preserve the descriptor pointer
                // supplied by dxgkrnl, but initialize every output field on
                // both the count-only call and the descriptor-fill call.
                RtlZeroMemory(pSegmentInfo, sizeof(*pSegmentInfo));
                pSegmentInfo->pSegmentDescriptor = pSegmentDesc;

#if defined(VIOGPU_TARGET_VISTA)
                // Segment 1 is fixed VGA memory for Vista's CPU-accessed
                // shared primary. Segment 2 is the guest-page aperture and
                // optional segment 3 is the mappable host-memory BAR.
                pSegmentInfo->NbSegment = HasUsableShmem() ? 3 : 2;
#else
                pSegmentInfo->NbSegment = HasUsableShmem() ? 2 : 1;
#endif

                pSegmentInfo->PagingBufferPrivateDataSize = 0;
                pSegmentInfo->PagingBufferSegmentId = 0;
                pSegmentInfo->PagingBufferSize = 10 * PAGE_SIZE;

                if (pSegmentDesc != NULL)
                {
                    memset(pSegmentDesc, 0, sizeof(*pSegmentDesc) * pSegmentInfo->NbSegment);

#if defined(VIOGPU_TARGET_VISTA)
                    ULONGLONG frameBufferSize = GetFrameBufferSize();
                    if (GetFrameBufferPA().QuadPart == 0 ||
                        frameBufferSize == 0 ||
                        frameBufferSize > (ULONGLONG)(SIZE_T)-1)
                    {
                        return STATUS_DEVICE_CONFIGURATION_ERROR;
                    }

                    // Vista's VIDMM_GLOBAL::SetupPrimaryCpuAccess constructs
                    // an MDL from CpuTranslatedAddress. A shared primary in an
                    // aperture with CpuTranslatedAddress == 0 reaches the
                    // checked MmRotatePhysicalView assertion. Match the WDDM
                    // reference layout: a fixed CPU-visible VRAM segment for
                    // primaries followed by a page-backed aperture.
                    pSegmentDesc[0].BaseAddress.QuadPart = FRAMEBUFFER_GPU_BASE_VA;
                    pSegmentDesc[0].CpuTranslatedAddress = GetFrameBufferPA();
                    pSegmentDesc[0].Size = (SIZE_T)frameBufferSize;
                    pSegmentDesc[0].CommitLimit = 0;
                    pSegmentDesc[0].Flags.CpuVisible = TRUE;

                    VIOGPU_SEGMENTDESCRIPTOR *apertureDesc = &pSegmentDesc[1];
#else
                    VIOGPU_SEGMENTDESCRIPTOR *apertureDesc = &pSegmentDesc[0];
#endif

                    // 0 = paging buffers from SYSTEM memory. The CPU-visible
                    // aperture contains DXGK-supplied, locked guest pages.
                    // Pointing the paging pool there
                    // leaves VIDMM_DMA_POOL::BeginCPUAccess with nothing to
                    // map, and every TDR's debug-info collection bugchecks
                    // 0x7E instead of recovering (dxgmms1 null-class AV).
                    // VBoxMPWddm uses 0 as well.

                    //
                    // Fill out aperture segment descriptor
                    //
                    apertureDesc->BaseAddress.QuadPart = VioGpuAdapter::APERTURE_GPU_BASE_VA;
                    // (SIZE_T) so the product is not evaluated in int -- 1GiB
                    // fits today, but doubling the constant would overflow.
                    apertureDesc->Size = VioGpuAdapter::APERTURE_SIZE;
                    apertureDesc->CommitLimit = VioGpuAdapter::APERTURE_SIZE;
                    apertureDesc->Flags.Aperture = TRUE;
                    // Every allocation placed here is reported CpuVisible.
                    // Vista's VidMm checks that contract for a standard
                    // shared-primary allocation and rejects the allocation if
                    // any supported segment lacks DXGK_SEGMENTFLAGS::CpuVisible.
                    // An aperture remains CPU-visible through its mapped MDL;
                    // it does not require a fixed CpuTranslatedAddress.
                    apertureDesc->Flags.CpuVisible = TRUE;
#if !defined(VIOGPU_TARGET_VISTA)
                    apertureDesc->Flags.CacheCoherent = TRUE;
                    apertureDesc->Flags.DirectFlip = TRUE;
#endif

                    if (HasUsableShmem())
                    {
                        VIOGPU_SEGMENTDESCRIPTOR *shmemDesc =
                            &pSegmentDesc[SHMEM_SEGMENT_ID - 1];
                        shmemDesc->BaseAddress.QuadPart = VioGpuAdapter::SHMEM_GPU_BASE_VA;
                        shmemDesc->Size = (SIZE_T)m_VioDev.shmem.length;
                        shmemDesc->CommitLimit = (SIZE_T)m_VioDev.shmem.length;
                        // FIXME: is this correct?
                        shmemDesc->CpuTranslatedAddress.QuadPart = m_PciResources.GetPciBar(m_VioDev.shmem.bar)->GetPA().QuadPart + m_VioDev.shmem.offset;
                        shmemDesc->Flags.Aperture = FALSE;
                        shmemDesc->Flags.CacheCoherent = TRUE;
                        shmemDesc->Flags.CpuVisible = TRUE;
#if !defined(VIOGPU_TARGET_VISTA)
                        shmemDesc->Flags.DirectFlip = TRUE;
#endif
                    }
                }
                DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s Requested segments\n", __FUNCTION__));
                return STATUS_SUCCESS;
            }

        default:
            {
                DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s unknown type %d\n", __FUNCTION__, pQueryAdapterInfo->Type));
                return STATUS_NOT_SUPPORTED;
            }
    }
}

// Defined in the non-paged section below; runs at DISPATCH from the completion DPC.
static void PresentFenceCb(void *ctx, void *unused1, void *unused2);

NTSTATUS VioGpuAdapter::Escape(_In_ CONST DXGKARG_ESCAPE *pEscape)
{
    PAGED_CODE();

    if ((pEscape == NULL) || (pEscape->pPrivateDriverData == NULL))
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (pEscape->PrivateDriverDataSize >= sizeof(TRITON_TRACE_REQUEST) &&
        ((TRITON_TRACE_REQUEST *)pEscape->pPrivateDriverData)->type == TRITON_TRACE_ESCAPE_TYPE) {
        TRITON_TRACE_REQUEST *request = (TRITON_TRACE_REQUEST *)pEscape->pPrivateDriverData;
        if (request->operation == TT_MARK) {
            if (request->length != pEscape->PrivateDriverDataSize - 4 ||
                !request->run || request->run != VioGpuTraceRun() || !request->frame)
                return STATUS_INVALID_PARAMETER;
            VioGpuDevice *device = VioGpuDevice::FromHandle(pEscape->hDevice);
            if (!device) return STATUS_INVALID_PARAMETER;
            device->traceTag.run = request->run;
            device->traceTag.frame = request->frame;
            device->traceTag.command = 0;
            device->traceTag.context = device->m_Context.GetId();
            device->traceTag.pid = HandleToULong(PsGetCurrentProcessId());
            VioGpuTraceRecord(TT_KMD_MARK, device->traceTag, request->arg0, request->arg1);
            request->arg0 = device->traceTag.context;
            request->arg1 = KeQueryPerformanceCounter(NULL).QuadPart;
            return STATUS_SUCCESS;
        }
        return VioGpuTraceControl(request, pEscape->PrivateDriverDataSize);
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s Flags = %d\n", __FUNCTION__, pEscape->Flags.Value));
    PVIOGPU_ESCAPE pVioGpuEscape = (PVIOGPU_ESCAPE)pEscape->pPrivateDriverData;
    NTSTATUS status = STATUS_SUCCESS;

    const UINT headerSize = sizeof(pVioGpuEscape->Type) + sizeof(pVioGpuEscape->DataLength);
    UINT size = pEscape->PrivateDriverDataSize;
    if (size < headerSize)
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("%s buffer too small %d, should be at least %zu\n",
                  __FUNCTION__,
                  size,
                  headerSize));
        return STATUS_INVALID_BUFFER_SIZE;
    }
    if (pVioGpuEscape->DataLength > size - headerSize)
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("%s payload length %u exceeds supplied buffer %u\n",
                  __FUNCTION__,
                  pVioGpuEscape->DataLength,
                  size - headerSize));
        return STATUS_INVALID_BUFFER_SIZE;
    }

    switch (pVioGpuEscape->Type)
    {
        case VIOGPU_GET_DEVICE_ID:
            {
                CreateResolutionEvent();
                size = sizeof(ULONG);
                if (pVioGpuEscape->DataLength < size)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s buffer too small %d, should be at least %d\n",
                              __FUNCTION__,
                              pVioGpuEscape->DataLength,
                              size));
                    return STATUS_INVALID_BUFFER_SIZE;
                }
                pVioGpuEscape->Id = m_Id;
                break;
            }
        case VIOGPU_GET_CUSTOM_RESOLUTION:
            {
                size = sizeof(VIOGPU_DISP_MODE);
                if (pVioGpuEscape->DataLength < size)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s buffer too small %d, should be at least %d\n",
                              __FUNCTION__,
                              pVioGpuEscape->DataLength,
                              size));
                    return STATUS_INVALID_BUFFER_SIZE;
                }
                vidpn.EscapeCustomResoulution(&pVioGpuEscape->Resolution);
                break;
            }
        case VIOGPU_GET_CAPS:
            {
                size = sizeof(VIOGPU_CAPSET_REQ);
                if (pVioGpuEscape->DataLength < size)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s buffer too small %d, should be at least %d\n",
                              __FUNCTION__,
                              pVioGpuEscape->DataLength,
                              size));
                    return STATUS_INVALID_BUFFER_SIZE;
                }

                if (pVioGpuEscape->Capset.CapsetId == 0 ||
                    pVioGpuEscape->Capset.CapsetId > VIRTIO_GPU_MAX_CAPSET_ID)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s capset id %llu out of range\n",
                              __FUNCTION__,
                              (ULONGLONG)pVioGpuEscape->Capset.CapsetId));
                    return STATUS_INVALID_PARAMETER_1;
                }
                if (!(m_supportedCapsetIDs & (1ull << pVioGpuEscape->Capset.CapsetId)))
                {
                    DbgPrint(TRACE_LEVEL_ERROR, ("%s capset id is not supported\n", __FUNCTION__));
                    return STATUS_INVALID_PARAMETER_1;
                }
                CAPSET_INFO *pCapsetInfo = &m_capsetInfos[pVioGpuEscape->Capset.CapsetId];
                if (pCapsetInfo->max_version < pVioGpuEscape->Capset.Version)
                {
                    DbgPrint(TRACE_LEVEL_ERROR, ("%s capset version is too low\n", __FUNCTION__));
                    return STATUS_INVALID_PARAMETER_2;
                };

                PGPU_VBUFFER vbuf = NULL;
                if (!ctrlQueue.AskCapset(&vbuf,
                                         pVioGpuEscape->Capset.CapsetId,
                                         pCapsetInfo->max_size,
                                         pVioGpuEscape->Capset.Version) ||
                    vbuf == NULL)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s AskCapset failed for capset id %llu\n",
                              __FUNCTION__,
                              (ULONGLONG)pVioGpuEscape->Capset.CapsetId));
                    status = STATUS_IO_TIMEOUT;
                    break;
                }
                __try
                {
                    UCHAR *buf = ((PGPU_RESP_CAPSET)vbuf->resp_buf)->capset_data;
                    ULONG to_copy = min(pVioGpuEscape->Capset.Size, pCapsetInfo->max_size);
                    UCHAR *userCapset = VIOGPU_UM_PTR_AS(UCHAR *, pVioGpuEscape->Capset.Capset);
                    ProbeForWrite(userCapset, to_copy, sizeof(UCHAR));
                    memcpy(userCapset, buf, to_copy);
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    DbgPrint(TRACE_LEVEL_WARNING, ("Failed to copy capset to user buffer"));
                    status = STATUS_INVALID_PARAMETER;
                }
                ctrlQueue.ReleaseBuffer(vbuf);

                break;
            }
        case VIOGPU_GET_PCI_INFO:
            {
                size = sizeof(VIOGPU_PCI_INFO_REQ);
                if (pVioGpuEscape->DataLength < size)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s buffer too small %d, should be at least %d\n",
                              __FUNCTION__,
                              pVioGpuEscape->DataLength,
                              size));
                    return STATUS_INVALID_BUFFER_SIZE;
                }
                pVioGpuEscape->PciInfo.Domain = 0; // TODO: How to get domain?
                pVioGpuEscape->PciInfo.Bus = m_PciBus;
                pVioGpuEscape->PciInfo.Dev = m_PciDev;
                pVioGpuEscape->PciInfo.Func = m_PciFunc;
                break;
            }
        case VIOGPU_RES_INFO:
            {
                size = sizeof(VIOGPU_RES_INFO_REQ);
                if (pVioGpuEscape->DataLength < size)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s buffer too small %d, should be at least %d\n",
                              __FUNCTION__,
                              pVioGpuEscape->DataLength,
                              size));
                    return STATUS_INVALID_BUFFER_SIZE;
                }
                VioGpuAllocation *allocation = AllocationFromHandle(pVioGpuEscape->ResourceInfo.ResHandle);
                if (allocation == NULL)
                {
                    DbgPrint(TRACE_LEVEL_ERROR, ("%s invalid handle\n", __FUNCTION__));
                    return STATUS_INVALID_PARAMETER;
                }

                status = allocation->EscapeResourceInfo(&pVioGpuEscape->ResourceInfo);

                break;
            }
        case VIOGPU_RES_BUSY:
            {
                size = sizeof(VIOGPU_RES_BUSY_REQ);
                if (pVioGpuEscape->DataLength < size)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s buffer too small %d, should be at least %d\n",
                              __FUNCTION__,
                              pVioGpuEscape->DataLength,
                              size));
                    return STATUS_INVALID_BUFFER_SIZE;
                }
                VioGpuAllocation *allocation = AllocationFromHandle(pVioGpuEscape->ResourceBusy.ResHandle);
                if (allocation == NULL)
                {
                    DbgPrint(TRACE_LEVEL_ERROR, ("%s invalid handle\n", __FUNCTION__));
                    return STATUS_INVALID_PARAMETER;
                }
                status = allocation->EscapeResourceBusy(&pVioGpuEscape->ResourceBusy);

                break;
            }
        case VIOGPU_CTX_INIT:
            {
                size = sizeof(VIOGPU_CTX_INIT_REQ);
                if (pVioGpuEscape->DataLength < size)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s buffer too small %d, should be at least %d\n",
                              __FUNCTION__,
                              pVioGpuEscape->DataLength,
                              size));
                    return STATUS_INVALID_BUFFER_SIZE;
                }
                if ((pVioGpuEscape->CtxInit.CapsetID == 0) ||
                    (pVioGpuEscape->CtxInit.CapsetID > VIRTIO_GPU_MAX_CAPSET_ID) ||
                    !(m_supportedCapsetIDs & (1ull << pVioGpuEscape->CtxInit.CapsetID)) ||
                    (pVioGpuEscape->CtxInit.NumRings == 0) ||
                    (pVioGpuEscape->CtxInit.NumRings > 64) ||
                    (pVioGpuEscape->CtxInit.DebugName[sizeof(pVioGpuEscape->CtxInit.DebugName) - 1] != 0))
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s invalid context request capset=%u rings=%u\n",
                              __FUNCTION__,
                              pVioGpuEscape->CtxInit.CapsetID,
                              pVioGpuEscape->CtxInit.NumRings));
                    return STATUS_INVALID_PARAMETER;
                }
                VioGpuDevice *pDevice = VioGpuDevice::FromHandle(pEscape->hDevice);
                if (pDevice == NULL)
                {
                    DbgPrint(TRACE_LEVEL_ERROR, ("%s no hDdevice(context) supplied\n", __FUNCTION__));
                    return STATUS_INVALID_PARAMETER;
                }

                bool needsVirgl =
                    pVioGpuEscape->CtxInit.CapsetID == VIRTIO_GPU_CAPSET_VENUS ||
                    pVioGpuEscape->CtxInit.CapsetID == VIRTIO_GPU_CAPSET_NEPTUNE;
                bool has_virgl =
                    !!(m_supportedCapsetIDs & (1llu << VIRTIO_GPU_CAPSET_VIRGL));
                bool has_virgl2 =
                    !!(m_supportedCapsetIDs & (1llu << VIRTIO_GPU_CAPSET_VIRGL2));
                if (needsVirgl && !has_virgl && !has_virgl2)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s device does not support required virgl shadow context\n",
                              __FUNCTION__));
                    return STATUS_NOT_SUPPORTED;
                }

                status = pDevice->m_Context.Init(&pVioGpuEscape->CtxInit);
                if (!NT_SUCCESS(status))
                {
                    return status;
                }
                // The UMD references the context by its virtio id when it
                // targets cross-device submits (IMPORT present_ctx_id).
                pVioGpuEscape->CtxInit.CtxId = pDevice->m_Context.GetId();

                if (pVioGpuEscape->CtxInit.CapsetID == VIRTIO_GPU_CAPSET_VENUS ||
                    pVioGpuEscape->CtxInit.CapsetID == VIRTIO_GPU_CAPSET_NEPTUNE)
                {
                    VIOGPU_CTX_INIT_REQ VirglCtx;
                    memset(&VirglCtx, 0, sizeof(VirglCtx));
                    VirglCtx.CapsetID = has_virgl2 ? VIRTIO_GPU_CAPSET_VIRGL2 : VIRTIO_GPU_CAPSET_VIRGL;
                    VirglCtx.NumRings = 64;
                    memcpy(VirglCtx.DebugName, "virgl-shadow-win32", sizeof("virgl-shadow-win32") - 1);

                    status = pDevice->m_Virgl.Init(&VirglCtx);
                    if (!NT_SUCCESS(status))
                    {
                        return status;
                    }
                }

                break;
            }
        case VIOGPU_BLIT_INIT:
            {
                size = sizeof(VIOGPU_BLIT_INIT_REQ);
                if (pVioGpuEscape->DataLength < size)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s buffer too small %d, should be at least %d\n",
                              __FUNCTION__,
                              pVioGpuEscape->DataLength,
                              size));
                    return STATUS_INVALID_BUFFER_SIZE;
                }
                VioGpuDevice *pDevice = VioGpuDevice::FromHandle(pEscape->hDevice);
                if (pDevice == NULL)
                {
                    DbgPrint(TRACE_LEVEL_ERROR, ("%s no hDdevice(context) supplied\n", __FUNCTION__));
                    return STATUS_INVALID_PARAMETER;
                }

                PKEVENT newEventUM = NULL;
                PKEVENT newEventKM = NULL;
                if (!NT_SUCCESS(ObReferenceObjectByHandle(VioGpuUmHandleValue(pVioGpuEscape->BlitInit.EventUM),
                                                          SYNCHRONIZE | EVENT_MODIFY_STATE,
                                                          *ExEventObjectType,
                                                          UserMode,
                                                          (void **)&newEventUM,
                                                          NULL)))
                {
                    DbgPrint(TRACE_LEVEL_ERROR, ("---> %s: Unable to reference user-mode event object 0x%llx\n", __FUNCTION__, pVioGpuEscape->BlitInit.EventUM));
                    return STATUS_INVALID_HANDLE;
                }

                if (!NT_SUCCESS(ObReferenceObjectByHandle(VioGpuUmHandleValue(pVioGpuEscape->BlitInit.EventKM),
                                                          SYNCHRONIZE | EVENT_MODIFY_STATE,
                                                          *ExEventObjectType,
                                                          UserMode,
                                                          (void **)&newEventKM,
                                                          NULL)))
                {
                    ObDereferenceObject(newEventUM);
                    DbgPrint(TRACE_LEVEL_ERROR, ("---> %s: Unable to reference user-mode event object 0x%llx\n", __FUNCTION__, pVioGpuEscape->BlitInit.EventKM));
                    return STATUS_INVALID_HANDLE;
                }

                PKEVENT oldEventUM = pDevice->m_hUM;
                PKEVENT oldEventKM = pDevice->m_hKM;
                pDevice->m_hUM = newEventUM;
                pDevice->m_hKM = newEventKM;
                pDevice->m_pBlit = VIOGPU_UM_PTR_AS(PVIOGPU_BLIT_PRESENT, pVioGpuEscape->BlitInit.pBlitPresent);
                if (oldEventUM != NULL)
                {
                    ObDereferenceObject(oldEventUM);
                }
                if (oldEventKM != NULL)
                {
                    ObDereferenceObject(oldEventKM);
                }
                break;
            }
        case VIOGPU_SUBMIT_PRESENT_FENCE:
            {
                size = sizeof(VIOGPU_PRESENT_FENCE_REQ);
                if (pVioGpuEscape->DataLength < size)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s buffer too small %d, should be at least %d\n",
                              __FUNCTION__, pVioGpuEscape->DataLength, size));
                    return STATUS_INVALID_BUFFER_SIZE;
                }
                VioGpuDevice *pDevice = VioGpuDevice::FromHandle(pEscape->hDevice);
                if (pDevice == NULL)
                {
                    DbgPrint(TRACE_LEVEL_ERROR, ("%s no hDevice(context) supplied\n", __FUNCTION__));
                    return STATUS_INVALID_PARAMETER;
                }
                if ((pVioGpuEscape->PresentFence.RingIdx == 0) ||
                    (pVioGpuEscape->PresentFence.RingIdx >= pDevice->m_Context.GetNumRings()))
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s invalid present-fence ring %u (context rings=%u)\n",
                              __FUNCTION__,
                              pVioGpuEscape->PresentFence.RingIdx,
                              pDevice->m_Context.GetNumRings()));
                    return STATUS_INVALID_PARAMETER;
                }
                // EventUM is optional.  The npt transport passes an event so
                // its guest-side waiter can observe GPU completion (windowed
                // presents block on it; the fence-feedback path also uses it);
                // a caller that only needs the flip-gate token may pass 0.
                PKEVENT pEvent = NULL;
                if (pVioGpuEscape->PresentFence.EventUM != 0 &&
                    !NT_SUCCESS(ObReferenceObjectByHandle(VioGpuUmHandleValue(pVioGpuEscape->PresentFence.EventUM),
                                                          SYNCHRONIZE | EVENT_MODIFY_STATE,
                                                          *ExEventObjectType,
                                                          UserMode,
                                                          (void **)&pEvent,
                                                          NULL)))
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("---> %s: SUBMIT_PRESENT_FENCE bad event 0x%llx\n",
                              __FUNCTION__, pVioGpuEscape->PresentFence.EventUM));
                    return STATUS_INVALID_HANDLE;
                }

                PRESENT_FENCE_CTX *pCtx = (PRESENT_FENCE_CTX *)
                    ExAllocateFromNPagedLookasideList(&m_PresentFenceLookaside);
                if (pCtx == NULL)
                {
                    if (pEvent != NULL)
                    {
                        ObDereferenceObject(pEvent);
                    }
                    return STATUS_INSUFFICIENT_RESOURCES;
                }
                pCtx->pAdapter = this;
                pCtx->pEvent = pEvent;

                // Empty fenced SUBMIT_3D on the event ring.  The host defers the
                // used-ring response until the fence's D3DMetal proxy signals (real
                // GPU completion), so PresentFenceCb fires exactly when the frame
                // finished rendering on the host GPU.
                status = ctrlQueue.SubmitCommand(NULL, 0, pDevice->m_Context.GetId(), TRUE,
                                                 pVioGpuEscape->PresentFence.RingIdx,
                                                 PresentFenceCb, pCtx);
                break;
            }
        default:
            DbgPrint(TRACE_LEVEL_ERROR, ("%s: invalid Escape type 0x%x\n", __FUNCTION__, pVioGpuEscape->Type));
            status = STATUS_INVALID_PARAMETER;
    }

    return status;
}

NTSTATUS VioGpuAdapter::QueryInterface(_In_ CONST PQUERY_INTERFACE pQueryInterface)
{
    PAGED_CODE();

    VIOGPU_ASSERT(pQueryInterface != NULL);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s Version = %d\n", __FUNCTION__, pQueryInterface->Version));

    return STATUS_NOT_SUPPORTED;
}

#if !defined(VIOGPU_TARGET_VISTA)
NTSTATUS VioGpuAdapter::StopDeviceAndReleasePostDisplayOwnership(_In_ D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
                                                                 _Out_ DXGK_DISPLAY_INFORMATION *pDisplayInfo)
{
    PAGED_CODE();

    VIOGPU_ASSERT(TargetId < MAX_CHILDREN);
    // SetPowerState's first argument is a HardwareUid -- the adapter's
    // own DISPLAY_ADAPTER_HW_ID, not a video-target id. Passing the
    // child TargetId here short-circuits the function (TargetId never
    // matches DISPLAY_ADAPTER_HW_ID), so the D0 wake-up is skipped.
    if (m_MonitorPowerState > PowerDeviceD0)
    {
        SetPowerState(DISPLAY_ADAPTER_HW_ID, PowerDeviceD0, PowerActionNone);
    }
    vidpn.ReleasePostDisplayOwnership(TargetId, pDisplayInfo);
    return StopDevice();
}
#endif

PAGED_CODE_SEG_END

//
// Non-Paged Code
//
#pragma code_seg(push)
#pragma code_seg()

// SUBMIT_PRESENT_FENCE completion: the host retired the event-ring fence (real GPU
// completion), so wake the UMD's waiter.  Runs at DISPATCH from the response
// DPC drain -- must be non-paged.  The UMD's own handle keeps the event referenced,
// so ObDereferenceObject here never triggers deletion at raised IRQL.
static void PresentFenceCb(void *ctx, void *, void *)
{
    VioGpuAdapter::PRESENT_FENCE_CTX *pCtx = (VioGpuAdapter::PRESENT_FENCE_CTX *)ctx;
    VioGpuAdapter *pAdapter = pCtx->pAdapter;

    if (pCtx->pEvent != NULL)
    {
        KeSetEvent(pCtx->pEvent, IO_NO_INCREMENT, FALSE);
        ObDereferenceObject(pCtx->pEvent);
    }

    ExFreeToNPagedLookasideList(&pAdapter->m_PresentFenceLookaside, pCtx);
}

VOID VioGpuAdapter::DpcRoutine(VOID)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    PGPU_VBUFFER pvbuf = NULL;
    UINT len = 0;
    ULONG reason;
    while ((reason = InterlockedExchange((PLONG)&m_PendingWorks, 0)) != 0)
    {
        if ((reason & ISR_REASON_DISPLAY))
        {
            while ((pvbuf = ctrlQueue.DequeueBuffer(&len)) != NULL)
            {
                DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s ctrlQueue pvbuf = %p len = %d\n", __FUNCTION__, pvbuf, len));

                PGPU_CTRL_HDR pcmd = (PGPU_CTRL_HDR)pvbuf->buf;
                PGPU_CTRL_HDR resp = (PGPU_CTRL_HDR)pvbuf->resp_buf;

                // resp_buf is allocated in GetBuf alongside the vbuf,
                // so in normal flow it's never NULL -- but defensive:
                // a vbuf rebuilt without a response (zero resp_size)
                // would land here with resp == NULL, and the error
                // check below would deref it.
                if (!resp)
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("<--> %s pvbuf=%p has no resp_buf for cmd_type=0x%x\n",
                              __FUNCTION__, pvbuf, pcmd ? pcmd->type : 0));
                    if (pvbuf->complete_cb != NULL &&
                        InterlockedExchange(&pvbuf->complete_fired, 1) == 0)
                    {
                        pvbuf->complete_cb(pvbuf->complete_ctx, pvbuf->buf, NULL);
                    }
                    ctrlQueue.ReleaseQueueBuffer(pvbuf);
                    ctrlQueue.ReleaseBuffer(pvbuf);
                    continue;
                }

                if (resp->type >= VIRTIO_GPU_RESP_ERR_UNSPEC)
                {
                    if (pcmd->type == VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB)
                    {
                        PGPU_RES_CREATE_BLOB blob_req = (PGPU_RES_CREATE_BLOB)pvbuf->buf;
                        DbgPrint(TRACE_LEVEL_FATAL,
                                 ("!!!!! Command %x (create blob) for res_id=%d blob_id=%lld flags=%x failed: %x\n",
                                  pcmd->type,
                                  blob_req->resource_id,
                                  blob_req->blob_id,
                                  blob_req->blob_flags,
                                  resp->type));
                    }
                    else
                    {
                        DbgPrint(TRACE_LEVEL_FATAL, ("!!!!! Command %x failed: %x\n", pcmd->type, resp->type));
                    }
                    // The completion callback fires unconditionally
                    // below: callbacks that care about host-side
                    // failure (Ask*/Create*) must inspect resp->type
                    // before treating the call as successful. The
                    // command-submission path (QueueRunningCb) does
                    // not yet surface the error to DXGK; without the
                    // per-fence tracking that lives in the venus
                    // backend, the fence still completes from DXGK's
                    // point of view.
                }
                if (resp->type != VIRTIO_GPU_RESP_OK_NODATA)
                {
                    DbgPrint(TRACE_LEVEL_VERBOSE,
                             ("<--- %s type = %xlu flags = %lu fence_id = %llu ctx_id = %lu cmd_type = %lu\n",
                              __FUNCTION__,
                              resp->type,
                              resp->flags,
                              resp->fence_id,
                              resp->ctx_id,
                              pcmd->type));
                }
                if (pvbuf->complete_cb != NULL &&
                    InterlockedExchange(&pvbuf->complete_fired, 1) == 0)
                {
                    pvbuf->complete_cb(pvbuf->complete_ctx, pvbuf->buf, pvbuf->resp_buf);
                }
                ctrlQueue.ReleaseQueueBuffer(pvbuf);
                ctrlQueue.ReleaseBuffer(pvbuf);
            };
        }
        if ((reason & ISR_REASON_CURSOR))
        {
            while ((pvbuf = m_CursorQueue.DequeueCursor(&len)) != NULL)
            {
                DbgPrint(TRACE_LEVEL_VERBOSE,
                         ("---> %s m_CursorQueue pvbuf = %p len = %u\n", __FUNCTION__, pvbuf, len));
                if (pvbuf->complete_cb != NULL &&
                    InterlockedExchange(&pvbuf->complete_fired, 1) == 0)
                    pvbuf->complete_cb(pvbuf->complete_ctx, pvbuf->buf, NULL);
                m_CursorQueue.ReleaseQueueBuffer(pvbuf);
                m_CursorQueue.ReleaseBuffer(pvbuf);
            };
        }
        if (reason & ISR_REASON_CHANGE)
        {
            DbgPrint(TRACE_LEVEL_FATAL, ("---> %s ConfigChanged\n", __FUNCTION__));
            KeSetEvent(&m_ConfigUpdateEvent, IO_NO_INCREMENT, FALSE);
        }
    }
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    // DxgkCbNotifyDpc commits the interrupt notifications that
    // DxgkCbNotifyInterrupt queued so the scheduler acts on them. DMA
    // completions and CRTC vsyncs are notified from the command-worker and
    // flip threads (NotifyInterrupt -> DxgkCbQueueDpc), which do not set
    // m_PendingWorks. Calling this only after draining a hardware ISR
    // reason dropped every such notification, so the scheduler never
    // observed DMA fences completing and timed the engine out. Commit
    // unconditionally: with no pending notifications it is a cheap no-op.
    m_DxgkInterface.DxgkCbNotifyDpc((HANDLE)m_DxgkInterface.DeviceHandle);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

VOID VioGpuAdapter::ResetDevice(VOID)
{
    // Bugcheck display reset may run at HIGH_LEVEL with other CPUs frozen.
    // Do not lock/walk scheduler queues or signal dispatcher events here.
    // Normal PnP/TDR teardown owns render-event cancellation.
    if (IsHardwareInit())
    {
        ctrlQueue.DisableInterrupt();
        m_CursorQueue.DisableInterrupt();
        virtio_device_reset(&m_VioDev);
    }
}

#pragma code_seg(pop) // End Non-Paged Code

PAGED_CODE_SEG_BEGIN
NTSTATUS VioGpuAdapter::WriteRegistryString(_In_ HANDLE DevInstRegKeyHandle,
                                            _In_ PCWSTR pszwValueName,
                                            _In_ PCSTR pszValue)
{
    PAGED_CODE();

    NTSTATUS Status = STATUS_SUCCESS;
    ANSI_STRING AnsiStrValue;
    UNICODE_STRING UnicodeStrValue;
    UNICODE_STRING UnicodeStrValueName;
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    RtlInitUnicodeString(&UnicodeStrValueName, pszwValueName);

    RtlInitAnsiString(&AnsiStrValue, pszValue);
    Status = RtlAnsiStringToUnicodeString(&UnicodeStrValue, &AnsiStrValue, TRUE);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("RtlAnsiStringToUnicodeString failed with Status: 0x%X\n", Status));
        return Status;
    }

    Status = ZwSetValueKey(DevInstRegKeyHandle,
                           &UnicodeStrValueName,
                           0,
                           REG_SZ,
                           UnicodeStrValue.Buffer,
                           UnicodeStrValue.MaximumLength);

    RtlFreeUnicodeString(&UnicodeStrValue);

    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("ZwSetValueKey failed with Status: 0x%X\n", Status));
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return Status;
}

NTSTATUS VioGpuAdapter::WriteRegistryDWORD(_In_ HANDLE DevInstRegKeyHandle,
                                           _In_ PCWSTR pszwValueName,
                                           _In_ PDWORD pdwValue)
{
    PAGED_CODE();

    NTSTATUS Status = STATUS_SUCCESS;
    UNICODE_STRING UnicodeStrValueName;
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    RtlInitUnicodeString(&UnicodeStrValueName, pszwValueName);

    Status = ZwSetValueKey(DevInstRegKeyHandle, &UnicodeStrValueName, 0, REG_DWORD, pdwValue, sizeof(DWORD));

    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("ZwSetValueKey failed with Status: 0x%X\n", Status));
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return Status;
}

NTSTATUS VioGpuAdapter::ReadRegistryDWORD(_In_ HANDLE DevInstRegKeyHandle,
                                          _In_ PCWSTR pszwValueName,
                                          _Inout_ PDWORD pdwValue)
{
    PAGED_CODE();

    NTSTATUS Status = STATUS_SUCCESS;
    UNICODE_STRING UnicodeStrValueName;
    ULONG ulRes;
    UCHAR Buf[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(DWORD)];
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    RtlInitUnicodeString(&UnicodeStrValueName, pszwValueName);

    Status = ZwQueryValueKey(DevInstRegKeyHandle,
                             &UnicodeStrValueName,
                             KeyValuePartialInformation,
                             Buf,
                             sizeof(Buf),
                             &ulRes);

    if (Status == STATUS_SUCCESS)
    {
        if (((PKEY_VALUE_PARTIAL_INFORMATION)Buf)->Type == REG_DWORD &&
            (((PKEY_VALUE_PARTIAL_INFORMATION)Buf)->DataLength == sizeof(DWORD)))
        {
            ASSERT(((PKEY_VALUE_PARTIAL_INFORMATION)Buf)->DataLength == sizeof(DWORD));
            *pdwValue = *((PDWORD) & (((PKEY_VALUE_PARTIAL_INFORMATION)Buf)->Data));
        }
        else
        {
            Status = STATUS_INVALID_PARAMETER;
            VioGpuDbgBreak();
        }
    }

    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("ZwQueryValueKey failed with Status: 0x%X\n", Status));
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return Status;
}

NTSTATUS VioGpuAdapter::SetRegisterInfo(_In_ ULONG Id, _In_ DWORD MemSize)
{
    PAGED_CODE();

    NTSTATUS Status = STATUS_SUCCESS;
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    PCSTR StrHWInfoChipType = "QEMU VIRTIO GPU";
    PCSTR StrHWInfoDacType = "VIRTIO GPU";
    PCSTR StrHWInfoAdapterString = "VIRTIO GPU";
    PCSTR StrHWInfoBiosString = "SEABIOS VIRTIO GPU";

    HANDLE DevInstRegKeyHandle;
    Status = IoOpenDeviceRegistryKey(m_pPhysicalDevice, PLUGPLAY_REGKEY_DRIVER, KEY_SET_VALUE, &DevInstRegKeyHandle);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("IoOpenDeviceRegistryKey failed for PDO: 0x%p, Status: 0x%X", m_pPhysicalDevice, Status));
        return Status;
    }

    do
    {
        Status = WriteRegistryString(DevInstRegKeyHandle, L"HardwareInformation.ChipType", StrHWInfoChipType);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR, ("WriteRegistryString failed for ChipType with Status: 0x%X", Status));
            break;
        }

        Status = WriteRegistryString(DevInstRegKeyHandle, L"HardwareInformation.DacType", StrHWInfoDacType);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR, ("WriteRegistryString failed DacType with Status: 0x%X", Status));
            break;
        }

        Status = WriteRegistryString(DevInstRegKeyHandle, L"HardwareInformation.AdapterString", StrHWInfoAdapterString);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR, ("WriteRegistryString failed for AdapterString with Status: 0x%X", Status));
            break;
        }

        Status = WriteRegistryString(DevInstRegKeyHandle, L"HardwareInformation.BiosString", StrHWInfoBiosString);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR, ("WriteRegistryString failed for BiosString with Status: 0x%X", Status));
            break;
        }

        DWORD MemorySize = MemSize;
        Status = WriteRegistryDWORD(DevInstRegKeyHandle, L"HardwareInformation.MemorySize", &MemorySize);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR, ("WriteRegistryDWORD failed for MemorySize with Status: 0x%X", Status));
            break;
        }

        DWORD DeviceId = Id;
        Status = WriteRegistryDWORD(DevInstRegKeyHandle, L"VioGpuAdapterID", &DeviceId);
        if (!NT_SUCCESS(Status))
        {
            DbgPrint(TRACE_LEVEL_ERROR, ("WriteRegistryDWORD failed for VioGpuAdapterID with Status: 0x%X", Status));
        }
    } while (0);

    ZwClose(DevInstRegKeyHandle);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return Status;
}

NTSTATUS VioGpuAdapter::GetRegisterInfo(void)
{
    PAGED_CODE();

    NTSTATUS Status = STATUS_SUCCESS;
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    HANDLE DevInstRegKeyHandle;
    Status = IoOpenDeviceRegistryKey(m_pPhysicalDevice, PLUGPLAY_REGKEY_DRIVER, KEY_READ, &DevInstRegKeyHandle);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("IoOpenDeviceRegistryKey failed for PDO: 0x%p, Status: 0x%X", m_pPhysicalDevice, Status));
        return Status;
    }

    DWORD value = 0;
    Status = ReadRegistryDWORD(DevInstRegKeyHandle, L"HWCursor", &value);
    if (NT_SUCCESS(Status))
    {
        SetPointerEnabled(!!value);
    }

    value = 0;
    Status = ReadRegistryDWORD(DevInstRegKeyHandle, L"FlexResolution", &value);
    if (NT_SUCCESS(Status))
    {
        SetFlexResolution(!!value);
    }

    value = 0;
    Status = ReadRegistryDWORD(DevInstRegKeyHandle, L"UsePhysicalMemory", &value);
    if (NT_SUCCESS(Status))
    {
        SetUsePhysicalMemory(!!value);
    }

    ZwClose(DevInstRegKeyHandle);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return Status;
}

NTSTATUS VioGpuAdapter::GetPCIInfo(void)
{
    PAGED_CODE();

    NTSTATUS status = STATUS_SUCCESS;
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    ULONG len;
    UINT32 pciBus;
	UINT32 pciAddr;

	status = IoGetDeviceProperty(m_pPhysicalDevice, DevicePropertyBusNumber, sizeof(pciBus), (PVOID)&pciBus, &len);
	if(!NT_SUCCESS(status)) {
		DbgPrint(TRACE_LEVEL_ERROR,
                 ("IoGetDeviceProperty failed for PDO: 0x%p, Status: 0x%X", m_pPhysicalDevice, status));
		return status;
	}

	status = IoGetDeviceProperty(m_pPhysicalDevice, DevicePropertyAddress, sizeof(pciAddr), (PVOID)&pciAddr, &len);
	if(!NT_SUCCESS(status)) {
		DbgPrint(TRACE_LEVEL_ERROR,
                 ("IoGetDeviceProperty failed for PDO: 0x%p, Status: 0x%X", m_pPhysicalDevice, status));
		return status;
	}

    m_PciBus = pciBus;
    m_PciDev = (pciAddr >> 16) & 0xFFFF;
    m_PciFunc = pciAddr & 0xFFFF;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return status;
}
PAGED_CODE_SEG_END

PAGED_CODE_SEG_BEGIN

NTSTATUS VioGpuAdapter::VioGpuAdapterInit()
{
    PAGED_CODE();
    NTSTATUS status = STATUS_SUCCESS;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    if (IsHardwareInit())
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("Already Initialized\n"));
        VioGpuDbgBreak();
        return status;
    }
    status = VirtIoDeviceInit();
    if (!NT_SUCCESS(status))
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("Failed to initialize virtio device, error %x\n", status));
        VioGpuDbgBreak();
        return status;
    }

    m_u64HostFeatures = virtio_get_features(&m_VioDev);
    DbgPrint(TRACE_LEVEL_FATAL,
             ("VISTA-HW: virtio initialized; host-features=%I64x\n", m_u64HostFeatures));
    m_u64GuestFeatures = 0;
    do
    {
        struct virtqueue *vqs[2];
        if (!AckFeature(VIRTIO_F_VERSION_1))
        {
            status = STATUS_UNSUCCESSFUL;
            break;
        }
#if (NTDDI_VERSION >= NTDDI_WIN10)
        AckFeature(VIRTIO_F_ACCESS_PLATFORM);
#endif

        // Ack the feature bits the driver implements so the host
        // actually honours the corresponding fields in ctx_init and
        // resource_uuid commands; an unset bit makes those fields
        // silently ignored.
        AckFeature(VIRTIO_GPU_F_CONTEXT_INIT);
        AckFeature(VIRTIO_GPU_F_RESOURCE_UUID);
        AckFeature(VIRTIO_GPU_F_VIRGL);
        AckFeature(VIRTIO_GPU_F_RESOURCE_BLOB);
        AckFeature(VIRTIO_GPU_F_EDID);

        status = virtio_set_features(&m_VioDev, m_u64GuestFeatures);
        if (!NT_SUCCESS(status))
        {
            DbgPrint(TRACE_LEVEL_FATAL, ("%s virtio_set_features failed with %x\n", __FUNCTION__, status));
            VioGpuDbgBreak();
            break;
        }

        DbgPrint(TRACE_LEVEL_FATAL,
                 ("VISTA-HW: negotiated guest-features=%I64x; locating queues\n",
                  m_u64GuestFeatures));

        status = virtio_find_queues(&m_VioDev, 2, vqs);
        if (!NT_SUCCESS(status))
        {
            DbgPrint(TRACE_LEVEL_FATAL, ("virtio_find_queues failed with error %x\n", status));
            VioGpuDbgBreak();
            break;
        }

        if (!ctrlQueue.Init(&m_VioDev, vqs[0], 0) || !m_CursorQueue.Init(&m_VioDev, vqs[1], 1))
        {
            DbgPrint(TRACE_LEVEL_FATAL, ("Failed to initialize virtio queues\n"));
            status = STATUS_INSUFFICIENT_RESOURCES;
            VioGpuDbgBreak();
            break;
        }

        DbgPrint(TRACE_LEVEL_FATAL,
                 ("VISTA-HW: queues ready; reading display and capset configuration\n"));

        virtio_get_config(&m_VioDev,
                          FIELD_OFFSET(GPU_CONFIG, num_scanouts),
                          &m_u32NumScanouts,
                          sizeof(m_u32NumScanouts));

        virtio_get_config(&m_VioDev, FIELD_OFFSET(GPU_CONFIG, num_capsets), &m_u32NumCapsets, sizeof(m_u32NumCapsets));
        DbgPrint(TRACE_LEVEL_FATAL,
                 ("VISTA-HW: configuration scanouts=%lu capsets=%lu\n",
                  m_u32NumScanouts,
                  m_u32NumCapsets));
    } while (0);
    if (status == STATUS_SUCCESS)
    {
        virtio_device_ready(&m_VioDev);
        SetHardwareInit(TRUE);
        DbgPrint(TRACE_LEVEL_FATAL, ("VISTA-HW: virtio device marked ready\n"));
    }
    else
    {
        virtio_add_status(&m_VioDev, VIRTIO_CONFIG_S_FAILED);
        VioGpuDbgBreak();
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    return status;
}

void VioGpuAdapter::VioGpuAdapterClose()
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_FATAL, ("---> %s\n", __FUNCTION__));

    if (IsHardwareInit())
    {
        // Stop all scanout submissions while both queues are still valid.
        // The VidPn teardown destroys its framebuffer object and can emit
        // control-queue commands.
        vidpn.Powerdown();

        // The ISR samples IsHardwareInit() and then dereferences
        // ctrlQueue / m_CursorQueue / m_VioDev. Flip the flag and
        // disable interrupts under DxgkCb-synchronisation so the
        // ISR cannot read TRUE here and then touch state that the
        // teardown below is about to invalidate.
        BOOLEAN syncRet = FALSE;
        m_DxgkInterface.DxgkCbSynchronizeExecution(
            m_DxgkInterface.DeviceHandle,
            [](PVOID p) -> BOOLEAN {
                VioGpuAdapter *self = (VioGpuAdapter *)p;
                self->SetHardwareInit(FALSE);
                self->ctrlQueue.DisableInterrupt();
                self->m_CursorQueue.DisableInterrupt();
                return TRUE;
            },
            this, 0, &syncRet);
        virtio_device_reset(&m_VioDev);
        ctrlQueue.Close();
        m_CursorQueue.Close();
        virtio_delete_queues(&m_VioDev);
        m_GpuBuf.Close();
        // Both virtqueues have stopped and all callback references are drained.
        if (m_PointerResource) resourceIdr.PutId(m_PointerResource);
        m_PointerResource = 0;
        m_CursorSegment.Close();
        m_PointerFailed = m_PointerVisible = FALSE;
        virtio_device_shutdown(&m_VioDev);
    }
    DbgPrint(TRACE_LEVEL_FATAL, ("<--- %s\n", __FUNCTION__));
}

BOOLEAN VioGpuAdapter::AckFeature(UINT64 Feature)
{
    PAGED_CODE();

    if (virtio_is_feature_enabled(m_u64HostFeatures, Feature))
    {
        virtio_feature_enable(m_u64GuestFeatures, Feature);
        return TRUE;
    }
    return FALSE;
}

NTSTATUS VioGpuAdapter::VirtIoDeviceInit()
{
    PAGED_CODE();

    return virtio_device_initialize(&m_VioDev,
                                    &VioGpuSystemOps,
                                    static_cast<IVioGpuPCI *>(this),
                                    m_PciResources.IsMSIEnabled());
}

VOID VioGpuAdapter::CreateResolutionEvent(VOID)
{
    PAGED_CODE();

    if (m_ResolutionEvent != NULL && m_ResolutionEventHandle != NULL)
    {
        return;
    }
    DECLARE_UNICODE_STRING_SIZE(DeviceNumber, 10);
    DECLARE_UNICODE_STRING_SIZE(EventName, 256);

    RtlIntegerToUnicodeString(m_Id, 10, &DeviceNumber);
    NTSTATUS status = RtlUnicodeStringPrintf(&EventName,
                                             L"%ws%ws%ws",
                                             BASE_NAMED_OBJECTS,
                                             RESOLUTION_EVENT_NAME,
                                             DeviceNumber.Buffer);
    if (!NT_SUCCESS(status))
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("RtlUnicodeStringPrintf failed 0x%x\n", status));
        return;
    }
    m_ResolutionEvent = IoCreateNotificationEvent(&EventName, &m_ResolutionEventHandle);
    if (m_ResolutionEvent == NULL)
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("<--> %s\n", __FUNCTION__));
        return;
    }
    KeClearEvent(m_ResolutionEvent);
    ObReferenceObject(m_ResolutionEvent);
}

VOID VioGpuAdapter::NotifyResolutionEvent(VOID)
{
    PAGED_CODE();

    if (m_ResolutionEvent != NULL)
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("NotifyResolutionEvent\n"));
        KeSetEvent(m_ResolutionEvent, IO_NO_INCREMENT, FALSE);
        KeClearEvent(m_ResolutionEvent);
    }
}

VOID VioGpuAdapter::CloseResolutionEvent(VOID)
{
    PAGED_CODE();

    if (m_ResolutionEventHandle != NULL)
    {
        ZwClose(m_ResolutionEventHandle);
        m_ResolutionEventHandle = NULL;
    }

    if (m_ResolutionEvent != NULL)
    {
        ObDereferenceObject(m_ResolutionEvent);
        m_ResolutionEvent = NULL;
    }
}

NTSTATUS VioGpuAdapter::HWInit(PCM_RESOURCE_LIST pResList, BOOLEAN preserveIds)
{
    PAGED_CODE();

    NTSTATUS status = STATUS_SUCCESS;
    HANDLE threadHandle = 0;
    DbgPrint(TRACE_LEVEL_INFORMATION, ("---> %s\n", __FUNCTION__));
    UINT size = 0;
    do
    {
        if (!m_PciResources.Init(GetDxgkInterface(), pResList))
        {
            DbgPrint(TRACE_LEVEL_FATAL, ("Incomplete resources\n"));
            status = STATUS_INSUFFICIENT_RESOURCES;
            VioGpuDbgBreak();
            break;
        }

        status = VioGpuAdapterInit();
        if (!NT_SUCCESS(status))
        {
            DbgPrint(TRACE_LEVEL_FATAL, ("%s Failed initialize adapter %x\n", __FUNCTION__, status));
            VioGpuDbgBreak();
            break;
        }

        DbgPrint(TRACE_LEVEL_FATAL,
                 ("VISTA-HW: allocating control buffers before capset enumeration\n"));

        size = ctrlQueue.QueryAllocation() + m_CursorQueue.QueryAllocation();
        DbgPrint(TRACE_LEVEL_FATAL, ("%s size %d\n", __FUNCTION__, size));
        ASSERT(size);

        if (!m_GpuBuf.Init(size))
        {
            DbgPrint(TRACE_LEVEL_FATAL, ("Failed to initialize buffers\n"));
            status = STATUS_INSUFFICIENT_RESOURCES;
            VioGpuDbgBreak();
            break;
        }

        ctrlQueue.SetGpuBuf(&m_GpuBuf);
        m_CursorQueue.SetGpuBuf(&m_GpuBuf);

        if (!preserveIds && !resourceIdr.Init(1))
        {
            DbgPrint(TRACE_LEVEL_FATAL, ("Failed to initialize id generator\n"));
            status = STATUS_INSUFFICIENT_RESOURCES;
            VioGpuDbgBreak();
            break;
        }

        if (!preserveIds && !ctxIdr.Init(1))
        {
            DbgPrint(TRACE_LEVEL_FATAL, ("Failed to initialize id generator\n"));
            status = STATUS_INSUFFICIENT_RESOURCES;
            VioGpuDbgBreak();
            break;
        }

        m_supportedCapsetIDs = 0;
        for (UINT32 i = 0; i < m_u32NumCapsets; i++)
        {
            PGPU_VBUFFER vbuf = NULL;

            DbgPrint(TRACE_LEVEL_FATAL,
                     ("VISTA-HW: querying capset index %lu of %lu\n", i, m_u32NumCapsets));
            if (!ctrlQueue.AskCapsetInfo(&vbuf, i) || vbuf == NULL || vbuf->resp_buf == NULL)
            {
                // The control queue completes requests through the display
                // interrupt/DPC path.  On a failed or timed-out request there
                // is no response object to inspect; dereferencing it used to
                // turn a recoverable host/interrupt failure into an AV during
                // PnP start and leave Vista's installer indefinitely pending.
                DbgPrint(TRACE_LEVEL_FATAL,
                         ("VISTA-HW: capset index %lu did not complete\n", i));
                status = STATUS_DEVICE_NOT_READY;
                if (vbuf != NULL)
                {
                    ctrlQueue.ReleaseBuffer(vbuf);
                }
                break;
            }
            PGPU_RESP_CAPSET_INFO resp = (PGPU_RESP_CAPSET_INFO)vbuf->resp_buf;

            if (!resp)
            {
                DbgPrint(TRACE_LEVEL_FATAL, ("%s Failed to get info for capset %d", __FUNCTION__, i));
                ctrlQueue.ReleaseBuffer(vbuf);
                continue;
            }

            ULONG capset_id = resp->capset_id;
            if (capset_id > 63 || capset_id <= 0)
            {
                ctrlQueue.ReleaseBuffer(vbuf);
                continue; // Invalid capset id, capsets ids are in range from 1 to 63 per specification
            }
            m_capsetInfos[capset_id].id = capset_id;
            m_capsetInfos[capset_id].max_size = resp->capset_max_size;
            m_capsetInfos[capset_id].max_version = resp->capset_max_version;
            m_supportedCapsetIDs |= 1ull << capset_id;
            DbgPrint(TRACE_LEVEL_FATAL,
                     ("CAPSET INFO %d    id: %d; version: %d; size: %d\n",
                      i,
                      capset_id,
                      resp->capset_max_size,
                      resp->capset_max_version));
            ctrlQueue.ReleaseBuffer(vbuf);
        }

    } while (0);

    // Propagate a negotiation failure from the do/while(0) block; the
    // worker thread and frame segment cannot start on a half-initialised
    // virtio device.
    if (!NT_SUCCESS(status))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("%s aborting HWInit after negotiation failure status=0x%x\n",
                  __FUNCTION__, status));
        return status;
    }

    InterlockedExchange(&m_bStopWorkThread, FALSE);
    KeClearEvent(&m_ConfigUpdateEvent);
    status = PsCreateSystemThread(&threadHandle,
                                  (ACCESS_MASK)0,
                                  NULL,
                                  (HANDLE)0,
                                  NULL,
                                  VioGpuAdapter::ThreadWork,
                                  this);

    if (!NT_SUCCESS(status))
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("%s failed to create system thread, status %x\n", __FUNCTION__, status));
        VioGpuDbgBreak();
        return status;
    }
    // ObReferenceObjectByHandle must succeed or HWClose has no way to
    // wait on / dereference the running kernel thread. On failure,
    // signal the thread to exit and fail HWInit so no orphan worker
    // outlives this call.
    status = ObReferenceObjectByHandle(threadHandle,
                                       0,
                                       NULL,
                                       KernelMode,
                                       (PVOID *)(&m_pWorkThread),
                                       NULL);
    if (!NT_SUCCESS(status))
    {
        DbgPrint(TRACE_LEVEL_FATAL,
                 ("%s ObReferenceObjectByHandle failed status=0x%x; signalling worker to exit\n",
                  __FUNCTION__, status));
        m_pWorkThread = NULL;
        InterlockedExchange(&m_bStopWorkThread, TRUE);
        KeSetEvent(&m_ConfigUpdateEvent, IO_NO_INCREMENT, FALSE);
        ZwClose(threadHandle);
        VioGpuDbgBreak();
        return status;
    }
    ZwClose(threadHandle);

    // FIXME: bar 0 is not required to be present
    PHYSICAL_ADDRESS fb_pa = m_PciResources.GetPciBar(0)->GetPA();
    // The framebuffer segment is described with 32-bit sizes; clamp instead of
    // letting the cast wrap (a >= 4GiB BAR 0 would truncate to 0 and trip the
    // ASSERT(size) in frameSegment.Init).
    ULONGLONG fb_bar_size = m_PciResources.GetPciBar(0)->GetSize();
    UINT fb_size = (fb_bar_size > MAXULONG) ? MAXULONG : (UINT)fb_bar_size;
    /*if (fb_pa.QuadPart == 0 && fb_size == 0) {
        DbgPrint(TRACE_LEVEL_WARNING, ("%s bar 0 is empty, trying 2\n", __FUNCTION__));
        fb_pa = m_PciResources.GetPciBar(2)->GetPA();
        fb_size = m_PciResources.GetPciBar(2)->GetSize();
    }*/

    DbgPrint(TRACE_LEVEL_INFORMATION, ("%s framebuffer %p +0x%x\n", __FUNCTION__, fb_pa.QuadPart, fb_size));

    // Vista's DWM hardware path evaluates the dedicated, CPU-visible
    // framebuffer segment.  An 8 MiB fallback gives an otherwise functional
    // WDDM/SM2 adapter a zero Aero eligibility score.  Keep the fallback at
    // the documented 128 MiB Aero class; normal QEMU runs provide that exact
    // BAR size through virtio-vga's vgamem_mb property.
#if defined(VIOGPU_TARGET_VISTA)
    UINT req_size = 128u * 1024u * 1024u;
#elif NTDDI_VERSION > NTDDI_WINBLUE
    UINT req_size = 0x1000000;
#else
    UINT req_size = 0x800000;
#endif

    if (!IsUsePhysicalMemory() || fb_pa.QuadPart == 0 || fb_size < req_size)
    {
        fb_pa.QuadPart = 0LL;
        fb_size = max(req_size, fb_size);
    }

    if (!frameSegment.Init(fb_size, &fb_pa))
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("%s failed to allocate FB memory segment\n", __FUNCTION__));
        status = STATUS_INSUFFICIENT_RESOURCES;
        VioGpuDbgBreak();
        return status;
    }

    return status;
}

NTSTATUS VioGpuAdapter::HWClose(void)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_INFORMATION, ("---> %s\n", __FUNCTION__));
    SetHardwareInit(FALSE);

    InterlockedExchange(&m_bStopWorkThread, TRUE);
    KeSetEvent(&m_ConfigUpdateEvent, IO_NO_INCREMENT, FALSE);

    // HWInit can fail before it creates or references the configuration
    // worker.  Dxgkrnl still calls RemoveDevice for that partially-created
    // adapter, so teardown must not pass a null object to the dispatcher or
    // ObDereferenceObject.  This is also needed after a completed HWClose,
    // where the stored reference has already been released.
    if (m_pWorkThread != NULL)
    {
        // The configuration worker waits only on m_ConfigUpdateEvent.  It was
        // woken above, so wait until it is really gone before closing shared
        // frame/queue state and releasing its object reference.
        KeWaitForSingleObject(m_pWorkThread, Executive, KernelMode, FALSE, NULL);

        ObDereferenceObject(m_pWorkThread);
        m_pWorkThread = NULL;
    }

    frameSegment.Close();

    DbgPrint(TRACE_LEVEL_INFORMATION, ("<--- %s\n", __FUNCTION__));

    return STATUS_SUCCESS;
}

#if !defined(VIOGPU_TARGET_VISTA)
BOOLEAN FindUpdateRect(_In_ ULONG NumMoves,
                       _In_ D3DKMT_MOVE_RECT *pMoves,
                       _In_ ULONG NumDirtyRects,
                       _In_ PRECT pDirtyRect,
                       _In_ D3DKMDT_VIDPN_PRESENT_PATH_ROTATION Rotation,
                       _Out_ PRECT pUpdateRect)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(Rotation);
    BOOLEAN updated = FALSE;

    if (pUpdateRect == NULL)
    {
        return FALSE;
    }

    if (NumMoves == 0 && NumDirtyRects == 0)
    {
        pUpdateRect->bottom = 0;
        pUpdateRect->left = 0;
        pUpdateRect->right = 0;
        pUpdateRect->top = 0;
    }

    for (ULONG i = 0; i < NumMoves; i++)
    {
        PRECT pRect = &pMoves[i].DestRect;
        if (!updated)
        {
            *pUpdateRect = *pRect;
            updated = TRUE;
        }
        else
        {
            pUpdateRect->bottom = max(pRect->bottom, pUpdateRect->bottom);
            pUpdateRect->left = min(pRect->left, pUpdateRect->left);
            pUpdateRect->right = max(pRect->right, pUpdateRect->right);
            pUpdateRect->top = min(pRect->top, pUpdateRect->top);
        }
    }
    for (ULONG i = 0; i < NumDirtyRects; i++)
    {
        PRECT pRect = &pDirtyRect[i];
        if (!updated)
        {
            *pUpdateRect = *pRect;
            updated = TRUE;
        }
        else
        {
            pUpdateRect->bottom = max(pRect->bottom, pUpdateRect->bottom);
            pUpdateRect->left = min(pRect->left, pUpdateRect->left);
            pUpdateRect->right = max(pRect->right, pUpdateRect->right);
            pUpdateRect->top = min(pRect->top, pUpdateRect->top);
        }
    }
    if (Rotation == D3DKMDT_VPPR_ROTATE90 || Rotation == D3DKMDT_VPPR_ROTATE270)
    {
    }
    return updated;
}
#endif

NTSTATUS VioGpuAdapter::UpdateChildStatus(BOOLEAN connect)
{
    PAGED_CODE();
    NTSTATUS Status(STATUS_SUCCESS);
    DXGK_CHILD_STATUS ChildStatus;
    PDXGKRNL_INTERFACE pDXGKInterface(GetDxgkInterface());

    // Dedupe against the cached state: DXGK only needs to see actual
    // transitions, and the cache also gates the hotplug-disconnect
    // direction.
    if (!!m_scanoutConnected[0] == !!connect)
    {
        return STATUS_SUCCESS;
    }
    m_scanoutConnected[0] = connect ? TRUE : FALSE;

    RtlZeroMemory(&ChildStatus, sizeof(ChildStatus));

    ChildStatus.Type = StatusConnection;
    ChildStatus.ChildUid = 0;
    ChildStatus.HotPlug.Connected = connect;
    Status = pDXGKInterface->DxgkCbIndicateChildStatus(pDXGKInterface->DeviceHandle, &ChildStatus);
    if (Status != STATUS_SUCCESS)
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("<--- %s DxgkCbIndicateChildStatus failed with status %x\n ", __FUNCTION__, Status));
    }
    return Status;
}

PAGED_CODE_SEG_END

BOOLEAN VioGpuAdapter::InterruptRoutine(_In_ ULONG MessageNumber)
{
    if (!IsHardwareInit())
    {
        return FALSE;
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s MessageNumber = %d\n", __FUNCTION__, MessageNumber));
    BOOLEAN serviced = TRUE;
    ULONG intReason = 0;
    // return FALSE;
    if (m_PciResources.IsMSIEnabled())
    {
        switch (MessageNumber)
        {
            case 0:
                intReason = ISR_REASON_CHANGE;
                break;
            case 1:
                intReason = ISR_REASON_DISPLAY;
                break;
            case 2:
                intReason = ISR_REASON_CURSOR;
                break;
            default:
                serviced = FALSE;
                DbgPrint(TRACE_LEVEL_FATAL,
                         ("---> %s Unknown Interrupt Reason MessageNumber%d\n", __FUNCTION__, MessageNumber));
        }
    }
    else
    {
        UNREFERENCED_PARAMETER(MessageNumber);
        UCHAR isrstat = virtio_read_isr_status(&m_VioDev);

        // Per virtio 1.x: bit 0 = queue notification, bit 1 = config change.
        // Either or both may be set; missing the bitmask decode silently
        // dropped queue completions when config-change rode the same INTx.
        if (isrstat & 0x01)
        {
            intReason |= (ISR_REASON_DISPLAY | ISR_REASON_CURSOR);
        }
        if (isrstat & VIRTIO_PCI_ISR_CONFIG)
        {
            intReason |= ISR_REASON_CHANGE;
        }
        if (intReason == 0)
        {
            serviced = FALSE;
        }
    }

    if (serviced)
    {
        InterlockedOr((PLONG)&m_PendingWorks, intReason);
        m_DxgkInterface.DxgkCbQueueDpc(m_DxgkInterface.DeviceHandle);
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    return serviced;
}

void VioGpuAdapter::ThreadWork(_In_ PVOID Context)
{
    VioGpuAdapter *pdev = reinterpret_cast<VioGpuAdapter *>(Context);
    pdev->ThreadWorkRoutine();
}

void VioGpuAdapter::ThreadWorkRoutine(void)
{
    KeSetPriorityThread(KeGetCurrentThread(), LOW_REALTIME_PRIORITY);

    for (;;)
    {
        KeWaitForSingleObject(&m_ConfigUpdateEvent, Executive, KernelMode, FALSE, NULL);

        if (InterlockedCompareExchange(&m_bStopWorkThread, FALSE, FALSE) != FALSE)
        {
            PsTerminateSystemThread(STATUS_SUCCESS);
            break;
        }

        ConfigChanged();
        NotifyResolutionEvent();
    }
}

void VioGpuAdapter::ConfigChanged(void)
{
    DbgPrint(TRACE_LEVEL_FATAL, ("<--> %s\n", __FUNCTION__));
    UINT32 events_read, events_clear = 0;
    virtio_get_config(&m_VioDev, FIELD_OFFSET(GPU_CONFIG, events_read), &events_read, sizeof(events_read));
    if (events_read & VIRTIO_GPU_EVENT_DISPLAY)
    {
        vidpn.GetDisplayInfo();
        events_clear |= VIRTIO_GPU_EVENT_DISPLAY;
        virtio_set_config(&m_VioDev, FIELD_OFFSET(GPU_CONFIG, events_clear), &events_clear, sizeof(events_clear));

        // Probe per-scanout enable state and emit child-status
        // transitions in both directions. With MAX_CHILDREN==1 this
        // only walks scanout 0, but the pattern survives a future
        // multi-monitor refactor.
        PGPU_VBUFFER vbuf = NULL;
        if (ctrlQueue.AskDisplayInfo(&vbuf))
        {
            for (UINT i = 0; i < MAX_CHILDREN && i < m_u32NumScanouts; i++)
            {
                ULONG xres = 0, yres = 0;
                BOOLEAN connected = ctrlQueue.GetDisplayInfo(vbuf, i, &xres, &yres);
                UpdateChildStatus(connected);
            }
            ctrlQueue.ReleaseBuffer(vbuf);
        }
        else
        {
            // Fall back to the previous always-connect behaviour if
            // the host did not give us info.
            UpdateChildStatus(TRUE);
        }
    }
}

VioGpuAllocation *VioGpuAdapter::AllocationFromHandle(D3DKMT_HANDLE handle)
{
    DXGKARGCB_GETHANDLEDATA getHandleData;
    getHandleData.hObject = handle;
    getHandleData.Type = DXGK_HANDLE_ALLOCATION;
    getHandleData.Flags.DeviceSpecific = 0;
    return VioGpuAllocation::FromHandle(m_DxgkInterface.DxgkCbGetHandleData(&getHandleData));
}

VioGpuResource *VioGpuAdapter::ResourceFromHandle(D3DKMT_HANDLE handle)
{
    DXGKARGCB_GETHANDLEDATA getHandleData;
    getHandleData.hObject = handle;
    getHandleData.Type = DXGK_HANDLE_RESOURCE;
    getHandleData.Flags.DeviceSpecific = 0;
    return VioGpuResource::FromHandle(m_DxgkInterface.DxgkCbGetHandleData(&getHandleData));
}
