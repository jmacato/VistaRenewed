#pragma once

#include "handle.h"

#define VIOGPU_MAX_RUNNING 1

class VioGpuAdapter;
class VioGpuDevice;
class VioGpuContext;
class VioGpuAllocation;
class VioGpuCommander;

// Lifetime invariant: VioGpuCommand passes `this` as the complete_ctx
// of SubmitCommand / TransferHostCmd / MapBlob / UnmapBlob, which the
// host responds to from a DPC. The object must outlive every such
// in-flight callback.
//
// In the current flow this holds because Run() is the only path that
// frees the object (the `end:` arm) and Run() is only re-entered via
// QueueRunningCb -> QueueRunning -> commander queue -> Run(), so a
// cmd that submitted is never deleted while its callback is in flight.
// AddPending / DropPending track outstanding async submissions and the
// dtor asserts the count is zero -- a future change that frees a cmd
// from another path would trip the assert before the host's DPC could
// dereference freed memory.
class VioGpuCommand final : public HandleBase<"VIOGCOMM"_M, VioGpuCommand>
{
  public:
    VioGpuCommand(VioGpuAdapter *adapter, VioGpuDevice *device);
    ~VioGpuCommand();

    void Run();

    void PrepareSubmit(const DXGKARG_SUBMITCOMMAND *pSubmitCommand,
                       VioGpuDevice *device);
    void QueueRunning();
    static void QueueRunningCb(void *cmd, void *, void *);

    // Report either DMA_COMPLETED or DMA_FAULTED once, after every command in
    // the DMA stream has finished.
    void NotifyCompletion();
    void SetPreemption(const DXGKARG_PREEMPTCOMMAND *request);

    // Clear NeedsInitialPresent only when this DMA stream reaches its normal
    // completion path without a parser, transport, or host error.
    void SetInitialPresentCompletion(VioGpuAllocation *allocation);

    // Change the VidPN scanout source only after this Present DMA stream has
    // completed successfully and the authoritative source generation is still
    // current. The allocation is also held by m_allocations.
    void SetScanoutSourceCompletion(VioGpuAllocation *allocation,
                                    LONG sourceGeneration, BOOLEAN dmaFlip = FALSE);

    // Takes ownership of a referenced event object obtained by Render while
    // still in the submitting process context.  The raw user HANDLE never
    // leaves DxgkDdiRender.
    void SetRenderEvent(_In_ PKEVENT event);
    // Safe to call from normal retirement and cancellation/teardown paths.
    // It signals first and releases the reference exactly once.
    void SignalRenderEvent();

    void SetDmaBuf(char *pDmaBuffer)
    {
        m_pDmaBuffer = pDmaBuffer;
    }

    BOOLEAN BelongsToDevice(const VioGpuDevice *device) const
    {
        return device != NULL && m_pExpectedDevice == device;
    }

    BOOLEAN HasDmaBuffer() const
    {
        return m_pDmaBuffer != NULL;
    }

    NTSTATUS AttachAllocations(DXGK_ALLOCATIONLIST *allocationList,
                               UINT allocationListLength,
                               VioGpuDevice *expectedDevice);

    LIST_ENTRY list_entry;

  private:
    VioGpuAdapter *m_pAdapter;
    VioGpuCommander *m_pCommander;
    VioGpuDevice *m_pDevice;
    // Render/Present create the command before DxgkDdiSubmitCommand runs.
    // Retain the creating device so recycled private data cannot move a
    // command to a different WDDM context/device at submit time.
    VioGpuDevice *m_pExpectedDevice;

    VioGpuAllocation **m_allocations;
    UINT m_allocationsLength;

    UINT m_FenceId;
    // The node/engine dxgkrnl submitted this DMA buffer on. The
    // DMA_COMPLETED interrupt must report these back unchanged so the
    // scheduler matches the completion to the right engine; reporting a
    // fixed 0/0 strands submissions on any non-zero engine and the
    // scheduler eventually declares the engine hung (TDR).
    UINT m_NodeOrdinal;
    UINT m_EngineOrdinal;
    // DXGK_SUBMITCOMMANDFLAGS.NullRendering: the runtime is timing
    // the submission path itself (profiling) and wants the fence to
    // complete without executing the DMA body.
    BOOLEAN m_NullRendering;
    BOOLEAN m_Preemption;

    // Outstanding async submissions where `this` is the complete_ctx.
    // Tracked so the dtor can assert no callback is still pending.
    volatile LONG m_pendingCallbacks;

    // Once-guard for NotifyCompletion(): the DMA_COMPLETED interrupt must fire
    // exactly once per command even though both the DPC completion path and the
    // Run() epilogue can reach it.
    volatile LONG m_notified;
    volatile LONG m_failureStatus;

    // Non-owning pointer to one of m_allocations. MarkBusy keeps it alive
    // until the command releases its allocation set.
    VioGpuAllocation *m_pInitialPresentCompletion;
    VioGpuAllocation *m_pScanoutSourceCompletion;
    LONG m_scanoutSourceGeneration;
    BOOLEAN m_scanoutSourceIsDmaFlip;

    // Present only for a terminal VIOGPU_CMD_SIGNAL_EVENT.  Volatile and
    // atomically exchanged because reset/cancel can race the worker's normal
    // retirement path.
    PKEVENT volatile m_pRenderEvent;

    void AddPending();
    // Returns the post-decrement count so callers can re-queue the command
    // exactly once -- when the last outstanding async submission completes.
    LONG DropPending();
    void RecordFailure(NTSTATUS status);
    void RecordIssueFailure(NTSTATUS status);
    void ReleaseAllocations();

    char *m_pDmaBuffer;
    char *m_pCommand;
    char *m_pEnd;
};

class VioGpuCommander
{
  public:
    VioGpuCommander(VioGpuAdapter *pAdapter);

    NTSTATUS Start();
    void Stop();

    void CommandFinished(VioGpuCommand *cmd);

    NTSTATUS Patch(const DXGKARG_PATCH *pPatch);
    NTSTATUS SubmitCommand(const DXGKARG_SUBMITCOMMAND *pSubmitCommand);
    NTSTATUS QueuePreemption(const DXGKARG_PREEMPTCOMMAND *request);

    // Wake render-event waiters for all queued/in-flight work.  Commands stay
    // owned by their normal completion path; this method only releases the
    // referenced user event so device removal/reset cannot strand a waiter.
    void CancelRenderEvents();

    _IRQL_requires_max_(DISPATCH_LEVEL) _IRQL_saves_global_(OldIrql, Irql) _IRQL_raises_(DISPATCH_LEVEL) void LockQueue(
                                                                                                        KIRQL *Irql);
    _IRQL_requires_(DISPATCH_LEVEL) _IRQL_restores_global_(OldIrql, Irql) void UnlockQueue(KIRQL Irql);

    VioGpuCommand *DequeueRunning();
    void QueueRunning(VioGpuCommand *cmd);

    VioGpuCommand *DequeueSubmitted();
    void QueueSubmitted(VioGpuCommand *cmd);

  private:
    static void ThreadWork(PVOID Context);
    void ThreadWorkRoutine(void);

    VioGpuAdapter *m_pAdapter;

    KEVENT m_QueueEvent;
    // Used only when PsCreateSystemThread succeeds but obtaining a stable
    // thread-object reference fails.  Start() must still wait until the
    // thread stops before its embedded state can be destroyed.
    KEVENT m_ThreadExitEvent;
    PETHREAD m_pWorkThread;
    volatile BOOLEAN m_bStopWorkThread;

    LIST_ENTRY m_SubmittedQueue;
    LIST_ENTRY m_RunningQueue;

    KSPIN_LOCK m_Lock;

    UINT m_running;
    // At most one command may execute or wait on a host callback.  It is not
    // on either queue while that callback is outstanding, so teardown needs
    // this separate reference to wake a terminal render event.
    VioGpuCommand *volatile m_ActiveCommand;
};
