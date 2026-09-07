/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Per-ring sync-queue worker thread.  Polls a sync_file fd per
 * submitted fence and retires the fence when the host signals it.
 */

#include "npt_queue.h"

#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>

#include "npt_context.h"
#include "npt_profile.h"
#include "util/u_thread.h"

/* ----------------------------------------------------------------- */
/* sync alloc / free / retire                                          */
/* ----------------------------------------------------------------- */

static struct npt_queue_sync *
npt_queue_alloc_sync(uint32_t flags,
                     uint32_t ring_idx,
                     uint64_t fence_id,
                     int sync_fd)
{
   struct npt_queue_sync *sync = malloc(sizeof(*sync));
   if (!sync)
      return NULL;

   sync->sync_fd  = sync_fd;
   sync->flags    = flags;
   sync->ring_idx = ring_idx;
   sync->fence_id = fence_id;
   sync->timeouts = 0;

   return sync;
}

static void
npt_queue_free_sync(struct npt_queue_sync *sync)
{
   if (sync->sync_fd >= 0)
      close(sync->sync_fd);
   free(sync);
}

static inline void
npt_queue_sync_retire(struct npt_queue *queue, struct npt_queue_sync *sync)
{
   queue->context->retire_fence(queue->context->ctx_id,
                                 sync->ring_idx, sync->fence_id);
   npt_queue_free_sync(sync);
}

/* ----------------------------------------------------------------- */
/* submit                                                              */
/* ----------------------------------------------------------------- */

bool
npt_queue_sync_submit(struct npt_queue *queue,
                      uint32_t flags,
                      uint32_t ring_idx,
                      uint64_t fence_id,
                      int sync_fd)
{
   struct npt_queue_sync *sync =
      npt_queue_alloc_sync(flags, ring_idx, fence_id, sync_fd);
   if (!sync) {
      if (sync_fd >= 0)
         close(sync_fd);
      return false;
   }

   /* Append to the queue's sync list and wake the worker. */
   mtx_lock(&queue->sync_thread.mutex);
   list_addtail(&sync->head, &queue->sync_thread.syncs);
   cnd_signal(&queue->sync_thread.cond);
   mtx_unlock(&queue->sync_thread.mutex);

   return true;
}

/* ----------------------------------------------------------------- */
/* worker thread                                                       */
/* ----------------------------------------------------------------- */

static int
npt_wait_sync_fd(int fd, int timeout_ms)
{
   if (fd < 0)
      return 0;

   struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
   int ret;
   do {
      ret = poll(&pfd, 1, timeout_ms);
   } while (ret < 0 && errno == EINTR);

   /* Treat POLLNVAL / POLLERR / POLLHUP and any other error as a
    * signal so the worker retires the fence rather than re-polling a
    * dead fd forever.  A concurrent close() of the sync fd can race a
    * poll already in flight; the sync queue is that fd's only
    * downstream and must not wedge. */
   if (ret > 0 && (pfd.revents & (POLLNVAL | POLLERR | POLLHUP)))
      return 1;
   if (ret < 0)
      return 1;
   return ret;
}

static int
npt_queue_thread(void *arg)
{
   struct npt_queue *queue = arg;
   struct npt_context *ctx = queue->context;
   char thread_name[16];
   snprintf(thread_name, sizeof(thread_name), "npt-queue-%u", ctx->ctx_id);
   u_thread_setname(thread_name);

   /* sync_thread.mutex protects the list; dropped across the poll,
    * reacquired before popping the entry. */
   const int kPollTimeoutMs = 3000;

   /* Bounded device-lost bailout.  Without a clear DEVICE_LOST
    * signal (we only have a sync_file fd), consecutive poll
    * timeouts are the best evidence the producer is wedged.  After
    * ~30 s, retire so the guest unblocks instead of hanging forever. */
   const unsigned kMaxTimeouts = 10;

   mtx_lock(&queue->sync_thread.mutex);
   while (true) {
      while (list_is_empty(&queue->sync_thread.syncs) && !queue->sync_thread.join)
         cnd_wait(&queue->sync_thread.cond, &queue->sync_thread.mutex);

      if (queue->sync_thread.join)
         break;

      struct npt_queue_sync *sync =
         list_first_entry(&queue->sync_thread.syncs, struct npt_queue_sync, head);

      mtx_unlock(&queue->sync_thread.mutex);

      struct timespec t0, t1;
      const bool trace = NPT_DEBUG(FENCE_TRACE);
      if (trace)
         clock_gettime(CLOCK_MONOTONIC, &t0);
      int rc = npt_wait_sync_fd(sync->sync_fd, kPollTimeoutMs);
      if (trace)
         clock_gettime(CLOCK_MONOTONIC, &t1);

      mtx_lock(&queue->sync_thread.mutex);

      if (rc == 0) {
         /* Poll timeout: bump the counter, and once it crosses
          * kMaxTimeouts retire the fence anyway so a wedged host
          * (driver hang, missing sync_file signal) doesn't trap the
          * guest forever.  Healthy GPUs never hit more than one or
          * two timeouts in a row. */
         if (++sync->timeouts < kMaxTimeouts)
            continue;
         npt_log("queue %u fence_id=%" PRIu64 ": timed out %u times "
                 "(~%u s), retiring as device-lost",
                 queue->ring_idx, sync->fence_id, sync->timeouts,
                 kMaxTimeouts * (kPollTimeoutMs / 1000));
      }

      if (trace) {
         const int64_t dt_ns =
            (int64_t)(t1.tv_sec  - t0.tv_sec ) * 1000000000ll +
            (int64_t)(t1.tv_nsec - t0.tv_nsec);
         const uint64_t wait_us = dt_ns > 0 ? (uint64_t)(dt_ns / 1000) : 0;
         npt_profile_log_q_retire(queue->ring_idx, sync->fence_id,
                                  sync->sync_fd, wait_us, rc);
      }

      list_del(&sync->head);
      npt_queue_sync_retire(queue, sync);
   }
   mtx_unlock(&queue->sync_thread.mutex);

   return 0;
}

/* ----------------------------------------------------------------- */
/* thread init / fini                                                  */
/* ----------------------------------------------------------------- */

static int
npt_queue_sync_thread_init(struct npt_queue *queue)
{
   STATIC_ASSERT(thrd_success == 0);

   int ret = mtx_init(&queue->sync_thread.mutex, mtx_plain);
   if (ret != thrd_success)
      return ret;

   ret = cnd_init(&queue->sync_thread.cond);
   if (ret != thrd_success)
      goto fail_cnd_init;

   list_inithead(&queue->sync_thread.syncs);
   queue->sync_thread.join = false;

   ret = thrd_create(&queue->sync_thread.thread, npt_queue_thread, queue);
   if (ret != thrd_success)
      goto fail_thrd_create;

   return 0;

fail_thrd_create:
   cnd_destroy(&queue->sync_thread.cond);
fail_cnd_init:
   mtx_destroy(&queue->sync_thread.mutex);
   return ret;
}

static void
npt_queue_sync_thread_fini(struct npt_queue *queue)
{
   /* Signal exit and join. */
   mtx_lock(&queue->sync_thread.mutex);
   queue->sync_thread.join = true;
   cnd_signal(&queue->sync_thread.cond);
   mtx_unlock(&queue->sync_thread.mutex);

   thrd_join(queue->sync_thread.thread, NULL);

   /* Drain leftover syncs: free them and let the renderer treat
    * unretired fences as context-destroy. */
   unsigned leftover = 0;
   list_for_each_entry_safe (struct npt_queue_sync, sync,
                             &queue->sync_thread.syncs, head) {
      list_del(&sync->head);
      npt_queue_sync_retire(queue, sync);
      leftover++;
   }
   if (leftover)
      npt_log("queue %u: drained %u leftover sync(s) on destroy",
              queue->ring_idx, leftover);

   mtx_destroy(&queue->sync_thread.mutex);
   cnd_destroy(&queue->sync_thread.cond);
}

/* ----------------------------------------------------------------- */
/* create / destroy                                                    */
/* ----------------------------------------------------------------- */

struct npt_queue *
npt_queue_create(struct npt_context *ctx, uint32_t ring_idx)
{
   /* Reject ring_idx values that would overflow ctx->sync_queues[].
    * Submit already bounds-checks, but creation has other callers; a
    * defensive check here prevents an orphan queue from arising on a
    * corrupt-command path. */
   if (ring_idx >= ARRAY_SIZE(ctx->sync_queues))
      return NULL;

   struct npt_queue *queue = calloc(1, sizeof(*queue));
   if (!queue)
      return NULL;

   queue->context  = ctx;
   queue->ring_idx = ring_idx;

   if (npt_queue_sync_thread_init(queue)) {
      free(queue);
      return NULL;
   }

   return queue;
}

void
npt_queue_destroy(struct npt_queue *queue)
{
   if (!queue)
      return;
   npt_queue_sync_thread_fini(queue);
   free(queue);
}
