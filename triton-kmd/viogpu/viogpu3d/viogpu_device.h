#pragma once
#include "handle.h"
#include "viogpum.h"
#include "viogpu.h"
#include "viogpu_trace.h"

class VioGpuAdapter;
class VioGpuDevice;
class VioGpuAllocation;
class VioGpuDeviceAllocation;
class CtrlQueue;

class VioGpuDxContext final : public HandleBase<"VIOGCTXT"_M, VioGpuDxContext>
{
  public:
    explicit VioGpuDxContext(VioGpuDevice *device) : m_pDevice(device) {}

    VioGpuDevice *GetDevice() const
    {
        return m_pDevice;
    }

  private:
    VioGpuDevice *m_pDevice;
};

// This class represents the actual VirtIO 3D ctx_id
class VioGpuContext
{
  public:
    VioGpuContext(VioGpuAdapter *pAdapter);
    ~VioGpuContext();

    NTSTATUS Init(VIOGPU_CTX_INIT_REQ *pOptions);

    inline ULONG GetId() const
    {
        return m_id;
    }

    inline BOOLEAN IsVirgl() const {
        return m_Capset == VIRTIO_GPU_CAPSET_VIRGL ||
               m_Capset == VIRTIO_GPU_CAPSET_VIRGL2;
    }

    inline UINT GetCapset() const {
        return m_Capset;
    }

    inline UINT GetNumRings() const {
        return m_NumRings;
    }

    inline BOOL IsEmpty() const
    {
        return m_empty;
    }

  private:
    VioGpuAdapter *m_pAdapter;
    ULONG m_id;
    BOOL m_empty;
    UINT m_Capset;
    UINT m_NumRings;
};

// Class that represents DXGKRNL Context, often passed as hContext
class VioGpuDevice final : public HandleBase<"VIOGDEVI"_M, VioGpuDevice>
{
  friend class VioGpuContext;
  friend class VioGpuCommander;
  public:
    VioGpuTraceTag traceTag = {};
    VioGpuDevice(VioGpuAdapter *pAdapter);
    ~VioGpuDevice();

    inline BOOLEAN CanBlit() const {
        return m_pBlit != nullptr;
    }

    //NTSTATUS Init(VIOGPU_CTX_INIT_REQ *pOptions);

    NTSTATUS OpenAllocation(_In_ CONST DXGKARG_OPENALLOCATION *pOpenAllocation);

    NTSTATUS EnsureVirglAttachment(VioGpuDeviceAllocation *allocation);

    NTSTATUS GenerateBltPresent(DXGKARG_PRESENT *pPresent,
                                VioGpuDeviceAllocation *src,
                                VioGpuDeviceAllocation *dst,
                                VioGpuAllocation **pInitialPresentCompletion);
    NTSTATUS GenerateBltPresentUM(DXGKARG_PRESENT *pPresent, VioGpuAllocation *src, VioGpuAllocation *dst);
    NTSTATUS Present(_Inout_ DXGKARG_PRESENT *pPresent);
    NTSTATUS Render(DXGKARG_RENDER *pRender);

    // Bounded per-device diagnostics: a desktop device must not consume the
    // trace budget of a subsequently created fullscreen application.
    void TracePresent(ULONG stage, ULONG flags, ULONG source,
                      LONG generation, BOOLEAN armed);

    CtrlQueue *GetCtrlQueue();

    VioGpuContext m_Context;
    VioGpuContext m_Virgl;

    PRKEVENT m_hUM, m_hKM;
    volatile PVIOGPU_BLIT_PRESENT m_pBlit;
  protected:
    VioGpuAdapter *m_pAdapter;
    volatile LONG m_presentTraceCount;
};

class VioGpuDeviceAllocation final : public HandleBase<"VIOGDEAL"_M, VioGpuDeviceAllocation>
{
  friend class VioGpuDevice;
  friend class VioGpuAllocation;
  public:
    VioGpuDeviceAllocation(VioGpuDevice *device, VioGpuAllocation *allocation);
    VioGpuDeviceAllocation(const VioGpuDeviceAllocation &other) = delete;
    VioGpuDeviceAllocation& operator=(const VioGpuDeviceAllocation &other) = delete;

    ~VioGpuDeviceAllocation();

    VioGpuAllocation *GetAllocation();
    VioGpuDevice *GetDevice();
    NTSTATUS GetStatus() const { return m_Status; }

    inline ULONG GetCtxId() const
    {
        return m_pDevice->m_Context.GetId();
    }

  protected:
    void Ref()
    {
        InterlockedIncrement64(&m_RefCount);
    }

    bool Unref()
    {
        return InterlockedDecrement64(&m_RefCount) == 0;
    }

    LONGLONG GetRef()
    {
        return m_RefCount;
    }

    bool m_AttachedToVirgl;
  private:
    VioGpuAllocation *m_pAllocation;
    VioGpuDevice *m_pDevice;
    volatile LONGLONG m_RefCount;
    NTSTATUS m_Status;
    // Whether CTX_ATTACH_RESOURCE actually issued. The destructor
    // skips DETACH if FALSE so we never send a stray DETACH for a
    // pair that never attached (e.g., when OpenAllocation's blob
    // create fails after the device-allocation is constructed).
    bool m_attached;
};
