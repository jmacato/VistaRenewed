/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 */

#include "npt_shmem_pool.h"

void
npt_shmem_pool_init(struct npt_shmem_pool *pool, size_t min_alloc_size)
{
   memset(pool, 0, sizeof(*pool));
   pool->min_alloc_size = 1;
   while (pool->min_alloc_size < min_alloc_size)
      pool->min_alloc_size <<= 1;
   mtx_init(&pool->mutex, mtx_plain);
}

void
npt_shmem_pool_fini(struct npt_renderer *renderer,
                    struct npt_shmem_pool *pool)
{
   if (pool->shmem)
      npt_renderer_shmem_unref(renderer, pool->shmem);
   mtx_destroy(&pool->mutex);
}

static bool
npt_shmem_pool_grow_locked(struct npt_renderer *renderer,
                           struct npt_shmem_pool *pool,
                           size_t size)
{
   size_t alloc_size = pool->min_alloc_size;
   while (alloc_size < size) {
      alloc_size <<= 1;
      if (!alloc_size)
         return false;
   }

   struct npt_renderer_shmem *shmem =
      npt_renderer_shmem_create(renderer, alloc_size);
   if (!shmem) {
      npt_log("shmem pool: failed to allocate %zu bytes", alloc_size);
      return false;
   }

   if (pool->shmem)
      npt_renderer_shmem_unref(renderer, pool->shmem);

   pool->shmem = shmem;
   pool->size = alloc_size;
   pool->used = 0;
   /* RESOURCE_CREATE_BLOB is async vs the ring thread; flag so the
    * first caller pays the virtqueue-seqno roundtrip before naming
    * res_id over the ring. */
   pool->shmem_needs_roundtrip = true;
   return true;
}

struct npt_renderer_shmem *
npt_shmem_pool_alloc(struct npt_renderer *renderer,
                     struct npt_shmem_pool *pool,
                     size_t size, size_t *out_offset,
                     bool *out_shmem_fresh)
{
   mtx_lock(&pool->mutex);

   if (size > pool->size - pool->used) {
      if (!npt_shmem_pool_grow_locked(renderer, pool, size)) {
         mtx_unlock(&pool->mutex);
         if (out_shmem_fresh)
            *out_shmem_fresh = false;
         return NULL;
      }
   }

   struct npt_renderer_shmem *shmem =
      npt_renderer_shmem_ref(renderer, pool->shmem);
   *out_offset = pool->used;
   pool->used += size;
   pool->used = (pool->used + 7) & ~(size_t)7;

   /* Report-once: only the first alloc after a grow pays the roundtrip. */
   if (out_shmem_fresh) {
      *out_shmem_fresh = pool->shmem_needs_roundtrip;
      pool->shmem_needs_roundtrip = false;
   }

   mtx_unlock(&pool->mutex);
   return shmem;
}
