#include <cassert>
#include <cstdio>
#include <cstdint>
using UINT = unsigned; using ULONG = uint32_t; using LONG = int32_t;
using ULONGLONG = unsigned long long; using LONGLONG = long long;
using NTSTATUS = int; using PVOID = void *;
struct LARGE_INTEGER { LONGLONG QuadPart; };
struct Event {};
constexpr int FALSE=0, TRUE=1, WaitAny=0, Executive=0, KernelMode=0;
constexpr int STATUS_SUCCESS=0, STATUS_WAIT_0=0, STATUS_TIMEOUT=258;
constexpr int STATUS_INVALID_PARAMETER=-1, STATUS_DEVICE_BUSY=-2;
constexpr int STATUS_CANCELLED=-3, STATUS_IO_TIMEOUT=-4;
static LONG InterlockedCompareExchange(volatile LONG *p, LONG v, LONG e) {
 LONG old=*p; if (old==e) *p=v; return old;
}
static void InterlockedExchange(volatile LONG *p, LONG v) { *p=v; }
static ULONGLONG now;
static unsigned waits;
static int outcome;
static volatile LONG *sequence;
static void KeClearEvent(Event *) {}
static void KeQueryPerformanceCounter(LARGE_INTEGER *f) { f->QuadPart=10000000; }
static ULONGLONG VioGpuVsyncNow(ULONGLONG) { return now; }
static NTSTATUS KeWaitForMultipleObjects(int n, PVOID *, int, int, int, int,
                                        LARGE_INTEGER *t, void *) {
 assert(n==2 && t->QuadPart<0); ++waits; now+=166667;
 if (outcome==STATUS_WAIT_0) *sequence=(LONG)((ULONG)*sequence+1);
 return outcome;
}
struct VioGpuVidPN {
 volatile LONG m_synchronizedFlip=0, m_vblankSequence=0, m_shouldFlipStop=0;
 Event m_vblankEvent, m_vsyncStopEvent;
 bool dmaDone=false;
 int reports=0;
 void Flip() { assert(dmaDone && m_synchronizedFlip); ++reports; }
 NTSTATUS BeginSynchronizedFlip(UINT);
 void EndSynchronizedFlip();
};
/* SOURCE_UNDER_TEST */
int main() {
 VioGpuVidPN v; sequence=&v.m_vblankSequence;
 assert(v.BeginSynchronizedFlip(0)==STATUS_INVALID_PARAMETER);
 assert(v.BeginSynchronizedFlip(5)==STATUS_INVALID_PARAMETER);
 for (unsigned i=1;i<=4;i++) {
  waits=0; outcome=0; v.dmaDone=false;
  assert(v.BeginSynchronizedFlip(i)==STATUS_SUCCESS);
  assert(waits==i && v.m_synchronizedFlip && v.reports==(int)i-1);
  assert(v.BeginSynchronizedFlip(1)==STATUS_DEVICE_BUSY);
  v.dmaDone=true; v.EndSynchronizedFlip();
  assert(!v.m_synchronizedFlip && v.reports==(int)i);
 }
 v.m_vblankSequence=-1; waits=0;
 assert(v.BeginSynchronizedFlip(1)==STATUS_SUCCESS && waits==1);
 v.EndSynchronizedFlip();
 outcome=STATUS_TIMEOUT;
 assert(v.BeginSynchronizedFlip(1)==STATUS_IO_TIMEOUT && !v.m_synchronizedFlip);
 outcome=1;
 assert(v.BeginSynchronizedFlip(1)==STATUS_CANCELLED && !v.m_synchronizedFlip);
 v.m_shouldFlipStop=1; waits=0;
 assert(v.BeginSynchronizedFlip(1)==STATUS_CANCELLED && !waits && !v.m_synchronizedFlip);
 puts("PASS interval count, wrap, DMA-before-vblank ownership, timeout and stop cancellation");
}
