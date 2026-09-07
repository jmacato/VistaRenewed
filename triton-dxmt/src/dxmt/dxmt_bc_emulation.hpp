#pragma once

#include "winemetal.h"
#include <cstddef>
#include <cstdint>

namespace dxmt {

/*
 * CPU decompression of BC blocks, for devices where Metal has no BC support
 * (iOS before 16.4, A-series GPUs).  There MTLQueryDXGIFormat maps each BC
 * format to an uncompressed one (MTL_DXGI_FORMAT_EMULATED_BC) and every
 * CPU-visible upload path decompresses through these helpers:
 *
 *   BC1/BC2/BC3/BC7 (+sRGB) -> RGBA8Unorm(_sRGB)     4 B/texel
 *   BC4              -> R8Unorm / R8Snorm            1 B/texel
 *   BC5              -> RG8Unorm / RG8Snorm          2 B/texel
 *   BC6H             -> RGBA16Float                  8 B/texel
 */

/* Texel size in bytes of the format `bc_format` is emulated as. */
uint32_t BCEmulatedTexelSize(WMTPixelFormat bc_format);

/* Size in bytes of one compressed block of `bc_format` (8 or 16). */
uint32_t BCBlockSize(WMTPixelFormat bc_format);

/*
 * Decompress `width` x `height` texels.  `src` points at the first BC block
 * (the origin must be block-aligned), with rows of blocks `src_bytes_per_row`
 * apart.  Only the first `src_bytes_per_row_valid` bytes of each block row
 * are read -- applications do pass a SysMemPitch/RowPitch smaller than a
 * tight block row, and reading a full row regardless would run off the end
 * of their allocation; blocks beyond the valid bytes decode as zero.  Pass
 * `src_bytes_per_row` when the pitch is driver-computed.  Texel rows are
 * written to `dst`, `dst_bytes_per_row` apart, texels tightly packed at
 * BCEmulatedTexelSize().
 */
void DecompressBC(
    WMTPixelFormat bc_format, const void *src, size_t src_bytes_per_row, size_t src_bytes_per_row_valid,
    void *dst, size_t dst_bytes_per_row, uint32_t width, uint32_t height
);

} // namespace dxmt
