/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Neptune-internals side of the Triton shared-resource bridge (see
 * tritonSharedBridge.h).  Speaks the Neptune transport (ring, SHARED
 * subgroup dispatch, COM wrapper cache) that the Triton DDI TUs cannot.
 */

#include "tritonSharedBridge.h"
#include "tritonDitherControl.h"

#include <string.h>

#include "npt_com.h"
#include "npt_common.h"
#include "npt_device.h"
#include "npt_dispatch.h"
#include "npt_renderer.h"
#include "npt_resource.h"
#include "npt_ring.h"
#include "npt_shared_texture.h"
#include "npt_transport_defs.h"

#include "neptune-protocol/npt_protocol_defs.h"
#include "neptune-protocol/npt_protocol_client_id3d11devicechild.h"
#include "neptune-protocol/npt_protocol_client_id3d11devicecontext.h"

int32_t
tritonSharedBridgeCopyColor(void *context, void *dst_rtv, void *src_srv)
{
   return npt_dispatch_resource_copy_color(context, dst_rtv, src_srv);
}

int32_t
tritonSharedBridgeGetDitherCaps(void *context, uint32_t *flags)
{
   static const GUID guid = TRITON_DITHER_CAPS_GUID;
   struct TritonDitherCaps caps = {0};
   UINT size = sizeof(caps);
   HRESULT hr;

   if (!context || !flags)
      return (int32_t)0x80070057; /* E_INVALIDARG */
   *flags = 0;
   hr = npt_id3d11devicechild_default_GetPrivateData(context, &guid, &size, &caps);
   if (hr < 0)
      return hr;
   if (size != sizeof(caps) || caps.version != TRITON_DITHER_VERSION)
      return (int32_t)0x887a0004; /* DXGI_ERROR_UNSUPPORTED */
   *flags = caps.flags;
   return hr;
}

int32_t
tritonSharedBridgeSetDither(void *context, uint32_t enabled)
{
   static const GUID guid = TRITON_DITHER_STATE_GUID;
   const struct TritonDitherState state = {TRITON_DITHER_VERSION, enabled};

   if (!context || enabled > 1)
      return (int32_t)0x80070057; /* E_INVALIDARG */
   /* The ordinary SetPrivateData thunk is asynchronous. This state affects
    * draw correctness, so propagate host validation/feature errors. */
   return npt_call_ID3D11DeviceChild_SetPrivateData(
      npt_com_self_ring(context), npt_com_self_id(context), &guid,
      sizeof(state), &state);
}

int32_t
tritonSharedBridgeTraceQueryRead(void *context, void *query, void *data, uint32_t size)
{
   /* Feedback generations are not a trustworthy source for diagnostic query
    * reuse yet. Query the host object after the existing completion fence;
    * never accept a stale shared-memory result or force an extra GPU flush. */
   return npt_id3d11devicecontext_default_GetData(context, query, data, size, 1);
}

/* The two descs carry the same export half but are copied field by
 * field, so only the plane-array bound has to agree. */
_Static_assert(TRITON_SHARED_MAX_PLANES == NPT_SHARED_TEXTURE_MAX_PLANES,
               "shared texture plane count mismatch");

/* The Vista D3D9 declaration lives in triton9.h.  Keep this transport unit
 * independent of Windows DDI headers while satisfying prototype checking. */
bool triton9DrainPrimaryTransport(void *pWrapper, uint32_t timeout_ms);
bool triton9ReleaseImportTransport(void *pWrapper, uint32_t alloc,
                                   uint32_t res_kmt);
bool triton9TransportHealthy(void *pWrapper);
bool npt_vgw32_release_import_res_checked(struct npt_renderer *renderer,
                                          uint32_t alloc, uint32_t res_kmt);

bool
tritonSharedBridgeDrain(void *pWrapper, uint32_t timeout_ms)
{
   struct npt_device *dev;
   struct npt_ring *ring;

   if (!pWrapper || !timeout_ms)
      return false;
   dev = npt_com_self_device(pWrapper);
   ring = npt_com_self_ring(pWrapper);
   if (!dev || !dev->renderer || !ring)
      return false;

   /* Wait for the host dispatcher to consume preceding proxy calls, then
    * retire an explicit renderer marker. Host API dispatch can enqueue GPU
    * work asynchronously; callers needing completed pixels must wait for a
    * GPU fence separately before using this transport boundary. */
   if (!npt_ring_wait_all_timeout(ring, timeout_ms))
      return false;
   return npt_renderer_submit_cmd_sync(dev->renderer, NULL, 0);
}

bool
triton9DrainPrimaryTransport(void *pWrapper, uint32_t timeout_ms)
{
   struct npt_device *dev;

   if (!pWrapper || !timeout_ms)
      return false;
   dev = npt_com_self_device(pWrapper);
   if (!dev || !dev->renderer || !dev->ring)
      return false;

   /* npt_com_send_release always uses dev->ring.  Do not derive this drain
    * from the caller's TLS ring, because that can retire an unrelated tail. */
   if (!npt_ring_wait_all_timeout(dev->ring, timeout_ms))
      return false;
   return npt_renderer_submit_cmd_sync(dev->renderer, NULL, 0);
}

bool
triton9ReleaseImportTransport(void *pWrapper, uint32_t alloc,
                              uint32_t res_kmt)
{
   struct npt_device *dev;

   if (!pWrapper || (!alloc && !res_kmt))
      return false;
   dev = npt_com_self_device(pWrapper);
   if (!dev || !dev->renderer)
      return false;
   return npt_vgw32_release_import_res_checked(dev->renderer, alloc, res_kmt);
}

bool
triton9TransportHealthy(void *pWrapper)
{
   struct npt_device *dev;
   struct npt_ring *ring;

   if (!pWrapper)
      return false;
   dev = npt_com_self_device(pWrapper);
   ring = npt_com_self_ring(pWrapper);
   if (!dev || !dev->renderer || !dev->ring || !ring)
      return false;
   return npt_ring_is_healthy(dev->ring) && npt_ring_is_healthy(ring);
}

bool
tritonSharedBridgeExportBlob(void *pResourceWrapper,
                             struct triton_shared_texture_desc *opts)
{
   if (!opts)
      return false;

   /* Export via the shared helper, then copy the export half into the
    * Triton-side desc. */
   struct npt_shared_texture_desc exp;
   memset(&exp, 0, sizeof(exp));
   if (!npt_shared_texture_export_blob(pResourceWrapper, &exp))
      return false;

   opts->blob_id = exp.blob_id;
   opts->create_ctx_id = exp.create_ctx_id;
   opts->plane_count = exp.plane_count;
   opts->texture_layout = exp.texture_layout;
   opts->modifier = exp.modifier;
   opts->allocation_size = exp.allocation_size;
   for (uint32_t i = 0;
        i < exp.plane_count && i < TRITON_SHARED_MAX_PLANES; i++) {
      opts->planes[i].offset = exp.planes[i].offset;
      opts->planes[i].pitch = exp.planes[i].pitch;
   }
   return true;
}

bool
tritonSharedBridgeCancelExportBlob(void *pResourceWrapper, uint64_t blob_id)
{
   if (!pResourceWrapper || !blob_id)
      return false;
   struct npt_ring *ring = npt_com_self_ring(pResourceWrapper);
   return ring && NPT_SUCCEEDED(npt_dispatch_shared_cancel_export(ring, blob_id));
}

bool
tritonSharedBridgeImportRes(void *pDeviceWrapper, uint32_t res_id,
                            uint64_t size, uint32_t *out_alloc,
                            uint32_t *out_res_kmt)
{
   if (!pDeviceWrapper || !res_id || !out_alloc || !out_res_kmt)
      return false;
   struct npt_device *dev = npt_com_self_device(pDeviceWrapper);
   if (!dev || !dev->renderer)
      return false;
   return npt_renderer_import_res(dev->renderer, res_id, size,
                                  out_alloc, out_res_kmt);
}

bool
tritonSharedBridgeReleaseImportRes(void *pDeviceWrapper, uint32_t alloc,
                                   uint32_t res_kmt)
{
   if (!pDeviceWrapper)
      return false;
   struct npt_device *dev = npt_com_self_device(pDeviceWrapper);
   if (!dev || !dev->renderer)
      return false;
   return npt_renderer_release_import_res(dev->renderer, alloc, res_kmt);
}

bool
tritonSharedBridgeQueryRes(void *pDeviceWrapper, uint32_t res_id,
                           struct triton_shared_texture_desc *opts)
{
   if (!pDeviceWrapper || !res_id || !opts)
      return false;
   struct npt_ring *ring = npt_com_self_ring(pDeviceWrapper);
   if (!ring)
      return false;
   struct npt_cmd_shared_query_layout_reply reply = {0};
   if (NPT_FAILED(npt_dispatch_shared_query_layout(ring, res_id, &reply)) ||
       reply.export_info.plane_count != 1 || !reply.width || !reply.height)
      return false;
   memset(opts, 0, sizeof(*opts));
   opts->width = reply.width;
   opts->height = reply.height;
   opts->format = reply.format;
   opts->mip_levels = opts->array_size = opts->sample_count = 1;
   opts->bind_flags = 0x28; /* D3D11_BIND_RENDER_TARGET | SHADER_RESOURCE */
   opts->misc_flags = 2; /* D3D11_RESOURCE_MISC_SHARED */
   opts->plane_count = reply.export_info.plane_count;
   opts->modifier = reply.export_info.modifier;
   opts->allocation_size = reply.export_info.allocation_size;
   opts->texture_layout = reply.export_info.texture_layout;
   for (uint32_t i = 0; i < opts->plane_count; i++) {
      opts->planes[i].offset = reply.export_info.planes[i].offset;
      opts->planes[i].pitch = reply.export_info.planes[i].pitch;
   }
   return true;
}

void *
tritonSharedBridgeOpenRes(void *pDeviceWrapper, uint32_t res_id,
                          const struct triton_shared_texture_desc *opts)
{
   if (!pDeviceWrapper || !res_id || !opts)
      return NULL;
   struct npt_device *dev = npt_com_self_device(pDeviceWrapper);
   if (!dev || !dev->renderer || !npt_com_self_ring(pDeviceWrapper))
      return NULL;
   if (opts->plane_count < 1 ||
       opts->plane_count > NPT_BLOB_EXPORT_MAX_PLANES)
      return NULL;

   /* Mint the consumer-side object id up front; the host registers the
    * imported texture under it, so the wrapper we build below resolves
    * to the same host object on subsequent calls. */
   uint64_t mint = npt_com_allocate_next_id();

   struct npt_cmd_shared_open_res cmd;
   memset(&cmd, 0, sizeof(cmd));
   cmd.mint_object_id = mint;
   cmd.res_id = res_id;
   cmd.width = opts->width;
   cmd.height = opts->height;
   cmd.mip_levels = opts->mip_levels;
   cmd.array_size = opts->array_size;
   cmd.format = opts->format;
   cmd.sample_count = opts->sample_count;
   cmd.usage = opts->usage;
   cmd.bind_flags = opts->bind_flags;
   cmd.cpu_access_flags = opts->cpu_access_flags;
   cmd.misc_flags = opts->misc_flags;
   cmd.export_info.modifier = opts->modifier;
   cmd.export_info.allocation_size = opts->allocation_size;
   cmd.export_info.plane_count = opts->plane_count;
   cmd.export_info.texture_layout = opts->texture_layout;
   for (uint32_t i = 0; i < opts->plane_count; i++) {
      cmd.export_info.planes[i].offset = opts->planes[i].offset;
      cmd.export_info.planes[i].pitch = opts->planes[i].pitch;
   }

   HRESULT hr = npt_dispatch_shared_open_res(
      npt_com_self_ring(pDeviceWrapper),
      npt_com_self_id(pDeviceWrapper), &cmd);
   if (NPT_FAILED(hr)) {
      npt_log("shared bridge: open res_id=%u failed hr=0x%x", res_id, hr);
      return NULL;
   }

   void *wrapper = npt_com_get_or_wrap_or_release(
      dev, &NPT_IID_ID3D11Texture2D, mint,
      (struct npt_com_base *)pDeviceWrapper);
   struct npt_d3d11_texture *texture = npt_d3d11_texture_cast(wrapper);
   if (!texture) {
      if (wrapper)
         npt_com_default_release(wrapper);
      return NULL;
   }
   const struct npt_d3d11_texture_desc desc = {
      .width = opts->width,
      .height = opts->height,
      .depth = 1,
      .mip_levels = opts->mip_levels,
      .array_size = opts->array_size,
      .format = opts->format,
      .sample_count = opts->sample_count,
      .usage = opts->usage,
      .bind_flags = opts->bind_flags,
      .cpu_access_flags = opts->cpu_access_flags,
      .misc_flags = opts->misc_flags,
      .texture_layout = opts->texture_layout,
   };
   npt_d3d11_texture_set_desc(texture, &desc);
   return wrapper;
}
