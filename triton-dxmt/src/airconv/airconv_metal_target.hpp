#pragma once

#include "airconv_public.h"
#include <cstdint>
#include <string>

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

namespace dxmt {

/*
 * The AIR triple a generated shader carries has to name the platform it will be
 * loaded on -- a module built for one Apple OS will not load on another -- so it
 * follows the platform this library was built for, not the host that built it.
 * A Wine cross build has no TARGET_OS_* and correctly falls through to macOS.
 */
struct MetalTarget {
  uint32_t air_minor;  /* air.version 2.<air_minor> */
  uint32_t msl_minor;  /* air.language_version Metal 3.<msl_minor> */
  uint32_t os_major;
  uint32_t os_minor;
  std::string triple;
};

inline MetalTarget
GetMetalTarget(SM50_SHADER_METAL_VERSION metal_version) {
  MetalTarget target;

#if defined(TARGET_OS_VISION) && TARGET_OS_VISION
  const char *os = "xros";
  /* visionOS is Metal 3.1 from 1.0 on; there is no 3.0 to ask for, and the
   * Metal compiler clamps up to AIR 2.6 there whatever -std says. */
  switch (metal_version) {
  case SM50_SHADER_METAL_320: target = {7, 2, 2, 0, {}}; break;
  default:                    target = {6, 1, 1, 0, {}}; break;
  }
#else
#if defined(TARGET_OS_TV) && TARGET_OS_TV
  const char *os = "tvos";
  const uint32_t v300 = 16, v310 = 17, v320 = 18;
#elif defined(TARGET_OS_IOS) && TARGET_OS_IOS
  const char *os = "ios";
  const uint32_t v300 = 16, v310 = 17, v320 = 18;
#else
  const char *os = "macosx";
  const uint32_t v300 = 13, v310 = 14, v320 = 15;
#endif
  switch (metal_version) {
  case SM50_SHADER_METAL_320: target = {7, 2, v320, 0, {}}; break;
  case SM50_SHADER_METAL_310: target = {6, 1, v310, 0, {}}; break;
  default:                    target = {5, 0, v300, 0, {}}; break;
  }
#endif

  target.triple = std::string("air64-apple-") + os + std::to_string(target.os_major) + "." +
                  std::to_string(target.os_minor) + ".0";
#if defined(TARGET_OS_SIMULATOR) && TARGET_OS_SIMULATOR
  /* Simulator AIR is a separate target from the device's. */
  target.triple += "-simulator";
#endif
  return target;
}

} // namespace dxmt
