#include "viogpu_command.h"
#include "viogpu_device.h"
#include "viogpu_adapter.h"
#include "baseobj.h"

#pragma code_seg(push)
#pragma code_seg()

VioGpuCommand::VioGpuCommand(VioGpuAdapter *adapter, VioGpuDevice *device)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    m_pAdapter = adapter;
    m_pCommander = &adapter->commander;
    m_pDevice = NULL;
    m_pExpectedDevice = device;

    m_FenceId = 0;
    m_NodeOrdinal = 0;
    m_EngineOrdinal = 0;
    m_NullRendering = FALSE;
    m_Preemption = FALSE;
    m_pendingCallbacks = 0;
    m_notified = 0;
    m_failureStatus = STATUS_SUCCESS;
    m_pInitialPresentCompletion = NULL;
    m_pScanoutSourceCompletion = NULL;
    m_scanoutSourceGeneration = 0;
    m_scanoutSourceIsDmaFlip = FALSE;
    m_pRenderEvent = NULL;
    m_pDmaBuffer = NULL;
    m_pCommand = NULL;
    m_pEnd = NULL;

    m_allocations = NULL;
    m_allocationsLength = 0;

    list_entry.Blink = NULL;
    list_entry.Flink = NULL;
};

VioGpuCommand::~VioGpuCommand()
{
    // A failed/cancelled submission can be deleted without reaching the
    // terminal packet in Run().  Always wake the client rather than retain a
    // user event reference or leave its finite wait to expire.
    SignalRenderEvent();

    // Tripping this means a cmd was freed while a queue completion
    // callback was still going to dereference `this`. In the current
    // code the only delete path is Run() -> `end:`, reached only when
    // the body is fully drained and the last submit's callback has
    // already fired and re-queued the cmd onto the running list --
    // so the count must be zero. A future caller that frees the cmd
    // from a different path (an error tearing down a partially-
    // submitted command) would need to wait for outstanding callbacks
    // first.
    LONG pending = m_pendingCallbacks;
    if (pending != 0)
    {
        DbgPrint(TRACE_LEVEL_FATAL,
                 ("%s cmd=%p destroyed with %d outstanding callbacks\n",
                  __FUNCTION__, this, pending));
        ASSERT(pending == 0);
    }

    ReleaseAllocations();
}

void VioGpuCommand::ReleaseAllocations()
{
    VioGpuAllocation **allocations = m_allocations;
    UINT allocationCount = m_allocationsLength;
    m_allocations = NULL;
    m_allocationsLength = 0;

    if (allocations != NULL)
    {
        for (UINT i = 0; i < allocationCount; ++i)
        {
            if (allocations[i] != NULL)
            {
                allocations[i]->UnmarkBusy();
            }
        }
        delete[] allocations;
    }
}

void VioGpuCommand::SetRenderEvent(_In_ PKEVENT event)
{
    ASSERT(event != NULL);
    PVOID old = InterlockedCompareExchangePointer((PVOID volatile *)&m_pRenderEvent,
                                                   event, NULL);
    // Render validates one terminal event per DMA stream.  Keep this assert
    // so a future parser change cannot overwrite and leak a reference.
    ASSERT(old == NULL);
    if (old != NULL)
    {
        KeSetEvent(event, IO_NO_INCREMENT, FALSE);
        ObDereferenceObject(event);
    }
}

void VioGpuCommand::SignalRenderEvent()
{
    PKEVENT event = (PKEVENT)InterlockedExchangePointer((PVOID volatile *)&m_pRenderEvent,
                                                         NULL);
    if (event != NULL)
    {
        KeSetEvent(event, IO_NO_INCREMENT, FALSE);
        ObDereferenceObject(event);
    }
}

void VioGpuCommand::SetInitialPresentCompletion(VioGpuAllocation *allocation)
{
    ASSERT(allocation != NULL);
    m_pInitialPresentCompletion = allocation;
}

void VioGpuCommand::SetScanoutSourceCompletion(VioGpuAllocation *allocation,
                                               LONG sourceGeneration,
                                               BOOLEAN dmaFlip)
{
    ASSERT(allocation != NULL);
    m_pScanoutSourceCompletion = allocation;
    m_scanoutSourceGeneration = sourceGeneration;
    m_scanoutSourceIsDmaFlip = dmaFlip;
}

void VioGpuCommand::RecordFailure(NTSTATUS status)
{
    if (NT_SUCCESS(status))
    {
        status = STATUS_UNSUCCESSFUL;
    }
    InterlockedCompareExchange(&m_failureStatus, status, STATUS_SUCCESS);
}

void VioGpuCommand::RecordIssueFailure(NTSTATUS status)
{
    if (NT_SUCCESS(status))
    {
        return;
    }

    // QueueBuffer reports a local issue failure through a synchronous callback
    // with no response. That callback records the generic device error before
    // the queue helper can return its exact allocation or enqueue status.
    InterlockedCompareExchange(&m_failureStatus, status,
                               STATUS_IO_DEVICE_ERROR);
    RecordFailure(status);
}

void VioGpuCommand::AddPending()
{
    InterlockedIncrement(&m_pendingCallbacks);
}

LONG VioGpuCommand::DropPending()
{
    LONG remaining = InterlockedDecrement(&m_pendingCallbacks);
    if (remaining < 0)
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("%s cmd=%p pending underflow %d\n",
                  __FUNCTION__, this, remaining));
        InterlockedExchange(&m_pendingCallbacks, 0);
        return 0;
    }
    return remaining;
}

void VioGpuCommand::PrepareSubmit(const DXGKARG_SUBMITCOMMAND *pSubmitCommand,
                                  VioGpuDevice *device)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s", __FUNCTION__));

    m_FenceId = pSubmitCommand->SubmissionFenceId;
#if defined(VIOGPU_TARGET_VISTA)
    // WDDM 1.0 has one node.  Its submit argument reports only the engine.
    m_NodeOrdinal = 0;
#else
    m_NodeOrdinal = pSubmitCommand->NodeOrdinal;
#endif
    m_EngineOrdinal = pSubmitCommand->EngineOrdinal;
    if (m_pDmaBuffer)
    {
        m_pCommand = (char *)m_pDmaBuffer + pSubmitCommand->DmaBufferSubmissionStartOffset;
        m_pEnd = (char *)m_pDmaBuffer + pSubmitCommand->DmaBufferSubmissionEndOffset;
    }
    m_pDevice = device;

    // Capture the only submit flag we react to. Paging / ContextSwitch /
    // Flip can legitimately arrive with an empty DMA range; Run() falls
    // through to the fence-completion arm in that case, so they need
    // no special handling. NullRendering does need to short-circuit so
    // the runtime's submission-overhead profiling does not actually
    // execute the body.
    m_NullRendering = pSubmitCommand->Flags.NullRendering ? TRUE : FALSE;
}

#pragma code_seg(pop)

PAGED_CODE_SEG_BEGIN

void VioGpuCommand::Run()
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    if (m_NullRendering)
    {
        // The runtime asked us to simulate insertion of the DMA buffer
        // without executing its body. Skip straight to the fence
        // completion at `end:` so the submission is timed without the
        // host running anything.
        DbgPrint(TRACE_LEVEL_VERBOSE,
                 ("<---> %s fence_id=%d NullRendering: skipping body\n",
                  __FUNCTION__, m_FenceId));
        goto end;
    }
    if (!NT_SUCCESS((NTSTATUS)InterlockedCompareExchange(&m_failureStatus,
                                                         STATUS_SUCCESS,
                                                         STATUS_SUCCESS)))
    {
        goto end;
    }

    while (m_pCommand < m_pEnd)
    {
        SIZE_T remaining = (SIZE_T)(m_pEnd - m_pCommand);
        if (remaining < sizeof(VIOGPU_COMMAND_HDR))
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("%s fence_id=%d truncated command header (%Iu bytes remain)\n",
                      __FUNCTION__, m_FenceId, remaining));
            RecordFailure(STATUS_INVALID_BUFFER_SIZE);
            goto end;
        }
        VIOGPU_COMMAND_HDR *cmdHdr = (VIOGPU_COMMAND_HDR *)m_pCommand;
        if ((SIZE_T)cmdHdr->size > remaining - sizeof(VIOGPU_COMMAND_HDR))
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("%s fence_id=%d command %u body %u exceeds remaining DMA bytes\n",
                      __FUNCTION__, m_FenceId, cmdHdr->type, cmdHdr->size));
            RecordFailure(STATUS_INVALID_BUFFER_SIZE);
            goto end;
        }

        void *cmdBody = m_pCommand + sizeof(VIOGPU_COMMAND_HDR);
        m_pCommand += sizeof(VIOGPU_COMMAND_HDR) + cmdHdr->size;

        DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s fence_id=%d running command=%d\n", __FUNCTION__, m_FenceId, cmdHdr->type));

        switch (cmdHdr->type)
        {
            case VIOGPU_CMD_SUBMIT:
                {
                    PBYTE submitCmd = new (VIOGPU_NONPAGED_POOL) BYTE[cmdHdr->size];
                    if (!submitCmd)
                    {
                        DbgPrint(TRACE_LEVEL_ERROR,
                                 ("%s fence_id=%d OOM allocating submit buffer (size=%u); skipping command\n",
                                  __FUNCTION__,
                                  m_FenceId,
                                  cmdHdr->size));
                        RecordFailure(STATUS_INSUFFICIENT_RESOURCES);
                        goto end;
                    }
                    RtlCopyMemory(submitCmd, cmdBody, cmdHdr->size);

                    AddPending();
                    NTSTATUS issueStatus =
                        m_pAdapter->ctrlQueue.SubmitCommand(
                            submitCmd,
                            cmdHdr->size,
                            (cmdHdr->flags & VIOGPU_EXECBUF_VIRGL) != 0
                                ? m_pDevice->m_Virgl.GetId()
                                : m_pDevice->m_Context.GetId(),
                            (cmdHdr->flags & VIOGPU_EXECBUF_RING_IDX) != 0,
                            cmdHdr->ring_idx,
                            VioGpuCommand::QueueRunningCb,
                            this);
                    RecordIssueFailure(issueStatus);
                    return;
                }

            case VIOGPU_CMD_TRANSFER_TO_HOST:
            case VIOGPU_CMD_TRANSFER_FROM_HOST:
                {
                    if (cmdHdr->size != sizeof(VIOGPU_TRANSFER_CMD))
                    {
                        DbgPrint(TRACE_LEVEL_ERROR,
                                 ("%s fence_id=%d invalid transfer size=%u\n",
                                  __FUNCTION__, m_FenceId, cmdHdr->size));
                        RecordFailure(STATUS_INVALID_BUFFER_SIZE);
                        goto end;
                    }
                    VIOGPU_TRANSFER_CMD *transferCmd = (VIOGPU_TRANSFER_CMD *)cmdBody;

                    AddPending();
                    NTSTATUS issueStatus =
                        m_pAdapter->ctrlQueue.TransferHostCmd(
                            cmdHdr->type == VIOGPU_CMD_TRANSFER_TO_HOST,
                            (cmdHdr->flags & VIOGPU_EXECBUF_VIRGL) != 0
                                ? m_pDevice->m_Virgl.GetId()
                                : m_pDevice->m_Context.GetId(),
                            false,
                            0,
                            transferCmd,
                            VioGpuCommand::QueueRunningCb,
                            this);
                    RecordIssueFailure(issueStatus);
                    return;
                }

            case VIOGPU_CMD_MAP_BLOB:
            case VIOGPU_CMD_UNMAP_BLOB:
                {
                    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s fence_id=%d running map/unmap blob, curr=%p, end=%p\n", __FUNCTION__, m_FenceId, m_pCommand, m_pEnd));

                    if ((cmdHdr->size == 0) || ((cmdHdr->size % sizeof(ULONG)) != 0))
                    {
                        DbgPrint(TRACE_LEVEL_ERROR,
                                 ("%s fence_id=%d invalid map/unmap size=%u\n",
                                  __FUNCTION__, m_FenceId, cmdHdr->size));
                        RecordFailure(STATUS_INVALID_BUFFER_SIZE);
                        goto end;
                    }

                    const BOOLEAN isMap = (cmdHdr->type == VIOGPU_CMD_MAP_BLOB);
                    ULONG *map_idx = (ULONG *)cmdBody;

                    size_t num_maps = cmdHdr->size / sizeof(ULONG);

                    // Validate every index -- bounds, non-NULL, blob, and
                    // mappability -- before issuing anything. Doing the
                    // mappability check here (rather than in the issue loop)
                    // keeps the issue loop free of any early exit that could
                    // leave host ops in flight while we jump to `end:`.
                    BOOLEAN valid = TRUE;
                    for (size_t i = 0; i < num_maps; i++) {
                        if (map_idx[i] >= m_allocationsLength)
                        {
                            DbgPrint(TRACE_LEVEL_ERROR, ("<---> %s fence_id=%d map/unmap blob %d: invalid index=%u\n", __FUNCTION__, m_FenceId, i, map_idx[i]));
                            valid = FALSE; break;
                        }
                        VioGpuAllocation *a = m_allocations[map_idx[i]];
                        if (a == NULL)
                        {
                            DbgPrint(TRACE_LEVEL_ERROR, ("<---> %s fence_id=%d map/unmap blob %d: allocation %d is NULL\n", __FUNCTION__, m_FenceId, i, map_idx[i]));
                            valid = FALSE; break;
                        }
                        if (!a->IsBlob())
                        {
                            DbgPrint(TRACE_LEVEL_ERROR, ("<---> %s fence_id=%d map/unmap blob %d: allocation %d is not blob\n", __FUNCTION__, m_FenceId, i, map_idx[i]));
                            valid = FALSE; break;
                        }
                        if (!a->IsMappable())
                        {
                            DbgPrint(TRACE_LEVEL_ERROR, ("<---> %s fence_id=%d res_id=%d cannot map unmappable blob (flags=%d)\n", __FUNCTION__, m_FenceId, a->GetId(), a->m_Blob.Options.blob_flags));
                            valid = FALSE; break;
                        }
                    }
                    if (!valid)
                    {
                        RecordFailure(STATUS_INVALID_PARAMETER);
                        goto end;
                    }

                    // Issue each map/unmap that actually changes state, every
                    // one carrying QueueRunningCb. A loop guard holds one
                    // pending ref so a host response arriving mid-issue cannot
                    // re-queue this command before the whole batch is posted.
                    // Whoever drops the count to zero -- the guard release below
                    // when nothing is in flight, otherwise the final host
                    // completion -- re-enters Run() exactly once to reach `end:`.
                    // The guard guarantees the DMA fence is always completed,
                    // including the case where every blob is already in the
                    // requested state and no host command is issued at all.
                    AddPending();
                    for (size_t i = 0; i < num_maps; i++) {
                        VioGpuAllocation *allocation = m_allocations[map_idx[i]];
                        UINT ctxId = m_pDevice->m_Context.GetId();
                        AddPending();
                        BOOLEAN issued = FALSE;
                        NTSTATUS operationStatus = isMap
                            ? allocation->MapBlob(ctxId,
                                                  VioGpuCommand::QueueRunningCb,
                                                  this,
                                                  &issued)
                            : allocation->UnmapBlob(ctxId,
                                                    VioGpuCommand::QueueRunningCb,
                                                    this,
                                                    &issued);
                        DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s fence_id=%d %s blob res_id=%d issued=%d\n", __FUNCTION__, m_FenceId, isMap ? "map" : "unmap", allocation->GetId(), issued));
                        if (!NT_SUCCESS(operationStatus))
                        {
                            RecordIssueFailure(operationStatus);
                        }
                        if (!issued)
                        {
                            // Already in the requested state: no host round-trip
                            // and therefore no callback -- undo the tentative ref.
                            DropPending();
                        }
                    }
                    if (DropPending() == 0)
                    {
                        // Nothing was actually issued to the host (all blobs
                        // already in the requested state). No callback will
                        // re-queue us, so fall through to complete the fence.
                        break;
                    }
                    // At least one host map/unmap is in flight; the completion
                    // that drops the count to zero re-queues us to reach `end:`.
                    return;
                }

            case VIOGPU_CMD_SIGNAL_EVENT:
                {
                    if (cmdHdr->size != sizeof(VIOGPU_SIGNAL_EVENT_CMD))
                    {
                        RecordFailure(STATUS_INVALID_BUFFER_SIZE);
                        goto end;
                    }
                    // Render accepted this only as the terminal packet and
                    // stored a referenced PKEVENT on this command.  We reach
                    // it only after all preceding host work has returned to
                    // the serialized commander, so it is an ordered GPU-done
                    // completion point rather than a CPU submission ack.
                    SignalRenderEvent();
                    break;
                }

            case VIOGPU_CMD_COPY_FIXED_PRIMARY:
                {
                    if (cmdHdr->size < sizeof(VIOGPU_COPY_FIXED_PRIMARY_CMD))
                    {
                        DbgPrint(TRACE_LEVEL_ERROR,
                                 ("%s fence_id=%d short fixed-primary copy size=%u\n",
                                  __FUNCTION__, m_FenceId, cmdHdr->size));
                        RecordFailure(STATUS_INVALID_BUFFER_SIZE);
                        goto end;
                    }

                    VIOGPU_COPY_FIXED_PRIMARY_CMD *copy =
                        (VIOGPU_COPY_FIXED_PRIMARY_CMD *)cmdBody;
                    const SIZE_T rectBytes =
                        (SIZE_T)copy->RectCount * sizeof(RECT);
                    if (copy->RectCount == 0 ||
                        rectBytes / sizeof(RECT) != copy->RectCount ||
                        rectBytes > MAXULONG - sizeof(*copy) ||
                        cmdHdr->size != sizeof(*copy) + rectBytes ||
                        copy->SourceAllocationIndex >= m_allocationsLength ||
                        copy->DestinationAllocationIndex >= m_allocationsLength)
                    {
                        DbgPrint(TRACE_LEVEL_ERROR,
                                 ("%s fence_id=%d invalid fixed-primary copy packet\n",
                                  __FUNCTION__, m_FenceId));
                        RecordFailure(STATUS_INVALID_PARAMETER);
                        goto end;
                    }

                    VioGpuAllocation *source =
                        m_allocations[copy->SourceAllocationIndex];
                    VioGpuAllocation *destination =
                        m_allocations[copy->DestinationAllocationIndex];
                    if (source == NULL || destination == NULL)
                    {
                        RecordFailure(STATUS_INVALID_HANDLE);
                        goto end;
                    }

                    const RECT *rects = (const RECT *)(copy + 1);
                    NTSTATUS copyStatus = destination->CopyToFixedPrimary(
                        source, rects, copy->RectCount,
                        copy->SourceDeltaX, copy->SourceDeltaY);
                    if (!NT_SUCCESS(copyStatus))
                    {
                        DbgPrint(TRACE_LEVEL_ERROR,
                                 ("%s fixed-primary CPU blt failed src=%d dst=%d status=0x%X\n",
                                  __FUNCTION__, source->GetId(),
                                  destination->GetId(), copyStatus));
                        RecordFailure(copyStatus);
                        goto end;
                    }
                    DbgPrint(TRACE_LEVEL_INFORMATION,
                             ("%s fixed-primary CPU blt src=%d dst=%d rects=%u\n",
                              __FUNCTION__, source->GetId(),
                              destination->GetId(), copy->RectCount));
                    break;
                }

            case VIOGPU_CMD_FLUSH_FIXED_PRIMARY:
                {
                    if (cmdHdr->size !=
                            sizeof(VIOGPU_FLUSH_FIXED_PRIMARY_CMD) ||
                        cmdHdr->flags != 0 || cmdHdr->ring_idx != 0)
                    {
                        DbgPrint(TRACE_LEVEL_ERROR,
                                 ("%s fence_id=%d invalid fixed-primary flush packet\n",
                                  __FUNCTION__, m_FenceId));
                        RecordFailure(STATUS_INVALID_PARAMETER);
                        goto end;
                    }

                    VIOGPU_FLUSH_FIXED_PRIMARY_CMD *flush =
                        (VIOGPU_FLUSH_FIXED_PRIMARY_CMD *)cmdBody;
                    if (flush->DestinationAllocationIndex >=
                        m_allocationsLength)
                    {
                        RecordFailure(STATUS_INVALID_HANDLE);
                        goto end;
                    }
                    VioGpuAllocation *destination =
                        m_allocations[flush->DestinationAllocationIndex];
                    if (destination == NULL || !destination->IsPrimary() ||
                        destination->IsBlob())
                    {
                        RecordFailure(STATUS_INVALID_HANDLE);
                        goto end;
                    }

                    // The transfer is complete, so wake the display thread if
                    // this allocation is still the selected source. Never let
                    // a present destination bypass VidPN source ownership.
                    m_pAdapter->vidpn.RearmFlipIfScanout(destination);
                    break;
                }

            case VIOGPU_CMD_NOP:
                if (cmdHdr->size != 0)
                {
                    RecordFailure(STATUS_INVALID_BUFFER_SIZE);
                    goto end;
                }
                break;
            default:
                {
                    DbgPrint(TRACE_LEVEL_ERROR,
                             ("%s fence_id=%d unknown command type=%u\n",
                              __FUNCTION__, m_FenceId, cmdHdr->type));
                    RecordFailure(STATUS_NOT_SUPPORTED);
                    goto end;
                }
        }
    }

end:
#if defined(VIOGPU_TARGET_VISTA)
    // Wake the display thread only after the copy and transfer commands have
    // completed successfully. RearmFlipIfScanout also verifies that the
    // destination still owns the scanout.
    if (NT_SUCCESS((NTSTATUS)InterlockedCompareExchange(&m_failureStatus, 0, 0)) &&
        m_allocations != NULL &&
        m_allocationsLength > DXGK_PRESENT_DESTINATION_INDEX)
    {
        VioGpuAllocation *destination =
            m_allocations[DXGK_PRESENT_DESTINATION_INDEX];
        if (destination != NULL && destination->IsPrimary() &&
            !destination->IsBlob())
        {
            m_pAdapter->vidpn.RearmFlipIfScanout(destination);
        }
    }
#endif
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s finished fence_id=%d, this=%p, m_pAdapter=%p\n", __FUNCTION__, m_FenceId, this, m_pAdapter));

    if (NT_SUCCESS((NTSTATUS)InterlockedCompareExchange(&m_failureStatus, 0, 0)) &&
        m_pInitialPresentCompletion != NULL)
    {
        m_pInitialPresentCompletion->CompleteInitialPresent();
    }
    m_pInitialPresentCompletion = NULL;

    if (NT_SUCCESS((NTSTATUS)InterlockedCompareExchange(&m_failureStatus, 0, 0)) &&
        m_pScanoutSourceCompletion != NULL)
    {
        // Read the segment address at retirement, after any Patch callback.
        // Host blt presentation preserves the current address; DMA flips
        // report their own allocation's address on the following vsync.
        m_pAdapter->vidpn.SetScanoutSourceIfGeneration(
            m_pScanoutSourceCompletion, m_scanoutSourceGeneration,
            m_scanoutSourceIsDmaFlip);
    }
    m_pScanoutSourceCompletion = NULL;

    ReleaseAllocations();

    // Report completion only after the worker has performed all ordered local
    // work and released the in-flight allocation references.
    NotifyCompletion();

    m_pCommander->CommandFinished(this);

    delete this;
}

NTSTATUS VioGpuCommand::AttachAllocations(DXGK_ALLOCATIONLIST *allocationList,
                                          UINT allocationListLength,
                                          VioGpuDevice *expectedDevice)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    if (expectedDevice == NULL || expectedDevice != m_pExpectedDevice)
    {
        return STATUS_INVALID_PARAMETER;
    }

    // A command stream is allowed to contain no allocation references.  The
    // Vista render-event packet deliberately uses that form, so do not depend
    // on implementation-defined new[0] behaviour here.
    if (allocationListLength == 0)
    {
        m_allocations = NULL;
        m_allocationsLength = 0;
        return STATUS_SUCCESS;
    }

    if (allocationList == NULL ||
        (SIZE_T)allocationListLength > ((SIZE_T)-1 / sizeof(*m_allocations)))
    {
        return STATUS_INVALID_PARAMETER;
    }

    m_allocations = new (VIOGPU_NONPAGED_POOL) VioGpuAllocation *[allocationListLength];
    if (!m_allocations)
    {
        m_allocationsLength = 0;
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    m_allocationsLength = allocationListLength;
    RtlZeroMemory(m_allocations, sizeof(*m_allocations) * allocationListLength);
    for (UINT i = 0; i < allocationListLength; i++)
    {
        HANDLE handle = allocationList[i].hDeviceSpecificAllocation;
        VioGpuDeviceAllocation *deviceAllocation =
            VioGpuDeviceAllocation::FromHandle(handle);
        if (handle != NULL &&
            (deviceAllocation == NULL ||
             deviceAllocation->GetDevice() != expectedDevice ||
             deviceAllocation->GetAllocation() == NULL))
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("%s rejected allocation slot %u handle=%p\n",
                      __FUNCTION__, i, handle));
            ReleaseAllocations();
            return STATUS_INVALID_HANDLE;
        }
        if (deviceAllocation != NULL)
        {
            m_allocations[i] = deviceAllocation->GetAllocation();
            m_allocations[i]->MarkBusy();
        }
        else
        {
            m_allocations[i] = NULL;
        }
    }
    return STATUS_SUCCESS;
}

PAGED_CODE_SEG_END

#pragma code_seg(push)
#pragma code_seg()

void VioGpuCommand::QueueRunning()
{
    m_pCommander->QueueRunning(this);
}

void VioGpuCommand::SetPreemption(const DXGKARG_PREEMPTCOMMAND *request)
{
    m_Preemption = TRUE;
    m_FenceId = request->PreemptionFenceId;
    m_NodeOrdinal = request->NodeOrdinal;
    m_EngineOrdinal = request->EngineOrdinal;
}

void VioGpuCommand::NotifyCompletion()
{
    // Report the DMA result at most once.
    if (InterlockedExchange(&m_notified, 1) != 0)
    {
        return;
    }

    DXGKARGCB_NOTIFY_INTERRUPT_DATA interrupt = {};
    NTSTATUS failure =
        (NTSTATUS)InterlockedCompareExchange(&m_failureStatus, 0, 0);
    if (m_Preemption)
    {
        // This marker runs behind every earlier submission. Only now may
        // the scheduler regard those buffers as drained and reschedule.
        interrupt.InterruptType = DXGK_INTERRUPT_DMA_PREEMPTED;
        interrupt.DmaPreempted.PreemptionFenceId = m_FenceId;
        interrupt.DmaPreempted.LastCompletedFenceId =
            (UINT)InterlockedCompareExchange(&m_pAdapter->m_LastCompletedFenceId, 0, 0);
        interrupt.DmaPreempted.NodeOrdinal = m_NodeOrdinal;
        interrupt.DmaPreempted.EngineOrdinal = m_EngineOrdinal;
    }
    else if (NT_SUCCESS(failure))
    {
        // VIOGPU_MAX_RUNNING is one, so successful fences advance in order.
        InterlockedExchange(&m_pAdapter->m_LastCompletedFenceId, m_FenceId);
        interrupt.InterruptType = DXGK_INTERRUPT_DMA_COMPLETED;
        interrupt.DmaCompleted.SubmissionFenceId = m_FenceId;
        interrupt.DmaCompleted.NodeOrdinal = m_NodeOrdinal;
        interrupt.DmaCompleted.EngineOrdinal = m_EngineOrdinal;
    }
    else
    {
        interrupt.InterruptType = DXGK_INTERRUPT_DMA_FAULTED;
        interrupt.DmaFaulted.FaultedFenceId = m_FenceId;
        interrupt.DmaFaulted.Status = failure;
        interrupt.DmaFaulted.NodeOrdinal = m_NodeOrdinal;
        interrupt.DmaFaulted.EngineOrdinal = m_EngineOrdinal;
    }
    m_pAdapter->NotifyInterrupt(&interrupt, true);
}

void VioGpuCommand::QueueRunningCb(void *cmd, void *, void *responseBuffer)
{
    VioGpuCommand *self = (VioGpuCommand *)cmd;
    PGPU_CTRL_HDR response = (PGPU_CTRL_HDR)responseBuffer;
    if (response == NULL ||
        response->type < VIRTIO_GPU_RESP_OK_NODATA ||
        response->type >= VIRTIO_GPU_RESP_ERR_UNSPEC)
    {
        NTSTATUS status = STATUS_IO_DEVICE_ERROR;
        if (response != NULL)
        {
            if (response->type == VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY)
            {
                status = STATUS_INSUFFICIENT_RESOURCES;
            }
            else if (response->type == VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID ||
                     response->type == VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID ||
                     response->type == VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID ||
                     response->type == VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER)
            {
                status = STATUS_INVALID_PARAMETER;
            }
        }
        self->RecordFailure(status);
    }
    // Pair with the AddPending() that ran before the matching submit.
    // Re-queue only when this was the LAST outstanding submission: a
    // single DMA body can issue several async ops (e.g. a multi-index
    // MAP_BLOB), and Run() must re-enter exactly once -- when they have
    // all completed -- to advance past the command and reach `end:`.
    // Dropping to zero is the unique edge that re-queues; an earlier
    // completion just decrements. Drop before queue so the dtor's
    // zero-pending assertion can't observe a transient count.
    if (self->DropPending() == 0)
    {
        // Resume on the PASSIVE_LEVEL worker. It must perform local copies,
        // scanout rearming, allocation release, and initial-present state
        // changes before it reports the DXGK fence.
        self->QueueRunning();
    }
}

#pragma code_seg(pop)

PAGED_CODE_SEG_BEGIN

VioGpuCommander::VioGpuCommander(VioGpuAdapter *pAdapter)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    m_pAdapter = pAdapter;
    m_bStopWorkThread = FALSE;
    m_pWorkThread = NULL;

    KeInitializeEvent(&m_QueueEvent, SynchronizationEvent, FALSE);
    KeInitializeEvent(&m_ThreadExitEvent, NotificationEvent, FALSE);

    InitializeListHead(&m_SubmittedQueue);
    InitializeListHead(&m_RunningQueue);
    KeInitializeSpinLock(&m_Lock);

    m_running = 0;
    m_ActiveCommand = NULL;
}

NTSTATUS VioGpuCommander::Start()
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    // StopDevice may be followed by StartDevice without unloading the
    // adapter.  A previous stop leaves this flag set, which would otherwise
    // make the freshly-created worker terminate before accepting work.
    m_bStopWorkThread = FALSE;
    KeClearEvent(&m_ThreadExitEvent);

    HANDLE threadHandle = 0;

    NTSTATUS status = PsCreateSystemThread(&threadHandle,
                                           (ACCESS_MASK)0,
                                           NULL,
                                           (HANDLE)0,
                                           NULL,
                                           VioGpuCommander::ThreadWork,
                                           this);

    if (!NT_SUCCESS(status))
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("%s failed to create command worker thread, status %x\n", __FUNCTION__, status));
        VioGpuDbgBreak();
        return status;
    }

    status = ObReferenceObjectByHandle(threadHandle,
                                       0,
                                       NULL,
                                       KernelMode,
                                       (PVOID *)(&m_pWorkThread),
                                       NULL);
    if (!NT_SUCCESS(status))
    {
        DbgPrint(TRACE_LEVEL_FATAL,
                 ("%s failed to reference command worker thread, status %x\n",
                  __FUNCTION__, status));
        m_pWorkThread = NULL;
        m_bStopWorkThread = TRUE;
        KeSetEvent(&m_QueueEvent, IO_NO_INCREMENT, FALSE);
        KeWaitForSingleObject(&m_ThreadExitEvent,
                              Executive,
                              KernelMode,
                              FALSE,
                              NULL);
    }
    ZwClose(threadHandle);

    return status;
}

void VioGpuCommander::Stop()
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    // The worker can be stopped while a host command is outstanding.  Wake
    // private render-event waiters before stopping it; their command object
    // remains owned by the ordinary completion path, avoiding a callback/UAF.
    CancelRenderEvents();

    if (m_pWorkThread == NULL)
    {
        return;
    }

    m_bStopWorkThread = TRUE;
    KeSetEvent(&m_QueueEvent, IO_NO_INCREMENT, FALSE);

    // Teardown must not release queues or adapter memory while this worker
    // can still touch either.  The worker waits only on m_QueueEvent, which
    // was set above, so an unbounded object wait is deterministic here.
    KeWaitForSingleObject(m_pWorkThread, Executive, KernelMode, FALSE, NULL);

    ObDereferenceObject(m_pWorkThread);
    m_pWorkThread = NULL;

    // A command can leave the submitted list between the first scan and the
    // worker observing m_bStopWorkThread.  Scan again after the worker exits
    // so that small hand-off window cannot strand its render-event waiter.
    CancelRenderEvents();
}

void VioGpuCommander::CancelRenderEvents()
{
    KIRQL oldIrql;
    LockQueue(&oldIrql);

    for (PLIST_ENTRY entry = m_SubmittedQueue.Flink;
         entry != &m_SubmittedQueue;
         entry = entry->Flink)
    {
        CONTAINING_RECORD(entry, VioGpuCommand, list_entry)->SignalRenderEvent();
    }
    for (PLIST_ENTRY entry = m_RunningQueue.Flink;
         entry != &m_RunningQueue;
         entry = entry->Flink)
    {
        CONTAINING_RECORD(entry, VioGpuCommand, list_entry)->SignalRenderEvent();
    }
    VioGpuCommand *active = m_ActiveCommand;
    if (active != NULL)
    {
        active->SignalRenderEvent();
    }

    UnlockQueue(oldIrql);
}

void VioGpuCommander::ThreadWork(PVOID Context)
{
    PAGED_CODE();

    VioGpuCommander *pdev = reinterpret_cast<VioGpuCommander *>(Context);
    pdev->ThreadWorkRoutine();
}

void VioGpuCommander::ThreadWorkRoutine(void)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s\n", __FUNCTION__));

    KeSetPriorityThread(KeGetCurrentThread(), LOW_REALTIME_PRIORITY);

    for (;;)
    {
        KeWaitForSingleObject(&m_QueueEvent, Executive, KernelMode, FALSE, NULL);

        while (m_running < VIOGPU_MAX_RUNNING)
        {
            VioGpuCommand *command = DequeueSubmitted();
            if (command == NULL)
            {
                break;
            }
            KIRQL oldIrql;
            LockQueue(&oldIrql);
            ASSERT(m_ActiveCommand == NULL);
            m_ActiveCommand = command;
            UnlockQueue(oldIrql);
            QueueRunning(command);
            m_running++;
        }

        DbgPrint(TRACE_LEVEL_VERBOSE, ("%s Running command\n", __FUNCTION__));
        while (true)
        {
            VioGpuCommand *command = DequeueRunning();
            if (command == NULL)
            {
                break;
            }
            command->Run();
        }

        if (m_bStopWorkThread)
        {
            KIRQL oldIrql;
            LockQueue(&oldIrql);
            BOOLEAN idle = IsListEmpty(&m_SubmittedQueue) &&
                           IsListEmpty(&m_RunningQueue) &&
                           m_ActiveCommand == NULL && m_running == 0;
            UnlockQueue(oldIrql);
            if (idle)
            {
                KeSetEvent(&m_ThreadExitEvent, IO_NO_INCREMENT, FALSE);
                PsTerminateSystemThread(STATUS_SUCCESS);
                return;
            }
        }
    }
}

void VioGpuCommander::CommandFinished(VioGpuCommand *cmd)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s", __FUNCTION__));

    KIRQL oldIrql;
    LockQueue(&oldIrql);
    if (m_ActiveCommand == cmd)
    {
        m_ActiveCommand = NULL;
    }
    m_running--;
    UnlockQueue(oldIrql);
    KeSetEvent(&m_QueueEvent, IO_NO_INCREMENT, FALSE);
}

NTSTATUS VioGpuCommander::Patch(const DXGKARG_PATCH *pPatch)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--> %s \n", __FUNCTION__));

    if (pPatch == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    VioGpuDxContext *context = VioGpuDxContext::FromHandle(pPatch->hContext);
    VioGpuDevice *pDevice = context ? context->GetDevice() : NULL;
    if ((pDevice == NULL) ||
        ((pPatch->AllocationListSize != 0) && (pPatch->pAllocationList == NULL)))
    {
        return STATUS_INVALID_PARAMETER;
    }

    for (UINT i = 0; i < pPatch->AllocationListSize; i++)
    {
        const DXGK_ALLOCATIONLIST *allocList = &pPatch->pAllocationList[i];
        VioGpuDeviceAllocation *deviceAllocation = VioGpuDeviceAllocation::FromHandle(allocList->hDeviceSpecificAllocation);
        VioGpuAllocation *allocation = deviceAllocation ? deviceAllocation->GetAllocation() : nullptr;
        if (allocation)
        {
            allocation->m_SegmentAddress = allocList->PhysicalAddress;
        }
#if defined(VIOGPU_TARGET_VISTA)
        if (allocation && allocation->IsPrimary() && !allocation->IsBlob())
        {
            if (allocList->SegmentId != VioGpuAdapter::FRAMEBUFFER_SEGMENT_ID)
            {
                return STATUS_INVALID_PARAMETER;
            }
            NTSTATUS backingStatus =
                allocation->AttachFrameBufferBacking(allocList->PhysicalAddress);
            if (!NT_SUCCESS(backingStatus))
            {
                return backingStatus;
            }
        }
#endif
        if (allocation && allocation->IsBlob() &&
            allocList->SegmentId == VioGpuAdapter::SHMEM_SEGMENT_ID)
        {
            allocation->m_Blob.MapOffset = allocList->PhysicalAddress.QuadPart - VioGpuAdapter::SHMEM_GPU_BASE_VA;
            DbgPrint(TRACE_LEVEL_VERBOSE, ("<--> %s res_id=%d base=%p addr=%p off=%llx\n",
                                           __FUNCTION__,
                                           allocation->GetId(),
                                           pDevice->m_pAdapter->GetShmemPA(),
                                           allocList->PhysicalAddress.QuadPart,
                                           allocation->m_Blob.MapOffset));
        }
    }

    return STATUS_SUCCESS;
}

PAGED_CODE_SEG_END

#pragma code_seg(push)
#pragma code_seg()
NTSTATUS VioGpuCommander::QueuePreemption(const DXGKARG_PREEMPTCOMMAND *request)
{
    VioGpuCommand *marker = new (VIOGPU_NONPAGED_POOL) VioGpuCommand(m_pAdapter, NULL);
    if (marker == NULL)
        return STATUS_INSUFFICIENT_RESOURCES;
    marker->SetPreemption(request);
    QueueSubmitted(marker);
    return STATUS_SUCCESS;
}

NTSTATUS VioGpuCommander::SubmitCommand(const DXGKARG_SUBMITCOMMAND *pSubmitCommand)
{
    VioGpuDxContext *context = pSubmitCommand != NULL
                                   ? VioGpuDxContext::FromHandle(pSubmitCommand->hContext)
                                   : NULL;
    VioGpuDevice *submitDevice = context != NULL ? context->GetDevice() : NULL;
    if ((pSubmitCommand == NULL) ||
        (pSubmitCommand->DmaBufferSubmissionStartOffset >
         pSubmitCommand->DmaBufferSubmissionEndOffset) ||
        (pSubmitCommand->DmaBufferSubmissionEndOffset >
         pSubmitCommand->DmaBufferSize) ||
        (pSubmitCommand->DmaBufferPrivateDataSubmissionStartOffset >
         pSubmitCommand->DmaBufferPrivateDataSubmissionEndOffset) ||
        (pSubmitCommand->DmaBufferPrivateDataSubmissionEndOffset >
         pSubmitCommand->DmaBufferPrivateDataSize) ||
        ((pSubmitCommand->DmaBufferPrivateDataSize != 0) &&
         (pSubmitCommand->pDmaBufferPrivateData == NULL)) ||
        ((pSubmitCommand->DmaBufferPrivateDataSize != 0) &&
         (pSubmitCommand->DmaBufferPrivateDataSize < sizeof(void *))) ||
        (submitDevice == NULL))
    {
        return STATUS_INVALID_PARAMETER;
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s fence_id=%d\n", __FUNCTION__, pSubmitCommand->SubmissionFenceId));

    VioGpuCommand *cmd = NULL;
    if (pSubmitCommand->pDmaBufferPrivateData &&
        pSubmitCommand->DmaBufferPrivateDataSize >= sizeof(void *))
    {
        void *privateHandle = *(void **)pSubmitCommand->pDmaBufferPrivateData;
        cmd = VioGpuCommand::FromHandle(privateHandle);
        if ((privateHandle != NULL) &&
            (cmd == NULL || !cmd->BelongsToDevice(submitDevice)))
        {
            return STATUS_INVALID_PARAMETER;
        }
    }

    if (!cmd)
    {
        if (pSubmitCommand->DmaBufferSubmissionStartOffset !=
            pSubmitCommand->DmaBufferSubmissionEndOffset)
        {
            return STATUS_INVALID_PARAMETER;
        }
        cmd = new (VIOGPU_NONPAGED_POOL) VioGpuCommand(m_pAdapter, submitDevice);
        if (cmd == NULL)
        {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    if ((pSubmitCommand->DmaBufferSubmissionStartOffset !=
         pSubmitCommand->DmaBufferSubmissionEndOffset) &&
        !cmd->HasDmaBuffer())
    {
        // A nonempty Render/Present submission must refer to the DMA virtual
        // address captured when the command was translated.  SubmitCommand
        // receives only the physical address, so continuing here would leave
        // Run() with a NULL command range and falsely complete the fence.
        return STATUS_INVALID_PARAMETER;
    }

    cmd->PrepareSubmit(pSubmitCommand, submitDevice);
    InterlockedExchange(&m_pAdapter->m_LastSubmittedFenceId, pSubmitCommand->SubmissionFenceId);
    QueueSubmitted(cmd);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return STATUS_SUCCESS;
}

_IRQL_requires_max_(DISPATCH_LEVEL) _IRQL_saves_global_(OldIrql,
                                                        Irql) _IRQL_raises_(DISPATCH_LEVEL) void VioGpuCommander::
                                                                                                    LockQueue(KIRQL *Irql)
{
    KIRQL SavedIrql = KeGetCurrentIrql();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s at IRQL %d\n", __FUNCTION__, SavedIrql));

    if (SavedIrql < DISPATCH_LEVEL)
    {
        KeAcquireSpinLock(&m_Lock, &SavedIrql);
    }
    else if (SavedIrql == DISPATCH_LEVEL)
    {
        KeAcquireSpinLockAtDpcLevel(&m_Lock);
    }
    else
    {
        VioGpuDbgBreak();
    }
    *Irql = SavedIrql;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

_IRQL_requires_(DISPATCH_LEVEL) _IRQL_restores_global_(OldIrql, Irql) void VioGpuCommander::UnlockQueue(KIRQL Irql)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s at IRQL %d\n", __FUNCTION__, Irql));

    if (Irql < DISPATCH_LEVEL)
    {
        KeReleaseSpinLock(&m_Lock, Irql);
    }
    else
    {
        KeReleaseSpinLockFromDpcLevel(&m_Lock);
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

VioGpuCommand *VioGpuCommander::DequeueRunning()
{
    KIRQL oldIrql;
    LockQueue(&oldIrql);
    PLIST_ENTRY result = NULL;
    if (!IsListEmpty(&m_RunningQueue))
    {
        result = RemoveHeadList(&m_RunningQueue);
    }
    UnlockQueue(oldIrql);
    if (!result)
    {
        return NULL;
    }
    return CONTAINING_RECORD(result, VioGpuCommand, list_entry);
}

void VioGpuCommander::QueueRunning(VioGpuCommand *cmd)
{
    KIRQL oldIrql;
    LockQueue(&oldIrql);
    InsertTailList(&m_RunningQueue, &cmd->list_entry);
    UnlockQueue(oldIrql);

    KeSetEvent(&m_QueueEvent, IO_NO_INCREMENT, FALSE);
}

VioGpuCommand *VioGpuCommander::DequeueSubmitted()
{
    KIRQL oldIrql;
    LockQueue(&oldIrql);
    PLIST_ENTRY result = NULL;
    if (!IsListEmpty(&m_SubmittedQueue))
    {
        result = RemoveHeadList(&m_SubmittedQueue);
    }
    UnlockQueue(oldIrql);
    if (!result)
    {
        return NULL;
    }
    return CONTAINING_RECORD(result, VioGpuCommand, list_entry);
}

void VioGpuCommander::QueueSubmitted(VioGpuCommand *cmd)
{
    KIRQL oldIrql;
    LockQueue(&oldIrql);
    InsertTailList(&m_SubmittedQueue, &cmd->list_entry);
    UnlockQueue(oldIrql);

    KeSetEvent(&m_QueueEvent, IO_NO_INCREMENT, FALSE);
}

#pragma code_seg(pop)
