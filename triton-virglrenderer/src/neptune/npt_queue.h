/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Per-ring sync queue.  A worker thread processes a list of
 * (sync_fd, ring_idx, fence_id) tuples and calls ctx->retire_fence
 * on each in order.  We don't own a Vulkan queue: the GPU work is
 * performed by the host D3D library; we wait on the sync_file fd it
 * hands us via the present callback.
 */

#ifndef NPT_QUEUE_H
#define NPT_QUEUE_H

#include "npt_common.h"

struct npt_context;

struct npt_queue_sync {
   /* sync-file fd carrying the GPU-done payload.  -1 means no GPU
    * work to wait on (e.g. CPU-timeline fence) and the worker
    * retires immediately.  Closed by the worker after the wait. */
   int sync_fd;

   uint32_t flags;
   uint32_t ring_idx;
   uint64_t fence_id;

   /* Bumped on each poll(sync_fd) timeout; once it crosses a
    * threshold the worker retires as device-lost rather than spin. */
   unsigned timeouts;

   struct list_head head;
};

struct npt_queue {
   struct npt_context *context;
   uint32_t ring_idx;

   /* Submitted fences go on sync_thread.syncs.  The worker pops in
    * order, polls on sync_fd, and retires via ctx->retire_fence. */
   struct {
      mtx_t mutex;
      cnd_t cond;
      struct list_head syncs;
      thrd_t thread;
      bool join;
   } sync_thread;
};

struct npt_queue *
npt_queue_create(struct npt_context *ctx, uint32_t ring_idx);

void
npt_queue_destroy(struct npt_queue *queue);

bool
npt_queue_sync_submit(struct npt_queue *queue,
                      uint32_t flags,
                      uint32_t ring_idx,
                      uint64_t fence_id,
                      int sync_fd);

#endif /* NPT_QUEUE_H */
