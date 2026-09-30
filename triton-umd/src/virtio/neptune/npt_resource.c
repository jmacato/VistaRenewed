/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 */

#include "npt_com.h"
#include "npt_device.h"
#include "npt_renderer.h"
#include "npt_resource.h"
#include "npt_ring.h"
#include "npt_shmem_pool.h"

#include "neptune-protocol/npt_protocol_client_id3d11resource.h"
#include "neptune-protocol/npt_protocol_client_id3d11texture1d.h"
#include "neptune-protocol/npt_protocol_client_id3d11texture2d.h"
#include "neptune-protocol/npt_protocol_client_id3d11texture3d.h"

#include "util/list.h"

#include <stdlib.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#endif

/* ==========================================================================
 * Map shadow-staging rename ring
 * ========================================================================== */

/* Async shmem destroy worker.  Each slot teardown is munmap + virtgpu
 * gem_close (two Wine syscalls); funneling them through a singleton
 * thread keeps the Release path from blocking on kernel ioctl
 * turnaround.  Correctness: COM_RELEASE was queued in npt_com_destroy
 * before aux_destroy, and the host blob view is independent of the
 * guest mmap, so destroying out of order is safe.  Lazy-init, detached
 * worker; process exit doesn't drain — the kernel reclaims leftover
 * GEM handles + munmaps. */
struct shmem_reap_entry {
   struct npt_renderer *renderer;
   struct npt_renderer_shmem *shmem;
   struct list_head head;
};

static struct {
   _Atomic int state;            /* see NPT_CALL_ONCE: 0=uninit, 1=in-progress, 2=ready */
   mtx_t mutex;
   cnd_t cond;
   struct list_head queue;
} g_shmem_reaper;

#if defined(_WIN32)
static DWORD WINAPI shmem_reaper_main(LPVOID arg)
#else
static void *shmem_reaper_main(void *arg)
#endif
{
   (void)arg;
   for (;;) {
      mtx_lock(&g_shmem_reaper.mutex);
      while (list_is_empty(&g_shmem_reaper.queue))
         cnd_wait(&g_shmem_reaper.cond, &g_shmem_reaper.mutex);
      struct shmem_reap_entry *e =
         list_first_entry(&g_shmem_reaper.queue,
                          struct shmem_reap_entry, head);
      list_del(&e->head);
      mtx_unlock(&g_shmem_reaper.mutex);

      e->renderer->shmem_ops.destroy(e->renderer, e->shmem);
      free(e);
   }
#if defined(_WIN32)
   return 0;
#else
   return NULL;
#endif
}

static void
shmem_reaper_init_impl(void)
{
   mtx_init(&g_shmem_reaper.mutex, mtx_plain);
   cnd_init(&g_shmem_reaper.cond);
   list_inithead(&g_shmem_reaper.queue);
#if defined(_WIN32)
   HANDLE h = CreateThread(NULL, 0, shmem_reaper_main, NULL, 0, NULL);
   if (h) CloseHandle(h);
#else
   pthread_t t;
   if (pthread_create(&t, NULL, shmem_reaper_main, NULL) == 0)
      pthread_detach(t);
#endif
}

static void
shmem_reaper_lazy_init(void)
{
   NPT_CALL_ONCE(g_shmem_reaper.state, shmem_reaper_init_impl());
}

static void
shmem_reaper_unref(struct npt_renderer *renderer,
                   struct npt_renderer_shmem *shmem)
{
   if (!shmem || !npt_refcount_dec(&shmem->refcount))
      return;

   shmem_reaper_lazy_init();

   struct shmem_reap_entry *e = malloc(sizeof(*e));
   if (!e) {
      /* Allocation failure: synchronous fallback (leaks would compound). */
      renderer->shmem_ops.destroy(renderer, shmem);
      return;
   }
   e->renderer = renderer;
   e->shmem    = shmem;

   mtx_lock(&g_shmem_reaper.mutex);
   list_addtail(&e->head, &g_shmem_reaper.queue);
   cnd_signal(&g_shmem_reaper.cond);
   mtx_unlock(&g_shmem_reaper.mutex);
}

void
npt_d3d_map_ring_init(struct npt_d3d_map_ring *r, struct npt_com_base *com)
{
   memset(r, 0, sizeof(*r));
   r->com = com;
}

static bool
alloc_slot_locked(struct npt_d3d_map_ring *r, uint32_t idx)
{
   if (idx >= NPT_D3D_MAP_SLOT_MAX)
      return false;
   if (r->slots[idx].shmem)
      return true;

   struct npt_device *dev = r->com->base.device;
   struct npt_renderer *rndr = dev->renderer;
   /* Sub-allocate from the per-device map pool.  npt_shmem_pool_alloc
    * grows the pool's shmem when there isn't room; old shmem stays
    * alive via prior slot refs. */
   size_t off = 0;
   bool fresh = false;
   struct npt_renderer_shmem *s =
      npt_shmem_pool_alloc(rndr, &dev->map_pool, r->aligned_slot_size,
                           &off, &fresh);
   if (!s) {
      npt_log("d3d_map_ring: pool_alloc slot %u (%u bytes) failed",
              idx, r->aligned_slot_size);
      return false;
   }
   /* Pool growth races the ring thread; a fresh pool shmem must be
    * roundtripped before any ring command names its res_id.  Map cmds
    * are synchronous and the ring layer already roundtrips for sync
    * calls, so the first slot of a freshly-grown pool is naturally
    * registered in time.  Pure-async lifecycles would need explicit
    * handling. */
   (void)fresh;
   r->slots[idx].shmem = s;
   r->slots[idx].shmem_res_id = s->res_id;
   r->slots[idx].offset = (uint32_t)off;
   r->slots[idx].pending_seqno = 0;
   r->slots[idx].pending_ring = NULL;
   r->slots[idx].in_flight = false;
   if (idx + 1u > r->active_count)
      r->active_count = idx + 1u;
   return true;
}

bool
npt_d3d_map_ring_alloc_shmem(struct npt_d3d_map_ring *r,
                             uint32_t aligned_slot_size)
{
   if (!aligned_slot_size)
      return false;
   if (aligned_slot_size > r->aligned_slot_size && r->active_count) {
      if (r->is_mapped)
         return false;
      /* A larger host pitch can require a larger slot. Retire every queued
       * rename upload before releasing its staging storage. */
      for (uint32_t i = 0; i < r->active_count; i++) {
         if (r->slots[i].in_flight) {
            struct npt_ring *ring = r->slots[i].pending_ring
               ? r->slots[i].pending_ring : r->com->base.device->ring;
            if (npt_ring_wait_seqno(ring, r->slots[i].pending_seqno) == UINT32_MAX)
               return false;
         }
      }
      npt_d3d_map_ring_fini(r);
   }
   if (aligned_slot_size > r->aligned_slot_size)
      r->aligned_slot_size = aligned_slot_size;
   for (uint32_t i = 0; i < NPT_D3D_MAP_SLOT_INIT; i++) {
      if (!alloc_slot_locked(r, i))
         return false;
   }
   return true;
}

void
npt_d3d_map_ring_fini(struct npt_d3d_map_ring *r)
{
   if (!r->active_count)
      return;
   struct npt_renderer *rndr = r->com->base.device->renderer;

   /* No pending_seqno wait: rotate_slot is the only consumer and the
    * ring is going away.  Host-side ordering is covered by COM_RELEASE
    * (queued in npt_com_destroy before this fini runs). */
   for (uint32_t i = 0; i < r->active_count; i++) {
      if (r->slots[i].shmem) {
         shmem_reaper_unref(rndr, r->slots[i].shmem);
         r->slots[i].shmem = NULL;
         r->slots[i].shmem_res_id = 0;
      }
      r->slots[i].in_flight = false;
      r->slots[i].pending_ring = NULL;
   }
   r->active_count = 0;
   r->aligned_slot_size = 0;
   r->current_slot = 0;
}

uint32_t
npt_d3d_map_ring_rotate_slot(struct npt_d3d_map_ring *r)
{
   if (!r->aligned_slot_size)
      return 0;

   const uint32_t cnt = r->active_count;
   const uint32_t next = cnt ? (r->current_slot + 1u) % cnt : 0u;

   /* Fast path: the wrap-around slot is already free. */
   if (cnt && !r->slots[next].in_flight) {
      r->current_slot = next;
      return next;
   }

   /* Grow path: bring up a fresh slot at active_count. */
   if (cnt < NPT_D3D_MAP_SLOT_MAX && alloc_slot_locked(r, cnt)) {
      r->current_slot = cnt;
      return cnt;
   }

   /* At cap (or grow failed): block on the wrap-around slot. */
   if (r->slots[next].in_flight) {
      struct npt_ring *ring = r->slots[next].pending_ring
         ? r->slots[next].pending_ring
         : r->com->base.device->ring;
      npt_ring_wait_seqno(ring, r->slots[next].pending_seqno);
      r->slots[next].in_flight = false;
      r->slots[next].pending_ring = NULL;
   }
   r->current_slot = next;
   return next;
}

void
npt_d3d_map_ring_mark_slot_submitted(struct npt_d3d_map_ring *r,
                                     uint32_t slot, uint32_t seqno,
                                     struct npt_ring *ring)
{
   if (slot >= NPT_D3D_MAP_SLOT_MAX)
      return;
   r->slots[slot].pending_seqno = seqno;
   r->slots[slot].pending_ring = ring;
   r->slots[slot].in_flight = true;
}

/* ==========================================================================
 * ID3D11Buffer wrapper
 * ========================================================================== */

static inline struct npt_d3d11_buffer_aux *
buf_aux(const struct npt_d3d11_buffer *b)
{
   return b ? ((struct npt_com_base *)b)->aux : NULL;
}

static void
npt_d3d11_buffer_aux_destroy(void *aux_raw)
{
   struct npt_d3d11_buffer_aux *aux = aux_raw;
   npt_d3d_map_ring_fini(&aux->map_ring);
   free(aux);
}

void
npt_d3d11_buffer_aux_init(struct npt_com_base *com,
                          struct npt_device *dev, uint64_t host_id)
{
   struct npt_d3d11_buffer_aux *aux = com->aux;
   aux->com = com;
   npt_d3d_map_ring_init(&aux->map_ring, com);
   com->aux_destroy = npt_d3d11_buffer_aux_destroy;
   (void)dev; (void)host_id;
}

struct npt_d3d11_buffer *
npt_d3d11_buffer_cast(void *resource)
{
   if (!resource) return NULL;
   struct npt_com_base *com = resource;
   if (com->aux_destroy != npt_d3d11_buffer_aux_destroy) return NULL;
   return (struct npt_d3d11_buffer *)resource;
}

void
npt_d3d11_buffer_set_byte_width(struct npt_d3d11_buffer *b, uint32_t bytes)
{
   struct npt_d3d11_buffer_aux *aux = buf_aux(b);
   if (aux) aux->byte_width = bytes;
}

uint32_t
npt_d3d11_buffer_get_byte_width(struct npt_d3d11_buffer *b)
{
   struct npt_d3d11_buffer_aux *aux = buf_aux(b);
   return aux ? aux->byte_width : 0;
}

void
npt_d3d11_buffer_set_bind_flags(struct npt_d3d11_buffer *b, uint32_t flags)
{
   struct npt_d3d11_buffer_aux *aux = buf_aux(b);
   if (aux) aux->bind_flags = flags;
}

uint32_t
npt_d3d11_buffer_get_bind_flags(struct npt_d3d11_buffer *b)
{
   const struct npt_d3d11_buffer_aux *aux = buf_aux(b);
   return aux ? aux->bind_flags : 0;
}

bool
npt_d3d11_buffer_ensure_map_shmem(struct npt_d3d11_buffer *b)
{
   struct npt_d3d11_buffer_aux *aux = buf_aux(b);
   if (!aux || !aux->byte_width) return false;
   if (aux->byte_width > UINT32_MAX - 63u) return false;
   const uint32_t slot_size = (aux->byte_width + 63u) & ~63u;
   return npt_d3d_map_ring_alloc_shmem(&aux->map_ring, slot_size);
}

uint32_t
npt_d3d11_buffer_get_map_shmem_res_id(struct npt_d3d11_buffer *b)
{
   struct npt_d3d11_buffer_aux *aux = buf_aux(b);
   return aux ? npt_d3d_map_ring_slot_res_id(&aux->map_ring, 0) : 0;
}

uint32_t
npt_d3d11_buffer_get_slot_shmem_res_id(struct npt_d3d11_buffer *b, uint32_t slot)
{
   struct npt_d3d11_buffer_aux *aux = buf_aux(b);
   return aux ? npt_d3d_map_ring_slot_res_id(&aux->map_ring, slot) : 0;
}

bool
npt_d3d11_buffer_get_is_mapped(struct npt_d3d11_buffer *b)
{
   struct npt_d3d11_buffer_aux *aux = buf_aux(b);
   return aux ? aux->map_ring.is_mapped : false;
}

void
npt_d3d11_buffer_set_is_mapped(struct npt_d3d11_buffer *b, bool mapped)
{
   struct npt_d3d11_buffer_aux *aux = buf_aux(b);
   if (aux) aux->map_ring.is_mapped = mapped;
}

uint32_t
npt_d3d11_buffer_slot_offset(const struct npt_d3d11_buffer *b, uint32_t slot)
{
   const struct npt_d3d11_buffer_aux *aux = buf_aux(b);
   return aux ? npt_d3d_map_ring_slot_offset(&aux->map_ring, slot) : 0;
}

void *
npt_d3d11_buffer_slot_ptr(const struct npt_d3d11_buffer *b, uint32_t slot)
{
   const struct npt_d3d11_buffer_aux *aux = buf_aux(b);
   return aux ? npt_d3d_map_ring_slot_ptr(&aux->map_ring, slot) : NULL;
}

uint32_t
npt_d3d11_buffer_get_current_slot(const struct npt_d3d11_buffer *b)
{
   const struct npt_d3d11_buffer_aux *aux = buf_aux(b);
   return aux ? aux->map_ring.current_slot : 0;
}

void
npt_d3d11_buffer_set_current_slot(struct npt_d3d11_buffer *b, uint32_t slot)
{
   struct npt_d3d11_buffer_aux *aux = buf_aux(b);
   if (aux && slot < NPT_D3D_MAP_SLOT_MAX) aux->map_ring.current_slot = slot;
}

uint32_t
npt_d3d11_buffer_rotate_slot(struct npt_d3d11_buffer *b)
{
   struct npt_d3d11_buffer_aux *aux = buf_aux(b);
   return aux ? npt_d3d_map_ring_rotate_slot(&aux->map_ring) : 0;
}

void
npt_d3d11_buffer_mark_slot_submitted(struct npt_d3d11_buffer *b,
                                     uint32_t slot, uint32_t seqno,
                                     struct npt_ring *ring)
{
   struct npt_d3d11_buffer_aux *aux = buf_aux(b);
   if (aux) npt_d3d_map_ring_mark_slot_submitted(&aux->map_ring, slot, seqno, ring);
}

void
npt_d3d11_buffer_set_last_map_access_flags(struct npt_d3d11_buffer *b, uint32_t flags)
{
   struct npt_d3d11_buffer_aux *aux = buf_aux(b);
   if (aux) aux->map_ring.last_map_access_flags = flags;
}

uint32_t
npt_d3d11_buffer_get_last_map_access_flags(const struct npt_d3d11_buffer *b)
{
   const struct npt_d3d11_buffer_aux *aux = buf_aux(b);
   return aux ? aux->map_ring.last_map_access_flags : 0;
}

/* ==========================================================================
 * ID3D11Texture{1,2,3}D wrapper
 * ========================================================================== */

static inline struct npt_d3d11_texture_aux *
tex_aux(const struct npt_d3d11_texture *t)
{
   return t ? ((struct npt_com_base *)t)->aux : NULL;
}

static void
npt_d3d11_texture_aux_destroy(void *aux_raw)
{
   struct npt_d3d11_texture_aux *aux = aux_raw;
   npt_d3d_map_ring_fini(&aux->map_ring);
   mtx_destroy(&aux->desc_mutex);
   free(aux);
}

void
npt_d3d11_texture_aux_init(struct npt_com_base *com,
                           struct npt_device *dev, uint64_t host_id)
{
   struct npt_d3d11_texture_aux *aux = com->aux;
   aux->com = com;
   mtx_init(&aux->desc_mutex, mtx_plain);
   atomic_init(&aux->desc_state, 0);
   npt_d3d_map_ring_init(&aux->map_ring, com);
   com->aux_destroy = npt_d3d11_texture_aux_destroy;
   aux->mip_levels = 1;
   aux->array_size = 1;
   aux->sample_count = 1;
   (void)dev; (void)host_id;
}

struct npt_d3d11_texture *
npt_d3d11_texture_cast(void *resource)
{
   if (!resource) return NULL;
   struct npt_com_base *com = resource;
   if (!com->aux) return NULL;
   /* QueryInterface tier aliases share their ancestor's aux and deliberately
    * have no destructor. Match the first owning ancestor, just as the COM
    * cache does, and never reinterpret another family's side storage. */
   for (struct npt_com_base *owner = com; owner; owner = owner->base.parent) {
      if (!owner->aux_destroy)
         continue;
      return owner->aux_destroy == npt_d3d11_texture_aux_destroy &&
         owner->aux == com->aux ? (struct npt_d3d11_texture *)resource : NULL;
   }
   return NULL;
}

uint32_t
npt_dxgi_format_bytes_per_pixel(DXGI_FORMAT fmt)
{
   switch ((int)fmt) {
   case DXGI_FORMAT_R32G32B32A32_TYPELESS:
   case DXGI_FORMAT_R32G32B32A32_FLOAT:
   case DXGI_FORMAT_R32G32B32A32_UINT:
   case DXGI_FORMAT_R32G32B32A32_SINT:
      return 16;
   case DXGI_FORMAT_R32G32B32_TYPELESS:
   case DXGI_FORMAT_R32G32B32_FLOAT:
   case DXGI_FORMAT_R32G32B32_UINT:
   case DXGI_FORMAT_R32G32B32_SINT:
      return 12;
   case DXGI_FORMAT_R16G16B16A16_TYPELESS:
   case DXGI_FORMAT_R16G16B16A16_FLOAT:
   case DXGI_FORMAT_R16G16B16A16_UNORM:
   case DXGI_FORMAT_R16G16B16A16_UINT:
   case DXGI_FORMAT_R16G16B16A16_SNORM:
   case DXGI_FORMAT_R16G16B16A16_SINT:
   case DXGI_FORMAT_R32G32_TYPELESS:
   case DXGI_FORMAT_R32G32_FLOAT:
   case DXGI_FORMAT_R32G32_UINT:
   case DXGI_FORMAT_R32G32_SINT:
   case DXGI_FORMAT_R32G8X24_TYPELESS:
   case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
   case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
   case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
      return 8;
   case DXGI_FORMAT_R10G10B10A2_TYPELESS:
   case DXGI_FORMAT_R10G10B10A2_UNORM:
   case DXGI_FORMAT_R10G10B10A2_UINT:
   case DXGI_FORMAT_R11G11B10_FLOAT:
   case DXGI_FORMAT_R8G8B8A8_TYPELESS:
   case DXGI_FORMAT_R8G8B8A8_UNORM:
   case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
   case DXGI_FORMAT_R8G8B8A8_UINT:
   case DXGI_FORMAT_R8G8B8A8_SNORM:
   case DXGI_FORMAT_R8G8B8A8_SINT:
   case DXGI_FORMAT_R16G16_TYPELESS:
   case DXGI_FORMAT_R16G16_FLOAT:
   case DXGI_FORMAT_R16G16_UNORM:
   case DXGI_FORMAT_R16G16_UINT:
   case DXGI_FORMAT_R16G16_SNORM:
   case DXGI_FORMAT_R16G16_SINT:
   case DXGI_FORMAT_R32_TYPELESS:
   case DXGI_FORMAT_D32_FLOAT:
   case DXGI_FORMAT_R32_FLOAT:
   case DXGI_FORMAT_R32_UINT:
   case DXGI_FORMAT_R32_SINT:
   case DXGI_FORMAT_R24G8_TYPELESS:
   case DXGI_FORMAT_D24_UNORM_S8_UINT:
   case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
   case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
   case DXGI_FORMAT_R9G9B9E5_SHAREDEXP:
   case DXGI_FORMAT_B8G8R8A8_UNORM:
   case DXGI_FORMAT_B8G8R8X8_UNORM:
   case DXGI_FORMAT_B8G8R8A8_TYPELESS:
   case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
   case DXGI_FORMAT_B8G8R8X8_TYPELESS:
   case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
      return 4;
   case DXGI_FORMAT_R8G8_TYPELESS:
   case DXGI_FORMAT_R8G8_UNORM:
   case DXGI_FORMAT_R8G8_UINT:
   case DXGI_FORMAT_R8G8_SNORM:
   case DXGI_FORMAT_R8G8_SINT:
   case DXGI_FORMAT_R16_TYPELESS:
   case DXGI_FORMAT_R16_FLOAT:
   case DXGI_FORMAT_D16_UNORM:
   case DXGI_FORMAT_R16_UNORM:
   case DXGI_FORMAT_R16_UINT:
   case DXGI_FORMAT_R16_SNORM:
   case DXGI_FORMAT_R16_SINT:
   case DXGI_FORMAT_B5G6R5_UNORM:
   case DXGI_FORMAT_B5G5R5A1_UNORM:
   case DXGI_FORMAT_B4G4R4A4_UNORM:
      return 2;
   case DXGI_FORMAT_R8_TYPELESS:
   case DXGI_FORMAT_R8_UNORM:
   case DXGI_FORMAT_R8_UINT:
   case DXGI_FORMAT_R8_SNORM:
   case DXGI_FORMAT_R8_SINT:
   case DXGI_FORMAT_A8_UNORM:
      return 1;
   default:
      return 0;
   }
}

static uint32_t
format_block_height(DXGI_FORMAT fmt)
{
   switch ((int)fmt) {
   case DXGI_FORMAT_BC1_TYPELESS:
   case DXGI_FORMAT_BC1_UNORM:
   case DXGI_FORMAT_BC1_UNORM_SRGB:
   case DXGI_FORMAT_BC2_TYPELESS:
   case DXGI_FORMAT_BC2_UNORM:
   case DXGI_FORMAT_BC2_UNORM_SRGB:
   case DXGI_FORMAT_BC3_TYPELESS:
   case DXGI_FORMAT_BC3_UNORM:
   case DXGI_FORMAT_BC3_UNORM_SRGB:
   case DXGI_FORMAT_BC4_TYPELESS:
   case DXGI_FORMAT_BC4_UNORM:
   case DXGI_FORMAT_BC4_SNORM:
   case DXGI_FORMAT_BC5_TYPELESS:
   case DXGI_FORMAT_BC5_UNORM:
   case DXGI_FORMAT_BC5_SNORM:
   case DXGI_FORMAT_BC6H_TYPELESS:
   case DXGI_FORMAT_BC6H_UF16:
   case DXGI_FORMAT_BC6H_SF16:
   case DXGI_FORMAT_BC7_TYPELESS:
   case DXGI_FORMAT_BC7_UNORM:
   case DXGI_FORMAT_BC7_UNORM_SRGB:
      return 4;
   default:
      return 1;
   }
}

uint32_t
npt_dxgi_format_block_height(DXGI_FORMAT fmt)
{
   return format_block_height(fmt);
}

uint32_t
npt_dxgi_format_block_rows(DXGI_FORMAT fmt, uint32_t texel_rows)
{
   const uint32_t bh = format_block_height(fmt);
   return texel_rows / bh + (texel_rows % bh != 0);
}

/* Bytes actually occupied by one row of blocks: the tight
 * RowSizeInBytes of D3D's UpdateSubresource source contract.
 * Returns 0 when the format's size is unknown (callers keep
 * their legacy full-pitch sizing in that case). */
uint32_t
npt_dxgi_format_row_bytes(DXGI_FORMAT fmt, uint32_t texel_width)
{
   uint64_t bytes;
   switch ((int)fmt) {
   case DXGI_FORMAT_BC1_TYPELESS:
   case DXGI_FORMAT_BC1_UNORM:
   case DXGI_FORMAT_BC1_UNORM_SRGB:
   case DXGI_FORMAT_BC4_TYPELESS:
   case DXGI_FORMAT_BC4_UNORM:
   case DXGI_FORMAT_BC4_SNORM:
      bytes = ((uint64_t)texel_width + 3u) / 4u * 8u; break;
   case DXGI_FORMAT_BC2_TYPELESS:
   case DXGI_FORMAT_BC2_UNORM:
   case DXGI_FORMAT_BC2_UNORM_SRGB:
   case DXGI_FORMAT_BC3_TYPELESS:
   case DXGI_FORMAT_BC3_UNORM:
   case DXGI_FORMAT_BC3_UNORM_SRGB:
   case DXGI_FORMAT_BC5_TYPELESS:
   case DXGI_FORMAT_BC5_UNORM:
   case DXGI_FORMAT_BC5_SNORM:
   case DXGI_FORMAT_BC6H_TYPELESS:
   case DXGI_FORMAT_BC6H_UF16:
   case DXGI_FORMAT_BC6H_SF16:
   case DXGI_FORMAT_BC7_TYPELESS:
   case DXGI_FORMAT_BC7_UNORM:
   case DXGI_FORMAT_BC7_UNORM_SRGB:
      bytes = ((uint64_t)texel_width + 3u) / 4u * 16u; break;
   case DXGI_FORMAT_R8G8_B8G8_UNORM:
   case DXGI_FORMAT_G8R8_G8B8_UNORM:
   case DXGI_FORMAT_YUY2:
      /* Packed pairs: 32 bits per two texels. */
      bytes = ((uint64_t)texel_width + 1u) / 2u * 4u; break;
   case DXGI_FORMAT_Y210:
   case DXGI_FORMAT_Y216:
      /* Packed pairs: 64 bits per two texels. */
      bytes = ((uint64_t)texel_width + 1u) / 2u * 8u; break;
   case DXGI_FORMAT_AYUV:
   case DXGI_FORMAT_Y410:
      bytes = (uint64_t)texel_width * 4u; break;
   case DXGI_FORMAT_Y416:
      bytes = (uint64_t)texel_width * 8u; break;
   case DXGI_FORMAT_NV12:
   case DXGI_FORMAT_420_OPAQUE:
      /* The luma plane establishes the source row pitch.  The
       * interleaved chroma plane has the same byte pitch for NV12 and is
       * covered by npt_dxgi_format_subresource_rows. */
      bytes = texel_width; break;
   case DXGI_FORMAT_NV11:
      /* Four luma samples per interleaved chroma pair. Legal widths are
       * multiples of four; the whole footprint includes chroma padding. */
      bytes = ((uint64_t)texel_width + 3u) / 4u * 4u; break;
   case DXGI_FORMAT_P010:
   case DXGI_FORMAT_P016:
      bytes = (uint64_t)texel_width * 2u; break;
   case DXGI_FORMAT_R1_UNORM:
      bytes = ((uint64_t)texel_width + 7u) / 8u; break;
   default:
      bytes = (uint64_t)texel_width * npt_dxgi_format_bytes_per_pixel(fmt);
      break;
   }
   return bytes <= UINT32_MAX ? (uint32_t)bytes : 0;
}

uint32_t
npt_dxgi_format_subresource_rows(DXGI_FORMAT fmt, uint32_t height)
{
   switch ((int)fmt) {
   case DXGI_FORMAT_NV12:
   case DXGI_FORMAT_P010:
   case DXGI_FORMAT_P016:
   case DXGI_FORMAT_420_OPAQUE:
      /* Luma plane, then a half-height interleaved chroma plane. */
      return height + height / 2u;
   case DXGI_FORMAT_NV11:
      /* Luma plane, then a half-pitch full-height chroma plane, then
       * padding out to twice the luma plane. */
      return height * 2u;
   default:
      /* P208 / V208 / V408 are multi-plane too but their initial-data
       * footprint is undocumented; one row per texel row under-copies
       * rather than reading past the app's buffer. */
      return npt_dxgi_format_block_rows(fmt, height);
   }
}

static void
texture_publish_desc(struct npt_d3d11_texture *t,
                       const struct npt_d3d11_texture_desc *d,
                       bool extended)
{
   struct npt_d3d11_texture_aux *aux = tex_aux(t);
   if (!aux || !d) return;
   aux->width = d->width;
   aux->height = d->height ? d->height : 1;
   aux->depth = d->depth ? d->depth : 1;
   if (d->mip_levels) {
      aux->mip_levels = d->mip_levels;
   } else {
      uint32_t largest = aux->width;
      if (aux->height > largest) largest = aux->height;
      if (aux->depth > largest) largest = aux->depth;
      aux->mip_levels = 1;
      while (largest > 1u) {
         largest >>= 1;
         ++aux->mip_levels;
      }
   }
   aux->array_size = d->array_size ? d->array_size : 1;
   aux->format = d->format;
   aux->bytes_per_pixel = npt_dxgi_format_bytes_per_pixel(d->format);
   aux->usage = d->usage;
   aux->bind_flags = d->bind_flags;
   aux->cpu_access_flags = d->cpu_access_flags;
   aux->misc_flags = d->misc_flags;
   aux->sample_count = d->sample_count ? d->sample_count : 1;
   aux->sample_quality = d->sample_quality;
   aux->texture_layout = d->texture_layout;
   atomic_store_explicit(&aux->desc_state, extended ? 2u : 1u,
                          memory_order_release);
}

void
npt_d3d11_texture_set_desc(struct npt_d3d11_texture *t,
                           const struct npt_d3d11_texture_desc *d)
{
   /* Creation/import callers own an unpublished wrapper and know its full
    * description. In particular base CreateTexture implies UNDEFINED layout. */
   texture_publish_desc(t, d, true);
}

void
npt_d3d11_texture_fill_desc1d(const struct npt_d3d11_texture *t,
                              D3D11_TEXTURE1D_DESC *out)
{
   const struct npt_d3d11_texture_aux *aux = tex_aux(t);
   if (!aux || !out) return;
   out->Width = aux->width;
   out->MipLevels = aux->mip_levels;
   out->ArraySize = aux->array_size;
   out->Format = aux->format;
   out->Usage = (D3D11_USAGE)aux->usage;
   out->BindFlags = aux->bind_flags;
   out->CPUAccessFlags = aux->cpu_access_flags;
   out->MiscFlags = aux->misc_flags;
}

void
npt_d3d11_texture_fill_desc2d(const struct npt_d3d11_texture *t,
                              D3D11_TEXTURE2D_DESC *out)
{
   const struct npt_d3d11_texture_aux *aux = tex_aux(t);
   if (!aux || !out) return;
   out->Width = aux->width;
   out->Height = aux->height;
   out->MipLevels = aux->mip_levels;
   out->ArraySize = aux->array_size;
   out->Format = aux->format;
   out->SampleDesc.Count = aux->sample_count;
   out->SampleDesc.Quality = aux->sample_quality;
   out->Usage = (D3D11_USAGE)aux->usage;
   out->BindFlags = aux->bind_flags;
   out->CPUAccessFlags = aux->cpu_access_flags;
   out->MiscFlags = aux->misc_flags;
}

void
npt_d3d11_texture_fill_desc2d1(const struct npt_d3d11_texture *t,
                               D3D11_TEXTURE2D_DESC1 *out)
{
   const struct npt_d3d11_texture_aux *aux = tex_aux(t);
   if (!aux || !out) return;
   out->Width = aux->width;
   out->Height = aux->height;
   out->MipLevels = aux->mip_levels;
   out->ArraySize = aux->array_size;
   out->Format = aux->format;
   out->SampleDesc.Count = aux->sample_count;
   out->SampleDesc.Quality = aux->sample_quality;
   out->Usage = (D3D11_USAGE)aux->usage;
   out->BindFlags = aux->bind_flags;
   out->CPUAccessFlags = aux->cpu_access_flags;
   out->MiscFlags = aux->misc_flags;
   out->TextureLayout = aux->texture_layout;
}

void
npt_d3d11_texture_fill_desc3d(const struct npt_d3d11_texture *t,
                              D3D11_TEXTURE3D_DESC *out)
{
   const struct npt_d3d11_texture_aux *aux = tex_aux(t);
   if (!aux || !out) return;
   out->Width = aux->width;
   out->Height = aux->height;
   out->Depth = aux->depth;
   out->MipLevels = aux->mip_levels;
   out->Format = aux->format;
   out->Usage = (D3D11_USAGE)aux->usage;
   out->BindFlags = aux->bind_flags;
   out->CPUAccessFlags = aux->cpu_access_flags;
   out->MiscFlags = aux->misc_flags;
}

void
npt_d3d11_texture_fill_desc3d1(const struct npt_d3d11_texture *t,
                               D3D11_TEXTURE3D_DESC1 *out)
{
   const struct npt_d3d11_texture_aux *aux = tex_aux(t);
   if (!aux || !out) return;
   out->Width = aux->width;
   out->Height = aux->height;
   out->Depth = aux->depth;
   out->MipLevels = aux->mip_levels;
   out->Format = aux->format;
   out->Usage = (D3D11_USAGE)aux->usage;
   out->BindFlags = aux->bind_flags;
   out->CPUAccessFlags = aux->cpu_access_flags;
   out->MiscFlags = aux->misc_flags;
   out->TextureLayout = aux->texture_layout;
}

bool
npt_d3d11_texture_has_desc(const struct npt_d3d11_texture *t)
{
   const struct npt_d3d11_texture_aux *aux = tex_aux(t);
   return aux && atomic_load_explicit(&aux->desc_state, memory_order_acquire) &&
      aux->width && aux->height && aux->depth &&
      npt_dxgi_format_row_bytes(aux->format, aux->width) != 0;
}

bool
npt_d3d11_texture_ensure_desc(struct npt_d3d11_texture *t,
                              D3D11_RESOURCE_DIMENSION dimension,
                              bool extended)
{
   struct npt_d3d11_texture_aux *aux = tex_aux(t);
   if (!aux)
      return false;
   const unsigned needed = extended ? 2u : 1u;
   if (atomic_load_explicit(&aux->desc_state, memory_order_acquire) >= needed)
      return true;

   mtx_lock(&aux->desc_mutex);
   unsigned state = atomic_load_explicit(&aux->desc_state, memory_order_acquire);
   if (state >= needed) {
      mtx_unlock(&aux->desc_mutex);
      return true;
   }

   bool ok = false;
   struct npt_ring *ring = npt_com_self_ring(t);
   if (!npt_ring_is_healthy(ring))
      goto done;
   if (dimension == D3D11_RESOURCE_DIMENSION_UNKNOWN)
      npt_id3d11resource_default_GetType(t, &dimension);

   struct npt_d3d11_texture_desc d = {0};
   d.height = d.depth = d.array_size = d.sample_count = 1;
   switch (dimension) {
   case D3D11_RESOURCE_DIMENSION_TEXTURE1D: {
      D3D11_TEXTURE1D_DESC desc = {0};
      npt_id3d11texture1d_default_GetDesc(t, &desc);
      d.width = desc.Width;
      d.mip_levels = desc.MipLevels;
      d.array_size = desc.ArraySize;
      d.format = desc.Format;
      d.usage = desc.Usage;
      d.bind_flags = desc.BindFlags;
      d.cpu_access_flags = desc.CPUAccessFlags;
      d.misc_flags = desc.MiscFlags;
      extended = true;
      break;
   }
   case D3D11_RESOURCE_DIMENSION_TEXTURE2D: {
      D3D11_TEXTURE2D_DESC1 desc = {0};
      if (extended) {
         npt_id3d11texture2d1_default_GetDesc1(t, &desc);
      } else {
         D3D11_TEXTURE2D_DESC base = {0};
         npt_id3d11texture2d_default_GetDesc(t, &base);
         desc.Width = base.Width;
         desc.Height = base.Height;
         desc.MipLevels = base.MipLevels;
         desc.ArraySize = base.ArraySize;
         desc.Format = base.Format;
         desc.SampleDesc = base.SampleDesc;
         desc.Usage = base.Usage;
         desc.BindFlags = base.BindFlags;
         desc.CPUAccessFlags = base.CPUAccessFlags;
         desc.MiscFlags = base.MiscFlags;
      }
      d.width = desc.Width;
      d.height = desc.Height;
      d.mip_levels = desc.MipLevels;
      d.array_size = desc.ArraySize;
      d.format = desc.Format;
      d.sample_count = desc.SampleDesc.Count;
      d.sample_quality = desc.SampleDesc.Quality;
      d.usage = desc.Usage;
      d.bind_flags = desc.BindFlags;
      d.cpu_access_flags = desc.CPUAccessFlags;
      d.misc_flags = desc.MiscFlags;
      d.texture_layout = desc.TextureLayout;
      break;
   }
   case D3D11_RESOURCE_DIMENSION_TEXTURE3D: {
      D3D11_TEXTURE3D_DESC1 desc = {0};
      if (extended) {
         npt_id3d11texture3d1_default_GetDesc1(t, &desc);
      } else {
         D3D11_TEXTURE3D_DESC base = {0};
         npt_id3d11texture3d_default_GetDesc(t, &base);
         desc.Width = base.Width;
         desc.Height = base.Height;
         desc.Depth = base.Depth;
         desc.MipLevels = base.MipLevels;
         desc.Format = base.Format;
         desc.Usage = base.Usage;
         desc.BindFlags = base.BindFlags;
         desc.CPUAccessFlags = base.CPUAccessFlags;
         desc.MiscFlags = base.MiscFlags;
      }
      d.width = desc.Width;
      d.height = desc.Height;
      d.depth = desc.Depth;
      d.mip_levels = desc.MipLevels;
      d.format = desc.Format;
      d.usage = desc.Usage;
      d.bind_flags = desc.BindFlags;
      d.cpu_access_flags = desc.CPUAccessFlags;
      d.misc_flags = desc.MiscFlags;
      d.texture_layout = desc.TextureLayout;
      break;
   }
   default:
      goto done;
   }
   /* The generated void thunks leave zeroed output on a failed RPC. Never
    * publish that output, or claim a healthy descriptor after transport loss. */
   if (!npt_ring_is_healthy(ring) || !d.width || !d.height || !d.depth ||
       !d.mip_levels || d.mip_levels > 32 || !d.array_size ||
       !d.sample_count || d.format == DXGI_FORMAT_UNKNOWN)
      goto done;
   if (!state) {
      texture_publish_desc(t, &d, extended);
   } else {
      /* Readers may already use the published base fields. Only the
       * previously unknown layout can change during an extended query. */
      if (aux->width != d.width || aux->height != d.height ||
          aux->depth != d.depth || aux->mip_levels != d.mip_levels ||
          aux->array_size != d.array_size || aux->format != d.format ||
          aux->sample_count != d.sample_count ||
          aux->sample_quality != d.sample_quality || aux->usage != d.usage ||
          aux->bind_flags != d.bind_flags ||
          aux->cpu_access_flags != d.cpu_access_flags ||
          aux->misc_flags != d.misc_flags)
         goto done;
      aux->texture_layout = d.texture_layout;
      atomic_store_explicit(&aux->desc_state, 2u, memory_order_release);
   }
   ok = true;
done:
   mtx_unlock(&aux->desc_mutex);
   return ok;
}

bool
npt_d3d11_texture_is_mappable(const struct npt_d3d11_texture *t)
{
   const struct npt_d3d11_texture_aux *aux = tex_aux(t);
   if (!aux) return false;
   if (!npt_d3d11_texture_has_desc(t))
      return false;
   if (aux->usage == D3D11_USAGE_DYNAMIC)
      return (aux->cpu_access_flags & D3D11_CPU_ACCESS_WRITE) != 0;
   if (aux->usage == D3D11_USAGE_STAGING)
      return (aux->cpu_access_flags &
              (D3D11_CPU_ACCESS_WRITE | D3D11_CPU_ACCESS_READ)) != 0;
   return false;
}

static inline uint32_t
mip_dim(uint32_t base, uint32_t mip)
{
   return mip < 32u && (base >> mip) ? (base >> mip) : 1u;
}

static uint32_t
texture_subresource_mip(const struct npt_d3d11_texture_aux *aux, uint32_t subresource)
{
   const uint32_t mips = aux->mip_levels ? aux->mip_levels : 1u;
   return subresource % mips;
}

/* Full row_pitch * rows * depth footprint of one subresource: the
 * region a Map hands out and an Unmap transfers, charging the full
 * pitch for every row including the last. */
uint32_t
npt_d3d11_texture_get_subresource_byte_size(const struct npt_d3d11_texture *t,
                                            uint32_t subresource,
                                            uint32_t row_pitch)
{
   const struct npt_d3d11_texture_aux *aux = tex_aux(t);
   if (!aux) return 0;
   const uint32_t mip = texture_subresource_mip(aux, subresource);
   const uint32_t rows =
      npt_dxgi_format_subresource_rows(aux->format, mip_dim(aux->height, mip));
   const uint32_t d = mip_dim(aux->depth, mip);
   const uint64_t slice = (uint64_t)row_pitch * rows;
   /* Reject before multiplying by depth so malformed dimensions cannot wrap. */
   if (!d || slice > UINT32_MAX / d)
      return 0;
   return (uint32_t)(slice * d);
}

/* A mapped volume may pad between slices. Only the last slice uses its
 * row footprint; DepthPitch describes the distance between earlier slices. */
uint32_t
npt_d3d11_texture_get_subresource_map_byte_size(const struct npt_d3d11_texture *t,
                                                uint32_t subresource,
                                                uint32_t row_pitch,
                                                uint32_t depth_pitch)
{
   const struct npt_d3d11_texture_aux *aux = tex_aux(t);
   if (!aux) return 0;
   const uint32_t mip = texture_subresource_mip(aux, subresource);
   const uint32_t rows =
      npt_dxgi_format_subresource_rows(aux->format, mip_dim(aux->height, mip));
   const uint32_t depth = mip_dim(aux->depth, mip);
   const uint32_t row_bytes =
      npt_dxgi_format_row_bytes(aux->format, mip_dim(aux->width, mip));
   const uint64_t slice = (uint64_t)row_pitch * rows;
   if (!row_bytes || row_pitch < row_bytes || !slice || slice > UINT32_MAX)
      return 0;
   if (depth <= 1)
      return (uint32_t)slice;
   if (depth_pitch < slice ||
       (uint64_t)(depth - 1) > (UINT32_MAX - slice) / depth_pitch)
      return 0;
   return (uint32_t)((uint64_t)depth_pitch * (depth - 1) + slice);
}

/* Validated update region in texels, with its memory-row geometry. The
 * descriptor, not a caller-supplied box, bounds all source offset arithmetic. */
bool
npt_d3d11_texture_get_update_layout(const struct npt_d3d11_texture *t,
                                    uint32_t subresource, const D3D11_BOX *box,
                                    D3D11_BOX *region, uint32_t *row_bytes,
                                    uint32_t *rows, uint32_t *block_height,
                                    bool *planar)
{
   const struct npt_d3d11_texture_aux *aux = tex_aux(t);
   if (!aux || !aux->width || !aux->height || !aux->depth ||
       !aux->mip_levels || aux->mip_levels > 32 || !aux->array_size ||
       (uint64_t)subresource >= (uint64_t)aux->mip_levels * aux->array_size)
      return false;
   const uint32_t mip = texture_subresource_mip(aux, subresource);
   const uint32_t width = mip_dim(aux->width, mip);
   const uint32_t height = mip_dim(aux->height, mip);
   const uint32_t depth = mip_dim(aux->depth, mip);
   *region = (D3D11_BOX){0, 0, 0, width, height, depth};
   if (box) {
      if (box->left >= box->right || box->top >= box->bottom ||
          box->front >= box->back || box->right > width ||
          box->bottom > height || box->back > depth)
         return false;
      *region = *box;
   }
   *block_height = format_block_height(aux->format);
   if (*block_height > 1 &&
       ((region->left % 4u) || (region->top % 4u) ||
        (region->right != width && region->right % 4u) ||
        (region->bottom != height && region->bottom % 4u)))
      return false;
   *row_bytes = npt_dxgi_format_row_bytes(aux->format,
                                          region->right - region->left);
   *rows = npt_dxgi_format_block_rows(aux->format,
                                      region->bottom - region->top);
   const uint32_t full_rows =
      npt_dxgi_format_subresource_rows(aux->format, height);
   *planar = full_rows != npt_dxgi_format_block_rows(aux->format, height);
   if (*planar) {
      /* D3D9/10 do not expose planar resources. Keep the modern full-plane
       * path intact; a rectangular split cannot address chroma rows. */
      if (region->left || region->top || region->right != width ||
          region->bottom != height || region->front || region->back != 1)
         return false;
      *rows = full_rows;
   }
   return *row_bytes && *rows;
}

/* Sizes of a no-box UpdateSubresource transfer.  The return value is
 * the full-pitch footprint the host's UpdateSubresource may consume
 * (the ring reservation); *out_copy_size is the extent D3D guarantees
 * readable in the caller's buffer -- (rows-1)*pitch + RowSizeInBytes
 * per slice -- which is all the guest may copy.  Planar formats copy
 * the full footprint: MSDN documents their initial-data and staging
 * layout as rowPitch * subresource_rows with no final-row discount.
 * Returns 0 (with *out_copy_size = 0) when the transfer cannot be
 * sized. */
uint32_t
npt_d3d11_texture_get_subresource_update_sizes(const struct npt_d3d11_texture *t,
                                               uint32_t subresource,
                                               uint32_t row_pitch,
                                               uint32_t depth_pitch,
                                               uint32_t *out_copy_size)
{
   *out_copy_size = 0;
   const struct npt_d3d11_texture_aux *aux = tex_aux(t);
   if (!aux) return 0;
   const uint32_t mip = texture_subresource_mip(aux, subresource);
   const uint32_t mh  = mip_dim(aux->height, mip);
   const uint32_t d   = mip_dim(aux->depth, mip);
   const uint32_t rows = npt_dxgi_format_subresource_rows(aux->format, mh);
   const uint32_t last_row =
      npt_dxgi_format_row_bytes(aux->format, mip_dim(aux->width, mip));
   const bool tight = last_row &&
      rows == npt_dxgi_format_block_rows(aux->format, mh);
   const uint64_t slice = (uint64_t)row_pitch * rows;
   const uint64_t strides = (d > 1u && depth_pitch)
      ? (uint64_t)depth_pitch * (d - 1u)
      : slice * (d - 1u);
   const uint64_t full = strides + slice;
   uint64_t copy = tight
      ? strides + (uint64_t)row_pitch * (rows - 1u) + last_row
      : full;
   if (copy > full)
      copy = full;
   if (!full || full > 0xffffffffu)
      return 0;
   *out_copy_size = (uint32_t)copy;
   return (uint32_t)full;
}

void
npt_d3d11_texture_get_mip_dimensions(const struct npt_d3d11_texture *t,
                                     uint32_t subresource,
                                     uint32_t *out_height,
                                     uint32_t *out_depth)
{
   const struct npt_d3d11_texture_aux *aux = tex_aux(t);
   if (!aux) {
      if (out_height) *out_height = 0;
      if (out_depth)  *out_depth  = 0;
      return;
   }
   const uint32_t mip = texture_subresource_mip(aux, subresource);
   if (out_height) *out_height = mip_dim(aux->height, mip);
   if (out_depth)  *out_depth  = mip_dim(aux->depth,  mip);
}

uint32_t
npt_d3d11_texture_get_bytes_per_pixel(const struct npt_d3d11_texture *t)
{
   const struct npt_d3d11_texture_aux *aux = tex_aux(t);
   return aux ? aux->bytes_per_pixel : 0;
}

DXGI_FORMAT
npt_d3d11_texture_get_format(const struct npt_d3d11_texture *t)
{
   const struct npt_d3d11_texture_aux *aux = tex_aux(t);
   return aux ? aux->format : DXGI_FORMAT_UNKNOWN;
}

bool
npt_d3d11_texture_grow_map_shmem(struct npt_d3d11_texture *t, uint32_t min_size)
{
   struct npt_d3d11_texture_aux *aux = tex_aux(t);
   if (!aux || !min_size || min_size > UINT32_MAX - 63u)
      return false;
   return npt_d3d_map_ring_alloc_shmem(&aux->map_ring,
                                       (min_size + 63u) & ~63u);
}

bool
npt_d3d11_texture_ensure_map_shmem(struct npt_d3d11_texture *t)
{
   if (!npt_d3d11_texture_is_mappable(t)) return false;
   struct npt_d3d11_texture_aux *aux = tex_aux(t);
   if (!aux) return false;
   const uint32_t row_bytes = npt_dxgi_format_row_bytes(aux->format, aux->width);
   const uint64_t aligned_row_pitch = ((uint64_t)row_bytes + 255u) & ~(uint64_t)255u;
   const uint32_t rows = npt_dxgi_format_subresource_rows(aux->format, aux->height);
   const uint64_t slice = aligned_row_pitch * rows;
   /* The protocol uses 32-bit byte extents; actual allocation failure, rather
    * than an arbitrary 64 MiB policy, limits otherwise representable slots. */
   if (!row_bytes || !aux->depth || slice > (UINT32_MAX - 63u) / aux->depth)
      return false;
   return npt_d3d11_texture_grow_map_shmem(t, (uint32_t)(slice * aux->depth));
}

uint32_t npt_d3d11_texture_get_map_shmem_res_id(const struct npt_d3d11_texture *t)
{ const struct npt_d3d11_texture_aux *aux = tex_aux(t); return aux ? npt_d3d_map_ring_slot_res_id(&aux->map_ring, 0) : 0; }

uint32_t npt_d3d11_texture_get_slot_shmem_res_id(const struct npt_d3d11_texture *t,
                                                 uint32_t slot)
{ const struct npt_d3d11_texture_aux *aux = tex_aux(t); return aux ? npt_d3d_map_ring_slot_res_id(&aux->map_ring, slot) : 0; }

uint32_t npt_d3d11_texture_get_shmem_size(const struct npt_d3d11_texture *t)
{ const struct npt_d3d11_texture_aux *aux = tex_aux(t); return aux ? aux->map_ring.aligned_slot_size * aux->map_ring.active_count : 0; }

uint32_t npt_d3d11_texture_get_slot_size(const struct npt_d3d11_texture *t)
{ const struct npt_d3d11_texture_aux *aux = tex_aux(t); return aux ? aux->map_ring.aligned_slot_size : 0; }

void *npt_d3d11_texture_shmem_ptr(const struct npt_d3d11_texture *t)
{
   const struct npt_d3d11_texture_aux *aux = tex_aux(t);
   return aux ? npt_d3d_map_ring_slot_ptr(&aux->map_ring, 0) : NULL;
}

uint32_t npt_d3d11_texture_get_current_slot(const struct npt_d3d11_texture *t)
{ const struct npt_d3d11_texture_aux *aux = tex_aux(t); return aux ? aux->map_ring.current_slot : 0; }

void npt_d3d11_texture_set_current_slot(struct npt_d3d11_texture *t, uint32_t slot)
{
   struct npt_d3d11_texture_aux *aux = tex_aux(t);
   if (aux && slot < NPT_D3D_MAP_SLOT_MAX) aux->map_ring.current_slot = slot;
}

uint32_t npt_d3d11_texture_slot_offset(const struct npt_d3d11_texture *t, uint32_t slot)
{ const struct npt_d3d11_texture_aux *aux = tex_aux(t); return aux ? npt_d3d_map_ring_slot_offset(&aux->map_ring, slot) : 0; }

void *npt_d3d11_texture_slot_ptr(const struct npt_d3d11_texture *t, uint32_t slot)
{
   const struct npt_d3d11_texture_aux *aux = tex_aux(t);
   return aux ? npt_d3d_map_ring_slot_ptr(&aux->map_ring, slot) : NULL;
}

uint32_t npt_d3d11_texture_rotate_slot(struct npt_d3d11_texture *t)
{
   struct npt_d3d11_texture_aux *aux = tex_aux(t);
   return aux ? npt_d3d_map_ring_rotate_slot(&aux->map_ring) : 0;
}

void npt_d3d11_texture_mark_slot_submitted(struct npt_d3d11_texture *t,
                                           uint32_t slot, uint32_t seqno,
                                           struct npt_ring *ring)
{
   struct npt_d3d11_texture_aux *aux = tex_aux(t);
   if (aux) npt_d3d_map_ring_mark_slot_submitted(&aux->map_ring, slot, seqno, ring);
}

bool npt_d3d11_texture_get_is_mapped(const struct npt_d3d11_texture *t)
{ const struct npt_d3d11_texture_aux *aux = tex_aux(t); return aux ? aux->map_ring.is_mapped : false; }

void npt_d3d11_texture_set_mapped_state(struct npt_d3d11_texture *t,
                                        uint32_t subresource, uint32_t row_pitch,
                                        uint32_t byte_size, uint32_t access_flags)
{
   struct npt_d3d11_texture_aux *aux = tex_aux(t);
   if (!aux) return;
   aux->map_ring.is_mapped = true;
   aux->map_ring.last_map_access_flags = access_flags;
   aux->last_map_subresource = subresource;
   aux->last_map_row_pitch = row_pitch;
   aux->last_map_byte_size = byte_size;
}

void npt_d3d11_texture_clear_mapped_state(struct npt_d3d11_texture *t)
{
   struct npt_d3d11_texture_aux *aux = tex_aux(t);
   if (!aux) return;
   aux->map_ring.is_mapped = false;
   aux->map_ring.last_map_access_flags = 0;
   aux->last_map_byte_size = 0;
}

uint32_t npt_d3d11_texture_get_last_map_subresource(const struct npt_d3d11_texture *t)
{ const struct npt_d3d11_texture_aux *aux = tex_aux(t); return aux ? aux->last_map_subresource : 0; }
uint32_t npt_d3d11_texture_get_last_map_byte_size(const struct npt_d3d11_texture *t)
{ const struct npt_d3d11_texture_aux *aux = tex_aux(t); return aux ? aux->last_map_byte_size : 0; }
uint32_t npt_d3d11_texture_get_last_map_access_flags(const struct npt_d3d11_texture *t)
{ const struct npt_d3d11_texture_aux *aux = tex_aux(t); return aux ? aux->map_ring.last_map_access_flags : 0; }

uint32_t npt_d3d11_texture_get_cached_row_pitch(const struct npt_d3d11_texture *t)
{ const struct npt_d3d11_texture_aux *aux = tex_aux(t); return aux ? aux->cached_row_pitch : 0; }
uint32_t npt_d3d11_texture_get_cached_depth_pitch(const struct npt_d3d11_texture *t)
{ const struct npt_d3d11_texture_aux *aux = tex_aux(t); return aux ? aux->cached_depth_pitch : 0; }

void npt_d3d11_texture_set_cached_pitches(struct npt_d3d11_texture *t,
                                          uint32_t row_pitch, uint32_t depth_pitch)
{
   struct npt_d3d11_texture_aux *aux = tex_aux(t);
   if (!aux) return;
   aux->cached_row_pitch = row_pitch;
   aux->cached_depth_pitch = depth_pitch;
}
