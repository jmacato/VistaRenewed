
using ULONG = unsigned long;
struct VioGpuTraceTag { unsigned long long run = 0, frame = 0; };
constexpr int TT_SYNC_BEGIN=42, TT_SYNC_END=43;
static void VioGpuTraceRecord(int, VioGpuTraceTag, unsigned long) {}
#include <cassert>
#include <cstdio>
using NTSTATUS = int;
using BOOLEAN = bool;
constexpr bool FALSE=false;
constexpr int STATUS_SUCCESS=0, STATUS_UNSUCCESSFUL=-1;
#define NT_SUCCESS(s) ((s)>=0)
static int InterlockedCompareExchange(int *p,int v,int e) {
 int old=*p; if(old==e)*p=v; return old;
}
struct Vidpn {
 int status=0, calls=0;
 int BeginSynchronizedFlip(unsigned i) { assert(i>0); ++calls; return status; }
};
struct Adapter { Vidpn vidpn; };
struct VioGpuCommand {
 VioGpuTraceTag m_traceTag;
 int m_failureStatus=0;
 void *m_pScanoutSourceCompletion=this;
 unsigned m_scanoutFlipInterval=1;
 Adapter *m_pAdapter;
 void RecordFailure(NTSTATUS);
 bool Begin();
};
/* SOURCE_UNDER_TEST */
int main() {
 Adapter a; VioGpuCommand c; c.m_pAdapter=&a;
 assert(c.Begin() && c.m_failureStatus==0 && a.vidpn.calls==1);
 c.m_scanoutFlipInterval=0;
 assert(!c.Begin() && c.m_failureStatus==0 && a.vidpn.calls==1);
 c.m_scanoutFlipInterval=1; a.vidpn.status=-4;
 assert(!c.Begin() && c.m_failureStatus==-4 && a.vidpn.calls==2);
 assert(!c.Begin() && c.m_failureStatus==-4 && a.vidpn.calls==2);
 c.m_failureStatus=0; c.m_pScanoutSourceCompletion=nullptr;
 assert(!c.Begin() && c.m_failureStatus==0 && a.vidpn.calls==2);
 puts("PASS successful vblank is not a DMA fault; immediate bypass; failure propagation");
}
