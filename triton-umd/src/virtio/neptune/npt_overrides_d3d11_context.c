/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * ID3D11DeviceContext{,1..4}: Map/Unmap shadow staging,
 * UpdateSubresource (registry-skipped because pSrcData is unsized).
 */

#include "npt_com.h"
#include "npt_device.h"
#include "npt_dispatch.h"
#include "npt_env.h"
#include "npt_overrides.h"
#include "npt_resource.h"
#include "npt_ring.h"

#include <stdlib.h>

#include "neptune-protocol/npt_protocol_client_id3d11devicecontext.h"

#define NPT_REGISTER_OVERRIDE_D3D11_DEVICE_CONTEXT4(m, f) \
   NPT_REGISTER_OVERRIDE(id3d11devicecontext4, m, f)
#define NPT_REGISTER_OVERRIDE_D3D11_DEVICE_CONTEXT3(m, f) \
   NPT_REGISTER_OVERRIDE(id3d11devicecontext3, m, f); \
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE_CONTEXT4(m, f)
#define NPT_REGISTER_OVERRIDE_D3D11_DEVICE_CONTEXT2(m, f) \
   NPT_REGISTER_OVERRIDE(id3d11devicecontext2, m, f); \
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE_CONTEXT3(m, f)
#define NPT_REGISTER_OVERRIDE_D3D11_DEVICE_CONTEXT1(m, f) \
   NPT_REGISTER_OVERRIDE(id3d11devicecontext1, m, f); \
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE_CONTEXT2(m, f)
#define NPT_REGISTER_OVERRIDE_D3D11_DEVICE_CONTEXT(m, f) \
   NPT_REGISTER_OVERRIDE(id3d11devicecontext, m, f); \
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE_CONTEXT1(m, f)

static const GUID *const context_tiers[] = {
   &NPT_IID_ID3D11DeviceContext,  &NPT_IID_ID3D11DeviceContext1,
   &NPT_IID_ID3D11DeviceContext2, &NPT_IID_ID3D11DeviceContext3,
   &NPT_IID_ID3D11DeviceContext4, NULL,
};

static uint32_t
npt_d3d11_map_to_access_flags(D3D11_MAP map_type)
{
   switch (map_type) {
   case D3D11_MAP_READ:
      return NPT_MAP_ACCESS_READ;
   case D3D11_MAP_WRITE:
      return NPT_MAP_ACCESS_WRITE;
   case D3D11_MAP_READ_WRITE:
      return NPT_MAP_ACCESS_READ | NPT_MAP_ACCESS_WRITE;
   case D3D11_MAP_WRITE_DISCARD:
      return NPT_MAP_ACCESS_WRITE | NPT_MAP_ACCESS_DISCARD;
   case D3D11_MAP_WRITE_NO_OVERWRITE:
      return NPT_MAP_ACCESS_WRITE | NPT_MAP_ACCESS_NO_OVERWRITE;
   default:
      return NPT_MAP_ACCESS_WRITE;
   }
}


/*
 * WRITE_DISCARD / WRITE_NO_OVERWRITE take the rename-ring fast path
 * (no round-trip; Unmap replays Map+memcpy+Unmap on the host).
 * Other Map types take the sync MAP_RESOURCE path on slot 0.
 */
static HRESULT
ctx_Map_buffer(void *self, struct npt_d3d11_buffer *b, UINT Subresource,
               D3D11_MAP MapType, UINT MapFlags,
               D3D11_MAPPED_SUBRESOURCE *pMappedResource)
{
   if (!npt_d3d11_buffer_ensure_map_shmem(b))
      return NPT_E_OUTOFMEMORY;

   /* Bisection knob: NPT_PERF=no_dynamic_map_fast_path. */
   if (!NPT_PERF(NO_DYNAMIC_MAP_FAST_PATH) &&
       (MapType == D3D11_MAP_WRITE_DISCARD ||
        MapType == D3D11_MAP_WRITE_NO_OVERWRITE)) {
      const uint32_t flags = npt_d3d11_map_to_access_flags(MapType);
      if (MapType == D3D11_MAP_WRITE_DISCARD)
         npt_d3d11_buffer_rotate_slot(b);
      const uint32_t byte_width = npt_d3d11_buffer_get_byte_width(b);
      pMappedResource->pData =
         npt_d3d11_buffer_slot_ptr(b, npt_d3d11_buffer_get_current_slot(b));
      pMappedResource->RowPitch = byte_width;
      pMappedResource->DepthPitch = byte_width;
      npt_d3d11_buffer_set_last_map_access_flags(b, flags);
      npt_d3d11_buffer_set_is_mapped(b, true);
      return NPT_S_OK;
   }

   /* Sync round-trip, slot 0. */
   uint64_t context_id = ((struct npt_com_base *)self)->base.id;
   uint64_t buffer_id  = ((struct npt_com_base *)b)->base.id;

   uint32_t row_pitch = 0, depth_pitch = 0;
   HRESULT hr = npt_dispatch_resource_map(
      npt_com_self_ring(self), context_id, buffer_id, Subresource,
      npt_d3d11_map_to_access_flags(MapType), MapFlags,
      npt_d3d11_buffer_get_map_shmem_res_id(b),
      /*pessimistic_size=*/npt_d3d11_buffer_get_byte_width(b),
      /*mip_height=*/0, /*mip_depth=*/0,
      /*shmem_offset=*/npt_d3d11_buffer_slot_offset(b, 0),
      &row_pitch, &depth_pitch);
   if (NPT_FAILED(hr))
      return hr;

   /* access_flags=0 => Unmap reuses sync-MAP map_state. */
   npt_d3d11_buffer_set_current_slot(b, 0);
   pMappedResource->pData = npt_d3d11_buffer_slot_ptr(b, 0);
   pMappedResource->RowPitch = row_pitch;
   pMappedResource->DepthPitch = depth_pitch;
   npt_d3d11_buffer_set_last_map_access_flags(b, 0);
   npt_d3d11_buffer_set_is_mapped(b, true);
   return NPT_S_OK;
}

/*
 * 1D/2D/3D unified.  WRITE_DISCARD/NO_OVERWRITE with a cached
 * RowPitch take the rename-ring fast path (no round-trip; first Map
 * pays the sync cost).  All other paths go through the sync
 * MAP_RESOURCE on slot 0; host RowPitch/DepthPitch are stashed for
 * the next fast-path Map.
 */
static HRESULT
ctx_Map_texture(void *self, struct npt_d3d11_texture *t, UINT Subresource,
                D3D11_MAP MapType, UINT MapFlags,
                D3D11_MAPPED_SUBRESOURCE *pMappedResource)
{
   memset(pMappedResource, 0, sizeof(*pMappedResource));
   if (npt_d3d11_texture_get_is_mapped(t))
      return NPT_E_FAIL;
   if (!npt_d3d11_texture_ensure_desc(t, D3D11_RESOURCE_DIMENSION_UNKNOWN, false))
      return NPT_E_FAIL;
   if (!npt_d3d11_texture_is_mappable(t))
      return NPT_E_NOTIMPL;
   if (!npt_d3d11_texture_ensure_map_shmem(t))
      return NPT_E_OUTOFMEMORY;

   uint32_t mip_h = 0, mip_d = 0;
   npt_d3d11_texture_get_mip_dimensions(t, Subresource, &mip_h, &mip_d);
   const uint32_t per_slot    = npt_d3d11_texture_get_slot_size(t);
   const uint32_t cached_rp   = npt_d3d11_texture_get_cached_row_pitch(t);
   const uint32_t cached_dp   = npt_d3d11_texture_get_cached_depth_pitch(t);
   /* A volume map must obtain the current backend slice pitch. */
   const bool can_async = (mip_d == 1) && (cached_rp != 0) &&
      npt_d3d11_texture_get_last_map_subresource(t) == Subresource &&
      !NPT_PERF(NO_DYNAMIC_MAP_FAST_PATH) &&
      (MapType == D3D11_MAP_WRITE_DISCARD ||
       MapType == D3D11_MAP_WRITE_NO_OVERWRITE);

   if (can_async) {
      const uint32_t byte_size =
         npt_d3d11_texture_get_subresource_map_byte_size(t, Subresource,
                                                        cached_rp, cached_dp);
      if (byte_size && byte_size <= per_slot) {
         if (MapType == D3D11_MAP_WRITE_DISCARD)
            npt_d3d11_texture_rotate_slot(t);
         const uint32_t slot = npt_d3d11_texture_get_current_slot(t);
         pMappedResource->pData = npt_d3d11_texture_slot_ptr(t, slot);
         pMappedResource->RowPitch = cached_rp;
         pMappedResource->DepthPitch = cached_dp;
         npt_d3d11_texture_set_mapped_state(t, Subresource, cached_rp,
                                            byte_size,
                                            npt_d3d11_map_to_access_flags(MapType));
         return NPT_S_OK;
      }
      /* Cached pitch implies a byte_size that overruns one slot
       * (defensive: shouldn't happen for 2D); fall through to sync. */
   }

   /* Sync MAP_RESOURCE on slot 0; host MAP dispatcher reads from
    * offset 0.  Cache the returned pitches for the next fast-path. */
   uint64_t context_id  = ((struct npt_com_base *)self)->base.id;
   uint64_t resource_id = ((struct npt_com_base *)t)->base.id;
   npt_d3d11_texture_set_current_slot(t, 0);

   /* MAP_RESOURCE copies rows of storage, not rows of texels. */
   mip_h = npt_dxgi_format_subresource_rows(
      npt_d3d11_texture_get_format(t), mip_h);

   /* One retry accommodates backend row/slice alignment absent from the
    * descriptor. An abandoned successful Map must never upload stale SHM. */
   for (unsigned attempt = 0; attempt < 2; attempt++) {
      const uint32_t slot_size = npt_d3d11_texture_get_slot_size(t);
      uint32_t row_pitch = 0, depth_pitch = 0;
      HRESULT hr = npt_dispatch_resource_map(
         npt_com_self_ring(self), context_id, resource_id, Subresource,
         npt_d3d11_map_to_access_flags(MapType), MapFlags,
         npt_d3d11_texture_get_map_shmem_res_id(t), slot_size,
         mip_h, mip_d, npt_d3d11_texture_slot_offset(t, 0),
         &row_pitch, &depth_pitch);
      if (NPT_FAILED(hr))
         return hr;

      const uint32_t byte_size =
         npt_d3d11_texture_get_subresource_map_byte_size(t, Subresource,
                                                         row_pitch, depth_pitch);
      if (!byte_size || byte_size > slot_size) {
         const HRESULT aborted = npt_dispatch_resource_abort(
            npt_com_self_ring(self), context_id, resource_id, Subresource);
         if (NPT_FAILED(aborted) || !byte_size || attempt != 0)
            return NPT_E_FAIL;
         if (!npt_d3d11_texture_grow_map_shmem(t, byte_size))
            return NPT_E_OUTOFMEMORY;
         continue;
      }

      npt_d3d11_texture_set_cached_pitches(t, row_pitch, depth_pitch);
      pMappedResource->pData = npt_d3d11_texture_shmem_ptr(t);
      pMappedResource->RowPitch = row_pitch;
      pMappedResource->DepthPitch = depth_pitch;
      /* access_flags=0 => Unmap reuses sync-MAP map_state. */
      npt_d3d11_texture_set_mapped_state(t, Subresource, row_pitch,
                                         byte_size, /*access_flags=*/0);
      return NPT_S_OK;
   }
   return NPT_E_FAIL;
}

static void
ctx_Unmap_buffer(void *self, struct npt_d3d11_buffer *b, UINT Subresource)
{
   struct npt_ring *ring = npt_com_self_ring(self);
   const uint32_t slot = npt_d3d11_buffer_get_current_slot(b);
   const uint32_t access_flags =
      npt_d3d11_buffer_get_last_map_access_flags(b);

   uint32_t seqno = 0;
   npt_dispatch_resource_unmap_seqno(ring,
      ((struct npt_com_base *)self)->base.id,
      ((struct npt_com_base *)b)->base.id,
      Subresource,
      npt_d3d11_buffer_get_slot_shmem_res_id(b, slot),
      npt_d3d11_buffer_get_byte_width(b),
      npt_d3d11_buffer_slot_offset(b, slot),
      access_flags,
      &seqno);

   /* Sync Maps land on slot 0 and don't need rename-ring tracking. */
   if (access_flags)
      npt_d3d11_buffer_mark_slot_submitted(b, slot, seqno, ring);

   npt_d3d11_buffer_set_is_mapped(b, false);
   npt_d3d11_buffer_set_last_map_access_flags(b, 0);
}

static void
ctx_Unmap_texture(void *self, struct npt_d3d11_texture *t)
{
   struct npt_ring *ring = npt_com_self_ring(self);
   const uint32_t access_flags = npt_d3d11_texture_get_last_map_access_flags(t);
   const uint32_t slot = npt_d3d11_texture_get_current_slot(t);

   uint32_t seqno = 0;
   if (access_flags) {
      npt_dispatch_resource_unmap_seqno(ring,
         ((struct npt_com_base *)self)->base.id,
         ((struct npt_com_base *)t)->base.id,
         npt_d3d11_texture_get_last_map_subresource(t),
         npt_d3d11_texture_get_slot_shmem_res_id(t, slot),
         npt_d3d11_texture_get_last_map_byte_size(t),
         /*shmem_offset=*/npt_d3d11_texture_slot_offset(t, slot),
         access_flags,
         &seqno);
      npt_d3d11_texture_mark_slot_submitted(t, slot, seqno, ring);
   } else {
      npt_dispatch_resource_unmap(ring,
         ((struct npt_com_base *)self)->base.id,
         ((struct npt_com_base *)t)->base.id,
         npt_d3d11_texture_get_last_map_subresource(t),
         npt_d3d11_texture_get_slot_shmem_res_id(t, slot),
         npt_d3d11_texture_get_last_map_byte_size(t),
         /*shmem_offset=*/npt_d3d11_texture_slot_offset(t, slot),
         /*access_flags=*/0);
   }

   npt_d3d11_texture_clear_mapped_state(t);
}

/* Resources without a wrapper fall through to E_NOTIMPL / no-op. */
static HRESULT NPT_STDMETHODCALLTYPE
ctx_Map_override(void *self, ID3D11Resource *pResource, UINT Subresource,
                 D3D11_MAP MapType, UINT MapFlags,
                 D3D11_MAPPED_SUBRESOURCE *pMappedResource)
{
   if (!pResource || !pMappedResource)
      return NPT_E_INVALIDARG;

   struct npt_d3d11_buffer *b = npt_d3d11_buffer_cast(pResource);
   if (b)
      return ctx_Map_buffer(self, b, Subresource, MapType, MapFlags,
                            pMappedResource);

   struct npt_d3d11_texture *t = npt_d3d11_texture_cast(pResource);
   if (t)
      return ctx_Map_texture(self, t, Subresource, MapType, MapFlags,
                             pMappedResource);

   return NPT_E_NOTIMPL;
}

static void NPT_STDMETHODCALLTYPE
ctx_Unmap_override(void *self, ID3D11Resource *pResource, UINT Subresource)
{
   if (!pResource) return;

   struct npt_d3d11_buffer *b = npt_d3d11_buffer_cast(pResource);
   if (b) {
      if (npt_d3d11_buffer_get_is_mapped(b))
         ctx_Unmap_buffer(self, b, Subresource);
      return;
   }

   struct npt_d3d11_texture *t = npt_d3d11_texture_cast(pResource);
   if (t && npt_d3d11_texture_get_is_mapped(t))
      ctx_Unmap_texture(self, t);
}

/* Keep every command below the host's 64 MiB update cap. Full resources
 * may exceed it: split at complete memory rows (BC block rows) and slices.
 * Unlike caller pitches, packed payload sizes never include source padding. */
#define NPT_RESOURCE_UPDATE_CHUNK_BYTES (4u << 20)

static uint32_t
ctx_update_min(uint32_t a, uint32_t b)
{
   return a < b ? a : b;
}

static void
ctx_update_failed(struct npt_device *dev, struct npt_ring *ring)
{
   npt_log("UpdateSubresource: upload failed; marking device transport failed");
   if (ring)
      atomic_store_explicit(&ring->failed, true, memory_order_release);
   if (dev->ring)
      atomic_store_explicit(&dev->ring->failed, true, memory_order_release);
}

static bool
ctx_update_buffer(struct npt_ring *ring, uint64_t resource_id,
                   struct npt_d3d11_buffer *buffer, UINT subresource,
                   const D3D11_BOX *box, const void *source)
{
   const uint32_t width = npt_d3d11_buffer_get_byte_width(buffer);
   D3D11_BOX region = {0, 0, 0, width, 1, 1};
   if (subresource || !width)
      return false;
   if (box) {
      if (box->left >= box->right || box->right > width || box->top ||
          box->front || box->bottom != 1 || box->back != 1)
         return false;
      region = *box;
   }
   const uint32_t bytes = region.right - region.left;
   if ((uintptr_t)source > UINTPTR_MAX - (bytes - 1u))
      return false;
   /* Base UpdateSubresource requires a NULL box for constant buffers,
    * including large D3D11.1 CBs. Preserve whole-buffer uploads within the
    * host command cap, and never convert a larger CB into invalid boxes. */
   const bool constant_buffer =
      npt_d3d11_buffer_get_bind_flags(buffer) & D3D11_BIND_CONSTANT_BUFFER;
   if (!box && bytes <= (64u << 20))
      return npt_dispatch_resource_update(ring, resource_id, 0, 0, 0,
                                          NULL, source, bytes, bytes);
   if (constant_buffer)
      return false;
   for (uint32_t offset = 0; offset < bytes;) {
      const uint32_t count = ctx_update_min(bytes - offset, NPT_RESOURCE_UPDATE_CHUNK_BYTES);
      D3D11_BOX chunk = region;
      chunk.left += offset;
      chunk.right = chunk.left + count;
      if (!npt_dispatch_resource_update(ring, resource_id, 0, 0, 0,
             !box && count == bytes ? NULL : &chunk,
             (const uint8_t *)source + offset, count, count))
         return false;
      offset += count;
   }
   return true;
}

static bool
ctx_update_texture(struct npt_ring *ring, uint64_t resource_id,
                    struct npt_d3d11_texture *texture, UINT subresource,
                    const D3D11_BOX *box, const void *source,
                    UINT source_row_pitch, UINT source_depth_pitch)
{
   D3D11_BOX region;
   uint32_t row_bytes, rows, block_height;
   bool planar;
   if (!npt_d3d11_texture_ensure_desc(texture,
                                     D3D11_RESOURCE_DIMENSION_UNKNOWN, false))
      return false;
   if (!npt_d3d11_texture_get_update_layout(texture, subresource, box,
          &region, &row_bytes, &rows, &block_height, &planar))
      return false;
   const uint32_t depth = region.back - region.front;
   /* A single row/slice has no following stride. In particular UINT_MAX
    * RowPitch is legal for a one-row upload whose source has just one texel. */
   const uint64_t row_pitch = rows > 1 ? source_row_pitch : row_bytes;
   if (row_pitch < row_bytes)
      return false;
   const uint64_t slice_extent = (uint64_t)(rows - 1u) * row_pitch + row_bytes;
   const uint64_t depth_pitch = depth > 1 ? source_depth_pitch : slice_extent;
   if (depth_pitch < slice_extent ||
       (uint64_t)(depth - 1u) > (UINT64_MAX - slice_extent) / depth_pitch)
      return false;
   const uint64_t extent = (uint64_t)(depth - 1u) * depth_pitch + slice_extent;
   if (extent > SIZE_MAX || (uintptr_t)source > UINTPTR_MAX - (extent - 1u))
      return false;

   if (planar) {
      /* Preserve the existing full-plane modern layout. Plane-aware splitting
       * needs a different wire operation; it is not advertised by D3D9/10. */
      const uint64_t bytes = row_pitch * rows;
      if (bytes > (64u << 20) || bytes > SIZE_MAX ||
          (uintptr_t)source > UINTPTR_MAX - (bytes - 1u))
         return false;
      return npt_dispatch_resource_update(ring, resource_id, subresource,
               source_row_pitch, source_depth_pitch, box, source,
               (uint32_t)bytes, (uint32_t)bytes);
   }
   if (row_bytes > NPT_RESOURCE_UPDATE_CHUNK_BYTES)
      return false; /* exceeds every legal D3D9/10/11 texture row */
   const uint32_t max_rows = ctx_update_min(rows, NPT_RESOURCE_UPDATE_CHUNK_BYTES / row_bytes);
   const uint32_t capacity = max_rows * row_bytes;
   uint8_t *packed = NULL;
   if (row_pitch != row_bytes && rows > 1) {
      packed = malloc(capacity);
      if (!packed)
         return false;
   }
   bool ok = true;
   for (uint32_t z = 0; ok && z < depth; ++z) {
      for (uint32_t y = 0; y < rows;) {
         const uint32_t count = ctx_update_min(rows - y, max_rows);
         const uint32_t bytes = count * row_bytes;
         const uint8_t *data = (const uint8_t *)source +
            (size_t)z * depth_pitch + (size_t)y * row_pitch;
         if (packed) {
            for (uint32_t row = 0; row < count; ++row)
               memcpy(packed + (size_t)row * row_bytes,
                      data + (size_t)row * row_pitch, row_bytes);
            data = packed;
         }
         D3D11_BOX chunk = region;
         chunk.top += y * block_height;
         chunk.bottom = ctx_update_min(region.bottom, chunk.top + count * block_height);
         chunk.front += z;
         chunk.back = chunk.front + 1u;
         const bool whole = !box && depth == 1 && count == rows;
         if (!npt_dispatch_resource_update(ring, resource_id, subresource,
                row_bytes, bytes, whole ? NULL : &chunk, data, bytes, bytes)) {
            ok = false;
            break;
         }
         y += count;
      }
   }
   free(packed);
   return ok;
}

static void NPT_STDMETHODCALLTYPE
ctx_UpdateSubresource_override(void *self, ID3D11Resource *pDstResource,
                               UINT DstSubresource, const D3D11_BOX *pDstBox,
                               const void *pSrcData, UINT SrcRowPitch,
                               UINT SrcDepthPitch)
{
   if (!pDstResource || !pSrcData)
      return;
   if (pDstBox && (pDstBox->left >= pDstBox->right ||
                   pDstBox->top >= pDstBox->bottom ||
                   pDstBox->front >= pDstBox->back))
      return;
   struct npt_device *dev = npt_com_self_device(self);
   if (!dev)
      return;
   struct npt_ring *ring = npt_device_method_ring(dev);
   const uint64_t resource_id = ((struct npt_com_base *)pDstResource)->base.id;
   struct npt_d3d11_buffer *buffer = npt_d3d11_buffer_cast(pDstResource);
   struct npt_d3d11_texture *texture = npt_d3d11_texture_cast(pDstResource);
   const bool ok = buffer
      ? ctx_update_buffer(ring, resource_id, buffer, DstSubresource,
                           pDstBox, pSrcData)
      : texture && ctx_update_texture(ring, resource_id, texture, DstSubresource,
                                       pDstBox, pSrcData, SrcRowPitch, SrcDepthPitch);
   if (!ok)
      ctx_update_failed(dev, ring);
}

/* CopyFlags hints (DISCARD, NO_OVERWRITE) are advisory in the host
 * D3D library and lost on the wire either way. */
static void NPT_STDMETHODCALLTYPE
ctx_UpdateSubresource1_override(void *self, ID3D11Resource *pDstResource,
                                UINT DstSubresource, const D3D11_BOX *pDstBox,
                                const void *pSrcData, UINT SrcRowPitch,
                                UINT SrcDepthPitch, UINT CopyFlags)
{
   (void)CopyFlags;
   ctx_UpdateSubresource_override(self, pDstResource, DstSubresource,
                                  pDstBox, pSrcData, SrcRowPitch,
                                  SrcDepthPitch);
}

void
npt_overrides_d3d11_context_init(void)
{
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE_CONTEXT(Map,   ctx_Map_override);
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE_CONTEXT(Unmap, ctx_Unmap_override);
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE_CONTEXT (UpdateSubresource,
                                               ctx_UpdateSubresource_override);
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE_CONTEXT1(UpdateSubresource1,
                                               ctx_UpdateSubresource1_override);

   npt_com_register_family(context_tiers, 0, NULL);
}
