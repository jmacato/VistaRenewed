/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Neptune device: per-runtime-device transport for D3D9/10 DDIs,
 * otherwise a refcounted process singleton. Owns the
 * renderer, primary ring, wrapper cache, TLS / DC-SC / instance
 * rings, data shmem pool, and Win32 event emulation.
 */

#ifndef NPT_DEVICE_H
#define NPT_DEVICE_H

#include "npt_common.h"
#include "npt_renderer.h"
#include "npt_ring.h"
#include "npt_object.h"
#include "npt_shmem_pool.h"

/*
 * Wrapper cache: host_id -> guest wrapper, so a host pointer returned
 * twice (e.g. from GetParent) maps to the same wrapper.  Holds a weak
 * reference; the wrapper removes itself in Release.
 */
#define NPT_WRAPPER_CACHE_BUCKETS 1024

struct npt_wrapper_cache_entry {
   uint64_t host_id;
   void *wrapper;
   struct npt_wrapper_cache_entry *next;
};

struct npt_wrapper_cache {
   mtx_t mutex;
   struct npt_wrapper_cache_entry *buckets[NPT_WRAPPER_CACHE_BUCKETS];
   uint32_t count;
};

struct npt_device {
#if defined(NPT_D3D9_RUNTIME_DDI) || defined(NPT_D3D10_RUNTIME_DDI)
   atomic_uint runtime_refs;
#endif
   struct npt_renderer *renderer;
   /* Primary ring; fallback when TLS rings are disabled. */
   struct npt_ring *ring;

   struct npt_shmem_pool data_pool;

   /* D3D11 Map shadow-staging pool: every npt_d3d_map_ring slot
    * sub-allocates here instead of creating its own RESOURCE_CREATE_BLOB,
    * since per-resource shmems make qemu's ram_block_add list walk
    * (O(n) in find_ram_offset) the dominant cost.  Pool grows on
    * overflow; old blocks linger via per-slot refs until those slots
    * release. */
   struct npt_shmem_pool map_pool;

   /* Shared-memory feedback slots (D3D11 queries, ID3D11Fence, ...);
    * the host writes status so guest reads stay local.  Bump
    * allocator: grown shmems stay alive via per-wrapper refs taken
    * at allocation, so existing slots remain valid.  Lazily inited
    * so non-feedback workloads pay nothing. */
   mtx_t feedback_pool_init_mutex;
   _Atomic bool feedback_pool_inited;
   struct npt_shmem_pool feedback_pool;

   _Atomic uint64_t next_ring_id;

   /* Per-thread TLS ring list.  Walked by device_destroy and by the
    * cross-ring drain barrier. */
   mtx_t tls_rings_mutex;
   struct list_head tls_rings;

   /* Per-instance rings for single-threaded-per-instance interfaces;
    * walked alongside tls_rings by the drain barrier. */
   mtx_t instance_rings_mutex;
   struct list_head instance_rings;

   /* Shared ring for DC + SC: DXGI requires Present on the immediate-
    * context thread and the host D3D library has no internal locking
    * to coordinate DC vs SC vs Device, so DC and SC must share one
    * dispatch thread.  Lazy under instance_rings_mutex.  Torn down
    * when dc_sc_ring_refs hits zero. */
   _Atomic(struct npt_ring *) dc_sc_ring;
   _Atomic uint32_t dc_sc_ring_refs;

   /*
    * Multi-ring toggle: per-thread TLS rings for non-DC/SC traffic
    * AND the shared DC/SC instance ring.  Off => everything funnels
    * to primary.  Set once in npt_device_create from NPT_PERF_MULTI_RING.
    */
   bool multi_ring_enabled;

   struct npt_wrapper_cache wrapper_cache;

   /* Win32 event-HANDLE emulation; opaque, defined in npt_event.c. */
   struct npt_event_state *event;

   /* Set at the start of npt_device_destroy.  See sc_wsi_stop for the
    * one path that consumes it (skip an INFINITE thread join that can
    * stall on a server-side-closed X11 connection). */
   _Atomic bool shutting_down;
};

/*
 * Primary-ring size:
 *   Multi-ring: primary carries only COM_RELEASE_BATCH and stragglers;
 *   the DC/SC and TLS rings soak up high-volume traffic, so 256 KiB is
 *   plenty.
 *   Single-ring: primary IS the whole bus, so 2 MiB to absorb Create
 *   and Release bursts.  Keeps the direct/indirect threshold
 *   (buffer_size >> 4) at 128 KiB, large enough that most
 *   RESOURCE_UPDATE payloads stay on the fast inline path.
 */
#define NPT_RING_BUFFER_SIZE_MULTI   (256u * 1024u)
#define NPT_RING_BUFFER_SIZE_SINGLE  (2u * 1024u * 1024u)
/* Power-of-two initial; pool doubles on growth and never recycles
 * slot offsets, so old shmems just stick around. */
#define NPT_FEEDBACK_POOL_INITIAL_SIZE (16u * 1024u)

/* Cache-line align so neighbouring slots don't false-share between
 * the host poll thread and guest readers. */
#define NPT_FEEDBACK_SLOT_ALIGN 64u

/* Same 16 KiB sizing as TLS rings -- DC/SC method dispatches are small
 * and latency dominates. */
#define NPT_DC_SC_RING_BUFFER_SIZE   (16u * 1024u)
#define NPT_DATA_SHMEM_MIN_ALLOC   (64 * 1024)
/* Map-pool starts large enough to hold a fair working set of dynamic
 * CB / vertex-buffer Maps without immediately growing.  Doubles on
 * demand up to whatever the renderer caps us at. */
#define NPT_MAP_POOL_INITIAL_SIZE  (16u * 1024u * 1024u)

struct npt_device *
npt_device_create(void);

struct npt_renderer_shmem *
npt_device_alloc_data(struct npt_device *dev, size_t size, size_t *out_offset);

/*
 * Allocate a feedback slot (rounded to NPT_FEEDBACK_SLOT_ALIGN).
 * Caller takes a ref on the returned shmem.  *out_fresh==true means
 * the host hasn't seen this shmem yet -- caller must issue a
 * virtqueue-seqno roundtrip before sending any command that names
 * the res_id.  NULL return => caller falls back to the sync wire path.
 */
struct npt_renderer_shmem *
npt_device_alloc_feedback_slot(struct npt_device *dev,
                               uint32_t slot_size,
                               uint32_t *out_offset,
                               bool *out_fresh);

/*
 * Ring for handwritten paths that touch DC state (Map/Unmap, fence
 * arming, CreateBuffer initial data, ...): the shared DC/SC ring if
 * lazily created, else primary.  Falls back to primary on the first
 * setup calls (D3D11CreateDevice) which don't race DC state.
 */
struct npt_ring *
npt_device_method_ring(struct npt_device *dev);

/* Hit returns the wrapper with refcount bumped (caller owns the new
 * ref), miss returns NULL. */
void *
npt_device_wrapper_cache_lookup(struct npt_device *dev, uint64_t host_id);

/*
 * Insert if absent, else bump the existing wrapper's pub_ref and
 * return it (caller must free its own).  NULL return either way:
 * `new_wrapper` won the race, OR the existing wrapper was un-
 * resurrectable (pub_ref==0) -- in which case *retry_needed is true
 * and the caller should yield and re-lookup.
 */
void *
npt_device_wrapper_cache_insert_if_absent(struct npt_device *dev,
                                          uint64_t host_id,
                                          void *new_wrapper,
                                          bool *retry_needed);

void
npt_device_wrapper_cache_remove(struct npt_device *dev, uint64_t host_id);

/* Liveness check: true iff (host_id -> wrapper) is still mapped in the
 * device cache.  Used by per-wrapper sibling caches (e.g. swapchain's
 * buffer_cache) to verify a cached wrapper pointer is still valid
 * before dropping its ref -- npt_com_destroy's first step is cache_
 * remove, so any wrapper still in the cache is alive. */
bool
npt_device_wrapper_cache_is_live(struct npt_device *dev,
                                 uint64_t host_id, const void *wrapper);

/* `cb` may call cache_remove on its own entry but must not otherwise
 * mutate the cache.  Used at device teardown to drain leaked
 * wrappers' COM refs. */
typedef void (*npt_wrapper_cache_iter_fn)(uint64_t host_id, void *wrapper,
                                          void *user_data);
void
npt_device_wrapper_cache_foreach(struct npt_device *dev,
                                 npt_wrapper_cache_iter_fn cb,
                                 void *user_data);

/*
 * Refcounted transport (per acquisition for Vista D3D9, otherwise singleton).
 * Each top-level COM wrapper holds one
 * acquire-ref (children ride parent-priv_ref coupling instead).
 * Last release tears down the device.  NULL return = creation
 * failure. Retain pins an existing transport without creating another.
 */
struct npt_device *
npt_device_acquire(void);

void
npt_device_release(struct npt_device *dev);

void
npt_device_retain(struct npt_device *dev);

/* Cache backend flags after the first synchronous host device reply. */
void
npt_device_cache_workarounds(struct npt_device *dev);

bool
npt_device_is_shutting_down(const struct npt_device *dev);

#endif /* NPT_DEVICE_H */
