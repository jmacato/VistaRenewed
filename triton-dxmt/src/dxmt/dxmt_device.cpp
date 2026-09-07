#include "dxmt_device.hpp"
#include "config/config.hpp"
#include "dxmt_command_queue.hpp"
#include "Metal.hpp"
#include <TargetConditionals.h>

namespace dxmt {

class DeviceImpl : public Device {
public:
  virtual WMT::Device
  device() override {
    return device_;
  };
  virtual CommandQueue &
  queue() override {
    return cmd_queue_;
  };

  virtual WMTMetalVersion metalVersion() override {
    return metal_version_;
  };

  virtual uint64_t maxObjectThreadgroups() final {
    return max_object_threadgroups_;
  };

  DeviceImpl(const DEVICE_DESC &desc) : device_(desc.device), cmd_queue_(device_) {
    uint64_t os_major_version = 0, os_minor_version = 0;
    int version_conf = Config::getInstance().getOption<int>("dxmt.shaderMetalVersion", 0);
    switch (version_conf) {
    case WMTMetal300:
    case WMTMetal310:
    case WMTMetal320:
      metal_version_ = (WMTMetalVersion)version_conf;
      break;
    default:
      metal_version_ = WMTMetalVersionMax;
      break;
    }
    WMTGetOSVersion(&os_major_version, &os_minor_version, nullptr);
    /* Which OS release brought which Metal version differs per platform, and
     * WMTGetOSVersion reports whichever OS we are actually running on. */
#if TARGET_OS_OSX
    const uint64_t v310 = 14, v320 = 15;
#elif TARGET_OS_VISION
    /* visionOS is Metal 3.1 from 1.0 on -- it has no 3.0. */
    const uint64_t v310 = 0, v320 = 2;
#else /* iOS, tvOS */
    const uint64_t v310 = 17, v320 = 18;
#endif
    if (os_major_version >= v320) {
      metal_version_ = std::min(WMTMetal320, metal_version_);
    } else if (os_major_version >= v310) {
      metal_version_ = std::min(WMTMetal310, metal_version_);
    } else {
      metal_version_ = std::min(WMTMetal300, metal_version_);
    }
    if (!device_.supportsFamily(WMTGPUFamilyApple7)) {
      WARN("Experimental non-Apple GPU support");
      // Metal 3.2 features we need are basically not available. A cap, not a
      // floor -- an older OS has already selected something lower.
      metal_version_ = std::min(WMTMetal310, metal_version_);
      max_object_threadgroups_ = 1024;
      // macOS 26 bug: setShouldMaximizeConcurrentCompilation crashes on AMDGPU
      if (!(os_major_version >= 16 && os_major_version <= 26 && os_minor_version < 2))
        device_.setShouldMaximizeConcurrentCompilation(true);
    } else {
      max_object_threadgroups_ = -1ull;
      device_.setShouldMaximizeConcurrentCompilation(true);
    }
  }

private:
  WMT::Reference<WMT::Device> device_;
  CommandQueue cmd_queue_;
  WMTMetalVersion metal_version_;
  uint64_t max_object_threadgroups_;
};

std::unique_ptr<Device>
CreateDXMTDevice(const DEVICE_DESC &desc) {
  return std::make_unique<DeviceImpl>(desc);
};

} // namespace dxmt
