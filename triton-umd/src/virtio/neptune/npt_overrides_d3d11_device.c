/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * ID3D11Device{,1..5} overrides:
 *  - CreateBuffer/CreateTexture*: pInitialData->pSysMem can't be
 *    auto-marshalled; create with NULL data + ship payload via
 *    RESOURCE_UPDATE.
 *  - GetImmediateContext{,1,2,3}: tier-aware accessor cache.
 *  - Device4::RegisterDeviceRemovedEvent: short-circuit to the
 *    host library's S_OK + cookie 0xdeadbeef stub.
 *  - Device4::UnregisterDeviceRemoved: noop.
 */

#include "npt_com.h"
#include "npt_device.h"
#include "npt_dispatch.h"
#include "npt_env.h"
#include "npt_object.h"
#include "npt_overrides.h"
#include "npt_resource.h"
#include "npt_ring.h"

#include <stdlib.h>
#include <string.h>

/* Per-device storage: caches the immediate-context wrapper so repeat
 * GetImmediateContextN calls don't burn an async create + COM_RELEASE.
 * Tier-aware: a call asking for an equal-or-narrower tier returns the
 * cached wrapper (narrowing is ABI-safe); a wider-tier call evicts
 * and re-fetches.  Cache holds one pub_ref, dropped by aux_destroy. */
struct npt_d3d11_device_aux {
   mtx_t cache_mutex;
   void *cached_imm_ctx;
   /* 0..3; ignored when cached_imm_ctx == NULL. */
   int cached_imm_ctx_tier;
};

static void npt_d3d11_device_aux_init(struct npt_com_base *com,
                                      struct npt_device *dev, uint64_t host_id);

#include "neptune-protocol/npt_protocol_client_id3d11device.h"
#include "neptune-protocol/npt_protocol_guest_id3d11device.h"
#include "neptune-protocol/npt_protocol_defs.h"

/* Sentinel cookie matching the host library's stub. */
#define NPT_DEVICE_REMOVED_COOKIE 0xdeadbeef
#define NPT_INITIAL_UPDATE_CHUNK_BYTES (4u << 20)

/* Initial data is submitted after the create command because the wire
 * protocol cannot marshal an unsized pSysMem pointer.  A failed upload must
 * poison both rings so a deferred Create cannot be observed as a successful
 * resource with undefined contents. */
static void
npt_initial_upload_failed(struct npt_device *dev, struct npt_ring *ring)
{
   if (!dev)
      return;
   if (ring)
      atomic_store_explicit(&ring->failed, true, memory_order_release);
   if (dev->ring)
      atomic_store_explicit(&dev->ring->failed, true, memory_order_release);
}

static uint32_t
npt_full_mip_levels(uint32_t width, uint32_t height, uint32_t depth)
{
   uint32_t largest = width;
   if (height > largest) largest = height;
   if (depth > largest) largest = depth;
   uint32_t levels = 1;
   while (largest > 1u) {
      largest >>= 1;
      ++levels;
   }
   return levels;
}

/* RESOURCE_UPDATE is capped at 64 MiB by the host.  Large initial texture
 * payloads are therefore packed into row-aligned boxes.  This is deliberately
 * separate from the ordinary small upload path: it never exposes application
 * padding to the host, and each command stays below the wire cap. */
static bool
npt_upload_texture_initial_split(struct npt_device *dev, uint64_t tex_id,
                                 uint32_t subresource,
                                 const D3D11_SUBRESOURCE_DATA *d,
                                 uint32_t width, uint32_t height,
                                 uint32_t depth, uint32_t rows,
                                 uint32_t row_bytes, uint32_t block_height)
{
   if (!d || !d->pSysMem || !rows || !row_bytes ||
       row_bytes > NPT_INITIAL_UPDATE_CHUNK_BYTES || !d->SysMemPitch ||
       d->SysMemPitch < row_bytes ||
       (depth > 1u && (!d->SysMemSlicePitch ||
                       d->SysMemSlicePitch <
                          (uint64_t)d->SysMemPitch * (rows - 1u) + row_bytes)))
      return false;

   const uint64_t slice_extent =
      (uint64_t)(rows - 1u) * d->SysMemPitch + row_bytes;
   if (!depth || (uint64_t)(depth - 1u) >
          (UINT64_MAX - slice_extent) / (depth > 1u ? d->SysMemSlicePitch : 1u))
      return false;
   const uint64_t extent =
      (uint64_t)(depth - 1u) * d->SysMemSlicePitch + slice_extent;
   if (extent > SIZE_MAX || extent - 1u > UINTPTR_MAX - (uintptr_t)d->pSysMem)
      return false;

   const uint32_t max_rows = NPT_INITIAL_UPDATE_CHUNK_BYTES / row_bytes;
   if (!max_rows)
      return false;
   const size_t packed_bytes = (size_t)max_rows * row_bytes;
   uint8_t *packed = malloc(packed_bytes);
   if (!packed)
      return false;

   bool ok = true;
   const uint64_t slice_pitch = depth > 1u
      ? d->SysMemSlicePitch : (uint64_t)d->SysMemPitch * rows;
   for (uint32_t z = 0; ok && z < depth; ++z) {
      const uint64_t slice_offset = (uint64_t)z * slice_pitch;
      if (slice_offset > UINTPTR_MAX - (uintptr_t)d->pSysMem) {
         ok = false;
         break;
      }
      const uint8_t *slice = (const uint8_t *)d->pSysMem + slice_offset;
      for (uint32_t y = 0; ok && y < rows;) {
         const uint32_t count =
            (rows - y < max_rows) ? rows - y : max_rows;
         const uint64_t source_offset = (uint64_t)y * d->SysMemPitch;
         if (source_offset > UINTPTR_MAX - (uintptr_t)slice) {
            ok = false;
            break;
         }
         const uint8_t *source = slice + source_offset;
         for (uint32_t row = 0; row < count; ++row)
            memcpy(packed + (size_t)row * row_bytes,
                   source + (size_t)row * d->SysMemPitch, row_bytes);

         const uint64_t top64 = (uint64_t)y * block_height;
         const uint64_t bottom64 =
            (uint64_t)((y + count < rows) ? y + count : rows) * block_height;
         if (top64 >= height || bottom64 <= top64 ||
             bottom64 > UINT32_MAX)
            ok = false;
         else {
            const D3D11_BOX box = {
               0, (uint32_t)top64, z, width,
               (uint32_t)(bottom64 > height ? height : bottom64), z + 1u
            };
            const uint32_t bytes = count * row_bytes;
            ok = npt_dispatch_resource_update(
               npt_device_method_ring(dev), tex_id, subresource,
               row_bytes, bytes, &box, packed, bytes, bytes);
         }
         y += count;
      }
   }
   free(packed);
   return ok;
}

#define NPT_REGISTER_OVERRIDE_D3D11_DEVICE5(m, f) \
   NPT_REGISTER_OVERRIDE(id3d11device5, m, f)
#define NPT_REGISTER_OVERRIDE_D3D11_DEVICE4(m, f) \
   NPT_REGISTER_OVERRIDE(id3d11device4, m, f); \
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE5(m, f)
#define NPT_REGISTER_OVERRIDE_D3D11_DEVICE3(m, f) \
   NPT_REGISTER_OVERRIDE(id3d11device3, m, f); \
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE4(m, f)
#define NPT_REGISTER_OVERRIDE_D3D11_DEVICE2(m, f) \
   NPT_REGISTER_OVERRIDE(id3d11device2, m, f); \
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE3(m, f)
#define NPT_REGISTER_OVERRIDE_D3D11_DEVICE1(m, f) \
   NPT_REGISTER_OVERRIDE(id3d11device1, m, f); \
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE2(m, f)
#define NPT_REGISTER_OVERRIDE_D3D11_DEVICE(m, f) \
   NPT_REGISTER_OVERRIDE(id3d11device, m, f); \
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE1(m, f)

static const GUID *const device_tiers[] = {
   &NPT_IID_ID3D11Device,  &NPT_IID_ID3D11Device1, &NPT_IID_ID3D11Device2,
   &NPT_IID_ID3D11Device3, &NPT_IID_ID3D11Device4, &NPT_IID_ID3D11Device5,
   NULL,
};

static struct npt_d3d11_device_aux *
dev_aux(void *self)
{
   return ((struct npt_com_base *)self)->aux;
}

static HRESULT NPT_STDMETHODCALLTYPE
dev_CreateBuffer_override(void *self,
                          const D3D11_BUFFER_DESC *pDesc,
                          const D3D11_SUBRESOURCE_DATA *pInitialData,
                          ID3D11Buffer **ppBuffer)
{
   struct npt_device *dev = npt_com_self_device(self);
   uint64_t self_id = npt_com_self_id(self);

   /* Sync under multi-ring (cross-ring Create->Use race), async on
    * single-ring (FIFO ordering covers it).  NPT_PERF knob forces
    * sync for bisection. */
   ID3D11Buffer *raw = NULL;
   HRESULT hr;
   if (dev->multi_ring_enabled || NPT_PERF(NO_ASYNC_BUFFER_CREATE)) {
      hr = npt_call_ID3D11Device_CreateBuffer(
         npt_device_method_ring(dev), self_id, pDesc, NULL, &raw);
   } else {
      npt_async_ID3D11Device_CreateBuffer(
         npt_device_method_ring(dev), self_id, pDesc, NULL, &raw);
      hr = (HRESULT)0;  /* deferred-fatal: assume S_OK */
   }

   if (NPT_SUCCEEDED(hr) && raw && pInitialData && pInitialData->pSysMem &&
       pDesc && pDesc->ByteWidth > 0) {
      /* MSDN: SysMemPitch / SysMemSlicePitch ignored for buffers. */
      struct npt_ring *ring = npt_device_method_ring(dev);
      bool uploaded = true;
      const uint32_t bytes = pDesc->ByteWidth;
      if (pDesc->BindFlags & D3D11_BIND_CONSTANT_BUFFER &&
          bytes > (64u << 20)) {
         uploaded = false;
      } else if (bytes <= (64u << 20)) {
         uploaded = npt_dispatch_resource_update(ring, (uint64_t)(uintptr_t)raw,
                                                  /*subresource=*/0,
                                                  /*row_pitch=*/0,
                                                  /*depth_pitch=*/0,
                                                  /*box=*/NULL,
                                                  pInitialData->pSysMem,
                                                  bytes, bytes);
      } else {
         for (uint32_t offset = 0; uploaded && offset < bytes;) {
            const uint32_t count =
               (bytes - offset < NPT_INITIAL_UPDATE_CHUNK_BYTES)
               ? bytes - offset : NPT_INITIAL_UPDATE_CHUNK_BYTES;
            const D3D11_BOX box = { offset, 0, 0, offset + count, 1, 1 };
            uploaded = npt_dispatch_resource_update(
               ring, (uint64_t)(uintptr_t)raw, 0, 0, 0, &box,
               (const uint8_t *)pInitialData->pSysMem + offset, count, count);
            offset += count;
         }
      }
      if (!uploaded)
         npt_initial_upload_failed(dev, ring);
   }

   if (NPT_SUCCEEDED(hr) && raw) {
      struct npt_d3d11_buffer *b = (struct npt_d3d11_buffer *)
         npt_com_get_or_wrap_or_release(dev, &NPT_IID_ID3D11Buffer,
                                        (uint64_t)(uintptr_t)raw,
                                        (struct npt_com_base *)self);
      if (b && pDesc) {
         npt_d3d11_buffer_set_byte_width(b, pDesc->ByteWidth);
         npt_d3d11_buffer_set_bind_flags(b, pDesc->BindFlags);
      }
      if (ppBuffer) {
         *ppBuffer = (ID3D11Buffer *)b;
      } else if (b) {
         /* Validation-only call: drop the caller-owned pub_ref. */
         npt_com_default_release(b);
      }
   } else if (ppBuffer) {
      *ppBuffer = NULL;
   }
   return hr;
}

/* Upload pSysMem via RESOURCE_UPDATE post-Create (the protocol can't
 * serialize unsized void*).  Subresource count = MipLevels * ArraySize.
 *
 * D3D defines SysMemSlicePitch for 3D only and SysMemPitch for 2D and 3D
 * only; outside its resource type each field is free to hold stale
 * garbage, so 1D ignores both and sizes one tight row from the format.
 * The ring reservation charges the full pitch for every row -- the
 * host may consume that much -- but the copy stops at the extent D3D
 * guarantees readable in pSysMem, (rows-1)*pitch + RowSizeInBytes per
 * slice.  Planar formats copy the full footprint, which MSDN documents
 * as rowPitch * subresource_rows with no final-row discount.  The
 * app's pitches already account for the format, so no block-size
 * scaling of the pitch is needed -- only the row count, since one row
 * of memory covers four texel rows under block compression. */
static bool
npt_upload_texture_initial_data(struct npt_device *dev,
                                uint64_t tex_id,
                                const D3D11_SUBRESOURCE_DATA *init,
                                uint32_t num_subresources,
                                uint32_t width, uint32_t height, uint32_t depth,
                                uint32_t mip_levels, DXGI_FORMAT format)
{
   if (!init || !num_subresources)
      return true;
   if (!mip_levels || mip_levels > 32u || !width || !height || !depth)
      return false;
   bool ok = true;
   for (uint32_t sub = 0; sub < num_subresources; sub++) {
      const D3D11_SUBRESOURCE_DATA *d = &init[sub];
      if (!d->pSysMem)
         return false;
      const uint32_t mip = sub % mip_levels;
      const uint32_t mw = width  > (1u << mip) ? (width  >> mip) : 1u;
      const uint32_t mh = height > (1u << mip) ? (height >> mip) : 1u;
      const uint32_t md = depth  > (1u << mip) ? (depth  >> mip) : 1u;
      const uint32_t rows = npt_dxgi_format_subresource_rows(format, mh);
      const uint32_t last_row = npt_dxgi_format_row_bytes(format, mw);
      const bool tight = last_row &&
         rows == npt_dxgi_format_block_rows(format, mh);
      /* Strides describe this mip, not its base-level dimensions. A
       * one-row mip and a one-slice mip need no following stride. */
      const uint32_t row_pitch = rows > 1u ? d->SysMemPitch : last_row;
      if (!rows || !row_pitch || row_pitch < last_row)
         return false;
      const uint64_t slice = (uint64_t)row_pitch * rows;
      const uint64_t slice_copy = tight
         ? (uint64_t)row_pitch * (rows - 1u) + last_row : slice;
      const uint64_t strides = md > 1u
         ? (uint64_t)d->SysMemSlicePitch * (md - 1u) : 0;
      if ((md > 1u && d->SysMemSlicePitch < slice_copy) ||
          strides > UINT64_MAX - slice)
         return false;
      const uint64_t full = strides + slice;
      const uint64_t copy = strides + slice_copy;
      if (!copy || copy > SIZE_MAX ||
          copy - 1u > UINTPTR_MAX - (uintptr_t)d->pSysMem)
         return false;
      if (full > (uint64_t)(64u << 20)) {
         /* A plane is a single modern resource layout; its chroma plane
          * cannot be addressed by a D3D11_BOX row split.  D3D9/10 do not
          * expose these formats, so keep the modern bounded path explicit. */
         const bool planar =
            npt_dxgi_format_subresource_rows(format, mh) !=
            npt_dxgi_format_block_rows(format, mh);
         D3D11_SUBRESOURCE_DATA normalized = *d;
         normalized.SysMemPitch = row_pitch;
         if (planar || !npt_upload_texture_initial_split(
               dev, tex_id, sub, &normalized, mw, mh, md, rows, last_row,
               npt_dxgi_format_block_height(format))) {
            ok = false;
         }
         continue;
      }
      if (!npt_dispatch_resource_update(npt_device_method_ring(dev), tex_id, sub,
                                        row_pitch, md > 1u ? d->SysMemSlicePitch : 0,
                                        /*box=*/NULL,
                                        d->pSysMem, (uint32_t)full,
                                        (uint32_t)copy))
         ok = false;
   }
   return ok;
}

/* Shared post-Create for CreateTexture{1,2,3}D{,1}; only the desc-fill
 * differs.  out_resource=NULL = MSDN validation-only path. */
static void
finish_create_texture(void *self,
                      const GUID *iid, void *raw,
                      const D3D11_SUBRESOURCE_DATA *pInitialData,
                      const struct npt_d3d11_texture_desc *desc,
                      void **out_resource)
{
   if (!raw) {
      if (out_resource) *out_resource = NULL;
      return;
   }
   struct npt_device *dev = npt_com_self_device(self);
   const uint64_t raw_id  = (uint64_t)(uintptr_t)raw;

   if (pInitialData) {
      const uint32_t mips = desc->mip_levels ? desc->mip_levels :
         npt_full_mip_levels(desc->width, desc->height, desc->depth);
      const uint32_t arr  = desc->array_size ? desc->array_size : 1u;
      if (!npt_upload_texture_initial_data(dev, raw_id,
                                           pInitialData, mips * arr,
                                           desc->width, desc->height, desc->depth,
                                           mips, desc->format))
         npt_initial_upload_failed(dev, npt_device_method_ring(dev));
   }

   struct npt_d3d11_texture *t = (struct npt_d3d11_texture *)
      npt_com_get_or_wrap_or_release(dev, iid, raw_id,
                                     (struct npt_com_base *)self);
   if (t && npt_d3d11_texture_cast(t))
      npt_d3d11_texture_set_desc(t, desc);
   if (out_resource) {
      *out_resource = t;
   } else if (t) {
      /* Validation-only call: drop the caller-owned pub_ref. */
      npt_com_default_release(t);
   }
}

static HRESULT NPT_STDMETHODCALLTYPE
dev_CreateTexture1D_override(void *self,
                              const D3D11_TEXTURE1D_DESC *pDesc,
                              const D3D11_SUBRESOURCE_DATA *pInitialData,
                              ID3D11Texture1D **ppTexture1D)
{
   struct npt_device *dev = npt_com_self_device(self);
   uint64_t self_id = npt_com_self_id(self);

   /* Sync/async policy: see dev_CreateBuffer_override. */
   ID3D11Texture1D *raw = NULL;
   HRESULT hr;
   if (dev->multi_ring_enabled || NPT_PERF(NO_ASYNC_TEXTURE_CREATE)) {
      hr = npt_call_ID3D11Device_CreateTexture1D(
         npt_device_method_ring(dev), self_id, pDesc, NULL, &raw);
   } else {
      npt_async_ID3D11Device_CreateTexture1D(
         npt_device_method_ring(dev), self_id, pDesc, NULL, &raw);
      hr = (HRESULT)0;
   }

   if (NPT_SUCCEEDED(hr) && raw && pDesc) {
      const struct npt_d3d11_texture_desc d = {
         .width = pDesc->Width,
         .height = 1u,
         .depth = 1u,
         .mip_levels = pDesc->MipLevels,
         .array_size = pDesc->ArraySize,
         .format = pDesc->Format,
         .sample_count = 1u,
         .sample_quality = 0u,
         .usage = pDesc->Usage,
         .bind_flags = pDesc->BindFlags,
         .cpu_access_flags = pDesc->CPUAccessFlags,
         .misc_flags = pDesc->MiscFlags,
         .texture_layout = (D3D11_TEXTURE_LAYOUT)0,
      };
      finish_create_texture(self, &NPT_IID_ID3D11Texture1D, raw,
                            pInitialData, &d, (void **)ppTexture1D);
   } else if (ppTexture1D) {
      *ppTexture1D = NULL;
   }
   return hr;
}

static HRESULT NPT_STDMETHODCALLTYPE
dev_CreateTexture2D_override(void *self,
                              const D3D11_TEXTURE2D_DESC *pDesc,
                              const D3D11_SUBRESOURCE_DATA *pInitialData,
                              ID3D11Texture2D **ppTexture2D)
{
   struct npt_device *dev = npt_com_self_device(self);
   uint64_t self_id = npt_com_self_id(self);

   /* Sync/async policy: see dev_CreateBuffer_override. */
   ID3D11Texture2D *raw = NULL;
   HRESULT hr;
   if (dev->multi_ring_enabled || NPT_PERF(NO_ASYNC_TEXTURE_CREATE)) {
      hr = npt_call_ID3D11Device_CreateTexture2D(
         npt_device_method_ring(dev), self_id, pDesc, NULL, &raw);
   } else {
      npt_async_ID3D11Device_CreateTexture2D(
         npt_device_method_ring(dev), self_id, pDesc, NULL, &raw);
      hr = (HRESULT)0;
   }

   if (NPT_SUCCEEDED(hr) && raw && pDesc) {
      const struct npt_d3d11_texture_desc d = {
         .width = pDesc->Width,
         .height = pDesc->Height,
         .depth = 1u,
         .mip_levels = pDesc->MipLevels,
         .array_size = pDesc->ArraySize,
         .format = pDesc->Format,
         .sample_count = pDesc->SampleDesc.Count,
         .sample_quality = pDesc->SampleDesc.Quality,
         .usage = pDesc->Usage,
         .bind_flags = pDesc->BindFlags,
         .cpu_access_flags = pDesc->CPUAccessFlags,
         .misc_flags = pDesc->MiscFlags,
         .texture_layout = (D3D11_TEXTURE_LAYOUT)0,
      };
      finish_create_texture(self, &NPT_IID_ID3D11Texture2D, raw,
                            pInitialData, &d, (void **)ppTexture2D);
   } else if (ppTexture2D) {
      *ppTexture2D = NULL;
   }
   return hr;
}

static HRESULT NPT_STDMETHODCALLTYPE
dev_CreateTexture3D_override(void *self,
                              const D3D11_TEXTURE3D_DESC *pDesc,
                              const D3D11_SUBRESOURCE_DATA *pInitialData,
                              ID3D11Texture3D **ppTexture3D)
{
   struct npt_device *dev = npt_com_self_device(self);
   uint64_t self_id = npt_com_self_id(self);

   /* Sync/async policy: see dev_CreateBuffer_override. */
   ID3D11Texture3D *raw = NULL;
   HRESULT hr;
   if (dev->multi_ring_enabled || NPT_PERF(NO_ASYNC_TEXTURE_CREATE)) {
      hr = npt_call_ID3D11Device_CreateTexture3D(
         npt_device_method_ring(dev), self_id, pDesc, NULL, &raw);
   } else {
      npt_async_ID3D11Device_CreateTexture3D(
         npt_device_method_ring(dev), self_id, pDesc, NULL, &raw);
      hr = (HRESULT)0;
   }

   if (NPT_SUCCEEDED(hr) && raw && pDesc) {
      const struct npt_d3d11_texture_desc d = {
         .width = pDesc->Width,
         .height = pDesc->Height,
         .depth = pDesc->Depth,
         .mip_levels = pDesc->MipLevels,
         .array_size = 1u,
         .format = pDesc->Format,
         .sample_count = 1u,
         .sample_quality = 0u,
         .usage = pDesc->Usage,
         .bind_flags = pDesc->BindFlags,
         .cpu_access_flags = pDesc->CPUAccessFlags,
         .misc_flags = pDesc->MiscFlags,
         .texture_layout = (D3D11_TEXTURE_LAYOUT)0,
      };
      finish_create_texture(self, &NPT_IID_ID3D11Texture3D, raw,
                            pInitialData, &d, (void **)ppTexture3D);
   } else if (ppTexture3D) {
      *ppTexture3D = NULL;
   }
   return hr;
}

/* DESC1 = DESC + TextureLayout; otherwise identical to the non-1 path. */
static HRESULT NPT_STDMETHODCALLTYPE
dev3_CreateTexture2D1_override(void *self,
                               const D3D11_TEXTURE2D_DESC1 *pDesc1,
                               const D3D11_SUBRESOURCE_DATA *pInitialData,
                               ID3D11Texture2D1 **ppTexture2D)
{
   struct npt_device *dev = npt_com_self_device(self);
   uint64_t self_id = npt_com_self_id(self);

   ID3D11Texture2D1 *raw = NULL;
   HRESULT hr;
   if (dev->multi_ring_enabled || NPT_PERF(NO_ASYNC_TEXTURE_CREATE)) {
      hr = npt_call_ID3D11Device3_CreateTexture2D1(
         npt_device_method_ring(dev), self_id, pDesc1, NULL, &raw);
   } else {
      npt_async_ID3D11Device3_CreateTexture2D1(
         npt_device_method_ring(dev), self_id, pDesc1, NULL, &raw);
      hr = (HRESULT)0;
   }

   if (NPT_SUCCEEDED(hr) && raw && pDesc1) {
      const struct npt_d3d11_texture_desc d = {
         .width = pDesc1->Width,
         .height = pDesc1->Height,
         .depth = 1u,
         .mip_levels = pDesc1->MipLevels,
         .array_size = pDesc1->ArraySize,
         .format = pDesc1->Format,
         .sample_count = pDesc1->SampleDesc.Count,
         .sample_quality = pDesc1->SampleDesc.Quality,
         .usage = pDesc1->Usage,
         .bind_flags = pDesc1->BindFlags,
         .cpu_access_flags = pDesc1->CPUAccessFlags,
         .misc_flags = pDesc1->MiscFlags,
         .texture_layout = pDesc1->TextureLayout,
      };
      finish_create_texture(self, &NPT_IID_ID3D11Texture2D1, raw,
                            pInitialData, &d, (void **)ppTexture2D);
   } else if (ppTexture2D) {
      *ppTexture2D = NULL;
   }
   return hr;
}

static HRESULT NPT_STDMETHODCALLTYPE
dev3_CreateTexture3D1_override(void *self,
                               const D3D11_TEXTURE3D_DESC1 *pDesc1,
                               const D3D11_SUBRESOURCE_DATA *pInitialData,
                               ID3D11Texture3D1 **ppTexture3D)
{
   struct npt_device *dev = npt_com_self_device(self);
   uint64_t self_id = npt_com_self_id(self);

   ID3D11Texture3D1 *raw = NULL;
   HRESULT hr;
   if (dev->multi_ring_enabled || NPT_PERF(NO_ASYNC_TEXTURE_CREATE)) {
      hr = npt_call_ID3D11Device3_CreateTexture3D1(
         npt_device_method_ring(dev), self_id, pDesc1, NULL, &raw);
   } else {
      npt_async_ID3D11Device3_CreateTexture3D1(
         npt_device_method_ring(dev), self_id, pDesc1, NULL, &raw);
      hr = (HRESULT)0;
   }

   if (NPT_SUCCEEDED(hr) && raw && pDesc1) {
      const struct npt_d3d11_texture_desc d = {
         .width = pDesc1->Width,
         .height = pDesc1->Height,
         .depth = pDesc1->Depth,
         .mip_levels = pDesc1->MipLevels,
         .array_size = 1u,
         .format = pDesc1->Format,
         .sample_count = 1u,
         .sample_quality = 0u,
         .usage = pDesc1->Usage,
         .bind_flags = pDesc1->BindFlags,
         .cpu_access_flags = pDesc1->CPUAccessFlags,
         .misc_flags = pDesc1->MiscFlags,
         .texture_layout = pDesc1->TextureLayout,
      };
      finish_create_texture(self, &NPT_IID_ID3D11Texture3D1, raw,
                            pInitialData, &d, (void **)ppTexture3D);
   } else if (ppTexture3D) {
      *ppTexture3D = NULL;
   }
   return hr;
}

static HRESULT NPT_STDMETHODCALLTYPE
dev4_RegisterDeviceRemovedEvent_override(void *self, HANDLE hEvent, DWORD *pdwCookie)
{
   (void)self; (void)hEvent;
   if (pdwCookie)
      *pdwCookie = NPT_DEVICE_REMOVED_COOKIE;
   return NPT_S_OK;
}

static void NPT_STDMETHODCALLTYPE
dev4_UnregisterDeviceRemoved_override(void *self, DWORD dwCookie)
{
   (void)self; (void)dwCookie;
}

/*
 * GetImmediateContext{,1,2,3} cache: avoids an async Create + wrap +
 * COM_RELEASE per call.  Tier-aware: narrowing returns the cached
 * pointer; widening evicts and refetches with the right-tier vtbl.
 * Cache holds one pub_ref; aux_destroy drops it.
 */
static void
npt_d3d11_device_aux_destroy(void *aux_raw)
{
   struct npt_d3d11_device_aux *aux = aux_raw;
   void *cached = aux->cached_imm_ctx;
   aux->cached_imm_ctx = NULL;
   mtx_destroy(&aux->cache_mutex);
   if (cached)
      npt_com_default_release(cached);
   free(aux);
}

static void
npt_d3d11_device_aux_init(struct npt_com_base *com,
                          struct npt_device *dev, uint64_t host_id)
{
   struct npt_d3d11_device_aux *aux = com->aux;
   com->aux_destroy = npt_d3d11_device_aux_destroy;
   mtx_init(&aux->cache_mutex, mtx_plain);
   aux->cached_imm_ctx = NULL;
   aux->cached_imm_ctx_tier = 0;
   (void)dev; (void)host_id;
}

/* All four tier signatures are ABI-identical (just **out type). */
typedef void (NPT_STDMETHODCALLTYPE *dev_ctx_thunk_t)(void *self, void **out);

static const dev_ctx_thunk_t dev_default_ctx_thunks[4] = {
   (dev_ctx_thunk_t)npt_id3d11device_default_GetImmediateContext,
   (dev_ctx_thunk_t)npt_id3d11device1_default_GetImmediateContext1,
   (dev_ctx_thunk_t)npt_id3d11device2_default_GetImmediateContext2,
   (dev_ctx_thunk_t)npt_id3d11device3_default_GetImmediateContext3,
};

static void
dev_get_immediate_context_cached(void *self, int tier, void **ppOut)
{
   if (!ppOut)
      return;
   *ppOut = NULL;

   struct npt_d3d11_device_aux *aux = dev_aux(self);
   if (!aux) {
      /* OOM at ctor: bypass cache and use the tier's default thunk. */
      dev_default_ctx_thunks[tier](self, ppOut);
      return;
   }

   mtx_lock(&aux->cache_mutex);
   if (aux->cached_imm_ctx && aux->cached_imm_ctx_tier >= tier) {
      void *cached = aux->cached_imm_ctx;
      npt_com_default_addref(cached);
      mtx_unlock(&aux->cache_mutex);
      *ppOut = cached;
      return;
   }
   mtx_unlock(&aux->cache_mutex);

   /* Slow path: fetch the requested tier (pub_ref=1 caller hold). */
   void *fresh = NULL;
   dev_default_ctx_thunks[tier](self, &fresh);
   if (!fresh)
      return;

   /* Either reuse a winner peer's >= tier wrapper or publish ours
    * (which evicts any narrower cached wrapper). */
   void *result = NULL;
   void *evicted = NULL;
   mtx_lock(&aux->cache_mutex);
   if (aux->cached_imm_ctx && aux->cached_imm_ctx_tier >= tier) {
      result = aux->cached_imm_ctx;
      npt_com_default_addref(result);
   } else {
      evicted = aux->cached_imm_ctx;
      aux->cached_imm_ctx = fresh;
      aux->cached_imm_ctx_tier = tier;
      npt_com_default_addref(fresh); /* cache's hold */
      result = fresh;
      fresh = NULL;
   }
   mtx_unlock(&aux->cache_mutex);

   if (evicted)
      npt_com_default_release(evicted);
   if (fresh)
      npt_com_default_release(fresh);

   *ppOut = result;
}

static void NPT_STDMETHODCALLTYPE
dev_GetImmediateContext_override(void *self,
                                 ID3D11DeviceContext **ppImmediateContext)
{
   dev_get_immediate_context_cached(self, 0, (void **)ppImmediateContext);
}

static void NPT_STDMETHODCALLTYPE
dev1_GetImmediateContext1_override(void *self,
                                   ID3D11DeviceContext1 **ppImmediateContext)
{
   dev_get_immediate_context_cached(self, 1, (void **)ppImmediateContext);
}

static void NPT_STDMETHODCALLTYPE
dev2_GetImmediateContext2_override(void *self,
                                   ID3D11DeviceContext2 **ppImmediateContext)
{
   dev_get_immediate_context_cached(self, 2, (void **)ppImmediateContext);
}

static void NPT_STDMETHODCALLTYPE
dev3_GetImmediateContext3_override(void *self,
                                   ID3D11DeviceContext3 **ppImmediateContext)
{
   dev_get_immediate_context_cached(self, 3, (void **)ppImmediateContext);
}

/* This is a query, not an asynchronous state setter. The generated default
 * discards the host HRESULT and returns S_OK, masking device removal. */
static HRESULT NPT_STDMETHODCALLTYPE
dev_GetDeviceRemovedReason_override(void *self)
{
   struct npt_device *dev = npt_com_self_device(self);
   struct npt_ring *ring = npt_com_self_ring(self);
   const HRESULT removed = (HRESULT)0x887A0005; /* DXGI_ERROR_DEVICE_REMOVED */
   if (!dev || !npt_ring_is_healthy(dev->ring) || !npt_ring_is_healthy(ring))
      return removed;
   HRESULT hr = npt_call_ID3D11Device_GetDeviceRemovedReason(ring,
                                                            npt_com_self_id(self));
   return npt_ring_is_healthy(dev->ring) && npt_ring_is_healthy(ring) ? hr : removed;
}

void
npt_overrides_d3d11_device_init(void)
{
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE(GetDeviceRemovedReason,
                                      dev_GetDeviceRemovedReason_override);
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE (CreateBuffer,
                                       dev_CreateBuffer_override);
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE (CreateTexture1D,
                                       dev_CreateTexture1D_override);
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE (CreateTexture2D,
                                       dev_CreateTexture2D_override);
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE (CreateTexture3D,
                                       dev_CreateTexture3D_override);
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE (GetImmediateContext,
                                       dev_GetImmediateContext_override);
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE1(GetImmediateContext1,
                                       dev1_GetImmediateContext1_override);
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE2(GetImmediateContext2,
                                       dev2_GetImmediateContext2_override);
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE3(CreateTexture2D1,
                                       dev3_CreateTexture2D1_override);
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE3(CreateTexture3D1,
                                       dev3_CreateTexture3D1_override);
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE3(GetImmediateContext3,
                                       dev3_GetImmediateContext3_override);
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE4(RegisterDeviceRemovedEvent,
                                       dev4_RegisterDeviceRemovedEvent_override);
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE4(UnregisterDeviceRemoved,
                                       dev4_UnregisterDeviceRemoved_override);

   npt_com_register_family(device_tiers,
                           sizeof(struct npt_d3d11_device_aux),
                           npt_d3d11_device_aux_init);
}
