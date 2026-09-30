/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Shared aux types for asynchronous wrappers. Fences use host-written
 * feedback slots; queries use ordered GetData commands. Per-type aux
 * structs embed npt_d3d11_feedback_aux as their first member.
 */

#ifndef NPT_OVERRIDES_D3D11_FEEDBACK_H
#define NPT_OVERRIDES_D3D11_FEEDBACK_H

#include "npt_common.h"

struct npt_com_base;
struct npt_renderer_shmem;

struct npt_d3d11_feedback_aux {
   struct npt_com_base *com;

   /* NULL shmem = registration skipped (env opt-out, pool OOM, or
    * wrapper came via QI without going through Create); the
    * type-specific path falls back to the sync wire round-trip. */
   struct npt_renderer_shmem *fb_shmem;
   uint32_t fb_offset;

   /* Cleared on aux_destroy so UNREGISTER fires exactly once. */
   bool registered;
};

struct npt_d3d11_query_aux {
   /* Must be first so generic helpers can downcast. */
   struct npt_d3d11_feedback_aux base;

   /* Result size from the creation descriptor, or zero if unknown. */
   uint32_t query_data_size;

   /* Reserved while query feedback is disabled. Repeated queries need
    * generation changes ordered with their Begin/End commands. */
   _Atomic uint32_t local_version;
};

/* Type-safe downcast from an ID3D11Asynchronous (or any tier in the
 * query family: Query, Query1, Predicate, Counter) to its aux.
 * Returns NULL if `async` is not a wrapper in the query family --
 * checked via vtbl identity against the family's tier vtbls, so
 * QI-routed pointers from other families fail cleanly. */
struct npt_d3d11_query_aux *npt_d3d11_query_aux_cast(void *async);

#endif /* NPT_OVERRIDES_D3D11_FEEDBACK_H */
