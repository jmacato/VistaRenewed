/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Growable shmem bump-allocator pool.  Each pool_alloc returns a
 * refcounted shmem + offset; on grow, in-flight callers' refs keep
 * the old shmem alive until released.
 */

#ifndef NPT_SHMEM_POOL_H
#define NPT_SHMEM_POOL_H

#include "npt_common.h"
#include "npt_renderer.h"

struct npt_shmem_pool {
   mtx_t mutex;
   size_t min_alloc_size;
   struct npt_renderer_shmem *shmem;
   size_t size;
   size_t used;
   /* True when shmem was just created and no caller has yet
    * roundtripped to ensure the host's RESOURCE_CREATE_BLOB has
    * landed. */
   bool shmem_needs_roundtrip;
};

void
npt_shmem_pool_init(struct npt_shmem_pool *pool, size_t min_alloc_size);

void
npt_shmem_pool_fini(struct npt_renderer *renderer,
                    struct npt_shmem_pool *pool);

/*
 * Caller takes the returned shmem ref and must unref when done with
 * the region.  *out_shmem_fresh=true => shmem was just created;
 * caller must roundtrip before naming res_id over the ring.  NULL
 * out_shmem_fresh is fine if the shmem is never handed to the host.
 */
struct npt_renderer_shmem *
npt_shmem_pool_alloc(struct npt_renderer *renderer,
                     struct npt_shmem_pool *pool,
                     size_t size, size_t *out_offset,
                     bool *out_shmem_fresh);

#endif /* NPT_SHMEM_POOL_H */
