#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <new>
#include <initializer_list>
#define VIOGPU_TARGET_VISTA 1
using UINT = unsigned;
using ULONG = unsigned;
using LONG = int;
#define DbgPrint(...) ((void)0)
static LONG InterlockedDecrement(LONG *p) { return --*p; }
using ULONGLONG = uint64_t;
using NTSTATUS = int;
using UCHAR = unsigned char;
constexpr int STATUS_SUCCESS = 0, STATUS_NOT_SUPPORTED = -1,
    STATUS_INVALID_PARAMETER = -2, STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER = -3,
    STATUS_NO_MEMORY = -4;
#define NT_SUCCESS(s) ((s) >= 0)
#define TRUE true
#define FALSE false
#define VIOGPU_NONPAGED_POOL std::nothrow
#define RtlZeroMemory(p,n) std::memset(p,0,n)
constexpr unsigned DXGK_PRESENT_SOURCE_INDEX = 1, VIOGPU_CMD_NOP = 9;
constexpr unsigned VIOGPU_CMD_TRANSFER_FROM_HOST = 10, VIOGPU_EXECBUF_VIRGL = 4;
struct VIOGPU_TRANSFER_CMD {
 unsigned res_id; struct {unsigned x,y,z,width,height,depth;} box;
 unsigned level, stride, layer_stride; uint64_t offset;
};
struct VIOGPU_COMMAND_HDR { unsigned type, size, flags, ring_idx; };
struct D3DDDI_PATCHLOCATIONLIST { unsigned AllocationIndex, DriverId, SlotId; };
struct DXGK_ALLOCATIONLIST {
    void *hDeviceSpecificAllocation = nullptr;
    unsigned SegmentId = 1;
    uint64_t PhysicalAddress = 0x12000;
};
struct DXGKARG_PRESENT {
    UINT FlipInterval = 0;
    union {
        struct { unsigned Blt:1, ColorFill:1, Flip:1, FlipWithNoWait:1, rest:28; };
        unsigned Value;
    } Flags{};
    DXGK_ALLOCATIONLIST *pAllocationList;
    void *pDmaBuffer, *pDmaBufferPrivateData;
    unsigned DmaSize, PatchLocationListOutSize, MultipassOffset = 123;
    D3DDDI_PATCHLOCATIONLIST *pPatchLocationListOut;
};
struct VioGpuAllocation {
    bool primary = true, blob = true, layoutValid = true;
    bool IsBlob() {return blob;}
    bool GetDimensions(UINT *w, UINT *h) {*w=1280;*h=720;return layoutValid;}
    bool GetTransferLayout(int x,int y,ULONG *pitch,ULONGLONG *offset) {
        assert(x==0&&y==0);*pitch=5120;*offset=64;return layoutValid;
    }
    uint64_t m_SegmentAddress = 0;
    bool IsPrimary() { return primary; }
    unsigned GetId() { return 23; }
};
struct Adapter {
    struct { int GetScanoutSourceGeneration() { return 17; } } vidpn;
};
struct VioGpuDeviceAllocation;
struct VioGpuDevice {
    Adapter adapter;
    Adapter *m_pAdapter = &adapter;
    NTSTATUS Present(DXGKARG_PRESENT *);
    int attaches=0, attachStatus=STATUS_SUCCESS;
    NTSTATUS EnsureVirglAttachment(VioGpuDeviceAllocation *) {attaches++;return attachStatus;}
    void TracePresent(unsigned,unsigned,unsigned,int,bool) {}
};
struct VioGpuDeviceAllocation {
    VioGpuDevice *device;
    VioGpuAllocation *allocation;
    static VioGpuDeviceAllocation *FromHandle(void *p) { return static_cast<VioGpuDeviceAllocation *>(p); }
    VioGpuDevice *GetDevice() { return device; }
    VioGpuAllocation *GetAllocation() { return allocation; }
};
struct VioGpuCommand {
    VioGpuAllocation *source = nullptr;
    int generation = 0;
    bool flip = false;
    char *dma = nullptr;
    VioGpuCommand(Adapter *, VioGpuDevice *) {}
    int AttachAllocations(DXGK_ALLOCATIONLIST *, unsigned n, VioGpuDevice *) {
        assert(n == 2); return STATUS_SUCCESS;
    }
    void SetScanoutSourceCompletion(VioGpuAllocation *p, int g, bool f, unsigned interval) {
        assert(interval <= 4);
        source=p; generation=g; flip=f;
    }
    void SetDmaBuf(char *p) { dma=p; }
    void *ToHandle() { return this; }
};
/* SOURCE_UNDER_TEST */
int main() {
    VioGpuDevice device;
    VioGpuAllocation source;
    VioGpuDeviceAllocation handle{&device, &source};
    DXGK_ALLOCATIONLIST allocations[2];
    allocations[1].hDeviceSpecificAllocation = &handle;
    alignas(8) UCHAR storage[sizeof(VIOGPU_COMMAND_HDR)+sizeof(VIOGPU_TRANSFER_CMD)]{};
    auto &packet = *reinterpret_cast<VIOGPU_COMMAND_HDR *>(storage);
    D3DDDI_PATCHLOCATIONLIST patch{};
    void *command = nullptr;
    auto request = [&](unsigned flags) {
        DXGKARG_PRESENT p{};
        p.Flags.Value = flags; p.pAllocationList = allocations;
        p.pDmaBuffer = &packet; p.pDmaBufferPrivateData = &command;
        p.DmaSize = sizeof(storage); p.PatchLocationListOutSize = 1;
        p.pPatchLocationListOut = &patch;
        return p;
    };
    for (unsigned flags : {4u, 12u}) {
        auto p = request(flags);
        assert(device.Present(&p) == STATUS_SUCCESS);
        auto *queued = static_cast<VioGpuCommand *>(command);
        assert(queued && queued->source == &source && queued->generation == 17 && queued->flip);
        assert(packet.type == VIOGPU_CMD_NOP && packet.size == 0 && packet.flags == 0);
        assert(p.pDmaBuffer == reinterpret_cast<char *>(&packet) + sizeof(packet));
        assert(p.pPatchLocationListOut == &patch + 1 && patch.AllocationIndex == 1);
        assert(source.m_SegmentAddress == allocations[1].PhysicalAddress);
        delete queued; command = nullptr;
    }
    assert(device.attaches==0); // Exported blobs need no shadow download.
    source.blob=false;
    for (unsigned flags : {4u,12u}) {
        auto p=request(flags);
        assert(device.Present(&p)==STATUS_SUCCESS);
        auto *transfer=reinterpret_cast<VIOGPU_TRANSFER_CMD *>(&packet+1);
        assert(packet.type==VIOGPU_CMD_TRANSFER_FROM_HOST && packet.flags==VIOGPU_EXECBUF_VIRGL);
        assert(packet.size==sizeof(*transfer) && transfer->res_id==23);
        assert(transfer->box.width==1280 && transfer->box.height==720 && transfer->box.depth==1);
        assert(!transfer->box.x && !transfer->box.y && !transfer->box.z && !transfer->level);
        assert(transfer->stride==5120 && transfer->offset==64 && !transfer->layer_stride);
        assert(p.pDmaBuffer==storage+sizeof(storage));
        auto *queued=static_cast<VioGpuCommand *>(command);
        assert(queued->source==&source && queued->flip);delete queued;command=nullptr;
    }
    assert(device.attaches==2);
    auto shortDownload=request(12);shortDownload.DmaSize=sizeof(storage)-1;
    assert(device.Present(&shortDownload)==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && !command);
    assert(device.attaches==2); // Failed sizing must not attach or publish.
    source.layoutValid=false;auto badLayout=request(12);
    assert(device.Present(&badLayout)==STATUS_INVALID_PARAMETER && !command);
    source.layoutValid=true;device.attachStatus=STATUS_NO_MEMORY;
    auto badAttach=request(12);assert(device.Present(&badAttach)==STATUS_NO_MEMORY && !command);
    device.attachStatus=STATUS_SUCCESS;source.blob=true;
    for (unsigned bit = 0; bit < 32; ++bit) {
        if (bit == 2 || bit == 3) continue;
        auto p = request(12u | (1u << bit));
        assert(device.Present(&p) == STATUS_NOT_SUPPORTED && !command);
    }
    auto shortBuffer = request(12); shortBuffer.DmaSize = sizeof(packet)-1;
    assert(device.Present(&shortBuffer) == STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER);
    assert(shortBuffer.MultipassOffset == 0 && !command);
    source.primary = false;
    auto invalid = request(12);
    assert(device.Present(&invalid) == STATUS_INVALID_PARAMETER && !command);
    puts("PASS normal and no-wait flips queue ordered DMA; unsupported modifiers and invalid inputs rejected");
}
