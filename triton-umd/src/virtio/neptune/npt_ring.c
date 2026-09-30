/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 */

#include "npt_ring.h"
#include "npt_workaround.h"
#include "npt_device.h"
#include "npt_env.h"
#include "npt_profile.h"
#include "npt_tls.h"

#include "util/os_misc.h"
#ifndef _WIN32
#include <errno.h>
#include <sys/resource.h>
#include "util/os_time.h"
#endif

#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h> /* __cpuid: detect x86-on-non-x86 translators (FEX/TCG) */
#endif

static_assert(ATOMIC_INT_LOCK_FREE == 2 && sizeof(atomic_uint) == 4,
              "npt_ring requires always-lock-free 32-bit atomic_uint");

static bool
npt_ring_notify(struct npt_ring *ring);

#define NPT_RING_IDLE_TIMEOUT_NS (1000000) /* 1ms */

/* Must be > host's ALIVE refresh + scheduler jitter so a healthy host
 * isn't killed. */
#define NPT_RING_WATCHDOG_REPORT_PERIOD_US (3 * 1000 * 1000)

/* buffer_size >> NPT_RING_DIRECT_ORDER threshold. */
#define NPT_RING_DIRECT_ORDER 4u

/* Grows on demand. */
#define NPT_RING_UPLOAD_INITIAL_SIZE (1u << 20)

/* Grows on demand. */
#define NPT_RING_REPLY_POOL_MIN_SIZE (1u << 20)

/* No guest caller can wait forever for a host that has failed or disappeared. */
#define NPT_RING_WAIT_TIMEOUT_MS 15000u

static uint64_t
npt_ring_monotonic_ms(void)
{
#ifdef _WIN32
   return (uint64_t)GetTickCount64();
#else
   return (uint64_t)(os_time_get_nano() / 1000000);
#endif
}

static bool
npt_ring_mark_failed(struct npt_ring *ring, const char *reason)
{
   if (!ring)
      return false;
   if (!atomic_exchange_explicit(&ring->failed, true, memory_order_acq_rel))
      npt_log("ring %llu failed: %s", (unsigned long long)ring->id,
              reason ? reason : "unknown transport failure");
   return false;
}

static bool
npt_ring_is_failed(struct npt_ring *ring)
{
   if (!ring)
      return true;
   if (atomic_load_explicit(&ring->failed, memory_order_acquire))
      return true;
   if (ring->status &&
       (atomic_load_explicit(ring->status, memory_order_acquire) &
        NPT_RING_STATUS_FATAL_BIT)) {
      npt_ring_mark_failed(ring, "host reported fatal status");
      return true;
   }
   return false;
}

bool
npt_ring_is_healthy(struct npt_ring *ring)
{
   return !npt_ring_is_failed(ring);
}

/* A failed CREATE or DESTROY acknowledgement leaves the host's access to the
 * shared mappings uncertain.  Leak those mappings, but remove and free the
 * guest-only ring record so profile walks and later device teardown cannot
 * dereference a stale renderer or device pointer. */
static void
npt_ring_abandon_local_record(struct npt_ring *ring)
{
   if (!ring)
       return;
   npt_profile_unregister_ring(ring);
   /* Preserve every host-visible shmem reference, but retire both guest-only
    * mutexes before freeing their containing record. */
   mtx_destroy(&ring->reply_pool.mutex);
   mtx_destroy(&ring->mutex);
   free(ring);
}

/*
 * Whether an atomic read-modify-write works on the ring's write-combining
 * shmem.  ARM LSE atomics (ldclral) take a Data Abort on WC / Normal
 * Non-cacheable memory, and x86-on-ARM translators lower a LOCK-prefixed RMW
 * onto them, so both native ARM64 and translated x86 fault; plain loads and
 * stores are fine, so only the watchdog's ALIVE clear (the ring's sole atomic
 * RMW) is affected.
 *
 * -1 = undetected, 0 = unsupported (emulated), 1 = supported.  Detected once via
 * CPUID -- a passive read, no faulting: the translator self-identifies on the
 * hypervisor vendor leaf.  Real-x86 hypervisors (KVM/VMware/Hyper-V) also set
 * the present bit but do atomics fine, so we key off the specific translator
 * signatures, not the bit alone.
 */
static atomic_int npt_wc_atomic_ok = -1;

static int
npt_detect_wc_atomic_ok(void)
{
#if defined(_M_ARM64EC) || defined(__arm64ec__) || defined(_M_ARM64) || \
   defined(__aarch64__)
   /* All ARM64 flavors (native and arm64ec): the compiler lowers
    * atomic_fetch_and to an LSE atomic (ldclral) -- MSVC native arm64 does so
    * too, behind a runtime CPU-feature check that passes on every Apple
    * Silicon host -- and LSE atomics take a Data Abort on Write-Combining /
    * Non-cacheable memory.  The ring shmem is mapped WC, so the ALIVE-clear
    * faults on any wait that outlives the watchdog threshold.  Disable it;
    * the watchdog degrades to a no-op, as on the x86-emulation path. */
   return 0;
#elif defined(__x86_64__) || defined(__i386__)
   unsigned a, b, c, d;

   /* No hypervisor/emulator present bit -> bare-metal x86 -> atomics fine. */
   __cpuid(1, a, b, c, d);
   if (!(c & (1u << 31)))
      return 1;

   /* Hypervisor vendor leaf 0x40000000: 12-byte signature in EBX,ECX,EDX. */
   __cpuid(0x40000000u, a, b, c, d);
   (void)a;
   char sig[13];
   memcpy(sig + 0, &b, 4);
   memcpy(sig + 4, &c, 4);
   memcpy(sig + 8, &d, 4);
   sig[12] = '\0';

   if (!memcmp(sig, "FEXIFEXIEMU", 12) ||   /* FEX-Emu */
       !memcmp(sig, "TCGTCGTCGTCG", 12))    /* QEMU TCG (qemu-user / full TCG) */
      return 0;                             /* x86-on-non-x86: WC atomics fault */

   return 1; /* real-x86 hypervisor or an emulator that does atomics correctly */
#else
   return 1; /* other architectures: no known WC-atomic restriction */
#endif
}

/*
 * Adaptive backoff with watchdog.  After warn_order iters, check the
 * ring's ALIVE bit (host sets every NPT_RING_WATCHDOG_REPORT_PERIOD_US);
 * too many misses = host wedged.  `ring` may be NULL when the watchdog
 * isn't wanted (pre-shmem-map paths).
 */
static void
npt_ring_relax(struct npt_ring *ring, uint32_t *iter)
{
   /* busy_wait_order=8 (256 yields) hides a virtio-gpu HOST3D blob
    * page-staleness race: each yield is a scheduler slice for the
    * kernel to reconcile the guest VMA with the host memfd pages.  A
    * shorter budget lets reply reads see stale pre-submission bytes. */
   const uint32_t busy_wait_order = 8;
   const uint32_t base_sleep_us = 160;
   /* Warn at iter=4096 (~3.5s slept). */
   const uint32_t warn_order = 12;
   const uint32_t max_misses = 3;

   (*iter)++;
   if (*iter < (1u << busy_wait_order)) {
      thrd_yield();
   } else {
      const uint32_t shift = util_last_bit(*iter) - busy_wait_order - 1;
      npt_relax_sleep_us(base_sleep_us << shift);
   }

   if (ring && *iter >= (1u << warn_order) &&
       !(*iter & ((1u << warn_order) - 1))) {
      const uint32_t status =
         atomic_load_explicit(ring->status, memory_order_seq_cst);
      if (status & NPT_RING_STATUS_FATAL_BIT)
         return;

      if (status & NPT_RING_STATUS_ALIVE_BIT) {
         /* Clearing ALIVE (so a host re-set is detectable) is the ring's only
          * atomic RMW.  Skip it where WC atomics fault -- emulated x86-on-ARM
          * and every ARM64 flavor, per npt_detect_wc_atomic_ok.  With nothing
          * clearing ALIVE, the wedge report below stops firing once the host
          * has set it, leaving only the "host never came alive" case.  Native
          * x86 keeps the full watchdog. */
         if (atomic_load_explicit(&npt_wc_atomic_ok, memory_order_relaxed) == 1)
            atomic_fetch_and_explicit(ring->status,
                                      ~(unsigned)NPT_RING_STATUS_ALIVE_BIT,
                                      memory_order_seq_cst);
         ring->watchdog_misses = 0;
      } else {
         if (++ring->watchdog_misses >= max_misses) {
            /* Soft-abort: log instead of abort() so a developer can
             * attach a debugger to the wedged host render server.
             * Per-call-site (no static once-flag) so a deadlock that
             * advances multiple call sites is observable. */
            const uint32_t head =
               atomic_load_explicit(ring->head, memory_order_acquire);
            const uint32_t published =
               atomic_load_explicit(ring->tail, memory_order_acquire);
            npt_log("watchdog: ring %llu wedged after %u iters, head=%u tail=%u "
                    "status=0x%x is_tls=%d",
                    (unsigned long long)ring->id, *iter, head, published, status,
                    (int)ring->is_tls_ring);
            ring->watchdog_misses = 0;
         }
      }
   }
}

void
npt_ring_get_layout(size_t buf_size, size_t extra_size,
                    struct npt_ring_layout *layout)
{
   assert(buf_size && util_is_power_of_two_nonzero(buf_size));

   /* Explicit constants instead of alignas for MinGW portability. */
   layout->head_offset = 0;
   layout->tail_offset = 64;
   layout->status_offset = 128;
   layout->buffer_offset = 192;
   layout->buffer_size = buf_size;
   layout->extra_offset = layout->buffer_offset + buf_size;
   layout->extra_size = extra_size;
   layout->shmem_size = layout->extra_offset + extra_size;
}

static inline uint32_t
npt_ring_load_head(const struct npt_ring *ring)
{
   return atomic_load_explicit(ring->head, memory_order_acquire);
}

static inline void
npt_ring_store_tail(struct npt_ring *ring)
{
   /* Publish command bytes before the tail, and the tail before reading
    * host status. The second edge is Store->Load: SFENCE before a relaxed
    * tail store does not provide it, even on x86. Together with the host's
    * IDLE publication before its tail recheck, this prevents lost wakeups.
    * The full fence also drains command writes from WC mappings. */
   /* Do not use a seq_cst store here: x86 compilers implement it with
    * XCHG on the WC mapping, which translated x86/ARM cannot safely RMW.
    * The first fence drains WC payload writes before publishing the tail;
    * the second supplies the Store->Load edge for the idle handshake. */
   atomic_thread_fence(memory_order_seq_cst);
   atomic_store_explicit(ring->tail, ring->cur, memory_order_release);
   atomic_thread_fence(memory_order_seq_cst);
}

static inline uint32_t
npt_ring_load_status(const struct npt_ring *ring)
{
   /* acquire is enough: the producer side only reads status to test
    * IDLE / FATAL bits; release-on-write from host pairs with this. */
   return atomic_load_explicit(ring->status, memory_order_acquire);
}

static inline bool
npt_ring_has_space(const struct npt_ring *ring, uint32_t size)
{
   const uint32_t head = npt_ring_load_head(ring);
   return ring->cur + size - head <= ring->buffer_size;
}

static bool
npt_ring_wait_space(struct npt_ring *ring, uint32_t size)
{
   if (!ring || !size || size > ring->buffer_size || npt_ring_is_failed(ring))
      return false;

   if (likely(npt_ring_has_space(ring, size)))
      return true;

   /* Wait with adaptive backoff -- raw thrd_yield burns CPU when the
    * host falls behind. */
   uint32_t iter = 0;
   const uint64_t start = npt_ring_monotonic_ms();
   while (!npt_ring_has_space(ring, size)) {
      if (npt_ring_is_failed(ring))
         return false;
      if (npt_ring_monotonic_ms() - start >= NPT_RING_WAIT_TIMEOUT_MS)
         return npt_ring_mark_failed(ring, "wait for ring space timed out");
      if (iter && (iter & 0x7fff) == 0) {
         const uint32_t head =
            atomic_load_explicit(ring->head, memory_order_acquire);
         const uint32_t tail =
            atomic_load_explicit(ring->tail, memory_order_acquire);
         npt_log("wait_space: ring %llu size=%u head=%u tail=%u cur=%u iter=%u",
                 (unsigned long long)ring->id, size, head, tail, ring->cur, iter);
      }
      npt_ring_relax(ring, &iter);
   }

   npt_profile_record_full_wait(&ring->profile, iter);
   return true;
}

/* Wrap-aware write of `size` bytes from `data` starting at `cur`.
 * Does NOT advance ring->cur. */
static void
npt_ring_write_buffer_at(struct npt_ring *ring, uint32_t cur,
                         const void *data, uint32_t size)
{
   const uint32_t offset = cur & ring->buffer_mask;

   if (offset + size <= ring->buffer_size) {
      memcpy((uint8_t *)ring->buffer + offset, data, size);
   } else {
      const uint32_t s = ring->buffer_size - offset;
      memcpy((uint8_t *)ring->buffer + offset, data, s);
      memcpy(ring->buffer, (const uint8_t *)data + s, size - s);
   }
}

static void
npt_ring_write_buffer(struct npt_ring *ring,
                      const void *data, uint32_t size)
{
   npt_ring_write_buffer_at(ring, ring->cur, data, size);
   ring->cur += size;
}

/* Caller holds ring->mutex. */
static bool
npt_ring_submit_locked(struct npt_ring *ring,
                       const void *data, uint32_t size)
{
   if (!ring || !data || !size || !npt_ring_wait_space(ring, size))
      return false;
   npt_ring_write_buffer(ring, data, size);
   npt_ring_store_tail(ring);

   const uint32_t status = npt_ring_load_status(ring);
   if (unlikely(status & NPT_RING_STATUS_FATAL_BIT)) {
      return npt_ring_mark_failed(ring, "fatal status after submit");
   }

   if (status & NPT_RING_STATUS_IDLE_BIT) {
      /* IDLE is a promise to sleep, including after a spurious wake or a
       * feedback poll. An earlier doorbell cannot cover this submission. */
      if (!npt_ring_notify(ring))
         return false;
   }

   return true;
}

static bool
npt_ring_submit_dispatch_locked(struct npt_ring *ring, const void *data,
                                uint32_t size);

/*
 * Cross-ring drain barrier: wait for every TLS / instance ring to
 * dispatch past its current tail.  Closes the UAF window where a
 * COM_RELEASE(X) on primary could race a pending async use of X on
 * another thread's ring.
 *
 * Caller MUST hold primary->mutex on entry; we drop it for the drain
 * (holding it across long TLS-ring waits would serialize every other
 * primary path and starve the watchdog) and re-acquire before return.
 */
static bool
npt_ring_drain_peer_rings_unlocked(struct npt_ring *primary)
{
   struct npt_device *dev = primary->device;
   if (!dev || primary != dev->ring)
      return true;

   bool ok = true;

   mtx_unlock(&primary->mutex);

   mtx_lock(&dev->tls_rings_mutex);
   list_for_each_entry(struct npt_tls_ring, tr, &dev->tls_rings, dev_head) {
      mtx_lock(&tr->mutex);
      struct npt_ring *tr_ring =
         atomic_load_explicit(&tr->ring, memory_order_acquire);
      if (tr_ring && tr_ring != primary)
         ok = npt_ring_wait_all(tr_ring) && ok;
      mtx_unlock(&tr->mutex);
   }
   mtx_unlock(&dev->tls_rings_mutex);

   mtx_lock(&dev->instance_rings_mutex);
   list_for_each_entry(struct npt_ring, ir, &dev->instance_rings,
                       instance_head) {
      if (ir != primary)
         ok = npt_ring_wait_all(ir) && ok;
   }
   mtx_unlock(&dev->instance_rings_mutex);

   mtx_lock(&primary->mutex);
   if (!ok)
      return npt_ring_mark_failed(primary, "peer-ring drain failed");
   return true;
}

bool
npt_ring_send_com_release(struct npt_device *dev, uint64_t host_id)
{
   if (!dev || !dev->ring || !host_id)
      return false;

   /* Drain peer rings before the release crosses the wire so an
    * in-flight async use of host_id on a TLS / instance ring is
    * fully observed by the host before the release. */
   struct npt_ring *primary = dev->ring;
   mtx_lock(&primary->mutex);
   if (!npt_ring_drain_peer_rings_unlocked(primary)) {
      mtx_unlock(&primary->mutex);
      return false;
   }

   struct npt_cmd_com_release cmd;
   memset(&cmd, 0, sizeof(cmd));
   cmd.header.cmd_type =
      NPT_TRANSPORT_CMD_TYPE(NPT_TRANSPORT_SUBGROUP_COM,
                             NPT_TRANSPORT_COM_RELEASE);
   cmd.header.cmd_size = sizeof(cmd);
   cmd.header.object_id = host_id;
   const bool ok = npt_ring_submit_dispatch_locked(primary, &cmd, sizeof(cmd));
   mtx_unlock(&primary->mutex);
   return ok;
}

static bool
npt_ring_notify(struct npt_ring *ring)
{
   struct npt_cmd_notify_ring cmd;
   memset(&cmd, 0, sizeof(cmd));
   cmd.header.cmd_type = NPT_TRANSPORT_CMD_TYPE(NPT_TRANSPORT_SUBGROUP_RING,
                                                NPT_TRANSPORT_RING_NOTIFY);
   cmd.header.cmd_size = sizeof(cmd);
   cmd.ring_id = ring->id;

   npt_profile_record_notify(&ring->profile);
   if (!npt_renderer_submit_cmd(ring->renderer, &cmd, sizeof(cmd)))
      return npt_ring_mark_failed(ring, "ring notification submit failed");
   return true;
}

static inline bool
npt_ring_seqno_status(const struct npt_ring *ring, uint32_t seqno)
{
   /* The int32 wrap-safe comparison is only valid for a half-counter
    * (~2GB).  A long-lived slot tracker can hold a pending_seqno the
    * ring has long since pumped past, so the gap exceeds INT32_MAX
    * and the comparison wraps to "not reached".  Use the physical
    * invariant (head lags the write pointer by at most buffer_size):
    * if the unsigned distance from seqno to cur is one buffer's
    * worth, head has unconditionally passed it. */
   /* The published atomic tail is the producer progress visible to peer
    * threads.  Reading non-atomic cur here was a C data race. */
   const uint32_t published =
      atomic_load_explicit(ring->tail, memory_order_acquire);
   if ((uint32_t)(published - seqno) >= ring->buffer_size)
      return true;
   return (int32_t)(npt_ring_load_head(ring) - seqno) >= 0;
}

bool
npt_ring_wait_all(struct npt_ring *ring)
{
   return npt_ring_wait_all_timeout(ring, NPT_RING_WAIT_TIMEOUT_MS);
}

bool
npt_ring_wait_all_timeout(struct npt_ring *ring, uint32_t timeout_ms)
{
   if (!ring || !timeout_ms || npt_ring_is_failed(ring))
      return false;

   /* Snapshot the published tail.  Commands submitted after this point do
    * not belong to this drain.  The caller serializes its own command stream
    * before it takes this snapshot. */
   const uint32_t target =
      atomic_load_explicit(ring->tail, memory_order_acquire);
   const uint64_t start = npt_ring_monotonic_ms();
   uint32_t iter = 0;

   while (!npt_ring_seqno_status(ring, target)) {
      const uint32_t status = npt_ring_load_status(ring);
      if (status & NPT_RING_STATUS_FATAL_BIT) {
         npt_log("wait_all_timeout: ring %llu became fatal at target=%u",
                 (unsigned long long)ring->id, target);
         return npt_ring_mark_failed(ring, "fatal status during ring drain");
      }
      if ((npt_ring_monotonic_ms() - start) >= timeout_ms) {
         const uint32_t head =
            atomic_load_explicit(ring->head, memory_order_acquire);
         const uint32_t published =
            atomic_load_explicit(ring->tail, memory_order_acquire);
         npt_log("wait_all_timeout: ring %llu timed out after %u ms "
                 "target=%u head=%u tail=%u status=0x%x",
                 (unsigned long long)ring->id, timeout_ms, target, head,
                 published, status);
         return npt_ring_mark_failed(ring, "ring drain timed out");
      }
      npt_ring_relax(NULL, &iter);
   }

   return true;
}

uint32_t
npt_ring_wait_seqno(struct npt_ring *ring, uint32_t seqno)
{
   /* Each yield is a kernel scheduler slice for virtio-gpu page
    * bookkeeping that reconciles the guest VMA with the host memfd
    * pages -- without that, reads after head-advance can see stale
    * bytes. */
   if (npt_ring_is_failed(ring))
      return UINT32_MAX;
   uint32_t iter = 0;
   const uint64_t start = npt_ring_monotonic_ms();
   while (!npt_ring_seqno_status(ring, seqno)) {
      if (npt_ring_is_failed(ring))
         return UINT32_MAX;
      if (npt_ring_monotonic_ms() - start >= NPT_RING_WAIT_TIMEOUT_MS) {
         npt_ring_mark_failed(ring, "sequence wait timed out");
         return UINT32_MAX;
      }
      /* Log once per ~32k iters so a wedged caller's target seqno is
       * recoverable from the log alongside the ring's own watchdog. */
      if (iter && (iter & 0x7fff) == 0) {
         const uint32_t head =
            atomic_load_explicit(ring->head, memory_order_acquire);
         const uint32_t tail =
            atomic_load_explicit(ring->tail, memory_order_acquire);
         npt_log("wait_seqno: ring %llu target=%u head=%u tail=%u iter=%u",
                 (unsigned long long)ring->id, seqno, head, tail, iter);
      }
      npt_ring_relax(ring, &iter);
   }
   return iter;
}

/* Wait for the host to consume past the last EXECUTE on this ring
 * (i.e. done reading upload_shmem).  Caller holds ring->mutex. */
static bool
npt_ring_wait_upload_drained_locked(struct npt_ring *ring)
{
   return npt_ring_wait_seqno(ring, ring->upload_horizon_seqno) != UINT32_MAX;
}

/* Caller holds ring->mutex.  *out_fresh = a new upload_shmem was
 * just created => caller must roundtrip before the host consumes
 * its res_id (same reason as the reply-pool fresh flag). */
static bool
npt_ring_upload_reserve_locked(struct npt_ring *ring, uint32_t size,
                               uint32_t *out_offset, bool *out_fresh)
{
   uint32_t aligned;

   if (!ring || !size || !out_offset || !out_fresh ||
       size > UINT32_MAX - 7u || npt_ring_is_failed(ring))
      return false;
   aligned = (size + 7u) & ~7u;
   *out_fresh = false;

   if (!ring->upload_shmem || aligned > ring->upload_size) {
      if (ring->upload_shmem) {
         if (!npt_ring_wait_upload_drained_locked(ring))
            return false;
         npt_renderer_shmem_unref(ring->renderer, ring->upload_shmem);
         ring->upload_shmem = NULL;
      }
      uint32_t new_size = ring->upload_size ? ring->upload_size
                                            : NPT_RING_UPLOAD_INITIAL_SIZE;
      while (new_size < aligned) {
         if (new_size > UINT32_MAX / 2u)
            return npt_ring_mark_failed(ring,
                                        "upload allocation size overflow");
         new_size <<= 1;
      }
      ring->upload_shmem = npt_renderer_shmem_create(ring->renderer, new_size);
      if (!ring->upload_shmem) {
         ring->upload_size = 0;
         ring->upload_used = 0;
         return false;
      }
      ring->upload_size = new_size;
      ring->upload_used = 0;
      *out_fresh = true;
   } else if (ring->upload_used > ring->upload_size ||
              aligned > ring->upload_size - ring->upload_used) {
      /* Wrap: wait for indirect commands to retire, then reset. */
      if (!npt_ring_wait_upload_drained_locked(ring))
         return false;
      ring->upload_used = 0;
   }

   *out_offset = ring->upload_used;
   ring->upload_used += aligned;
   return true;
}

/*
 * SUBMIT goes via the sync path so it blocks until QEMU has handed
 * the command to the render server -- which, because the virtqueue
 * preserves order, happens only after any preceding
 * RESOURCE_CREATE_BLOB has reached the resource table.  WAIT goes on
 * the ring so the ring thread stops before processing the EXECUTE /
 * SET_REPLY_STREAM that references the fresh res_id.  Caller holds
 * ring->mutex.
 */
static bool
npt_ring_roundtrip_locked(struct npt_ring *ring)
{
   if (!ring || npt_ring_is_failed(ring))
      return false;
   const uint64_t seqno = ++ring->roundtrip_next;

   npt_profile_record_roundtrip(&ring->profile);

   struct npt_cmd_submit_virtqueue_seqno submit_cmd;
   memset(&submit_cmd, 0, sizeof(submit_cmd));
   submit_cmd.header.cmd_type =
      NPT_TRANSPORT_CMD_TYPE(NPT_TRANSPORT_SUBGROUP_RING,
                             NPT_TRANSPORT_RING_SUBMIT_VQ_SEQNO);
   submit_cmd.header.cmd_size = sizeof(submit_cmd);
   submit_cmd.ring_id = ring->id;
   submit_cmd.seqno = seqno;
   if (!npt_renderer_submit_cmd_sync(ring->renderer, &submit_cmd,
                                     sizeof(submit_cmd)))
      return npt_ring_mark_failed(ring, "roundtrip sync submit failed");

   struct npt_cmd_wait_virtqueue_seqno wait_cmd;
   memset(&wait_cmd, 0, sizeof(wait_cmd));
   wait_cmd.header.cmd_type =
      NPT_TRANSPORT_CMD_TYPE(NPT_TRANSPORT_SUBGROUP_RING,
                             NPT_TRANSPORT_RING_WAIT_VQ_SEQNO);
   wait_cmd.header.cmd_size = sizeof(wait_cmd);
   wait_cmd.ring_id = ring->id;
   wait_cmd.seqno = seqno;
   if (!npt_ring_submit_locked(ring, &wait_cmd, sizeof(wait_cmd)))
      return npt_ring_mark_failed(ring, "roundtrip wait submit failed");
   return true;
}

/*
 * Indirect EXECUTE_COMMAND_STREAM: copy bytes to upload_shmem and
 * place a small reference in the ring.  On fresh upload_shmem, force
 * a virtqueue-seqno roundtrip so the ring thread sees the
 * RESOURCE_CREATE_BLOB before it reads our res_id.  Caller holds
 * ring->mutex.
 */
static bool
npt_ring_submit_indirect_locked(struct npt_ring *ring, const void *data,
                                uint32_t size)
{
   uint32_t offset = 0;
   bool upload_fresh = false;
   if (!npt_ring_upload_reserve_locked(ring, size, &offset, &upload_fresh))
      return false;

   memcpy((uint8_t *)ring->upload_shmem->mmap_ptr + offset, data, size);

   if (upload_fresh && !npt_ring_roundtrip_locked(ring))
      return false;

   struct npt_cmd_execute_command_stream cmd;
   memset(&cmd, 0, sizeof(cmd));
   cmd.header.cmd_type =
      NPT_TRANSPORT_CMD_TYPE(NPT_TRANSPORT_SUBGROUP_RESOURCE,
                             NPT_TRANSPORT_RESOURCE_EXECUTE_CMD_STREAM);
   cmd.header.cmd_size = sizeof(cmd);
   cmd.res_id = ring->upload_shmem->res_id;
   cmd.offset = offset;
   cmd.size = size;

   if (!npt_ring_submit_locked(ring, &cmd, sizeof(cmd)))
      return false;

   /* Once head passes here, bytes through offset+size are reusable. */
   ring->upload_horizon_seqno = ring->cur;
   return true;
}

/* Caller holds ring->mutex. */
static bool
npt_ring_submit_dispatch_locked(struct npt_ring *ring, const void *data,
                                uint32_t size)
{
   if (size > ring->direct_size)
      return npt_ring_submit_indirect_locked(ring, data, size);
   return npt_ring_submit_locked(ring, data, size);
}

bool
npt_ring_submit_raw(struct npt_ring *ring, const void *data, uint32_t size)
{
   return npt_ring_submit_raw_seqno(ring, data, size, NULL);
}

/* Direct path twin of npt_ring_submit_locked that takes
 * [header || payload || padding]. */
static bool
npt_ring_submit_locked_split(struct npt_ring *ring,
                             const void *header, uint32_t header_size,
                             const void *payload, uint32_t payload_size,
                             uint32_t payload_padded_size)
{
   uint32_t total;
   if (!ring || !header || !header_size ||
       payload_padded_size > UINT32_MAX - header_size)
      return false;
   total = header_size + payload_padded_size;
   if (!npt_ring_wait_space(ring, total))
      return false;
   const uint32_t cur = ring->cur;
   npt_ring_write_buffer_at(ring, cur,               header,  header_size);
   if (payload_size)
      npt_ring_write_buffer_at(ring, cur + header_size, payload, payload_size);
   ring->cur = cur + total;
   npt_ring_store_tail(ring);

   const uint32_t status = npt_ring_load_status(ring);
   if (unlikely(status & NPT_RING_STATUS_FATAL_BIT)) {
      return npt_ring_mark_failed(ring, "fatal status after split submit");
   }
   if (status & NPT_RING_STATUS_IDLE_BIT) {
      if (!npt_ring_notify(ring))
         return false;
   }
   return true;
}

/* Indirect path twin of npt_ring_submit_indirect_locked that takes
 * [header || payload || padding] -- skips the caller-side concatenation
 * copy. */
static bool
npt_ring_submit_indirect_locked_split(struct npt_ring *ring,
                                      const void *header, uint32_t header_size,
                                      const void *payload, uint32_t payload_size,
                                      uint32_t payload_padded_size)
{
   uint32_t total;
   if (!ring || !header || !header_size ||
       payload_padded_size > UINT32_MAX - header_size)
      return false;
   total = header_size + payload_padded_size;
   uint32_t offset = 0;
   bool upload_fresh = false;
   if (!npt_ring_upload_reserve_locked(ring, total, &offset, &upload_fresh))
      return false;

   uint8_t *dst = (uint8_t *)ring->upload_shmem->mmap_ptr + offset;
   memcpy(dst,               header,  header_size);
   if (payload_size)
      memcpy(dst + header_size, payload, payload_size);

   if (upload_fresh && !npt_ring_roundtrip_locked(ring))
      return false;

   struct npt_cmd_execute_command_stream cmd;
   memset(&cmd, 0, sizeof(cmd));
   cmd.header.cmd_type =
      NPT_TRANSPORT_CMD_TYPE(NPT_TRANSPORT_SUBGROUP_RESOURCE,
                             NPT_TRANSPORT_RESOURCE_EXECUTE_CMD_STREAM);
   cmd.header.cmd_size = sizeof(cmd);
   cmd.res_id = ring->upload_shmem->res_id;
   cmd.offset = offset;
   cmd.size = total;

   if (!npt_ring_submit_locked(ring, &cmd, sizeof(cmd)))
      return false;

   ring->upload_horizon_seqno = ring->cur;
   return true;
}

bool
npt_ring_submit_raw_with_payload(struct npt_ring *ring,
                                 const void *header, uint32_t header_size,
                                 const void *payload, uint32_t payload_size,
                                 uint32_t payload_padded_size)
{
   if (!ring || !header || !header_size)
      return false;
   if (payload_size && !payload)
      return false;
   if (payload_size > payload_padded_size)
      return false;

   if (payload_padded_size > UINT32_MAX - header_size)
      return false;
   const uint32_t total = header_size + payload_padded_size;
   mtx_lock(&ring->mutex);
   bool ok;
   if (total > ring->direct_size) {
      ok = npt_ring_submit_indirect_locked_split(ring, header, header_size,
                                                 payload, payload_size,
                                                 payload_padded_size);
   } else {
      ok = npt_ring_submit_locked_split(ring, header, header_size,
                                        payload, payload_size,
                                        payload_padded_size);
   }
   mtx_unlock(&ring->mutex);
   return ok;
}

bool
npt_ring_force_roundtrip(struct npt_ring *ring)
{
   if (!ring)
      return false;
   mtx_lock(&ring->mutex);
   const bool ok = npt_ring_roundtrip_locked(ring);
   mtx_unlock(&ring->mutex);
   return ok;
}

bool
npt_ring_submit_raw_seqno(struct npt_ring *ring, const void *data,
                          uint32_t size, uint32_t *out_seqno)
{
   if (out_seqno)
      *out_seqno = 0;
   if (!ring || !data || !size || npt_ring_is_failed(ring))
      return false;

   mtx_lock(&ring->mutex);
   const bool ok = npt_ring_submit_dispatch_locked(ring, data, size);
   if (ok && out_seqno)
      *out_seqno = ring->cur;
   mtx_unlock(&ring->mutex);
   return ok;
}

struct npt_cs_encoder *
npt_ring_submit_command_init(struct npt_ring *ring,
                             struct npt_ring_submit_command *submit,
                             void *cmd_data, size_t cmd_size,
                             size_t reply_size)
{
   if (!submit)
      return NULL;
   memset(submit, 0, sizeof(*submit));
   if (!ring || npt_ring_is_failed(ring))
      return NULL;
   if (!cmd_data || !cmd_size || cmd_size > UINT32_MAX || reply_size > UINT32_MAX) {
      npt_ring_mark_failed(ring, "command allocation or sizing failed");
      return NULL;
   }
   submit->cmd_data = cmd_data;
   submit->cmd_size = cmd_size;
   submit->reply_size = reply_size;
   submit->enc.cur = (uint8_t *)cmd_data;
   submit->enc.end = (uint8_t *)cmd_data + cmd_size;
   return &submit->enc;
}

void
npt_ring_submit_command(struct npt_ring *ring,
                        struct npt_ring_submit_command *submit)
{
   if (!ring || !submit || !submit->cmd_size || !submit->cmd_data ||
       submit->cmd_size > UINT32_MAX || submit->reply_size > UINT32_MAX ||
       npt_ring_is_failed(ring))
      return;

   if (submit->enc.fatal || submit->enc.cur != submit->enc.end) {
      npt_ring_mark_failed(ring, "incomplete command encoding");
      return;
   }

   /* prof_cmd_type stashes cmd_type so get_command_reply can attribute
    * reply_ns -- sync thunks free cmd_data before the reply arrives. */
   struct npt_profile_submit_state prof;
   npt_profile_submit_begin(submit, ring->direct_size, &prof);

   /* Reply window must be carved BEFORE ring->mutex: the pool's growth
    * path takes its own lock and may call shmem_create. */
   bool shmem_fresh = false;
   if (submit->reply_size) {
      size_t off = 0;
      submit->reply_shmem = npt_shmem_pool_alloc(
         ring->renderer, &ring->reply_pool, submit->reply_size, &off,
         &shmem_fresh);
      if (!submit->reply_shmem) {
         npt_log("submit_command: reply pool alloc failed (%zu bytes)",
                 submit->reply_size);
         return;
      }
      if (off > UINT32_MAX || submit->reply_size > submit->reply_shmem->size ||
          off > submit->reply_shmem->size - submit->reply_size) {
         npt_log("submit_command: invalid reply window off=%zu size=%zu backing=%zu",
                 off, submit->reply_size, submit->reply_shmem->size);
         npt_renderer_shmem_unref(ring->renderer, submit->reply_shmem);
         submit->reply_shmem = NULL;
         return;
      }
      submit->reply_offset = (uint32_t)off;
   }

   mtx_lock(&ring->mutex);

   /* Fresh shmem: roundtrip so the virtqueue dispatcher's
    * RESOURCE_CREATE_BLOB lands before the ring thread reads
    * SET_REPLY_STREAM and finds no resource for our res_id. */
   if (shmem_fresh && !npt_ring_roundtrip_locked(ring)) {
      mtx_unlock(&ring->mutex);
      /* Resource creation can have reached the host even when its sync
       * acknowledgement failed.  Preserve this reference instead of freeing
       * memory the host can still access. */
      return;
   }

   if (submit->reply_size) {
      struct npt_cmd_set_reply_stream set_cmd;
      memset(&set_cmd, 0, sizeof(set_cmd));
      set_cmd.header.cmd_type =
         NPT_TRANSPORT_CMD_TYPE(NPT_TRANSPORT_SUBGROUP_CORE,
                                NPT_TRANSPORT_CORE_SET_REPLY_STREAM);
      set_cmd.header.cmd_size = sizeof(set_cmd);
      set_cmd.res_id = submit->reply_shmem->res_id;
      set_cmd.offset = submit->reply_offset;
      set_cmd.size = (uint32_t)submit->reply_size;
      if (!npt_ring_submit_locked(ring, &set_cmd, sizeof(set_cmd))) {
         mtx_unlock(&ring->mutex);
         return;
      }
   }

   /* SET_REPLY + (EXECUTE_)CMD stay back-to-back under ring->mutex,
    * so the host writes its reply into our window before any peer
    * SET_REPLY can re-target the stream. */
   if (!npt_ring_submit_dispatch_locked(ring, submit->cmd_data,
                                        (uint32_t)submit->cmd_size)) {
      npt_ring_mark_failed(ring, "command submission failed");
      mtx_unlock(&ring->mutex);
      /* SET_REPLY_STREAM can already be visible to the host.  Keep its shmem
       * alive on an uncertain submit failure; get_command_reply returns NULL
       * because the ring is poisoned. */
      return;
   }

   submit->seqno = ring->cur;

   mtx_unlock(&ring->mutex);

   npt_profile_submit_finalize(&ring->profile, &prof);
}

struct npt_cs_decoder *
npt_ring_get_command_reply(struct npt_ring *ring,
                           struct npt_ring_submit_command *submit)
{
   if (!ring || !submit || !submit->reply_size || npt_ring_is_failed(ring))
      return NULL;

   if (!submit->reply_shmem)
      return NULL;

   const bool prof_enabled = npt_profile_enabled();
   const uint64_t prof_t0 = prof_enabled ? npt_profile_now_ns() : 0;
   const bool prof_was_ready =
      prof_enabled && npt_ring_seqno_status(ring, submit->seqno);
   const uint32_t wait_iters = npt_ring_wait_seqno(ring, submit->seqno);
   if (wait_iters == UINT32_MAX || npt_ring_is_failed(ring))
      return NULL;
   if (prof_enabled) {
      const uint64_t prof_wait_ns = npt_profile_now_ns() - prof_t0;
      ring->profile.total_reply_ns += prof_wait_ns;
      if (!prof_was_ready)
         npt_profile_record_reply_wait(&ring->profile, wait_iters);
      /* prof_cmd_type was captured at submit; sync thunks free
       * cmd_data before this runs (would be UAF). */
      npt_profile_record_reply_ns(submit->prof_cmd_type, prof_wait_ns);
      npt_profile_record_thread_reply_ns(prof_wait_ns);
   }
   if (submit->reply_offset > submit->reply_shmem->size ||
       submit->reply_size >
          submit->reply_shmem->size - submit->reply_offset) {
      npt_ring_mark_failed(ring, "reply window exceeds backing allocation");
      return NULL;
   }

   const uint8_t *reply_base = (const uint8_t *)submit->reply_shmem->mmap_ptr
                               + submit->reply_offset;
   submit->dec.cur = reply_base;
   submit->dec.end = reply_base + submit->reply_size;
   return &submit->dec;
}

void
npt_ring_free_command_reply(struct npt_ring *ring,
                            struct npt_ring_submit_command *submit)
{
   if (!ring || !submit)
      return;
   npt_cs_decoder_fini_temp_pool(&submit->dec);
   submit->dec.cur = NULL;
   submit->dec.end = NULL;

   if (submit->reply_shmem) {
      npt_renderer_shmem_unref(ring->renderer, submit->reply_shmem);
      submit->reply_shmem = NULL;
   }
}

struct npt_ring *
npt_ring_create(struct npt_device *device,
                const struct npt_ring_layout *layout,
                uint64_t ring_id,
                bool is_tls_ring)
{
   if (!device || !device->renderer || !layout || !layout->buffer_size ||
       layout->buffer_size > UINT32_MAX ||
       !util_is_power_of_two_nonzero(layout->buffer_size) ||
       layout->buffer_size < (1u << NPT_RING_DIRECT_ORDER) ||
       layout->head_offset != 0 || layout->tail_offset != 64 ||
       layout->status_offset != 128 || layout->buffer_offset != 192 ||
       layout->buffer_size > SIZE_MAX - layout->buffer_offset ||
       layout->extra_offset != layout->buffer_offset + layout->buffer_size ||
       layout->shmem_size < NPT_RING_WORKAROUND_OFFSET + sizeof(uint32_t) ||
       layout->buffer_size > layout->shmem_size ||
       layout->buffer_offset > layout->shmem_size - layout->buffer_size ||
       layout->extra_offset > layout->shmem_size ||
       layout->extra_size > layout->shmem_size - layout->extra_offset)
      return NULL;
   /* A secondary ring must not become an apparent recovery from a failed
    * primary ring.  Its callers can fall back to primary; that poisoned ring
    * will then reject the next submission honestly. */
   if (device->ring && npt_ring_is_failed(device->ring))
      return NULL;

   struct npt_ring *ring = npt_alloc(sizeof(*ring));
   if (!ring)
      return NULL;

   struct npt_renderer *renderer = device->renderer;
   ring->id = ring_id;
   ring->renderer = renderer;
   ring->device = device;
   ring->is_tls_ring = is_tls_ring;
   list_inithead(&ring->instance_head);

   ring->shmem = npt_renderer_shmem_create(renderer, layout->shmem_size);
   if (!ring->shmem || !ring->shmem->mmap_ptr || !ring->shmem->res_id ||
       ring->shmem->size < layout->shmem_size) {
      if (ring->shmem)
         npt_renderer_shmem_unref(renderer, ring->shmem);
      free(ring);
      return NULL;
   }

   uint8_t *base = ring->shmem->mmap_ptr;
   ring->head = (volatile atomic_uint *)(base + layout->head_offset);
   ring->tail = (volatile atomic_uint *)(base + layout->tail_offset);
   ring->status = (volatile atomic_uint *)(base + layout->status_offset);
   /* Host writes the backend workaround-flags word here once at ring create. */
   ring->wa_word = (const volatile atomic_uint *)(base + NPT_RING_WORKAROUND_OFFSET);
   ring->buffer = base + layout->buffer_offset;
   ring->extra = base + layout->extra_offset;

   ring->buffer_size = layout->buffer_size;
   ring->buffer_mask = layout->buffer_size - 1;
   ring->cur = 0;
   atomic_init(&ring->failed, false);
   ring->direct_size = layout->buffer_size >> NPT_RING_DIRECT_ORDER;
   assert(ring->direct_size);

   atomic_store_explicit(ring->head, 0, memory_order_relaxed);
   atomic_store_explicit(ring->tail, 0, memory_order_relaxed);
   atomic_store_explicit(ring->status, 0, memory_order_relaxed);

   /* Detect once whether atomic RMWs work on the WC ring shmem here; gates the
    * watchdog's ALIVE clear -- see npt_detect_wc_atomic_ok. */
   if (atomic_load_explicit(&npt_wc_atomic_ok, memory_order_acquire) < 0) {
      const int ok = npt_detect_wc_atomic_ok();
      atomic_store_explicit(&npt_wc_atomic_ok, ok, memory_order_release);
      npt_log("ring WC-atomic support -> %s",
              ok ? "yes (native x86)"
                 : "no -- watchdog ALIVE-clear disabled");
   }

   if (mtx_init(&ring->mutex, mtx_plain) != thrd_success) {
      npt_renderer_shmem_unref(renderer, ring->shmem);
      free(ring);
      return NULL;
   }

   npt_shmem_pool_init(&ring->reply_pool, NPT_RING_REPLY_POOL_MIN_SIZE);

   ring->upload_shmem = NULL;
   ring->upload_size = 0;
   ring->upload_used = 0;
   ring->upload_horizon_seqno = 0;

   npt_profile_register_ring(ring);

   struct npt_cmd_create_ring create_cmd;
   memset(&create_cmd, 0, sizeof(create_cmd));
   create_cmd.header.cmd_type = NPT_TRANSPORT_CMD_TYPE(NPT_TRANSPORT_SUBGROUP_RING,
                                                       NPT_TRANSPORT_RING_CREATE);
   create_cmd.header.cmd_size = sizeof(create_cmd);
   create_cmd.ring_id = ring_id;
   create_cmd.res_id = ring->shmem->res_id;
   create_cmd.head_offset = layout->head_offset;
   create_cmd.tail_offset = layout->tail_offset;
   create_cmd.status_offset = layout->status_offset;
   create_cmd.workaround_offset = NPT_RING_WORKAROUND_OFFSET;
   create_cmd.buffer_offset = layout->buffer_offset;
   create_cmd.buffer_size = layout->buffer_size;
   create_cmd.extra_offset = layout->extra_offset;
   create_cmd.extra_size = layout->extra_size;
   create_cmd.idle_timeout = is_tls_ring
                                ? npt_env.tls_idle_timeout_ns
                                : NPT_RING_IDLE_TIMEOUT_NS;
   /* Monitor every ring: without this, the cross-ring drain would
    * spin on a TLS ring whose ALIVE bit never gets set. */
   create_cmd.monitor_report_period_us = NPT_RING_WATCHDOG_REPORT_PERIOD_US;

   /* Forward priority so the host doesn't starve under elevated
    * guest foreground load.  Win32 needs a different API. */
#if !defined(_WIN32)
   errno = 0;
   const int prio = getpriority(PRIO_PROCESS, 0);
   if (!(prio == -1 && errno)) {
      create_cmd.priority_valid = 1;
      create_cmd.priority = prio;
   }
#endif

   /* TLS rings use SYNC so the ring thread is polling before the
    * caller can submit; primary stays async (device init masks the
    * latency). */
   const bool create_ok = is_tls_ring
      ? npt_renderer_submit_cmd_sync(renderer, &create_cmd, sizeof(create_cmd))
      : npt_renderer_submit_cmd(renderer, &create_cmd, sizeof(create_cmd));
   if (!create_ok) {
      /* Submission failure can be an acknowledgement failure after the host
       * observed CREATE_RING.  Leak the host-visible mapping intentionally;
       * freeing it here would create a use-after-free. */
      npt_ring_mark_failed(ring, "ring creation submit failed");
      npt_ring_abandon_local_record(ring);
      return NULL;
   }

   return ring;
}

void
npt_ring_destroy(struct npt_ring *ring)
{
   if (!ring)
      return;

   /*
    * DESTROY_RING is synchronous via the renderer (not the ring) so
    * a ring stop-command isn't asked to wait on its own thread.  The
    * sync ensures the host has stopped reading the shmem before we
    * unref it.
    */
   if (ring->shmem) {
      struct npt_cmd_destroy_ring cmd;
      memset(&cmd, 0, sizeof(cmd));
      cmd.header.cmd_type = NPT_TRANSPORT_CMD_TYPE(NPT_TRANSPORT_SUBGROUP_RING,
                                                   NPT_TRANSPORT_RING_DESTROY);
      cmd.header.cmd_size = sizeof(cmd);
      cmd.ring_id = ring->id;
      if (!npt_renderer_submit_cmd_sync(ring->renderer, &cmd, sizeof(cmd))) {
         npt_ring_mark_failed(ring, "ring destruction submit failed");
         /* The host can still read every mapping owned by this ring.  Keep
          * those backing objects, but discard the guest-only record. */
         npt_ring_abandon_local_record(ring);
         return;
      }

      npt_renderer_shmem_unref(ring->renderer, ring->shmem);
   }
   if (ring->upload_shmem)
      npt_renderer_shmem_unref(ring->renderer, ring->upload_shmem);
   npt_shmem_pool_fini(ring->renderer, &ring->reply_pool);
   mtx_destroy(&ring->mutex);
   npt_profile_unregister_ring(ring);
   free(ring);
}
