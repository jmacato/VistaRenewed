/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef NPT_OBJECT_H
#define NPT_OBJECT_H

#include "npt_common.h"

struct npt_device;
struct npt_com_base;
struct npt_ring;

/*
 * Base bookkeeping embedded in every guest COM wrapper (via npt_com_base,
 * which prepends lpVtbl for COM ABI).  `id` is the host COM pointer cast
 * to uint64_t; the guest sends it as the wire object_id and the host
 * recovers the original pointer via npt_object_from_id.
 *
 * Dual refcount: pub_ref is what AddRef/Release expose to the app;
 * priv_ref is the runtime's internal hold (in-flight RPCs, parent
 * coupling).  Destruction only happens when both reach zero.  A child
 * wrapper holds a priv_ref on its parent for the duration of its own
 * pub_ref>0 lifetime, mirroring the D3D11DeviceChild contract.
 */
struct npt_object {
   uint64_t id;
   _Atomic uint32_t pub_ref;
   _Atomic uint32_t priv_ref;
   struct npt_device *device;
   struct npt_com_base *parent;
   /* Non-NULL for single-threaded-per-instance interfaces (DC*, SC*)
    * to keep host dispatch serial across guest threads; NULL for
    * thread-safe-by-contract interfaces, which fall through to the
    * caller's TLS ring. */
   struct npt_ring *instance_ring;
   /* Number of npt_device_acquire refs inherited by this wrapper.
    * Each top-level entry point bumps this on the returned wrapper
    * (whether freshly created or cache-hit) and npt_com_destroy
    * issues exactly this many releases. */
   _Atomic uint32_t device_ref_holds;
};

static inline void
npt_object_init(struct npt_object *obj, uint64_t host_id,
                struct npt_device *device)
{
   obj->id = host_id;
   /* Release publishes a fully-initialised refcount to a concurrent
    * cache-lookup observer; ARM64 needs the explicit barrier. */
   atomic_store_explicit(&obj->pub_ref, 1, memory_order_release);
   atomic_store_explicit(&obj->priv_ref, 0, memory_order_relaxed);
   obj->device = device;
   obj->parent = NULL;
   obj->instance_ring = NULL;
   atomic_store_explicit(&obj->device_ref_holds, 0, memory_order_relaxed);
}

#endif /* NPT_OBJECT_H */
