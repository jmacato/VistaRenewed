
using ULONG = unsigned long;
struct VioGpuTraceTag { unsigned long long run = 0, frame = 0; };
static VioGpuTraceTag VioGpuTraceThreadTag() { return {}; }
#include <cassert>
#include <cstdio>
#include <cstddef>
using UINT = unsigned;
using LONG = int;
#define DbgPrint(...) ((void)0)
static LONG InterlockedDecrement(LONG *p) { return --*p; }
using BOOLEAN = bool;
using KIRQL = int;
#define TRUE true
#define FALSE false
#define IO_NO_INCREMENT 0
struct PHYSICAL_ADDRESS { long long QuadPart; };
struct VioGpuAllocation {
    PHYSICAL_ADDRESS m_SegmentAddress;
    int refs = 1;
    bool compatible = true;
    void AddRef() { ++refs; }
    void ReleaseDeferred() { --refs; }
};
static void InterlockedOr(int *p, int bits) { *p |= bits; }
static void KeSetEvent(int *p, int, bool) { ++*p; }
struct VioGpuVidPN {
    VioGpuTraceTag m_sourceTraceTag;
    LONG m_sourceGeneration = 7;
    VioGpuAllocation *m_sourceRes = nullptr;
    PHYSICAL_ADDRESS m_sourceAddress = {4096};
    int m_shouldFlip = 0, m_flipReadyEvent = 0;
    bool IsScanoutSourceCompatible(VioGpuAllocation *p) { return !p || p->compatible; }
    KIRQL AcquireSourceLock() { return 0; }
    void ReleaseSourceLock(KIRQL) {}
    BOOLEAN SetScanoutSourceIfGeneration(VioGpuAllocation *, LONG, BOOLEAN);
    BOOLEAN SetScanoutSourceIfGeneration(VioGpuAllocation *, PHYSICAL_ADDRESS, LONG, BOOLEAN);
};
/* SOURCE_UNDER_TEST */
int main() {
    VioGpuVidPN v;
    VioGpuAllocation zero{{0}}, other{{8192}}, invalid{{12288}};
    // A valid zero-offset DMA flip must replace the previous nonzero address.
    assert(v.SetScanoutSourceIfGeneration(&zero, 7, true));
    assert(v.m_sourceAddress.QuadPart == 0 && zero.refs == 2);
    assert(v.SetScanoutSourceIfGeneration(&other, 7, true));
    assert(v.m_sourceAddress.QuadPart == 8192 && zero.refs == 1 && other.refs == 2);
    // Blt promotion changes the resource, preserving the scheduler address.
    assert(v.SetScanoutSourceIfGeneration(&zero, 7, false));
    assert(v.m_sourceAddress.QuadPart == 8192 && other.refs == 1 && zero.refs == 2);
    int events = v.m_flipReadyEvent;
    assert(!v.SetScanoutSourceIfGeneration(&other, 6, true));
    assert(v.m_sourceRes == &zero && v.m_sourceAddress.QuadPart == 8192);
    assert(other.refs == 1 && v.m_flipReadyEvent == events);
    invalid.compatible = false;
    assert(!v.SetScanoutSourceIfGeneration(&invalid, 7, true));
    assert(invalid.refs == 1 && v.m_flipReadyEvent == events);
    assert(v.SetScanoutSourceIfGeneration(nullptr, 7, false));
    assert(zero.refs == 1 && v.m_sourceAddress.QuadPart == 8192);
    puts("PASS zero-offset DMA flip, blt address preservation, stale generation and ownership");
}
