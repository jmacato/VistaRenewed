/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Guest-side perf/debug logging.  All NPT-* log lines emitted by the
 * guest live in npt_profile.c; per-flag semantics are documented at
 * the enum in npt_env.h.
 */

#ifndef NPT_PROFILE_H
#define NPT_PROFILE_H

#include <stdbool.h>
#include <stdint.h>

#include "npt_common.h"
#include "npt_cs.h"

#define NPT_PROFILE_NUM_SLOTS 1024u

/* Producer holds ring->mutex on submit. */
struct npt_profile_ring {
   uint64_t submits;
   uint64_t submits_async;
   uint64_t submits_sync;
   uint64_t bytes_submitted;
   uint64_t notifies;
   uint64_t indirect_submits;
   uint64_t full_waits;
   uint64_t space_wait_iters;
   uint64_t reply_waits;
   uint64_t reply_wait_iters;
   uint64_t total_submit_ns;
   uint64_t total_reply_ns;
   uint64_t roundtrips;
};

struct npt_profile_slot {
   /* 0 = empty. */
   uint32_t cmd_type;
   uint32_t count;
   uint64_t submit_ns;
   uint64_t reply_ns;
   uint64_t bytes;
};

struct npt_profile {
   bool enabled;
   uint64_t period_ns;

   mtx_t rings_mutex;
   struct list_head rings;

   struct npt_profile_slot slots[NPT_PROFILE_NUM_SLOTS];

   /* Written under rings_mutex. */
   uint64_t last_dump_ns;

   /* Stutter detection (Present-to-Present interval > threshold). */
   uint64_t stutter_threshold_ns;
   uint64_t last_present_ns;
   struct npt_profile_ring last_present_agg;
   struct npt_profile_slot last_present_slots[NPT_PROFILE_NUM_SLOTS];
};

extern struct npt_profile npt_profile;

struct npt_ring;

void npt_profile_init(void);
uint64_t npt_profile_now_ns(void);
void npt_profile_dump(const char *reason);

/* Call once per IDXGISwapChain::Present.  If the gap since the previous
 * call exceeds the stutter threshold, logs a one-line STUTTER event
 * with the per-frame delta of agg counters and the top cmd_types that
 * fired during the slow frame. */
void npt_profile_present_marker(void);

/* Emit one NPT-PRESENT-TIMING line per Present.  Caller stamps the
 * four checkpoints inline (gated on NPT_DEBUG(PRESENT_TIMING)) so the
 * clock_gettime cost stays out of the hot path when off, and hands
 * them off here for formatting.  t0..t3 are CLOCK_MONOTONIC ns:
 *   t0 = entry, t1 = after backpressure, t2 = after host Present,
 *   t3 = after WSI flip. */
void npt_profile_log_present_timing(unsigned sync_interval,
                                    uint64_t t0, uint64_t t1,
                                    uint64_t t2, uint64_t t3);

/* Emit one NPT-PO-A line per Present.  Trace the (flip_idx,
 * image_index, wait_fence_fd) tuple so out-of-order frame delivery
 * to X can be diagnosed against the host fence_trace. */
void npt_profile_log_present_order(unsigned flip_idx,
                                   unsigned image_index_pre,
                                   unsigned image_count,
                                   int wait_fence_fd);

/* Per-thread sync-wait accounting.  TLS-only; readable from anywhere
 * that wants to know *this thread's* cumulative time blocked on sync
 * replies.  Used by present_marker to distinguish "Present thread
 * actually stalled on sync calls" from "background thread did the
 * waits, Present thread was off doing CPU work". */
extern NPT_TLS uint64_t npt_tl_reply_ns;
extern NPT_TLS uint64_t npt_tl_sync_count;
extern NPT_TLS uint64_t npt_tl_submit_ns;
extern NPT_TLS uint64_t npt_tl_submit_count;

static inline void
npt_profile_record_thread_reply_ns(uint64_t reply_ns)
{
   if (!npt_profile.enabled)
      return;
   npt_tl_reply_ns += reply_ns;
   npt_tl_sync_count++;
}

static inline void
npt_profile_record_thread_submit_ns(uint64_t submit_ns)
{
   if (!npt_profile.enabled)
      return;
   npt_tl_submit_ns += submit_ns;
   npt_tl_submit_count++;
}

/* Always called from ring create/destroy so a runtime toggle works. */
void npt_profile_register_ring(struct npt_ring *ring);
void npt_profile_unregister_ring(struct npt_ring *ring);

static inline bool
npt_profile_enabled(void)
{
   return npt_profile.enabled;
}

/* MurmurHash3 finalizer; shared so record_submit and record_reply_ns
 * walk the slot array in lockstep. */
static inline uint32_t
npt_profile_slot_hash(uint32_t cmd_type)
{
   uint32_t h = cmd_type;
   h = (h ^ (h >> 16)) * 0x85ebca6bu;
   h = (h ^ (h >> 13)) * 0xc2b2ae35u;
   return (h ^ (h >> 16)) & (NPT_PROFILE_NUM_SLOTS - 1u);
}

static inline void
npt_profile_record_submit(struct npt_profile_ring *p,
                          uint32_t cmd_type, uint32_t bytes,
                          uint64_t submit_ns, uint64_t reply_ns,
                          bool is_sync, bool is_indirect)
{
   if (!npt_profile.enabled)
      return;

   p->submits++;
   if (is_sync) {
      p->submits_sync++;
      p->total_reply_ns += reply_ns;
   } else {
      p->submits_async++;
   }
   p->bytes_submitted += bytes;
   if (is_indirect)
      p->indirect_submits++;
   p->total_submit_ns += submit_ns;

   const uint32_t h = npt_profile_slot_hash(cmd_type);
   for (uint32_t i = 0; i < NPT_PROFILE_NUM_SLOTS; i++) {
      uint32_t idx = (h + i) & (NPT_PROFILE_NUM_SLOTS - 1u);
      struct npt_profile_slot *s = &npt_profile.slots[idx];
      if (s->cmd_type == 0 || s->cmd_type == cmd_type) {
         s->cmd_type = cmd_type;
         s->count++;
         s->submit_ns += submit_ns;
         s->reply_ns += reply_ns;
         s->bytes += bytes;
         return;
      }
   }
}

/* Slot must already exist (created at submit); this only accumulates. */
static inline void
npt_profile_record_reply_ns(uint32_t cmd_type, uint64_t reply_ns)
{
   if (!npt_profile.enabled || !cmd_type)
      return;
   const uint32_t h = npt_profile_slot_hash(cmd_type);
   for (uint32_t i = 0; i < NPT_PROFILE_NUM_SLOTS; i++) {
      uint32_t idx = (h + i) & (NPT_PROFILE_NUM_SLOTS - 1u);
      struct npt_profile_slot *s = &npt_profile.slots[idx];
      if (s->cmd_type == cmd_type) {
         s->reply_ns += reply_ns;
         return;
      }
      if (s->cmd_type == 0)
         return;
   }
}

static inline void
npt_profile_record_notify(struct npt_profile_ring *p)
{
   if (npt_profile.enabled)
      p->notifies++;
}

static inline void
npt_profile_record_full_wait(struct npt_profile_ring *p, uint32_t iters)
{
   if (!npt_profile.enabled)
      return;
   p->full_waits++;
   p->space_wait_iters += iters;
}

static inline void
npt_profile_record_reply_wait(struct npt_profile_ring *p, uint32_t iters)
{
   if (!npt_profile.enabled)
      return;
   p->reply_waits++;
   p->reply_wait_iters += iters;
}

static inline void
npt_profile_record_roundtrip(struct npt_profile_ring *p)
{
   if (npt_profile.enabled)
      p->roundtrips++;
}

static inline void
npt_profile_maybe_dump(struct npt_profile_ring *p)
{
   if (!npt_profile.enabled)
      return;
   /* Clock once every ~4K submits.  At 50K submits/s that's ~12
    * checks/s, well above the 1Hz dump rate, so we never miss a
    * window by more than ~80ms. */
   if (likely(p->submits & 0xFFFu))
      return;
   uint64_t now = npt_profile_now_ns();
   if (now - npt_profile.last_dump_ns >= npt_profile.period_ns) {
      npt_profile.last_dump_ns = now;
      npt_profile_dump("periodic");
   }
}

struct npt_profile_submit_state {
   /* 0 = profiling off. */
   uint64_t t0;
   uint32_t cmd_type;
   uint32_t cmd_size;
   bool is_sync;
   bool is_indirect;
};

static inline void
npt_profile_submit_begin(struct npt_ring_submit_command *submit,
                         uint32_t direct_size,
                         struct npt_profile_submit_state *ps)
{
   if (!npt_profile.enabled) {
      ps->t0 = 0;
      return;
   }

   ps->t0 = npt_profile_now_ns();
   /* cmd_type is the first u32 of every command. */
   ps->cmd_type = submit->cmd_size >= sizeof(uint32_t)
                     ? *(const uint32_t *)submit->cmd_data
                     : 0u;
   ps->cmd_size = (uint32_t)submit->cmd_size;
   ps->is_sync = submit->reply_size != 0;
   ps->is_indirect = submit->cmd_size > direct_size;
   submit->prof_cmd_type = ps->cmd_type;
}

static inline void
npt_profile_submit_finalize(struct npt_profile_ring *p,
                            const struct npt_profile_submit_state *ps)
{
   if (!ps->t0)
      return;
   const uint64_t submit_ns = npt_profile_now_ns() - ps->t0;
   /* reply_ns recorded separately by record_reply_ns in
    * get_command_reply so sync and async share the submit column. */
   npt_profile_record_submit(p, ps->cmd_type, ps->cmd_size,
                             submit_ns, 0, ps->is_sync, ps->is_indirect);
   npt_profile_record_thread_submit_ns(submit_ns);
   npt_profile_maybe_dump(p);
}

#endif /* NPT_PROFILE_H */
