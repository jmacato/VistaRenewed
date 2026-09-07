/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * NPT_PERF / NPT_DEBUG hold comma-separated tokens, each toggling
 * one bit in the matching bitmask.  Perf tokens follow the "no_*"
 * opt-out convention (everything on by default; the env is a
 * bisection killswitch).  Add a new flag by extending the enum here
 * and the option table in npt_env.c.
 */

#ifndef NPT_ENV_H
#define NPT_ENV_H

#include <stdint.h>

#include "npt_common.h"
#include "util/macros.h"

enum npt_perf {
   NPT_PERF_NO_QUERY_FEEDBACK        = 1ull << 0,

   /* Opt-in: multi-ring on D3D11 is not the default. */
   NPT_PERF_MULTI_RING               = 1ull << 1,

   NPT_PERF_NO_ASYNC_BUFFER_CREATE   = 1ull << 3,
   NPT_PERF_NO_ASYNC_TEXTURE_CREATE  = 1ull << 4,
   NPT_PERF_NO_DYNAMIC_MAP_FAST_PATH = 1ull << 5,
   NPT_PERF_NO_FENCE_FEEDBACK        = 1ull << 6,
};

enum npt_debug {
   NPT_DEBUG_PROFILE                 = 1ull << 0,

   /* Skip the virtgpu probe in npt_device_create so the guest falls
    * through to the vtest backend even when /dev/dri/renderDxxx is
    * accessible. */
   NPT_DEBUG_FORCE_VTEST             = 1ull << 1,

   /* Per-Present timing breakdown. */
   NPT_DEBUG_PRESENT_TIMING          = 1ull << 2,

   /* Per-Present image-index trace across the WSI chain
    * (sc_Present_override -> npt_do_wsi_present -> X11 IDLE_NOTIFY).
    * Diagnoses visible frames appearing out of order. */
   NPT_DEBUG_PRESENT_ORDER           = 1ull << 3,

   /* Report the connector's full KMS mode list through IDXGIOutput
    * instead of capping it to the active CRTC scanout.  For modeset
    * experiments where the guest is meant to drive resolution changes. */
   NPT_DEBUG_EXPOSE_ALL_MODES        = 1ull << 4,
};

struct npt_env {
   uint64_t perf;
   uint64_t debug;
   /* Scalar tuning knobs (parsed once in npt_env_init).  These are
    * runtime-tunable values that don't fit the NPT_PERF/NPT_DEBUG
    * bitmask, but live here so every env-var read is in one place. */
   uint32_t profile_period_ms;     /* NPT_PROFILE_PERIOD_MS */
   uint32_t stutter_threshold_ms;  /* NPT_STUTTER_MS */
   uint64_t tls_idle_timeout_ns;   /* NPT_TLS_IDLE_TIMEOUT_NS */
};

extern struct npt_env npt_env;

void npt_env_init(void);

/* unlikely(): "perf disabled" is the rare bisect path. */
#define NPT_PERF(category)  (unlikely(npt_env.perf & NPT_PERF_##category))
#define NPT_DEBUG(category) (unlikely(npt_env.debug & NPT_DEBUG_##category))

#endif /* NPT_ENV_H */
