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
      npt_dispatch_resource_update(npt_device_method_ring(dev),
                                   (uint64_t)(uintptr_t)raw,
                                   /*subresource=*/0,
                                   /*row_pitch=*/0,
                                   /*depth_pitch=*/0,
                                   /*box=*/NULL,
                                   pInitialData->pSysMem,
                                   pDesc->ByteWidth, pDesc->ByteWidth);
   }

   if (NPT_SUCCEEDED(hr) && raw) {
      struct npt_d3d11_buffer *b = (struct npt_d3d11_buffer *)
         npt_com_get_or_wrap_or_release(dev, &NPT_IID_ID3D11Buffer,
                                        (uint64_t)(uintptr_t)raw,
                                        (struct npt_com_base *)self);
      if (b && pDesc)
         npt_d3d11_buffer_set_byte_width(b, pDesc->ByteWidth);
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
static void
npt_upload_texture_initial_data(struct npt_device *dev,
                                uint64_t tex_id,
                                const D3D11_SUBRESOURCE_DATA *init,
                                uint32_t num_subresources,
                                uint32_t width, uint32_t height, uint32_t depth,
                                uint32_t mip_levels, DXGI_FORMAT format)
{
   if (!init || !num_subresources)
      return;
   for (uint32_t sub = 0; sub < num_subresources; sub++) {
      const D3D11_SUBRESOURCE_DATA *d = &init[sub];
      if (!d->pSysMem)
         continue;
      const uint32_t mip = sub % mip_levels;
      const uint32_t mw = width  > (1u << mip) ? (width  >> mip) : 1u;
      const uint32_t mh = height > (1u << mip) ? (height >> mip) : 1u;
      const uint32_t md = depth  > (1u << mip) ? (depth  >> mip) : 1u;
      const uint32_t rows = npt_dxgi_format_subresource_rows(format, mh);
      const uint32_t last_row = npt_dxgi_format_row_bytes(format, mw);
      const bool tight = last_row &&
         rows == npt_dxgi_format_block_rows(format, mh);
      uint64_t full = 0, copy = 0;
      if (depth > 1u) {
         if (!d->SysMemSlicePitch)
            continue;
         const uint64_t strides =
            (uint64_t)d->SysMemSlicePitch * (md - 1u);
         const uint64_t slice = (uint64_t)d->SysMemPitch * rows;
         full = strides + (slice ? slice : d->SysMemSlicePitch);
         copy = tight && d->SysMemPitch
            ? strides + (uint64_t)d->SysMemPitch * (rows - 1u) + last_row
            : full;
      } else if (height > 1u) {
         if (!d->SysMemPitch)
            continue;
         full = (uint64_t)d->SysMemPitch * rows;
         copy = tight
            ? (uint64_t)d->SysMemPitch * (rows - 1u) + last_row
            : full;
      } else {
         /* A single row: neither pitch carries information D3D
          * promises. */
         full = copy = last_row;
      }
      if (copy > full)
         copy = full;
      if (full == 0 || copy == 0 || full > (uint64_t)(64u << 20))
         continue;
      npt_dispatch_resource_update(npt_device_method_ring(dev), tex_id, sub,
                                   d->SysMemPitch, d->SysMemSlicePitch,
                                   /*box=*/NULL,
                                   d->pSysMem, (uint32_t)full,
                                   (uint32_t)copy);
   }
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
      const uint32_t mips = desc->mip_levels ? desc->mip_levels : 1u;
      const uint32_t arr  = desc->array_size ? desc->array_size : 1u;
      npt_upload_texture_initial_data(dev, raw_id,
                                      pInitialData, mips * arr,
                                      desc->width, desc->height, desc->depth,
                                      mips, desc->format);
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

void
npt_overrides_d3d11_device_init(void)
{
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
