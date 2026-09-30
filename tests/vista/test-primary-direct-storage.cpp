/* SPDX-License-Identifier: MIT
 * Bounded native capability experiment, not a driver implementation.
 * Candidate pixels never pass through a staging allocation. Readback buffers
 * and glReadPixels below are explicitly labeled validation oracles.
 */
#define GL_GLEXT_PROTOTYPES
#include <vulkan/vulkan.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <linux/kvm.h>
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
    std::string tiling, device = "NVIDIA", cpuMap = "vulkan";
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

struct DmaBufCPUAccess {
    int fd;
    bool active = false;
    bool sync(bool start) {
        dma_buf_sync request{uint64_t(DMA_BUF_SYNC_RW | (start ? DMA_BUF_SYNC_START : DMA_BUF_SYNC_END))};
        int rc = -1, saved = 0;
        unsigned retries = 0;
        do {
            rc = ioctl(fd, DMA_BUF_IOCTL_SYNC, &request);
            saved = rc < 0 ? errno : 0;
        } while (rc < 0 && (saved == EINTR || saved == EAGAIN) && ++retries < 64);
        event("dma_buf_cpu_sync", rc == 0 ? "passed" : "failed",
              std::string("\"phase\":") + quote(start ? "start_rw" : "end_rw") +
              ",\"errno\":" + std::to_string(saved) + "," + number("retries", retries));
        return rc == 0;
    }
    explicit DmaBufCPUAccess(int descriptor) : fd(descriptor) {
        if (fd >= 0) {
            if (!sync(true)) throw Stop("DMA-BUF CPU access synchronization unsupported or failed", true);
            active = true;
        }
    }
    void finish() {
        if (active) {
            active = false;
            if (!sync(false)) throw Stop("DMA-BUF end CPU access failed");
        }
    }
    ~DmaBufCPUAccess() { if (active) sync(false); }
};

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

static bool kvmCheck(Candidate& a) {
#if !defined(__x86_64__)
    event("kvm_exact_mapping", "unsupported", "\"reason\":\"probe requires x86_64 host\""); return false;
#else
    struct Fd { int n; explicit Fd(int f = -1) : n(f) {} ~Fd() { if (n >= 0) close(n); } };
    const uint64_t bytes = a.size;
    if (reinterpret_cast<uintptr_t>(a.map) % 4096 || bytes % 4096 || bytes >= 0xe0000000u ||
        a.layout.offset % 4 || a.layout.rowPitch % 4) {
        event("kvm_exact_mapping", "unsupported", "\"reason\":\"exact candidate pointer/range not suitable for page-aligned KVM slot\""); return false;
    }
    // Exercise a real pixel in every physical page containing image pixels,
    // plus both image endpoints and pixels adjacent to page boundaries.
    std::vector<std::pair<uint32_t, uint32_t>> points;
    std::vector<uint8_t> pages(bytes / 4096, 0);
    a.kvmPixels.assign(uint64_t(a.o.width) * a.o.height, 0);
    for (uint32_t y = 0; y < a.o.height; y++) for (uint32_t x = 0; x < a.o.width; x++) {
        uint64_t offset = a.layout.offset + y * a.layout.rowPitch + x * 4;
        uint64_t page = offset / 4096;
        if (!pages[page] || (offset % 4096 == 4092) ||
            (x == a.o.width - 1 && y == a.o.height - 1) || (x == 7 && y == 9)) {
            pages[page] = 1;
            points.emplace_back(x, y);
            a.kvmPixels[uint64_t(y) * a.o.width + x] = 1;
        }
    }
    std::vector<uint8_t> code;
    std::vector<size_t> failureBranches;
    auto byte = [&](uint8_t b) { code.push_back(b); };
    auto u32 = [&](uint32_t n) { for (unsigned i = 0; i < 4; i++) byte(uint8_t(n >> (8 * i))); };
    auto address = [&](const auto& p) { return uint32_t(0x10000000 + a.layout.offset + p.second * a.layout.rowPitch + p.first * 4); };
    auto compare = [&](const auto& p, uint32_t want) {
        byte(0xa1); u32(address(p)); // mov eax, [candidate pixel]
        byte(0x3d); u32(want);
        byte(0x0f); byte(0x85); failureBranches.push_back(code.size()); u32(0); // jne rel32
    };
    // Check all old values before modifying any pixels.
    for (auto p : points) compare(p, packed(expected(a.o, p.first, p.second, true), a.o.bgra));
    for (auto p : points) {
        byte(0xc7); byte(0x05); u32(address(p)); u32(packed(kvmColor(a.o, p.first, p.second), a.o.bgra));
    }
    for (auto p : points) compare(p, packed(kvmColor(a.o, p.first, p.second), a.o.bgra));
    byte(0xb0); byte(0); byte(0xe6); byte(0xe9); byte(0xf4);
    size_t bad = code.size();
    byte(0xb0); byte(1); byte(0xe6); byte(0xe9); byte(0xf4);
    for (size_t branch : failureBranches) {
        uint32_t displacement = uint32_t(bad - branch - 4);
        for (unsigned i = 0; i < 4; i++) code[branch + i] = uint8_t(displacement >> (8 * i));
    }
    const size_t ramBytes = (code.size() + 0x2fff) & ~size_t(4095);
    Fd kvm(open("/dev/kvm", O_RDWR | O_CLOEXEC));
    auto fail = [](const char *where) { event("kvm_exact_mapping", "failed", "\"operation\":" + quote(where) + ",\"errno\":" + std::to_string(errno) + ",\"error\":" + quote(strerror(errno))); return false; };
    if (kvm.n < 0) return fail("open /dev/kvm");
    if (ioctl(kvm.n, KVM_GET_API_VERSION, 0) != KVM_API_VERSION) return fail("KVM_GET_API_VERSION");
    Fd vm(ioctl(kvm.n, KVM_CREATE_VM, 0));
    if (vm.n < 0) return fail("KVM_CREATE_VM");
    if (ioctl(vm.n, KVM_SET_TSS_ADDR, 0xfffbd000ul) < 0) return fail("KVM_SET_TSS_ADDR");
    uint64_t identity = 0xfffbc000ul;
    if (ioctl(vm.n, KVM_SET_IDENTITY_MAP_ADDR, &identity) < 0) return fail("KVM_SET_IDENTITY_MAP_ADDR");
    void *ram = mmap(nullptr, ramBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ram == MAP_FAILED) return fail("guest code mmap");
    struct Ram { void *p; size_t n; ~Ram() { munmap(p, n); } } release{ram, ramBytes};
    kvm_userspace_memory_region codeSlot{0, 0, 0, ramBytes, reinterpret_cast<uint64_t>(ram)};
    if (ioctl(vm.n, KVM_SET_USER_MEMORY_REGION, &codeSlot) < 0) return fail("code memory slot");
    kvm_userspace_memory_region imageSlot{1, 0, 0x10000000, bytes, reinterpret_cast<uint64_t>(a.map)};
    if (ioctl(vm.n, KVM_SET_USER_MEMORY_REGION, &imageSlot) < 0) return fail("exact candidate pointer memory slot");
    event("kvm_memory_slot", "accepted", number("mapped_bytes", bytes) + "," + number("allocation_bytes", a.size) +
          "," + number("pixel_probes", points.size()) + "," +
          number("pixel_pages", std::count(pages.begin(), pages.end(), 1)) +
          ",\"complete_allocation_mapped\":true,\"cpu_map\":" + quote(a.o.cpuMap) +
          ",\"exact_candidate_pointer\":true,\"bounce_buffer\":false");
    memcpy(static_cast<uint8_t *>(ram) + 0x1000, code.data(), code.size());
    Fd cpu(ioctl(vm.n, KVM_CREATE_VCPU, 0));
    if (cpu.n < 0) return fail("KVM_CREATE_VCPU");
    int runSize = ioctl(kvm.n, KVM_GET_VCPU_MMAP_SIZE, 0);
    if (runSize < int(sizeof(kvm_run))) return fail("KVM_GET_VCPU_MMAP_SIZE");
    void *runMemory = mmap(nullptr, runSize, PROT_READ | PROT_WRITE, MAP_SHARED, cpu.n, 0);
    if (runMemory == MAP_FAILED) return fail("kvm_run mmap");
    struct RunMap { void *p; size_t n; ~RunMap() { munmap(p, n); } } runRelease{runMemory, size_t(runSize)};
    auto run = static_cast<kvm_run *>(runMemory);
    kvm_sregs s{};
    if (ioctl(cpu.n, KVM_GET_SREGS, &s) < 0) return fail("KVM_GET_SREGS");
    s.cs = {}; s.cs.base = 0; s.cs.limit = 0xffffffff; s.cs.selector = 8;
    s.cs.type = 11; s.cs.present = 1; s.cs.s = 1; s.cs.db = 1; s.cs.g = 1;
    s.ds = s.es = s.fs = s.gs = s.ss = s.cs;
    s.ds.type = s.es.type = s.fs.type = s.gs.type = s.ss.type = 3;
    s.ds.selector = s.es.selector = s.fs.selector = s.gs.selector = s.ss.selector = 16;
    s.cr0 |= 1; s.cr0 &= ~(uint64_t(1) << 31); s.cr3 = 0; s.cr4 = 0; s.efer = 0;
    if (ioctl(cpu.n, KVM_SET_SREGS, &s) < 0) return fail("KVM_SET_SREGS");
    kvm_regs regs{}; regs.rip = 0x1000; regs.rsp = ramBytes - 16; regs.rflags = 2;
    if (ioctl(cpu.n, KVM_SET_REGS, &regs) < 0) return fail("KVM_SET_REGS");
    event("kvm_execute", "starting", "\"timeout_seconds\":3");
    signal(SIGALRM, SIG_DFL); alarm(3); // Per-case process is sacrificed on a stuck ioctl.
    int rc = ioctl(cpu.n, KVM_RUN, 0);
    alarm(0);
    if (rc < 0) return fail("KVM_RUN");
    bool ok = run->exit_reason == KVM_EXIT_IO && run->io.direction == KVM_EXIT_IO_OUT &&
              run->io.port == 0xe9 && run->io.size == 1 && run->io.count == 1 &&
              *(static_cast<uint8_t *>(runMemory) + run->io.data_offset) == 0;
    uint64_t errors = 0;
    for (auto p : points) {
        uint32_t actual;
        memcpy(&actual, a.pixels() + p.second * a.layout.rowPitch + p.first * 4, 4);
        errors += actual != packed(kvmColor(a.o, p.first, p.second), a.o.bgra);
    }
    ok &= !errors;
    event("kvm_exact_mapping", ok ? "passed" : "failed", number("exit_reason", run->exit_reason) +
          "," + number("host_observed_errors", errors) + "," + number("pixel_probes", points.size()) +
          ",\"complete_allocation_mapped\":true");
    return ok;
#endif
}

static bool gpuOracle(Candidate& a, bool kvm) {
    Vulkan& v = a.v;
    struct Objects {
        VkDevice device;
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void *map = nullptr;
        ~Objects() {
            if (map) vkUnmapMemory(device, memory);
            if (buffer) vkDestroyBuffer(device, buffer, nullptr);
            if (memory) vkFreeMemory(device, memory, nullptr);
        }
    } objects{v.device};
    VkBufferCreateInfo bc{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bc.size = uint64_t(a.o.width) * a.o.height * 4; bc.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bc.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer& buffer = objects.buffer;
    vkcheck(vkCreateBuffer(v.device, &bc, nullptr, &buffer), "oracle buffer");
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(v.device, buffer, &req);
    VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ma.allocationSize = req.size;
    ma.memoryTypeIndex = v.memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory& memory = objects.memory;
    vkcheck(vkAllocateMemory(v.device, &ma, nullptr, &memory), "oracle memory");
    vkcheck(vkBindBufferMemory(v.device, buffer, memory, 0), "oracle binding");
    auto cmd = v.begin();
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copy.imageExtent = {a.o.width, a.o.height, 1};
    vkCmdCopyImageToBuffer(cmd, a.image, VK_IMAGE_LAYOUT_GENERAL, buffer, 1, &copy);
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    v.submit(cmd);
    void *&map = objects.map;
    vkcheck(vkMapMemory(v.device, memory, 0, req.size, 0, &map), "oracle map");
    event("oracle_readback", "observed", "\"candidate_path\":false,\"purpose\":\"verify GPU consumes CPU and KVM writes\"");
    bool ok = checkPixels(a.o, static_cast<uint8_t *>(map), uint64_t(a.o.width) * 4, true,
                          kvm ? &a.kvmPixels : nullptr, a.o.bgra, "gpu_observed_cpu_writes");
    return ok;
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

static bool glExtension(const char *wanted) {
    GLint count = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &count);
    for (GLint i = 0; i < count && i < 65536; i++) {
        const char *name = reinterpret_cast<const char *>(glGetStringi(GL_EXTENSIONS, i));
        if (name && !strcmp(name, wanted)) return true;
    }
    return false;
}

struct GLResult {
    bool framebuffer = false, sampling = false;
    bool externalDesktop = false, externalGles = false;
};

static bool glSampleOracle(Candidate& a, GLuint source, bool kvm,
                           GLenum target = GL_TEXTURE_2D, bool gles = false) {
    bool external = target == GL_TEXTURE_EXTERNAL_OES;
    const std::string mode = std::string("\"api\":") + quote(gles ? "gles3" : "desktop_gl") +
                             ",\"target\":" + quote(external ? "external_oes" : "texture_2d");
    std::string vertex = gles ? "#version 300 es\n" : external ? "#version 130\n" : "#version 330 core\n";
    vertex +=
        "void main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);"
        "gl_Position=vec4(p*2.0-1.0,0.0,1.0);}";
    std::string fragment;
    if (external && gles) {
        fragment = "#version 300 es\n#extension GL_OES_EGL_image_external_essl3 : require\n"
            "precision highp float;precision highp samplerExternalOES;"
            "uniform samplerExternalOES image;out vec4 color;"
            "void main(){color=texelFetch(image,ivec2(gl_FragCoord.xy),0);}";
    } else if (external) {
        // Desktop advertisement/compiler acceptance is tested independently;
        // the OES specification alone does not promise desktop GL support.
        fragment = "#version 130\n#extension GL_OES_EGL_image_external : require\n"
            "uniform samplerExternalOES image;out vec4 color;"
            "void main(){color=texture2D(image,gl_FragCoord.xy/vec2(" +
            std::to_string(a.o.width) + ".0," + std::to_string(a.o.height) + ".0));}";
    } else {
        fragment = "#version 330 core\nuniform sampler2D image;out vec4 color;"
            "void main(){color=texelFetch(image,ivec2(gl_FragCoord.xy),0);}";
    }
    struct Objects {
        GLuint vs = 0, fs = 0, program = 0, vao = 0, texture = 0, fbo = 0;
        ~Objects() {
            glDeleteFramebuffers(1, &fbo); glDeleteTextures(1, &texture); glDeleteVertexArrays(1, &vao);
            if (program) glDeleteProgram(program);
            if (vs) glDeleteShader(vs);
            if (fs) glDeleteShader(fs);
        }
    } objects;
    auto shader = [](GLenum type, const char *text) {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &text, nullptr); glCompileShader(s);
        GLint good = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &good);
        if (!good) {
            char log[2048]; GLsizei n = 0; glGetShaderInfoLog(s, sizeof(log), &n, log);
            event("gl_oracle_shader", "failed", "\"log\":" + quote(std::string(log, n)));
            glDeleteShader(s); return GLuint(0);
        }
        return s;
    };
    event("gl_sampling_oracle", "starting", mode);
    objects.vs = shader(GL_VERTEX_SHADER, vertex.c_str());
    objects.fs = shader(GL_FRAGMENT_SHADER, fragment.c_str());
    if (!objects.vs || !objects.fs) return false;
    objects.program = glCreateProgram();
    glAttachShader(objects.program, objects.vs); glAttachShader(objects.program, objects.fs);
    glLinkProgram(objects.program);
    GLint linked = 0; glGetProgramiv(objects.program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[2048]; GLsizei length = 0;
        glGetProgramInfoLog(objects.program, sizeof(log), &length, log);
        event("gl_oracle_program", "failed", mode + ",\"log\":" + quote(std::string(log, length)));
        return false;
    }
    glGenTextures(1, &objects.texture); glBindTexture(GL_TEXTURE_2D, objects.texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, a.o.width, a.o.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenFramebuffers(1, &objects.fbo); glBindFramebuffer(GL_FRAMEBUFFER, objects.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, objects.texture, 0);
    GLenum complete = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (complete != GL_FRAMEBUFFER_COMPLETE) {
        event("gl_oracle_fbo", "failed", number("framebuffer_status", complete)); return false;
    }
    glViewport(0, 0, a.o.width, a.o.height);
    glDisable(GL_BLEND); glDisable(GL_DITHER);
    if (!gles) glDisable(GL_FRAMEBUFFER_SRGB);
    glDisable(GL_DEPTH_TEST); glDisable(GL_SCISSOR_TEST);
    // An absent draw must not accidentally pass because the allocator reused
    // an earlier oracle texture containing the expected candidate pixels.
    glClearColor(1.0f, 0.0f, 1.0f, 0.0f); glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(objects.program); glUniform1i(glGetUniformLocation(objects.program, "image"), 0);
    glActiveTexture(GL_TEXTURE0); glBindTexture(target, source);
    glGenVertexArrays(1, &objects.vao); glBindVertexArray(objects.vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    GLenum error = glGetError();
    event("gl_candidate_sampling", error == GL_NO_ERROR ? "submitted" : "failed", mode + "," + number("gl_error", error));
    std::vector<uint8_t> oracle(uint64_t(a.o.width) * a.o.height * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, a.o.width, a.o.height, GL_RGBA, GL_UNSIGNED_BYTE, oracle.data());
    GLenum readError = glGetError();
    event("gl_sample_oracle_readback", readError == GL_NO_ERROR ? "observed" : "failed",
          mode + ",\"candidate_path\":false," + number("gl_error", readError));
    bool ok = checkPixels(a.o, oracle.data(), uint64_t(a.o.width) * 4, true,
                          kvm ? &a.kvmPixels : nullptr, false,
                          external ? (gles ? "gles_external_sampled_pixels" : "desktop_external_sampled_pixels") : "gl_sampled_candidate_pixels");
    glFinish();
    return !error && !readError && ok;
}

static bool glExternalSample(Candidate& a, EGLImageKHR image, bool kvm, bool gles) {
    const std::string mode = std::string("\"api\":") + quote(gles ? "gles3" : "desktop_gl");
    bool base = glExtension("GL_OES_EGL_image_external");
    bool essl3 = glExtension("GL_OES_EGL_image_external_essl3");
    GLenum queryError = glGetError();
    event("gl_external_extensions", "observed", mode + ",\"oes_external\":" + (base ? "true" : "false") +
          ",\"oes_external_essl3\":" + (essl3 ? "true" : "false") + "," + number("gl_error", queryError));
    if (queryError || !base || (gles && !essl3)) {
        event("gl_external_sampling", "unsupported", mode + ",\"reason\":\"required shader/texture extension absent\"");
        return false;
    }
    using TargetImage = void (*)(GLenum, GLeglImageOES);
    auto targetImage = reinterpret_cast<TargetImage>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    if (!targetImage) return false;
    struct Texture { GLuint value = 0; ~Texture() { glFinish(); glDeleteTextures(1, &value); } } texture;
    glGenTextures(1, &texture.value); glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture.value);
    targetImage(GL_TEXTURE_EXTERNAL_OES, image);
    GLenum importError = glGetError();
    event("gl_external_import", importError ? "failed" : "passed", mode + "," + number("gl_error", importError));
    if (importError) return false;
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    GLenum parameterError = glGetError();
    event("gl_external_parameters", parameterError ? "failed" : "passed", mode + "," + number("gl_error", parameterError));
    bool ok = !parameterError && glSampleOracle(a, texture.value, kvm, GL_TEXTURE_EXTERNAL_OES, gles);
    event("gl_external_sampling", ok ? "passed" : "failed", mode +
          ",\"sample_only\":true,\"current_qemu_integration_proven\":false");
    return ok;
}

static bool glesExternalSample(Candidate& a, EGLDisplay display, EGLContext desktop,
                               EGLImageKHR image, bool kvm) {
    struct Context {
        EGLDisplay display;
        EGLContext desktop, es = EGL_NO_CONTEXT;
        bool restored = false;
        bool restore() {
            if (restored) return true;
            if (!eglBindAPI(EGL_OPENGL_API) ||
                !eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, desktop))
                return false;
            if (es != EGL_NO_CONTEXT && !eglDestroyContext(display, es))
                return false;
            es = EGL_NO_CONTEXT;
            restored = true;
            return true;
        }
        ~Context() {
            if (!restore()) {
                event("result", "failed", "\"reason\":\"cannot restore desktop EGL context\"");
                // No GL cleanup is safe with an unknown current context.
                _exit(1);
            }
        }
    } context{display, desktop};
    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        event("gles_external_context", "unsupported", number("egl_error", eglGetError())); return false;
    }
    const EGLint configAttrs[]{EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR, EGL_NONE};
    EGLConfig config; EGLint count = 0;
    if (!eglChooseConfig(display, configAttrs, &config, 1, &count) || !count) {
        event("gles_external_context", "unsupported", "\"reason\":\"GLES3 config absent\""); return false;
    }
    const EGLint contextAttrs[]{EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    context.es = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttrs);
    if (context.es == EGL_NO_CONTEXT || !eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context.es)) {
        event("gles_external_context", "unsupported", number("egl_error", eglGetError())); return false;
    }
    const char *renderer = reinterpret_cast<const char *>(glGetString(GL_RENDERER));
    const char *version = reinterpret_cast<const char *>(glGetString(GL_VERSION));
    event("gles_external_context", "created", "\"renderer\":" + quote(renderer ? renderer : "null") +
          ",\"version\":" + quote(version ? version : "null") + ",\"desktop_gl_compatibility_proven\":false");
    bool ok = glExternalIdentity(a) && glExternalSample(a, image, kvm, true);
    glFinish();
    if (!context.restore()) throw Stop("failed to restore desktop EGL context");
    return ok;
}

static GLResult glImport(Candidate& a, bool kvm) {
    GLResult result;
    if (!a.exported) { event("gl_same_allocation", "unsupported", "\"reason\":\"non-export allocation variant\""); return result; }
    a.exportFd();
    auto queryDevices = reinterpret_cast<PFNEGLQUERYDEVICESEXTPROC>(eglGetProcAddress("eglQueryDevicesEXT"));
    auto queryString = reinterpret_cast<PFNEGLQUERYDEVICESTRINGEXTPROC>(eglGetProcAddress("eglQueryDeviceStringEXT"));
    auto platform = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    auto createImage = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    auto destroyImage = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    using TargetImage = void (*)(GLenum, GLeglImageOES);
    auto targetImage = reinterpret_cast<TargetImage>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    if (!queryDevices || !queryString || !platform || !createImage || !destroyImage || !targetImage) {
        event("gl_same_allocation", "unsupported", "\"reason\":\"EGL device/image entry point missing\""); return result;
    }
    EGLDeviceEXT devices[32]; EGLint count = 0;
    if (!queryDevices(32, devices, &count)) throw Stop("eglQueryDevicesEXT failed");
    EGLDisplay display = EGL_NO_DISPLAY; EGLContext context = EGL_NO_CONTEXT;
    for (EGLint i = 0; i < count; i++) {
        const char *path = queryString(devices[i], EGL_DRM_RENDER_NODE_FILE_EXT);
        struct stat st{};
        if (!path || stat(path, &st) || a.v.renderMajor < 0 ||
            major(st.st_rdev) != a.v.renderMajor || minor(st.st_rdev) != a.v.renderMinor) continue;
        display = platform(EGL_PLATFORM_DEVICE_EXT, devices[i], nullptr);
        if (display == EGL_NO_DISPLAY || !eglInitialize(display, nullptr, nullptr)) { display = EGL_NO_DISPLAY; continue; }
        break;
    }
    if (display == EGL_NO_DISPLAY) {
        event("gl_same_allocation", "unsupported", "\"reason\":\"no EGL device with Vulkan DRM render-node identity\""); return result;
    }
    struct EGLCleanup { EGLDisplay d; EGLContext& c; ~EGLCleanup() {
        eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (c != EGL_NO_CONTEXT) eglDestroyContext(d, c);
        eglTerminate(d);
    }} cleanup{display, context};
    if (!eglBindAPI(EGL_OPENGL_API)) throw Stop("eglBindAPI failed");
    const EGLint configs[]{EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE};
    EGLConfig config; EGLint n = 0;
    if (!eglChooseConfig(display, configs, &config, 1, &n) || !n) throw Stop("no EGL GL config", true);
    context = eglCreateContext(display, config, EGL_NO_CONTEXT, nullptr);
    if (context == EGL_NO_CONTEXT || !eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context))
        throw Stop("EGL surfaceless context failed", true);
    const char *renderer = reinterpret_cast<const char *>(glGetString(GL_RENDERER));
    event("gl_device", "observed", "\"renderer\":" + quote(renderer ? renderer : "null"));
    if (!glExternalIdentity(a)) return result;
    externalOwnership(a, true);
    if (a.layout.offset > INT32_MAX || a.layout.rowPitch > INT32_MAX) throw Stop("EGL layout exceeds attribute range", true);
    auto attempt = [&](bool explicitModifier) {
        GLResult result;
        const std::string mode = std::string("\"modifier_mode\":") + quote(explicitModifier ? "explicit_linear" : "implicit");
        event("egl_import_attempt", "starting", mode);
        std::vector<EGLint> attrs{EGL_WIDTH, EGLint(a.o.width), EGL_HEIGHT, EGLint(a.o.height),
            EGL_LINUX_DRM_FOURCC_EXT, EGLint(a.o.bgra ? 0x34325241 : 0x34324241),
            EGL_DMA_BUF_PLANE0_FD_EXT, a.fd, EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGLint(a.layout.offset),
            EGL_DMA_BUF_PLANE0_PITCH_EXT, EGLint(a.layout.rowPitch)};
        if (explicitModifier) attrs.insert(attrs.end(), {
            EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, 0, EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, 0});
        attrs.push_back(EGL_NONE);
        EGLImageKHR image = createImage(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attrs.data());
        EGLint imageError = eglGetError();
        event("egl_candidate_import", image != EGL_NO_IMAGE_KHR ? "passed" : "failed", mode + "," + number("egl_error", imageError));
        if (image == EGL_NO_IMAGE_KHR) {
            event("gl_same_allocation", "unsupported", mode + "," + number("egl_error", imageError)); return result;
        }
        GLuint texture = 0, framebuffer = 0;
        glGenTextures(1, &texture); glBindTexture(GL_TEXTURE_2D, texture);
        GLenum beforeImport = glGetError();
        targetImage(GL_TEXTURE_2D, image);
        GLenum importError = glGetError();
        GLint importedWidth = 0, importedHeight = 0, internalFormat = 0;
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &importedWidth);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &importedHeight);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &internalFormat);
        GLenum queryError = glGetError();
        event("gl_candidate_texture", importError == GL_NO_ERROR ? "imported" : "failed",
              mode + "," + number("prior_gl_error", beforeImport) + "," + number("import_gl_error", importError) +
              "," + number("query_gl_error", queryError) + "," + number("width", importedWidth) +
              "," + number("height", importedHeight) + "," + number("internal_format", internalFormat));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        GLenum parameterError = glGetError();
        glGenFramebuffers(1, &framebuffer); glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        GLenum framebufferError = glGetError();
        event("gl_candidate_fbo", status == GL_FRAMEBUFFER_COMPLETE ? "complete" : "incomplete",
              mode + "," + number("framebuffer_status", status) + "," + number("gl_error", framebufferError) +
              "," + number("parameter_gl_error", parameterError));
        bool textureValid = !beforeImport && !importError && !queryError && !parameterError &&
                            importedWidth == int(a.o.width) && importedHeight == int(a.o.height);
        bool ok = textureValid && !framebufferError && status == GL_FRAMEBUFFER_COMPLETE;
        if (ok) {
            std::vector<uint8_t> oracle(uint64_t(a.o.width) * a.o.height * 4);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, a.o.width, a.o.height, GL_RGBA, GL_UNSIGNED_BYTE, oracle.data());
            GLenum error = glGetError();
            event("gl_oracle_readback", error == GL_NO_ERROR ? "observed" : "failed", "\"candidate_path\":false," + number("gl_error", error));
            ok = error == GL_NO_ERROR && checkPixels(a.o, oracle.data(), uint64_t(a.o.width) * 4,
                                                    true, kvm ? &a.kvmPixels : nullptr, false, "gl_same_allocation_pixels");
        }
        result.framebuffer = ok;
        if (textureValid) result.sampling = glSampleOracle(a, texture, kvm);
        event("gl_sampling_path", result.sampling ? "passed" : "failed",
              "\"current_qemu_fbo_path_proven\":false,\"requires_separate_sampling_implementation_if_fbo_incomplete\":true");
        if (explicitModifier) {
            // Keep desktop support and a separate ES context as distinct
            // capabilities; neither changes the direct 2D/FBO gate above.
            result.externalDesktop = glExternalSample(a, image, kvm, false);
            result.externalGles = glesExternalSample(a, display, context, image, kvm);
        }
        glFinish(); glDeleteFramebuffers(1, &framebuffer); glDeleteTextures(1, &texture); destroyImage(display, image);
        event("gl_same_allocation", ok ? "passed" : "failed", mode + "," + number("framebuffer_status", status));
        return result;
    };
    GLResult implicit = attempt(false);
    event("egl_implicit_result", "diagnostic", std::string("\"framebuffer\":") +
          (implicit.framebuffer ? "true" : "false") + ",\"sampling\":" +
          (implicit.sampling ? "true" : "false") + ",\"explicit_linear_proven\":false");
    const char *extensionString = eglQueryString(display, EGL_EXTENSIONS);
    std::string extensions = std::string(" ") + (extensionString ? extensionString : "") + " ";
    if (extensions.find(" EGL_EXT_image_dma_buf_import_modifiers ") == std::string::npos) {
        event("egl_explicit_linear", "unsupported", "\"reason\":\"EGL modifier extension absent\"");
        return GLResult{};
    }
    auto queryModifiers = reinterpret_cast<PFNEGLQUERYDMABUFMODIFIERSEXTPROC>(eglGetProcAddress("eglQueryDmaBufModifiersEXT"));
    if (queryModifiers) {
        EGLint count = 0;
        EGLint fourcc = a.o.bgra ? 0x34325241 : 0x34324241;
        if (queryModifiers(display, fourcc, 0, nullptr, nullptr, &count) && count > 0 && count < 65536) {
            std::vector<EGLuint64KHR> modifiers(count);
            std::vector<EGLBoolean> externalOnly(count);
            EGLint written = 0;
            if (queryModifiers(display, fourcc, count, modifiers.data(), externalOnly.data(), &written)) {
                for (EGLint i = 0; i < std::min(count, written); i++) if (modifiers[i] == 0)
                    event("egl_linear_modifier", "advertised", std::string("\"external_only\":") +
                          (externalOnly[i] ? "true" : "false"));
            }
        }
        event("egl_modifier_query", "observed", number("count", std::max(0, count)) + "," + number("egl_error", eglGetError()));
    }
    return attempt(true);
}

int main(int argc, char **argv) {
    try {
        Options o;
        for (int i = 1; i + 1 < argc; i += 2) {
            std::string key = argv[i], value = argv[i + 1];
            if (key == "--width") o.width = std::stoul(value);
            else if (key == "--height") o.height = std::stoul(value);
            else if (key == "--format" && (value == "bgra" || value == "rgba")) o.bgra = value == "bgra";
            else if (key == "--tiling") o.tiling = value;
            else if (key == "--device") o.device = value;
            else if (key == "--cpu-map") o.cpuMap = value;
            else throw Stop("invalid argument: " + key);
        }
        if (argc % 2 != 1 || o.width < 64 || o.height < 64 || o.width > 4096 || o.height > 4096 ||
            (o.tiling != "linear" && o.tiling != "linear-dmabuf" && o.tiling != "drm-linear") ||
            (o.cpuMap != "vulkan" && o.cpuMap != "dmabuf"))
            throw Stop("required: --width N --height N --format bgra|rgba --tiling linear|linear-dmabuf|drm-linear [--device NVIDIA] [--cpu-map vulkan|dmabuf]");
        event("case", "starting", number("width", o.width) + "," + number("height", o.height) +
              ",\"format\":" + quote(o.bgra ? "BGRA" : "RGBA") + ",\"tiling\":" + quote(o.tiling) +
              ",\"cpu_map\":" + quote(o.cpuMap));
        Vulkan v(o); Candidate a(v, o);
        gpuPattern(a);
        if (o.cpuMap == "dmabuf") externalOwnership(a, true);
        DmaBufCPUAccess cpuAccess(o.cpuMap == "dmabuf" ? a.fd : -1);
        if (!checkPixels(o, a.pixels(), a.layout.rowPitch, false, nullptr, o.bgra, "direct_cpu_reads_gpu_pattern"))
            throw Stop("direct mapped bytes disagree with GPU writes");
        for (auto p : {std::pair<uint32_t, uint32_t>{1, 2}, {o.width / 2, o.height / 2}, {o.width - 2, o.height - 3}}) {
            uint32_t value = packed(cpuColor, o.bgra);
            memcpy(a.pixels() + p.second * a.layout.rowPitch + p.first * 4, &value, 4);
        }
        std::atomic_thread_fence(std::memory_order_seq_cst);
        bool kvm = kvmCheck(a);
        cpuAccess.finish();
        if (o.cpuMap == "dmabuf") externalOwnership(a, false);
        bool gpu = gpuOracle(a, kvm);
        GLResult gl = glImport(a, kvm);
        event("external_sampling_storage", kvm && gpu && gl.externalDesktop ? "desktop_sample_only_pass" :
              kvm && gpu && gl.externalGles ? "gles_sample_only_pass" : "unproven",
              "\"cpu_map\":" + quote(o.cpuMap) + ",\"desktop_external_sampling\":" +
              (gl.externalDesktop ? "true" : "false") + ",\"gles_external_sampling\":" +
              (gl.externalGles ? "true" : "false") +
              ",\"direct_2d_framebuffer_proven\":false,\"current_qemu_integration_proven\":false");
        event("result", kvm && gpu && gl.framebuffer && gl.sampling ? "complete_capability_pass" : "incomplete_capability",
              "\"cpu_map\":" + quote(o.cpuMap) + ",\"kvm_exact_mapping\":" + (kvm ? "true" : "false") +
              ",\"gpu_observes_cpu_writes\":" + (gpu ? "true" : "false") +
              ",\"gl_same_allocation_framebuffer\":" + (gl.framebuffer ? "true" : "false") +
              ",\"gl_same_allocation_sampling\":" + (gl.sampling ? "true" : "false") +
              ",\"desktop_external_sampling\":" + (gl.externalDesktop ? "true" : "false") +
              ",\"gles_external_sampling\":" + (gl.externalGles ? "true" : "false") +
              ",\"current_qemu_integration_proven\":false" +
              ",\"application_performance_proven\":false");
        return kvm && gpu && gl.framebuffer && gl.sampling ? 0 : 2;
    } catch (const Stop& e) {
        event("result", e.unsupported ? "unsupported" : "failed", "\"reason\":" + quote(e.what()));
        return e.unsupported ? 2 : 1;
    } catch (const std::exception& e) {
        event("result", "failed", "\"reason\":" + quote(e.what())); return 1;
    }
}
