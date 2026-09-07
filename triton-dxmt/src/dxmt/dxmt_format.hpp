#pragma once

#include "Metal.hpp"
#include <map>

namespace dxmt {
enum class FormatCapability : int {
  None = 0,
  Atomic = 0x1,
  Filter = 0x2,
  Write = 0x4,
  Color = 0x8,
  Blend = 0x10,
  MSAA = 0x20,
  Sparse = 0x40,
  Resolve = 0x80,
  DepthStencil = 0x100,
  TextureBufferRead = 0x200,
  TextureBufferWrite = 0x400,
  TextureBufferReadWrite = 0x800
};

class FormatCapabilityInspector {
public:
  std::map<WMTPixelFormat, FormatCapability> textureCapabilities{};
  void Inspect(WMT::Device device);
};

WMTPixelFormat Forget_sRGB(WMTPixelFormat format);
WMTPixelFormat Recall_sRGB(WMTPixelFormat format);

inline bool
Is_sRGBVariant(WMTPixelFormat format) {
  return Forget_sRGB(format) != format;
}

bool IsBlockCompressionFormat(WMTPixelFormat format);

/* Metal statically type-checks shader outputs against attachment
 * formats, so pipelines that mix float outputs with integer
 * attachments (undefined but legal in D3D11) need the output register
 * redeclared with the attachment's component type. */
inline bool
IsUintColorFormat(WMTPixelFormat format) {
  switch (format) {
  case WMTPixelFormatR8Uint:
  case WMTPixelFormatR16Uint:
  case WMTPixelFormatRG8Uint:
  case WMTPixelFormatR32Uint:
  case WMTPixelFormatRG16Uint:
  case WMTPixelFormatRGBA8Uint:
  case WMTPixelFormatRGB10A2Uint:
  case WMTPixelFormatRG32Uint:
  case WMTPixelFormatRGBA16Uint:
  case WMTPixelFormatRGBA32Uint:
    return true;
  default:
    return false;
  }
}

inline bool
IsSintColorFormat(WMTPixelFormat format) {
  switch (format) {
  case WMTPixelFormatR8Sint:
  case WMTPixelFormatR16Sint:
  case WMTPixelFormatRG8Sint:
  case WMTPixelFormatR32Sint:
  case WMTPixelFormatRG16Sint:
  case WMTPixelFormatRGBA8Sint:
  case WMTPixelFormatRG32Sint:
  case WMTPixelFormatRGBA16Sint:
  case WMTPixelFormatRGBA32Sint:
    return true;
  default:
    return false;
  }
}

inline bool
IsIntegerColorFormat(WMTPixelFormat format) {
  return IsUintColorFormat(format) || IsSintColorFormat(format);
}

uint32_t DepthStencilPlanarFlags(WMTPixelFormat format);

enum MTL_DXGI_FORMAT_FLAG {
  MTL_DXGI_FORMAT_TYPELESS = 1,
  MTL_DXGI_FORMAT_BC = 2,
  MTL_DXGI_FORMAT_BACKBUFFER = 4,
  MTL_DXGI_FORMAT_DEPTH_PLANER = 16,
  MTL_DXGI_FORMAT_STENCIL_PLANER = 32,
  MTL_DXGI_FORMAT_EMULATED_D24 = 256,
  MTL_DXGI_FORMAT_EMULATED_LINEAR_DEPTH_STENCIL = 512,
  /* device has no BC support: PixelFormat is the uncompressed stand-in,
   * EmulatedBC the original BC format; uploads decompress on the CPU */
  MTL_DXGI_FORMAT_EMULATED_BC = 1024,
};

struct MTL_DXGI_FORMAT_DESC {
  WMTPixelFormat PixelFormat;
  WMTAttributeFormat AttributeFormat;
  union {
    uint32_t BytesPerTexel;
    uint32_t BlockSize;
  };
  uint32_t Flag;
  /* the BC format PixelFormat stands in for, when MTL_DXGI_FORMAT_EMULATED_BC */
  WMTPixelFormat EmulatedBC;
};

int32_t MTLQueryDXGIFormat(WMT::Device device, uint32_t format, MTL_DXGI_FORMAT_DESC &description);

uint32_t MTLGetTexelSize(WMTPixelFormat format);

WMTPixelFormat MTLGetUnsignedIntegerFormat(WMTPixelFormat format);

bool IsUnorm8RenderTargetFormat(WMTPixelFormat format);

bool IsIntegerFormat(WMTPixelFormat format);

void SanitizeRTVClearColor(WMTPixelFormat format, WMTClearColor &color);

} // namespace dxmt