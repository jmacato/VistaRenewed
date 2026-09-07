/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 */

#include <assert.h>
#include "npt_event.h"
#include "npt_device.h"
#include "npt_dispatch.h"
#include "npt_renderer.h"
#include "npt_ring.h"
#include "nptunix/npt_unixlib.h"

#include "util/hash_table.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <unistd.h>
#endif

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct event_pending {
   int    sync_fd;
   void  *handle;
   struct event_pending *next;
};

/* Upper bound on waiter threads.  Sized for the concurrent presents a
 * compositor drives through one device; past it arms queue rather than
 * spawn, since each extra thread only pays off while an arm is outstanding. */
#define NPT_EVENT_WAITER_MAX 8

/* Per-registered HANDLE: outstanding-arm refcount.  Inserted on the
 * first arm (refcount=1) and freed under registry_mutex when the
 * waiter completes the last in-flight arm and drops it to zero. */
struct event_registry_entry {
   uint32_t refcount;
};

struct npt_event_state {
   mtx_t                          registry_mutex;
   /* HANDLE -> struct event_registry_entry. */
   struct hash_table             *registry;

   /* Half-open [base, end) carving the upper portion of the host's
    * sync_queues table; sized from the renderer's
    * max_timeline_count so we don't collide with Present rings.
    * See npt_renderer_event_ring_base in npt_renderer.h. */
   uint32_t                       ring_base;
   uint32_t                       ring_end;
   _Atomic uint32_t               next_event_ring_idx;

   /* Holds ARM + submit_present_fence back-to-back on the same ring. */
   mtx_t                          arm_mutex;

   /* Arms waiting for a waiter thread, oldest first.  Each entry occupies a
    * waiter for as long as its fence takes to retire, so the queue is FIFO
    * and serviced by a pool: one thread would make every arm wait out the
    * one ahead of it, and the present gate arms once per frame per
    * swapchain, with several swapchains presenting concurrently. */
   mtx_t                          pending_mutex;
   cnd_t                          pending_cond;
   struct event_pending         *pending_head;
   struct event_pending         *pending_tail;
   uint32_t                       pending_count;
   _Atomic bool                   waiter_join;
   /* Threads alive, and of those the ones blocked in cnd_wait with no entry.
    * Both under pending_mutex; waiter_count is also the used length of
    * waiter_thread. */
   uint32_t                       waiter_count;
   uint32_t                       waiter_idle;
#if defined(_WIN32)
   HANDLE                         waiter_thread[NPT_EVENT_WAITER_MAX];
#else
   pthread_t                      waiter_thread[NPT_EVENT_WAITER_MAX];
#endif
};

/* Drop one outstanding-arm refcount.  When it hits zero, remove the
 * entry from the registry and emit RELEASE_EVENT so the host frees its
 * proxy.  Subsequent arms for the same HANDLE re-REGISTER (host's
 * npt_event_arm lazy-creates if the proxy is missing). */
static void
event_release_one(struct npt_device *dev, void *hEvent)
{
   struct npt_event_state *st = dev->event;
   uint64_t token = (uint64_t)(uintptr_t)hEvent;

   mtx_lock(&st->registry_mutex);
   if (st->registry) {
      struct hash_entry *e = _mesa_hash_table_search(st->registry, hEvent);
      if (e) {
         struct event_registry_entry *ent = e->data;
         if (--ent->refcount == 0) {
            _mesa_hash_table_remove(st->registry, e);
            free(ent);
            /* Submitted on the method ring and still under registry_mutex, so
             * this token's REGISTER/RELEASE reach the host in refcount-
             * transition order (see npt_dispatch_event_release_ring). */
            npt_dispatch_event_release_ring(npt_device_method_ring(dev), token);
         }
      }
   }
   mtx_unlock(&st->registry_mutex);
}

#if defined(__WINE__)
static void
do_set_event(void *handle)
{
   if (handle)
      SetEvent((HANDLE)handle);
}
/* PE has no poll(); route through the unixlib. */
static int
wait_one(int fd, int timeout_ms)
{
   if (npt_unixlib_ensure_init() != 0)
      return -1;
   struct npt_unix_wait_fd_params p = {
      .fd = fd, .timeout_ms = timeout_ms, .result = -1,
   };
   if (npt_wine_unix_call(npt_unix_wait_fd, &p) != 0)
      return -1;
   return p.result;
}
static void
close_fd(int fd)
{
   if (fd < 0 || npt_unixlib_ensure_init() != 0)
      return;
   struct npt_unix_close_params p = { .fd = fd };
   npt_wine_unix_call(npt_unix_close, &p);
}
#elif defined(_WIN32)
static void
do_set_event(void *handle)
{
   if (handle)
      SetEvent((HANDLE)handle);
}
static int
wait_one(int fd, int timeout_ms)
{
   /* fd carries a Win32 auto-reset event HANDLE from
    * npt_vgw32_submit_present_fence; the KMD signals it from its completion DPC
    * when the host retires the event-ring fence, i.e. at GPU completion. */
   HANDLE h = (HANDLE)(ULONG_PTR)(ULONG)fd;
   DWORD rc = WaitForSingleObject(h, (DWORD)timeout_ms);
   return rc == WAIT_OBJECT_0 ? 1 : (rc == WAIT_TIMEOUT ? 0 : -1);
}
static void
close_fd(int fd)
{
   if (fd >= 0)
      CloseHandle((HANDLE)(ULONG_PTR)(ULONG)fd);
}
#else
static void
do_set_event(void *handle)
{
   (void)handle;
}
static int
wait_one(int fd, int timeout_ms)
{
   struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
   int ret;
   do { ret = poll(&pfd, 1, timeout_ms); } while (ret < 0 && errno == EINTR);
   return ret;
}
static void
close_fd(int fd)
{
   if (fd >= 0) close(fd);
}
#endif

#if defined(_WIN32)
static DWORD WINAPI event_waiter_thread(LPVOID arg)
#else
static void *event_waiter_thread(void *arg)
#endif
{
   struct npt_device *dev = arg;
   struct npt_event_state *st = dev->event;

   for (;;) {
      mtx_lock(&st->pending_mutex);
      st->waiter_idle++;
      while (!st->pending_head && !atomic_load(&st->waiter_join))
         cnd_wait(&st->pending_cond, &st->pending_mutex);
      st->waiter_idle--;
      if (atomic_load(&st->waiter_join) && !st->pending_head) {
         mtx_unlock(&st->pending_mutex);
         break;
      }
      struct event_pending *p = st->pending_head;
      st->pending_head = p->next;
      if (!st->pending_head)
         st->pending_tail = NULL;
      st->pending_count--;
      mtx_unlock(&st->pending_mutex);

      /* 5s caps shutdown latency on an arm mid-flight at join. */
      int rc = wait_one(p->sync_fd, 5000);
      if (rc > 0)
         do_set_event(p->handle);
      close_fd(p->sync_fd);
      event_release_one(dev, p->handle);
      free(p);
   }
#if defined(_WIN32)
   return 0;
#else
   return NULL;
#endif
}

/* Add one thread to the pool.  Caller holds pending_mutex except at init,
 * where no other thread can see the state yet. */
static void
event_waiter_spawn(struct npt_device *dev, struct npt_event_state *st)
{
   if (st->waiter_count >= NPT_EVENT_WAITER_MAX)
      return;
#if defined(_WIN32)
   HANDLE t = CreateThread(NULL, 0, event_waiter_thread, dev, 0, NULL);
   if (!t)
      return;
   st->waiter_thread[st->waiter_count++] = t;
#else
   if (pthread_create(&st->waiter_thread[st->waiter_count], NULL,
                      event_waiter_thread, dev) != 0)
      return;
   st->waiter_count++;
#endif
}

void
npt_event_init(struct npt_device *dev)
{
   struct npt_event_state *st = calloc(1, sizeof(*st));
   if (!st) return;

   mtx_init(&st->registry_mutex, mtx_plain);
   mtx_init(&st->arm_mutex, mtx_plain);
   mtx_init(&st->pending_mutex, mtx_plain);
   cnd_init(&st->pending_cond);
   const struct npt_renderer_info *info = &dev->renderer->info;
   st->ring_base = npt_renderer_event_ring_base(info);
   st->ring_end  = npt_renderer_event_ring_end(info);
   if (st->ring_end <= st->ring_base) {
      npt_log("event: max_timeline_count=%u too small to partition; "
              "Win32 events will share Present rings",
              info->max_timeline_count);
      st->ring_base = 1;
      st->ring_end  = info->max_timeline_count > 1
                      ? info->max_timeline_count : 2;
   }
   atomic_init(&st->next_event_ring_idx, st->ring_base);
   atomic_init(&st->waiter_join, false);
   st->pending_head = NULL;
   st->registry = _mesa_pointer_hash_table_create(NULL);

   /* Set dev->event before thread create so the waiter can read it. */
   dev->event = st;

   event_waiter_spawn(dev, st);
}

void
npt_event_fini(struct npt_device *dev)
{
   struct npt_event_state *st = dev->event;
   if (!st) return;

   /* Wake every waiter so they observe the flag immediately.  The count is
    * stable here: only npt_event_arm grows the pool, and the device is being
    * destroyed. */
   mtx_lock(&st->pending_mutex);
   atomic_store(&st->waiter_join, true);
   cnd_broadcast(&st->pending_cond);
   const uint32_t waiters = st->waiter_count;
   mtx_unlock(&st->pending_mutex);

   for (uint32_t i = 0; i < waiters; i++) {
#if defined(_WIN32)
      WaitForSingleObject(st->waiter_thread[i], INFINITE);
      CloseHandle(st->waiter_thread[i]);
#else
      pthread_join(st->waiter_thread[i], NULL);
#endif
   }

   while (st->pending_head) {
      struct event_pending *p = st->pending_head;
      st->pending_head = p->next;
      close_fd(p->sync_fd);
      free(p);
   }
   st->pending_tail  = NULL;
   st->pending_count = 0;

   /* Symmetric RELEASE for every entry left in the registry.  The
    * host's npt_event_fini will sweep anything we miss, but explicit
    * release keeps the host's per-context accounting balanced and
    * silences the host's "leaked at teardown" diagnostic. */
   if (st->registry) {
      hash_table_foreach(st->registry, e) {
         uint64_t token = (uint64_t)(uintptr_t)e->key;
         free(e->data);
         npt_dispatch_event_release(dev->renderer, token);
      }
      _mesa_hash_table_destroy(st->registry, NULL);
   }
   cnd_destroy(&st->pending_cond);
   mtx_destroy(&st->pending_mutex);
   mtx_destroy(&st->arm_mutex);
   mtx_destroy(&st->registry_mutex);
   free(st);
   dev->event = NULL;
}

bool
npt_event_arm(struct npt_device *dev, void *hEvent)
{
   if (!dev || !hEvent || !dev->event)
      return false;

   struct npt_event_state *st = dev->event;
   uint64_t token = (uint64_t)(uintptr_t)hEvent;

   /* Take a refcount under the registry lock; the matching decrement
    * happens in the waiter (or in the failure paths below).  When the
    * count drops to zero event_release_one emits RELEASE_EVENT. */
   mtx_lock(&st->registry_mutex);
   if (!st->registry) {
      mtx_unlock(&st->registry_mutex);
      return false;
   }
   struct hash_entry *e = _mesa_hash_table_search(st->registry, hEvent);
   if (e) {
      ((struct event_registry_entry *)e->data)->refcount++;
   } else {
      struct event_registry_entry *ent = calloc(1, sizeof(*ent));
      if (!ent) {
         mtx_unlock(&st->registry_mutex);
         return false;
      }
      ent->refcount = 1;
      _mesa_hash_table_insert(st->registry, hEvent, ent);
      /* Submitted on the method ring so it precedes the ARM_EVENT_FENCE issued
       * below (see npt_dispatch_event_register_ring).  Holding registry_mutex
       * across the submit also keeps this REGISTER before any other thread's
       * ARM, since that thread must take registry_mutex to bump the refcount
       * before it can arm. */
      npt_dispatch_event_register_ring(npt_device_method_ring(dev), token);
   }
   mtx_unlock(&st->registry_mutex);

   /* Round-robin across event rings so independent in-flight arms
    * don't queue up on one sync-queue worker. */
   uint32_t ring_idx = atomic_fetch_add(&st->next_event_ring_idx, 1);
   const uint32_t span = st->ring_end - st->ring_base;
   ring_idx = st->ring_base + (ring_idx - st->ring_base) % span;

   /* Same dispatch thread as the SetEventOnCompletion that follows
    * and the DC::Signal that triggers the fence.  Pre-DC fallback
    * to primary is safe (nothing's racing yet). */
   mtx_lock(&st->arm_mutex);
   bool arm_ok = npt_dispatch_event_arm_fence(npt_device_method_ring(dev),
                                              token, ring_idx);
   if (!arm_ok) {
      mtx_unlock(&st->arm_mutex);
      event_release_one(dev, hEvent);
      return false;
   }
   int sync_fd = npt_renderer_submit_present_fence(dev->renderer, ring_idx);
   mtx_unlock(&st->arm_mutex);

   if (sync_fd < 0) {
      event_release_one(dev, hEvent);
      return false;
   }

   struct event_pending *p = calloc(1, sizeof(*p));
   if (!p) {
      close_fd(sync_fd);
      event_release_one(dev, hEvent);
      return false;
   }
   p->sync_fd = sync_fd;
   p->handle  = hEvent;

   mtx_lock(&st->pending_mutex);
   p->next = NULL;
   if (st->pending_tail)
      st->pending_tail->next = p;
   else
      st->pending_head = p;
   st->pending_tail = p;
   st->pending_count++;
   /* Grow the pool when the queue outruns the waiters still unclaimed: an
    * idle waiter that has been signalled but not yet woken is still counted
    * idle, so comparing against the depth avoids piling a second arm onto it.
    * Each arm gates a present and holds its waiter until the fence retires. */
   if (st->pending_count > st->waiter_idle)
      event_waiter_spawn(dev, st);
   cnd_signal(&st->pending_cond);
   mtx_unlock(&st->pending_mutex);
   return true;
}

/*
 * Single-use token arm for callers that consume the sync_file
 * themselves (the guest swapchain's per-Present GPU-done fence)
 * instead of routing through the waiter thread.  Shares the ring
 * allocator and arm_mutex with npt_event_arm so ARM +
 * submit_present_fence pairs from both users can never cross-match on
 * a ring's host-side FIFO.
 *
 * The caller owns the returned fd (or -1) and must pair a successful
 * arm with npt_event_release_token once the wait is over -- tokens
 * are single-use: the host eventfd proxy is never drained, so a
 * signaled proxy can't be re-armed for a second wait.
 */
int
npt_event_arm_token_fd(struct npt_device *dev, uint64_t token)
{
   struct npt_event_state *st = dev->event;
   if (!st || !token)
      return -1;

   npt_dispatch_event_register(dev->renderer, token);

   uint32_t ring_idx = atomic_fetch_add(&st->next_event_ring_idx, 1);
   /* npt_event_init repairs a degenerate range, so the span is never zero --
    * npt_event_arm relies on the same invariant.  Asserting it keeps the
    * registration above paired with a release on every reachable exit. */
   const uint32_t span = st->ring_end - st->ring_base;
   assert(span);
   ring_idx = st->ring_base + (ring_idx - st->ring_base) % span;

   mtx_lock(&st->arm_mutex);
   bool arm_ok = npt_dispatch_event_arm_fence(npt_device_method_ring(dev),
                                              token, ring_idx);
   int sync_fd = arm_ok
      ? npt_renderer_submit_present_fence(dev->renderer, ring_idx) : -1;
   mtx_unlock(&st->arm_mutex);

   if (sync_fd < 0)
      npt_dispatch_event_release(dev->renderer, token);
   return sync_fd;
}

void
npt_event_release_token(struct npt_device *dev, uint64_t token)
{
   if (token)
      npt_dispatch_event_release(dev->renderer, token);
}
