/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Shared/exportable-texture helpers used by both the guest DXGI
 * swapchain (Wine) and the Triton D3D11 UMD (Windows).  A presentable
 * surface on either path is the same thing: a host D3D11 texture with
 * exportable linear storage, staged as this context's pending blob so
 * the guest can bind it to a virtio-gpu resource.
 *
 * The interface is deliberately stdint-only (same discipline as
 * tritonSharedBridge.h) so Triton DDI TUs can include it without
 * pulling in the npt headers.
 */

#ifndef NPT_SHARED_TEXTURE_H
#define NPT_SHARED_TEXTURE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* dxvk-native vendor MiscFlags bit (DXVK_D3D11_RESOURCE_MISC_LINEAR_EXPORT
 * in dxvk's d3d11_texture.h): forces DRM_FORMAT_MOD_LINEAR on the
 * exported image so modifier-less consumers (QEMU scanout, DRI3 pixmaps)
 * can import it. */
#define NPT_D3D11_MISC_LINEAR_EXPORT 0x40000000u

#define NPT_SHARED_TEXTURE_MAX_PLANES 4

/* Wire-neutral description of an exported texture.  The export half is
 * filled by npt_shared_texture_export_blob; the D3D11 half is owned by
 * the caller (used by the OPEN_RES consumer side). */
struct npt_shared_texture_desc {
   /* Filled by npt_shared_texture_export_blob. */
   uint64_t blob_id;
   uint32_t create_ctx_id;
   uint32_t plane_count;
   uint32_t texture_layout;
   uint64_t modifier;
   uint64_t allocation_size;
   struct {
      uint64_t offset;
      uint64_t pitch;
   } planes[NPT_SHARED_TEXTURE_MAX_PLANES];
   /* Owned by the caller (D3D11 description of the host texture). */
   uint32_t width;
   uint32_t height;
   uint32_t mip_levels;
   uint32_t array_size;
   uint32_t format;           /* DXGI_FORMAT */
   uint32_t sample_count;
   uint32_t usage;            /* D3D11_USAGE */
   uint32_t bind_flags;
   uint32_t cpu_access_flags;
   uint32_t misc_flags;
};

/* Export the host texture behind \p texture_wrapper (an
 * ID3D11Texture2D* Neptune COM wrapper) as a dmabuf and stage it as
 * this context's pending blob under blob_id == the texture's object
 * id.  Fills the export half of \p desc. */
bool npt_shared_texture_export_blob(void *texture_wrapper,
                                    struct npt_shared_texture_desc *desc);

/* Shared textures reject *_SRGB and TYPELESS formats; exportable
 * surfaces for such backbuffers must be created with the UNORM base
 * (gamma is applied through SRGB views, which dxvk permits on UNORM
 * textures via its mutable view-format list).  Identity for formats
 * with no fixup.  Takes/returns DXGI_FORMAT values. */
uint32_t npt_shared_texture_host_format(uint32_t dxgi_format);

/* DXGI B8G8R8A8/R8G8B8A8-family formats -> enum virgl_formats (the
 * wire namespace SET_SCANOUT_BLOB and the WSI backend consume).  Returns
 * zero for a format that this scanout path cannot represent. */
uint32_t npt_shared_texture_virgl_format(uint32_t dxgi_format);

/* Create a width x height, mips=1, RENDER_TARGET|SHADER_RESOURCE,
 * MISC_SHARED|LINEAR_EXPORT host texture through the device wrapper's
 * vtbl (\p device_wrapper is an ID3D11Device* Neptune COM wrapper).
 * \p dxgi_format should already be fixed up via
 * npt_shared_texture_host_format.  Returns the ID3D11Texture2D*
 * wrapper (caller owns one public reference) or NULL. */
void *npt_shared_texture_create_exportable(void *device_wrapper,
                                           uint32_t width, uint32_t height,
                                           uint32_t dxgi_format);

#ifdef __cplusplus
}
#endif

#endif /* NPT_SHARED_TEXTURE_H */
