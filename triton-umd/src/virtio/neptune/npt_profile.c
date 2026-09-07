/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 */

#include "npt_profile.h"
#include "npt_device.h"
#include "npt_env.h"
#include "npt_ring.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
static LARGE_INTEGER g_qpc_freq;

static uint64_t
npt_profile_now_ns_win(void)
{
   /* g_qpc_freq is populated unconditionally by npt_profile_init so any
    * gated-or-not caller (e.g. PRESENT_TIMING ternary) is DBZ-safe. */
   if (unlikely(g_qpc_freq.QuadPart <= 0))
      return 0;
   LARGE_INTEGER c;
   QueryPerformanceCounter(&c);
   /* Split sec/rem to dodge 64-bit overflow on counter * 1e9. */
   const uint64_t sec = (uint64_t)c.QuadPart / (uint64_t)g_qpc_freq.QuadPart;
   const uint64_t rem = (uint64_t)c.QuadPart % (uint64_t)g_qpc_freq.QuadPart;
   return sec * 1000000000ull +
          (rem * 1000000000ull) / (uint64_t)g_qpc_freq.QuadPart;
}
#else
#include <time.h>
static uint64_t
npt_profile_now_ns_posix(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
#endif

struct npt_profile npt_profile;

NPT_TLS uint64_t npt_tl_reply_ns;
NPT_TLS uint64_t npt_tl_sync_count;
NPT_TLS uint64_t npt_tl_submit_ns;
NPT_TLS uint64_t npt_tl_submit_count;
/* Snapshot of the above at the previous Present marker, so we can
 * report the per-frame delta on the Present thread. */
static NPT_TLS uint64_t npt_tl_reply_ns_prev;
static NPT_TLS uint64_t npt_tl_sync_count_prev;
static NPT_TLS uint64_t npt_tl_submit_ns_prev;
static NPT_TLS uint64_t npt_tl_submit_count_prev;

uint64_t
npt_profile_now_ns(void)
{
#ifdef _WIN32
   return npt_profile_now_ns_win();
#else
   return npt_profile_now_ns_posix();
#endif
}

void
npt_profile_init(void)
{
   mtx_init(&npt_profile.rings_mutex, mtx_plain);
   list_inithead(&npt_profile.rings);

#ifdef _WIN32
   /* Populate the QPC frequency unconditionally so npt_profile_now_ns is
    * safe to call from any non-PROFILE-gated path (e.g. PRESENT_TIMING
    * uses npt_profile_now_ns directly). */
   QueryPerformanceFrequency(&g_qpc_freq);
#endif

   /* npt_com_init runs npt_env_init before us. */
   if (!NPT_DEBUG(PROFILE)) {
      npt_profile.enabled = false;
      return;
   }

#ifdef _WIN32
   if (g_qpc_freq.QuadPart <= 0) {
      npt_profile.enabled = false;
      return;
   }
#endif

   npt_profile.enabled = true;

   npt_profile.period_ns =
      (uint64_t)npt_env.profile_period_ms * 1000000ull;
   npt_profile.last_dump_ns = npt_profile_now_ns();

   /* Stutter threshold: Present-to-Present interval >= this triggers
    * a STUTTER log with per-frame delta of agg counters and top-N
    * cmd_types. */
   npt_profile.stutter_threshold_ns =
      (uint64_t)npt_env.stutter_threshold_ms * 1000000ull;

   npt_log("profile: enabled, period=%ums stutter_threshold=%ums",
           (unsigned)npt_env.profile_period_ms,
           (unsigned)npt_env.stutter_threshold_ms);
}

void
npt_profile_register_ring(struct npt_ring *ring)
{
   list_inithead(&ring->profile_head);
   memset(&ring->profile, 0, sizeof(ring->profile));
   mtx_lock(&npt_profile.rings_mutex);
   list_addtail(&ring->profile_head, &npt_profile.rings);
   mtx_unlock(&npt_profile.rings_mutex);
}

void
npt_profile_unregister_ring(struct npt_ring *ring)
{
   mtx_lock(&npt_profile.rings_mutex);
   if (!list_is_empty(&ring->profile_head))
      list_del(&ring->profile_head);
   mtx_unlock(&npt_profile.rings_mutex);
}

/* Group 0 = transport, 1-3 = toplevel, 255 = COM dispatch. */
static void
npt_profile_format_cmd(char *buf, size_t buf_size, uint32_t cmd_type)
{
   uint32_t group = (cmd_type >> 24) & 0xFFu;
   if (group == 255u) {
      /* npt_protocol_defs.h layout: bits 23:8 = interface_id, 7:0 = method. */
      uint32_t iface_id = (cmd_type >> 8) & 0xFFFFu;
      uint32_t method = cmd_type & 0xFFu;
      snprintf(buf, buf_size, "com.iface%u.m%u", iface_id, method);
   } else if (group >= 1u && group <= 3u) {
      uint32_t func_id = cmd_type & 0x00FFFFFFu;
      snprintf(buf, buf_size, "top.%u.%u", group, func_id);
   } else if (group == 0u) {
      uint32_t t = cmd_type & 0x00FFFFFFu;
      snprintf(buf, buf_size, "xport.%u", t);
   } else {
      snprintf(buf, buf_size, "unk.0x%08x", cmd_type);
   }
}

static int
npt_profile_cmp_desc(const void *a, const void *b)
{
   const struct npt_profile_slot *const *pa = a;
   const struct npt_profile_slot *const *pb = b;
   if ((*pb)->count != (*pa)->count)
      return (*pb)->count > (*pa)->count ? 1 : -1;
   return 0;
}

/* For the kind= field on NPT-PROF-GUEST-RING. */
static const char *
npt_profile_ring_kind(const struct npt_ring *ring)
{
   if (ring->is_tls_ring)
      return "tls";
   if (ring->device && ring == ring->device->ring)
      return "primary";
   return "instance";
}

/* Sum each ring's profile counters into one aggregate. */
static void
npt_profile_agg_snapshot(struct npt_profile_ring *agg)
{
   memset(agg, 0, sizeof(*agg));
   mtx_lock(&npt_profile.rings_mutex);
   list_for_each_entry(struct npt_ring, r, &npt_profile.rings, profile_head) {
      const struct npt_profile_ring *p = &r->profile;
      agg->submits             += p->submits;
      agg->submits_async       += p->submits_async;
      agg->submits_sync        += p->submits_sync;
      agg->bytes_submitted     += p->bytes_submitted;
      agg->notifies            += p->notifies;
      agg->indirect_submits    += p->indirect_submits;
      agg->full_waits          += p->full_waits;
      agg->space_wait_iters    += p->space_wait_iters;
      agg->reply_waits         += p->reply_waits;
      agg->reply_wait_iters    += p->reply_wait_iters;
      agg->total_submit_ns     += p->total_submit_ns;
      agg->total_reply_ns      += p->total_reply_ns;
      agg->roundtrips          += p->roundtrips;
   }
   mtx_unlock(&npt_profile.rings_mutex);
}

/* Per-cmd_type delta computed against the snapshot taken at the
 * previous Present.  Slots are addressed by hash so the same array
 * index identifies the same cmd_type across snapshots. */
struct cmd_delta {
   uint32_t cmd_type;
   uint32_t count;
   uint64_t submit_ns;
   uint64_t reply_ns;
   uint64_t bytes;
};

static int
cmd_delta_cmp_count_desc(const void *a, const void *b)
{
   const struct cmd_delta *pa = a, *pb = b;
   if (pb->count != pa->count)
      return pb->count > pa->count ? 1 : -1;
   return 0;
}

static int
cmd_delta_cmp_replyns_desc(const void *a, const void *b)
{
   const struct cmd_delta *pa = a, *pb = b;
   if (pb->reply_ns != pa->reply_ns)
      return pb->reply_ns > pa->reply_ns ? 1 : -1;
   return 0;
}

void
npt_profile_present_marker(void)
{
   if (!npt_profile.enabled)
      return;

   const uint64_t now = npt_profile_now_ns();

   /* First call: prime the snapshots and bail. */
   if (!npt_profile.last_present_ns) {
      npt_profile_agg_snapshot(&npt_profile.last_present_agg);
      memcpy(npt_profile.last_present_slots, npt_profile.slots,
             sizeof(npt_profile.last_present_slots));
      npt_profile.last_present_ns = now;
      npt_tl_reply_ns_prev    = npt_tl_reply_ns;
      npt_tl_sync_count_prev  = npt_tl_sync_count;
      npt_tl_submit_ns_prev   = npt_tl_submit_ns;
      npt_tl_submit_count_prev = npt_tl_submit_count;
      return;
   }

   const uint64_t dt_ns = now - npt_profile.last_present_ns;

   /* Always refresh snapshots so the next stutter sees fresh deltas,
    * but only emit a log when the gap is over threshold. */
   if (dt_ns >= npt_profile.stutter_threshold_ns) {
      struct npt_profile_ring agg;
      npt_profile_agg_snapshot(&agg);
      const struct npt_profile_ring *prev = &npt_profile.last_present_agg;

      const uint64_t d_submits   = agg.submits          - prev->submits;
      const uint64_t d_sync      = agg.submits_sync     - prev->submits_sync;
      const uint64_t d_bytes     = agg.bytes_submitted  - prev->bytes_submitted;
      const uint64_t d_subns     = agg.total_submit_ns  - prev->total_submit_ns;
      const uint64_t d_repns     = agg.total_reply_ns   - prev->total_reply_ns;
      const uint64_t d_fullwaits = agg.full_waits       - prev->full_waits;
      const uint64_t d_replywaits= agg.reply_waits      - prev->reply_waits;

      const uint64_t tl_reply_ns_d    = npt_tl_reply_ns    - npt_tl_reply_ns_prev;
      const uint64_t tl_sync_count_d  = npt_tl_sync_count  - npt_tl_sync_count_prev;
      const uint64_t tl_submit_ns_d   = npt_tl_submit_ns   - npt_tl_submit_ns_prev;
      const uint64_t tl_submit_cnt_d  = npt_tl_submit_count - npt_tl_submit_count_prev;
      const double dt_ms      = (double)dt_ns / 1e6;
      const double tl_reply_ms  = (double)tl_reply_ns_d   / 1e6;
      const double tl_submit_ms = (double)tl_submit_ns_d  / 1e6;
      const double tl_total_ms  = tl_reply_ms + tl_submit_ms;
      const double tl_pct = dt_ms > 0.0 ? (tl_total_ms * 100.0 / dt_ms) : 0.0;

      npt_log("STUTTER dt_ms=%llu submits=+%llu sync=+%llu "
              "bytes=+%llu submit_ms=%.1f reply_ms=%.1f "
              "full_waits=+%llu reply_waits=+%llu "
              "present_thread: submit_ms=%.1f reply_ms=%.1f "
              "(driver=%.0f%% of dt) submits=+%llu sync=+%llu",
              (unsigned long long)(dt_ns / 1000000ull),
              (unsigned long long)d_submits,
              (unsigned long long)d_sync,
              (unsigned long long)d_bytes,
              (double)d_subns / 1e6, (double)d_repns / 1e6,
              (unsigned long long)d_fullwaits,
              (unsigned long long)d_replywaits,
              tl_submit_ms, tl_reply_ms, tl_pct,
              (unsigned long long)tl_submit_cnt_d,
              (unsigned long long)tl_sync_count_d);

      /* Top-5 cmd_types by per-frame count and by per-frame reply_ns
       * — the count list catches "this frame issued 8000 SetCB" bursts,
       * the reply_ns list catches "this frame did one slow Map sync". */
      struct cmd_delta deltas[NPT_PROFILE_NUM_SLOTS];
      uint32_t n = 0;
      for (uint32_t i = 0; i < NPT_PROFILE_NUM_SLOTS; i++) {
         const struct npt_profile_slot *cur = &npt_profile.slots[i];
         const struct npt_profile_slot *prv = &npt_profile.last_present_slots[i];
         if (!cur->cmd_type)
            continue;
         struct cmd_delta d = {
            .cmd_type  = cur->cmd_type,
            .count     = cur->count     - prv->count,
            .submit_ns = cur->submit_ns - prv->submit_ns,
            .reply_ns  = cur->reply_ns  - prv->reply_ns,
            .bytes     = cur->bytes     - prv->bytes,
         };
         if (d.count == 0 && d.submit_ns == 0 && d.reply_ns == 0)
            continue;
         deltas[n++] = d;
      }

      qsort(deltas, n, sizeof(deltas[0]), cmd_delta_cmp_count_desc);
      const uint32_t top_count = n < 5 ? n : 5;
      for (uint32_t i = 0; i < top_count; i++) {
         char label[32];
         npt_profile_format_cmd(label, sizeof(label), deltas[i].cmd_type);
         npt_log("STUTTER-CMD-COUNT cmd=0x%08x %s "
                 "count=+%u submit_ms=%.2f reply_ms=%.2f bytes=+%llu",
                 (unsigned)deltas[i].cmd_type, label,
                 (unsigned)deltas[i].count,
                 (double)deltas[i].submit_ns / 1e6,
                 (double)deltas[i].reply_ns  / 1e6,
                 (unsigned long long)deltas[i].bytes);
      }

      qsort(deltas, n, sizeof(deltas[0]), cmd_delta_cmp_replyns_desc);
      const uint32_t top_reply = n < 5 ? n : 5;
      for (uint32_t i = 0; i < top_reply; i++) {
         if (!deltas[i].reply_ns)
            break;
         char label[32];
         npt_profile_format_cmd(label, sizeof(label), deltas[i].cmd_type);
         npt_log("STUTTER-CMD-REPLY cmd=0x%08x %s "
                 "count=+%u reply_ms=%.2f submit_ms=%.2f",
                 (unsigned)deltas[i].cmd_type, label,
                 (unsigned)deltas[i].count,
                 (double)deltas[i].reply_ns  / 1e6,
                 (double)deltas[i].submit_ns / 1e6);
      }

      /* Refresh from the post-aggregation snapshot. */
      npt_profile.last_present_agg = agg;
   } else {
      npt_profile_agg_snapshot(&npt_profile.last_present_agg);
   }

   memcpy(npt_profile.last_present_slots, npt_profile.slots,
          sizeof(npt_profile.last_present_slots));
   npt_profile.last_present_ns = now;
   /* Snapshot the Present thread's TLS counters so the next stutter
    * shows the *delta* on this thread only. */
   npt_tl_reply_ns_prev    = npt_tl_reply_ns;
   npt_tl_sync_count_prev  = npt_tl_sync_count;
   npt_tl_submit_ns_prev   = npt_tl_submit_ns;
   npt_tl_submit_count_prev = npt_tl_submit_count;
}

void
npt_profile_dump(const char *reason)
{
   if (!npt_profile.enabled)
      return;

   struct npt_profile_ring agg;
   memset(&agg, 0, sizeof(agg));

   mtx_lock(&npt_profile.rings_mutex);
   list_for_each_entry(struct npt_ring, r, &npt_profile.rings,
                       profile_head) {
      const struct npt_profile_ring *p = &r->profile;
      agg.submits             += p->submits;
      agg.submits_async       += p->submits_async;
      agg.submits_sync        += p->submits_sync;
      agg.bytes_submitted     += p->bytes_submitted;
      agg.notifies            += p->notifies;
      agg.indirect_submits    += p->indirect_submits;
      agg.full_waits          += p->full_waits;
      agg.space_wait_iters    += p->space_wait_iters;
      agg.reply_waits         += p->reply_waits;
      agg.reply_wait_iters    += p->reply_wait_iters;
      agg.total_submit_ns     += p->total_submit_ns;
      agg.total_reply_ns      += p->total_reply_ns;
      agg.roundtrips          += p->roundtrips;

      npt_log("NPT-PROF-GUEST-RING reason=%s ring_id=%llu kind=%s "
              "is_tls=%d submits=%llu async=%llu sync=%llu bytes=%llu "
              "notify=%llu indirect=%llu full_waits=%llu "
              "space_iters=%llu reply_waits=%llu reply_iters=%llu "
              "submit_ns=%llu reply_ns=%llu rt=%llu",
              reason ? reason : "?",
              (unsigned long long)r->id,
              npt_profile_ring_kind(r),
              r->is_tls_ring ? 1 : 0,
              (unsigned long long)p->submits,
              (unsigned long long)p->submits_async,
              (unsigned long long)p->submits_sync,
              (unsigned long long)p->bytes_submitted,
              (unsigned long long)p->notifies,
              (unsigned long long)p->indirect_submits,
              (unsigned long long)p->full_waits,
              (unsigned long long)p->space_wait_iters,
              (unsigned long long)p->reply_waits,
              (unsigned long long)p->reply_wait_iters,
              (unsigned long long)p->total_submit_ns,
              (unsigned long long)p->total_reply_ns,
              (unsigned long long)p->roundtrips);
   }
   mtx_unlock(&npt_profile.rings_mutex);

   npt_log("NPT-PROF-GUEST reason=%s submits=%llu async=%llu sync=%llu "
           "bytes=%llu notify=%llu indirect=%llu full_waits=%llu "
           "space_iters=%llu reply_waits=%llu reply_iters=%llu "
           "submit_ns=%llu reply_ns=%llu rt=%llu",
           reason ? reason : "?",
           (unsigned long long)agg.submits,
           (unsigned long long)agg.submits_async,
           (unsigned long long)agg.submits_sync,
           (unsigned long long)agg.bytes_submitted,
           (unsigned long long)agg.notifies,
           (unsigned long long)agg.indirect_submits,
           (unsigned long long)agg.full_waits,
           (unsigned long long)agg.space_wait_iters,
           (unsigned long long)agg.reply_waits,
           (unsigned long long)agg.reply_wait_iters,
           (unsigned long long)agg.total_submit_ns,
           (unsigned long long)agg.total_reply_ns,
           (unsigned long long)agg.roundtrips);

   /* Top 20 hot methods. */
   struct npt_profile_slot *ptrs[NPT_PROFILE_NUM_SLOTS];
   uint32_t n = 0;
   for (uint32_t i = 0; i < NPT_PROFILE_NUM_SLOTS; i++) {
      if (npt_profile.slots[i].cmd_type != 0 && npt_profile.slots[i].count != 0)
         ptrs[n++] = &npt_profile.slots[i];
   }
   qsort(ptrs, n, sizeof(ptrs[0]), npt_profile_cmp_desc);

   const uint32_t top_n = n < 20u ? n : 20u;
   for (uint32_t i = 0; i < top_n; i++) {
      char label[32];
      npt_profile_format_cmd(label, sizeof(label), ptrs[i]->cmd_type);
      npt_log("NPT-PROF-GUEST-METHOD cmd=0x%08x %s count=%u "
              "submit_ns=%llu reply_ns=%llu bytes=%llu",
              (unsigned)ptrs[i]->cmd_type, label,
              (unsigned)ptrs[i]->count,
              (unsigned long long)ptrs[i]->submit_ns,
              (unsigned long long)ptrs[i]->reply_ns,
              (unsigned long long)ptrs[i]->bytes);
   }
}

void
npt_profile_log_present_timing(unsigned sync_interval,
                               uint64_t t0, uint64_t t1,
                               uint64_t t2, uint64_t t3)
{
   static _Atomic unsigned long s_n = 0;
   const unsigned long n =
      atomic_fetch_add_explicit(&s_n, 1, memory_order_relaxed) + 1;
   npt_log("NPT-PRESENT-TIMING #%lu sync=%u t_us=%llu bp_us=%llu "
           "call_us=%llu wsi_us=%llu total_us=%llu",
           n, sync_interval,
           (unsigned long long)(t0 / 1000ull),
           (unsigned long long)((t1 - t0) / 1000ull),
           (unsigned long long)((t2 - t1) / 1000ull),
           (unsigned long long)((t3 - t2) / 1000ull),
           (unsigned long long)((t3 - t0) / 1000ull));
}

void
npt_profile_log_present_order(unsigned flip_idx,
                              unsigned image_index_pre,
                              unsigned image_count,
                              int wait_fence_fd)
{
   static _Atomic unsigned long s_n = 0;
   const unsigned long n =
      atomic_fetch_add_explicit(&s_n, 1, memory_order_relaxed) + 1;
   npt_log("NPT-PO-A seq=%lu flip_idx=%u image_index_pre=%u "
           "image_count=%u wait_fence_fd=%d tid=%u",
           n, flip_idx, image_index_pre, image_count, wait_fence_fd,
           (unsigned)GetCurrentThreadId());
}
