/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Shared/exportable-texture helpers (see npt_shared_texture.h).
 * Speaks the Neptune transport (ring, SHARED subgroup dispatch, COM
 * wrapper cache) on behalf of the guest swapchain and Triton.
 */

#include "npt_shared_texture.h"

#include <inttypes.h>
#include <string.h>

#include "npt_com.h"
#include "npt_common.h"
#include "npt_device.h"
#include "npt_dispatch.h"
#include "npt_renderer.h"
#include "npt_transport_defs.h"

#include "neptune-protocol/npt_protocol_client_id3d11device.h"
#include "neptune-protocol/npt_protocol_defs.h"
#include "neptune-protocol/npt_protocol_directx_types.h"

bool
npt_shared_texture_export_blob(void *texture_wrapper,
                               struct npt_shared_texture_desc *desc)
{
   if (!desc)
      return false;

   /* Never leave an old export identity in a caller-owned description when
    * this attempt fails.  The D3D11 fields below planes[] remain owned by the
    * caller. */
   desc->blob_id = 0;
   desc->create_ctx_id = 0;
   desc->plane_count = 0;
   desc->texture_layout = 0;
   desc->modifier = 0;
   desc->allocation_size = 0;
   memset(desc->planes, 0, sizeof(desc->planes));

   if (!texture_wrapper)
      return false;

   struct npt_device *dev = npt_com_self_device(texture_wrapper);
   if (!dev || !dev->renderer)
      return false;
   struct npt_renderer *renderer = dev->renderer;

   const uint64_t texture_id = npt_com_self_id(texture_wrapper);
   if (!texture_id)
      return false;

   /* Reply window for npt_blob_export_info.  data_off is required: the
    * info shmem comes from a bump-allocated pool, so the host deposits
    * the struct at our offset, not at the resource start. */
   size_t info_off = 0;
   struct npt_renderer_shmem *shm =
      npt_device_alloc_data(dev, sizeof(struct npt_blob_export_info),
                            &info_off);
   if (!shm)
      return false;
   if (info_off > UINT32_MAX) {
      npt_log("shared texture: export reply offset is too large (%" PRIu64 ")",
              (uint64_t)info_off);
      npt_renderer_shmem_unref(renderer, shm);
      return false;
   }
   memset((uint8_t *)shm->mmap_ptr + info_off, 0,
          sizeof(struct npt_blob_export_info));

   /* blob_id = the texture's object id: unique within this context,
    * which is the scope of the host's pending-blob table. */
   HRESULT hr = npt_dispatch_shared_export_blob(
      npt_com_self_ring(texture_wrapper), texture_id, texture_id,
      shm->res_id, (uint32_t)info_off);

   bool ok = false;
   if (NPT_SUCCEEDED(hr)) {
      const struct npt_blob_export_info *info =
         (const struct npt_blob_export_info *)
            ((uint8_t *)shm->mmap_ptr + info_off);
      if (info->plane_count == 1 &&
          info->allocation_size &&
          info->planes[0].pitch &&
          info->planes[0].offset < info->allocation_size &&
          !info->planes[1].offset && !info->planes[1].pitch &&
          !info->planes[2].offset && !info->planes[2].pitch &&
          !info->planes[3].offset && !info->planes[3].pitch) {
         desc->blob_id = texture_id;
         desc->create_ctx_id = renderer->info.virtio_ctx_id;
         desc->plane_count = info->plane_count;
         desc->texture_layout = info->texture_layout;
         desc->modifier = info->modifier;
         desc->allocation_size = info->allocation_size;
         for (uint32_t i = 0; i < info->plane_count; i++) {
            desc->planes[i].offset = info->planes[i].offset;
            desc->planes[i].pitch = info->planes[i].pitch;
         }
         ok = true;
      } else {
         npt_log("shared texture: export id 0x%016" PRIx64
                 " returned bad info (planes=%u size=%" PRIu64 ")",
                 texture_id, info->plane_count, info->allocation_size);
      }
   } else {
      npt_log("shared texture: export id 0x%016" PRIx64 " failed hr=0x%x",
              texture_id, hr);
   }

   npt_renderer_shmem_unref(renderer, shm);
   return ok;
}

uint32_t
npt_shared_texture_host_format(uint32_t dxgi_format)
{
   switch (dxgi_format) {
   case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
   case DXGI_FORMAT_R8G8B8A8_TYPELESS:
      return DXGI_FORMAT_R8G8B8A8_UNORM;
   case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
   case DXGI_FORMAT_B8G8R8A8_TYPELESS:
      return DXGI_FORMAT_B8G8R8A8_UNORM;
   default:
      return dxgi_format;
   }
}

uint32_t
npt_shared_texture_virgl_format(uint32_t dxgi_format)
{
   switch (dxgi_format) {
   case DXGI_FORMAT_B8G8R8A8_UNORM: return 1;  /* VIRGL_FORMAT_B8G8R8A8_UNORM */
   case DXGI_FORMAT_B8G8R8X8_UNORM: return 2;  /* VIRGL_FORMAT_B8G8R8X8_UNORM */
   case DXGI_FORMAT_R8G8B8A8_UNORM: return 67; /* VIRGL_FORMAT_R8G8B8A8_UNORM */
   default:
      npt_log("shared texture: DXGI format %u is not scanout-capable",
              dxgi_format);
      return 0;
   }
}

void *
npt_shared_texture_create_exportable(void *device_wrapper,
                                     uint32_t width, uint32_t height,
                                     uint32_t dxgi_format)
{
   if (!device_wrapper || !width || !height || width > UINT32_MAX / 4 ||
       !npt_shared_texture_virgl_format(dxgi_format))
      return NULL;

   /* MISC_SHARED gives the texture exportable dedicated storage; the
    * vendor LINEAR_EXPORT bit forces DRM_FORMAT_MOD_LINEAR so
    * modifier-less consumers can import it.  SHADER_RESOURCE lets
    * copy/composite paths sample it. */
   D3D11_TEXTURE2D_DESC d;
   memset(&d, 0, sizeof(d));
   d.Width            = width;
   d.Height           = height;
   d.MipLevels        = 1;
   d.ArraySize        = 1;
   d.Format           = (DXGI_FORMAT)dxgi_format;
   d.SampleDesc.Count = 1;
   d.Usage            = D3D11_USAGE_DEFAULT;
   d.BindFlags        = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
   d.MiscFlags        = D3D11_RESOURCE_MISC_SHARED |
                        NPT_D3D11_MISC_LINEAR_EXPORT;

   const struct npt_id3d11device_client_vtbl *v =
      (const void *)((struct npt_com_base *)device_wrapper)->lpVtbl;
   ID3D11Texture2D *tex = NULL;
   HRESULT hr = v->CreateTexture2D(device_wrapper, &d, NULL, &tex);
   if (NPT_FAILED(hr) || !tex) {
      npt_log("shared texture: exportable %ux%u fmt=%u create failed hr=0x%x",
              width, height, dxgi_format, hr);
      return NULL;
   }
   return tex;
}
