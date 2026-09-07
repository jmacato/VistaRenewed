/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 */

#include "npt_event.h"
#include "npt_context.h"
#include "npt_library.h"
#include "npt_renderer.h"

#include "util/hash_table.h"
#include "util/list.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/eventfd.h>
#endif

static int
event_fd_create(struct npt_event_fd *out)
{
#if defined(__linux__)
   out->fd = eventfd(0, EFD_CLOEXEC);
   return out->fd < 0 ? -1 : 0;
#elif defined(__APPLE__)
   struct npt_d3d_library *lib = npt_renderer_get_library();
   if (!lib || !lib->pfn_event_create) {
      npt_log("event: backend event API unavailable");
      return -1;
   }
   /* Manual-reset matches Win32's default for guest-created HANDLEs
    * (we don't auto-clear on read). */
   out->handle = lib->pfn_event_create(/*manual_reset=*/1, /*initial_state=*/0);
   return out->handle ? 0 : -1;
#else
   /* Prefer pipe2(O_CLOEXEC) where available (Linux, FreeBSD) to
    * avoid the fd-leak window between pipe() and fcntl().  Fall back
    * to pipe() + fcntl() on platforms without pipe2. */
   int fds[2];
#ifdef HAVE_PIPE2
   if (pipe2(fds, O_CLOEXEC) == 0) {
      out->read_fd = fds[0];
      out->write_fd = fds[1];
      return 0;
   }
   if (errno != ENOSYS)
      return -1;
#endif
   if (pipe(fds) != 0)
      return -1;
   /* Best-effort CLOEXEC: non-fatal, fds remain usable on failure. */
   for (int i = 0; i < 2; i++) {
      int flags = fcntl(fds[i], F_GETFD);
      if (flags >= 0)
         fcntl(fds[i], F_SETFD, flags | FD_CLOEXEC);
   }
   out->read_fd = fds[0];
   out->write_fd = fds[1];
   return 0;
#endif
}

static void
event_fd_destroy(struct npt_event_fd *fd)
{
#if defined(__linux__)
   if (fd->fd >= 0) {
      close(fd->fd);
      fd->fd = -1;
   }
#elif defined(__APPLE__)
   if (fd->handle) {
      struct npt_d3d_library *lib = npt_renderer_get_library();
      if (lib && lib->pfn_event_close)
         lib->pfn_event_close(fd->handle);
      fd->handle = NULL;
   }
#else
   if (fd->read_fd >= 0) {
      close(fd->read_fd);
      fd->read_fd = -1;
   }
   if (fd->write_fd >= 0) {
      close(fd->write_fd);
      fd->write_fd = -1;
   }
#endif
}

/* The value handed to the host as the HANDLE; SetEvent acts on it.
 * An fd cast to a pointer on eventfd/pipe platforms, the d3dmetal-
 * native event handle on darwin. */
static void *
event_fd_signal_handle(const struct npt_event_fd *fd)
{
#if defined(__linux__)
   return fd->fd >= 0 ? (void *)(uintptr_t)fd->fd : NULL;
#elif defined(__APPLE__)
   return fd->handle;
#else
   return fd->write_fd >= 0 ? (void *)(uintptr_t)fd->write_fd : NULL;
#endif
}

/* Caller polls + closes the returned dup. */
static int
event_fd_dup_wait_fd(const struct npt_event_fd *fd)
{
#if defined(__linux__)
   return dup(fd->fd);
#elif defined(__APPLE__)
   struct npt_d3d_library *lib = npt_renderer_get_library();
   return (lib && lib->pfn_event_dup_fd) ? lib->pfn_event_dup_fd(fd->handle)
                                         : -1;
#else
   return dup(fd->read_fd);
#endif
}

static void
event_fd_init_invalid(struct npt_event_fd *fd)
{
#if defined(__linux__)
   fd->fd = -1;
#elif defined(__APPLE__)
   fd->handle = NULL;
#else
   fd->read_fd = -1;
   fd->write_fd = -1;
#endif
}

static uint32_t
tok_hash(const void *key)
{
   uint64_t t = *(const uint64_t *)key;
   t ^= t >> 33; t *= 0xff51afd7ed558ccdULL;
   t ^= t >> 33; t *= 0xc4ceb9fe1a85ec53ULL;
   t ^= t >> 33;
   return (uint32_t)t;
}

static bool
tok_equal(const void *a, const void *b)
{
   return *(const uint64_t *)a == *(const uint64_t *)b;
}

bool
npt_event_init(struct npt_context *ctx)
{
   if (mtx_init(&ctx->event_mutex, mtx_plain) != thrd_success)
      return false;

   ctx->event_proxies = _mesa_hash_table_create(NULL, tok_hash, tok_equal);
   if (!ctx->event_proxies) {
      mtx_destroy(&ctx->event_mutex);
      return false;
   }

   list_inithead(&ctx->event_pending_arms);
   return true;
}

void
npt_event_fini(struct npt_context *ctx)
{
   if (!ctx->event_proxies)
      return;

   mtx_lock(&ctx->event_mutex);

   /* Guests should send RELEASE_EVENT for every REGISTER, leaving
    * the table empty by teardown.  Log non-zero residue so the leak
    * is visible. */
   uint32_t leaked = _mesa_hash_table_num_entries(ctx->event_proxies);
   if (leaked)
      npt_log("event_fini: %u proxies leaked at context teardown", leaked);

   list_for_each_entry_safe(struct npt_event_pending_arm, p,
                            &ctx->event_pending_arms, head) {
      if (p->dup_fd >= 0)
         close(p->dup_fd);
      list_del(&p->head);
      free(p);
   }

   hash_table_foreach(ctx->event_proxies, entry) {
      struct npt_event_proxy *pr = entry->data;
      event_fd_destroy(&pr->proxy);
      free(pr);
   }
   _mesa_hash_table_destroy(ctx->event_proxies, NULL);
   ctx->event_proxies = NULL;

   mtx_unlock(&ctx->event_mutex);
   mtx_destroy(&ctx->event_mutex);
}

static struct npt_event_proxy *
lookup_locked(struct npt_context *ctx, uint64_t token)
{
   struct hash_entry *e =
      _mesa_hash_table_search_pre_hashed(ctx->event_proxies,
                                         tok_hash(&token), &token);
   return e ? e->data : NULL;
}

static void
npt_event_proxy_unref_locked(struct npt_context *ctx,
                             struct npt_event_proxy *pr,
                             struct hash_entry *e);

/* Create + insert a proxy at `initial_refcount`.  Caller holds event_mutex.
 * REGISTER_EVENT passes 1 (the registration reference).  A lazy create from
 * npt_event_arm passes 0 so ONLY the arm's reference holds the proxy: ARM and
 * its REGISTER_EVENT travel on different host channels (a per-event ring vs
 * the renderer command stream) and can be decoded out of order.  If ARM lands
 * first and took a registration reference too, the later REGISTER_EVENT would
 * add a second one that the guest's single RELEASE_EVENT never balances —
 * leaking the proxy's kqueue+pipe fds.  With 0 here, REGISTER always
 * contributes exactly one registration reference regardless of arrival order. */
static struct npt_event_proxy *
event_proxy_create_locked(struct npt_context *ctx, uint64_t token,
                          uint32_t initial_refcount)
{
   struct npt_event_fd hfd;
   event_fd_init_invalid(&hfd);
   if (event_fd_create(&hfd) < 0) {
      npt_log("event: fd create failed: %s", strerror(errno));
      return NULL;
   }
   struct npt_event_proxy *pr = calloc(1, sizeof(*pr));
   if (!pr) {
      event_fd_destroy(&hfd);
      return NULL;
   }
   pr->token    = token;
   pr->proxy    = hfd;
   pr->refcount = initial_refcount;
   /* Key points into pr->token so the hash key stays valid for pr's
    * lifetime. */
   _mesa_hash_table_insert_pre_hashed(ctx->event_proxies,
                                      tok_hash(&pr->token),
                                      &pr->token, pr);
   return pr;
}

void
npt_event_register(struct npt_context *ctx, uint64_t token)
{
   if (!token)
      return;

   mtx_lock(&ctx->event_mutex);
   struct npt_event_proxy *pr = lookup_locked(ctx, token);
   if (pr) {
      pr->refcount++;
      mtx_unlock(&ctx->event_mutex);
      return;
   }
   event_proxy_create_locked(ctx, token, /*initial_refcount=*/1);
   mtx_unlock(&ctx->event_mutex);
}

bool
npt_event_arm(struct npt_context *ctx, uint64_t token, uint32_t ring_idx)
{
   if (!token)
      return false;

   mtx_lock(&ctx->event_mutex);
   struct npt_event_proxy *pr = lookup_locked(ctx, token);
   if (!pr) {
      /* Lazy-create for an ARM that beat its REGISTER_EVENT, WITHOUT a
       * registration reference — the arm reference taken below holds it. */
      pr = event_proxy_create_locked(ctx, token, /*initial_refcount=*/0);
      if (!pr) {
         mtx_unlock(&ctx->event_mutex);
         return false;
      }
   }
   /* Take the arm reference now so the fd stays alive even if the guest sends
    * RELEASE_EVENT before the fence completes, and so the failure paths below
    * can undo it (freeing a just-lazy-created proxy) via the unref helper. */
   pr->refcount++;

   int dup_fd = event_fd_dup_wait_fd(&pr->proxy);
   if (dup_fd < 0) {
      npt_log("event: dup(proxy wait fd) failed: %s", strerror(errno));
      npt_event_proxy_unref_locked(ctx, pr, NULL);
      mtx_unlock(&ctx->event_mutex);
      return false;
   }

   struct npt_event_pending_arm *p = calloc(1, sizeof(*p));
   if (!p) {
      close(dup_fd);
      npt_event_proxy_unref_locked(ctx, pr, NULL);
      mtx_unlock(&ctx->event_mutex);
      return false;
   }
   p->ring_idx = ring_idx;
   p->dup_fd   = dup_fd;
   p->proxy    = pr;
   list_addtail(&p->head, &ctx->event_pending_arms);

   mtx_unlock(&ctx->event_mutex);
   return true;
}

/* Caller holds ctx->event_mutex.  Passing `e` from an existing
 * lookup avoids a re-search; NULL forces a fresh lookup. */
static void
npt_event_proxy_unref_locked(struct npt_context *ctx,
                              struct npt_event_proxy *pr,
                              struct hash_entry *e)
{
   if (--pr->refcount > 0)
      return;

   event_fd_destroy(&pr->proxy);

   if (!e) {
      e = _mesa_hash_table_search_pre_hashed(ctx->event_proxies,
                                             tok_hash(&pr->token),
                                             &pr->token);
   }
   if (e)
      _mesa_hash_table_remove(ctx->event_proxies, e);
   free(pr);
}

void
npt_event_release(struct npt_context *ctx, uint64_t token)
{
   if (!token)
      return;

   mtx_lock(&ctx->event_mutex);
   struct hash_entry *e =
      _mesa_hash_table_search_pre_hashed(ctx->event_proxies,
                                         tok_hash(&token), &token);
   if (!e) {
      mtx_unlock(&ctx->event_mutex);
      return;
   }
   npt_event_proxy_unref_locked(ctx, e->data, e);
   mtx_unlock(&ctx->event_mutex);
}

int
npt_event_pop_pending_arm(struct npt_context *ctx,
                           uint32_t ring_idx, uint64_t fence_id)
{
   /* Match on ring_idx only.  fence_id is virgl-allocated when the
    * matching command submission arrives — strictly after the guest
    * sent ARM_EVENT_FENCE — so the guest can't include it.  Per-ring
    * FIFO pairing works because the guest serialises ARM and submit
    * on one ring. */
   (void)fence_id;
   int fd = -1;
   mtx_lock(&ctx->event_mutex);
   list_for_each_entry_safe(struct npt_event_pending_arm, p,
                            &ctx->event_pending_arms, head) {
      if (p->ring_idx == ring_idx) {
         fd = p->dup_fd;
         list_del(&p->head);
         /* dup_fd survives the proxy free: the read end remains
          * pollable; writes to the closed write end stop, with the
          * sync queue's poll timeout as fallback. */
         if (p->proxy)
            npt_event_proxy_unref_locked(ctx, p->proxy, NULL);
         free(p);
         break;
      }
   }
   mtx_unlock(&ctx->event_mutex);
   return fd;
}

void *
npt_event_lookup(struct npt_context *ctx, uint64_t token)
{
   if (!token || !ctx || !ctx->event_proxies)
      return NULL;

   mtx_lock(&ctx->event_mutex);
   struct npt_event_proxy *pr = lookup_locked(ctx, token);
   void *ret = pr ? event_fd_signal_handle(&pr->proxy) : NULL;
   mtx_unlock(&ctx->event_mutex);
   return ret;
}

/* Out-of-line so npt_cs.h doesn't need npt_context.h (header cycle). */
void *
npt_event_replace_by_token(struct npt_dispatch_context *dispatch,
                            npt_object_id id)
{
   if (!id || !dispatch)
      return NULL;
   return npt_event_lookup(npt_context_from_dispatch(dispatch),
                            (uint64_t)id);
}
