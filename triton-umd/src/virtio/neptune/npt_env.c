/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 */

#include "npt_env.h"

#include <stdlib.h>

#include "util/u_debug.h"
#include "util/os_misc.h"

struct npt_env npt_env;

static const struct debug_control npt_perf_options[] = {
   { "no_query_feedback",        NPT_PERF_NO_QUERY_FEEDBACK },
   { "multi_ring",               NPT_PERF_MULTI_RING },
   { "no_async_buffer_create",   NPT_PERF_NO_ASYNC_BUFFER_CREATE },
   { "no_async_texture_create",  NPT_PERF_NO_ASYNC_TEXTURE_CREATE },
   { "no_dynamic_map_fast_path", NPT_PERF_NO_DYNAMIC_MAP_FAST_PATH },
   { "no_fence_feedback",        NPT_PERF_NO_FENCE_FEEDBACK },
   { NULL, 0 },
};

static const struct debug_control npt_debug_options[] = {
   { "profile",                  NPT_DEBUG_PROFILE },
   { "force_vtest",              NPT_DEBUG_FORCE_VTEST },
   { "present_timing",           NPT_DEBUG_PRESENT_TIMING },
   { "present_order",            NPT_DEBUG_PRESENT_ORDER },
   { "expose_all_modes",         NPT_DEBUG_EXPOSE_ALL_MODES },
   { NULL, 0 },
};

static uint32_t
parse_clamped_u32(const char *name, uint32_t fallback,
                  uint32_t lo, uint32_t hi)
{
   const char *s = os_get_option(name);
   if (!s || !*s)
      return fallback;
   unsigned long v = strtoul(s, NULL, 10);
   return (v >= lo && v <= hi) ? (uint32_t)v : fallback;
}

static uint64_t
parse_nonzero_u64(const char *name, uint64_t fallback)
{
   const char *s = os_get_option(name);
   if (!s || !*s)
      return fallback;
   unsigned long long v = strtoull(s, NULL, 10);
   return v ? (uint64_t)v : fallback;
}

static void
npt_env_init_impl(void)
{
   npt_env.perf =
      parse_debug_string(os_get_option("NPT_PERF"), npt_perf_options);
   npt_env.debug =
      parse_debug_string(os_get_option("NPT_DEBUG"), npt_debug_options);

   /* Profiler dump cadence; only consulted under NPT_DEBUG=profile. */
   npt_env.profile_period_ms =
      parse_clamped_u32("NPT_PROFILE_PERIOD_MS", 1000u, 1u, 60000u);

   /* Present-to-Present gap triggering a STUTTER log line.  Default
    * 50 ms (= the gap is < 20 fps).  */
   npt_env.stutter_threshold_ms =
      parse_clamped_u32("NPT_STUTTER_MS", 50u, 1u, 10000u);

   /* TLS rings park 10x longer than the primary ring's 1ms idle so
    * worker-thread traffic doesn't churn the host wakeup path.  +9ms
    * worst-case first-submit-after-park latency. */
   npt_env.tls_idle_timeout_ns =
      parse_nonzero_u64("NPT_TLS_IDLE_TIMEOUT_NS", 10ull * 1000000ull);
}

static _Atomic int g_npt_env_init_state;

void
npt_env_init(void)
{
   NPT_CALL_ONCE(g_npt_env_init_state, npt_env_init_impl());
}
