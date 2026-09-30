/* SPDX-License-Identifier: MIT
 * CPU regression for external memory mapping. Production allocator/map bodies
 * are inserted by the runner; Vulkan allocation and mapping are simulated.
 */
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <vector>

using VkDeviceSize = unsigned long long;
using VkMemoryPropertyFlags = unsigned;
using VkResult = int;
constexpr int VK_NULL_HANDLE = 0, VK_SUCCESS = 0;
constexpr unsigned VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT = 2;
constexpr unsigned VK_EXTERNAL_MEMORY_HANDLE_TYPE_FLAG_BITS_MAX_ENUM = 0x7fffffffu;
#define likely(x) (x)
namespace dxvk { using mutex = std::mutex; }

static std::array<unsigned char, 4096> pixels;
static unsigned cpuClears, mapCalls, unmapCalls;
namespace bit {
std::vector<int> BitMask(unsigned mask) {
  return mask ? std::vector<int>{0} : std::vector<int>{};
}
void bclear(void* p, size_t size) {
  ++cpuClears;
  std::memset(p, 0, size);
}
}
namespace str {
template<typename... T> std::string format(T...) { return {}; }
}
struct Logger { static void debug(const std::string&) {} };
struct DxvkError : std::runtime_error { using std::runtime_error::runtime_error; };
template<typename T> using Rc = std::shared_ptr<T>;
struct VkMemoryRequirements { VkDeviceSize size = 4096; unsigned memoryTypeBits = 1; };
struct DxvkAllocationInfo {
  unsigned properties = 7;
  int* importedFd = nullptr;
  unsigned handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_FLAG_BITS_MAX_ENUM;
};
struct DxvkDeviceMemory {
  int memory = 0;
  VkDeviceSize size = 4096;
  void* mapPtr = nullptr;
};
struct DxvkResourceAllocation {};
struct Type {};
struct Functions {
  int device() { return 0; }
  int vkMapMemory(int, int, VkDeviceSize, VkDeviceSize, unsigned, void** pointer) {
    ++mapCalls;
    *pointer = pixels.data();
    return VK_SUCCESS;
  }
  void vkUnmapMemory(int, int) { ++unmapCalls; }
};
struct Device {
  Functions functions;
  struct Config { bool zeroMappedMemory = false; } options;
  Functions* vkd() { return &functions; }
  Config& config() { return options; }
};
struct DxvkMemoryAllocator {
  std::mutex m_mutex;
  Type m_memTypes[1];
  Device* m_device;
  unsigned getMemoryTypeMask(unsigned) { return 1; }
  DxvkDeviceMemory allocateDeviceMemory(Type&, VkDeviceSize size, const void*, int* fd) {
    if (fd) *fd = -1;
    return {1, size, nullptr};
  }
  Rc<DxvkResourceAllocation> createAllocation(
      Type&, const DxvkDeviceMemory&, const DxvkAllocationInfo&) {
    return std::make_shared<DxvkResourceAllocation>();
  }
  void freeDeviceMemory(Type&, DxvkDeviceMemory) {}
  Rc<DxvkResourceAllocation> allocateDedicatedMemory(
      const VkMemoryRequirements&, const DxvkAllocationInfo&, const void*);
  void mapDeviceMemory(DxvkDeviceMemory&, VkMemoryPropertyFlags, bool = true);
};

/* PRODUCTION_FUNCTIONS */

static unsigned checks, failures;
static void check(bool good, const char* name) {
  ++checks;
  if (!good) {
    ++failures;
    std::printf("FAIL %s\n", name);
  }
}
static void pattern() {
  for (size_t i = 0; i < pixels.size(); ++i)
    pixels[i] = (i * 37u + 91u) % 255u + 1u;
  cpuClears = mapCalls = unmapCalls = 0;
}

int main() {
  // Unshared, DMA-BUF export/import, and another external-handle type.
  const unsigned handles[] = {
    VK_EXTERNAL_MEMORY_HANDLE_TYPE_FLAG_BITS_MAX_ENUM, 512, 4,
  };
  for (bool clear : {false, true}) {
    for (auto handle : handles) {
      for (bool importing : {false, true}) {
        if (importing && handle == VK_EXTERNAL_MEMORY_HANDLE_TYPE_FLAG_BITS_MAX_ENUM)
          continue;
        pattern();
        auto expected = pixels;
        bool expectClear = clear && handle == VK_EXTERNAL_MEMORY_HANDLE_TYPE_FLAG_BITS_MAX_ENUM;
        if (expectClear) expected.fill(0);
        Device device;
        device.options.zeroMappedMemory = clear;
        DxvkMemoryAllocator allocator;
        allocator.m_device = &device;
        int fd = 19;
        DxvkAllocationInfo info;
        info.handleType = handle;
        info.importedFd = importing ? &fd : nullptr;
        auto allocation = allocator.allocateDedicatedMemory({}, info, nullptr);
        check(bool(allocation), "allocation survives mapping");
        check(mapCalls == 1 && !unmapCalls, "one real mapping call");
        check(pixels == expected, "external payload preserved / ordinary debug clear retained");
        check(cpuClears == unsigned(expectClear), "no CPU initialization of external allocation");
        check(fd == (importing ? -1 : 19), "mapping does not alter fd ownership");
      }
    }
  }
  Device device;
  device.options.zeroMappedMemory = true;
  DxvkMemoryAllocator allocator;
  allocator.m_device = &device;
  pattern();
  const auto expected = pixels;
  DxvkDeviceMemory memory{1, pixels.size(), pixels.data()};
  allocator.mapDeviceMemory(memory, 7, false);
  check(!mapCalls && !cpuClears && pixels == expected, "already mapped external payload unchanged");
  allocator.mapDeviceMemory(memory, 0, false);
  check(unmapCalls == 1 && !memory.mapPtr && pixels == expected, "unmap preserves pixels");
  allocator.mapDeviceMemory(memory, 7, false);
  check(mapCalls == 1 && !cpuClears && pixels == expected, "external remap preserves pixels");
  pattern();
  memory.mapPtr = nullptr;
  allocator.mapDeviceMemory(memory, 0);
  check(!mapCalls && !cpuClears && !unmapCalls, "nonvisible allocation is not mapped or cleared");
  allocator.mapDeviceMemory(memory, 7);
  check(mapCalls == 1 && cpuClears == 1, "default ordinary mapping still clears");
  std::printf("checks=%u failures=%u\n", checks, failures);
  return failures ? 1 : 0;
}
