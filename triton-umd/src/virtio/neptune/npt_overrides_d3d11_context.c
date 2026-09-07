/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * ID3D11DeviceContext{,1..4}: Map/Unmap shadow staging,
 * UpdateSubresource (registry-skipped because pSrcData is unsized),
 * and Begin/End/GetData with shared-memory query feedback.
 */

#include "npt_com.h"
#include "npt_overrides_d3d11_feedback.h"
#include "npt_device.h"
#include "npt_dispatch.h"
#include "npt_env.h"
#include "npt_overrides.h"
#include "npt_resource.h"
#include "npt_ring.h"

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
   if (!npt_d3d11_texture_is_mappable(t))
      return NPT_E_NOTIMPL;
   if (!npt_d3d11_texture_ensure_map_shmem(t))
      return NPT_E_OUTOFMEMORY;

   const uint32_t per_slot    = npt_d3d11_texture_get_slot_size(t);
   const uint32_t shmem_total = npt_d3d11_texture_get_shmem_size(t);
   const uint32_t cached_rp   = npt_d3d11_texture_get_cached_row_pitch(t);
   const uint32_t cached_dp   = npt_d3d11_texture_get_cached_depth_pitch(t);
   const bool can_async = (cached_rp != 0) &&
      !NPT_PERF(NO_DYNAMIC_MAP_FAST_PATH) &&
      (MapType == D3D11_MAP_WRITE_DISCARD ||
       MapType == D3D11_MAP_WRITE_NO_OVERWRITE);

   if (can_async) {
      const uint32_t byte_size =
         npt_d3d11_texture_get_subresource_byte_size(t, Subresource, cached_rp);
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

   uint32_t mip_h = 0, mip_d = 0;
   npt_d3d11_texture_get_mip_dimensions(t, Subresource, &mip_h, &mip_d);

   uint32_t row_pitch = 0, depth_pitch = 0;
   HRESULT hr = npt_dispatch_resource_map(
      npt_com_self_ring(self), context_id, resource_id, Subresource,
      npt_d3d11_map_to_access_flags(MapType), MapFlags,
      npt_d3d11_texture_get_map_shmem_res_id(t),
      /*byte_size=*/per_slot,
      mip_h, mip_d,
      /*shmem_offset=*/npt_d3d11_texture_slot_offset(t, 0),
      &row_pitch, &depth_pitch);
   if (NPT_FAILED(hr))
      return hr;

   /* Bound Unmap memcpy at host_row_pitch * h * d.  Refuse if the
    * host's RowPitch implies a region larger than per_slot, or one
    * that cannot be sized at all -- silent overrun is worse than a
    * clean failure. */
   const uint32_t byte_size =
      npt_d3d11_texture_get_subresource_byte_size(t, Subresource, row_pitch);
   if (!byte_size || byte_size > per_slot) {
      npt_log("ctx_Map_texture: row_pitch=%u implies %u-byte mapped "
              "region, exceeds per-slot %u (shmem %u) -- refusing",
              row_pitch, byte_size, per_slot, shmem_total);
      memset(pMappedResource, 0, sizeof(*pMappedResource));
      return NPT_E_FAIL;
   }

   /* RowPitch is stable per (resource, subresource); pitch drift
    * would trip the byte_size > per_slot guard above. */
   npt_d3d11_texture_set_cached_pitches(t, row_pitch, depth_pitch);

   pMappedResource->pData = npt_d3d11_texture_shmem_ptr(t);
   pMappedResource->RowPitch = row_pitch;
   pMappedResource->DepthPitch = depth_pitch;
   /* access_flags=0 => Unmap reuses sync-MAP map_state. */
   npt_d3d11_texture_set_mapped_state(t, Subresource, row_pitch,
                                      byte_size, /*access_flags=*/0);
   return NPT_S_OK;
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

/*
 * The generator skips UpdateSubresource (pSrcData is unsized);
 * route through RESOURCE_UPDATE.  Box sizing per resource:
 *   buffer:    pDstBox ? (right-left) : ByteWidth
 *   tex1d:     pDstBox ? (right-left)*bpp : width*bpp
 *   tex2d:     SrcRowPitch * rows_of_memory(height_box)
 *   tex3d:     SrcDepthPitch * depth_box
 */
static void NPT_STDMETHODCALLTYPE
ctx_UpdateSubresource_override(void *self, ID3D11Resource *pDstResource,
                               UINT DstSubresource, const D3D11_BOX *pDstBox,
                               const void *pSrcData, UINT SrcRowPitch,
                               UINT SrcDepthPitch)
{
   if (!pDstResource || !pSrcData)
      return;

   /* MSDN: an empty box -- top >= bottom, left >= right, or
    * front >= back -- is a no-op.  Return before the unsigned box
    * extents below wrap. */
   if (pDstBox && (pDstBox->left >= pDstBox->right ||
                   pDstBox->top >= pDstBox->bottom ||
                   pDstBox->front >= pDstBox->back))
      return;

   struct npt_device *dev = npt_com_self_device(self);
   if (!dev)
      return;

   uint64_t resource_id = ((struct npt_com_base *)pDstResource)->base.id;
   uint64_t byte_size = 0;   /* ring reservation; what the host may consume */
   uint64_t copy_size = 0;   /* what D3D guarantees readable at pSrcData */

   struct npt_d3d11_buffer *b = npt_d3d11_buffer_cast(pDstResource);
   if (b) {
      byte_size = pDstBox ? (pDstBox->right - pDstBox->left)
                          : npt_d3d11_buffer_get_byte_width(b);
      copy_size = byte_size;
   } else {
      struct npt_d3d11_texture *t = npt_d3d11_texture_cast(pDstResource);
      if (t) {
         if (pDstBox) {
            /* Dimension from aux: depth>1 => 3D, height>1 => 2D, else 1D. */
            uint32_t tex_h = 0, tex_d = 0;
            npt_d3d11_texture_get_mip_dimensions(t, DstSubresource,
                                                 &tex_h, &tex_d);
            const uint32_t box_w = pDstBox->right  - pDstBox->left;
            const uint32_t box_h = pDstBox->bottom - pDstBox->top;
            const uint32_t box_d = pDstBox->back   - pDstBox->front;
            /* Box bounds are texels; SrcRowPitch spans a row of memory,
             * which under block compression is four texel rows.  The
             * reservation charges the full pitch for every row, but the
             * copy stops at the D3D-guaranteed source extent,
             *   (rows-1)*RowPitch + RowSizeInBytes
             * per slice (dwm's section bitmaps end exactly at a page
             * edge -> AV in the ring copy past that extent). */
            const DXGI_FORMAT fmt = npt_d3d11_texture_get_format(t);
            uint32_t rows = npt_dxgi_format_block_rows(fmt, box_h);
            bool planar = false;
            if (pDstBox->top == 0 && box_h == tex_h) {
               /* A full-height box covers the chroma plane(s) that the
                * planar formats store after the luma rows; a partial
                * box has no documented planar footprint and stays
                * luma-only. */
               const uint32_t sub_rows =
                  npt_dxgi_format_subresource_rows(fmt, box_h);
               planar = sub_rows != rows;
               rows = sub_rows;
            }
            const uint32_t last_row = npt_dxgi_format_row_bytes(fmt, box_w);
            /* MSDN sizes planar transfers by the full pitch of every
             * row, chroma included; no final-row discount there. */
            const bool tight = last_row && !planar;
            if (tex_d > 1) {
               const uint64_t strides =
                  (uint64_t)SrcDepthPitch * (box_d - 1u);
               byte_size = strides + (uint64_t)SrcRowPitch * rows;
               copy_size = tight
                  ? strides + (uint64_t)SrcRowPitch * (rows - 1u) + last_row
                  : byte_size;
            } else if (tex_h > 1) {
               byte_size = (uint64_t)SrcRowPitch * rows;
               copy_size = tight
                  ? (uint64_t)SrcRowPitch * (rows - 1u) + last_row
                  : byte_size;
            } else {
               /* 1D: SrcRowPitch is meaningless; one tight row. */
               byte_size = last_row;
               copy_size = last_row;
            }
            if (copy_size > byte_size)
               copy_size = byte_size;
         } else {
            uint32_t copy32 = 0;
            byte_size = npt_d3d11_texture_get_subresource_update_sizes(
               t, DstSubresource, SrcRowPitch, SrcDepthPitch, &copy32);
            copy_size = copy32;
         }
      }
   }

   if (!byte_size || !copy_size || byte_size > 0xffffffffu) {
      npt_log("ctx_UpdateSubresource: unsupported resource type "
              "(resource=%p sub=%u row=%u depth=%u) -- dropping update",
              pDstResource, DstSubresource, SrcRowPitch, SrcDepthPitch);
      return;
   }

   (void)npt_dispatch_resource_update(npt_device_method_ring(dev), resource_id,
                                      DstSubresource, SrcRowPitch,
                                      SrcDepthPitch, pDstBox,
                                      pSrcData, (uint32_t)byte_size,
                                      (uint32_t)copy_size);
}

/*
 * Query feedback: each query has a 128-B shmem slot.  Begin clears
 * the flag and bumps a version; the host's poll writes [result,
 * version, flag=1] when GetData is ready; guest GetData reads
 * locally.
 */

static struct npt_query_feedback_slot *
ctx_query_slot(struct npt_d3d11_query_aux *aux)
{
   if (!aux || !aux->base.fb_shmem)
      return NULL;
   return (struct npt_query_feedback_slot *)
      ((uint8_t *)aux->base.fb_shmem->mmap_ptr + aux->base.fb_offset);
}

static void NPT_STDMETHODCALLTYPE
ctx_Begin_override(void *self, ID3D11Asynchronous *pAsync)
{
   struct npt_d3d11_query_aux *aux = npt_d3d11_query_aux_cast(pAsync);
   if (aux && aux->base.registered) {
      /* Bump version + clear flag BEFORE the wire Begin so a racing
       * guest GetData sees flag=0 or a version mismatch (S_FALSE).
       * "New version + old result" is impossible: host writes flag=1
       * only after the result with release ordering. */
      uint32_t v = atomic_fetch_add_explicit(&aux->local_version, 1,
                                             memory_order_relaxed) + 1;
      struct npt_query_feedback_slot *slot = ctx_query_slot(aux);
      if (slot) {
         atomic_store_explicit(&slot->state, ((uint64_t)v) << 32,
                               memory_order_release);
      }
   }
   npt_id3d11devicecontext_default_Begin(self, pAsync);
}

static HRESULT NPT_STDMETHODCALLTYPE
ctx_GetData_override(void *self, ID3D11Asynchronous *pAsync, void *pData,
                     UINT DataSize, UINT GetDataFlags)
{
   struct npt_d3d11_query_aux *aux = npt_d3d11_query_aux_cast(pAsync);
   struct npt_query_feedback_slot *slot = ctx_query_slot(aux);
   if (!slot || !aux->base.registered) {
      /* Counter, OOM at Create, or QI-routed wrapper. */
      return npt_id3d11devicecontext_default_GetData(self, pAsync, pData,
                                                     DataSize, GetDataFlags);
   }

   const uint64_t s = atomic_load_explicit(&slot->state,
                                           memory_order_acquire);
   const uint32_t flag = (uint32_t)(s & 0xFFFFFFFFu);
   const uint32_t version = (uint32_t)(s >> 32);
   const uint32_t expected =
      atomic_load_explicit(&aux->local_version, memory_order_relaxed);

   if (flag == NPT_QUERY_FEEDBACK_FLAG_READY && version == expected) {
      if (pData && DataSize) {
         const UINT copy = DataSize <= aux->query_data_size ?
                           DataSize : aux->query_data_size;
         memcpy(pData, slot->result, copy);
      }
      return NPT_S_OK;
   }

   /* DONOTFLUSH: caller polls; return S_FALSE locally.  Otherwise
    * the spec allows GetData to flush the immediate context, so go
    * sync to drive result availability. */
   if (GetDataFlags & 0x1u /* D3D11_ASYNC_GETDATA_DONOTFLUSH */)
      return NPT_S_FALSE;
   return npt_id3d11devicecontext_default_GetData(self, pAsync, pData,
                                                  DataSize, GetDataFlags);
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
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE_CONTEXT(Begin,   ctx_Begin_override);
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE_CONTEXT(GetData, ctx_GetData_override);

   npt_com_register_family(context_tiers, 0, NULL);
}
