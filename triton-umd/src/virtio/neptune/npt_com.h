/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef NPT_COM_H
#define NPT_COM_H

#include "npt_common.h"
#include "npt_cs.h"
#include "npt_device.h"
#include "npt_object.h"
#include "npt_ring.h"
#include "npt_tls.h"

#include <stddef.h>

/* Also defined by neptune-protocol headers; guard against redefinition. */
#ifndef NPT_STDMETHODCALLTYPE
#  if defined(_WIN32)
#    define NPT_STDMETHODCALLTYPE __stdcall
#  else
#    define NPT_STDMETHODCALLTYPE
#  endif
#endif

#if defined(_WIN32)
#define NPT_WINAPI __stdcall
#if defined(_MSC_VER)
/* The .def file controls exports under MSVC; emitting __declspec(dllexport)
 * here would conflict with the SDK's __declspec(dllimport) declarations
 * of the same names (D3D11CreateDevice, CreateDXGIFactory, ...). */
#define NPT_DLLEXPORT
#else
#define NPT_DLLEXPORT __attribute__((dllexport))
#endif
#define NPT_API NPT_DLLEXPORT NPT_WINAPI
#else
#define NPT_WINAPI
#define NPT_DLLEXPORT __attribute__((visibility("default")))
#define NPT_API NPT_DLLEXPORT
#endif

/*
 * Every Neptune COM wrapper starts with `lpVtbl` followed by struct
 * npt_object.  `base.id` is the host COM pointer cast to uint64_t.
 */
struct npt_com_base {
   const void **lpVtbl;
   struct npt_object base;
   /* Free-form per-family side state: a malloc'd struct, a pool index
    * packed into a pointer, etc.  Populated when the IID's ctor-table
    * entry declares aux_size > 0 (npt_com_register_family). */
   void *aux;
   /* Owns aux's lifetime; called by npt_com_destroy. */
   void (*aux_destroy)(void *aux);
};

/* npt_object_get_id (npt_cs.h) reads host_id through struct npt_com_head;
 * pin the layout so a struct change trips here. */
_Static_assert(offsetof(struct npt_com_base, base) ==
                  offsetof(struct npt_com_head, id),
               "npt_com_base: base must overlay npt_com_head's id");
_Static_assert(offsetof(struct npt_object, id) == 0,
               "npt_object: id must be the first field");

/*
 * Synthetic host_ids for wrappers with no host counterpart (guest-side
 * IDXGIOutput, etc.).  x86_64 user pointers never have bit 63 set, so
 * this bit can't collide with a real host pointer.  npt_com_send_release
 * skips the wire send when this bit is on.
 */
#define NPT_GUEST_FAB_BIT (1ull << 63)
#define NPT_GUEST_KIND_OUTPUT 1u
#define NPT_GUEST_KIND_SWAPCHAIN 2u

static inline uint64_t
npt_com_make_guest_id(uint8_t kind, uint64_t low)
{
   return NPT_GUEST_FAB_BIT |
          ((uint64_t)kind << 56) |
          (low & 0x00ffffffffffffffull);
}

static inline bool
npt_com_id_is_guest_fab(uint64_t id)
{
   return (id & NPT_GUEST_FAB_BIT) != 0;
}

/* host_id = 0 for objects created locally before the host call. */
static inline void
npt_com_base_init(struct npt_com_base *com, const void **vtbl,
                  struct npt_device *dev, uint64_t host_id)
{
   com->lpVtbl = vtbl;
   npt_object_init(&com->base, host_id, dev);
   com->aux = NULL;
   com->aux_destroy = NULL;
}

static inline uint64_t
npt_com_self_id(void *self)
{
   return ((struct npt_com_base *)self)->base.id;
}

static inline struct npt_ring *
npt_com_self_ring(void *self)
{
   struct npt_com_base *com = self;
   if (com->base.instance_ring)
      return com->base.instance_ring;
   return npt_tls_get_ring(com->base.device);
}

static inline struct npt_device *
npt_com_self_device(void *self)
{
   return ((struct npt_com_base *)self)->base.device;
}

/*
 * Default IUnknown::AddRef: bump pub_ref; on 0->1, bump priv_ref and
 * AddRef the parent wrapper (parent coupling, see npt_object.h).
 * Returns the new public refcount.
 */
uint32_t NPT_STDMETHODCALLTYPE
npt_com_default_addref(void *self);

/*
 * Default IUnknown::Release: dec pub_ref; on 1->0, dec the matching
 * priv_ref pair (and parent's).  Final priv_ref==0 evicts from cache,
 * sends COM_RELEASE, and frees the wrapper.
 */
uint32_t NPT_STDMETHODCALLTYPE
npt_com_default_release(void *self);

/*
 * Tear down a wrapper bypassing refcount checks.  Runs the same
 * destruction sequence as a Release-driven destroy but does NOT touch
 * parent priv_refs, so it's safe to call from a flat foreach over the
 * wrapper cache without parent-first ordering.
 */
void
npt_com_force_destroy(struct npt_com_base *com);

/*
 * Satisfy QueryInterface without a host round-trip by matching `riid`
 * against a NULL-terminated chain of parent IIDs.  On match, bumps
 * refcount and writes *out = self.  E_NOINTERFACE on miss; caller
 * decides whether a host round-trip is worth attempting.
 */
HRESULT
npt_com_default_query_interface_chain(void *self, const GUID *riid,
                                      void **out, const GUID *const *chain);

/*
 * Cache lookup for `host_id` in dev's wrapper cache.  Hit returns the
 * cached wrapper with an extra public ref; miss runs the registered
 * ctor for `iid` and inserts the new wrapper.  `parent_wrapper`
 * enables parent coupling; NULL for top-level / unparented objects.
 * NULL return = OOM or no ctor registered.
 */
void *
npt_com_get_or_wrap(struct npt_device *dev, const GUID *iid,
                    uint64_t host_id, struct npt_com_base *parent_wrapper);

/*
 * Leak-safe variant: on wrapper-allocation failure, sends COM_RELEASE
 * for host_id so the host-side ref the CreateXxx call took doesn't
 * leak.  Treat NULL return as E_OUTOFMEMORY.
 */
void *
npt_com_get_or_wrap_or_release(struct npt_device *dev, const GUID *iid,
                               uint64_t host_id,
                               struct npt_com_base *parent_wrapper);

/*
 * Monotonic 64-bit guest-allocated object id.  Sent in command bodies
 * so the host can register {id -> host_ptr} without a reply round-trip.
 * Starts at 1; 0 stays reserved for null/invalid.
 */
uint64_t npt_com_allocate_next_id(void);

/*
 * Constructors return a wrapper with pub_ref==1, priv_ref==0; the
 * runtime sets parent and does the initial parent priv_ref bump.
 */
typedef void *(*npt_com_ctor_fn)(struct npt_device *dev, uint64_t object_id);

void
npt_com_register_default_ctor(const GUID *iid, npt_com_ctor_fn ctor);

/*
 * Aux state: runtime-allocated per-wrapper side data.  A family
 * declares aux_size and aux_init via npt_com_register_family; on
 * cache-miss the runtime calloc's aux_size bytes into com->aux and
 * calls aux_init, which fills aux and installs com->aux_destroy.
 * aux_destroy owns aux's lifetime (the runtime never free()s aux
 * itself, so aux can be any shape -- pool index, inline data, etc.).
 *
 * Vtbl overrides patch the generator's default vtbl in place via
 * NPT_REGISTER_OVERRIDE; no custom ctor is needed to swap the vtbl.
 */

typedef void (*npt_com_aux_init_fn)(struct npt_com_base *com,
                                    struct npt_device *dev,
                                    uint64_t host_id);

/*
 * Register a tiered family.  `tier_iids` is a NULL-terminated list
 * (e.g. {ID3D11Device, Device1, ..., Device5, NULL}); each wrapper
 * keeps its own tier's vtbl, so callers must QI to obtain a wider-
 * tier pointer.  Must run after the default-ctor populator.
 */
void
npt_com_register_family(const GUID *const *tier_iids,
                        size_t aux_size,
                        npt_com_aux_init_fn aux_init);

/* Typed-field assignment gives compile-time signature checking and
 * catches method-name typos. */
#define NPT_REGISTER_OVERRIDE(iface_lower, method_name, override_fn) \
    (npt_##iface_lower##_default_vtbl_storage.method_name = (override_fn))

/* Hard abort: a `skip_default` thunk was invoked without an override. */
void
npt_com_assert_overridden(const char *iid_name, const char *method_name);

/* Idempotent.  Must be called before any wrapper is constructed. */
void
npt_com_init(void);

bool
npt_com_send_release(struct npt_device *dev, uint64_t host_id);

/* Caller pre-allocates `guest_id` and builds the wrapper before
 * calling.  Success registers the queried host pointer under
 * guest_id; on failure the wrapper must be discarded. */
HRESULT
npt_com_send_query_interface(struct npt_device *dev, uint64_t src_id,
                             const GUID *iid, uint64_t guest_id);

#endif /* NPT_COM_H */
