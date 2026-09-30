#include <cassert>
#include <cstdio>
#define VIOGPU_TARGET_VISTA 1
#define UNREFERENCED_PARAMETER(x) (void)(x)
#define STATUS_SUCCESS 0
using LONG = int;
#define FALSE false
struct PHYSICAL_ADDRESS { long long QuadPart; };
static LONG InterlockedIncrement(LONG *p) { return ++*p; }
struct VioGpuAllocation { int id; };
struct VioGpuVidPN {
    LONG m_sourceGeneration = 35;
    VioGpuAllocation *displayed = nullptr;
    bool IsScanoutSourceCompatible(VioGpuAllocation *) const { return true; }
    bool SetScanoutSourceIfGeneration(VioGpuAllocation *p, PHYSICAL_ADDRESS, LONG generation, bool = false) {
        if (generation != m_sourceGeneration) return false;
        displayed = p; return true;
    }
    void SetScanoutSource(VioGpuAllocation *);
};
struct Adapter { VioGpuVidPN vidpn; };
struct Resource { void *ToHandle() { return this; } };
struct Output { void *hResource = nullptr; };
/* SOURCE_UNDER_TEST */
int main()
{
    Adapter adapter;
    VioGpuAllocation current{1}, queued{2}, allocated{3}, another{4};
    Resource resource;
    Output output;
    adapter.vidpn.displayed = &current;
    LONG queuedGeneration = adapter.vidpn.m_sourceGeneration;
    assert(FinishAllocation(&resource, &allocated, &adapter, &output) == STATUS_SUCCESS);
    assert(output.hResource == &resource);
    assert(adapter.vidpn.displayed == &current);
    assert(adapter.vidpn.m_sourceGeneration == queuedGeneration);
    assert(FinishAllocation(nullptr, &another, &adapter, &output) == STATUS_SUCCESS);
    assert(adapter.vidpn.displayed == &current);
    assert(adapter.vidpn.SetScanoutSourceIfGeneration(&queued, {}, queuedGeneration));
    assert(adapter.vidpn.displayed == &queued);
    assert(FinishAllocation(nullptr, nullptr, &adapter, &output) == STATUS_SUCCESS);
    assert(adapter.vidpn.displayed == &queued);
    puts("PASS allocation publishes its handle without selecting scanout or invalidating queued flips");
}
