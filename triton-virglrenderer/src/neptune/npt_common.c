/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 */

#include "npt_common.h"

#include <stdarg.h>

#include "util/u_debug.h"

static const struct debug_named_value npt_debug_options[] = {
   { "profile",     NPT_DEBUG_PROFILE,     "Enable host-side profiling" },
   { "fence_trace", NPT_DEBUG_FENCE_TRACE, "Per-frame fence/queue trace" },
   DEBUG_NAMED_VALUE_END
};

uint32_t npt_debug_flags;
uint32_t npt_profile_period_ms;

DEBUG_GET_ONCE_FLAGS_OPTION(npt_debug_flags, "NPT_DEBUG", npt_debug_options, 0)
DEBUG_GET_ONCE_NUM_OPTION(npt_profile_period_ms_raw,
                          "NPT_PROFILE_PERIOD_MS", 1000)

void
npt_debug_init(void)
{
   npt_debug_flags = debug_get_option_npt_debug_flags();

   /* Clamp to a sane range so a huge value can't suppress dumps. */
   long v = debug_get_option_npt_profile_period_ms_raw();
   npt_profile_period_ms = (v > 0 && v < 60000) ? (uint32_t)v : 1000u;
}

void
npt_log(const char *fmt, ...)
{
   va_list va;
   va_start(va, fmt);
   virgl_prefixed_logv("npt", VIRGL_LOG_LEVEL_INFO, fmt, va);
   va_end(va);
}
