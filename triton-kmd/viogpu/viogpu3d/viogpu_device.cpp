#include "viogpu_device.h"
#include "viogpu_adapter.h"
#include "baseobj.h"
#include "virgl_hw.h"

PAGED_CODE_SEG_BEGIN

VioGpuContext::VioGpuContext(VioGpuAdapter *pAdapter) {
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--> %s\n", __FUNCTION__));
    m_Capset = 0;
    m_pAdapter = pAdapter;
    m_id = m_pAdapter->ctxIdr.GetId();
    m_empty = TRUE;
    m_NumRings = 0;
}

NTSTATUS VioGpuContext::Init(VIOGPU_CTX_INIT_REQ *pOptions) {
    PAGED_CODE();

    if (pOptions == NULL || pOptions->CapsetID == 0 ||
        pOptions->NumRings == 0 || pOptions->NumRings > 64)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (!m_empty)
    {
        /* Neptune creates the host D3D11 proxy first.  Triton's D3D9 layer
         * then ensures that the same per-device virtio context exists before
         * it registers shared render targets.  Reuse a compatible context;
         * a second host context here would make the two UMD layers disagree
         * about resource ownership. */
        if (pOptions->CapsetID != m_Capset ||
            pOptions->NumRings > m_NumRings)
        {
            DbgPrint(TRACE_LEVEL_WARNING,
                     ("<--> %s incompatible context reuse (ctx_id=%d new capset=%d rings=%d old capset=%d rings=%d)\n",
                      __FUNCTION__, m_id, pOptions->CapsetID,
                      pOptions->NumRings, m_Capset, m_NumRings));
            return STATUS_INVALID_DEVICE_STATE;
        }

        DbgPrint(TRACE_LEVEL_INFORMATION,
                 ("<--> %s reusing context (ctx_id=%d capset=%d rings=%d)\n",
                  __FUNCTION__, m_id, m_Capset, m_NumRings));
        return STATUS_SUCCESS;
    }

    DbgPrint(TRACE_LEVEL_WARNING,
             ("<--> %s (ctx_id=%d capset=%d name=%s)\n",
              __FUNCTION__, m_id, pOptions->CapsetID, pOptions->DebugName));
    NTSTATUS status = m_pAdapter->ctrlQueue.CreateCtx(
        m_id, pOptions->CapsetID, pOptions->DebugName);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    m_Capset = pOptions->CapsetID;
    m_NumRings = pOptions->NumRings;
    m_empty = FALSE;
    return STATUS_SUCCESS;
}

PAGED_CODE_SEG_END

#pragma code_seg(push)
#pragma code_seg()

static void NotifyContextDestroyed(void *ctx, void *cmd, void *)
{
    VioGpuIdr *ctxIdr = reinterpret_cast<VioGpuIdr *>(ctx);
    PGPU_CTRL_HDR cmd_hdr = reinterpret_cast<PGPU_CTRL_HDR>(cmd);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--> %s freeing ctx %lu\n", __FUNCTION__, cmd_hdr->ctx_id));
    ctxIdr->PutId(cmd_hdr->ctx_id);
}

#pragma code_seg(pop)

PAGED_CODE_SEG_BEGIN

VioGpuContext::~VioGpuContext() {
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--> %s\n", __FUNCTION__));

    if (m_empty)
    {
        m_pAdapter->ctxIdr.PutId(m_id);
    }
    else
    {
        m_pAdapter->ctrlQueue.DestroyCtx(m_id,
                                         NotifyContextDestroyed,
                                         &m_pAdapter->ctxIdr);
    }
}

VioGpuDevice::VioGpuDevice(VioGpuAdapter *pAdapter) : m_Context(pAdapter), m_Virgl(pAdapter)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--> %s\n", __FUNCTION__));

    m_hUM = NULL;
    m_hKM = NULL;
    m_pBlit = NULL;

    m_pAdapter = pAdapter;
}

VioGpuDevice::~VioGpuDevice()
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--> %s", __FUNCTION__));

    if (m_hUM) ObDereferenceObject(m_hUM);
    if (m_hKM) ObDereferenceObject(m_hKM);
}

NTSTATUS VioGpuDevice::GenerateBltPresent(DXGKARG_PRESENT *pPresent,
                                          VioGpuDeviceAllocation *srcDev,
                                          VioGpuDeviceAllocation *dstDev,
                                          VioGpuAllocation **pInitialPresentCompletion)
{
    if (pPresent == NULL || srcDev == NULL || dstDev == NULL ||
        pInitialPresentCompletion == NULL ||
        pPresent->pDmaBuffer == NULL || pPresent->DmaSize == 0 ||
        pPresent->SubRectCnt == 0 || pPresent->pDstSubRects == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }
    *pInitialPresentCompletion = NULL;

    VioGpuAllocation *src = srcDev->GetAllocation();
    VioGpuAllocation *dst = dstDev->GetAllocation();
    if (src == NULL || dst == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--> %s\n", __FUNCTION__));

    DbgPrint(TRACE_LEVEL_WARNING,
             ("<--> %s src=%d dst=%d srcPrim=%d dstPrim=%d srcBlob=%d dstBlob=%d "
             "srcCoherent=%d dstCoherent=%d needsInitial=%d\n",
              __FUNCTION__, src->GetId(), dst->GetId(), src->IsPrimary(),
              dst->IsPrimary(), src->IsBlob(), dst->IsBlob(),
              src->IsCoherent(), dst->IsCoherent(),
              dst->NeedsInitialPresent()));

    UINT srcWidth = 0, srcHeight = 0, dstWidth = 0, dstHeight = 0;
    if (!src->GetDimensions(&srcWidth, &srcHeight) ||
        !dst->GetDimensions(&dstWidth, &dstHeight))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("%s missing 2D resource dimensions src=%d dst=%d\n",
                  __FUNCTION__, src->GetId(), dst->GetId()));
        return STATUS_INVALID_PARAMETER;
    }
    DbgPrint(TRACE_LEVEL_WARNING,
             ("%s dimensions src=%ux%u dst=%ux%u rects=%u "
              "srcRect=%ld,%ld,%ld,%ld dstRect=%ld,%ld,%ld,%ld\n",
              __FUNCTION__, srcWidth, srcHeight, dstWidth, dstHeight,
              pPresent->SubRectCnt,
              pPresent->SrcRect.left, pPresent->SrcRect.top,
              pPresent->SrcRect.right, pPresent->SrcRect.bottom,
              pPresent->DstRect.left, pPresent->DstRect.top,
              pPresent->DstRect.right, pPresent->DstRect.bottom));

    const LONGLONG sourceRectWidth =
        (LONGLONG)pPresent->SrcRect.right - pPresent->SrcRect.left;
    const LONGLONG sourceRectHeight =
        (LONGLONG)pPresent->SrcRect.bottom - pPresent->SrcRect.top;
    const LONGLONG destinationRectWidth =
        (LONGLONG)pPresent->DstRect.right - pPresent->DstRect.left;
    const LONGLONG destinationRectHeight =
        (LONGLONG)pPresent->DstRect.bottom - pPresent->DstRect.top;
    if (pPresent->SrcRect.left < 0 || pPresent->SrcRect.top < 0 ||
        pPresent->DstRect.left < 0 || pPresent->DstRect.top < 0 ||
        sourceRectWidth <= 0 || sourceRectHeight <= 0 ||
        destinationRectWidth != sourceRectWidth ||
        destinationRectHeight != sourceRectHeight ||
        (ULONGLONG)pPresent->SrcRect.right > srcWidth ||
        (ULONGLONG)pPresent->SrcRect.bottom > srcHeight ||
        (ULONGLONG)pPresent->DstRect.right > dstWidth ||
        (ULONGLONG)pPresent->DstRect.bottom > dstHeight)
    {
        // The owned command path implements translation-only copies. It must
        // not silently treat a stretch as an unscaled resource copy.
        return STATUS_NOT_SUPPORTED;
    }

    const LONGLONG dx64 = (LONGLONG)pPresent->SrcRect.left -
                          (LONGLONG)pPresent->DstRect.left;
    const LONGLONG dy64 = (LONGLONG)pPresent->SrcRect.top -
                          (LONGLONG)pPresent->DstRect.top;
    if (dx64 < (-2147483647LL - 1) || dx64 > 2147483647LL ||
        dy64 < (-2147483647LL - 1) || dy64 > 2147483647LL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    RECT coverRect = pPresent->pDstSubRects[0];
    for (UINT i = 0; i < pPresent->SubRectCnt; i++)
    {
        const RECT &rect = pPresent->pDstSubRects[i];
        const LONGLONG srcLeft = (LONGLONG)rect.left + dx64;
        const LONGLONG srcTop = (LONGLONG)rect.top + dy64;
        const LONGLONG srcRight = (LONGLONG)rect.right + dx64;
        const LONGLONG srcBottom = (LONGLONG)rect.bottom + dy64;
        if (rect.left < 0 || rect.top < 0 ||
            rect.right <= rect.left || rect.bottom <= rect.top ||
            rect.left < pPresent->DstRect.left ||
            rect.top < pPresent->DstRect.top ||
            rect.right > pPresent->DstRect.right ||
            rect.bottom > pPresent->DstRect.bottom ||
            (ULONGLONG)rect.right > dstWidth ||
            (ULONGLONG)rect.bottom > dstHeight ||
            srcLeft < 0 || srcTop < 0 ||
            srcRight <= srcLeft || srcBottom <= srcTop ||
            (ULONGLONG)srcRight > srcWidth ||
            (ULONGLONG)srcBottom > srcHeight)
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("%s invalid blt rectangle %u\n", __FUNCTION__, i));
            return STATUS_INVALID_PARAMETER;
        }
        if (i != 0)
        {
            coverRect.top = min(coverRect.top, rect.top);
            coverRect.left = min(coverRect.left, rect.left);
            coverRect.right = max(coverRect.right, rect.right);
            coverRect.bottom = max(coverRect.bottom, rect.bottom);
        }
    }

#if defined(VIOGPU_TARGET_VISTA)
    /* A standard shared primary is a present target, not DWM's pixel store.
     * For a full-size Neptune shared texture, use the host surface as the
     * scanout source (the same ownership model as VirtualBox's
     * BLIT_SURFACE_TO_SCREEN path).  Do not create a virgl copy or read it
     * back through the CPU-fixed primary. */
    const BOOLEAN directHostPresent =
        dst->IsPrimary() && !dst->IsBlob() &&
        src->IsHostPresentationSurface() &&
        srcWidth == dstWidth && srcHeight == dstHeight;
    if (directHostPresent)
    {
        if (pPresent->DmaSize < sizeof(VIOGPU_COMMAND_HDR))
        {
            pPresent->MultipassOffset = 0;
            return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
        }

        VIOGPU_COMMAND_HDR *cmd =
            (VIOGPU_COMMAND_HDR *)pPresent->pDmaBuffer;
        cmd->type = VIOGPU_CMD_NOP;
        cmd->size = 0;
        cmd->flags = 0;
        cmd->ring_idx = 0;
        pPresent->pDmaBuffer =
            (UCHAR *)pPresent->pDmaBuffer + sizeof(*cmd);
        DbgPrint(TRACE_LEVEL_INFORMATION,
                 ("%s direct host scanout src=%d dst-standard-primary=%d\n",
                  __FUNCTION__, src->GetId(), dst->GetId()));
        return STATUS_SUCCESS;
    }
#endif

    // A newly selected standard primary has a fresh zeroed backing store.
    // Vista's first Present after the VidPn transition is often a small dirty
    // rectangle (for example the activation window), while the source still
    // contains the complete desktop.  Bootstrap that primary with one full
    // frame; subsequent Presents retain the normal dirty-rectangle behavior.
    const BOOL fullPrimaryBootstrap =
        dst->IsPrimary() && !dst->IsBlob() && dst->NeedsInitialPresent() &&
        srcWidth == dstWidth && srcHeight == dstHeight;
    RECT bootstrapRect = { 0, 0, (LONG)dstWidth, (LONG)dstHeight };
    const RECT *copyRects = pPresent->pDstSubRects;
    UINT copyRectCnt = pPresent->SubRectCnt;
    LONGLONG copyDx64 = dx64;
    LONGLONG copyDy64 = dy64;
    if (fullPrimaryBootstrap)
    {
        copyRects = &bootstrapRect;
        copyRectCnt = 1;
        copyDx64 = 0;
        copyDy64 = 0;
        coverRect = bootstrapRect;
        DbgPrint(TRACE_LEVEL_INFORMATION,
                 ("%s full-primary bootstrap src=%d dst=%d %ux%u\n",
                  __FUNCTION__, src->GetId(), dst->GetId(),
                  dstWidth, dstHeight));
    }

#if defined(VIOGPU_TARGET_VISTA)
    /* Vista's legacy GDI path is a coherent shadow/staging surface copied to
     * the fixed primary in the DMA worker.  Host-backed DWM resources took
     * the direct-scanout return above and must never enter this path. */
    const BOOLEAN vistaFixedPrimary =
        dst->IsPrimary() && !dst->IsBlob() && !src->IsBlob() &&
        src->IsCoherent();
#else
    const BOOLEAN vistaFixedPrimary = FALSE;
#endif

    const UINT sizeOfSetType = 4 * (VIRGL_PIPE_RES_SET_TYPE_SIZE(1) + 1);
    const UINT sizeOfOneRect = 4 * (VIRGL_CMD_RESOURCE_COPY_REGION_SIZE + 1);
    const ULONGLONG fixedPrimaryCopyBody =
        sizeof(VIOGPU_COPY_FIXED_PRIMARY_CMD) +
        (ULONGLONG)copyRectCnt * sizeof(RECT);
    ULONGLONG requiredDma =
        sizeof(VIOGPU_COMMAND_HDR) + fixedPrimaryCopyBody +
        sizeof(VIOGPU_COMMAND_HDR) + sizeof(VIOGPU_TRANSFER_CMD) +
        sizeof(VIOGPU_COMMAND_HDR) +
            sizeof(VIOGPU_FLUSH_FIXED_PRIMARY_CMD);
    if (!vistaFixedPrimary)
    {
        requiredDma = sizeof(VIOGPU_COMMAND_HDR) +
                      (ULONGLONG)copyRectCnt * sizeOfOneRect;
        if (src->IsCoherent())
            requiredDma += sizeof(VIOGPU_COMMAND_HDR) + sizeof(VIOGPU_TRANSFER_CMD);
        if (dst->IsCoherent())
            requiredDma += sizeof(VIOGPU_COMMAND_HDR) + sizeof(VIOGPU_TRANSFER_CMD);
        if (src->IsBlob())
            requiredDma += sizeof(VIOGPU_COMMAND_HDR) + sizeOfSetType;
        if (dst->IsBlob())
            requiredDma += sizeof(VIOGPU_COMMAND_HDR) + sizeOfSetType;
    }
    if (requiredDma > pPresent->DmaSize)
    {
        DbgPrint(TRACE_LEVEL_WARNING,
                 ("%s needs %llu DMA bytes, has %u\n",
                  __FUNCTION__, requiredDma, pPresent->DmaSize));
        pPresent->MultipassOffset = 0;
        return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    }

    // Validate both coherent layouts before writing any part of the packet.
    ULONG checkedStride = 0;
    ULONGLONG checkedOffset = 0;
    if ((src->IsCoherent() &&
         !src->GetTransferLayout((LONG)((LONGLONG)coverRect.left + copyDx64),
                                 (LONG)((LONGLONG)coverRect.top + copyDy64),
                                 &checkedStride, &checkedOffset)) ||
        (dst->IsCoherent() &&
         !dst->GetTransferLayout(coverRect.left, coverRect.top,
                                 &checkedStride, &checkedOffset)))
    {
        return STATUS_INVALID_PARAMETER;
    }

    VIOGPU_BLOB_INFO srcBlobInfo = {};
    VIOGPU_BLOB_INFO dstBlobInfo = {};
    ULONGLONG srcBlobModifier = 0;
    ULONGLONG dstBlobModifier = 0;
    if (src->IsBlob())
    {
        auto sourceLock = src->LockGuard();
        if (!src->m_Blob.InfoValid)
        {
            return STATUS_INVALID_DEVICE_STATE;
        }
        srcBlobInfo = src->m_Blob.Info;
        srcBlobModifier = src->m_BlobModifier;
    }
    if (dst->IsBlob())
    {
        auto destinationLock = dst->LockGuard();
        if (!dst->m_Blob.InfoValid)
        {
            return STATUS_INVALID_DEVICE_STATE;
        }
        dstBlobInfo = dst->m_Blob.Info;
        dstBlobModifier = dst->m_BlobModifier;
    }

    UCHAR *dmaBuf = (UCHAR *)pPresent->pDmaBuffer;

    // The virgl shadow context is normally created by the UMD's
    // VIOGPU_CTX_INIT escape (VioGpu3DEscape). The GDI/System device that
    // carries the basic-model shadow->primary blt presents never runs that
    // escape, so m_Virgl holds only a guest-minted id with NO host context
    // behind it -- every CtxResource attach and SUBMIT below is silently
    // dropped by the host and the primary never receives the blt (frozen
    // black desktop). Create the host context lazily on first use.
    if (m_Virgl.IsEmpty())
    {
        bool has_virgl  = !!(m_pAdapter->m_supportedCapsetIDs & (1llu << VIRTIO_GPU_CAPSET_VIRGL));
        bool has_virgl2 = !!(m_pAdapter->m_supportedCapsetIDs & (1llu << VIRTIO_GPU_CAPSET_VIRGL2));
        if (has_virgl || has_virgl2)
        {
            VIOGPU_CTX_INIT_REQ VirglCtx;
            memset(&VirglCtx, 0, sizeof(VirglCtx));
            VirglCtx.CapsetID = has_virgl2 ? VIRTIO_GPU_CAPSET_VIRGL2 : VIRTIO_GPU_CAPSET_VIRGL;
            VirglCtx.NumRings = 64;
            memcpy(VirglCtx.DebugName, "virgl-gdi-blt", sizeof("virgl-gdi-blt") - 1);
            NTSTATUS contextStatus = m_Virgl.Init(&VirglCtx);
            if (!NT_SUCCESS(contextStatus))
            {
                DbgPrint(TRACE_LEVEL_ERROR,
                         ("%s failed to create lazy virgl context: 0x%X\n",
                          __FUNCTION__, contextStatus));
                return contextStatus;
            }
        }
        else
        {
            DbgPrint(TRACE_LEVEL_ERROR, ("%s no virgl capset for blt present\n", __FUNCTION__));
            return STATUS_UNSUCCESSFUL;
        }
    }

    // Finish all fallible control-queue work before writing the DMA packet.
    // A failed attach can then return without leaving a partially generated
    // packet or falsely recording a PIPE_RESOURCE_SET_TYPE command as sent.
    if (!srcDev->m_AttachedToVirgl)
    {
        NTSTATUS attachStatus =
            GetCtrlQueue()->CtxResource(true, m_Virgl.GetId(), src->GetId());
        if (!NT_SUCCESS(attachStatus))
            return attachStatus;
        srcDev->m_AttachedToVirgl = true;
    }
    if (!dstDev->m_AttachedToVirgl)
    {
        NTSTATUS attachStatus =
            GetCtrlQueue()->CtxResource(true, m_Virgl.GetId(), dst->GetId());
        if (!NT_SUCCESS(attachStatus))
            return attachStatus;
        dstDev->m_AttachedToVirgl = true;
    }

    INT dx = (INT)copyDx64;
    INT dy = (INT)copyDy64;

    if (vistaFixedPrimary)
    {
        // Vista's basic display path is a CPU shadow -> fixed-primary blt.
        // Uploading the shadow and asking the host GL context to copy it into
        // the scanout resource leaves an all-black boot desktop on ANGLE even
        // though both virtio commands return OK.  Preserve Vista's native
        // semantics: serialize the dirty-row copy before one
        // TRANSFER_TO_HOST for the primary.  The copy must run in the DMA
        // worker, after VidMm has mapped and patched the source allocation;
        // its MDL is not yet available while DxgkDdiPresent generates DMA.
        if (fixedPrimaryCopyBody > MAXULONG)
        {
            return STATUS_INVALID_BUFFER_SIZE;
        }

        VIOGPU_COMMAND_HDR *copyHeader = (VIOGPU_COMMAND_HDR *)dmaBuf;
        copyHeader->type = VIOGPU_CMD_COPY_FIXED_PRIMARY;
        copyHeader->size = (UINT)fixedPrimaryCopyBody;
        copyHeader->flags = 0;
        copyHeader->ring_idx = 0;
        dmaBuf += sizeof(*copyHeader);

        VIOGPU_COPY_FIXED_PRIMARY_CMD *copyBody =
            (VIOGPU_COPY_FIXED_PRIMARY_CMD *)dmaBuf;
        copyBody->SourceAllocationIndex = DXGK_PRESENT_SOURCE_INDEX;
        copyBody->DestinationAllocationIndex = DXGK_PRESENT_DESTINATION_INDEX;
        copyBody->SourceDeltaX = dx;
        copyBody->SourceDeltaY = dy;
        copyBody->RectCount = copyRectCnt;
        dmaBuf += sizeof(*copyBody);
        RtlCopyMemory(dmaBuf, copyRects,
                      (SIZE_T)copyRectCnt * sizeof(RECT));
        dmaBuf += (SIZE_T)copyRectCnt * sizeof(RECT);

        ULONG destinationStride = 0;
        ULONGLONG destinationOffset = 0;
        if (!dst->GetTransferLayout(coverRect.left,
                                    coverRect.top,
                                    &destinationStride,
                                    &destinationOffset))
        {
            return STATUS_INVALID_PARAMETER;
        }

        VIOGPU_COMMAND_HDR *cmd_hdr = (VIOGPU_COMMAND_HDR *)dmaBuf;
        cmd_hdr->type = VIOGPU_CMD_TRANSFER_TO_HOST;
        cmd_hdr->size = sizeof(VIOGPU_TRANSFER_CMD);
        cmd_hdr->flags = VIOGPU_EXECBUF_VIRGL;
        cmd_hdr->ring_idx = 0;
        dmaBuf += sizeof(VIOGPU_COMMAND_HDR);

        VIOGPU_TRANSFER_CMD *cmdBody = (VIOGPU_TRANSFER_CMD *)dmaBuf;
        dmaBuf += sizeof(VIOGPU_TRANSFER_CMD);
        RtlZeroMemory(cmdBody, sizeof(*cmdBody));
        cmdBody->res_id = dst->GetId();
        cmdBody->box.x = coverRect.left;
        cmdBody->box.y = coverRect.top;
        cmdBody->box.width = coverRect.right - coverRect.left;
        cmdBody->box.height = coverRect.bottom - coverRect.top;
        cmdBody->box.depth = 1;
        cmdBody->stride = destinationStride;
        cmdBody->offset = destinationOffset;

        VIOGPU_COMMAND_HDR *flushHeader =
            (VIOGPU_COMMAND_HDR *)dmaBuf;
        flushHeader->type = VIOGPU_CMD_FLUSH_FIXED_PRIMARY;
        flushHeader->size = sizeof(VIOGPU_FLUSH_FIXED_PRIMARY_CMD);
        flushHeader->flags = 0;
        flushHeader->ring_idx = 0;
        dmaBuf += sizeof(*flushHeader);

        VIOGPU_FLUSH_FIXED_PRIMARY_CMD *flushBody =
            (VIOGPU_FLUSH_FIXED_PRIMARY_CMD *)dmaBuf;
        flushBody->DestinationAllocationIndex =
            DXGK_PRESENT_DESTINATION_INDEX;
        dmaBuf += sizeof(*flushBody);

        pPresent->pDmaBuffer = dmaBuf;
        if (fullPrimaryBootstrap)
        {
            *pInitialPresentCompletion = dst;
        }
        DbgPrint(TRACE_LEVEL_INFORMATION,
                 ("%s queued fixed-primary CPU blt src=%d dst=%d rects=%u\n",
                  __FUNCTION__, src->GetId(), dst->GetId(),
                  copyRectCnt));
        return STATUS_SUCCESS;
    }

    // If source requires coherency (staging or shadow surface) then emit transfer
    /* Vista's standard shadow/staging source is the VirtualBox-equivalent
     * GMRFB surface.  Upload its guest backing before copying the virgl
     * resource into the fixed primary.  Render-target resources that carry a
     * host-linked blob are non-coherent and do not enter this branch. */
    if (src->IsCoherent())
    {
        ULONG transferStride;
        ULONGLONG transferOffset;
        LONG transferX = coverRect.left + (LONG)copyDx64;
        LONG transferY = coverRect.top + (LONG)copyDy64;
        if (!src->GetTransferLayout(transferX, transferY, &transferStride, &transferOffset))
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("<--> %s invalid source transfer layout res_id=%d x=%ld y=%ld\n",
                      __FUNCTION__, src->GetId(), transferX, transferY));
            return STATUS_INVALID_PARAMETER;
        }

        VIOGPU_COMMAND_HDR *cmd_hdr = (VIOGPU_COMMAND_HDR *)dmaBuf;
        cmd_hdr->type = VIOGPU_CMD_TRANSFER_TO_HOST;
        cmd_hdr->size = sizeof(VIOGPU_TRANSFER_CMD);
        cmd_hdr->flags = 0;
        cmd_hdr->ring_idx = 0;
        dmaBuf += sizeof(VIOGPU_COMMAND_HDR);

        // Route through the virgl shadow ctx like every other command in
        // this packet: m_Context is uninitialized for the GDI/System
        // device, and a TRANSFER_TO_HOST_3D on a nonexistent ctx is
        // rejected by the host (EINVAL) -- the shadow surface pixels never
        // reach the host texture and the desktop blits stay black.
        if (!m_Context.IsVirgl())
        {
            cmd_hdr->flags |= VIOGPU_EXECBUF_VIRGL;
        }

        VIOGPU_TRANSFER_CMD *cmdBody = (VIOGPU_TRANSFER_CMD *)dmaBuf;
        dmaBuf += sizeof(VIOGPU_TRANSFER_CMD);

        cmdBody->res_id = src->GetId();

        cmdBody->box.x = coverRect.left + (LONG)copyDx64;
        cmdBody->box.y = coverRect.top + (LONG)copyDy64;
        cmdBody->box.z = 0;
        cmdBody->box.width = coverRect.right - coverRect.left;
        cmdBody->box.height = coverRect.bottom - coverRect.top;
        cmdBody->box.depth = 1;

        cmdBody->layer_stride = 0;
        cmdBody->stride = transferStride;
        cmdBody->level = 0;
        cmdBody->offset = transferOffset;
    }

    // PIPE_RESOURCE_SET_TYPE is idempotent. Emit it in each DMA packet that
    // uses a blob. A generation-time cache can outlive an unsubmitted or
    // rejected packet and then suppress the only command the host needed.
    if (src->IsBlob())
    {

            VIOGPU_COMMAND_HDR *cmd_hdr = (VIOGPU_COMMAND_HDR *)dmaBuf;
            cmd_hdr->type = VIOGPU_CMD_SUBMIT;
            cmd_hdr->size = sizeOfSetType;
            cmd_hdr->flags = 0;
            cmd_hdr->ring_idx = 0;
            dmaBuf += sizeof(VIOGPU_COMMAND_HDR);

            if (!m_Context.IsVirgl())
            {
                cmd_hdr->flags |= VIOGPU_EXECBUF_VIRGL;
            }

            UINT *cmdBody = (UINT *)dmaBuf;
            dmaBuf += sizeOfSetType;

            cmdBody[0] = VIRGL_CMD0(VIRGL_CCMD_PIPE_RESOURCE_SET_TYPE, 0, VIRGL_PIPE_RES_SET_TYPE_SIZE(1));
            cmdBody[1] = src->GetId();
            cmdBody[2] = srcBlobInfo.format;
            cmdBody[3] = VIRGL_BIND_RENDER_TARGET | /*VIRGL_BIND_LINEAR |*/ VIRGL_BIND_SHARED;
            cmdBody[4] = srcBlobInfo.width;
            cmdBody[5] = srcBlobInfo.height;
            cmdBody[6] = 0; // usage seems to be ignored
            cmdBody[7] = (UINT)(srcBlobModifier & 0xFFFFFFFFull);
            cmdBody[8] = (UINT)(srcBlobModifier >> 32);
            cmdBody[9] = srcBlobInfo.strides[0];
            cmdBody[10] = srcBlobInfo.offsets[0];
    }

    if (dst->IsBlob())
    {

            VIOGPU_COMMAND_HDR *cmd_hdr = (VIOGPU_COMMAND_HDR *)dmaBuf;
            cmd_hdr->type = VIOGPU_CMD_SUBMIT;
            cmd_hdr->size = sizeOfSetType;
            cmd_hdr->flags = 0;
            cmd_hdr->ring_idx = 0;
            dmaBuf += sizeof(VIOGPU_COMMAND_HDR);

            if (!m_Context.IsVirgl())
            {
                cmd_hdr->flags |= VIOGPU_EXECBUF_VIRGL;
            }

            UINT *cmdBody = (UINT *)dmaBuf;
            dmaBuf += sizeOfSetType;

            cmdBody[0] = VIRGL_CMD0(VIRGL_CCMD_PIPE_RESOURCE_SET_TYPE, 0, VIRGL_PIPE_RES_SET_TYPE_SIZE(1));
            cmdBody[1] = dst->GetId();
            cmdBody[2] = dstBlobInfo.format;
            cmdBody[3] = VIRGL_BIND_RENDER_TARGET | /*VIRGL_BIND_LINEAR |*/ VIRGL_BIND_SHARED;
            cmdBody[4] = dstBlobInfo.width;
            cmdBody[5] = dstBlobInfo.height;
            cmdBody[6] = 0; // usage seems to be ignored
            cmdBody[7] = (UINT)(dstBlobModifier & 0xFFFFFFFFull);
            cmdBody[8] = (UINT)(dstBlobModifier >> 32);
            cmdBody[9] = dstBlobInfo.strides[0];
            cmdBody[10] = dstBlobInfo.offsets[0];
    }

    {
        UINT rectCnt = copyRectCnt;

        VIOGPU_COMMAND_HDR *cmd_hdr = (VIOGPU_COMMAND_HDR *)dmaBuf;
        cmd_hdr->type = VIOGPU_CMD_SUBMIT;
        cmd_hdr->size = rectCnt * sizeOfOneRect;
        cmd_hdr->flags = 0;
        cmd_hdr->ring_idx = 0;
        dmaBuf += sizeof(VIOGPU_COMMAND_HDR);

        if (!m_Context.IsVirgl()) {
            cmd_hdr->flags |= VIOGPU_EXECBUF_VIRGL;
        }

        for (UINT i = 0; i < rectCnt; i++)
        {
            UINT *cmdBody = (UINT *)dmaBuf;
            dmaBuf += sizeOfOneRect;

            RECT rect = copyRects[i];

            cmdBody[0] = VIRGL_CMD0(VIRGL_CCMD_RESOURCE_COPY_REGION, 0, VIRGL_CMD_RESOURCE_COPY_REGION_SIZE);
            cmdBody[1] = dst->GetId();
            cmdBody[2] = 0;
            cmdBody[3] = rect.left;
            cmdBody[4] = rect.top;
            cmdBody[5] = 0;

            cmdBody[6] = src->GetId();
            cmdBody[7] = 0;
            cmdBody[8] = rect.left + (LONG)copyDx64;
            cmdBody[9] = rect.top + (LONG)copyDy64;
            cmdBody[10] = 0;
            cmdBody[11] = rect.right - rect.left;
            cmdBody[12] = rect.bottom - rect.top;
            cmdBody[13] = 1;
        }
    }

    if (dst->IsCoherent())
    {
        ULONG transferStride;
        ULONGLONG transferOffset;
        if (!dst->GetTransferLayout(coverRect.left, coverRect.top, &transferStride, &transferOffset))
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("<--> %s invalid destination transfer layout res_id=%d x=%ld y=%ld\n",
                      __FUNCTION__, dst->GetId(), coverRect.left, coverRect.top));
            return STATUS_INVALID_PARAMETER;
        }

        VIOGPU_COMMAND_HDR *cmd_hdr = (VIOGPU_COMMAND_HDR *)dmaBuf;
        cmd_hdr->type = VIOGPU_CMD_TRANSFER_FROM_HOST;
        cmd_hdr->size = sizeof(VIOGPU_TRANSFER_CMD);
        cmd_hdr->flags = 0;
        cmd_hdr->ring_idx = 0;
        dmaBuf += sizeof(VIOGPU_COMMAND_HDR);

        if (!m_Context.IsVirgl()) {
            cmd_hdr->flags |= VIOGPU_EXECBUF_VIRGL;
        }

        VIOGPU_TRANSFER_CMD *cmdBody = (VIOGPU_TRANSFER_CMD *)dmaBuf;
        dmaBuf += sizeof(VIOGPU_TRANSFER_CMD);

        cmdBody->res_id = dst->GetId();

        cmdBody->box.x = coverRect.left;
        cmdBody->box.y = coverRect.top;
        cmdBody->box.z = 0;
        cmdBody->box.width = coverRect.right - coverRect.left;
        cmdBody->box.height = coverRect.bottom - coverRect.top;
        cmdBody->box.depth = 1;

        cmdBody->layer_stride = 0;
        cmdBody->stride = transferStride;
        cmdBody->level = 0;
        cmdBody->offset = transferOffset;
    }

    pPresent->pDmaBuffer = dmaBuf;

    if (fullPrimaryBootstrap)
    {
        *pInitialPresentCompletion = dst;
    }

    return STATUS_SUCCESS;
}

NTSTATUS VioGpuDevice::GenerateBltPresentUM(DXGKARG_PRESENT *pPresent, VioGpuAllocation *src, VioGpuAllocation *dst)
{
    UNREFERENCED_PARAMETER(src);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--> %s\n", __FUNCTION__));

    if (!CanBlit()) {
        DbgPrint(TRACE_LEVEL_FATAL, ("<--> %s Invoke VIOGPU_BLIT_INIT escape first\n", __FUNCTION__));
        return STATUS_INVALID_PARAMETER;
    }

    VIOGPU_BLIT_PRESENT blit;

    __try
    {
        ProbeForRead(m_pBlit, sizeof(blit), sizeof(ULONG));
        RtlCopyMemory(&blit, m_pBlit, sizeof(blit));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        DbgPrint(TRACE_LEVEL_FATAL, ("---> %s: Failed to copy from user\n", __FUNCTION__));
        return STATUS_INVALID_PARAMETER;
    }

    // Calculate rect covering all SubRectx
    RECT coverRect = pPresent->pDstSubRects[0];
    for (UINT i = 1; i < pPresent->SubRectCnt; i++)
    {
        coverRect.top = min(coverRect.top, pPresent->pDstSubRects[i].top);
        coverRect.left = min(coverRect.left, pPresent->pDstSubRects[i].left);
        coverRect.right = max(coverRect.right, pPresent->pDstSubRects[i].right);
        coverRect.bottom = max(coverRect.bottom, pPresent->pDstSubRects[i].bottom);
    }

    if (dst->IsBlob()) {
        blit.dst.alloc.Type = VIOGPU_RESOURCE_TYPE_BLOB;
        blit.dst.alloc.OptionsBlob = dst->m_Blob.Options;
        blit.dst.alloc.Size = dst->m_Size;
    } else {
        blit.dst.alloc.Type = VIOGPU_RESOURCE_TYPE_3D;
        blit.dst.alloc.Options3D = dst->m_3dOptions;
        blit.dst.alloc.Size = dst->m_Size;
    }
    dst->EscapeResourceInfo(&blit.dst.res_info);

    INT dx = pPresent->SrcRect.left - pPresent->DstRect.left;
    INT dy = pPresent->SrcRect.top - pPresent->DstRect.top;

    for (UINT i = 0; i < pPresent->SubRectCnt; i++)
    {
        KeClearEvent(m_hKM);
        KeClearEvent(m_hUM);

        RECT rect = pPresent->pDstSubRects[i];

        blit.src.rect.left = rect.left + dx;
        blit.src.rect.right = rect.right + dx;
        blit.src.rect.top = rect.top + dy;
        blit.src.rect.bottom = rect.bottom + dy;
        blit.dst.rect = rect;

        DbgPrint(TRACE_LEVEL_INFORMATION,
                 ("---> %s: DstRect = {.left = %ld, .top = %ld, .right = %ld, .bottom = %ld}\n",
                  __FUNCTION__,
                  blit.dst.rect.left,
                  blit.dst.rect.top,
                  blit.dst.rect.right,
                  blit.dst.rect.bottom));

        DbgPrint(TRACE_LEVEL_INFORMATION,
                 ("---> %s: SrcRect = {.left = %ld, .top = %ld, .right = %ld, .bottom = %ld}, dx = %d, dy = %d\n",
                  __FUNCTION__,
                  blit.src.rect.left,
                  blit.src.rect.top,
                  blit.src.rect.right,
                  blit.src.rect.bottom,
                  dx, dy));

        __try
        {
            ProbeForWrite(m_pBlit, sizeof(blit), sizeof(ULONG));
            RtlCopyMemory(m_pBlit, &blit, sizeof(blit));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            DbgPrint(TRACE_LEVEL_FATAL, ("---> %s: Failed to copy to user\n", __FUNCTION__));
            return STATUS_INVALID_PARAMETER;
        }

        /* Blit information set up complete */
        KeSetEvent(m_hUM, IO_NO_INCREMENT, FALSE);
        DbgPrint(TRACE_LEVEL_ERROR, ("---> %s: waiting for blit from userspace %p / %p\n", __FUNCTION__, m_hUM, m_hKM));

        LARGE_INTEGER timeout = {0};
        timeout.QuadPart = Int32x32To64(10000, -10000);
        /* Waiting for userspace to perform blit */
        if (!NT_SUCCESS(KeWaitForSingleObject(m_hKM, Executive, KernelMode, FALSE, &timeout))) {
            DbgPrint(TRACE_LEVEL_FATAL, ("---> %s: TIMEOUT waiting for blit from userspace\n", __FUNCTION__));
            break;
        }
        /* Blit done */
    }

    KeClearEvent(m_hUM);
    KeClearEvent(m_hKM);

    if (dst->IsCoherent())
    {
        ULONG transferStride;
        ULONGLONG transferOffset;
        if (!dst->GetTransferLayout(coverRect.left, coverRect.top, &transferStride, &transferOffset))
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("<--> %s invalid destination transfer layout res_id=%d x=%ld y=%ld\n",
                      __FUNCTION__, dst->GetId(), coverRect.left, coverRect.top));
            return STATUS_INVALID_PARAMETER;
        }

        UCHAR *dmaBuf = (UCHAR *)pPresent->pDmaBuffer;

        VIOGPU_COMMAND_HDR *cmd_hdr = (VIOGPU_COMMAND_HDR *)dmaBuf;
        cmd_hdr->type = VIOGPU_CMD_TRANSFER_FROM_HOST;
        cmd_hdr->size = sizeof(VIOGPU_TRANSFER_CMD);
        cmd_hdr->flags = 0;
        cmd_hdr->ring_idx = 0;
        dmaBuf += sizeof(VIOGPU_COMMAND_HDR);

        VIOGPU_TRANSFER_CMD *cmdBody = (VIOGPU_TRANSFER_CMD *)dmaBuf;
        dmaBuf += sizeof(VIOGPU_TRANSFER_CMD);

        cmdBody->res_id = dst->GetId();

        cmdBody->box.x = coverRect.left;
        cmdBody->box.y = coverRect.top;
        cmdBody->box.z = 0;
        cmdBody->box.width = coverRect.right - coverRect.left;
        cmdBody->box.height = coverRect.bottom - coverRect.top;
        cmdBody->box.depth = 1;

        cmdBody->layer_stride = 0;
        cmdBody->stride = transferStride;
        cmdBody->level = 0;
        cmdBody->offset = transferOffset;

        pPresent->pDmaBuffer = dmaBuf;
    }
    else
    {
        VIOGPU_COMMAND_HDR *cmd_hdr = (VIOGPU_COMMAND_HDR *)pPresent->pDmaBuffer;
        cmd_hdr->type = VIOGPU_CMD_NOP;
        cmd_hdr->size = 0;
        cmd_hdr->flags = 0;
        cmd_hdr->ring_idx = 0;
        pPresent->pDmaBuffer = (char *)pPresent->pDmaBuffer + sizeof(VIOGPU_COMMAND_HDR);
    }

    return STATUS_SUCCESS;
}

NTSTATUS VioGpuDevice::Present(_Inout_ DXGKARG_PRESENT *pPresent)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--> %s\n", __FUNCTION__));

    if (pPresent == NULL || pPresent->pAllocationList == NULL ||
        ((pPresent->DmaSize != 0) && (pPresent->pDmaBuffer == NULL)) ||
        ((pPresent->pDmaBuffer != NULL) &&
         (pPresent->pDmaBufferPrivateData == NULL ||
          pPresent->DmaBufferPrivateDataSize < sizeof(void *))) ||
        ((pPresent->pDmaBufferPrivateData != NULL) &&
         pPresent->DmaBufferPrivateDataSize < sizeof(void *)) ||
        ((pPresent->PatchLocationListOutSize != 0) &&
         pPresent->pPatchLocationListOut == NULL) ||
        ((pPresent->SubRectCnt != 0) && pPresent->pDstSubRects == NULL))
    {
        return STATUS_INVALID_PARAMETER;
    }

    // DMA buffers (and their private-data area) are RECYCLED by dxgkrnl.
    // Every path below that returns without storing a VioGpuCommand*
    // (windowed flips with no present bytes, error exits) would leave a
    // STALE pointer from the buffer's previous user; SubmitCommand then
    // resurrects that command and PrepareSubmit clobbers its fence id —
    // fences complete wrongly or never (intermittent TDR at first
    // windowed flip). NULL it up front; real writers below overwrite.
    if (pPresent->pDmaBufferPrivateData)
    {
        *(void **)pPresent->pDmaBufferPrivateData = NULL;
    }

    if (pPresent->Flags.Flip)
    {
        if (pPresent->Flags.Value != 0x4)
        {
            return STATUS_NOT_SUPPORTED;
        }
        // DMA flips must switch scanout when their ordered command retires.
        // SetVidPnSourceAddress only executes MMIO flips; Vista's caps here
        // deliberately select the DMA path. Never latch during translation.
        VioGpuAllocation *srcAlloc = NULL;
        DXGK_ALLOCATIONLIST *dxgk_src = &pPresent->pAllocationList[DXGK_PRESENT_SOURCE_INDEX];
        if (dxgk_src->hDeviceSpecificAllocation)
        {
            VioGpuDeviceAllocation *srcDev =
                VioGpuDeviceAllocation::FromHandle(dxgk_src->hDeviceSpecificAllocation);
            if (srcDev == NULL || srcDev->GetDevice() != this)
            {
                return STATUS_INVALID_PARAMETER;
            }
            srcAlloc = srcDev->GetAllocation();
            if (srcAlloc == NULL || !srcAlloc->IsPrimary())
            {
                return STATUS_INVALID_PARAMETER;
            }
        }
        else
        {
            return STATUS_INVALID_PARAMETER;
        }

        // Vista's checked SubmitPresent requires a successful Present to
        // consume DMA bytes, including flips. Queue a real scheduler packet
        // whose retirement latches the requested source for the display thread.
        if (pPresent->pDmaBuffer == NULL ||
            pPresent->DmaSize < sizeof(VIOGPU_COMMAND_HDR) ||
            pPresent->PatchLocationListOutSize < 1 ||
            pPresent->pPatchLocationListOut == NULL)
        {
            pPresent->MultipassOffset = 0;
            return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
        }
        VioGpuCommand *cmd = new (VIOGPU_NONPAGED_POOL)
            VioGpuCommand(m_pAdapter, this);
        if (cmd == NULL)
            return STATUS_NO_MEMORY;
        NTSTATUS status = cmd->AttachAllocations(
            pPresent->pAllocationList, DXGK_PRESENT_SOURCE_INDEX + 1, this);
        if (!NT_SUCCESS(status))
        {
            delete cmd;
            return status;
        }
        if (dxgk_src->SegmentId != 0)
            srcAlloc->m_SegmentAddress = dxgk_src->PhysicalAddress;
        cmd->SetScanoutSourceCompletion(srcAlloc,
            m_pAdapter->vidpn.GetScanoutSourceGeneration(), TRUE);
        cmd->SetDmaBuf((char *)pPresent->pDmaBuffer);
        VIOGPU_COMMAND_HDR *packet =
            (VIOGPU_COMMAND_HDR *)pPresent->pDmaBuffer;
        RtlZeroMemory(packet, sizeof(*packet));
        packet->type = VIOGPU_CMD_NOP;
        D3DDDI_PATCHLOCATIONLIST *patch = pPresent->pPatchLocationListOut;
        RtlZeroMemory(patch, sizeof(*patch));
        patch->AllocationIndex = DXGK_PRESENT_SOURCE_INDEX;
        patch->DriverId = 1;
        patch->SlotId = 1;
        *(void **)pPresent->pDmaBufferPrivateData = cmd->ToHandle();
        pPresent->pDmaBuffer = (UCHAR *)pPresent->pDmaBuffer + sizeof(*packet);
        pPresent->pPatchLocationListOut = patch + 1;
        return STATUS_SUCCESS;
    }

    DbgPrint(TRACE_LEVEL_VERBOSE,
             ("<---> %s Flags=(%s %s %s %s %s %s %s %s)\n",
              __FUNCTION__,
              pPresent->Flags.Blt ? "Blt" : "",
              pPresent->Flags.ColorFill ? "ColorFill" : "",
              pPresent->Flags.Flip ? "Flip" : "",
              pPresent->Flags.FlipWithNoWait ? "FlipWithNoWait" : "",
              pPresent->Flags.SrcColorKey ? "SrcColorKey" : "",
              pPresent->Flags.DstColorKey ? "DstColorKey" : "",
              pPresent->Flags.LinearToSrgb ? "LinearToSrgb" : "",
              pPresent->Flags.Rotate ? "Rotate" : ""));

    if (!pPresent->Flags.Blt || pPresent->Flags.Value != 0x1 ||
        pPresent->pDmaBuffer == NULL || pPresent->DmaSize == 0 ||
        pPresent->SubRectCnt == 0 || pPresent->pDstSubRects == NULL)
    {
        return STATUS_NOT_SUPPORTED;
    }

    // DxgkDdiPresent must report every allocation reference in the output
    // patch list, including prepatched references. Do not generate a partial
    // list when dxgkrnl supplies too little space.
    const UINT requiredPatchEntries =
        (pPresent->pAllocationList[DXGK_PRESENT_SOURCE_INDEX]
                 .hDeviceSpecificAllocation != NULL
             ? 1u
             : 0u) +
        (pPresent->pAllocationList[DXGK_PRESENT_DESTINATION_INDEX]
                 .hDeviceSpecificAllocation != NULL
             ? 1u
             : 0u);
    if (requiredPatchEntries > pPresent->PatchLocationListOutSize ||
        (requiredPatchEntries != 0 && pPresent->pPatchLocationListOut == NULL))
    {
        return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    }

    DXGK_ALLOCATIONLIST *dxgk_src =
        &pPresent->pAllocationList[DXGK_PRESENT_SOURCE_INDEX];
    DXGK_ALLOCATIONLIST *dxgk_dst =
        &pPresent->pAllocationList[DXGK_PRESENT_DESTINATION_INDEX];
    VioGpuDeviceAllocation *src =
        VioGpuDeviceAllocation::FromHandle(dxgk_src->hDeviceSpecificAllocation);
    VioGpuDeviceAllocation *dst =
        VioGpuDeviceAllocation::FromHandle(dxgk_dst->hDeviceSpecificAllocation);
    if (src == NULL || dst == NULL ||
        src->GetDevice() != this || dst->GetDevice() != this)
    {
        return STATUS_INVALID_PARAMETER;
    }

    VioGpuCommand *cmd = new (VIOGPU_NONPAGED_POOL) VioGpuCommand(m_pAdapter, this);
    if (!cmd)
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("%s VioGpuCommand allocation failed\n", __FUNCTION__));
        return STATUS_NO_MEMORY;
    }
    void **privateData = (void **)pPresent->pDmaBufferPrivateData;
    *privateData = cmd->ToHandle();

    cmd->SetDmaBuf((char *)pPresent->pDmaBuffer);

    D3DDDI_PATCHLOCATIONLIST *out = pPresent->pPatchLocationListOut;
    RtlZeroMemory(out, sizeof(*out));
    out->AllocationIndex = DXGK_PRESENT_SOURCE_INDEX;
    out->DriverId = 1;
    out->SlotId = 1;
    ++out;
    RtlZeroMemory(out, sizeof(*out));
    out->AllocationIndex = DXGK_PRESENT_DESTINATION_INDEX;
    out->DriverId = 2;
    out->SlotId = 2;
    pPresent->pPatchLocationListOut = out + 1;

    // Register the source/destination in the driver's in-flight set
    // (m_busy via MarkBusy) so EscapeResourceBusy from DxgkDdiDestroyAllocation
    // sees Present-attached work, matching Render. Present's pAllocationList
    // is a fixed-size array with index 0 reserved and source/destination at
    // DXGK_PRESENT_SOURCE_INDEX (1) / DXGK_PRESENT_DESTINATION_INDEX (2);
    // there is no separate length field, so the count is
    // DXGK_PRESENT_MAX_INDEX + 1.
    NTSTATUS attachStatus = cmd->AttachAllocations(pPresent->pAllocationList,
                                                   DXGK_PRESENT_MAX_INDEX + 1,
                                                   this);
    if (!NT_SUCCESS(attachStatus))
    {
        if (pPresent->pDmaBufferPrivateData)
        {
            VioGpuCommand **privateData = (VioGpuCommand **)pPresent->pDmaBufferPrivateData;
            if (*privateData == cmd)
            {
                *privateData = NULL;
            }
        }
        delete cmd;
        return attachStatus;
    }


    if (pPresent->Flags.Blt)
    {
        VioGpuAllocation *initialPresentCompletion = NULL;
        NTSTATUS presentStatus = GenerateBltPresent(pPresent,
                                                    src,
                                                    dst,
                                                    &initialPresentCompletion);
        if (!NT_SUCCESS(presentStatus))
        {
            if (*privateData == cmd)
                *privateData = NULL;
            delete cmd;
            return presentStatus;
        }
        if (initialPresentCompletion != NULL)
        {
            cmd->SetInitialPresentCompletion(initialPresentCompletion);
        }

        // A DWM host surface presented to the standard primary is the real
        // scanout source.  The standard primary remains the legacy GDI
        // target; treating it as a CPU copy destination loses the host
        // texture identity.  Keep the existing primary-source behavior for
        // flip-model clients.
        if (src)
        {
            VioGpuAllocation *srcAlloc = src->GetAllocation();
            VioGpuAllocation *dstAlloc = dst ? dst->GetAllocation() : NULL;
            UINT srcWidth = 0, srcHeight = 0, dstWidth = 0, dstHeight = 0;
            const BOOLEAN directHostScanout =
                srcAlloc && dstAlloc && dstAlloc->IsPrimary() &&
                !dstAlloc->IsBlob() && srcAlloc->IsHostPresentationSurface() &&
                srcAlloc->GetDimensions(&srcWidth, &srcHeight) &&
                dstAlloc->GetDimensions(&dstWidth, &dstHeight) &&
                srcWidth == dstWidth && srcHeight == dstHeight;
            if (directHostScanout)
            {
                // The NOP emitted by GenerateBltPresent still has a real
                // scheduler fence. Switch the visible source only from that
                // command's successful retirement path.
                cmd->SetScanoutSourceCompletion(
                    srcAlloc, m_pAdapter->vidpn.GetScanoutSourceGeneration());
            }
        }
        return STATUS_SUCCESS;
    }

    // Only Blt and Flip are implemented in Present. Returning
    // NOT_SUPPORTED for ColorFill / SrcColorKey / DstColorKey /
    // LinearToSrgb / Rotate / FlipWithNoWait lets the UMD fall back
    // to a Render-based path; a silent NOP + SUCCESS would not.
    DbgPrint(TRACE_LEVEL_INFORMATION,
             ("%s unsupported Present flags=0x%x\n", __FUNCTION__,
              pPresent->Flags.Value));
    if (pPresent->pDmaBufferPrivateData)
    {
        VioGpuCommand **privateData = (VioGpuCommand **)pPresent->pDmaBufferPrivateData;
        if (*privateData == cmd)
        {
            *privateData = NULL;
        }
    }
    delete cmd;
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS VioGpuDevice::Render(DXGKARG_RENDER *pRender)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));

    static const SIZE_T VIOGPU_CONTEXT_DMA_BUFFER_SIZE = 1024 * 1024;

    if ((pRender == NULL) ||
        ((pRender->CommandLength != 0) && (pRender->pCommand == NULL)) ||
        ((pRender->DmaSize != 0) && (pRender->pDmaBuffer == NULL)) ||
        ((pRender->CommandLength != pRender->MultipassOffset) &&
         (pRender->pDmaBufferPrivateData == NULL ||
          pRender->DmaBufferPrivateDataSize < sizeof(void *))) ||
        ((pRender->AllocationListSize != 0) &&
         (pRender->pAllocationList == NULL)) ||
        (pRender->MultipassOffset > pRender->CommandLength) ||
        ((pRender->PatchLocationListInSize != 0) &&
         (pRender->pPatchLocationListIn == NULL)) ||
        ((pRender->PatchLocationListOutSize != 0) &&
         (pRender->pPatchLocationListOut == NULL)) ||
        (pRender->PatchLocationListInSize > pRender->PatchLocationListOutSize))
    {
        return STATUS_INVALID_PARAMETER;
    }

    // See Present: recycled DMA private data must never carry a stale
    // command pointer into SubmitCommand (multipass/error exits below
    // return before the real write).
    if (pRender->pDmaBufferPrivateData)
    {
        *(void **)pRender->pDmaBufferPrivateData = NULL;
    }

    char *pDmaBufStart = (char *)pRender->pDmaBuffer;
    // The event object is referenced while DxgkDdiRender still runs in the
    // submitting process.  Do not retain the raw handle from the UMD packet.
    PKEVENT renderEvent = NULL;
    NTSTATUS renderStatus = STATUS_SUCCESS;
    BOOLEAN copiedPacket = FALSE;

    __try
    {
        // Bound on PatchLocationListOutSize (the OUT capacity) so a UMD
        // supplying more IN entries than OUT slots cannot overrun the
        // kernel buffer. The count of populated entries is signalled to
        // DxgK by advancing pPatchLocationListOut, per the DDI.
        UINT cPatch = pRender->PatchLocationListInSize;
        for (UINT i = 0; i < cPatch; i++)
        {
            D3DDDI_PATCHLOCATIONLIST *out = &pRender->pPatchLocationListOut[0];
            RtlZeroMemory(out, sizeof(*out));
            out->AllocationIndex = pRender->pPatchLocationListIn[i].AllocationIndex;
            out->SlotId = i;
            pRender->pPatchLocationListOut++;
        }

        unsigned char *dmaBuf = (unsigned char *)pRender->pDmaBuffer;
        unsigned char *endDmaBuf = dmaBuf + pRender->DmaSize;
        unsigned char *commandBase = (unsigned char *)pRender->pCommand;
        unsigned char *cmdBuf = commandBase + pRender->MultipassOffset;
        unsigned char *endBuf = commandBase + pRender->CommandLength;
        while (cmdBuf < endBuf)
        {
            SIZE_T inputRemaining = (SIZE_T)(endBuf - cmdBuf);
            SIZE_T outputRemaining = (SIZE_T)(endDmaBuf - dmaBuf);
            if (inputRemaining < sizeof(VIOGPU_COMMAND_HDR))
            {
                return STATUS_INVALID_USER_BUFFER;
            }

            VIOGPU_COMMAND_HDR inputHeader;
            memcpy(&inputHeader, cmdBuf, sizeof(inputHeader));
            if ((SIZE_T)inputHeader.size > inputRemaining - sizeof(inputHeader))
            {
                return STATUS_INVALID_USER_BUFFER;
            }

            SIZE_T packetSize = sizeof(inputHeader) + (SIZE_T)inputHeader.size;
            if (packetSize > outputRemaining)
            {
                pRender->pDmaBuffer = dmaBuf;
                pRender->MultipassOffset = (UINT)(cmdBuf - commandBase);

                // A packet can fail to fit only because dxgkrnl handed us
                // the tail of its current DMA buffer. Ask for a fresh
                // buffer without submitting an empty command. A packet
                // larger than the size advertised in CreateContext can
                // never make progress and must fail instead of retrying
                // forever.
                if (!copiedPacket)
                {
                    if (packetSize > VIOGPU_CONTEXT_DMA_BUFFER_SIZE)
                    {
                        return STATUS_INVALID_BUFFER_SIZE;
                    }
                    return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
                }

                // The copied prefix is a complete DMA submission. Build
                // its private command below so SubmitCommand can schedule
                // it before dxgkrnl calls Render again at MultipassOffset.
                renderStatus = STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
                break;
            }

            unsigned char *inputBody = cmdBuf + sizeof(inputHeader);
            switch (inputHeader.type)
            {
                case VIOGPU_CMD_NOP:
                    if ((inputHeader.size != 0) || (inputHeader.flags != 0) ||
                        (inputHeader.ring_idx != 0))
                    {
                        return STATUS_INVALID_USER_BUFFER;
                    }
                    break;

                case VIOGPU_CMD_SUBMIT:
                    if ((inputHeader.flags & ~(VIOGPU_EXECBUF_RING_IDX | VIOGPU_EXECBUF_VIRGL)) != 0 ||
                        (!(inputHeader.flags & VIOGPU_EXECBUF_RING_IDX) && inputHeader.ring_idx != 0))
                    {
                        return STATUS_INVALID_USER_BUFFER;
                    }
                    if (inputHeader.flags & VIOGPU_EXECBUF_RING_IDX)
                    {
                        const VioGpuContext *context =
                            (inputHeader.flags & VIOGPU_EXECBUF_VIRGL) ? &m_Virgl : &m_Context;
                        if (context->IsEmpty() || inputHeader.ring_idx >= context->GetNumRings())
                        {
                            return STATUS_INVALID_USER_BUFFER;
                        }
                    }
                    break;

                case VIOGPU_CMD_TRANSFER_TO_HOST:
                case VIOGPU_CMD_TRANSFER_FROM_HOST:
                    if ((inputHeader.size != sizeof(VIOGPU_TRANSFER_CMD)) ||
                        (inputHeader.flags & ~VIOGPU_EXECBUF_VIRGL) != 0 ||
                        (inputHeader.ring_idx != 0))
                    {
                        return STATUS_INVALID_USER_BUFFER;
                    }
                    break;

                case VIOGPU_CMD_MAP_BLOB:
                case VIOGPU_CMD_UNMAP_BLOB:
                    if ((inputHeader.size == 0) ||
                        ((inputHeader.size % sizeof(ULONG)) != 0) ||
                        (inputHeader.flags != 0) || (inputHeader.ring_idx != 0))
                    {
                        return STATUS_INVALID_USER_BUFFER;
                    }
                    break;

                case VIOGPU_CMD_SIGNAL_EVENT:
                {
                    // This marker is terminal.  Otherwise it acknowledges
                    // only a prefix of a DMA submission.
                    if ((inputHeader.size != sizeof(VIOGPU_SIGNAL_EVENT_CMD)) ||
                        (inputHeader.flags != 0) || (inputHeader.ring_idx != 0) ||
                        (cmdBuf + packetSize != endBuf) || (renderEvent != NULL))
                    {
                        return STATUS_INVALID_USER_BUFFER;
                    }

                    VIOGPU_SIGNAL_EVENT_CMD signal;
                    memcpy(&signal, inputBody, sizeof(signal));
                    if (signal.Event == 0)
                    {
                        return STATUS_INVALID_HANDLE;
                    }

                    NTSTATUS eventStatus = ObReferenceObjectByHandle(
                        VioGpuUmHandleValue(signal.Event),
                        SYNCHRONIZE | EVENT_MODIFY_STATE,
                        *ExEventObjectType,
                        UserMode,
                        (PVOID *)&renderEvent,
                        NULL);
                    if (!NT_SUCCESS(eventStatus))
                    {
                        DbgPrint(TRACE_LEVEL_WARNING,
                                 ("%s invalid render-event handle 0x%llx status=0x%X\n",
                                  __FUNCTION__, signal.Event, eventStatus));
                        return eventStatus;
                    }
                    break;
                }

                case VIOGPU_CMD_COPY_FIXED_PRIMARY:
                case VIOGPU_CMD_FLUSH_FIXED_PRIMARY:
                    // This is emitted only by the kernel's GDI Present path.
                    // Never accept allocation indices, copy rectangles, or a
                    // scanout flush from an untrusted UMD command stream.
                    return STATUS_INVALID_USER_BUFFER;

                default:
                    return STATUS_INVALID_USER_BUFFER;
            }

            // Commit the output only after the complete packet passes all
            // size, type, flag, and payload checks.
            memcpy(dmaBuf, cmdBuf, packetSize);
            dmaBuf += packetSize;
            cmdBuf += packetSize;
            copiedPacket = TRUE;
        }
        pRender->pDmaBuffer = dmaBuf;
        pRender->MultipassOffset = (UINT)(cmdBuf - commandBase);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        if (renderEvent != NULL)
        {
            KeSetEvent(renderEvent, IO_NO_INCREMENT, FALSE);
            ObDereferenceObject(renderEvent);
        }
        DbgPrint(TRACE_LEVEL_WARNING, ("<---> %s Usermode copy exception", __FUNCTION__));
        return STATUS_INVALID_PARAMETER;
    }

    // An empty Render needs no side-band object. SubmitCommand creates the
    // scheduler-only command for the empty DMA range.
    if (!copiedPacket)
    {
        return renderStatus;
    }

    VioGpuCommand *cmd = new (VIOGPU_NONPAGED_POOL) VioGpuCommand(m_pAdapter, this);
    if (!cmd)
    {
        if (renderEvent != NULL)
        {
            KeSetEvent(renderEvent, IO_NO_INCREMENT, FALSE);
            ObDereferenceObject(renderEvent);
        }
        DbgPrint(TRACE_LEVEL_ERROR, ("%s VioGpuCommand allocation failed\n", __FUNCTION__));
        return STATUS_NO_MEMORY;
    }
    if (pRender->pDmaBufferPrivateData)
    {
        void **privateData = (void **)pRender->pDmaBufferPrivateData;
        *privateData = cmd->ToHandle();
    }
    cmd->SetDmaBuf(pDmaBufStart);
    if (renderEvent != NULL)
    {
        cmd->SetRenderEvent(renderEvent);
        renderEvent = NULL;
    }
    NTSTATUS attachStatus = cmd->AttachAllocations(pRender->pAllocationList,
                                                    pRender->AllocationListSize,
                                                    this);
    if (!NT_SUCCESS(attachStatus))
    {
        if (pRender->pDmaBufferPrivateData)
        {
            *(void **)pRender->pDmaBufferPrivateData = NULL;
        }
        delete cmd;
        return attachStatus;
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    return renderStatus;
};

NTSTATUS VioGpuDevice::OpenAllocation(_In_ CONST DXGKARG_OPENALLOCATION *pOpenAllocation)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    if (pOpenAllocation == NULL ||
        (pOpenAllocation->NumAllocations != 0 &&
         pOpenAllocation->pOpenAllocation == NULL))
    {
        return STATUS_INVALID_PARAMETER;
    }

    for (UINT i = 0; i < pOpenAllocation->NumAllocations; i++)
    {
        pOpenAllocation->pOpenAllocation[i].hDeviceSpecificAllocation = NULL;
    }

    for (UINT i = 0; i < pOpenAllocation->NumAllocations; i++)
    {
        DXGK_OPENALLOCATIONINFO *openAllocationInfo = &pOpenAllocation->pOpenAllocation[i];
        VioGpuAllocation *allocation = m_pAdapter->AllocationFromHandle(openAllocationInfo->hAllocation);
        NTSTATUS openStatus = allocation ? STATUS_SUCCESS : STATUS_INVALID_HANDLE;
        VioGpuDeviceAllocation *devAlloc =
            allocation ? allocation->Open(this, &openStatus) : NULL;
        if (devAlloc == NULL)
        {
            for (UINT rollback = 0; rollback < i; rollback++)
            {
                DXGK_OPENALLOCATIONINFO *opened =
                    &pOpenAllocation->pOpenAllocation[rollback];
                VioGpuDeviceAllocation *openedDev =
                    VioGpuDeviceAllocation::FromHandle(
                        opened->hDeviceSpecificAllocation);
                if (openedDev != NULL)
                {
                    VioGpuAllocation *openedAllocation = openedDev->GetAllocation();
                    if (openedAllocation != NULL)
                        openedAllocation->Close(openedDev);
                }
                opened->hDeviceSpecificAllocation = NULL;
            }
            return openStatus;
        }
        openAllocationInfo->hDeviceSpecificAllocation = devAlloc->ToHandle();
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return STATUS_SUCCESS;
}

CtrlQueue *VioGpuDevice::GetCtrlQueue()
{
    PAGED_CODE();

    return &m_pAdapter->ctrlQueue;
}

VioGpuDeviceAllocation::VioGpuDeviceAllocation(VioGpuDevice *device, VioGpuAllocation *allocation)
{
    PAGED_CODE();

    //auto lock_guard = allocation->LockGuard();

    m_pAllocation = allocation;
    m_pDevice = device;
    m_RefCount = 1;
    m_Status = STATUS_SUCCESS;
    m_attached = false;

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s res_id=%d ctx=%p\n",
                                   __FUNCTION__,
                                   allocation->GetId(),
                                   device->m_Context.GetId()));

    if (m_pAllocation->IsBlob() && !m_pAllocation->IsCreated())
    {
        // Shared-texture blobs bind on the UMD transport context that
        // staged the pending dmabuf export (m_CreateCtxId); transport
        // shmem blobs (0) bind on the opening device's own context.
        UINT create_ctx = m_pAllocation->m_CreateCtxId
                              ? m_pAllocation->m_CreateCtxId
                              : m_pDevice->m_Context.GetId();
        DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s res_id=%d ctx_id=%d capset=%d blob_id=%llu creating blob resource\n",
                                       __FUNCTION__,
                                       allocation->GetId(),
                                       create_ctx,
                                       device->m_Context.GetCapset(),
                                       allocation->m_Blob.Options.blob_id));
        bool ok = m_pDevice->GetCtrlQueue()->CreateResourceBlob(m_pAllocation->GetId(), create_ctx, &m_pAllocation->m_Blob.Options, m_pAllocation->m_Size);
        m_pAllocation->m_Blob.Created = ok;
        if (!ok)
        {
            m_Status = STATUS_DEVICE_NOT_READY;
            m_AttachedToVirgl = false;
            return;
        }
    }

    // Attach the resource to the opening device's virtio context.  For a
    // cross-process open of a shared blob this is what forwards the host
    // dmabuf into the opener's render worker (proxy attach-forwarding).
    // A device that never issued VIOGPU_CTX_INIT has no host context to
    // attach to (the UMD's transport-context import rig covers the open
    // in that case); skip rather than name a nonexistent ctx.
    if (!m_pDevice->m_Context.IsEmpty())
    {
        m_Status = m_pDevice->GetCtrlQueue()->CtxResource(
            true, m_pDevice->m_Context.GetId(), m_pAllocation->GetId());
        if (!NT_SUCCESS(m_Status))
        {
            m_AttachedToVirgl = false;
            return;
        }
        m_attached = true;
    }
    m_AttachedToVirgl = false;
}

VioGpuDeviceAllocation::~VioGpuDeviceAllocation()
{
    PAGED_CODE();

    if (m_RefCount != 0 || m_pDevice == NULL || m_pAllocation == NULL)
    {
        DbgPrint(TRACE_LEVEL_ERROR, ("---> %s INVALID devalloc: ref=%lld devalloc=%p alloc=%p dev=%p\n", __FUNCTION__, m_RefCount, this, m_pAllocation, m_pDevice));
        VioGpuDbgBreak();
        return;
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s res_id=%d ctx_id=%d\n",
                                   __FUNCTION__,
                                   m_pAllocation->GetId(),
                                   m_pDevice->m_Context.GetId()));


    if (m_pAllocation->IsMapped())
    {
        // This is a driver bug
        DbgPrint(TRACE_LEVEL_WARNING, ("---> %s res_id=%d UNREACHABLE blob is still mapped \n", __FUNCTION__, m_pAllocation->GetId()));
        // FIXME: cannot do this here
        //m_pAllocation->UnmapBlob(m_pDevice->m_Context.GetId(), NULL, NULL);
    }

    if (m_attached)
    {
        m_pDevice->GetCtrlQueue()->CtxResource(false, m_pDevice->m_Context.GetId(), m_pAllocation->GetId());
    }

    if (m_AttachedToVirgl)
    {
        m_pDevice->GetCtrlQueue()->CtxResource(false, m_pDevice->m_Virgl.GetId(), m_pAllocation->GetId());
    }
}

VioGpuAllocation *VioGpuDeviceAllocation::GetAllocation()
{
    PAGED_CODE();

    return m_pAllocation;
}

VioGpuDevice *VioGpuDeviceAllocation::GetDevice()
{
    PAGED_CODE();

    return m_pDevice;
}

PAGED_CODE_SEG_END
