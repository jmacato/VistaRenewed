/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Host-side performance profiling implementation; see npt_profile.h
 * for design notes.
 */

#include "npt_profile.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "npt_common.h"
#include "npt_ring.h"

struct npt_profile npt_profile;

uint64_t
npt_profile_now_ns(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void
npt_profile_init(void)
{
   mtx_init(&npt_profile.rings_mutex, mtx_plain);
   list_inithead(&npt_profile.rings);

   if (!NPT_DEBUG(PROFILE)) {
      npt_profile.enabled = false;
      return;
   }

   npt_profile.enabled = true;
   npt_profile.period_ns =
      (uint64_t)npt_profile_period_ms * 1000000ull;
   npt_profile.last_dump_ns = npt_profile_now_ns();
   npt_log("profile: enabled, period=%ums",
           (unsigned)npt_profile_period_ms);
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

static void
npt_profile_format_cmd(char *buf, size_t buf_size, uint32_t cmd_type)
{
   uint32_t group = (cmd_type >> 24) & 0xFFu;
   if (group == 255u) {
      /* bits 23:8 = interface_id, bits 7:0 = method wire index. */
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
   const struct npt_profile_slot *pa = a;
   const struct npt_profile_slot *pb = b;
   if (pb->dispatch_ns != pa->dispatch_ns)
      return pb->dispatch_ns > pa->dispatch_ns ? 1 : -1;
   return 0;
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
      agg.dispatches   += p->dispatches;
      agg.dispatch_ns  += p->dispatch_ns;
      agg.relax_ns     += p->relax_ns;
      agg.relax_calls  += p->relax_calls;
      agg.idle_waits   += p->idle_waits;
      agg.idle_wait_ns += p->idle_wait_ns;
      agg.read_bytes   += p->read_bytes;

      npt_log("NPT-PROF-HOST-RING reason=%s ring_id=%llu "
              "dispatches=%llu dispatch_ns=%llu relax_ns=%llu "
              "relax_calls=%llu idle_waits=%llu idle_ns=%llu "
              "read_bytes=%llu",
              reason ? reason : "?",
              (unsigned long long)r->id,
              (unsigned long long)p->dispatches,
              (unsigned long long)p->dispatch_ns,
              (unsigned long long)p->relax_ns,
              (unsigned long long)p->relax_calls,
              (unsigned long long)p->idle_waits,
              (unsigned long long)p->idle_wait_ns,
              (unsigned long long)p->read_bytes);
   }
   mtx_unlock(&npt_profile.rings_mutex);

   npt_log("NPT-PROF-HOST reason=%s dispatches=%llu dispatch_ns=%llu "
           "relax_ns=%llu relax_calls=%llu idle_waits=%llu idle_ns=%llu "
           "read_bytes=%llu lookups=%llu lookup_ns=%llu lookup_misses=%llu",
           reason ? reason : "?",
           (unsigned long long)agg.dispatches,
           (unsigned long long)agg.dispatch_ns,
           (unsigned long long)agg.relax_ns,
           (unsigned long long)agg.relax_calls,
           (unsigned long long)agg.idle_waits,
           (unsigned long long)agg.idle_wait_ns,
           (unsigned long long)agg.read_bytes,
           (unsigned long long)npt_profile.lookups,
           (unsigned long long)npt_profile.lookup_ns,
           (unsigned long long)npt_profile.lookup_misses);

   /* Snapshot slots before sorting + printing.  Ring threads keep
    * writing into npt_profile.slots[] concurrently (the per-cmd_type
    * counter race is benign — see npt_profile.h).  Copying entire
    * slot structs into a local snapshot bounds the race to at most
    * one torn read per slot, avoiding the much worse case where a
    * slot is re-keyed to a different cmd_type between filter and
    * format. */
   struct npt_profile_slot snap[NPT_PROFILE_NUM_SLOTS];
   uint32_t n = 0;
   for (uint32_t i = 0; i < NPT_PROFILE_NUM_SLOTS; i++) {
      struct npt_profile_slot s = npt_profile.slots[i];
      if (s.cmd_type != 0 && s.count != 0)
         snap[n++] = s;
   }
   qsort(snap, n, sizeof(snap[0]), npt_profile_cmp_desc);

   const uint32_t top_n = n < 20u ? n : 20u;
   for (uint32_t i = 0; i < top_n; i++) {
      char label[32];
      npt_profile_format_cmd(label, sizeof(label), snap[i].cmd_type);
      npt_log("NPT-PROF-HOST-METHOD cmd=0x%08x %s count=%u "
              "dispatch_ns=%llu lookup_ns=%llu",
              (unsigned)snap[i].cmd_type, label,
              (unsigned)snap[i].count,
              (unsigned long long)snap[i].dispatch_ns,
              (unsigned long long)snap[i].lookup_ns);
   }
}

void
npt_profile_log_q_retire(uint32_t ring_idx, uint64_t fence_id,
                         int sync_fd, uint64_t poll_us, int rc)
{
   npt_log("NPT-Q-RETIRE queue=%u fence_id=%" PRIu64
           " sync_fd=%d poll_us=%" PRIu64 " rc=%d",
           ring_idx, fence_id, sync_fd, poll_us, rc);
}
