/* SPDX-License-Identifier: MIT
 * Test-only Vulkan allocation and pixel-pattern support for EGL copy fixtures.
 * No KVM probe or standalone executable is included.
 * Candidate pixels never pass through a staging allocation. Readback buffers
 * and glReadPixels below are explicitly labeled validation oracles.
 */
#define GL_GLEXT_PROTOTYPES
#include <vulkan/vulkan.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef GL_TEXTURE_EXTERNAL_OES
#define GL_TEXTURE_EXTERNAL_OES 0x8D65
#endif

static std::string quote(const std::string& s) {
    std::string r = "\"";
    for (unsigned char c : s) {
        if (c == '\\' || c == '"') r += '\\';
        if (c >= 32) r += c;
        else { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); r += b; }
    }
    return r + '"';
}
static void event(const char *stage, const char *status,
                  const std::string& detail = "") {
    printf("{\"stage\":%s,\"status\":%s%s%s}\n", quote(stage).c_str(),
           quote(status).c_str(), detail.empty() ? "" : ",", detail.c_str());
    fflush(stdout);
}
struct Stop : std::runtime_error {
    bool unsupported;
    Stop(const std::string& what, bool u = false) : runtime_error(what), unsupported(u) {}
};
static void vkcheck(VkResult r, const char *name, bool capability = false) {
    if (r != VK_SUCCESS) {
        bool unsupported = capability && (r == VK_ERROR_FORMAT_NOT_SUPPORTED ||
            r == VK_ERROR_FEATURE_NOT_PRESENT || r == VK_ERROR_EXTENSION_NOT_PRESENT);
        throw Stop(std::string(name) + ": VkResult=" + std::to_string(r), unsupported);
    }
}
static std::string number(const char *key, uint64_t n) {
    return quote(key) + ":" + std::to_string(n);
}
static std::string uuid(const uint8_t *bytes) {
    std::string result;
    for (unsigned i = 0; i < VK_UUID_SIZE; i++) {
        char hex[3]; snprintf(hex, sizeof(hex), "%02x", bytes[i]); result += hex;
    }
    return result;
}

struct Options {
    uint32_t width = 0, height = 0;
    bool bgra = true;
    std::string tiling, device = std::getenv("TRITON_TEST_DEVICE") ? std::getenv("TRITON_TEST_DEVICE") : "", cpuMap = "vulkan";
};

struct Vulkan {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    uint32_t family = 0;
    VkPhysicalDeviceMemoryProperties memories{};
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceIDProperties identities{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    std::vector<VkExtensionProperties> extensions;
    bool dma = false, modifiers = false;
    int64_t renderMajor = -1, renderMinor = -1;

    bool has(const char *n) const {
        return std::any_of(extensions.begin(), extensions.end(),
            [n](const VkExtensionProperties& p) { return !strcmp(n, p.extensionName); });
    }
    explicit Vulkan(const Options& o) {
      try {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "primary-direct-storage-capability";
        app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ci.pApplicationInfo = &app;
        vkcheck(vkCreateInstance(&ci, nullptr, &instance), "vkCreateInstance");
        uint32_t count = 0;
        vkcheck(vkEnumeratePhysicalDevices(instance, &count, nullptr), "enumerate devices");
        std::vector<VkPhysicalDevice> devices(count);
        vkcheck(vkEnumeratePhysicalDevices(instance, &count, devices.data()), "enumerate devices");
        for (auto p : devices) {
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(p, &props);
            if (std::string(props.deviceName).find(o.device) != std::string::npos) {
                physical = p; properties = props; break;
            }
        }
        if (!physical) throw Stop("requested Vulkan device missing", true);
        VkPhysicalDeviceProperties2 identityProps{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        identityProps.pNext = &identities;
        vkGetPhysicalDeviceProperties2(physical, &identityProps);
        event("device", "observed", "\"name\":" + quote(properties.deviceName) + "," +
              number("vendor", properties.vendorID) + "," + number("device", properties.deviceID) +
              "," + number("driver_version", properties.driverVersion) +
              ",\"device_uuid\":" + quote(uuid(identities.deviceUUID)) +
              ",\"driver_uuid\":" + quote(uuid(identities.driverUUID)));
        vkcheck(vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr), "extensions");
        extensions.resize(count);
        vkcheck(vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, extensions.data()), "extensions");
        vkGetPhysicalDeviceMemoryProperties(physical, &memories);
        for (uint32_t i = 0; i < memories.memoryTypeCount; i++)
            event("memory_type", "observed", number("index", i) + "," +
                number("flags", memories.memoryTypes[i].propertyFlags) + "," +
                number("heap", memories.memoryTypes[i].heapIndex));
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> queues(count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, queues.data());
        bool found = false;
        for (uint32_t i = 0; i < count; i++) if (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            family = i; found = true; break;
        }
        if (!found) throw Stop("graphics queue unavailable", true);
        std::vector<const char *> enabled;
        dma = has(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) && has(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
        modifiers = dma && has(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME) &&
                    has(VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME);
        if (dma) {
            enabled.push_back(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
            enabled.push_back(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
        }
        if (modifiers) {
            // The experiment requests Vulkan 1.1, so this dependency must be
            // enabled explicitly even when the physical device supports 1.2+.
            enabled.push_back(VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME);
            enabled.push_back(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
        }
        VkPhysicalDeviceLinearColorAttachmentFeaturesNV linear{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINEAR_COLOR_ATTACHMENT_FEATURES_NV};
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        if (has(VK_NV_LINEAR_COLOR_ATTACHMENT_EXTENSION_NAME)) {
            features.pNext = &linear;
            vkGetPhysicalDeviceFeatures2(physical, &features);
            if (linear.linearColorAttachment) enabled.push_back(VK_NV_LINEAR_COLOR_ATTACHMENT_EXTENSION_NAME);
        }
        if (has(VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME)) {
            VkPhysicalDeviceDrmPropertiesEXT drm{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT};
            VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
            props.pNext = &drm;
            vkGetPhysicalDeviceProperties2(physical, &props);
            if (drm.hasRender) { renderMajor = drm.renderMajor; renderMinor = drm.renderMinor; }
        }
        event("capabilities", "observed", std::string("\"dma_buf\":") + (dma ? "true" : "false") +
              ",\"drm_modifiers\":" + (modifiers ? "true" : "false") +
              ",\"linear_color_attachment\":" + (linear.linearColorAttachment ? "true" : "false") +
              ",\"render_major\":" + std::to_string(renderMajor) + ",\"render_minor\":" + std::to_string(renderMinor));
        float priority = 1.0f;
        VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        q.queueFamilyIndex = family; q.queueCount = 1; q.pQueuePriorities = &priority;
        VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dc.queueCreateInfoCount = 1; dc.pQueueCreateInfos = &q;
        dc.enabledExtensionCount = enabled.size(); dc.ppEnabledExtensionNames = enabled.data();
        if (linear.linearColorAttachment) dc.pNext = &linear;
        vkcheck(vkCreateDevice(physical, &dc, nullptr, &device), "vkCreateDevice");
        vkGetDeviceQueue(device, family, 0, &queue);
        VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pc.queueFamilyIndex = family;
        vkcheck(vkCreateCommandPool(device, &pc, nullptr, &pool), "command pool");
      } catch (...) {
        cleanup();
        throw;
      }
    }
    uint32_t memory(uint32_t mask, VkMemoryPropertyFlags flags) {
        for (uint32_t i = 0; i < memories.memoryTypeCount; i++)
            if ((mask & (1u << i)) && (memories.memoryTypes[i].propertyFlags & flags) == flags) return i;
        throw Stop("no compatible memory type with flags=" + std::to_string(flags), true);
    }
    VkCommandBuffer begin() {
        VkCommandBufferAllocateInfo a{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        a.commandPool = pool; a.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; a.commandBufferCount = 1;
        VkCommandBuffer c;
        vkcheck(vkAllocateCommandBuffers(device, &a, &c), "allocate commands");
        VkCommandBufferBeginInfo b{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        b.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkcheck(vkBeginCommandBuffer(c, &b), "begin commands"); return c;
    }
    void submit(VkCommandBuffer c) {
        vkcheck(vkEndCommandBuffer(c), "end commands");
        VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        struct Fence {
            VkDevice device;
            VkFence handle = VK_NULL_HANDLE;
            ~Fence() { if (handle) vkDestroyFence(device, handle, nullptr); }
        } fence{device};
        vkcheck(vkCreateFence(device, &fc, nullptr, &fence.handle), "create fence");
        VkSubmitInfo s{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        s.commandBufferCount = 1; s.pCommandBuffers = &c;
        vkcheck(vkQueueSubmit(queue, 1, &s, fence.handle), "queue submit");
        VkResult r = vkWaitForFences(device, 1, &fence.handle, VK_TRUE, 5000000000ull);
        if (r != VK_SUCCESS) {
            event("gpu_wait", "failed", "\"result\":" + std::to_string(r));
            _exit(3); // Do not enter an unbounded vkDeviceWaitIdle destructor.
        }
        vkFreeCommandBuffers(device, pool, 1, &c);
    }
    void cleanup() {
        if (device) { if (pool) vkDestroyCommandPool(device, pool, nullptr); vkDestroyDevice(device, nullptr); }
        if (instance) vkDestroyInstance(instance, nullptr);
        device = VK_NULL_HANDLE; instance = VK_NULL_HANDLE; pool = VK_NULL_HANDLE;
    }
    ~Vulkan() { cleanup(); }
};

using Pixel = std::array<uint8_t, 4>;
static const Pixel baseColor{17, 41, 93, 255};
static const Pixel colors[3]{{18, 52, 86, 128}, {201, 13, 71, 255}, {37, 149, 223, 64}};
static const Pixel cpuColor{231, 19, 117, 255};
static Pixel kvmColor(const Options& o, uint32_t x, uint32_t y) {
    // A bijection of the pixel index makes aliasing between probed pages
    // observable after every guest write has completed.
    uint32_t marker = (y * o.width + x) ^ 0xad1dd343u;
    return {uint8_t(marker), uint8_t(marker >> 8), uint8_t(marker >> 16), uint8_t(marker >> 24)};
}
static VkRect2D rectangle(const Options& o, unsigned i) {
    if (i == 0) return {{3, 5}, {11, 7}};
    if (i == 1) return {{int32_t(o.width / 2), int32_t(o.height / 3)}, {31, 13}};
    return {{int32_t(o.width - 17), int32_t(o.height - 19)}, {17, 19}};
}
static Pixel expected(const Options& o, uint32_t x, uint32_t y, bool patched,
                      const std::vector<uint8_t> *kvm = nullptr) {
    Pixel p = baseColor;
    for (unsigned i = 0; i < 3; i++) {
        VkRect2D r = rectangle(o, i);
        if (x >= uint32_t(r.offset.x) && y >= uint32_t(r.offset.y) &&
            x < uint32_t(r.offset.x) + r.extent.width && y < uint32_t(r.offset.y) + r.extent.height) p = colors[i];
    }
    if (patched && ((x == 1 && y == 2) || (x == o.width / 2 && y == o.height / 2) ||
                    (x == o.width - 2 && y == o.height - 3))) p = cpuColor;
    if (kvm && (*kvm)[uint64_t(y) * o.width + x]) p = kvmColor(o, x, y);
    return p;
}
static uint32_t packed(Pixel p, bool bgra) {
    if (bgra) std::swap(p[0], p[2]);
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
static bool checkPixels(const Options& o, const uint8_t *data, uint64_t pitch,
                        bool patched, const std::vector<uint8_t> *kvm,
                        bool storageBgra, const char *stage) {
    uint64_t errors = 0;
    uint32_t firstX = 0, firstY = 0, firstGot = 0, firstWant = 0;
    for (uint32_t y = 0; y < o.height; y++) for (uint32_t x = 0; x < o.width; x++) {
        uint32_t got;
        memcpy(&got, data + y * pitch + x * 4, 4);
        uint32_t want = packed(expected(o, x, y, patched, kvm), storageBgra);
        if (got != want && !errors++) { firstX = x; firstY = y; firstGot = got; firstWant = want; }
    }
    event(stage, errors ? "failed" : "passed", number("pixels", uint64_t(o.width) * o.height) + "," +
          number("errors", errors) + "," + number("first_x", firstX) + "," + number("first_y", firstY) +
          "," + number("first_actual", firstGot) + "," + number("first_expected", firstWant));
    return !errors;
}

struct Candidate {
    Vulkan& v;
    Options o;
    VkFormat format;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    VkSubresourceLayout layout{};
    void *map = nullptr;
    bool exported = false;
    int fd = -1;
    std::vector<uint8_t> kvmPixels;
    Candidate(Vulkan& context, const Options& options) : v(context), o(options),
        format(o.bgra ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8G8B8A8_UNORM) {
      try {
        bool drm = o.tiling == "drm-linear";
        exported = o.tiling != "linear";
        if (o.cpuMap == "dmabuf" && !exported) throw Stop("DMA-BUF CPU mapping requires an exported variant", true);
        if ((exported && !v.dma) || (drm && !v.modifiers)) throw Stop("requested export extensions absent", true);
        VkPhysicalDeviceImageDrmFormatModifierInfoEXT modQuery{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT};
        modQuery.drmFormatModifier = 0; modQuery.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkPhysicalDeviceExternalImageFormatInfo externalQuery{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
        externalQuery.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        if (drm) externalQuery.pNext = &modQuery;
        VkPhysicalDeviceImageFormatInfo2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
        query.format = format; query.type = VK_IMAGE_TYPE_2D;
        query.tiling = drm ? VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT : VK_IMAGE_TILING_LINEAR;
        query.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (exported) query.pNext = &externalQuery;
        VkExternalImageFormatProperties externalProperties{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
        VkImageFormatProperties2 properties{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
        if (exported) properties.pNext = &externalProperties;
        VkResult qr = vkGetPhysicalDeviceImageFormatProperties2(v.physical, &query, &properties);
        event("format_query", qr == VK_SUCCESS ? "supported" : "unsupported", "\"result\":" +
              std::to_string(qr) + "," + number("external_features", externalProperties.externalMemoryProperties.externalMemoryFeatures) +
              "," + number("compatible_handles", externalProperties.externalMemoryProperties.compatibleHandleTypes));
        vkcheck(qr, "image format query", true);
        if (o.width > properties.imageFormatProperties.maxExtent.width ||
            o.height > properties.imageFormatProperties.maxExtent.height ||
            !(properties.imageFormatProperties.sampleCounts & VK_SAMPLE_COUNT_1_BIT))
            throw Stop("exact dimensions or sample count unsupported", true);
        if (exported && !(externalProperties.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT))
            throw Stop("DMA-BUF export unsupported for exact image description", true);
        uint64_t linearModifier = 0;
        VkImageDrmFormatModifierListCreateInfoEXT mods{VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT};
        mods.drmFormatModifierCount = 1; mods.pDrmFormatModifiers = &linearModifier;
        VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        if (drm) external.pNext = &mods;
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D; ci.format = format; ci.extent = {o.width, o.height, 1};
        ci.mipLevels = ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = query.tiling; ci.usage = query.usage; ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (exported) ci.pNext = &external;
        vkcheck(vkCreateImage(v.device, &ci, nullptr, &image), "create candidate image", true);
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(v.device, image, &req);
        // KVM memory slots are page-sized. Allocate the complete final page,
        // rather than mapping beyond the Vulkan allocation or omitting the
        // image's tail. VkMemoryDedicatedAllocateInfo image-02964 permits an
        // allocation at least as large as the image requirement.
        size = (req.size + 4095) & ~VkDeviceSize(4095);
        uint32_t type = v.memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VkExportMemoryAllocateInfo exp{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
        exp.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicated.image = image;
        if (exported) dedicated.pNext = &exp;
        VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ma.pNext = &dedicated; ma.allocationSize = size; ma.memoryTypeIndex = type;
        event("candidate_memory", "selected", number("size", size) + "," + number("image_requirement", req.size) +
              "," + number("alignment", req.alignment) +
              "," + number("memory_type_bits", req.memoryTypeBits) + "," + number("memory_type", type) +
              "," + number("property_flags", v.memories.memoryTypes[type].propertyFlags));
        vkcheck(vkAllocateMemory(v.device, &ma, nullptr, &memory), "allocate candidate memory", true);
        vkcheck(vkBindImageMemory(v.device, image, memory, 0), "bind candidate memory");
        VkImageSubresource sub{drm ? VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
        vkGetImageSubresourceLayout(v.device, image, &sub, &layout);
        if (!layout.rowPitch || layout.rowPitch < uint64_t(o.width) * 4 || layout.offset > size ||
            uint64_t(o.width) * 4 > size - layout.offset ||
            o.height - 1 > (size - layout.offset - uint64_t(o.width) * 4) / layout.rowPitch)
            throw Stop("candidate has invalid mapped pixel layout");
        if (drm) {
            auto get = reinterpret_cast<PFN_vkGetImageDrmFormatModifierPropertiesEXT>(
                vkGetDeviceProcAddr(v.device, "vkGetImageDrmFormatModifierPropertiesEXT"));
            if (!get) throw Stop("missing DRM modifier query entry point");
            VkImageDrmFormatModifierPropertiesEXT p{VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT};
            vkcheck(get(v.device, image, &p), "get DRM modifier");
            if (p.drmFormatModifier != 0) throw Stop("driver selected non-linear modifier");
        }
        event("layout", "observed", number("offset", layout.offset) + "," + number("row_pitch", layout.rowPitch) +
              "," + number("layout_size", layout.size) + "," + number("allocation_size", size) +
              ",\"tight_vista_layout\":" + ((layout.offset == 0 && layout.rowPitch == uint64_t(o.width) * 4) ? "true" : "false"));
        if (o.cpuMap == "dmabuf") {
            exportFd();
            struct stat fdStat{};
            int statResult = fstat(fd, &fdStat);
            event("dma_buf_fd", statResult == 0 ? "observed" : "failed",
                  "\"stat_size\":" + std::to_string(fdStat.st_size) + ",\"errno\":" +
                  std::to_string(statResult < 0 ? errno : 0) + "," + number("requested_map_bytes", size));
            map = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (map == MAP_FAILED) {
                map = nullptr;
                throw Stop("exact candidate DMA-BUF mmap: errno=" + std::to_string(errno) + " " + strerror(errno), true);
            }
        } else {
            vkcheck(vkMapMemory(v.device, memory, 0, size, 0, &map), "direct candidate vkMapMemory", true);
        }
        event("candidate_map", "passed", "\"cpu_map\":" + quote(o.cpuMap) + "," +
              number("page_offset", reinterpret_cast<uintptr_t>(map) % 4096) + "," + number("mapped_bytes", size) +
              ",\"copied_storage\":false");
      } catch (...) {
        cleanup();
        throw;
      }
    }
    void exportFd() {
        if (fd >= 0) return;
        if (!exported) throw Stop("candidate does not support export", true);
        auto getFd = reinterpret_cast<PFN_vkGetMemoryFdKHR>(vkGetDeviceProcAddr(v.device, "vkGetMemoryFdKHR"));
        if (!getFd) throw Stop("missing vkGetMemoryFdKHR");
        VkMemoryGetFdInfoKHR fi{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
        fi.memory = memory; fi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        vkcheck(getFd(v.device, &fi, &fd), "export exact candidate DMA-BUF", true);
        event("dma_buf_export", "passed", "\"same_candidate_memory\":true");
    }
    uint8_t *pixels() { return static_cast<uint8_t *>(map) + layout.offset; }
    void cleanup() {
        if (map) {
            if (o.cpuMap == "dmabuf") munmap(map, size);
            else vkUnmapMemory(v.device, memory);
        }
        if (fd >= 0) close(fd);
        if (image) vkDestroyImage(v.device, image, nullptr);
        if (memory) vkFreeMemory(v.device, memory, nullptr);
        fd = -1; map = nullptr; image = VK_NULL_HANDLE; memory = VK_NULL_HANDLE;
    }
    ~Candidate() { cleanup(); }
};

static void externalOwnership(Candidate& a, bool release) {
    auto cmd = a.v.begin();
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = release ? VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT : 0;
    barrier.dstAccessMask = release ? 0 : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.oldLayout = barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = release ? a.v.family : VK_QUEUE_FAMILY_EXTERNAL;
    barrier.dstQueueFamilyIndex = release ? VK_QUEUE_FAMILY_EXTERNAL : a.v.family;
    barrier.image = a.image; barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, release ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         release ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);
    a.v.submit(cmd);
    event("external_ownership", "completed", std::string("\"direction\":") + quote(release ? "release" : "acquire"));
}

static VkClearColorValue clearColor(Pixel p) {
    VkClearColorValue c{};
    for (unsigned i = 0; i < 4; i++) c.float32[i] = float(p[i]) / 255.0f;
    return c;
}
static void gpuPattern(Candidate& a) {
    Vulkan& v = a.v;
    struct Objects {
        VkDevice device;
        VkImageView view = VK_NULL_HANDLE;
        VkRenderPass pass = VK_NULL_HANDLE;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        ~Objects() {
            if (framebuffer) vkDestroyFramebuffer(device, framebuffer, nullptr);
            if (pass) vkDestroyRenderPass(device, pass, nullptr);
            if (view) vkDestroyImageView(device, view, nullptr);
        }
    } objects{v.device};
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = a.image; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = a.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView& view = objects.view;
    vkcheck(vkCreateImageView(v.device, &vi, nullptr, &view), "candidate view");
    VkAttachmentDescription attachment{};
    attachment.format = a.format; attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; attachment.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_GENERAL};
    VkSubpassDescription sub{}; sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1; sub.pColorAttachments = &ref;
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount = 1; rp.pAttachments = &attachment; rp.subpassCount = 1; rp.pSubpasses = &sub;
    VkRenderPass& pass = objects.pass;
    vkcheck(vkCreateRenderPass(v.device, &rp, nullptr, &pass), "candidate render pass");
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = pass; fi.attachmentCount = 1; fi.pAttachments = &view;
    fi.width = a.o.width; fi.height = a.o.height; fi.layers = 1;
    VkFramebuffer& framebuffer = objects.framebuffer;
    vkcheck(vkCreateFramebuffer(v.device, &fi, nullptr, &framebuffer), "candidate framebuffer");
    auto cmd = v.begin();
    VkClearValue cv{}; cv.color = clearColor(baseColor);
    VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    bi.renderPass = pass; bi.framebuffer = framebuffer; bi.renderArea.extent = {a.o.width, a.o.height};
    bi.clearValueCount = 1; bi.pClearValues = &cv;
    vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
    for (unsigned i = 0; i < 3; i++) {
        VkClearAttachment ca{}; ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; ca.clearValue.color = clearColor(colors[i]);
        VkClearRect r{rectangle(a.o, i), 0, 1};
        vkCmdClearAttachments(cmd, 1, &ca, 1, &r);
    }
    vkCmdEndRenderPass(cmd);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    v.submit(cmd);
    event("candidate_gpu_write", "passed", "\"method\":\"render_pass_color_attachment_clear_rectangles\",\"staging\":false");
}

static bool glExternalIdentity(Candidate& a) {
    // EXTERNAL ownership requires the same driver as well as device. The
    // render-node match above alone is insufficient for a generic --device.
    GLint extensionCount = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &extensionCount);
    bool uuidQueries = false;
    for (GLint i = 0; i < extensionCount && i < 65536; i++) {
        const char *extension = reinterpret_cast<const char *>(glGetStringi(GL_EXTENSIONS, i));
        if (extension && (!strcmp(extension, "GL_EXT_memory_object") || !strcmp(extension, "GL_EXT_semaphore")))
            uuidQueries = true;
    }
    auto getUuid = reinterpret_cast<PFNGLGETUNSIGNEDBYTEVEXTPROC>(eglGetProcAddress("glGetUnsignedBytevEXT"));
    auto getDeviceUuid = reinterpret_cast<PFNGLGETUNSIGNEDBYTEI_VEXTPROC>(eglGetProcAddress("glGetUnsignedBytei_vEXT"));
    if (!uuidQueries || !getUuid || !getDeviceUuid || glGetError() != GL_NO_ERROR) {
        event("gl_external_identity", "unsupported", "\"reason\":\"GL device/driver UUID queries unavailable\"");
        return false;
    }
    std::array<GLubyte, GL_UUID_SIZE_EXT> driverUuid{};
    getUuid(GL_DRIVER_UUID_EXT, driverUuid.data());
    GLint deviceCount = 0;
    glGetIntegerv(GL_NUM_DEVICE_UUIDS_EXT, &deviceCount);
    bool deviceMatch = false;
    for (GLint i = 0; i < deviceCount && i < 256; i++) {
        std::array<GLubyte, GL_UUID_SIZE_EXT> deviceUuid{};
        getDeviceUuid(GL_DEVICE_UUID_EXT, i, deviceUuid.data());
        deviceMatch |= !memcmp(deviceUuid.data(), a.v.identities.deviceUUID, VK_UUID_SIZE);
    }
    GLenum identityError = glGetError();
    bool driverMatch = !memcmp(driverUuid.data(), a.v.identities.driverUUID, VK_UUID_SIZE);
    bool identityMatch = !identityError && driverMatch && deviceMatch && deviceCount <= 256;
    event("gl_external_identity", identityMatch ? "passed" : "unsupported",
          "\"driver_uuid\":" + quote(uuid(driverUuid.data())) + "," + number("gl_error", identityError) +
          ",\"driver_match\":" + (driverMatch ? "true" : "false") +
          ",\"device_match\":" + (deviceMatch ? "true" : "false"));
    return identityMatch;
}

