/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * COM/IUnknown plumbing for direct host-library calls: vtable
 * extraction and typed wrappers around AddRef / Release /
 * QueryInterface.  Used by code paths that bypass the generated
 * dispatcher (swapchain virtualization, transport-layer COM ops,
 * override helpers).  The generator's dispatch code also depends on
 * npt_com_vtable and NPT_COM_VTBL_FUNC; this header is pulled in
 * indirectly from npt_cs.h.
 */

#ifndef NPT_COM_H
#define NPT_COM_H

#include <stdint.h>

/* HRESULT, GUID. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include "neptune-protocol/npt_protocol_directx_types.h"
#pragma GCC diagnostic pop

/* IUnknown's three methods occupy vtable slots 0..2; derived
 * interfaces append at slot 3+. */
#define NPT_IUNKNOWN_VTBL_QUERY_INTERFACE 0
#define NPT_IUNKNOWN_VTBL_ADDREF          1
#define NPT_IUNKNOWN_VTBL_RELEASE         2

static inline void **
npt_com_vtable(void *obj)
{
   return *(void ***)obj;
}

/* Union pun avoids the -Wpedantic warning for ISO C's prohibition on
 * direct object-to-function pointer casts. */
#define NPT_COM_VTBL_FUNC(type, vtable, idx)   \
   (((union { void *p; type f; }){ .p = (vtable)[(idx)] }).f)

typedef HRESULT (NPT_STDMETHODCALLTYPE *PFN_IUnknown_QueryInterface)(void *self, const GUID *riid, void **ppv);
typedef uint32_t (NPT_STDMETHODCALLTYPE *PFN_IUnknown_AddRef)(void *self);
typedef uint32_t (NPT_STDMETHODCALLTYPE *PFN_IUnknown_Release)(void *self);

static inline HRESULT
npt_com_query_interface(void *obj, const GUID *riid, void **out)
{
   PFN_IUnknown_QueryInterface fn =
      NPT_COM_VTBL_FUNC(PFN_IUnknown_QueryInterface,
                        npt_com_vtable(obj),
                        NPT_IUNKNOWN_VTBL_QUERY_INTERFACE);
   return fn(obj, riid, out);
}

static inline uint32_t
npt_com_add_ref(void *obj)
{
   PFN_IUnknown_AddRef fn =
      NPT_COM_VTBL_FUNC(PFN_IUnknown_AddRef,
                        npt_com_vtable(obj),
                        NPT_IUNKNOWN_VTBL_ADDREF);
   return fn(obj);
}

static inline uint32_t
npt_com_release(void *obj)
{
   PFN_IUnknown_Release fn =
      NPT_COM_VTBL_FUNC(PFN_IUnknown_Release,
                        npt_com_vtable(obj),
                        NPT_IUNKNOWN_VTBL_RELEASE);
   return fn(obj);
}

#endif /* NPT_COM_H */
