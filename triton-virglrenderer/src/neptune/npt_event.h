/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Win32 event-HANDLE emulation.  An eventfd proxy is handed to the
 * host D3D library as the HANDLE.  When SetEvent fires on the proxy,
 * POLLIN signals on the fd and the sync-queue worker drives
 * retire_fence on the matching ARM_EVENT_FENCE arm.
 */

#ifndef NPT_EVENT_H
#define NPT_EVENT_H

#include "npt_common.h"

struct npt_context;

/* Linux uses eventfd (one fd); darwin uses a backend event handle (the
 * HANDLE the host library's SetEvent acts on, with the backend's
 * event_dup_fd providing the pollable side); other POSIX uses pipe2
 * (host writes the signal end, we poll the read end). */
struct npt_event_fd {
#if defined(__linux__)
   int fd;
#elif defined(__APPLE__)
   void *handle;
#else
   int read_fd;
   int write_fd;
#endif
};

struct npt_event_proxy {
   uint64_t token;               /* guest HANDLE = (uintptr_t)HANDLE */
   struct npt_event_fd proxy;
   /* +1 per REGISTER_EVENT, +1 per outstanding ARM.  Proxy stays
    * alive until both drop, so a late SetEvent after RELEASE_EVENT
    * doesn't hit EBADF and the in-flight dup_fd can still observe
    * the signal. */
   uint32_t refcount;
};

struct npt_event_pending_arm {
   uint32_t ring_idx;
   int      dup_fd;         /* pop transfers ownership to the sync queue */
   /* Keeps a refcount on the proxy until pop transfers dup_fd. */
   struct npt_event_proxy *proxy;
   struct list_head head;
};

bool npt_event_init(struct npt_context *ctx);
void npt_event_fini(struct npt_context *ctx);

/* Idempotent: re-register just bumps the refcount. */
void npt_event_register(struct npt_context *ctx, uint64_t token);

/* False if token isn't registered or dup/alloc fails. */
bool npt_event_arm(struct npt_context *ctx, uint64_t token,
                    uint32_t ring_idx);

void npt_event_release(struct npt_context *ctx, uint64_t token);

/* Returns the duped proxy fd (ownership transferred) or -1 on miss. */
int npt_event_pop_pending_arm(struct npt_context *ctx,
                               uint32_t ring_idx, uint64_t fence_id);

/* Returns the signal-end fd (cast to void *) or NULL on miss. */
void *npt_event_lookup(struct npt_context *ctx, uint64_t token);

struct npt_dispatch_context;
void *npt_event_replace_by_token(struct npt_dispatch_context *dispatch,
                                  npt_object_id id);

#endif /* NPT_EVENT_H */
