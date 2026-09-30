
using ULONG = unsigned long;
struct VioGpuTraceTag { unsigned long long run = 0, frame = 0; };
constexpr int TT_VBLANK=41, TT_VSYNC_TICK=44;
static unsigned long long VioGpuTraceRun() { return 0; }
static void VioGpuTraceRecord(int, VioGpuTraceTag, unsigned long long, unsigned long=0, unsigned long=0) {}
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>
using ULONGLONG = unsigned long long;
using LONGLONG = long long;
using LONG = long;
using NTSTATUS = long;
using KIRQL = int;
using PVOID = void *;
struct LARGE_INTEGER { LONGLONG QuadPart; };
using PHYSICAL_ADDRESS = LARGE_INTEGER;
struct Event { bool signaled = false; };
struct DXGKARGCB_NOTIFY_INTERRUPT_DATA {
    int InterruptType;
    struct { int VidPnTargetId; PHYSICAL_ADDRESS PhysicalAddress; } CrtcVsync;
};
constexpr int FALSE=0, TRUE=1, DISPATCH_LEVEL=2, LOW_REALTIME_PRIORITY=16;
constexpr int WaitAny=0, Executive=0, KernelMode=0, IO_NO_INCREMENT=0;
constexpr int STATUS_WAIT_0=0, STATUS_SUCCESS=0, DXGK_INTERRUPT_CRTC_VSYNC=1;
#define VIOGPU_ASSERT_CHK(x) assert(x)
static ULONGLONG clock_now;
static std::vector<ULONGLONG> reports, deadlines;
static unsigned wake_count;
static LONG *stop_flag;
static bool terminated, copying;
static int KeGetCurrentIrql() { return 0; }
static void *KeGetCurrentThread() { return nullptr; }
static void KeSetPriorityThread(void *, int) {}
static LONG InterlockedIncrement(volatile LONG *p) { return ++*p; }
static LONG InterlockedCompareExchange(volatile LONG *p, LONG value, LONG expected) {
    LONG old=*p; if (old==expected) *p=value; return old;
}
static LARGE_INTEGER KeQueryPerformanceCounter(LARGE_INTEGER *f) {
    if (f) f->QuadPart=10000000;
    return {(LONGLONG)clock_now};
}
static void KeSetTimer(Event *, LARGE_INTEGER due, void *) {
    assert(due.QuadPart < 0);
    deadlines.push_back(clock_now - due.QuadPart);
}
static void KeCancelTimer(Event *) {}
static void KeSetEvent(Event *e, int, int) { e->signaled=true; }
static void PsTerminateSystemThread(int) { terminated=true; }
static NTSTATUS KeWaitForMultipleObjects(int n, PVOID *, int, int, int, int, void *, void *) {
    assert(n==2);
    // First wake is early; third wake misses three periods. Every real tick
    // must still happen while the copy worker is blocked with its mutex held.
    static const ULONGLONG wakes[] = {9999, 10000, 49999, 50000, 51000};
    clock_now=wakes[wake_count++];
    if (wake_count==5) *stop_flag=TRUE;
    return STATUS_WAIT_0;
}
static LONGLONG VsyncPeriodFromRefresh(int) { return 10000; }
static ULONGLONG VioGpuNextVsyncDeadline(ULONGLONG d, ULONGLONG n, ULONGLONG p) {
    if (d<=n) d+=((n-d)/p+1)*p;
    return d;
}
struct Adapter {
    NTSTATUS NotifyInterrupt(DXGKARGCB_NOTIFY_INTERRUPT_DATA *i, bool) {
        assert(copying); // Vblank may only report the last completed address.
        assert(i->CrtcVsync.PhysicalAddress.QuadPart==1234);
        reports.push_back(clock_now);
        return STATUS_SUCCESS;
    }
};
class VioGpuVidPN {
public:
    VioGpuTraceTag m_displayedTraceTag;
    Event m_vsyncTimer, m_vsyncStopEvent, m_flipReadyEvent, m_vsyncExitEvent, m_vblankEvent;
    LONG m_vblankSequence=0, m_synchronizedFlip=FALSE;
    LONG m_shouldFlipStop=FALSE, m_shouldFlip=TRUE, m_vsyncEnabled=TRUE;
    LONGLONG m_vsyncTimerPeriod100ns=0;
    bool m_displayedAddressValid=true;
    PHYSICAL_ADDRESS m_displayedAddress={1234};
    Adapter *m_pAdapter;
    int GetActiveRefreshRate() { return 1000; }
    void PublishRasterTiming(ULONGLONG epoch, ULONGLONG period, int = FALSE) {
        assert(period == 10000 && epoch <= clock_now);
        assert(clock_now - epoch < period);
    }
    KIRQL AcquireSourceLock() { return 0; }
    void ReleaseSourceLock(KIRQL) {}
    void TryPromoteFlip() { assert(!"vblank must not acquire the copy mutex"); }
    void Flip();
    static void VsyncThread(void *);
};
/* SOURCE_UNDER_TEST */
int main() {
    Adapter a;
    VioGpuVidPN v;
    v.m_pAdapter=&a;
    stop_flag=&v.m_shouldFlipStop;
    copying=true;
    VioGpuVidPN::VsyncThread(&v);
    assert((reports==std::vector<ULONGLONG>{10000,49999,50000}));
    assert((deadlines==std::vector<ULONGLONG>{10000,10000,20000,50000,60000}));
    assert(terminated && v.m_vsyncExitEvent.signaled && v.m_flipReadyEvent.signaled);
    // Disabled vblank cannot leak an interrupt after ControlInterrupt.
    v.m_vsyncEnabled=FALSE;
    v.Flip();
    assert(reports.size()==3);
    VioGpuVidPN waiting;
    waiting.m_pAdapter=&a;
    waiting.m_synchronizedFlip=TRUE;
    stop_flag=&waiting.m_shouldFlipStop;
    wake_count=0; clock_now=0; reports.clear(); deadlines.clear();
    VioGpuVidPN::VsyncThread(&waiting);
    assert(reports.empty());
    assert(waiting.m_vblankSequence==3 && waiting.m_vblankEvent.signaled);
    puts("PASS vblank independent of blocked copy; no early tick; missed ticks skipped; stop and disable");
}
