#include "d3d11_texture.hpp"
#include "d3d11_resource.hpp"
#include "dxmt_bc_emulation.hpp"
#include "dxmt_format.hpp"
#include <memory>

namespace dxmt {

/* Under BC emulation the Metal texture is the uncompressed stand-in while
 * pSysMem holds BC blocks (SysMemPitch = bytes per block row): pushing the
 * blocks through replaceRegion would make Metal read them at the texel-row
 * pitch of the stand-in format -- an out-of-bounds read of the application's
 * buffer, or a bytesPerRow validation abort.  Decompress on the CPU first,
 * like every other upload path does.  Reads are clamped to the app-provided
 * pitch, matching ResourceInitializer::initWithCompressedData. */
static void
ReplaceRegionCompressed(
    WMT::Texture target, const MTL_DXGI_FORMAT_DESC &format, uint32_t width, uint32_t height, uint32_t depth,
    uint32_t level, uint32_t slice, const void *data, uint32_t row_pitch, uint32_t slice_pitch
) {
  uint32_t texel_size = BCEmulatedTexelSize(format.EmulatedBC);
  size_t bytes_per_row = (size_t)texel_size * width;
  size_t bytes_per_image = bytes_per_row * height;
  size_t src_bytes_per_row = ((width + 3) / 4) * (size_t)BCBlockSize(format.EmulatedBC);
  size_t src_bytes_per_row_valid = std::min<size_t>(row_pitch, src_bytes_per_row);

  auto scratch = std::unique_ptr<uint8_t[]>(new uint8_t[bytes_per_image * depth]);
  for (uint32_t z = 0; z < depth; z++) {
    DecompressBC(
        format.EmulatedBC, static_cast<const uint8_t *>(data) + (size_t)z * slice_pitch, row_pitch,
        src_bytes_per_row_valid, scratch.get() + z * bytes_per_image, bytes_per_row, width, height
    );
  }
  target.replaceRegion(
      {0, 0, 0}, {width, height, depth}, level, slice, scratch.get(), bytes_per_row,
      depth > 1 ? bytes_per_image : 0
  );
}

template <>
void InitializeTextureData(MTLD3D11Device *pDevice, WMT::Texture target,
                           const D3D11_TEXTURE3D_DESC1 &Desc,
                           const D3D11_SUBRESOURCE_DATA *subresources) {
  if (subresources == nullptr)
    return;
  MTL_DXGI_FORMAT_DESC format;
  MTLQueryDXGIFormat(pDevice->GetMTLDevice(), Desc.Format, format);
  uint32_t width = Desc.Width;
  uint32_t height = Desc.Height;
  uint32_t depth = Desc.Depth;
  for (uint32_t level = 0; level < Desc.MipLevels; level++) {
    if (format.Flag & MTL_DXGI_FORMAT_EMULATED_BC) {
      ReplaceRegionCompressed(
          target, format, width, height, depth, level, 0, subresources[level].pSysMem,
          subresources[level].SysMemPitch, subresources[level].SysMemSlicePitch
      );
    } else {
      target.replaceRegion({0, 0, 0}, {width, height, depth}, level, 0,
                           subresources[level].pSysMem,
                           subresources[level].SysMemPitch,
                           subresources[level].SysMemSlicePitch);
    }
    width = std::max(1u, width >> 1);
    height = std::max(1u, height >> 1);
    depth = std::max(1u, depth >> 1);
  }
};

template <>
void InitializeTextureData(MTLD3D11Device *pDevice, WMT::Texture target,
                           const D3D11_TEXTURE2D_DESC1 &Desc,
                           const D3D11_SUBRESOURCE_DATA *subresources) {
  if (subresources == nullptr)
    return;
  MTL_DXGI_FORMAT_DESC format;
  MTLQueryDXGIFormat(pDevice->GetMTLDevice(), Desc.Format, format);
  uint32_t width = Desc.Width;
  uint32_t height = Desc.Height;
  for (uint32_t level = 0; level < Desc.MipLevels; level++) {
    uint32_t bytes_per_row = 0, _, __;
    GetLinearTextureLayout(pDevice, Desc, level, bytes_per_row, _, __, false);
    for (uint32_t slice = 0; slice < Desc.ArraySize; slice++) {
      auto idx = D3D11CalcSubresource(level, slice, Desc.MipLevels);
      if (subresources[idx].SysMemPitch < bytes_per_row) {
        WARN("RowPitch provided ", subresources[idx].SysMemPitch,
             ", expect at least ", bytes_per_row);
      }
      if (format.Flag & MTL_DXGI_FORMAT_EMULATED_BC) {
        ReplaceRegionCompressed(
            target, format, width, height, 1, level, slice, subresources[idx].pSysMem,
            subresources[idx].SysMemPitch, 0
        );
        continue;
      }
      target.replaceRegion(
          {0, 0, 0}, {width, height, 1}, level, slice,
          subresources[idx].pSysMem,
          std::max(subresources[idx].SysMemPitch, bytes_per_row), 0);
    }
    width = std::max(1u, width >> 1);
    height = std::max(1u, height >> 1);
  }
};

template <>
void InitializeTextureData(MTLD3D11Device *pDevice, WMT::Texture target,
                           const D3D11_TEXTURE1D_DESC &Desc,
                           const D3D11_SUBRESOURCE_DATA *subresources) {
  /* BC formats are not legal on 1D textures, so no emulated-BC handling. */
  if (subresources == nullptr)
    return;
  uint32_t width = Desc.Width;
  for (uint32_t level = 0; level < Desc.MipLevels; level++) {
    for (uint32_t slice = 0; slice < Desc.ArraySize; slice++) {
      auto idx = D3D11CalcSubresource(level, slice, Desc.MipLevels);
      uint32_t bytes_per_row = 0, _, __;
      GetLinearTextureLayout(pDevice, Desc, level, bytes_per_row, _, __, false);
      target.replaceRegion(
          {0, 0, 0}, {width, 1, 1}, level, slice, subresources[idx].pSysMem,
          std::max(subresources[idx].SysMemPitch, bytes_per_row), 0);
    }
    width = std::max(1u, width >> 1);
  }
};

} // namespace dxmt
