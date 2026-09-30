#include "baseobj.h"
#include "bitops.h"
#include "viogpum.h"
#include "viogpu_allocation.h"
#include "viogpu_adapter.h"
#include "virgl_hw.h"

VioGpuAllocation::VioGpuAllocation(VioGpuAdapter *adapter, VIOGPU_RESOURCE_BLOB_OPTIONS *options, ULONGLONG size)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s BLOB\n", __FUNCTION__));

    m_adapter = adapter;
    m_Id = m_adapter->resourceIdr.GetId();
    m_CreateStatus = STATUS_SUCCESS;
    m_IsImport = FALSE;
    m_IsPrimary = FALSE;
    m_NeedsInitialPresent = FALSE;
    m_IsShared = FALSE;
    memcpy(&m_Blob.Options, options, sizeof(*options));
    RtlZeroMemory(&m_Blob.Info, sizeof(m_Blob.Info));
    // TODO: find a way to make valid
    // Probably via escape or something
    m_Blob.InfoValid = FALSE;
    m_Blob.Created = FALSE;
    m_Size = size;
    m_IsBlob = TRUE;
    m_Blob.Mapped = FALSE;

    m_pMDL = NULL;
    m_pageCount = 0;
    m_pageOffset = 0;
    m_BackingAttachedToHost = FALSE;
    m_DxPhysicalAddress = 0;

    KeInitializeEvent(&m_busyNotification, NotificationEvent, TRUE);
    m_busy = 0;
    KeInitializeSpinLock(&m_busyLock);

    ExInitializeFastMutex(&m_Lock);

    m_refCount = 1;
    m_deferReleaseItem = IoAllocateWorkItem(m_adapter->GetPhysicalDevice());
    if (m_deferReleaseItem == NULL)
    {
        m_CreateStatus = STATUS_INSUFFICIENT_RESOURCES;
    }
    m_deferReleaseCount = 0;
    m_deferReleaseQueued = 0;
    KeInitializeDpc(&m_deferReleaseDpc, VioGpuAllocation::DeferredReleaseDpc, this);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s res_id=%d blob_id=%lld\n", __FUNCTION__, m_Id, m_Blob.Options.blob_id));
}

VioGpuAllocation::VioGpuAllocation(VioGpuAdapter *adapter, VIOGPU_RESOURCE_3D_OPTIONS *options, ULONGLONG size)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s 3D\n", __FUNCTION__));

    m_adapter = adapter;
    m_Id = m_adapter->resourceIdr.GetId();
    m_CreateStatus = STATUS_SUCCESS;
    m_IsImport = FALSE;
    m_IsPrimary = !!(options->flags & VIOGPU_RESOURCE_FLAG_STANDARD_PRIMARY);
    m_NeedsInitialPresent = m_IsPrimary;
    m_IsShared = FALSE;
    memcpy(&m_3dOptions, options, sizeof(*options));
    m_3dOptions.flags &= ~VIOGPU_RESOURCE_FLAG_STANDARD_PRIMARY;
    m_Size = size;
    m_IsBlob = FALSE;

    // m_adapter->ctrlQueue.CreateResource(m_Id, m_options.format, m_options.width, m_options.height);
    m_CreateStatus = m_adapter->ctrlQueue.CreateResource3D(m_Id, &m_3dOptions);

    m_pMDL = NULL;
    m_pageCount = 0;
    m_pageOffset = 0;
    m_BackingAttachedToHost = FALSE;
    m_DxPhysicalAddress = 0;

    KeInitializeEvent(&m_busyNotification, NotificationEvent, TRUE);
    m_busy = 0;
    KeInitializeSpinLock(&m_busyLock);

    ExInitializeFastMutex(&m_Lock);

    m_refCount = 1;
    m_deferReleaseItem = IoAllocateWorkItem(m_adapter->GetPhysicalDevice());
    if (m_deferReleaseItem == NULL && NT_SUCCESS(m_CreateStatus))
    {
        m_CreateStatus = STATUS_INSUFFICIENT_RESOURCES;
    }
    m_deferReleaseCount = 0;
    m_deferReleaseQueued = 0;
    KeInitializeDpc(&m_deferReleaseDpc, VioGpuAllocation::DeferredReleaseDpc, this);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s res_id=%d 3D\n", __FUNCTION__, m_Id));
}

VioGpuAllocation::VioGpuAllocation(VioGpuAdapter *adapter, VIOGPU_RESOURCE_IMPORT_OPTIONS *options, ULONGLONG size)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s IMPORT res_id=%d\n", __FUNCTION__, options->res_id));

    m_adapter = adapter;
    // Adopt the existing host res_id; do NOT mint one. The owning allocation
    // (another process's shared-texture blob) created the resource on its
    // context and owns the id's lifetime.
    m_Id = options->res_id;
    m_CreateStatus = STATUS_SUCCESS;
    m_IsImport = TRUE;
    m_IsPrimary = FALSE;
    m_NeedsInitialPresent = FALSE;
    m_IsShared = FALSE;

    // Behave as a host-backed HOST3D blob so DxgkCreateAllocation's segment
    // selection treats it exactly like the dmabuf it aliases.
    RtlZeroMemory(&m_Blob.Options, sizeof(m_Blob.Options));
    m_Blob.Options.blob_mem = VIOGPU_BLOB_MEM_HOST3D;
    RtlZeroMemory(&m_Blob.Info, sizeof(m_Blob.Info));
    m_Blob.MapOffset = 0;
    m_Blob.InfoValid = FALSE;
    // The host resource already exists (created on the owning context), so
    // Open() must not re-issue RESOURCE_CREATE_BLOB.
    m_Blob.Created = TRUE;
    m_Blob.Mapped = FALSE;
    m_Size = size;
    m_IsBlob = TRUE;

    m_pMDL = NULL;
    m_pageCount = 0;
    m_pageOffset = 0;
    m_BackingAttachedToHost = FALSE;
    m_DxPhysicalAddress = 0;

    KeInitializeEvent(&m_busyNotification, NotificationEvent, TRUE);
    m_busy = 0;
    KeInitializeSpinLock(&m_busyLock);

    ExInitializeFastMutex(&m_Lock);

    m_refCount = 1;
    m_deferReleaseItem = IoAllocateWorkItem(m_adapter->GetPhysicalDevice());
    if (m_deferReleaseItem == NULL)
    {
        m_CreateStatus = STATUS_INSUFFICIENT_RESOURCES;
    }
    m_deferReleaseCount = 0;
    m_deferReleaseQueued = 0;
    KeInitializeDpc(&m_deferReleaseDpc, VioGpuAllocation::DeferredReleaseDpc, this);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s IMPORT res_id=%d size=%lld\n", __FUNCTION__, m_Id, size));
}

VioGpuAllocation::VioGpuAllocation(VioGpuAdapter *adapter, VIOGPU_RESOURCE_SHARED_TEXTURE_OPTIONS *options, ULONGLONG size)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s SHARED blob_id=0x%llx %dx%d primary=%d modifier=0x%llx\n", __FUNCTION__,
                                   options->blob_id, options->width, options->height,
                                   options->primary, options->modifier));

    m_adapter = adapter;
    // Blob-backed shared texture: mint the res_id now; the host binding
    // (RESOURCE_CREATE_BLOB on the UMD's transport context, which staged
    // the pending dmabuf export under blob_id) happens when the creating
    // device opens the allocation.
    m_Id = m_adapter->resourceIdr.GetId();
    m_CreateStatus = STATUS_SUCCESS;
    m_IsImport = FALSE;
    m_IsPrimary = !!options->primary;
    m_NeedsInitialPresent = FALSE;
    m_IsShared = TRUE;
    m_SharedWidth = options->width;
    m_SharedHeight = options->height;
    m_SharedFormat = options->format;
    m_BlobModifier = options->modifier;
    m_CreateCtxId = options->create_ctx_id;
    m_IsBlob = TRUE;
    RtlZeroMemory(&m_Blob.Options, sizeof(m_Blob.Options));
    m_Blob.Options.blob_mem = VIOGPU_BLOB_MEM_HOST3D;
    m_Blob.Options.blob_flags = VIOGPU_BLOB_FLAG_USE_SHAREABLE;
    m_Blob.Options.blob_id = options->blob_id;
    m_Blob.Info = options->ScanoutInfo;
    m_Blob.InfoValid = options->ScanoutInfo.width != 0;
    m_Blob.MapOffset = 0;
    m_Blob.Created = FALSE;
    m_Blob.Mapped = FALSE;
    m_Size = size;

    m_pMDL = NULL;
    m_pageCount = 0;
    m_pageOffset = 0;
    m_BackingAttachedToHost = FALSE;
    m_DxPhysicalAddress = 0;

    KeInitializeEvent(&m_busyNotification, NotificationEvent, TRUE);
    m_busy = 0;
    KeInitializeSpinLock(&m_busyLock);

    ExInitializeFastMutex(&m_Lock);

    m_refCount = 1;
    m_deferReleaseItem = IoAllocateWorkItem(m_adapter->GetPhysicalDevice());
    if (m_deferReleaseItem == NULL)
    {
        m_CreateStatus = STATUS_INSUFFICIENT_RESOURCES;
    }
    m_deferReleaseCount = 0;
    m_deferReleaseQueued = 0;
    KeInitializeDpc(&m_deferReleaseDpc, VioGpuAllocation::DeferredReleaseDpc, this);

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s SHARED res_id=%d blob_id=0x%llx size=%lld\n", __FUNCTION__,
                                   m_Id, options->blob_id, size));
}

static BOOLEAN IsLinear32BppVirtioFormat(ULONG format)
{
    switch (format)
    {
        case VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM:
        case VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM:
        case VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM:
        case VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM:
        case VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM:
        case VIRTIO_GPU_FORMAT_X8B8G8R8_UNORM:
        case VIRTIO_GPU_FORMAT_A8B8G8R8_UNORM:
        case VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM:
            return TRUE;
        default:
            return FALSE;
    }
}

// The shared-texture descriptor crosses two user/kernel boundaries and is
// later used to create a host texture and a scanout.  Treat every field as
// untrusted. Shared textures include higher precision colour data as well as
// DWM surfaces. Only the existing 32-bpp display formats may be primaries.
// Keep this allowlist aligned with the host export/import format contract so
// the producer and consumer agree about pitch and channel order.
static BOOLEAN IsValidVistaSharedTexture(
    const VIOGPU_RESOURCE_SHARED_TEXTURE_OPTIONS *options,
    ULONGLONG allocationSize)
{
#if !defined(VIOGPU_TARGET_VISTA)
    // Keep the existing modern Triton ABI broad.  The Vista package has a
    // smaller, audited format contract for typed shared colour surfaces.
    return options != NULL && options->blob_id != 0 &&
           options->width != 0 && options->height != 0 &&
           options->plane_count != 0 && options->plane_count <= 4 &&
           options->allocation_size != 0 &&
           options->allocation_size <= allocationSize;
#else
    const ULONG dxgiBgra8 = 87; // DXGI_FORMAT_B8G8R8A8_UNORM
    const ULONG dxgiBgrx8 = 88; // DXGI_FORMAT_B8G8R8X8_UNORM
    const ULONG dxgiRgba8 = 28; // DXGI_FORMAT_R8G8B8A8_UNORM
    const ULONG requiredBindFlags = 0x28; // D3D11_BIND_SRV | D3D11_BIND_RT
    const ULONG requiredMiscFlags = 0x2;  // D3D11_RESOURCE_MISC_SHARED
    ULONG expectedScanoutFormat;
    ULONG bytesPerPixel = 4;
    ULONGLONG rowBytes;
    ULONGLONG roundedSize;

    if (options == NULL || options->blob_id == 0 ||
        options->primary > 1 || options->width == 0 || options->height == 0 ||
        options->width > (options->primary ? 4096u : 8192u) ||
        options->height > (options->primary ? 4096u : 8192u) ||
        options->mip_levels != 1 || options->array_size != 1 ||
        options->sample_count != 1 || options->usage != 0 ||
        options->bind_flags != requiredBindFlags ||
        options->cpu_access_flags != 0 ||
        options->misc_flags != requiredMiscFlags ||
        options->texture_layout > 2 || options->plane_count != 1 ||
        options->allocation_size == 0 ||
        options->allocation_size > (ULONGLONG)(SIZE_T)-1 ||
        options->allocation_size > allocationSize ||
        options->allocation_size > MAXULONGLONG - (PAGE_SIZE - 1))
    {
        return FALSE;
    }

    if (options->format == dxgiBgra8)
    {
        expectedScanoutFormat = VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
    }
    else if (options->format == dxgiBgrx8)
    {
        expectedScanoutFormat = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
    }
    else if (options->format == dxgiRgba8)
    {
        expectedScanoutFormat = VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM;
    }
    else if (!options->primary && options->format == 29) // RGBA8 sRGB
    {
        expectedScanoutFormat = 104; // VIRGL_FORMAT_R8G8B8A8_SRGB
    }
    else if (!options->primary && options->format == 91) // BGRA8 sRGB
    {
        expectedScanoutFormat = 100; // VIRGL_FORMAT_B8G8R8A8_SRGB
    }
    else if (!options->primary && options->format == 93) // BGRX8 sRGB
    {
        expectedScanoutFormat = 101; // VIRGL_FORMAT_B8G8R8X8_SRGB
    }
    else if (!options->primary && options->format == 24) // RGB10A2 UNORM
    {
        expectedScanoutFormat = 8; // VIRGL_FORMAT_R10G10B10A2_UNORM
    }
    else if (!options->primary && options->format == 10) // RGBA16 FLOAT
    {
        expectedScanoutFormat = 94; // VIRGL_FORMAT_R16G16B16A16_FLOAT
        bytesPerPixel = 8;
    }
    else
    {
        return FALSE;
    }

    roundedSize = (options->allocation_size + PAGE_SIZE - 1) &
                  ~((ULONGLONG)PAGE_SIZE - 1);
    if (roundedSize == 0 || allocationSize != roundedSize)
    {
        return FALSE;
    }

    rowBytes = (ULONGLONG)options->width * bytesPerPixel;
    if (options->planes[0].offset > MAXULONG ||
        options->planes[0].pitch > MAXULONG ||
        options->planes[0].pitch < rowBytes ||
        options->planes[0].offset >= options->allocation_size ||
        rowBytes > options->allocation_size - options->planes[0].offset ||
        (ULONGLONG)(options->height - 1) >
            (options->allocation_size - options->planes[0].offset - rowBytes) /
                options->planes[0].pitch)
    {
        return FALSE;
    }

    if (options->ScanoutInfo.width != options->width ||
        options->ScanoutInfo.height != options->height ||
        options->ScanoutInfo.format != expectedScanoutFormat ||
        options->ScanoutInfo.strides[0] != (ULONG)options->planes[0].pitch ||
        options->ScanoutInfo.offsets[0] != (ULONG)options->planes[0].offset)
    {
        return FALSE;
    }

    for (ULONG plane = 1; plane < 4; ++plane)
    {
        if (options->planes[plane].offset != 0 ||
            options->planes[plane].pitch != 0 ||
            options->ScanoutInfo.strides[plane] != 0 ||
            options->ScanoutInfo.offsets[plane] != 0)
        {
            return FALSE;
        }
    }
    return TRUE;
#endif
}

BOOLEAN VioGpuAllocation::GetTransferLayout(LONG x, LONG y, ULONG *pStride, ULONGLONG *pOffset) const
{
    ULONG format;
    ULONG width;
    ULONG height;
    ULONG stride;
    ULONGLONG planeOffset;

    if (!pStride || !pOffset || x < 0 || y < 0)
        return FALSE;

    if (m_IsBlob)
    {
        if (!m_Blob.InfoValid)
            return FALSE;

        format = m_Blob.Info.format;
        width = m_Blob.Info.width;
        height = m_Blob.Info.height;
        stride = m_Blob.Info.strides[0];
        planeOffset = m_Blob.Info.offsets[0];
    }
    else
    {
        format = m_3dOptions.format;
        width = m_3dOptions.width;
        height = m_3dOptions.height;
        planeOffset = 0;

        ULONGLONG rowBytes = (ULONGLONG)width * 4;
        if (rowBytes > MAXULONG)
            return FALSE;
        stride = (ULONG)rowBytes;
    }

    if (!IsLinear32BppVirtioFormat(format) || !width || !height ||
        (ULONG)x >= width || (ULONG)y >= height ||
        stride < (ULONGLONG)width * 4)
        return FALSE;

    ULONGLONG offset = planeOffset + (ULONGLONG)(ULONG)y * stride + (ULONGLONG)(ULONG)x * 4;
    if (offset < planeOffset ||
        (m_Size && (m_Size < 4 || offset > m_Size - 4)))
        return FALSE;

    *pStride = stride;
    *pOffset = offset;
    return TRUE;
}

BOOLEAN VioGpuAllocation::GetDimensions(UINT *pWidth, UINT *pHeight) const
{
    if (pWidth == NULL || pHeight == NULL)
        return FALSE;

    if (m_IsBlob)
    {
        if (!m_Blob.InfoValid || !m_Blob.Info.width || !m_Blob.Info.height)
            return FALSE;
        *pWidth = m_Blob.Info.width;
        *pHeight = m_Blob.Info.height;
    }
    else
    {
        if (!m_3dOptions.width || !m_3dOptions.height)
            return FALSE;
        *pWidth = m_3dOptions.width;
        *pHeight = m_3dOptions.height;
    }
    return TRUE;
}

void VioGpuAllocation::AddRef()
{
    InterlockedIncrement(&m_refCount);
}

void VioGpuAllocation::Release()
{
    LONG newCount = InterlockedDecrement(&m_refCount);
    if (newCount < 0)
    {
        // Underflow indicates double-Release somewhere. ASSERT trips
        // in DBG; in retail, leak the object rather than free freed
        // memory.
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("%s refcount underflow alloc=%p count=%d\n",
                  __FUNCTION__, this, newCount));
        ASSERT(newCount >= 0);
        return;
    }
    if (newCount == 0)
    {
        delete this;
    }
}

// Requires IRQL <= DISPATCH_LEVEL. Only the caller that flips
// m_deferReleaseQueued gets to queue the single pre-allocated work item;
// the rest coalesce onto it through m_deferReleaseCount.
void VioGpuAllocation::QueueDeferredReleaseWorkItem()
{
    if (InterlockedCompareExchange(&m_deferReleaseQueued, 1, 0) == 0)
    {
        IoQueueWorkItem(m_deferReleaseItem,
                        VioGpuAllocation::DeferredReleaseWorker,
                        DelayedWorkQueue,
                        this);
    }
}

VOID VioGpuAllocation::DeferredReleaseDpc(_KDPC *Dpc, PVOID Context, PVOID Arg1, PVOID Arg2)
{
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Arg1);
    UNREFERENCED_PARAMETER(Arg2);

    reinterpret_cast<VioGpuAllocation *>(Context)->QueueDeferredReleaseWorkItem();
}

void VioGpuAllocation::ReleaseDeferred()
{
    // The destructor tears down LinkedList<VioGpuDeviceAllocation>, whose
    // entries' dtor is PAGED_CODE(). A Release that drops the last ref
    // from a DPC (or higher) would trip that contract, so at raised IRQL the
    // Release is punted to a work item that runs at PASSIVE_LEVEL.
    //
    // IoQueueWorkItem is itself only callable at IRQL <= DISPATCH_LEVEL, and
    // this function is reached above that: with FlipOnVSyncMmIo,
    // dxgmms1!VidSchiExecuteMmIoFlipAtISR invokes DdiSetVidPnSourceAddress
    // through KeSynchronizeExecution, i.e. at DIRQL holding the interrupt spin
    // lock. Queuing a work item from there deadlocks the machine in
    // nt!KiExitDispatcher -> HalpInterruptSendIpi ->
    // nt!KxWaitForSpinLockAndAcquire, and every other VidSch caller then piles
    // up behind the lock that is never released. A DPC may be queued from any
    // IRQL and runs at DISPATCH_LEVEL, so above DISPATCH_LEVEL hop through one
    // and let it queue the work item.
    if (!m_deferReleaseItem)
    {
        Release();
        return;
    }

    KIRQL irql = KeGetCurrentIrql();
    if (irql < DISPATCH_LEVEL)
    {
        Release();
        return;
    }

    InterlockedIncrement(&m_deferReleaseCount);

    if (irql > DISPATCH_LEVEL)
    {
        // KeInsertQueueDpc returning FALSE just means the DPC is already
        // queued; that pending DPC will drain the count we just bumped.
        KeInsertQueueDpc(&m_deferReleaseDpc, NULL, NULL);
    }
    else
    {
        QueueDeferredReleaseWorkItem();
    }
}

VOID VioGpuAllocation::DeferredReleaseWorker(PDEVICE_OBJECT DeviceObject, PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    VioGpuAllocation *alloc = reinterpret_cast<VioGpuAllocation *>(Context);

    // Claim the pending count before re-arming, never the other way round: a
    // ReleaseDeferred that bumps the count after the re-arm would queue a
    // second work item, and this one could meanwhile drop that reference and
    // destroy `alloc` out from under it. A count bumped in the window between
    // the two exchanges found the queue flag still set and so did not re-arm
    // itself; pick it up here.
    LONG pending = InterlockedExchange(&alloc->m_deferReleaseCount, 0);
    InterlockedExchange(&alloc->m_deferReleaseQueued, 0);
    if (alloc->m_deferReleaseCount != 0)
    {
        alloc->QueueDeferredReleaseWorkItem();
    }

    // `pending` is at least 1, so `alloc` is live for everything above; the
    // Releases below may free it, so touch no member past this point.
    while (pending-- > 0)
    {
        alloc->Release();
    }
}

void NotifyResourceDestroyed(void *ctx, void *cmd, void *)
{
    VioGpuIdr *resIdr = reinterpret_cast<VioGpuIdr *>(ctx);
    PGPU_RES_UNREF unref_cmd = reinterpret_cast<PGPU_RES_UNREF>(cmd);
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--> %s freeing res_id=%d\n", __FUNCTION__, unref_cmd->resource_id));
    resIdr->PutId(unref_cmd->resource_id);
}

VioGpuAllocation::~VioGpuAllocation(void)
{
    // m_DeviceAllocations.clear() invokes ~VioGpuDeviceAllocation, which is
    // PAGED_CODE(). Any caller that drops the last ref from IRQL >=
    // DISPATCH_LEVEL must funnel through ReleaseDeferred so the destructor
    // lands here at PASSIVE_LEVEL.
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s res_id=%d IsBlob=%d alloc=%p size=%zu\n", __FUNCTION__, m_Id, m_IsBlob, this, m_DeviceAllocations.size()));

    // A pending deferred release always holds a reference, so the DPC cannot
    // normally still be queued here; dequeue it anyway rather than leave a
    // callback pointing at freed memory.
    KeRemoveQueueDpc(&m_deferReleaseDpc);

    if (m_deferReleaseItem)
    {
        IoFreeWorkItem(m_deferReleaseItem);
        m_deferReleaseItem = NULL;
    }

    m_DeviceAllocations.clear();

    if (m_IsBlob && m_Blob.Mapped)
    {
        // Orphaned mapping (process died without the UMD unmap).  Do NOT
        // send UNMAP_BLOB here: back-to-back unmap+destroy hits the QEMU
        // async-unmap/destroy race and wedges its main loop (cmdq suspended
        // forever on the unmap).  The DISCARD_CONTENT paging op
        // (BuildPagingBuffer) performs the ordered host unmap when VidMm
        // actually frees the range; the destroy below is safe because the
        // host keeps blob memory alive until RES_UNREF.
        DbgPrint(TRACE_LEVEL_WARNING,
                 ("---> %s res_id=%d still-mapped blob at destroy (host unmap deferred to DISCARD)\n",
                  __FUNCTION__, m_Id));
        m_Blob.Mapped = FALSE;
    }

    if (m_IsImport)
    {
        // The adopted res_id is owned by another allocation (the transport
        // device's swapchain claim). RES_UNREF + PutId here would free a host
        // resource that is still referenced and double-free the id; the owning
        // allocation's destructor performs the single destroy. m_DeviceAllocations
        // was already cleared above, detaching the id from this device's context.
        DbgPrint(TRACE_LEVEL_VERBOSE, ("<--> %s IMPORT res_id=%d: skip DestroyResource (not owner)\n", __FUNCTION__, m_Id));
    }
    else if (!m_IsBlob || m_Blob.Created)
    {
        m_adapter->ctrlQueue.DestroyResource(m_Id, NotifyResourceDestroyed, &m_adapter->resourceIdr);
        // m_adapter->resourceIdr.PutId(m_Id);
    }
    else
    {
        // Creation never reached the host. Do not submit RES_UNREF for an id
        // the device has never seen.
        m_adapter->resourceIdr.PutId(m_Id);
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

_IRQL_requires_max_(APC_LEVEL) VOID VioGpuAllocation::Lock()
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));
    ExAcquireFastMutex(&m_Lock);
}

_IRQL_requires_max_(APC_LEVEL) VOID VioGpuAllocation::Unlock()
{
    ExReleaseFastMutex(&m_Lock);
}

NTSTATUS VioGpuAllocation::AttachBacking(MDL *pMDL, size_t pageCount, size_t pageOffset)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s res_id=%d, IsBlob=%d\n", __FUNCTION__, m_Id, m_IsBlob));

    return StoreBacking(pMDL, pageCount, pageOffset, TRUE);
}

NTSTATUS VioGpuAllocation::StoreBacking(MDL *pMDL, size_t pageCount,
                                        size_t pageOffset,
                                        BOOLEAN attachToHost)
{
    PAGED_CODE();

    auto lock_guard = LockGuard();

    if (m_pMDL != NULL)
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("%s res_id=%d already has guest backing\n",
                  __FUNCTION__, m_Id));
        return STATUS_INVALID_DEVICE_STATE;
    }

    if ((pMDL == NULL) || (pageCount == 0) ||
        (pageCount > (MAXULONG / sizeof(GPU_MEM_ENTRY))))
    {
        return STATUS_INVALID_PARAMETER;
    }

    SIZE_T mdlPages = ADDRESS_AND_SIZE_TO_SPAN_PAGES(MmGetMdlVirtualAddress(pMDL),
                                                     MmGetMdlByteCount(pMDL));
    if ((pageOffset > mdlPages) || (pageCount > mdlPages - pageOffset))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("%s MDL range offset=%Iu count=%Iu exceeds pages=%Iu\n",
                  __FUNCTION__, pageOffset, pageCount, mdlPages));
        return STATUS_INVALID_PARAMETER;
    }

    if (attachToHost)
    {
        GPU_MEM_ENTRY *ents = new (VIOGPU_NONPAGED_POOL) GPU_MEM_ENTRY[pageCount];
        if (ents == NULL)
        {
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        UINT entryCount = 0;
        PFN_NUMBER *pfns = MmGetMdlPfnArray(pMDL);
        for (UINT i = 0; i < pageCount; i++)
        {
            // PFN_NUMBER is 32-bit on i386, so widen before shifting -- otherwise
            // any page above the 4GiB line wraps and we hand the host a bogus PA.
            ULONGLONG pageAddress =
                (ULONGLONG)pfns[pageOffset + i] << PAGE_SHIFT;

            // One virtio backing entry can describe a physically contiguous
            // byte range.  Coalesce adjacent PFNs so a 128 MiB aperture does
            // not exceed QEMU's normal 16384-entry resource limit merely
            // because the MDL is expressed as one PFN per page.
            if (entryCount != 0 &&
                ents[entryCount - 1].addr + ents[entryCount - 1].length ==
                    pageAddress &&
                ents[entryCount - 1].length <= MAXULONG - PAGE_SIZE)
            {
                ents[entryCount - 1].length += PAGE_SIZE;
                continue;
            }

            ents[entryCount].addr = pageAddress;
            ents[entryCount].length = PAGE_SIZE;
            ents[entryCount].padding = 0;
            entryCount++;
        }

        DbgPrint(TRACE_LEVEL_INFORMATION,
                 ("%s res_id=%d coalesced backing pages=%Iu entries=%u\n",
                  __FUNCTION__, m_Id, pageCount, entryCount));
        NTSTATUS status =
            m_adapter->ctrlQueue.AttachBacking(m_Id, ents, entryCount);
        if (!NT_SUCCESS(status))
        {
            return status;
        }
    }

    m_pMDL = pMDL;
    m_pageCount = pageCount;
    m_pageOffset = pageOffset;
    m_BackingAttachedToHost = attachToHost;
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
    return STATUS_SUCCESS;
}

NTSTATUS VioGpuAllocation::AttachFrameBufferBacking(
    PHYSICAL_ADDRESS segmentAddress)
{
    PAGED_CODE();

    auto lock_guard = LockGuard();
    return AttachFrameBufferBackingLocked(segmentAddress);
}

NTSTATUS VioGpuAllocation::AttachFrameBufferBackingLocked(
    PHYSICAL_ADDRESS segmentAddress)
{

#if !defined(VIOGPU_TARGET_VISTA)
    UNREFERENCED_PARAMETER(segmentAddress);
    return STATUS_NOT_SUPPORTED;
#else
    const ULONGLONG segmentBase = VioGpuAdapter::FRAMEBUFFER_GPU_BASE_VA;
    const ULONGLONG segmentSize = m_adapter->GetFrameBufferSize();
    const ULONGLONG roundedSize =
        (m_Size + PAGE_SIZE - 1) & ~((ULONGLONG)PAGE_SIZE - 1);
    if (!m_IsPrimary || (m_IsBlob && !m_IsShared) || roundedSize == 0 ||
        roundedSize > MAXULONG || segmentAddress.QuadPart < segmentBase)
    {
        return STATUS_INVALID_PARAMETER;
    }

    const ULONGLONG offset = segmentAddress.QuadPart - segmentBase;
    if (offset > segmentSize || roundedSize > segmentSize - offset ||
        m_adapter->GetFrameBufferPA().QuadPart >
            MAXULONGLONG - offset)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (m_IsShared)
    {
        // The BAR supplies Vista's CPU-visible primary backing. The HOST3D
        // blob retains its exported host storage and must not receive a
        // RESOURCE_ATTACH_BACKING command for this separate CPU mapping.
        if (m_pMDL != NULL || m_BackingAttachedToHost)
        {
            return STATUS_INVALID_DEVICE_STATE;
        }
        m_DxPhysicalAddress = (SIZE_T)offset;
        return STATUS_SUCCESS;
    }

    if (m_BackingAttachedToHost && m_pMDL == NULL &&
        m_DxPhysicalAddress == (SIZE_T)offset)
    {
        return STATUS_SUCCESS;
    }

    if (m_BackingAttachedToHost)
    {
        m_adapter->ctrlQueue.DetachBacking(m_Id);
        m_BackingAttachedToHost = FALSE;
    }

    GPU_MEM_ENTRY *entry =
        new (VIOGPU_NONPAGED_POOL) GPU_MEM_ENTRY[1];
    if (entry == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    const ULONGLONG guestPhysicalAddress =
        m_adapter->GetFrameBufferPA().QuadPart + offset;
    entry[0].addr = guestPhysicalAddress;
    entry[0].length = (ULONG)roundedSize;
    entry[0].padding = 0;

    NTSTATUS status = m_adapter->ctrlQueue.AttachBacking(m_Id, entry, 1);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    m_pMDL = NULL;
    m_pageCount = (SIZE_T)(roundedSize / PAGE_SIZE);
    m_pageOffset = 0;
    m_BackingAttachedToHost = TRUE;
    m_DxPhysicalAddress = (SIZE_T)offset;
    DbgPrint(TRACE_LEVEL_INFORMATION,
             ("%s res_id=%d segment=%llx guest-pa=%llx bytes=%llx\n",
              __FUNCTION__, m_Id, segmentAddress.QuadPart,
              guestPhysicalAddress, roundedSize));
    return STATUS_SUCCESS;
#endif
}

void VioGpuAllocation::DetachBacking()
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s res_id=%d IsBlob=%d\n", __FUNCTION__, m_Id, m_IsBlob));

    auto lock_guard = LockGuard();

    const BOOLEAN detachFromHost = m_BackingAttachedToHost;
    m_pMDL = NULL;
    m_pageCount = 0;
    m_pageOffset = 0;
    m_BackingAttachedToHost = FALSE;

    if (detachFromHost)
    {
        m_adapter->ctrlQueue.DetachBacking(m_Id);
    }
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s\n", __FUNCTION__));
}

VOID VioGpuAllocation::CreateBlob(UINT ctx_id)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s res_id=%d IsBlob=%d\n", __FUNCTION__, m_Id, m_IsBlob));

    auto lock_guard = LockGuard();

    if (IsCreated()) return;

    bool ok = m_adapter->ctrlQueue.CreateResourceBlob(m_Id, ctx_id, &m_Blob.Options, m_Size);
    m_Blob.Created = ok;
}

NTSTATUS VioGpuAllocation::MapBlobLocked(UINT ctx_id,
                                         void (*complete_cb)(void *, void *, void *),
                                         void *complete_ctx,
                                         BOOLEAN *pIssued)
{
    *pIssued = FALSE;
    NTSTATUS status = m_adapter->ctrlQueue.ResourceMapBlob(
        m_Id, ctx_id, m_Blob.MapOffset, complete_cb, complete_ctx);
    if (!NT_SUCCESS(status))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("%s failed res_id=%d status=0x%x\n",
                  __FUNCTION__, m_Id, status));
        // QueueBuffer fires complete_cb synchronously after an enqueue
        // failure. Command allocation fails before a callback is installed.
        *pIssued = status == STATUS_DEVICE_NOT_READY;
        return status;
    }
    m_Blob.Mapped = TRUE;
    *pIssued = TRUE;
    return STATUS_SUCCESS;
}

NTSTATUS VioGpuAllocation::UnmapBlobLocked(UINT ctx_id,
                                           void (*complete_cb)(void *, void *, void *),
                                           void *complete_ctx,
                                           BOOLEAN *pIssued)
{
    // The host treats RESOURCE_UNMAP_BLOB as a per-(res_id) operation rather
    // than per-(res_id, ctx_id): the first UNMAP releases the host mapping,
    // any subsequent UNMAP — whether from a cross-context sharer (TYPE_IMPORT
    // adopting the same res_id) or a duplicate Close path — returns
    // INVALID_RESOURCE_ID. Gating on the local Mapped flag matches the host
    // semantics and makes Close idempotent across cross-context sharers.
    *pIssued = FALSE;
    if (!m_Blob.Mapped) return STATUS_SUCCESS;
    if (m_Id == 0)
    {
        // Phantom allocations (shared-registry backing) never created a
        // host blob; an UNMAP_BLOB for res_id 0 is the guest-error spam
        // in the QEMU log.  Clear local state and skip the wire op.
        m_Blob.Mapped = FALSE;
        return STATUS_SUCCESS;
    }
    NTSTATUS status = m_adapter->ctrlQueue.ResourceUnmapBlob(
        m_Id, ctx_id, complete_cb, complete_ctx);
    if (!NT_SUCCESS(status))
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("%s failed res_id=%d status=0x%x\n",
                  __FUNCTION__, m_Id, status));
        *pIssued = status == STATUS_DEVICE_NOT_READY;
        return status;
    }
    m_Blob.Mapped = FALSE;
    *pIssued = TRUE;
    return STATUS_SUCCESS;
}

NTSTATUS VioGpuAllocation::MapBlob(UINT ctx_id,
                                   void (*complete_cb)(void *, void *, void *),
                                   void *complete_ctx,
                                   BOOLEAN *pIssued)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s res_id=%d IsBlob=%d\n", __FUNCTION__, m_Id, m_IsBlob));

    if (pIssued == NULL || !m_IsBlob) return STATUS_INVALID_PARAMETER;
    *pIssued = FALSE;

    // Already mapped: no host command issued, so complete_cb will not fire --
    // report FALSE so the caller's pending count stays balanced.
    auto lock_guard = LockGuard();
    if (m_Blob.Mapped) return STATUS_SUCCESS;
    return MapBlobLocked(ctx_id, complete_cb, complete_ctx, pIssued);
}

NTSTATUS VioGpuAllocation::UnmapBlob(UINT ctx_id,
                                     void (*complete_cb)(void *, void *, void *),
                                     void *complete_ctx,
                                     BOOLEAN *pIssued)
{
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s res_id=%d IsBlob=%d\n", __FUNCTION__, m_Id, m_IsBlob));

    if (pIssued == NULL || !m_IsBlob) return STATUS_INVALID_PARAMETER;
    *pIssued = FALSE;

    auto lock_guard = LockGuard();
    if (!m_Blob.Mapped) return STATUS_SUCCESS;
    return UnmapBlobLocked(ctx_id, complete_cb, complete_ctx, pIssued);
}

VioGpuAllocationLockGuard::VioGpuAllocationLockGuard(VioGpuAllocation *allocation) : m_Allocation(allocation)
{
    m_Allocation->Lock();
}

VioGpuAllocationLockGuard::~VioGpuAllocationLockGuard()
{
    m_Allocation->Unlock();
}

// NONPAGED: called from VioGpuCommand::Run / AttachAllocations on the
// commander thread at DISPATCH_LEVEL (spinlocks held). Living in the
// PAGE segment bugchecks 0xD1 (instruction-fetch of paged-out code at
// IRQL 2) when UnmarkBusy is reached from Run.
void VioGpuAllocation::MarkBusy()
{
    // Serialize counter + event mutation under m_busyLock. Without it,
    // an interleaving where UnmarkBusy decrements to 0 and SetEvent's,
    // then MarkBusy increments and ClearEvent's, would leave the
    // counter > 0 with the event signalled -- causing EscapeResourceBusy
    // to busy-spin waking immediately on every check.
    KIRQL oldIrql;
    KeAcquireSpinLock(&m_busyLock, &oldIrql);
    InterlockedIncrement(&m_busy);
    KeClearEvent(&m_busyNotification);
    KeReleaseSpinLock(&m_busyLock, oldIrql);
}

void VioGpuAllocation::UnmarkBusy()
{
    KIRQL oldIrql;
    KeAcquireSpinLock(&m_busyLock, &oldIrql);
    LONG remaining = InterlockedDecrement(&m_busy);
    if (remaining < 0)
    {
        // Underflow: more UnmarkBusy than MarkBusy. Clamp back to 0
        // and signal so a waiter doesn't see a permanently-negative
        // counter. DBG trips the assert below.
        InterlockedExchange(&m_busy, 0);
        KeSetEvent(&m_busyNotification, IO_NO_INCREMENT, FALSE);
    }
    else if (remaining == 0)
    {
        KeSetEvent(&m_busyNotification, IO_NO_INCREMENT, FALSE);
    }
    KeReleaseSpinLock(&m_busyLock, oldIrql);

    if (remaining < 0)
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("%s busy underflow res_id=%d remaining=%d\n",
                  __FUNCTION__, m_Id, remaining));
    }
    ASSERT(remaining >= 0);
}

PAGED_CODE_SEG_BEGIN

D3DDDIFORMAT VioGpuToD3DDDIColorFormat(virtio_gpu_formats format)
{
    PAGED_CODE();

    switch (format)
    {
        case VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM:
            return D3DDDIFMT_A8R8G8B8;
        case VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM:
            return D3DDDIFMT_X8R8G8B8;
        case VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM:
            return D3DDDIFMT_A8B8G8R8;
        case VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM:
            return D3DDDIFMT_X8B8G8R8;
    }
    DbgPrint(TRACE_LEVEL_ERROR, ("---> %s Unsupported color format %d\n", __FUNCTION__, format));
    return D3DDDIFMT_X8B8G8R8;
}

static NTSTATUS VioGpuControlResponseStatus(PGPU_CTRL_HDR response)
{
    if (response != NULL &&
        response->type >= VIRTIO_GPU_RESP_OK_NODATA &&
        response->type < VIRTIO_GPU_RESP_ERR_UNSPEC)
    {
        return STATUS_SUCCESS;
    }
    if (response != NULL)
    {
        if (response->type == VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY)
        {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        if (response->type == VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID ||
            response->type == VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID ||
            response->type == VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID ||
            response->type == VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER)
        {
            return STATUS_INVALID_PARAMETER;
        }
    }
    return STATUS_IO_DEVICE_ERROR;
}

// Wait for one global control command to reach the host and copy its response
// before releasing the vbuf. VioGpuWaitCtxFinish keeps both the wait context
// and vbuf valid if the half-second bound expires and a later DPC completes it.
static NTSTATUS VioGpuQueueControlSync(CtrlQueue *queue, PGPU_VBUFFER vbuf)
{
    PVIOGPU_WAIT_CTX waitCtx = VioGpuAllocWaitCtx();
    if (waitCtx == NULL)
    {
        queue->ReleaseBuffer(vbuf);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    waitCtx->vbuf = vbuf;
    InterlockedIncrement(&waitCtx->refCount);
    vbuf->complete_cb = VioGpuWaitCtxCompleteCB;
    vbuf->complete_ctx = waitCtx;
    InterlockedIncrement(&vbuf->ref_count);

    if (queue->QueueBuffer(vbuf) == (UINT)-1)
    {
        // QueueBuffer fired the callback synchronously and dropped its own
        // reference. Finish the caller reference and report the enqueue fault.
        VioGpuWaitCtxFinish(waitCtx, vbuf, queue, STATUS_SUCCESS);
        queue->ReleaseBuffer(vbuf);
        return STATUS_DEVICE_NOT_READY;
    }

    LARGE_INTEGER timeout = {};
    // FLUSH and SET_SCANOUT run in sequence. Give each operation a half-second
    // bound so one failed promotion attempt cannot wait without a limit.
    timeout.QuadPart = -5000000LL;
    NTSTATUS waitStatus = KeWaitForSingleObject(&waitCtx->event,
                                                Executive,
                                                KernelMode,
                                                FALSE,
                                                &timeout);
    if (waitStatus == STATUS_TIMEOUT)
    {
        VioGpuWaitCtxFinish(waitCtx, vbuf, queue, waitStatus);
        return STATUS_IO_TIMEOUT;
    }
    if (!NT_SUCCESS(waitStatus))
    {
        // This wait is not alertable, so Vista normally returns only success
        // or timeout. Treat any other failure as caller abandonment. The
        // completion callback can then release the queue-owned references.
        VioGpuWaitCtxFinish(waitCtx, vbuf, queue, STATUS_TIMEOUT);
        return waitStatus;
    }

    VioGpuWaitCtxFinish(waitCtx, vbuf, queue, waitStatus);
    NTSTATUS responseStatus =
        VioGpuControlResponseStatus((PGPU_CTRL_HDR)vbuf->resp_buf);
    queue->ReleaseBuffer(vbuf);
    return responseStatus;
}

NTSTATUS VioGpuAllocation::CopyHostToPrimary(
    VioGpuAllocation *source, const RECT *rects, UINT count,
    LONG deltaX, LONG deltaY)
{
    PAGED_CODE();
    if (!source || source == this || !source->IsHostPresentationSurface() ||
        !IsPrimary() || IsBlob() || !rects || !count)
        return STATUS_INVALID_PARAMETER;

    VIOGPU_BLOB_INFO info;
    {
        auto guard = source->LockGuard();
        if (!source->m_Blob.InfoValid)
            return STATUS_INVALID_DEVICE_STATE;
        info = source->m_Blob.Info;
    }
    UINT width = 0, height = 0;
    if (!GetDimensions(&width, &height) || !info.width || !info.height ||
        info.width > 4096 || info.height > 4096 ||
        info.strides[0] < (ULONGLONG)info.width * 4 ||
        (info.format != VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM &&
         info.format != VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM) ||
        info.strides[1] || info.strides[2] || info.strides[3] ||
        info.offsets[1] || info.offsets[2] || info.offsets[3])
        return STATUS_INVALID_PARAMETER;

    // Validate the entire list before issuing any host writes.
    for (UINT i = 0; i < count; ++i) {
        const RECT &r = rects[i];
        if (r.left < 0 || r.top < 0 || r.right <= r.left || r.bottom <= r.top ||
            (ULONGLONG)r.right > width || (ULONGLONG)r.bottom > height ||
            (LONGLONG)r.left + deltaX < 0 ||
            (LONGLONG)r.top + deltaY < 0 ||
            (LONGLONG)r.right + deltaX > info.width ||
            (LONGLONG)r.bottom + deltaY > info.height)
            return STATUS_INVALID_PARAMETER;
    }
    for (UINT i = 0; i < count; ++i) {
        PGPU_VBUFFER vbuf = NULL;
        GPU_TRITON_PRESENT_BLT *b = (GPU_TRITON_PRESENT_BLT *)
            m_adapter->ctrlQueue.AllocCmd(&vbuf, sizeof(*b));
        if (!b)
            return STATUS_INSUFFICIENT_RESOURCES;
        RtlZeroMemory(b, sizeof(*b));
        b->hdr.type = VIRTIO_GPU_CMD_TRITON_PRESENT_BLT;
        b->source_resource_id = source->GetId();
        b->destination_resource_id = GetId();
        b->source_width = info.width;
        b->source_height = info.height;
        b->source_format = info.format;
        b->source_stride = info.strides[0];
        b->source_offset = info.offsets[0];
        b->source_rect.x = (ULONG)((LONGLONG)rects[i].left + deltaX);
        b->source_rect.y = (ULONG)((LONGLONG)rects[i].top + deltaY);
        b->source_rect.width = rects[i].right - rects[i].left;
        b->source_rect.height = rects[i].bottom - rects[i].top;
        b->destination_x = rects[i].left;
        b->destination_y = rects[i].top;
        NTSTATUS status = VioGpuQueueControlSync(&m_adapter->ctrlQueue, vbuf);
        if (!NT_SUCCESS(status))
            return status;
    }
    static LONG reported = 0;
    if (InterlockedCompareExchange(&reported, 1, 0) == 0)
        DbgPrint(TRACE_LEVEL_WARNING,
                 ("TRITON-HOST-BLT-CONSUMED src=%u dst=%u rects=%u\n",
                  source->GetId(), GetId(), count));
    return STATUS_SUCCESS;
}

static NTSTATUS VioGpuQueueScanout(CtrlQueue *queue,
                                   UINT scanId,
                                   UINT resourceId,
                                   UINT width,
                                   UINT height,
                                   UINT x,
                                   UINT y)
{
    PGPU_VBUFFER vbuf = NULL;
    PGPU_SET_SCANOUT command =
        (PGPU_SET_SCANOUT)queue->AllocCmd(&vbuf, sizeof(*command));
    if (command == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(command, sizeof(*command));
    command->hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    command->resource_id = resourceId;
    command->scanout_id = scanId;
    command->r.width = width;
    command->r.height = height;
    command->r.x = x;
    command->r.y = y;
    return VioGpuQueueControlSync(queue, vbuf);
}

static NTSTATUS VioGpuQueueScanoutBlob(CtrlQueue *queue,
                                       UINT scanId,
                                       UINT resourceId,
                                       const GPU_RECT &rect,
                                       const VIOGPU_BLOB_INFO &info)
{
    PGPU_VBUFFER vbuf = NULL;
    PGPU_SET_SCANOUT_BLOB command =
        (PGPU_SET_SCANOUT_BLOB)queue->AllocCmd(&vbuf, sizeof(*command));
    if (command == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(command, sizeof(*command));
    command->hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT_BLOB;
    command->r = rect;
    command->scanout_id = scanId;
    command->resource_id = resourceId;
    command->width = info.width;
    command->height = info.height;
    command->format = info.format;
    for (UINT plane = 0; plane < RTL_NUMBER_OF(command->strides); ++plane)
    {
        command->strides[plane] = info.strides[plane];
        command->offsets[plane] = info.offsets[plane];
    }

    return VioGpuQueueControlSync(queue, vbuf);
}

static NTSTATUS VioGpuQueueResourceFlush(CtrlQueue *queue,
                                         UINT resourceId,
                                         const GPU_RECT &rect)
{
    PGPU_VBUFFER vbuf = NULL;
    PGPU_RES_FLUSH command =
        (PGPU_RES_FLUSH)queue->AllocCmd(&vbuf, sizeof(*command));
    if (command == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(command, sizeof(*command));
    command->hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    command->resource_id = resourceId;
    command->r = rect;
    return VioGpuQueueControlSync(queue, vbuf);
}

NTSTATUS VioGpuAllocation::DisableScanout(CtrlQueue *queue, UINT scan_id)
{
    PAGED_CODE();
    if (queue == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }
    return VioGpuQueueScanout(queue, scan_id, 0, 0, 0, 0, 0);
}

NTSTATUS VioGpuAllocation::FlushToScreen(UINT scan_id)
{
    PAGED_CODE();

    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s res_id=%d IsBlob=%d\n", __FUNCTION__, m_Id, m_IsBlob));

    if (m_IsBlob) {
        // Snapshot under the allocation lock so a concurrent info update
        // cannot pair a pre-write rect with post-write framebuffer fields
        // in one scanout command.
        VIOGPU_BLOB_INFO info;
        BOOL infoValid;
        {
            auto lock_guard = LockGuard();
            info = m_Blob.Info;
            infoValid = m_Blob.InfoValid;
        }

        DbgPrint(TRACE_LEVEL_INFORMATION, ("---> %s scanout blob res_id=%d valid=%d %dx%d\n", __FUNCTION__, m_Id, infoValid, info.width, info.height));

        // A blob whose info has not been published yet has zero
        // dimensions; the host rejects a degenerate scanout rect, so
        // hold off the scanout until the info lands (the next flip
        // retries with the same resource).
        const ULONGLONG rowBytes = (ULONGLONG)info.width * 4;
        if (!infoValid || !info.width || !info.height ||
            (info.format != VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM &&
             info.format != VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM &&
             info.format != VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM) ||
            info.strides[0] < rowBytes || info.offsets[0] >= m_Size ||
            rowBytes > m_Size - info.offsets[0] ||
            (ULONGLONG)(info.height - 1) >
                (m_Size - info.offsets[0] - rowBytes) / info.strides[0] ||
            info.strides[1] != 0 || info.strides[2] != 0 || info.strides[3] != 0 ||
            info.offsets[1] != 0 || info.offsets[2] != 0 || info.offsets[3] != 0) {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("---> %s invalid blob scanout metadata res_id=%d valid=%d size=%llu %ux%u fmt=%u pitch=%u offset=%u\n",
                      __FUNCTION__, m_Id, infoValid, m_Size, info.width, info.height,
                      info.format, info.strides[0], info.offsets[0]));
            return STATUS_INVALID_DEVICE_STATE;
        }

        GPU_RECT rect;
        rect.x = 0;
        rect.y = 0;
        rect.width = info.width;
        rect.height = info.height;

        // Select the buffer before flushing: the host applies a flush only
        // to scanouts already bound to that resource. Flushing first leaves
        // a newly selected flip-chain member without a display update.
        NTSTATUS status = VioGpuQueueScanoutBlob(&m_adapter->ctrlQueue,
                                                scan_id,
                                                m_Id,
                                                rect,
                                                info);
        if (!NT_SUCCESS(status))
        {
            return status;
        }
        status = VioGpuQueueResourceFlush(&m_adapter->ctrlQueue, m_Id, rect);
        if (!NT_SUCCESS(status))
        {
            // Keep the flip armed; retrying the same scanout is idempotent.
            return status;
        }
    } else {
        DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s scanout 3d res_id=%d %dx%d\n", __FUNCTION__, m_Id, m_3dOptions.width, m_3dOptions.height));
        if (!m_3dOptions.width || !m_3dOptions.height)
        {
            return STATUS_INVALID_DEVICE_STATE;
        }
        GPU_RECT rect = {0, 0, m_3dOptions.width, m_3dOptions.height};
        // FLUSH publishes only scanouts already bound to this resource.
        // Ordinary flip-chain buffers need the same bind-before-flush order
        // as blobs; otherwise a new buffer's update is silently discarded.
        NTSTATUS status = VioGpuQueueScanout(&m_adapter->ctrlQueue,
                                             scan_id, m_Id,
                                             m_3dOptions.width, m_3dOptions.height,
                                             0, 0);
        if (!NT_SUCCESS(status)) return status;
        status = VioGpuQueueResourceFlush(&m_adapter->ctrlQueue, m_Id, rect);
        if (!NT_SUCCESS(status)) return status;
    }

    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s res_id=%d\n", __FUNCTION__, m_Id));
    return STATUS_SUCCESS;
}

NTSTATUS VioGpuAllocation::GetStandardAllocationDriverData(DXGKARG_GETSTANDARDALLOCATIONDRIVERDATA *pStandardAllocation)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_ERROR, ("---> %s type=%d\n", __FUNCTION__, pStandardAllocation->StandardAllocationType));

    pStandardAllocation->ResourcePrivateDriverDataSize = 0;
    if (pStandardAllocation->pAllocationPrivateDriverData == NULL)
    {
        pStandardAllocation->AllocationPrivateDriverDataSize = sizeof(VIOGPU_CREATE_ALLOCATION_EXCHANGE);
        return STATUS_SUCCESS;
    }
    if (pStandardAllocation->AllocationPrivateDriverDataSize <
        sizeof(VIOGPU_CREATE_ALLOCATION_EXCHANGE))
    {
        pStandardAllocation->AllocationPrivateDriverDataSize = sizeof(VIOGPU_CREATE_ALLOCATION_EXCHANGE);
        return STATUS_BUFFER_TOO_SMALL;
    }

    VIOGPU_CREATE_ALLOCATION_EXCHANGE *allocationExchange = (VIOGPU_CREATE_ALLOCATION_EXCHANGE *)pStandardAllocation->pAllocationPrivateDriverData;
    RtlZeroMemory(allocationExchange, sizeof(*allocationExchange));

    // TODO: make this work with blob
    allocationExchange->Type = VIOGPU_RESOURCE_TYPE_3D;

    allocationExchange->Options3D.target = 2;
    allocationExchange->Options3D.format = VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM;
    allocationExchange->Options3D.bind = VIRGL_BIND_RENDER_TARGET | VIRGL_BIND_SAMPLER_VIEW |
                                         VIRGL_BIND_DISPLAY_TARGET | VIRGL_BIND_SCANOUT;

    allocationExchange->Options3D.width = 1024;
    allocationExchange->Options3D.height = 768;
    allocationExchange->Options3D.depth = 1;

    allocationExchange->Options3D.array_size = 1;
    allocationExchange->Options3D.last_level = 0;
    allocationExchange->Options3D.nr_samples = 0;
    allocationExchange->Options3D.flags = 0;

    switch (pStandardAllocation->StandardAllocationType)
    {
        case D3DKMDT_STANDARDALLOCATION_SHAREDPRIMARYSURFACE:
            {
                D3DKMDT_SHAREDPRIMARYSURFACEDATA *surfaceData = pStandardAllocation->pCreateSharedPrimarySurfaceData;
                //[in] UINT                           Width;
                //[in] UINT                           Height;
                //[in] D3DDDIFORMAT                   Format;

                virtio_gpu_formats format;
                if ((surfaceData == NULL) || (surfaceData->Width == 0) ||
                    (surfaceData->Height == 0) ||
                    (surfaceData->Width > MAXULONG / 4) ||
                    !TryColorFormat(surfaceData->Format, &format))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                allocationExchange->Options3D.width = surfaceData->Width;
                allocationExchange->Options3D.height = surfaceData->Height;
                allocationExchange->Options3D.format = format;
                allocationExchange->Options3D.flags |=
                    VIOGPU_RESOURCE_FLAG_STANDARD_PRIMARY;
                allocationExchange->Size = (ULONGLONG)surfaceData->Width * (ULONGLONG)surfaceData->Height * 4;

                DbgPrint(TRACE_LEVEL_ERROR,
                         ("<--- %s shared primary surface: width=%d, height=%d, format=%d\n",
                          __FUNCTION__,
                          surfaceData->Width,
                          surfaceData->Height,
                          surfaceData->Format));
                return STATUS_SUCCESS;
            }

        case D3DKMDT_STANDARDALLOCATION_SHADOWSURFACE:
            {
                D3DKMDT_SHADOWSURFACEDATA *surfaceData = pStandardAllocation->pCreateShadowSurfaceData;
                //[in] UINT                           Width;
                //[in] UINT                           Height;
                //[in] D3DDDIFORMAT                   Format;

                virtio_gpu_formats format;
                if ((surfaceData == NULL) || (surfaceData->Width == 0) ||
                    (surfaceData->Height == 0) ||
                    (surfaceData->Width > MAXULONG / 4) ||
                    !TryColorFormat(surfaceData->Format, &format))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                allocationExchange->Options3D.width = surfaceData->Width;
                allocationExchange->Options3D.height = surfaceData->Height;
                allocationExchange->Options3D.format = format;
                allocationExchange->Size = (ULONGLONG)surfaceData->Width * (ULONGLONG)surfaceData->Height * 4;

                allocationExchange->Options3D.flags |= VIRGL_RESOURCE_FLAG_MAP_COHERENT;

                surfaceData->Pitch = surfaceData->Width * 4;
                DbgPrint(TRACE_LEVEL_ERROR,
                         ("<--- %s shadow surface: width=%d, height=%d, format=%d\n",
                          __FUNCTION__,
                          surfaceData->Width,
                          surfaceData->Height,
                          surfaceData->Format));
                return STATUS_SUCCESS;
            }

        case D3DKMDT_STANDARDALLOCATION_STAGINGSURFACE:
            {
                D3DKMDT_STAGINGSURFACEDATA *surfaceData = pStandardAllocation->pCreateStagingSurfaceData;
                //[in] UINT                           Width;
                //[in] UINT                           Height;

                if ((surfaceData == NULL) || (surfaceData->Width == 0) ||
                    (surfaceData->Height == 0) ||
                    (surfaceData->Width > MAXULONG / 4))
                {
                    return STATUS_INVALID_PARAMETER;
                }

                allocationExchange->Options3D.width = surfaceData->Width;
                allocationExchange->Options3D.height = surfaceData->Height;
                /* Vista's staging-surface descriptor has no format field.
                 * The WDDM 1.0 staging contract uses an XRGB surface. */
                allocationExchange->Options3D.format = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
                allocationExchange->Size = (ULONGLONG)surfaceData->Width * (ULONGLONG)surfaceData->Height * 4;

                allocationExchange->Options3D.flags |= VIRGL_RESOURCE_FLAG_MAP_COHERENT;

                surfaceData->Pitch = surfaceData->Width * 4;
                DbgPrint(TRACE_LEVEL_ERROR, ("<--- %s staging surface\n", __FUNCTION__));
                return STATUS_SUCCESS;
            }

        default:
            {
                DbgPrint(TRACE_LEVEL_FATAL, ("<--- Unknown standard allocation type \n"));
                return STATUS_NOT_SUPPORTED;
            }
    }
}

NTSTATUS VioGpuAllocation::DxgkCreateAllocation(VioGpuAdapter *adapter, DXGKARG_CREATEALLOCATION *pCreateAllocation)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    if ((adapter == NULL) || (pCreateAllocation == NULL) ||
        (pCreateAllocation->NumAllocations == 0) ||
        (pCreateAllocation->pAllocationInfo == NULL))
    {
        return STATUS_INVALID_PARAMETER;
    }

    pCreateAllocation->hResource = NULL;
    for (UINT i = 0; i < pCreateAllocation->NumAllocations; ++i)
    {
        pCreateAllocation->pAllocationInfo[i].hAllocation = NULL;
    }

    VioGpuResource *resource = NULL;
    if (pCreateAllocation->Flags.Resource)
    {
        resource = new (VIOGPU_NONPAGED_POOL) VioGpuResource();
        if (resource == NULL)
        {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    NTSTATUS status = STATUS_SUCCESS;
    VioGpuAllocation *primaryAllocation = NULL;
    for (UINT allocationIndex = 0;
         allocationIndex < pCreateAllocation->NumAllocations;
         ++allocationIndex)
    {
        DXGK_ALLOCATIONINFO *allocationInfo =
            &pCreateAllocation->pAllocationInfo[allocationIndex];
        VIOGPU_CREATE_ALLOCATION_EXCHANGE *resourceExchange = NULL;

        if ((allocationInfo->pPrivateDriverData != NULL) &&
            (allocationInfo->PrivateDriverDataSize >= sizeof(*resourceExchange)))
        {
            resourceExchange = (VIOGPU_CREATE_ALLOCATION_EXCHANGE *)
                allocationInfo->pPrivateDriverData;
        }
        else if ((pCreateAllocation->pPrivateDriverData != NULL) &&
                 (pCreateAllocation->PrivateDriverDataSize >= sizeof(*resourceExchange)))
        {
            resourceExchange = (VIOGPU_CREATE_ALLOCATION_EXCHANGE *)
                pCreateAllocation->pPrivateDriverData;
        }
        else
        {
            DbgPrint(TRACE_LEVEL_ERROR,
                     ("<--- %s allocation %u has no complete private data\n",
                      __FUNCTION__, allocationIndex));
            status = STATUS_INVALID_PARAMETER;
            goto CreateAllocationFailed;
        }

        const ULONGLONG apertureSize = (ULONGLONG)256 * 1024 * 4096;
        if ((resourceExchange->Size == 0) ||
            ((ULONGLONG)(SIZE_T)resourceExchange->Size != resourceExchange->Size))
        {
            status = STATUS_INVALID_PARAMETER;
            goto CreateAllocationFailed;
        }

        switch (resourceExchange->Type)
        {
            case VIOGPU_RESOURCE_TYPE_3D:
                if ((resourceExchange->Options3D.width == 0) ||
                    (resourceExchange->Options3D.height == 0) ||
                    (resourceExchange->Options3D.depth == 0) ||
                    (resourceExchange->Options3D.array_size == 0) ||
                    (resourceExchange->Size > apertureSize))
                {
                    status = STATUS_INVALID_PARAMETER;
                    goto CreateAllocationFailed;
                }
                break;
            case VIOGPU_RESOURCE_TYPE_BLOB:
                if ((resourceExchange->OptionsBlob.blob_mem < VIOGPU_BLOB_MEM_GUEST) ||
                    (resourceExchange->OptionsBlob.blob_mem > VIOGPU_BLOB_MEM_HOST3D_GUEST) ||
                    ((resourceExchange->OptionsBlob.blob_flags &
                      ~(VIOGPU_BLOB_FLAG_USE_MAPPABLE |
                        VIOGPU_BLOB_FLAG_USE_SHAREABLE |
                        VIOGPU_BLOB_FLAG_PINNED)) != 0) ||
                    ((resourceExchange->OptionsBlob.blob_mem != VIOGPU_BLOB_MEM_GUEST) &&
                     (!adapter->HasUsableShmem() ||
                      resourceExchange->Size > adapter->GetUsableShmemSize())) ||
                    ((resourceExchange->OptionsBlob.blob_mem == VIOGPU_BLOB_MEM_GUEST) &&
                     resourceExchange->Size > apertureSize))
                {
                    status = STATUS_INVALID_PARAMETER;
                    goto CreateAllocationFailed;
                }
                break;
            case VIOGPU_RESOURCE_TYPE_IMPORT:
                if ((resourceExchange->OptionsImport.res_id == 0) ||
                    !adapter->HasUsableShmem() ||
                    (resourceExchange->Size > adapter->GetUsableShmemSize()))
                {
                    status = STATUS_INVALID_PARAMETER;
                    goto CreateAllocationFailed;
                }
                break;
            case VIOGPU_RESOURCE_TYPE_SHARED:
                if ((resourceExchange->Size > apertureSize) ||
                    !IsValidVistaSharedTexture(&resourceExchange->OptionsShared,
                                               resourceExchange->Size) ||
                    (resourceExchange->OptionsShared.primary &&
                     primaryAllocation != NULL))
                {
                    status = STATUS_INVALID_PARAMETER;
                    goto CreateAllocationFailed;
                }
                break;
            default:
                DbgPrint(TRACE_LEVEL_ERROR,
                         ("<--- %s invalid resource type %d\n",
                          __FUNCTION__, resourceExchange->Type));
                status = STATUS_INVALID_PARAMETER;
                goto CreateAllocationFailed;
        }

        VioGpuAllocation *allocation = NULL;
        switch (resourceExchange->Type) {
        case VIOGPU_RESOURCE_TYPE_3D:
            allocation = new (VIOGPU_NONPAGED_POOL) VioGpuAllocation(adapter, &resourceExchange->Options3D, resourceExchange->Size);
            break;
        case VIOGPU_RESOURCE_TYPE_BLOB:
            // Actual resource creation is deferred to a later time (render)
            allocation = new (VIOGPU_NONPAGED_POOL) VioGpuAllocation(adapter, &resourceExchange->OptionsBlob, resourceExchange->Size);
            break;
        case VIOGPU_RESOURCE_TYPE_IMPORT:
            // Adopts an existing host res_id; no mint, no RESOURCE_CREATE_BLOB.
            allocation = new (VIOGPU_NONPAGED_POOL) VioGpuAllocation(adapter, &resourceExchange->OptionsImport, resourceExchange->Size);
            break;
        case VIOGPU_RESOURCE_TYPE_SHARED:
            // Blob-backed shared/presentable texture (dmabuf export staged by
            // the UMD; res_id minted here, bound at first open).
            allocation = new (VIOGPU_NONPAGED_POOL) VioGpuAllocation(adapter, &resourceExchange->OptionsShared, resourceExchange->Size);
            break;
        default:
            status = STATUS_INVALID_PARAMETER;
            goto CreateAllocationFailed;
    }

    if (allocation == NULL)
    {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto CreateAllocationFailed;
    }

    if (!NT_SUCCESS(allocation->GetCreationStatus()))
    {
        status = allocation->GetCreationStatus();
        delete allocation;
        goto CreateAllocationFailed;
    }

    allocationInfo->hAllocation = allocation->ToHandle();

    allocationInfo->Alignment = 0;
    allocationInfo->Size = (SIZE_T)resourceExchange->Size;
    allocationInfo->PitchAlignedSize = 0;
    allocationInfo->HintedBank.Value = 0;
    // Control-ring blobs are CPU-mapped and polled for the device's lifetime;
    // if VidMm DISCARDs their segment-2 backing under VRAM pressure the guest
    // mapping zeroes and the ring wedges (status=0x0).  MAXIMUM priority makes
    // VidMm evict everything else first.
    allocationInfo->AllocationPriority = allocation->IsPinned()
                                             ? D3DDDI_ALLOCATIONPRIORITY_MAXIMUM
                                             : D3DDDI_ALLOCATIONPRIORITY_NORMAL;
    allocationInfo->Flags.Value = 0;
    allocationInfo->MaximumRenamingListLength = 0;
    allocationInfo->pAllocationUsageHint = NULL;
#if !defined(VIOGPU_TARGET_VISTA)
    allocationInfo->PhysicalAdapterIndex = 0;
#endif

    allocationInfo->PreferredSegment.Value = 0;

    switch (resourceExchange->Type) {
        case VIOGPU_RESOURCE_TYPE_3D:
            DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s 3d res_id=%d size=%d %dx%d\n",
                                           __FUNCTION__,
                                           allocation->GetId(),
                                           allocationInfo->Size,
                                           resourceExchange->Options3D.width,
                                           resourceExchange->Options3D.height));
            allocationInfo->EvictionSegmentSet = 0;
#if defined(VIOGPU_TARGET_VISTA)
            if (allocation->IsPrimary())
            {
                allocationInfo->PreferredSegment.SegmentId0 =
                    VioGpuAdapter::FRAMEBUFFER_SEGMENT_ID;
                allocationInfo->SupportedReadSegmentSet =
                    VioGpuAdapter::FRAMEBUFFER_SEGMENT_SET;
                allocationInfo->SupportedWriteSegmentSet =
                    VioGpuAdapter::FRAMEBUFFER_SEGMENT_SET;
            }
            else
#endif
            {
                allocationInfo->PreferredSegment.SegmentId0 =
                    VioGpuAdapter::APERTURE_SEGMENT_ID;
                allocationInfo->SupportedReadSegmentSet =
                    VioGpuAdapter::APERTURE_SEGMENT_SET;
                allocationInfo->SupportedWriteSegmentSet =
                    VioGpuAdapter::APERTURE_SEGMENT_SET;
            }
            allocationInfo->PreferredSegment.Direction0 = 0;
            allocationInfo->Flags.CpuVisible = TRUE;
            if (allocation->IsPrimary())
            {
                primaryAllocation = allocation;
            }
            break;
        case VIOGPU_RESOURCE_TYPE_BLOB:
            DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s blob res_id=%d size=%d\n",
                                           __FUNCTION__,
                                           allocation->GetId(),
                                           allocationInfo->Size));
            if (allocation->IsGuestBlob())
            {
                //allocationInfo->EvictionSegmentSet = 0b01; // don't use apperture for eviction
                allocationInfo->PreferredSegment.SegmentId0 =
                    VioGpuAdapter::APERTURE_SEGMENT_ID;
                allocationInfo->PreferredSegment.Direction0 = 0;
                allocationInfo->Flags.CpuVisible = TRUE;
                allocationInfo->SupportedReadSegmentSet =
                    VioGpuAdapter::APERTURE_SEGMENT_SET;
                allocationInfo->SupportedWriteSegmentSet =
                    VioGpuAdapter::APERTURE_SEGMENT_SET;
            }
            else
            {
                // EvictionSegmentSet lists the segments an allocation may be
                // evicted TO, which must be aperture/system memory. Segment 2
                // is the non-aperture host shmem BAR (the residence, not an
                // eviction target); naming it here is invalid and the video
                // memory manager rejects the allocation (STATUS_INVALID_-
                // PARAMETER). Leave it 0, matching the guest-blob branch:
                // these host-backed BAR blobs are effectively pinned.
                allocationInfo->PreferredSegment.SegmentId0 =
                    VioGpuAdapter::SHMEM_SEGMENT_ID;
                allocationInfo->PreferredSegment.Direction0 = 0;
                allocationInfo->Flags.CpuVisible = !!(resourceExchange->OptionsBlob.blob_flags & VIOGPU_BLOB_FLAG_USE_MAPPABLE);
                // AccessedPhysically + ExplicitResidencyNotification were both
                // rejected by the video memory manager here (STATUS_INVALID_-
                // PARAMETER on D3DKMTCreateAllocation, observed on the Neptune
                // ring). The working 3D/guest-blob branches set neither, so
                // mirror them: a CpuVisible allocation in the CpuVisible shmem
                // segment is addressed through its CpuTranslatedAddress.
                // allocationInfo->Flags.Swizzled = TRUE;
                allocationInfo->SupportedReadSegmentSet =
                    VioGpuAdapter::SHMEM_SEGMENT_SET;
                allocationInfo->SupportedWriteSegmentSet =
                    VioGpuAdapter::SHMEM_SEGMENT_SET;
            }
            break;
        case VIOGPU_RESOURCE_TYPE_IMPORT:
            DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s import res_id=%d size=%lld\n",
                                           __FUNCTION__,
                                           allocation->GetId(),
                                           allocationInfo->Size));
            // Host-backed alias of an existing dmabuf res_id: residency matches
            // a non-mappable HOST3D blob (host shmem BAR segment, pinned; not
            // CpuVisible -- the guest never maps the host dmabuf).
            allocationInfo->PreferredSegment.SegmentId0 =
                VioGpuAdapter::SHMEM_SEGMENT_ID;
            allocationInfo->PreferredSegment.Direction0 = 0;
            allocationInfo->Flags.CpuVisible = FALSE;
            allocationInfo->SupportedReadSegmentSet =
                VioGpuAdapter::SHMEM_SEGMENT_SET;
            allocationInfo->SupportedWriteSegmentSet =
                VioGpuAdapter::SHMEM_SEGMENT_SET;
            break;
        case VIOGPU_RESOURCE_TYPE_SHARED:
            // Vista constructs a primary's CPU mapping from the segment's
            // CpuTranslatedAddress. An aperture has no fixed CPU address;
            // placing a primary there would map unrelated physical RAM.
            // Keep primary CPU backing in the framebuffer BAR, independently
            // of the shared texture's exported HOST3D storage.
            DbgPrint(TRACE_LEVEL_VERBOSE, ("<--- %s shared res_id=%d blob_id=0x%llx size=%d %dx%d primary=%d\n",
                                           __FUNCTION__,
                                           allocation->GetId(),
                                           resourceExchange->OptionsShared.blob_id,
                                           allocationInfo->Size,
                                           resourceExchange->OptionsShared.width,
                                           resourceExchange->OptionsShared.height,
                                           resourceExchange->OptionsShared.primary));
            allocationInfo->EvictionSegmentSet = 0;
#if defined(VIOGPU_TARGET_VISTA)
            if (allocation->IsPrimary())
            {
                allocationInfo->PreferredSegment.SegmentId0 =
                    VioGpuAdapter::FRAMEBUFFER_SEGMENT_ID;
                allocationInfo->SupportedReadSegmentSet =
                    VioGpuAdapter::FRAMEBUFFER_SEGMENT_SET;
                allocationInfo->SupportedWriteSegmentSet =
                    VioGpuAdapter::FRAMEBUFFER_SEGMENT_SET;
            }
            else
#endif
            {
                allocationInfo->PreferredSegment.SegmentId0 =
                    VioGpuAdapter::APERTURE_SEGMENT_ID;
                allocationInfo->SupportedReadSegmentSet =
                    VioGpuAdapter::APERTURE_SEGMENT_SET;
                allocationInfo->SupportedWriteSegmentSet =
                    VioGpuAdapter::APERTURE_SEGMENT_SET;
            }
            allocationInfo->PreferredSegment.Direction0 = 0;
            allocationInfo->Flags.CpuVisible = TRUE;
            if (resourceExchange->OptionsShared.primary)
            {
                primaryAllocation = allocation;
            }
            break;
    }

    }

    if (resource != NULL)
    {
        pCreateAllocation->hResource = resource->ToHandle();
    }
#if defined(VIOGPU_TARGET_VISTA)
    // Allocation is not a display-ownership transition. Selecting a new
    // primary here exposes unrendered pixels and increments the generation
    // behind queued flips. SetVidPnSourceAddress and ordered Present
    // completion are the authoritative scanout transitions on Vista.
    UNREFERENCED_PARAMETER(primaryAllocation);
#else
    if (primaryAllocation != NULL)
    {
        adapter->vidpn.SetScanoutSource(primaryAllocation);
    }
#endif

    return STATUS_SUCCESS;

CreateAllocationFailed:
    for (UINT i = 0; i < pCreateAllocation->NumAllocations; ++i)
    {
        VioGpuAllocation *created = VioGpuAllocation::FromHandle(
            pCreateAllocation->pAllocationInfo[i].hAllocation);
        pCreateAllocation->pAllocationInfo[i].hAllocation = NULL;
        if (created != NULL)
        {
            created->Release();
        }
    }
    delete resource;
    pCreateAllocation->hResource = NULL;
    return status;
}

// Minimal DXGI_FORMAT -> D3DDDIFORMAT mapping for the shared-allocation
// describe path. Numeric DXGI values avoid an interface-only dxgiformat.h
// dependency in the miniport. Unknown formats are not valid shared resources.
static D3DDDIFORMAT VioGpuDxgiFormatToD3DDDI(UINT dxgiFormat)
{
    switch (dxgiFormat) {
    case 87: case 91: /* BGRA8 UNORM / sRGB */ return D3DDDIFMT_A8R8G8B8;
    case 88: case 93: /* BGRX8 UNORM / sRGB */ return D3DDDIFMT_X8R8G8B8;
    case 28: case 29: /* RGBA8 UNORM / sRGB */ return D3DDDIFMT_A8B8G8R8;
    case 24: /* DXGI_FORMAT_R10G10B10A2_UNORM */ return D3DDDIFMT_A2B10G10R10;
    case 10: /* DXGI_FORMAT_R16G16B16A16_FLOAT */ return D3DDDIFMT_A16B16G16R16F;
    default: return D3DDDIFMT_UNKNOWN;
    }
}

NTSTATUS VioGpuAllocation::DescribeAllocation(DXGKARG_DESCRIBEALLOCATION *pDescribeAllocation)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s res_id=%d\n", __FUNCTION__, m_Id));

    auto lock_guard = LockGuard();

    // Refresh rate: active VidPN mode's rate if a source is pinned,
    // otherwise 60/1 as a safe default.
    D3DDDI_RATIONAL refresh = m_adapter->vidpn.GetActiveRefreshRate();
    if (refresh.Numerator && refresh.Denominator)
    {
        pDescribeAllocation->RefreshRate = refresh;
    }
    else
    {
        pDescribeAllocation->RefreshRate.Numerator = 60;
        pDescribeAllocation->RefreshRate.Denominator = 1;
    }

    if (m_IsShared) {
        // Host-COM-backed share: dimensions/format come from the producer's
        // descriptor; there is no virtio resource to query.
        pDescribeAllocation->Width = m_SharedWidth;
        pDescribeAllocation->Height = m_SharedHeight;
        pDescribeAllocation->PrivateDriverFormatAttribute = 0;
        pDescribeAllocation->Format = VioGpuDxgiFormatToD3DDDI(m_SharedFormat);
        pDescribeAllocation->MultisampleMethod.NumQualityLevels = 0;
        pDescribeAllocation->MultisampleMethod.NumSamples = 1;
        return STATUS_SUCCESS;
    }

    if (m_IsBlob) {
        if (!m_Blob.InfoValid) {
            DbgPrint(TRACE_LEVEL_ERROR, ("<---> %s res_id=%d does not have valid info\n", __FUNCTION__, m_Id));
            return STATUS_INVALID_PARAMETER;
        }

        pDescribeAllocation->Width = m_Blob.Info.width;
        pDescribeAllocation->Height = m_Blob.Info.height;
        pDescribeAllocation->PrivateDriverFormatAttribute = 0;

        pDescribeAllocation->Format = VioGpuToD3DDDIColorFormat((virtio_gpu_formats)m_Blob.Info.format);
    } else {
        pDescribeAllocation->Width = m_3dOptions.width;
        pDescribeAllocation->Height = m_3dOptions.height;
        pDescribeAllocation->PrivateDriverFormatAttribute = 0;

        pDescribeAllocation->Format = VioGpuToD3DDDIColorFormat((virtio_gpu_formats)m_3dOptions.format);
    }

    // Multisample mirrors the resource's nr_samples: 0 or 1 means
    // non-MSAA, anything else is MSAA with a single quality level
    // (vendor-specific quality variants are not surfaced). Blob
    // resources don't expose nr_samples, so treat them as non-MSAA.
    UINT nr_samples = m_IsBlob ? 0 : m_3dOptions.nr_samples;
    if (nr_samples > 1)
    {
        pDescribeAllocation->MultisampleMethod.NumQualityLevels = 1;
        pDescribeAllocation->MultisampleMethod.NumSamples = (UCHAR)nr_samples;
    }
    else
    {
        pDescribeAllocation->MultisampleMethod.NumQualityLevels = 0;
        pDescribeAllocation->MultisampleMethod.NumSamples = 1;
    }

    return STATUS_SUCCESS;
};

NTSTATUS VioGpuAllocation::MapApertureSegment(DXGKARG_BUILDPAGINGBUFFER *pBuildPagingBuffer)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s res_id=%d\n", __FUNCTION__, m_Id));

    size_t pageCount = pBuildPagingBuffer->MapApertureSegment.NumberOfPages;
    size_t mdlPageOffset = pBuildPagingBuffer->MapApertureSegment.MdlOffset;

    MDL *pMdl = pBuildPagingBuffer->MapApertureSegment.pMdl;

    if (pBuildPagingBuffer->MapApertureSegment.SegmentId !=
            VioGpuAdapter::APERTURE_SEGMENT_ID ||
        pMdl == NULL || pageCount == 0 ||
        pBuildPagingBuffer->MapApertureSegment.OffsetInPages >
            VioGpuAdapter::APERTURE_SIZE / PAGE_SIZE ||
        pageCount > VioGpuAdapter::APERTURE_SIZE / PAGE_SIZE -
            pBuildPagingBuffer->MapApertureSegment.OffsetInPages)
    {
        return STATUS_INVALID_PARAMETER;
    }

    if (!IsBlob() || IsGuestBlob()) {
        NTSTATUS status = AttachBacking(pMdl, pageCount, mdlPageOffset);
        if (!NT_SUCCESS(status))
        {
            return status;
        }
        SetDxPhysicalAddress(pBuildPagingBuffer->MapApertureSegment.OffsetInPages * PAGE_SIZE);
        return STATUS_SUCCESS;
    }

    // Shared host textures use the aperture only for VidMm residency.  Keep
    // the supplied pages so WDDM 1.0 FILL/TRANSFER operations have real
    // backing, but do not issue RESOURCE_ATTACH_BACKING for a HOST3D blob.
    // Its authoritative storage is the exported host texture.
    NTSTATUS status = StoreBacking(pMdl, pageCount, mdlPageOffset, FALSE);
    if (!NT_SUCCESS(status))
    {
        return status;
    }
    SetDxPhysicalAddress(pBuildPagingBuffer->MapApertureSegment.OffsetInPages * PAGE_SIZE);
    return STATUS_SUCCESS;
}

NTSTATUS VioGpuAllocation::UnmapApertureSegment(DXGKARG_BUILDPAGINGBUFFER *pBuildPagingBuffer)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s res_id=%d\n", __FUNCTION__, m_Id));

    if (pBuildPagingBuffer->UnmapApertureSegment.SegmentId !=
        VioGpuAdapter::APERTURE_SEGMENT_ID)
    {
        return STATUS_INVALID_PARAMETER;
    }
    DetachBacking();
    SetDxPhysicalAddress(0);
    return STATUS_SUCCESS;
}

static NTSTATUS VioGpuMapMdlPages(MDL *pMdl, SIZE_T pageOffset,
                                  SIZE_T byteCount, BYTE **ppBytes)
{
    if (pMdl == NULL || ppBytes == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    SIZE_T mdlPages = ADDRESS_AND_SIZE_TO_SPAN_PAGES(
        MmGetMdlVirtualAddress(pMdl), MmGetMdlByteCount(pMdl));
    if (pageOffset > mdlPages ||
        byteCount > (mdlPages - pageOffset) * (SIZE_T)PAGE_SIZE)
    {
        return STATUS_INVALID_PARAMETER;
    }

    PVOID mapped = MmGetSystemAddressForMdlSafe(pMdl, NormalPagePriority);
    if (mapped == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // MmGetSystemAddressForMdlSafe includes the MDL's byte offset.  Paging
    // offsets index the PFN array, so start from the first mapped page.
    *ppBytes = (BYTE *)mapped - MmGetMdlByteOffset(pMdl) +
               pageOffset * (SIZE_T)PAGE_SIZE;
    return STATUS_SUCCESS;
}

NTSTATUS VioGpuAllocation::CopyToFixedPrimary(
    VioGpuAllocation *source,
    const RECT *destinationRects,
    UINT rectCount,
    LONG sourceDeltaX,
    LONG sourceDeltaY)
{
    PAGED_CODE();

#if !defined(VIOGPU_TARGET_VISTA)
    UNREFERENCED_PARAMETER(source);
    UNREFERENCED_PARAMETER(destinationRects);
    UNREFERENCED_PARAMETER(rectCount);
    UNREFERENCED_PARAMETER(sourceDeltaX);
    UNREFERENCED_PARAMETER(sourceDeltaY);
    return STATUS_NOT_SUPPORTED;
#else
    if (source == NULL || source == this || destinationRects == NULL ||
        rectCount == 0 || m_adapter != source->m_adapter)
    {
        return STATUS_INVALID_PARAMETER;
    }

    // Present holds both allocations busy before it gets here.  Lock their
    // backing snapshots as well so a paging map/unmap cannot change the MDL
    // or fixed-segment offset while rows are copied.
    auto sourceLock = source->LockGuard();
    auto destinationLock = LockGuard();

    if (!m_IsPrimary || m_IsBlob || source->m_IsBlob ||
        !m_BackingAttachedToHost || m_pMDL != NULL ||
        source->m_pMDL == NULL || source->m_pageCount == 0 ||
        source->m_pageCount > ((SIZE_T)-1 / PAGE_SIZE) ||
        !IsLinear32BppVirtioFormat(m_3dOptions.format) ||
        source->m_3dOptions.format != m_3dOptions.format)
    {
        return STATUS_INVALID_DEVICE_STATE;
    }

    const SIZE_T sourceBackingBytes =
        source->m_pageCount * (SIZE_T)PAGE_SIZE;
    BYTE *sourceBytes = NULL;
    NTSTATUS status = VioGpuMapMdlPages(source->m_pMDL,
                                        source->m_pageOffset,
                                        sourceBackingBytes,
                                        &sourceBytes);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    if (m_DxPhysicalAddress > MAXULONG ||
        m_Size > (ULONGLONG)(SIZE_T)-1 ||
        m_Size > m_adapter->GetFrameBufferSize() ||
        m_DxPhysicalAddress >
            m_adapter->GetFrameBufferSize() - (SIZE_T)m_Size)
    {
        return STATUS_INVALID_DEVICE_STATE;
    }
    BYTE *destinationBytes = (BYTE *)m_adapter->GetFrameBufferVA(
        (ULONG)m_DxPhysicalAddress);
    if (destinationBytes == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    for (UINT rectIndex = 0; rectIndex < rectCount; ++rectIndex)
    {
        const RECT &rect = destinationRects[rectIndex];
        const LONGLONG sourceX64 = (LONGLONG)rect.left + sourceDeltaX;
        const LONGLONG sourceY64 = (LONGLONG)rect.top + sourceDeltaY;
        if (rect.left < 0 || rect.top < 0 ||
            rect.right <= rect.left || rect.bottom <= rect.top ||
            sourceX64 < 0 || sourceY64 < 0 ||
            sourceX64 > MAXLONG || sourceY64 > MAXLONG)
        {
            return STATUS_INVALID_PARAMETER;
        }

        ULONG sourceStride = 0;
        ULONG destinationStride = 0;
        ULONGLONG sourceOffset = 0;
        ULONGLONG destinationOffset = 0;
        if (!source->GetTransferLayout((LONG)sourceX64,
                                       (LONG)sourceY64,
                                       &sourceStride,
                                       &sourceOffset) ||
            !GetTransferLayout(rect.left, rect.top,
                               &destinationStride,
                               &destinationOffset))
        {
            return STATUS_INVALID_PARAMETER;
        }

        const SIZE_T rowBytes =
            (SIZE_T)(rect.right - rect.left) * sizeof(ULONG);
        const SIZE_T rowCount = (SIZE_T)(rect.bottom - rect.top);
        if (rowBytes > sourceStride || rowBytes > destinationStride ||
            sourceOffset > sourceBackingBytes ||
            rowBytes > sourceBackingBytes - (SIZE_T)sourceOffset ||
            rowCount - 1 >
                (sourceBackingBytes - (SIZE_T)sourceOffset - rowBytes) /
                    sourceStride ||
            destinationOffset > (SIZE_T)m_Size ||
            rowBytes > (SIZE_T)m_Size - (SIZE_T)destinationOffset ||
            rowCount - 1 >
                ((SIZE_T)m_Size - (SIZE_T)destinationOffset - rowBytes) /
                    destinationStride)
        {
            return STATUS_INVALID_PARAMETER;
        }

        BYTE *sourceRow = sourceBytes + (SIZE_T)sourceOffset;
        BYTE *destinationRow = destinationBytes +
                               (SIZE_T)destinationOffset;
        for (SIZE_T row = 0; row < rowCount; ++row)
        {
            RtlMoveMemory(destinationRow, sourceRow, rowBytes);
            sourceRow += sourceStride;
            destinationRow += destinationStride;
        }
    }

    return STATUS_SUCCESS;
#endif
}

NTSTATUS VioGpuAllocation::PagingTransfer(
    DXGKARG_BUILDPAGINGBUFFER *pBuildPagingBuffer)
{
    PAGED_CODE();
    if (pBuildPagingBuffer == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    const UINT sourceSegment = pBuildPagingBuffer->Transfer.Source.SegmentId;
    const UINT destinationSegment = pBuildPagingBuffer->Transfer.Destination.SegmentId;
    const SIZE_T transferOffset = pBuildPagingBuffer->Transfer.TransferOffset;
    const SIZE_T transferSize = pBuildPagingBuffer->Transfer.TransferSize;
    const BOOLEAN apertureTransfer =
        (sourceSegment == VioGpuAdapter::APERTURE_SEGMENT_ID &&
         destinationSegment == 0) ||
        (sourceSegment == 0 &&
         destinationSegment == VioGpuAdapter::APERTURE_SEGMENT_ID);
#if defined(VIOGPU_TARGET_VISTA)
    const BOOLEAN frameBufferTransfer =
        (sourceSegment == VioGpuAdapter::FRAMEBUFFER_SEGMENT_ID &&
         destinationSegment == 0) ||
        (sourceSegment == 0 &&
         destinationSegment == VioGpuAdapter::FRAMEBUFFER_SEGMENT_ID);
#else
    const BOOLEAN frameBufferTransfer = FALSE;
#endif
    if (!apertureTransfer && !frameBufferTransfer)
    {
        return STATUS_INVALID_PARAMETER;
    }
    if (transferSize == 0)
    {
        return STATUS_SUCCESS;
    }

    auto lock_guard = LockGuard();
    const PHYSICAL_ADDRESS segmentAddress =
        sourceSegment != 0
        ? pBuildPagingBuffer->Transfer.Source.SegmentAddress
        : pBuildPagingBuffer->Transfer.Destination.SegmentAddress;
    BYTE *allocationBytes = NULL;
    BYTE *systemBytes = NULL;

    if (frameBufferTransfer)
    {
#if defined(VIOGPU_TARGET_VISTA)
        const ULONGLONG segmentBase = VioGpuAdapter::FRAMEBUFFER_GPU_BASE_VA;
        const ULONGLONG segmentSize = m_adapter->GetFrameBufferSize();
        const ULONGLONG roundedAllocationSize =
            (m_Size + PAGE_SIZE - 1) & ~((ULONGLONG)PAGE_SIZE - 1);
        if (!m_IsPrimary || (m_IsBlob && !m_IsShared) || roundedAllocationSize == 0 ||
            segmentAddress.QuadPart < segmentBase)
        {
            return STATUS_INVALID_PARAMETER;
        }

        const ULONGLONG allocationOffset =
            segmentAddress.QuadPart - segmentBase;
        if (allocationOffset > MAXULONG || allocationOffset > segmentSize ||
            roundedAllocationSize > segmentSize - allocationOffset ||
            transferOffset > roundedAllocationSize ||
            transferSize > roundedAllocationSize - transferOffset ||
            transferOffset > MAXULONG - allocationOffset)
        {
            return STATUS_INVALID_PARAMETER;
        }

        allocationBytes = (BYTE *)m_adapter->GetFrameBufferVA(
            (ULONG)(allocationOffset + transferOffset));
        if (allocationBytes == NULL)
        {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        SetDxPhysicalAddress((SIZE_T)allocationOffset);
#endif
    }
    else
    {
        if (m_pMDL == NULL ||
            m_pageCount > ((SIZE_T)-1 / PAGE_SIZE) ||
            transferOffset > m_pageCount * (SIZE_T)PAGE_SIZE ||
            transferSize > m_pageCount * (SIZE_T)PAGE_SIZE - transferOffset)
        {
            return STATUS_INVALID_DEVICE_STATE;
        }
        if (segmentAddress.QuadPart !=
            VioGpuAdapter::APERTURE_GPU_BASE_VA + m_DxPhysicalAddress)
        {
            return STATUS_INVALID_PARAMETER;
        }

        NTSTATUS status = VioGpuMapMdlPages(
            m_pMDL, m_pageOffset, m_pageCount * (SIZE_T)PAGE_SIZE,
            &allocationBytes);
        if (!NT_SUCCESS(status))
        {
            return status;
        }
        allocationBytes += transferOffset;
    }

    MDL *systemMdl = sourceSegment == 0
        ? pBuildPagingBuffer->Transfer.Source.pMdl
        : pBuildPagingBuffer->Transfer.Destination.pMdl;
    NTSTATUS status = VioGpuMapMdlPages(
        systemMdl, pBuildPagingBuffer->Transfer.MdlOffset,
        transferSize, &systemBytes);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    if (sourceSegment == 0)
    {
        RtlMoveMemory(allocationBytes, systemBytes, transferSize);
    }
    else
    {
        RtlMoveMemory(systemBytes, allocationBytes, transferSize);
    }
#if defined(VIOGPU_TARGET_VISTA)
    if (frameBufferTransfer)
    {
        return AttachFrameBufferBackingLocked(segmentAddress);
    }
#endif
    return STATUS_SUCCESS;
}

NTSTATUS VioGpuAllocation::PagingFill(
    DXGKARG_BUILDPAGINGBUFFER *pBuildPagingBuffer)
{
    PAGED_CODE();
    if (pBuildPagingBuffer == NULL ||
        (pBuildPagingBuffer->Fill.FillSize & (sizeof(ULONG) - 1)) != 0)
    {
        return STATUS_INVALID_PARAMETER;
    }
    if (pBuildPagingBuffer->Fill.FillSize == 0)
    {
        return STATUS_SUCCESS;
    }

    auto lock_guard = LockGuard();
    const SIZE_T fillSize = pBuildPagingBuffer->Fill.FillSize;

#if defined(VIOGPU_TARGET_VISTA)
    if (pBuildPagingBuffer->Fill.Destination.SegmentId ==
        VioGpuAdapter::FRAMEBUFFER_SEGMENT_ID)
    {
        const ULONGLONG segmentBase = VioGpuAdapter::FRAMEBUFFER_GPU_BASE_VA;
        const ULONGLONG segmentSize = m_adapter->GetFrameBufferSize();
        const ULONGLONG segmentAddress =
            pBuildPagingBuffer->Fill.Destination.SegmentAddress.QuadPart;
        const ULONGLONG roundedAllocationSize =
            (m_Size + PAGE_SIZE - 1) & ~((ULONGLONG)PAGE_SIZE - 1);
        if (!m_IsPrimary || (m_IsBlob && !m_IsShared) || roundedAllocationSize == 0 ||
            segmentAddress < segmentBase)
        {
            return STATUS_INVALID_PARAMETER;
        }

        const ULONGLONG allocationOffset = segmentAddress - segmentBase;
        if (allocationOffset > MAXULONG || allocationOffset > segmentSize ||
            roundedAllocationSize > segmentSize - allocationOffset ||
            fillSize > roundedAllocationSize)
        {
            return STATUS_INVALID_PARAMETER;
        }

        ULONG *destination = (ULONG *)m_adapter->GetFrameBufferVA(
            (ULONG)allocationOffset);
        if (destination == NULL)
        {
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        const SIZE_T valueCount = fillSize / sizeof(ULONG);
        for (SIZE_T i = 0; i < valueCount; ++i)
        {
            destination[i] = pBuildPagingBuffer->Fill.FillPattern;
        }
        return AttachFrameBufferBackingLocked(
            pBuildPagingBuffer->Fill.Destination.SegmentAddress);
    }

    if (pBuildPagingBuffer->Fill.Destination.SegmentId ==
        VioGpuAdapter::SHMEM_SEGMENT_ID)
    {
        const ULONGLONG segmentBase = VioGpuAdapter::SHMEM_GPU_BASE_VA;
        const ULONGLONG segmentSize = m_adapter->GetUsableShmemSize();
        const ULONGLONG segmentAddress =
            pBuildPagingBuffer->Fill.Destination.SegmentAddress.QuadPart;
        const ULONGLONG roundedAllocationSize =
            (m_Size + PAGE_SIZE - 1) & ~((ULONGLONG)PAGE_SIZE - 1);

        if (!m_IsBlob || IsGuestBlob() || segmentSize == 0 ||
            roundedAllocationSize == 0 || segmentAddress < segmentBase)
        {
            return STATUS_INVALID_PARAMETER;
        }

        const ULONGLONG allocationOffset = segmentAddress - segmentBase;
        if (allocationOffset > segmentSize ||
            roundedAllocationSize > segmentSize - allocationOffset ||
            fillSize > roundedAllocationSize)
        {
            return STATUS_INVALID_PARAMETER;
        }

        // A HOST3D blob has no guest backing store to fill at this point.
        // Its shmem window becomes accessible only after the later
        // RESOURCE_MAP_BLOB command, whose offset is patched into the render
        // allocation list.  Vista nevertheless sends an initial zero-fill
        // while making the allocation resident.  Accept that operation as a
        // validated no-op, as Vista-era virtual WDDM drivers do for
        // host-owned surfaces, and remember the residence offset for the
        // subsequent explicit blob map.
        m_Blob.MapOffset = allocationOffset;
        return STATUS_SUCCESS;
    }
#endif

    if (pBuildPagingBuffer->Fill.Destination.SegmentId !=
        VioGpuAdapter::APERTURE_SEGMENT_ID)
    {
        return STATUS_INVALID_PARAMETER;
    }
    if (m_pMDL == NULL || m_pageCount > ((SIZE_T)-1 / PAGE_SIZE) ||
        fillSize > m_pageCount * (SIZE_T)PAGE_SIZE ||
        pBuildPagingBuffer->Fill.Destination.SegmentAddress.QuadPart !=
            VioGpuAdapter::APERTURE_GPU_BASE_VA + m_DxPhysicalAddress)
    {
        return STATUS_INVALID_DEVICE_STATE;
    }

    BYTE *allocationBytes = NULL;
    NTSTATUS status = VioGpuMapMdlPages(m_pMDL, m_pageOffset, fillSize,
                                        &allocationBytes);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    ULONG *destination = (ULONG *)allocationBytes;
    const SIZE_T valueCount = fillSize / sizeof(ULONG);
    for (SIZE_T i = 0; i < valueCount; ++i)
    {
        destination[i] = pBuildPagingBuffer->Fill.FillPattern;
    }
    return STATUS_SUCCESS;
}

NTSTATUS VioGpuAllocation::EscapeResourceInfo(VIOGPU_RES_INFO_REQ *resInfo)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s res_id=%d isBlob=%d\n", __FUNCTION__, m_Id, m_IsBlob));

    if (resInfo == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    auto lock_guard = LockGuard();

    D3DKMT_HANDLE handle = resInfo->ResHandle;
    RtlZeroMemory(resInfo, sizeof(*resInfo));
    resInfo->ResHandle = handle;
    resInfo->Id = m_Id;
    resInfo->Size = m_Size;
    resInfo->IsCreated = IsCreated();
    if (m_IsBlob)
    {
        resInfo->IsBlob = TRUE;
        resInfo->InfoValid = m_Blob.InfoValid;
        resInfo->BlobMem = m_Blob.Options.blob_mem;
        resInfo->BlobId = m_Blob.Options.blob_id;
        resInfo->Info = m_Blob.Info;
    }
    else
    {
        resInfo->IsBlob = FALSE;
    }

    return STATUS_SUCCESS;
}

NTSTATUS VioGpuAllocation::EscapeResourceBusy(VIOGPU_RES_BUSY_REQ *resBusy)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<---> %s res_id=%d\n", __FUNCTION__, m_Id));

    while (resBusy->Wait && m_busy != 0)
    {
        KeWaitForSingleObject(&m_busyNotification, UserRequest, KernelMode, FALSE, NULL);
    }

    resBusy->IsBusy = m_busy != 0;

    return STATUS_SUCCESS;
}

LinkedList<VioGpuDeviceAllocation>::Entry *VioGpuAllocation::Find(VioGpuDevice *pDevice)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("---> %s\n", __FUNCTION__));

    return m_DeviceAllocations.find([pDevice](VioGpuDeviceAllocation *pDeviceAllocation) [[msvc::forceinline]]
    {
        return pDeviceAllocation->GetDevice() == pDevice;
    });
}

VioGpuDeviceAllocation *VioGpuAllocation::Open(VioGpuDevice *pDevice,
                                                NTSTATUS *pStatus)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--> %s alloc=%p\n", __FUNCTION__, this));
    if (pStatus == NULL || pDevice == NULL)
        return nullptr;
    *pStatus = STATUS_SUCCESS;
    auto lock_guard = LockGuard();

    auto pEntry = Find(pDevice);
    if (pEntry == nullptr)
    {
        pEntry = m_DeviceAllocations.emplace_back(pDevice, this);
        if (pEntry == nullptr)
        {
            *pStatus = STATUS_INSUFFICIENT_RESOURCES;
            return nullptr;
        }
        if (!NT_SUCCESS(pEntry->value.GetStatus()))
        {
            *pStatus = pEntry->value.GetStatus();
            pEntry->value.Unref();
            m_DeviceAllocations.remove(pEntry);
            return nullptr;
        }
    }
    else
    {
        pEntry->value.Ref();
    }

    if (pEntry == nullptr)
    {
        VioGpuDbgBreak();
    }

    VioGpuDeviceAllocation *pDeviceAllocation = &pEntry->value;
    *pStatus = pDeviceAllocation->GetStatus();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--> %s alloc=%p devalloc=%p ref=%lld dev=%p\n", __FUNCTION__, this, pDeviceAllocation, pDeviceAllocation->GetRef(), pDeviceAllocation->GetDevice()));

    return pDeviceAllocation;
}

void VioGpuAllocation::Close(VioGpuDeviceAllocation *pDeviceAllocation)
{
    PAGED_CODE();
    DbgPrint(TRACE_LEVEL_VERBOSE, ("<--> %s alloc=%p devalloc=%p ref=%lld dev=%p\n", __FUNCTION__, this, pDeviceAllocation, pDeviceAllocation->GetRef(), pDeviceAllocation->GetDevice()));
    auto lock_guard = LockGuard();

    auto pEntry = Find(pDeviceAllocation->GetDevice());
    if (pEntry == nullptr || &pEntry->value != pDeviceAllocation)
    {
        DbgPrint(TRACE_LEVEL_ERROR,
                 ("%s allocation=%p has no matching device allocation=%p\n",
                  __FUNCTION__, this, pDeviceAllocation));
        return;
    }

    if (pEntry->value.Unref())
    {
        DbgPrint(TRACE_LEVEL_VERBOSE, ("<--> %s alloc=%p removing devalloc=%p size=%zu\n", __FUNCTION__, this, &pEntry->value, m_DeviceAllocations.size()));
        if (m_IsBlob)
        {
            BOOLEAN issued = FALSE;
            UnmapBlobLocked(pEntry->value.GetCtxId(), NULL, NULL, &issued);
        }
        m_DeviceAllocations.remove(pEntry);
    }
}

PAGED_CODE_SEG_END
