/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Guest-side ring buffer.  Single-producer (guest writes tail) /
 * single-consumer (host writes head) circular buffer over a shmem
 * blob.  Used for the primary ring, per-instance (DC, swapchain)
 * rings, and TLS rings.
 */

#ifndef NPT_RING_H
#define NPT_RING_H

#include "npt_common.h"
#include "npt_cs.h"
#include "npt_profile.h"
#include "npt_renderer.h"
#include "npt_shmem_pool.h"
#include "npt_transport_defs.h"

/*
 * Shmem layout (64-byte aligned):
 *   0:   head (host writes, guest reads)
 *   64:  tail (guest writes, host reads)
 *   128: status (host writes, guest reads; NPT_RING_STATUS_*_BIT)
 *   192: circular buffer
 *   192 + buffer_size: extra region
 */

struct npt_ring_layout {
   size_t head_offset;
   size_t tail_offset;
   size_t status_offset;

   size_t buffer_offset;
   size_t buffer_size;

   size_t extra_offset;
   size_t extra_size;

   size_t shmem_size;
};

struct npt_device;

struct npt_ring {
   uint64_t id;
   struct npt_renderer *renderer;
   /* Backpointer used by release-batch flush (cross-ring drain) and
    * by TLS rings to find primary on teardown. */
   struct npt_device *device;
   bool is_tls_ring;
   /* Linked into dev->instance_rings for DC / SC wrappers; empty
    * otherwise. */
   struct list_head instance_head;
   struct npt_renderer_shmem *shmem;

   uint32_t buffer_size;
   uint32_t buffer_mask;

   const volatile atomic_uint *head;
   volatile atomic_uint *tail;
   volatile atomic_uint *status;
   /* Host-written once at ring create: the backend workaround flags word
    * (NPT_WA_* | NPT_WA_FLAGS_PRESENT). Read by npt_host_workaround_flags(). */
   const volatile atomic_uint *wa_word;
   void *buffer;
   void *extra;

   uint32_t cur;

   /* A renderer-submit failure, fatal host status, or bounded-wait timeout
    * poisons the ring.  No later command may turn that asynchronous failure
    * into apparent success. */
   atomic_bool failed;

   mtx_t mutex;

   /* Rate-limit idle notifications: only notify if
    * NPT_RING_IDLE_TIMEOUT_NS elapsed since the last one, to avoid
    * EXECBUFFER round-trips when the host is briefly idle. */
   int64_t last_notify;
   int64_t next_notify;

   /* Bump allocator for per-call reply windows.  Each sync submission
    * carves a region and SET_REPLY_STREAMs it just before the command,
    * so concurrent sync calls don't fight over a fixed slot. */
   struct npt_shmem_pool reply_pool;

   /* Indirect ("upload") shmem: commands larger than direct_size are
    * copied here and referenced by EXECUTE_COMMAND_STREAM (res_id,
    * offset, size).  Keeps the ring small while allowing multi-MiB
    * RESOURCE_UPDATE.  Doubles on demand; resets to offset 0 once the
    * host has caught up past upload_horizon_seqno. */
   struct npt_renderer_shmem *upload_shmem;
   uint32_t upload_size;
   uint32_t upload_used;
   uint32_t upload_horizon_seqno;

   /* buffer_size >> NPT_RING_DIRECT_ORDER. */
   uint32_t direct_size;

   /* Bumped each time npt_ring_relax sees ALIVE cleared; reset on
    * ALIVE set.  abort()s if it hits the threshold so a wedged host
    * fails loudly instead of spinning forever. */
   uint32_t watchdog_misses;

   /* Virtqueue-seqno roundtrip counter; advanced under ring->mutex. */
   uint64_t roundtrip_next;

   struct npt_profile_ring profile;

   /* Global list of live rings for the profile dump walker. */
   struct list_head profile_head;
};

void
npt_ring_get_layout(size_t buf_size, size_t extra_size,
                    struct npt_ring_layout *layout);

struct npt_ring *
npt_ring_create(struct npt_device *device,
                const struct npt_ring_layout *layout,
                uint64_t ring_id,
                bool is_tls_ring);

void
npt_ring_destroy(struct npt_ring *ring);

/* Block until the host has advanced `head` past `cur` (no
 * un-dispatched bytes). */
bool
npt_ring_wait_all(struct npt_ring *ring);

/* Wait until the host has consumed every command published before this call.
 * This variant lets the caller select the hard deadline.  Both wait functions
 * report a fatal ring or timeout to the caller. */
bool
npt_ring_wait_all_timeout(struct npt_ring *ring, uint32_t timeout_ms);

/* Report poison or a fatal host status without submitting more work. */
bool
npt_ring_is_healthy(struct npt_ring *ring);

/* Used for transport commands that must stay ordered with ring
 * commands (notably RESOURCE_UPDATE, which must precede any Draw
 * that reads the buffer); going via submit_cmd would race the ring
 * thread on a different host thread. */
bool
npt_ring_submit_raw(struct npt_ring *ring, const void *data, uint32_t size);

/* `out_seqno` is the value `head` must reach for the bytes to be
 * fully consumed -- the N-slot rename ring uses it to decide when a
 * shmem slot is safe to reuse. */
bool
npt_ring_submit_raw_seqno(struct npt_ring *ring, const void *data,
                          uint32_t size, uint32_t *out_seqno);

/* Submit [header || payload || padding] without forcing the caller to
 * first coalesce into one malloc'd buffer.  Reserves
 * header_size + payload_padded_size bytes in the ring (so the host
 * parser advances correctly), but only memcpy's payload_size bytes
 * from `payload` -- padding tail bytes are left as whatever was
 * already in the ring/upload_shmem.  Used by RESOURCE_UPDATE where
 * payload_size can be multi-MB; saves a malloc + memcpy per
 * subresource on texture initial-data uploads. */
bool
npt_ring_submit_raw_with_payload(struct npt_ring *ring,
                                 const void *header, uint32_t header_size,
                                 const void *payload, uint32_t payload_size,
                                 uint32_t payload_padded_size);

/*
 * Force a virtqueue-seqno roundtrip: SUBMIT via the renderer's sync
 * path + matching WAIT through the ring so the ring thread blocks
 * until the host's virtqueue dispatcher catches up.  Required before
 * any ring command that names a freshly-created shmem's res_id
 * (e.g. REGISTER_QUERY_FEEDBACK), otherwise the ring thread races
 * the resource-table register.
 */
bool
npt_ring_force_roundtrip(struct npt_ring *ring);

/* Yield-heavy poll until head is past seqno.  Returns the relax-iteration
 * count.  UINT32_MAX reports a fatal ring or timeout. */
uint32_t
npt_ring_wait_seqno(struct npt_ring *ring, uint32_t seqno);

/*
 * Queue a COM_RELEASE on the device's primary ring.  Routing every
 * release through primary lets the cross-ring drain barrier (waits
 * for TLS / instance rings to dispatch past their tail) cover releases
 * from any ring with a single barrier — closes the UAF window where
 * a release here could race a pending async use of host_id on
 * another thread's ring.
 */
bool
npt_ring_send_com_release(struct npt_device *dev, uint64_t host_id);

#endif /* NPT_RING_H */
