#define BCDEC_BC4BC5_PRECISE
#define BCDEC_IMPLEMENTATION
#include "bcdec/bcdec.h"

#include "dxmt_bc_emulation.hpp"

#include <algorithm>
#include <cstring>

namespace dxmt {

uint32_t
BCEmulatedTexelSize(WMTPixelFormat bc_format) {
  switch (bc_format) {
  case WMTPixelFormatBC4_RUnorm:
  case WMTPixelFormatBC4_RSnorm:
    return 1;
  case WMTPixelFormatBC5_RGUnorm:
  case WMTPixelFormatBC5_RGSnorm:
    return 2;
  case WMTPixelFormatBC6H_RGBUfloat:
  case WMTPixelFormatBC6H_RGBFloat:
    return 8; /* RGBA16Float */
  default:
    return 4; /* BC1/2/3/7 -> RGBA8 */
  }
}

uint32_t
BCBlockSize(WMTPixelFormat bc_format) {
  switch (bc_format) {
  case WMTPixelFormatBC1_RGBA:
  case WMTPixelFormatBC1_RGBA_sRGB:
  case WMTPixelFormatBC4_RUnorm:
  case WMTPixelFormatBC4_RSnorm:
    return 8;
  default:
    return 16;
  }
}

/* Decode one full 4x4 block, texel rows `pitch` bytes apart. */
static void
DecodeBlock(WMTPixelFormat bc_format, const uint8_t *block, uint8_t *texels, size_t pitch) {
  switch (bc_format) {
  case WMTPixelFormatBC1_RGBA:
  case WMTPixelFormatBC1_RGBA_sRGB:
    bcdec_bc1(block, texels, pitch);
    break;
  case WMTPixelFormatBC2_RGBA:
  case WMTPixelFormatBC2_RGBA_sRGB:
    bcdec_bc2(block, texels, pitch);
    break;
  case WMTPixelFormatBC3_RGBA:
  case WMTPixelFormatBC3_RGBA_sRGB:
    bcdec_bc3(block, texels, pitch);
    break;
  case WMTPixelFormatBC4_RUnorm:
    bcdec_bc4(block, texels, pitch, 0);
    break;
  case WMTPixelFormatBC4_RSnorm:
    bcdec_bc4(block, texels, pitch, 1);
    break;
  case WMTPixelFormatBC5_RGUnorm:
    bcdec_bc5(block, texels, pitch, 0);
    break;
  case WMTPixelFormatBC5_RGSnorm:
    bcdec_bc5(block, texels, pitch, 1);
    break;
  case WMTPixelFormatBC6H_RGBUfloat:
  case WMTPixelFormatBC6H_RGBFloat: {
    /* bcdec emits half3; expand to half4 with alpha = 1.0h */
    uint16_t rgb[4 * 4 * 3];
    bcdec_bc6h_half(block, rgb, 4 * 3, bc_format == WMTPixelFormatBC6H_RGBFloat);
    for (unsigned r = 0; r < 4; r++) {
      uint16_t *out = reinterpret_cast<uint16_t *>(texels + r * pitch);
      for (unsigned c = 0; c < 4; c++) {
        out[c * 4 + 0] = rgb[(r * 4 + c) * 3 + 0];
        out[c * 4 + 1] = rgb[(r * 4 + c) * 3 + 1];
        out[c * 4 + 2] = rgb[(r * 4 + c) * 3 + 2];
        out[c * 4 + 3] = 0x3C00; /* 1.0h */
      }
    }
    break;
  }
  case WMTPixelFormatBC7_RGBAUnorm:
  case WMTPixelFormatBC7_RGBAUnorm_sRGB:
    bcdec_bc7(block, texels, pitch);
    break;
  default:
    for (unsigned r = 0; r < 4; r++)
      std::memset(texels + r * pitch, 0, 4 * BCEmulatedTexelSize(bc_format));
    break;
  }
}

void
DecompressBC(
    WMTPixelFormat bc_format, const void *src, size_t src_bytes_per_row, size_t src_bytes_per_row_valid,
    void *dst, size_t dst_bytes_per_row, uint32_t width, uint32_t height
) {
  uint32_t texel_size = BCEmulatedTexelSize(bc_format);
  uint32_t block_size = BCBlockSize(bc_format);
  uint8_t texels[4 * 4 * 8]; /* one block at the largest texel size */

  for (uint32_t by = 0; by * 4 < height; by++) {
    const uint8_t *src_row = reinterpret_cast<const uint8_t *>(src) + by * src_bytes_per_row;
    uint8_t *dst_row = reinterpret_cast<uint8_t *>(dst) + (by * 4) * dst_bytes_per_row;
    uint32_t rows = std::min(4u, height - by * 4);
    for (uint32_t bx = 0; bx * 4 < width; bx++) {
      uint32_t cols = std::min(4u, width - bx * 4);
      uint8_t *dst_block = dst_row + bx * 4 * texel_size;
      /* never read past the caller-declared valid bytes of a block row */
      if ((bx + 1) * (size_t)block_size > src_bytes_per_row_valid) {
        for (uint32_t r = 0; r < rows; r++)
          std::memset(dst_block + r * dst_bytes_per_row, 0, cols * texel_size);
        continue;
      }
      if (rows == 4 && cols == 4) {
        /* interior block: decode straight into the destination */
        DecodeBlock(bc_format, src_row + bx * block_size, dst_block, dst_bytes_per_row);
        continue;
      }
      DecodeBlock(bc_format, src_row + bx * block_size, texels, 4 * texel_size);
      for (uint32_t r = 0; r < rows; r++)
        std::memcpy(dst_block + r * dst_bytes_per_row, texels + r * 4 * texel_size, cols * texel_size);
    }
  }
}

} // namespace dxmt
