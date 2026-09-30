/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 */

#include "npt_device.h"
#include "npt_com.h"
#include "npt_dispatch.h"
#include "npt_env.h"
#include "npt_event.h"
#include "npt_tls.h"
#include "npt_workaround.h"
#include "npt_ring.h"

#include <inttypes.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#endif

#if defined(__WINE__)
#include "nptunix/npt_unixlib.h"
#endif

static inline uint32_t
npt_wrapper_cache_hash(uint64_t host_id)
{
   return (uint32_t)((host_id * 0x9E3779B97F4A7C15ull) >> 32) &
          (NPT_WRAPPER_CACHE_BUCKETS - 1);
}

static void
npt_wrapper_cache_init(struct npt_wrapper_cache *c)
{
   memset(c, 0, sizeof(*c));
   mtx_init(&c->mutex, mtx_plain);
}

static void
npt_wrapper_cache_fini(struct npt_wrapper_cache *c)
{
   /* The device-destroy drain should have emptied every chain; free
    * defensively in case of a teardown race. */
   for (uint32_t i = 0; i < NPT_WRAPPER_CACHE_BUCKETS; i++) {
      struct npt_wrapper_cache_entry *e = c->buckets[i];
      while (e) {
         struct npt_wrapper_cache_entry *next = e->next;
         free(e);
         e = next;
      }
      c->buckets[i] = NULL;
   }
   c->count = 0;
   mtx_destroy(&c->mutex);
}

/* CAS-bump pub_ref guarding against resurrecting a pub_ref==0 wrapper
 * already in destruction.  NULL = caller treats as cache miss. */
static void *
npt_wrapper_cache_try_acquire_locked(void *wrapper)
{
   struct npt_com_base *com = wrapper;
   uint32_t prev = atomic_load(&com->base.pub_ref);
   while (prev != 0) {
      if (atomic_compare_exchange_weak(&com->base.pub_ref, &prev, prev + 1))
         return com;
   }
   return NULL;
}

void *
npt_device_wrapper_cache_lookup(struct npt_device *dev, uint64_t host_id)
{
   if (!host_id)
      return NULL;

   struct npt_wrapper_cache *c = &dev->wrapper_cache;
   const uint32_t h = npt_wrapper_cache_hash(host_id);
   void *result = NULL;

   /* The pub_ref bump must happen under the mutex; otherwise a
    * concurrent destroyer could evict and free between unlock and
    * bump. */
   mtx_lock(&c->mutex);
   for (struct npt_wrapper_cache_entry *e = c->buckets[h]; e; e = e->next) {
      if (e->host_id == host_id) {
         result = npt_wrapper_cache_try_acquire_locked(e->wrapper);
         break;
      }
   }
   mtx_unlock(&c->mutex);

   return result;
}

void *
npt_device_wrapper_cache_insert_if_absent(struct npt_device *dev,
                                          uint64_t host_id,
                                          void *new_wrapper,
                                          bool *retry_needed)
{
   if (retry_needed)
      *retry_needed = false;
   if (!host_id || !new_wrapper)
      return NULL;

   struct npt_wrapper_cache *c = &dev->wrapper_cache;
   const uint32_t h = npt_wrapper_cache_hash(host_id);
   void *winner = NULL;

   struct npt_wrapper_cache_entry *fresh = malloc(sizeof(*fresh));
   if (!fresh) {
      npt_log("wrapper_cache_insert_if_absent: OOM (host_id=0x%" PRIx64 ")",
              host_id);
      return NULL;
   }

   mtx_lock(&c->mutex);
   struct npt_wrapper_cache_entry *existing = NULL;
   for (struct npt_wrapper_cache_entry *e = c->buckets[h]; e; e = e->next) {
      if (e->host_id == host_id) {
         existing = e;
         break;
      }
   }
   bool inserted = false;
   if (existing) {
      if (existing->wrapper != new_wrapper) {
         winner = npt_wrapper_cache_try_acquire_locked(existing->wrapper);
         /* pub_ref==0 = mid-destruction: don't overwrite, caller retries. */
         if (!winner && retry_needed)
            *retry_needed = true;
      }
   } else {
      fresh->host_id = host_id;
      fresh->wrapper = new_wrapper;
      fresh->next = c->buckets[h];
      c->buckets[h] = fresh;
      c->count++;
      inserted = true;
   }
   mtx_unlock(&c->mutex);

   if (!inserted)
      free(fresh);
   return winner;
}

void
npt_device_wrapper_cache_remove(struct npt_device *dev, uint64_t host_id)
{
   if (!host_id)
      return;

   struct npt_wrapper_cache *c = &dev->wrapper_cache;
   const uint32_t h = npt_wrapper_cache_hash(host_id);
   struct npt_wrapper_cache_entry *removed = NULL;

   mtx_lock(&c->mutex);
   for (struct npt_wrapper_cache_entry **link = &c->buckets[h]; *link;
        link = &(*link)->next) {
      if ((*link)->host_id == host_id) {
         removed = *link;
         *link = removed->next;
         c->count--;
         break;
      }
   }
   mtx_unlock(&c->mutex);

   free(removed);
}

bool
npt_device_wrapper_cache_is_live(struct npt_device *dev,
                                 uint64_t host_id, const void *wrapper)
{
   if (!dev || !host_id || !wrapper)
      return false;
   struct npt_wrapper_cache *c = &dev->wrapper_cache;
   const uint32_t h = npt_wrapper_cache_hash(host_id);
   bool live = false;
   mtx_lock(&c->mutex);
   for (struct npt_wrapper_cache_entry *e = c->buckets[h]; e; e = e->next) {
      if (e->host_id == host_id && e->wrapper == wrapper) {
         live = true;
         break;
      }
   }
   mtx_unlock(&c->mutex);
   return live;
}

void
npt_device_wrapper_cache_foreach(struct npt_device *dev,
                                 npt_wrapper_cache_iter_fn cb,
                                 void *user_data)
{
   if (!cb)
      return;

   struct npt_wrapper_cache *c = &dev->wrapper_cache;

   /* Snapshot so the callback can safely call cache_remove without
    * invalidating our iterator. */
   mtx_lock(&c->mutex);
   const uint32_t cap = c->count;
   struct {
      uint64_t host_id;
      void *wrapper;
   } *snapshot = cap ? malloc(sizeof(*snapshot) * cap) : NULL;
   uint32_t n = 0;
   if (snapshot) {
      for (uint32_t i = 0; i < NPT_WRAPPER_CACHE_BUCKETS; i++) {
         for (struct npt_wrapper_cache_entry *e = c->buckets[i];
              e && n < cap; e = e->next) {
            snapshot[n].host_id = e->host_id;
            snapshot[n].wrapper = e->wrapper;
            n++;
         }
      }
   }
   mtx_unlock(&c->mutex);

   if (!snapshot)
      return;
   for (uint32_t i = 0; i < n; i++)
      cb(snapshot[i].host_id, snapshot[i].wrapper, user_data);
   free(snapshot);
}

#if !defined(__WINE__) && !defined(NPT_D3D9_RUNTIME_DDI) && !defined(NPT_D3D10_RUNTIME_DDI)
static struct npt_device *g_npt_device;
static mtx_t g_npt_device_mutex;
static _Atomic int g_npt_device_mutex_inited;
static uint32_t g_npt_device_use_count;  /* protected by g_npt_device_mutex */
#endif

/* The host backend's workaround flags, read from the primary ring blob and
 * cached on the first read that sees NPT_WA_FLAGS_PRESENT.  Every consumer is a
 * device-level DDI, so it runs after D3D11CreateDevice has blocked for its reply
 * on the primary ring, and the host writes the word before it creates the ring
 * thread that serves that reply -- the flags are always present by the first
 * read that matters, and the zero return is only defence in depth.
 *
 * The Wine path passes prebuilt DXBC and never calls this, so it returns 0
 * there. */
static atomic_uint g_host_workarounds;

void
npt_device_cache_workarounds(struct npt_device *dev)
{
   if (dev && dev->ring && dev->ring->wa_word) {
      uint32_t w = atomic_load_explicit(dev->ring->wa_word, memory_order_acquire);
      if (w & NPT_WA_FLAGS_PRESENT)
         atomic_store_explicit(&g_host_workarounds, w, memory_order_relaxed);
   }
}

uint32_t
npt_host_workaround_flags(void)
{
   uint32_t c = atomic_load_explicit(&g_host_workarounds, memory_order_relaxed);
   if (c & NPT_WA_FLAGS_PRESENT)
      return c & ~(uint32_t)NPT_WA_FLAGS_PRESENT;
#if !defined(__WINE__) && !defined(NPT_D3D9_RUNTIME_DDI) && !defined(NPT_D3D10_RUNTIME_DDI)
   struct npt_device *dev = g_npt_device;
   if (dev && dev->ring && dev->ring->wa_word) {
      uint32_t w = atomic_load_explicit(dev->ring->wa_word, memory_order_acquire);
      if (w & NPT_WA_FLAGS_PRESENT) {
         atomic_store_explicit(&g_host_workarounds, w, memory_order_relaxed);
         return w & ~(uint32_t)NPT_WA_FLAGS_PRESENT;
      }
   }
#endif
   return 0;
}

static void npt_device_destroy(struct npt_device *dev);

struct npt_device *
npt_device_create(void)
{
   struct npt_device *dev = npt_alloc(sizeof(*dev));
   if (!dev)
      return NULL;

   /* virtgpu inside a VM, vtest otherwise.  NPT_DEBUG=force_vtest skips
    * the virtgpu probe so the guest can be tested over a local
    * virgl_test_server (Neptune capset) Unix socket even when a
    * /dev/dri/renderDxxx is present. */
   if (!NPT_DEBUG(FORCE_VTEST))
      dev->renderer = npt_renderer_create_virtgpu();
   if (!dev->renderer)
      dev->renderer = npt_renderer_create_vtest();
   if (!dev->renderer) {
      npt_log("failed to create renderer");
      free(dev);
      return NULL;
   }

   atomic_store(&dev->next_ring_id, 1);
   npt_wrapper_cache_init(&dev->wrapper_cache);

   mtx_init(&dev->tls_rings_mutex, mtx_plain);
   list_inithead(&dev->tls_rings);
   npt_tls_once_init();

   mtx_init(&dev->instance_rings_mutex, mtx_plain);
   list_inithead(&dev->instance_rings);
   atomic_store_explicit(&dev->dc_sc_ring, (struct npt_ring *)NULL,
                         memory_order_relaxed);
   atomic_store_explicit(&dev->dc_sc_ring_refs, 0, memory_order_relaxed);

   /* Cached so per-call sites can branch without re-reading the env. */
   dev->multi_ring_enabled = NPT_PERF(MULTI_RING);

   struct npt_ring_layout layout;
   const uint32_t primary_size = dev->multi_ring_enabled
      ? NPT_RING_BUFFER_SIZE_MULTI
      : NPT_RING_BUFFER_SIZE_SINGLE;
   npt_ring_get_layout(primary_size, 0, &layout);

   uint64_t ring_id = atomic_fetch_add(&dev->next_ring_id, 1);
   dev->ring = npt_ring_create(dev, &layout, ring_id, false /* primary */);
   if (!dev->ring) {
      npt_log("failed to create ring");
      mtx_destroy(&dev->instance_rings_mutex);
      mtx_destroy(&dev->tls_rings_mutex);
      npt_wrapper_cache_fini(&dev->wrapper_cache);
      npt_renderer_destroy(dev->renderer);
      free(dev);
      return NULL;
   }

   npt_shmem_pool_init(&dev->data_pool, NPT_DATA_SHMEM_MIN_ALLOC);
   npt_shmem_pool_init(&dev->map_pool,  NPT_MAP_POOL_INITIAL_SIZE);

   /* Feedback pool is brought up on first CreateQuery / CreateFence. */
   mtx_init(&dev->feedback_pool_init_mutex, mtx_plain);
   atomic_store_explicit(&dev->feedback_pool_inited, false,
                         memory_order_relaxed);

   npt_event_init(dev);

   npt_log("Neptune device created successfully");
   return dev;
}

/* Drain wrappers the app forgot to Release.  An earlier wrapper's
 * aux_destroy may release sibling wrappers (e.g. swapchain's
 * buffer_cache holds backbuffer surface wrappers); those siblings can
 * already be destroyed by the time the foreach snapshot reaches them.
 * Verify the wrapper is still live in the cache before touching it.
 * The cache is the canonical liveness tracker -- npt_com_destroy
 * removes the entry as its first step. */
static void
npt_device_destroy_drain_wrapper(uint64_t host_id, void *wrapper,
                                 void *user_data)
{
   struct npt_device *dev = user_data;
   struct npt_wrapper_cache *c = &dev->wrapper_cache;
   const uint32_t h = npt_wrapper_cache_hash(host_id);

   mtx_lock(&c->mutex);
   bool live = false;
   for (struct npt_wrapper_cache_entry *e = c->buckets[h]; e; e = e->next) {
      if (e->host_id == host_id && e->wrapper == wrapper) {
         live = true;
         break;
      }
   }
   mtx_unlock(&c->mutex);

   if (!live)
      return;

   struct npt_com_base *com = wrapper;
   uint32_t pub = atomic_load(&com->base.pub_ref);
   npt_log("device_destroy: leaked wrapper host_id=0x%" PRIx64
           " pub_ref=%u -- forcing destroy", host_id, pub);
   npt_com_force_destroy(com);
}

bool
npt_device_is_shutting_down(const struct npt_device *dev)
{
   return dev && atomic_load_explicit(
                    &((struct npt_device *)dev)->shutting_down,
                    memory_order_acquire);
}

static void
npt_device_destroy(struct npt_device *dev)
{
   if (!dev)
      return;

   atomic_store_explicit(&dev->shutting_down, true, memory_order_release);

   /* Order: drain leaked wrappers (needs a live ring for COM_RELEASE)
    * -> stop event waiter -> data pool -> ring -> cache -> renderer. */
   npt_device_wrapper_cache_foreach(dev, npt_device_destroy_drain_wrapper,
                                    dev);
   npt_event_fini(dev);
   npt_shmem_pool_fini(dev->renderer, &dev->data_pool);
   npt_shmem_pool_fini(dev->renderer, &dev->map_pool);
   if (atomic_load_explicit(&dev->feedback_pool_inited,
                            memory_order_acquire)) {
      npt_shmem_pool_fini(dev->renderer, &dev->feedback_pool);
   }
   mtx_destroy(&dev->feedback_pool_init_mutex);

   /* Tear down TLS rings before primary.  Two-stage: this destroys
    * the ring and clears tr->ring; the tls_ring struct is freed by the
    * thread's tss dtor. */
   mtx_lock(&dev->tls_rings_mutex);
   list_for_each_entry_safe(struct npt_tls_ring, tr, &dev->tls_rings,
                            dev_head) {
      /* Reinit so concurrent thread-exit dtors see empty and skip. */
      list_delinit(&tr->dev_head);
      mtx_unlock(&dev->tls_rings_mutex);
      npt_tls_destroy_ring(tr);
      mtx_lock(&dev->tls_rings_mutex);
   }
   mtx_unlock(&dev->tls_rings_mutex);

   npt_ring_destroy(dev->ring);
   mtx_destroy(&dev->instance_rings_mutex);
   mtx_destroy(&dev->tls_rings_mutex);
   npt_wrapper_cache_fini(&dev->wrapper_cache);
   npt_renderer_destroy(dev->renderer);
   free(dev);
}

#if defined(NPT_D3D9_RUNTIME_DDI) || defined(NPT_D3D10_RUNTIME_DDI)

/* Runtime callback handles and rotating DMA buffers belong to one outer
 * runtime device. Sharing this transport across devices leaves stale handles
 * as soon as the original owner is destroyed. */
struct npt_device *
npt_device_acquire(void)
{
   struct npt_device *dev = npt_device_create();
   if (dev)
      atomic_init(&dev->runtime_refs, 1);
   return dev;
}

void
npt_device_release(struct npt_device *dev)
{
   if (dev && atomic_fetch_sub_explicit(&dev->runtime_refs, 1,
                                         memory_order_acq_rel) == 1)
      npt_device_destroy(dev);
}

#elif defined(__WINE__)

/* Wine PE build: the singleton state lives in the nptunix.so unixlib
 * (loaded once per Wine process) so d3d11.dll and dxgi.dll share a
 * single host context.  Without this every PE DLL would open its own
 * vtest connection, get its own forked virgl_render_server, and any
 * COM object created via dxgi.dll would be unknown to d3d11.dll's
 * context when forwarded across (e.g. an IDXGIAdapter handed to
 * D3D11CreateDevice). */

struct npt_device *
npt_device_acquire(void)
{
   if (npt_unixlib_ensure_init() != 0)
      return NULL;

   struct npt_unix_singleton_acquire_params ap = { 0 };
   npt_wine_unix_call(npt_unix_singleton_acquire, &ap);

   if (!ap.won_creation)
      return (struct npt_device *)ap.device_out;

   /* Elected to create.  Concurrent acquirers sleep until publish. */
   struct npt_device *dev = npt_device_create();

   struct npt_unix_singleton_publish_params pp = {
      .device = dev,
      .success = dev ? 1u : 0u,
   };
   npt_wine_unix_call(npt_unix_singleton_publish, &pp);

   return dev;
}

void
npt_device_release(struct npt_device *dev)
{
   (void)dev;
   if (npt_unixlib_ensure_init() != 0)
      return;

   struct npt_unix_singleton_release_params rp = { 0 };
   npt_wine_unix_call(npt_unix_singleton_release, &rp);

   /* Destroy outside the unixlib lock: device_destroy walks the cache
    * and tears down threads. */
   if (rp.device_out)
      npt_device_destroy((struct npt_device *)rp.device_out);
}

#else /* !__WINE__ */

struct npt_device *
npt_device_acquire(void)
{
   NPT_CALL_ONCE(g_npt_device_mutex_inited,
                 mtx_init(&g_npt_device_mutex, mtx_plain));

   mtx_lock(&g_npt_device_mutex);
   if (!g_npt_device) {
      g_npt_device = npt_device_create();
      if (!g_npt_device) {
         mtx_unlock(&g_npt_device_mutex);
         return NULL;
      }
   }
   g_npt_device_use_count++;
   struct npt_device *dev = g_npt_device;
   mtx_unlock(&g_npt_device_mutex);
   return dev;
}

void
npt_device_release(struct npt_device *dev)
{
   (void)dev;
   /* Defensive: release without a matching acquire would be a bug. */
   if (atomic_load(&g_npt_device_mutex_inited) != 2)
      return;

   mtx_lock(&g_npt_device_mutex);
   if (g_npt_device_use_count == 0) {
      mtx_unlock(&g_npt_device_mutex);
      return;
   }
   struct npt_device *to_destroy = NULL;
   if (--g_npt_device_use_count == 0) {
      to_destroy = g_npt_device;
      g_npt_device = NULL;
   }
   mtx_unlock(&g_npt_device_mutex);

   /* Destroy outside the singleton mutex: device_destroy walks the
    * cache and tears down threads -- holding the mutex would deadlock
    * a concurrent acquire mid-teardown. */
   if (to_destroy)
      npt_device_destroy(to_destroy);
}

#endif /* transport ownership */

void
npt_device_retain(struct npt_device *dev)
{
#if defined(NPT_D3D9_RUNTIME_DDI) || defined(NPT_D3D10_RUNTIME_DDI)
   if (dev)
      atomic_fetch_add_explicit(&dev->runtime_refs, 1, memory_order_relaxed);
#else
   (void)dev;
   npt_device_acquire();
#endif
}

struct npt_renderer_shmem *
npt_device_alloc_data(struct npt_device *dev, size_t size, size_t *out_offset)
{
   /* RESOURCE_UPDATE payloads ride the ring's WAIT_RING_SEQNO; no
    * extra freshness roundtrip needed. */
   return npt_shmem_pool_alloc(dev->renderer, &dev->data_pool,
                               size, out_offset, NULL);
}

struct npt_renderer_shmem *
npt_device_alloc_feedback_slot(struct npt_device *dev,
                               uint32_t slot_size,
                               uint32_t *out_offset,
                               bool *out_fresh)
{
   if (!slot_size)
      return NULL;
   slot_size = (slot_size + NPT_FEEDBACK_SLOT_ALIGN - 1u) &
               ~(NPT_FEEDBACK_SLOT_ALIGN - 1u);

   if (!atomic_load_explicit(&dev->feedback_pool_inited,
                             memory_order_acquire)) {
      mtx_lock(&dev->feedback_pool_init_mutex);
      if (!atomic_load_explicit(&dev->feedback_pool_inited,
                                memory_order_relaxed)) {
         npt_shmem_pool_init(&dev->feedback_pool,
                             NPT_FEEDBACK_POOL_INITIAL_SIZE);
         atomic_store_explicit(&dev->feedback_pool_inited, true,
                               memory_order_release);
      }
      mtx_unlock(&dev->feedback_pool_init_mutex);
   }

   size_t off = 0;
   bool fresh = false;
   struct npt_renderer_shmem *shmem =
      npt_shmem_pool_alloc(dev->renderer, &dev->feedback_pool,
                           slot_size, &off, &fresh);
   if (!shmem)
      return NULL;
   /* The shmem pool returns whatever bytes were last written; zero
    * before either side races a read. */
   void *base = (uint8_t *)shmem->mmap_ptr + off;
   memset(base, 0, slot_size);
   if (out_offset)
      *out_offset = (uint32_t)off;
   if (out_fresh)
      *out_fresh = fresh;
   return shmem;
}

struct npt_ring *
npt_device_method_ring(struct npt_device *dev)
{
   if (!dev)
      return NULL;
   struct npt_ring *r =
      atomic_load_explicit(&dev->dc_sc_ring, memory_order_acquire);
   return r ? r : dev->ring;
}
