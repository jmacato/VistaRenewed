/* SPDX-License-Identifier: MIT
 * Real DXVK direct-primary storage test. Staging copies are explicitly
 * labeled observation oracles and are never candidate backing storage.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h>
#include <dxvk_shared_resource.h>
#include <linux/dma-buf.h>
#include <linux/kvm.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

template<typename T> struct Com {
  T* p = nullptr;
  ~Com() { if (p) p->Release(); }
  T* operator->() const { return p; }
  T** operator&() { return &p; }
  operator T*() const { return p; }
};
struct Fd { int fd = -1; ~Fd() { if (fd >= 0) close(fd); } };
struct Mapping {
  void* p = MAP_FAILED;
  size_t size = 0;
  ~Mapping() { if (p != MAP_FAILED) munmap(p, size); }
};
static unsigned checks = 0;
static void require(bool ok, const char* what) {
  if (!ok) throw std::runtime_error(what);
  ++checks;
}
static void hr(HRESULT result, const char* what) {
  if (FAILED(result)) {
    char msg[256]; snprintf(msg, sizeof(msg), "%s: HRESULT=%08x", what, unsigned(result));
    throw std::runtime_error(msg);
  }
  ++checks;
}
struct Device {
  Com<ID3D11Device> device;
  Com<ID3D11DeviceContext> context;
  Com<ID3D11DeviceContext1> context1;
  Device() {
    const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    hr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
      D3D11_SDK_VERSION, &device, nullptr, &context), "create hardware DXVK device");
    hr(context->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&context1), "context1");
    Com<IDXGIDevice> dxgi; Com<IDXGIAdapter> adapter;
    hr(device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgi), "DXGI device");
    hr(dxgi->GetAdapter(&adapter), "DXGI adapter");
    DXGI_ADAPTER_DESC desc{};
    hr(adapter->GetDesc(&desc), "DXGI adapter description");
    char name[129]{};
    for (unsigned i = 0; i < 128 && desc.Description[i]; ++i) name[i] = char(desc.Description[i]);
    printf("DEVICE name=%s vendor=%x device=%x\n", name, desc.VendorId, desc.DeviceId);
    require(desc.VendorId == 0x10de, "NVIDIA device required for retained capability comparison");
  }
  void complete() {
    Com<ID3D11Query> event;
    D3D11_QUERY_DESC desc{D3D11_QUERY_EVENT, 0};
    hr(device->CreateQuery(&desc, &event), "completion event");
    context->End(event); context->Flush();
    BOOL done = FALSE; HRESULT result = S_FALSE;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (result == S_FALSE && std::chrono::steady_clock::now() < deadline) {
      result = context->GetData(event, &done, sizeof(done), D3D11_ASYNC_GETDATA_DONOTFLUSH);
      if (result == S_FALSE) usleep(1000);
    }
    if (result != S_OK || !done) {
      fprintf(stderr, "FAIL bounded GPU completion\n"); fflush(nullptr); _exit(3);
    }
    ++checks;
  }
};

struct CpuAccess {
  int fd;
  bool active = false;
  void sync(bool start) {
    dma_buf_sync request{uint64_t(DMA_BUF_SYNC_RW | (start ? DMA_BUF_SYNC_START : DMA_BUF_SYNC_END))};
    int result;
    unsigned retries = 0;
    do { result = ioctl(fd, DMA_BUF_IOCTL_SYNC, &request); }
    while (result < 0 && (errno == EINTR || errno == EAGAIN) && ++retries < 64);
    require(result == 0, start ? "DMA-BUF CPU START" : "DMA-BUF CPU END");
  }
  explicit CpuAccess(int value) : fd(value) { sync(true); active = true; }
  void finish() { active = false; sync(false); }
  ~CpuAccess() {
    if (active) {
      dma_buf_sync request{DMA_BUF_SYNC_RW | DMA_BUF_SYNC_END};
      ioctl(fd, DMA_BUF_IOCTL_SYNC, &request);
    }
  }
};

using Color = std::array<uint8_t, 4>;
static uint32_t pack(Color p, bool bgra) {
  if (bgra) std::swap(p[0], p[2]);
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
static void pixels(const uint32_t* actual, const std::vector<uint32_t>& expected, const char* stage) {
  size_t errors = 0;
  for (size_t i = 0; i < expected.size(); ++i) errors += actual[i] != expected[i];
  printf("PIXELS stage=%s count=%zu errors=%zu\n", stage, expected.size(), errors);
  require(errors == 0, stage);
}
static void clearRect(Device& d, ID3D11RenderTargetView* rtv, const D3D11_RECT& rect,
                      Color color, unsigned width, bool bgra, std::vector<uint32_t>& expected) {
  FLOAT value[4];
  for (unsigned i = 0; i < 4; ++i) value[i] = float(color[i]) / 255.0f;
  d.context1->ClearView(rtv, value, &rect, 1);
  for (LONG y = rect.top; y < rect.bottom; ++y)
    for (LONG x = rect.left; x < rect.right; ++x) expected[size_t(y) * width + x] = pack(color, bgra);
}
static std::vector<uint32_t> gpuOracle(Device& d, ID3D11Texture2D* source) {
  D3D11_TEXTURE2D_DESC desc{}; source->GetDesc(&desc);
  desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ; desc.MiscFlags = 0;
  Com<ID3D11Texture2D> staging;
  hr(d.device->CreateTexture2D(&desc, nullptr, &staging), "create observation-only staging texture");
  d.context->CopyResource(staging, source);
  d.complete();
  D3D11_MAPPED_SUBRESOURCE map{};
  hr(d.context->Map(staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &map), "map observation oracle");
  std::vector<uint32_t> result(size_t(desc.Width) * desc.Height);
  for (unsigned y = 0; y < desc.Height; ++y)
    memcpy(result.data() + size_t(y) * desc.Width, (char*)map.pData + size_t(y) * map.RowPitch, desc.Width * 4);
  d.context->Unmap(staging, 0);
  return result;
}

static void guestMapping(void* pointer, size_t bytes, std::vector<uint32_t>& expected) {
  require(uintptr_t(pointer) % 4096 == 0 && bytes % 4096 == 0 && bytes < 0xe0000000u,
          "exact KVM pointer/full allocation alignment");
  std::vector<uint32_t> points;
  for (size_t i = 0; i < expected.size(); ++i)
    if (i % 1024 == 0 || i % 1024 == 1023 || i + 1 == expected.size()) points.push_back(i);
  std::vector<uint8_t> code;
  std::vector<size_t> branches;
  auto byte = [&](uint8_t v) { code.push_back(v); };
  auto word = [&](uint32_t v) { for (unsigned i = 0; i < 4; ++i) byte(uint8_t(v >> (i * 8))); };
  auto compare = [&](uint32_t index, uint32_t value) {
    byte(0xa1); word(0x10000000u + index * 4); byte(0x3d); word(value);
    byte(0x0f); byte(0x85); branches.push_back(code.size()); word(0);
  };
  for (auto p : points) compare(p, expected[p]);
  for (auto p : points) {
    expected[p] = p ^ 0xa174f83bu;
    byte(0xc7); byte(0x05); word(0x10000000u + p * 4); word(expected[p]);
  }
  for (auto p : points) compare(p, expected[p]);
  byte(0xb0); byte(0); byte(0xe6); byte(0xe9); byte(0xf4);
  size_t failed = code.size();
  byte(0xb0); byte(1); byte(0xe6); byte(0xe9); byte(0xf4);
  for (auto p : branches) {
    uint32_t delta = uint32_t(failed - p - 4);
    for (unsigned i = 0; i < 4; ++i) code[p + i] = uint8_t(delta >> (i * 8));
  }
  Fd kvm{open("/dev/kvm", O_RDWR | O_CLOEXEC)};
  require(kvm.fd >= 0 && ioctl(kvm.fd, KVM_GET_API_VERSION, 0) == KVM_API_VERSION, "KVM API");
  Fd vm{ioctl(kvm.fd, KVM_CREATE_VM, 0)};
  require(vm.fd >= 0, "create KVM VM");
  require(ioctl(vm.fd, KVM_SET_TSS_ADDR, 0xfffbd000ul) == 0, "KVM TSS");
  uint64_t identity = 0xfffbc000ul;
  require(ioctl(vm.fd, KVM_SET_IDENTITY_MAP_ADDR, &identity) == 0, "KVM identity map");
  size_t ramBytes = (code.size() + 0x2fff) & ~size_t(4095);
  Mapping ram{mmap(nullptr, ramBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0), ramBytes};
  require(ram.p != MAP_FAILED, "guest code memory");
  memcpy((char*)ram.p + 0x1000, code.data(), code.size());
  kvm_userspace_memory_region codeSlot{0, 0, 0, ramBytes, uintptr_t(ram.p)};
  kvm_userspace_memory_region pixelSlot{1, 0, 0x10000000, bytes, uintptr_t(pointer)};
  require(ioctl(vm.fd, KVM_SET_USER_MEMORY_REGION, &codeSlot) == 0, "guest code slot");
  require(ioctl(vm.fd, KVM_SET_USER_MEMORY_REGION, &pixelSlot) == 0, "exact DMA-BUF full allocation KVM slot");
  Fd cpu{ioctl(vm.fd, KVM_CREATE_VCPU, 0)};
  require(cpu.fd >= 0, "KVM CPU");
  int runSize = ioctl(kvm.fd, KVM_GET_VCPU_MMAP_SIZE, 0);
  require(runSize >= int(sizeof(kvm_run)), "KVM run size");
  Mapping runMap{mmap(nullptr, runSize, PROT_READ | PROT_WRITE, MAP_SHARED, cpu.fd, 0), size_t(runSize)};
  require(runMap.p != MAP_FAILED, "KVM run map");
  kvm_sregs s{};
  require(ioctl(cpu.fd, KVM_GET_SREGS, &s) == 0, "KVM get segments");
  s.cs = {}; s.cs.base = 0; s.cs.limit = 0xffffffff; s.cs.selector = 8;
  s.cs.type = 11; s.cs.present = 1; s.cs.s = 1; s.cs.db = 1; s.cs.g = 1;
  s.ds = s.es = s.fs = s.gs = s.ss = s.cs;
  s.ds.type = s.es.type = s.fs.type = s.gs.type = s.ss.type = 3;
  s.ds.selector = s.es.selector = s.fs.selector = s.gs.selector = s.ss.selector = 16;
  s.cr0 |= 1; s.cr0 &= ~(uint64_t(1) << 31); s.cr3 = 0; s.cr4 = 0; s.efer = 0;
  require(ioctl(cpu.fd, KVM_SET_SREGS, &s) == 0, "KVM set segments");
  kvm_regs regs{}; regs.rip = 0x1000; regs.rsp = ramBytes - 16; regs.rflags = 2;
  require(ioctl(cpu.fd, KVM_SET_REGS, &regs) == 0, "KVM registers");
  alarm(3); int rc = ioctl(cpu.fd, KVM_RUN, 0); alarm(0);
  auto run = static_cast<kvm_run*>(runMap.p);
  require(rc == 0 && run->exit_reason == KVM_EXIT_IO && run->io.direction == KVM_EXIT_IO_OUT
       && run->io.port == 0xe9 && run->io.size == 1 && run->io.count == 1
       && *((uint8_t*)runMap.p + run->io.data_offset) == 0, "guest checks and unique page writes");
  pixels(static_cast<const uint32_t*>(pointer), expected, "host observes guest writes");
  printf("KVM exact_fd_pointer=true mapped_bytes=%zu probes=%zu pages=%zu\n",
         bytes, points.size(), (expected.size() * 4 + 4095) / 4096);
}

static void negativeDescriptions(Device& d, D3D11_TEXTURE2D_DESC base) {
  std::vector<D3D11_TEXTURE2D_DESC> invalid;
  auto push = [&](auto edit) { auto desc = base; edit(desc); invalid.push_back(desc); };
  push([](auto& v) { v.MiscFlags &= ~D3D11_RESOURCE_MISC_SHARED; });
  push([](auto& v) { v.CPUAccessFlags = D3D11_CPU_ACCESS_READ; });
  push([](auto& v) { v.Usage = D3D11_USAGE_STAGING; });
  push([](auto& v) { v.ArraySize = 2; });
  push([](auto& v) { v.MipLevels = 0; });
  push([](auto& v) { v.MipLevels = 2; });
  push([](auto& v) { v.SampleDesc.Count = 2; });
  push([](auto& v) { v.SampleDesc.Quality = 1; });
  push([](auto& v) { v.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; });
  push([](auto& v) { v.Format = DXGI_FORMAT_B8G8R8X8_UNORM; });
  push([](auto& v) { v.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS; });
  push([](auto& v) { v.BindFlags |= D3D11_BIND_UNORDERED_ACCESS; });
  push([](auto& v) { v.Width = 0; });
  push([](auto& v) { v.MiscFlags |= D3D11_RESOURCE_MISC_GDI_COMPATIBLE; });
  for (auto desc : invalid) {
    Com<ID3D11Texture2D> rejected;
    require(FAILED(d.device->CreateTexture2D(&desc, nullptr, &rejected)) && !rejected.p,
            "invalid direct-primary request rejected");
  }
  printf("INVALID_DESCRIPTIONS rejected=%zu\n", invalid.size());
}

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IOLBF, 0);
  try {
    require(argc == 4, "arguments: width height bgra|rgba");
    unsigned width = std::stoul(argv[1]), height = std::stoul(argv[2]);
    bool bgra = std::string(argv[3]) == "bgra";
    require(width >= 64 && width <= 1920 && height >= 64 && height <= 1080
         && (bgra || std::string(argv[3]) == "rgba"), "bounded fixture dimensions/format");
    Device producer;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width; desc.Height = height; desc.MipLevels = desc.ArraySize = 1;
    desc.Format = bgra ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | DXVK_D3D11_RESOURCE_MISC_DIRECT_PRIMARY;
    negativeDescriptions(producer, desc);
    Com<ID3D11Texture2D> texture;
    hr(producer.device->CreateTexture2D(&desc, nullptr, &texture), "create actual DXVK direct primary");
    D3D11_TEXTURE2D_DESC publicDesc{}; texture->GetDesc(&publicDesc);
    require(publicDesc.MiscFlags == D3D11_RESOURCE_MISC_SHARED && publicDesc.CPUAccessFlags == 0,
            "public description strips private contract");
    Com<ID3D11RenderTargetView> rtv;
    hr(producer.device->CreateRenderTargetView(texture, nullptr, &rtv), "direct primary RTV");
    Com<IDXGIResource> resource;
    hr(texture->QueryInterface(__uuidof(IDXGIResource), (void**)&resource), "shared resource");
    HANDLE handle = nullptr;
    hr(resource->GetSharedHandle(&handle), "get direct allocation descriptor");
    require(handle != nullptr && handle != INVALID_HANDLE_VALUE, "valid descriptor pointer");
    auto descriptor = *static_cast<DxvkSharedTextureDescriptor*>(handle);
    require(descriptor.magic == DXVK_SHARED_DESCRIPTOR_TEXTURE && descriptor.version == 1
         && descriptor.structSize == sizeof(descriptor)
         && descriptor.meta.MiscFlags == desc.MiscFlags, "descriptor preserves private contract and ABI");
    require(descriptor.planeCount == 1 && descriptor.drmFormatModifier == 0
         && descriptor.planes[0].offset == 0 && descriptor.planes[0].pitch == width * 4
         && descriptor.allocationSize >= uint64_t(width) * height * 4
         && descriptor.allocationSize % 4096 == 0, "exact direct storage layout");
    Fd fd{dup(descriptor.fd)}; require(fd.fd >= 0, "retain exported fd"); descriptor.fd = fd.fd;
    Mapping map{mmap(nullptr, descriptor.allocationSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd.fd, 0),
                size_t(descriptor.allocationSize)};
    require(map.p != MAP_FAILED, "map exact exported allocation");
    printf("STORAGE width=%u height=%u format=%s offset=%llu pitch=%llu allocation=%llu\n",
      width, height, argv[3], (unsigned long long)descriptor.planes[0].offset,
      (unsigned long long)descriptor.planes[0].pitch, (unsigned long long)descriptor.allocationSize);
    std::vector<uint32_t> expected(size_t(width) * height);
    clearRect(producer, rtv, {0, 0, LONG(width), LONG(height)}, {17, 41, 93, 255}, width, bgra, expected);
    clearRect(producer, rtv, {3, 5, 14, 12}, {18, 52, 86, 128}, width, bgra, expected);
    clearRect(producer, rtv, {LONG(width / 2), LONG(height / 3), LONG(width / 2 + 31), LONG(height / 3 + 13)},
              {201, 13, 71, 255}, width, bgra, expected);
    clearRect(producer, rtv, {LONG(width - 17), LONG(height - 19), LONG(width), LONG(height)},
              {37, 149, 223, 64}, width, bgra, expected);
    producer.complete();
    {
      CpuAccess access(fd.fd);
      pixels((uint32_t*)map.p, expected, "direct CPU observes DXVK GPU pattern before any readback");
      for (auto p : {size_t(width + 1), expected.size() / 2, expected.size() - 2})
        ((uint32_t*)map.p)[p] = expected[p] = uint32_t(p) ^ 0xf195ba70u;
      std::atomic_thread_fence(std::memory_order_seq_cst);
      guestMapping(map.p, map.size, expected);
      access.finish();
    }
    auto oracle = gpuOracle(producer, texture);
    pixels(oracle.data(), expected, "producer GPU observes CPU/KVM changes; observation-only readback");
    Device consumer;
    Com<ID3D11Texture2D> imported;
    hr(consumer.device->OpenSharedResource(&descriptor, __uuidof(ID3D11Texture2D), (void**)&imported),
       "import actual direct-primary descriptor on second DXVK device");
    imported->GetDesc(&publicDesc);
    require(publicDesc.MiscFlags == D3D11_RESOURCE_MISC_SHARED && publicDesc.CPUAccessFlags == 0,
            "imported public description strips private contract");
    oracle = gpuOracle(consumer, imported);
    pixels(oracle.data(), expected, "imported GPU observes exact allocation");
    Com<ID3D11RenderTargetView> importedRtv;
    hr(consumer.device->CreateRenderTargetView(imported, nullptr, &importedRtv), "imported primary RTV");
    for (unsigned iteration = 0; iteration < 3; ++iteration) {
      clearRect(consumer, importedRtv, {19, 23, 43, 39},
                {uint8_t(71 + iteration), 191, 37, 128}, width, bgra, expected);
      consumer.complete();
      { CpuAccess access(fd.fd); pixels((uint32_t*)map.p, expected, "CPU observes imported GPU writes"); access.finish(); }
      oracle = gpuOracle(producer, texture);
      pixels(oracle.data(), expected, "producer observes imported GPU writes");
    }
    // Default shared resources retain their public and transport descriptions.
    auto ordinaryDesc = desc; ordinaryDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    Com<ID3D11Texture2D> ordinary;
    hr(producer.device->CreateTexture2D(&ordinaryDesc, nullptr, &ordinary), "ordinary shared allocation control");
    ordinary->GetDesc(&publicDesc);
    require(publicDesc.MiscFlags == ordinaryDesc.MiscFlags, "ordinary shared flags unchanged");
    printf("RESULT direct_primary_dxvk_pass checks=%u application_performance_proven=false persistent_vista_mapping_proven=false\n", checks);
    return 0;
  } catch (const std::exception& error) {
    fprintf(stderr, "RESULT failed checks=%u reason=%s\n", checks, error.what()); return 1;
  }
}
