#pragma once

#include "helper.h"
#include "viogpu.h"
#include "viogpu_trace.h"

class VioGpuAdapter;
class VioGpuAllocation;
class VioGpuObj;

// DXGK_DISPLAY_INFORMATION belongs to the system-display callbacks that were
// added after WDDM 1.0.  Vista still needs the same small set of fields for
// its internal mode bookkeeping, so keep that state private to this driver.
// Do not expose this replacement through a DDI callback.
#if defined(VIOGPU_TARGET_VISTA)
typedef struct _VIOGPU_DISPLAY_INFORMATION
{
    UINT Width;
    UINT Height;
    UINT Pitch;
    D3DDDIFORMAT ColorFormat;
    D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId;
    ULONG AcpiId;
    PHYSICAL_ADDRESS PhysicAddress;
} VIOGPU_DISPLAY_INFORMATION;
#else
typedef DXGK_DISPLAY_INFORMATION VIOGPU_DISPLAY_INFORMATION;
#endif

typedef struct _CURRENT_MODE
{
    VIOGPU_DISPLAY_INFORMATION DispInfo;
    D3DKMDT_VIDPN_PRESENT_PATH_ROTATION Rotation;
    D3DKMDT_VIDPN_PRESENT_PATH_SCALING Scaling;
    UINT SrcModeWidth;
    UINT SrcModeHeight;
    struct _CURRENT_MODE_FLAGS
    {
        UINT SourceNotVisible : 1;
        UINT FullscreenPresent : 1;
        UINT FrameBufferIsActive : 1;
        UINT DoNotMapOrUnmap : 1;
        UINT IsInternal : 1;
        UINT Unused : 27;
    } Flags;

    PHYSICAL_ADDRESS ZeroedOutStart;
    PHYSICAL_ADDRESS ZeroedOutEnd;

    union {
        VOID *Ptr;
        ULONG64 Force8Bytes;
    } FrameBuffer;
} CURRENT_MODE;

class VioGpuVidPN
{
  public:
    VioGpuVidPN(VioGpuAdapter *adapter);
    ~VioGpuVidPN();

    NTSTATUS Start(ULONG *pNumberOfViews, ULONG *pNumberOfChildren);
#if !defined(VIOGPU_TARGET_VISTA)
    NTSTATUS AcquirePostDisplayOwnership();
    void ReleasePostDisplayOwnership(D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId, DXGK_DISPLAY_INFORMATION *pDisplayInfo);
#endif
    void Powerdown();

    NTSTATUS IsVidPnSourceModeFieldsValid(CONST D3DKMDT_VIDPN_SOURCE_MODE *pSourceMode) const;
    NTSTATUS IsVidPnPathFieldsValid(CONST D3DKMDT_VIDPN_PRESENT_PATH *pPath) const;

    NTSTATUS CommitVidPn(_In_ CONST DXGKARG_COMMITVIDPN *CONST pCommitVidPn);
    NTSTATUS
    UpdateActiveVidPnPresentPath(_In_ CONST DXGKARG_UPDATEACTIVEVIDPNPRESENTPATH *CONST pUpdateActiveVidPnPresentPath);

    NTSTATUS SetCurrentMode(ULONG Mode, CURRENT_MODE *pCurrentMode);
    ULONG GetModeCount(void)
    {
        return m_ModeCount;
    }
    VOID BlackOutScreen(CURRENT_MODE *pCurrentMod);

    NTSTATUS GetModeList(VIOGPU_DISPLAY_INFORMATION *pDispInfo);

    NTSTATUS CreateFrameBufferObj(PVIDEO_MODE_INFORMATION pModeInfo, CURRENT_MODE *pCurrentMode);
    void DestroyFrameBufferObj(BOOLEAN bReset);

    BOOLEAN GpuObjectAttach(UINT res_id, VioGpuObj *obj);
    PBYTE GetEdidData(UINT Idx);

    PBYTE GetCTA861Data(void);
    void SetVideoModeInfo(UINT Idx, PVIOGPU_DISP_MODE pModeInfo);
    BOOLEAN GetDisplayInfo(void);
    int ProcessEdid(void);
    void FixEdid(void);
    BOOLEAN GetEdids(void);
    int AddEdidModes(void);
    BOOLEAN UpdateModes(USHORT xres, USHORT yres, int &cnt);
    void SetCustomDisplay(_In_ USHORT xres, _In_ USHORT yres);

    NTSTATUS EscapeCustomResoulution(VIOGPU_DISP_MODE *resolution);

    NTSTATUS IsSupportedVidPn(_Inout_ DXGKARG_ISSUPPORTEDVIDPN *pIsSupportedVidPn);
    NTSTATUS RecommendFunctionalVidPn(_In_ CONST DXGKARG_RECOMMENDFUNCTIONALVIDPN *CONST pRecommendFunctionalVidPn);
    NTSTATUS RecommendVidPnTopology(_In_ CONST DXGKARG_RECOMMENDVIDPNTOPOLOGY *CONST pRecommendVidPnTopology);
    NTSTATUS RecommendMonitorModes(_In_ CONST DXGKARG_RECOMMENDMONITORMODES *CONST pRecommendMonitorModes);
    NTSTATUS EnumVidPnCofuncModality(_In_ CONST DXGKARG_ENUMVIDPNCOFUNCMODALITY *CONST pEnumCofuncModality);
    NTSTATUS SetVidPnSourceVisibility(_In_ CONST DXGKARG_SETVIDPNSOURCEVISIBILITY *pSetVidPnSourceVisibility);
#if !defined(VIOGPU_TARGET_VISTA)
    NTSTATUS QueryVidPnHWCapability(_Inout_ DXGKARG_QUERYVIDPNHWCAPABILITY *pVidPnHWCaps);

    NTSTATUS SystemDisplayEnable(_In_ D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
                                 _In_ PDXGKARG_SYSTEM_DISPLAY_ENABLE_FLAGS Flags,
                                 _Out_ UINT *pWidth,
                                 _Out_ UINT *pHeight,
                                 _Out_ D3DDDIFORMAT *pColorFormat);
    VOID SystemDisplayWrite(_In_reads_bytes_(SourceHeight *SourceStride) VOID *pSource,
                            _In_ UINT SourceWidth,
                            _In_ UINT SourceHeight,
                            _In_ UINT SourceStride,
                            _In_ INT PositionX,
                            _In_ INT PositionY);
#endif

    void Flip();
    static void FlipThread(void *ctx);
    static void VsyncThread(void *ctx);

    // Scan out an armed source. Returns TRUE only after the scanout and flush
    // commands have completed successfully on the host.
    BOOLEAN TryPromoteFlip();
    NTSTATUS CompletePendingFlip();
    void SetVsyncEnabled(BOOLEAN Enabled)
    {
        InterlockedExchange(&m_vsyncEnabled, Enabled ? TRUE : FALSE);
    }

    NTSTATUS SetVidPnSourceAddress(const DXGKARG_SETVIDPNSOURCEADDRESS *pSetVidPnSourceAddress);

    // Override the source-0 scanout to an arbitrary allocation (a standing
    // dmabuf primary the UMD blits the composited frame into), independent of
    // dxgkrnl's flip. The vsync Flip thread then scans it out continuously.
    // Used by the blt-present path where dxgkrnl issues no SetVidPnSourceAddress
    // for the windowed present source.
    // Creation-time promotion preserves the scheduler's current address.
    void SetScanoutSource(VioGpuAllocation *res);
    BOOLEAN SetScanoutSourceIfGeneration(VioGpuAllocation *res,
                                         LONG sourceGeneration, BOOLEAN dmaFlip = FALSE);
    LONG GetScanoutSourceGeneration()
    {
        return InterlockedCompareExchange(&m_sourceGeneration, 0, 0);
    }
    void RearmFlipIfScanout(VioGpuAllocation *res);
    // Currently-committed refresh rate, or {0,0} if no source mode is
    // pinned. Caller is responsible for choosing a default.
    D3DDDI_RATIONAL GetActiveRefreshRate() const;
    NTSTATUS GetScanLine(DXGKARG_GETSCANLINE *args);
    void PublishRasterTiming(ULONGLONG epoch, ULONGLONG period);
    NTSTATUS BeginSynchronizedFlip(UINT interval);
    void EndSynchronizedFlip();

  private:
    // Epoch/period share the source lock so x86 never observes a torn clock.
    ULONGLONG m_rasterEpoch100ns = 0;
    ULONGLONG m_rasterPeriod100ns = 166667;
    volatile LONG m_rasterHeight = 0;
    BOOLEAN IsScanoutSourceCompatible(VioGpuAllocation *res) const;
    BOOLEAN SetScanoutSourceIfGeneration(VioGpuAllocation *res,
                                         PHYSICAL_ADDRESS addr,
                                         LONG sourceGeneration, BOOLEAN addressValid);
    void StopFlipThread();
    NTSTATUS SetSourceModeAndPath(CONST D3DKMDT_VIDPN_SOURCE_MODE *pSourceMode,
                                  CONST D3DKMDT_VIDPN_PRESENT_PATH *pPath);
    NTSTATUS AddSingleMonitorMode(_In_ CONST DXGKARG_RECOMMENDMONITORMODES *CONST pRecommendMonitorModes);
    NTSTATUS AddSingleSourceMode(_In_ CONST DXGK_VIDPNSOURCEMODESET_INTERFACE *pVidPnSourceModeSetInterface,
                                 D3DKMDT_HVIDPNSOURCEMODESET hVidPnSourceModeSet,
                                 _In_opt_ CONST D3DKMDT_VIDPN_TARGET_MODE *pVidPnPinnedTargetModeInfo,
                                 D3DDDI_VIDEO_PRESENT_SOURCE_ID SourceId);
    NTSTATUS AddSingleTargetMode(_In_ CONST DXGK_VIDPNTARGETMODESET_INTERFACE *pVidPnTargetModeSetInterface,
                                 D3DKMDT_HVIDPNTARGETMODESET hVidPnTargetModeSet,
                                 _In_opt_ CONST D3DKMDT_VIDPN_SOURCE_MODE *pVidPnPinnedSourceModeInfo,
                                 D3DDDI_VIDEO_PRESENT_SOURCE_ID SourceId);
    BOOLEAN IsDuplicateModeSize(UINT ModeIndex) const;
    D3DDDI_VIDEO_PRESENT_SOURCE_ID FindSourceForTarget(D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId, BOOLEAN DefaultToZero);
    VOID BuildVideoSignalInfo(D3DKMDT_VIDEO_SIGNAL_INFO *pVideoSignalInfo, PVIDEO_MODE_INFORMATION pModeInfo, ULONG RefreshRate);

    // m_sourceLock is taken from PASSIVE/DISPATCH (mode set, FlipThread) and
    // also from DIRQL: with FlipCaps.FlipOnVSyncMmIo set, dxgkrnl runs
    // DdiSetVidPnSourceAddress inside dxgmms1!VidSchiExecuteMmIoFlipAtISR via
    // KeSynchronizeExecution. KeAcquireSpinLock raises to DISPATCH_LEVEL and
    // must not be called above it -- at DIRQL it hangs the CPU in
    // nt!KeAcquireSpinLockRaiseToDpc. Go through these helpers so the caller's
    // IRQL is honoured wherever the lock is taken.
    __forceinline KIRQL AcquireSourceLock()
    {
        KIRQL irql = KeGetCurrentIrql();
        if (irql >= DISPATCH_LEVEL)
        {
            KeAcquireSpinLockAtDpcLevel(&m_sourceLock);
            return irql;
        }
        KeAcquireSpinLock(&m_sourceLock, &irql);
        return irql;
    }

    __forceinline void ReleaseSourceLock(KIRQL irql)
    {
        if (irql >= DISPATCH_LEVEL)
        {
            KeReleaseSpinLockFromDpcLevel(&m_sourceLock);
        }
        else
        {
            KeReleaseSpinLock(&m_sourceLock, irql);
        }
    }

    VioGpuAdapter *m_pAdapter;
    DXGKRNL_INTERFACE *m_pDxgkInterface;

    CURRENT_MODE m_CurrentModes[MAX_VIEWS];

    PVIDEO_MODE_INFORMATION m_ModeInfo;
    ULONG m_ModeCount;
    PUSHORT m_ModeNumbers;
    USHORT m_CurrentModeIndex;
    USHORT m_CustomModeIndex;
    BYTE m_EDIDs[MAX_CHILDREN][EDID_RAW_BLOCK_SIZE];
    BOOLEAN m_bEDID;
    ULONG m_RefreshRate;
    volatile LONG m_ActiveRefreshRate;

#if !defined(VIOGPU_TARGET_VISTA)
    DXGK_DISPLAY_INFORMATION m_SystemDisplayInfo;
    D3DDDI_VIDEO_PRESENT_SOURCE_ID m_SystemDisplaySourceId;
#endif

    VioGpuObj *m_pFrameBuf;

    PHYSICAL_ADDRESS m_sourceAddress = {0};
    VioGpuTraceTag m_sourceTraceTag = {};
    VioGpuAllocation *m_sourceRes = NULL;
    KSPIN_LOCK m_sourceLock;
    FAST_MUTEX m_flipSubmitMutex;
    NTSTATUS m_lastFlipStatus = STATUS_SUCCESS;
    BOOLEAN TryPromoteFlipLocked();
    // SetVidPnSourceAddress, primary creation, and teardown advance this value.
    // A Present completion may replace the source only within its generation.
    volatile LONG m_sourceGeneration = 0;
    volatile LONG m_shouldFlip = 0;

    // What is ACTUALLY on screen, as opposed to what has been latched.
    // dxgkrnl retires a queued flip -- and frees the primary it displaced --
    // when a vsync reports that flip's address, so reporting a latched but
    // not-yet-scanned-out address would hand a buffer back while it is still
    // the one being displayed. TryPromoteFlipLocked publishes the address
    // only after the host has completed the scanout copy.
    PHYSICAL_ADDRESS m_displayedAddress = {0};
    VioGpuTraceTag m_displayedTraceTag = {};
    BOOLEAN m_displayedAddressValid = FALSE;

    // Signalled after a PASSIVE_LEVEL producer latches a new source. This
    // avoids waiting for the next periodic vsync tick.
    KEVENT m_flipReadyEvent;

    // Signalled immediately before the flip system thread terminates.  This
    // also covers the rare path where ObReferenceObjectByHandle fails and no
    // PETHREAD pointer is available for KeWaitForSingleObject.
    KEVENT m_flipExitEvent;
    KEVENT m_vsyncExitEvent;
    KEVENT m_vsyncStopEvent;

    // Vblank deadline timer. Source-ready events cannot postpone the tick.
    KTIMER m_vsyncTimer;

    // Period the timer is currently programmed with (100ns units), so the
    // vsync thread re-arms it only when a mode change alters the refresh
    // rate.
    LONGLONG m_vsyncTimerPeriod100ns = 0;

    volatile LONG m_flipPromotes = 0;
    volatile LONG m_flipSubmitFailures = 0;

    // NULL whenever no thread reference is held, so that the stop paths can
    // tell whether the reference is still theirs to drop.  Checked builds
    // fill fresh objects with 0xCD rather than zeroes.
    PETHREAD m_pFlipThread = NULL;
    PETHREAD m_pVsyncThread = NULL;
    // Read by the flip system thread and written by pageable start/stop paths.
    // Use interlocked access so teardown cannot miss the stop request on
    // another processor.
    volatile LONG m_shouldFlipStop = FALSE;
    // DxgkDdiControlInterrupt owns this state. Do not report synthetic CRTC
    // interrupts before dxgkrnl enables them or after it disables them.
    volatile LONG m_vsyncEnabled = FALSE;
    volatile LONG m_synchronizedFlip = FALSE;
    volatile LONG m_vblankSequence = 0;
    KEVENT m_vblankEvent;
};
