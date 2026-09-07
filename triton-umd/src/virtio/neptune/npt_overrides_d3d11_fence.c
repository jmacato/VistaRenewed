/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * ID3D11Fence overrides:
 *  - SetEventOnCompletion pre-arms the host eventfd proxy.
 *  - GetCompletedValue reads the per-fence feedback slot.
 *  - Device5::CreateFence defers to the default thunk, then
 *    seeds + registers the feedback slot.
 */

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "npt_com.h"
#include "npt_overrides_d3d11_feedback.h"
#include "npt_device.h"
#include "npt_dispatch.h"
#include "npt_env.h"
#include "npt_event.h"
#include "npt_overrides.h"
#include "npt_ring.h"

#include "neptune-protocol/npt_protocol_client_id3d11device.h"
#include "neptune-protocol/npt_protocol_client_id3d11fence.h"
#include "neptune-protocol/npt_protocol_defs.h"
#include "neptune-protocol/npt_protocol_guest_id3d11fence.h"

#define NPT_REGISTER_OVERRIDE_D3D11_FENCE(m, f) \
   NPT_REGISTER_OVERRIDE(id3d11fence, m, f)

#define NPT_REGISTER_OVERRIDE_D3D11_DEVICE5(m, f) \
   NPT_REGISTER_OVERRIDE(id3d11device5, m, f)

static const GUID *const fence_tiers[] = { &NPT_IID_ID3D11Fence, NULL };

/* Slot is a single atomic completed_value -- no type-specific fields. */
struct npt_d3d11_fence_aux {
   struct npt_d3d11_feedback_aux base;
};

static void npt_d3d11_fence_aux_destroy(void *aux_raw);

static void
npt_d3d11_fence_aux_init(struct npt_com_base *com,
                         struct npt_device *dev, uint64_t host_id)
{
   struct npt_d3d11_fence_aux *aux = com->aux;
   aux->base.com = com;
   aux->base.fb_shmem = NULL;
   aux->base.fb_offset = 0;
   aux->base.registered = false;
   com->aux_destroy = npt_d3d11_fence_aux_destroy;
   /* Slot alloc + REGISTER happen post-Create (aux_init has no
    * InitialValue).  QI-routed wrappers stay unregistered and fall
    * through to the sync GetCompletedValue. */
   (void)dev; (void)host_id;
}

static void
npt_d3d11_fence_aux_destroy(void *aux_raw)
{
   struct npt_d3d11_fence_aux *aux = aux_raw;
   /* Host-side unregister piggybacks on COM_RELEASE; no separate
    * UNREGISTER_FENCE_FEEDBACK opcode needed. */
   if (aux->base.fb_shmem && aux->base.com && aux->base.com->base.device) {
      npt_renderer_shmem_unref(aux->base.com->base.device->renderer,
                               aux->base.fb_shmem);
   }
   free(aux);
}

/* Default-on; NPT_PERF=no_fence_feedback is the regression killswitch. */
static void
npt_d3d11_fence_finalize_create(struct npt_device *dev, void *wrapper,
                                UINT64 initial_value)
{
   if (NPT_PERF(NO_FENCE_FEEDBACK))
      return;
   if (!dev || !wrapper)
      return;
   struct npt_com_base *com = wrapper;
   struct npt_d3d11_fence_aux *aux = com->aux;
   if (!aux || aux->base.registered)
      return;

   uint32_t fb_offset = 0;
   bool fresh = false;
   struct npt_renderer_shmem *shmem =
      npt_device_alloc_feedback_slot(dev, NPT_FENCE_FEEDBACK_SLOT_SIZE,
                                     &fb_offset, &fresh);
   if (!shmem)
      return;

   /* Seed with InitialValue: host poll only updates the slot when
    * GetCompletedValue advances, so pre-Signal reads would otherwise
    * see memset-zero.  Guest-exclusive until REGISTER, plain write OK. */
   struct npt_d3d11_fence_feedback_slot *slot =
      (struct npt_d3d11_fence_feedback_slot *)
      ((uint8_t *)shmem->mmap_ptr + fb_offset);
   atomic_store_explicit(&slot->completed_value, initial_value,
                         memory_order_relaxed);

   aux->base.fb_shmem = shmem;
   aux->base.fb_offset = fb_offset;

   /* Fresh shmem requires a roundtrip so REGISTER_FENCE_FEEDBACK's
    * res_id has been registered when the host ring thread reads it. */
   if (fresh && !npt_ring_force_roundtrip(dev->ring)) {
      npt_renderer_shmem_unref(dev->renderer, shmem);
      aux->base.fb_shmem = NULL;
      aux->base.fb_offset = 0;
      return;
   }

   if (npt_dispatch_feedback_register_fence(dev->ring, com->base.id,
                                            shmem->res_id, fb_offset))
      aux->base.registered = true;
}

/* NULL on QI-routed wrappers that bypassed Device5::CreateFence. */
static struct npt_d3d11_fence_aux *
fence_aux(void *self)
{
   struct npt_com_base *com = self;
   if (!com || com->aux_destroy != npt_d3d11_fence_aux_destroy)
      return NULL;
   return com->aux;
}

static struct npt_d3d11_fence_feedback_slot *
fence_slot(struct npt_d3d11_fence_aux *aux)
{
   if (!aux || !aux->base.fb_shmem)
      return NULL;
   return (struct npt_d3d11_fence_feedback_slot *)
      ((uint8_t *)aux->base.fb_shmem->mmap_ptr + aux->base.fb_offset);
}

static UINT64 NPT_STDMETHODCALLTYPE
fence_GetCompletedValue_override(void *self)
{
   struct npt_d3d11_fence_aux *aux = fence_aux(self);
   struct npt_d3d11_fence_feedback_slot *slot = fence_slot(aux);
   if (!slot || !aux->base.registered) {
      /* Env opt-out, pool OOM, or QI-routed wrapper. */
      return npt_id3d11fence_default_GetCompletedValue(self);
   }
   /* Acquire pairs with the host's release-CAS.  Monotonic fences
    * mean any observed value is a valid lower bound. */
   return atomic_load_explicit(&slot->completed_value,
                               memory_order_acquire);
}

static HRESULT NPT_STDMETHODCALLTYPE
fence_SetEventOnCompletion_override(void *self, UINT64 Value, HANDLE hEvent)
{
   struct npt_device *dev = npt_com_self_device(self);

   /* Arm before the D3D call so the host's npt_win32_handle_replace
    * sees the proxy when it decodes hEvent. */
   if (hEvent && !npt_event_arm(dev, (void *)hEvent))
      return NPT_E_FAIL;

   /* Async dispatch, not npt_call_: a synchronous round-trip's reply is
    * serialised behind the submitted draw stream, so under heavy submission
    * it blocks for the whole backlog.  The host-side return value does not
    * matter -- the event-ring gate that actually signals the event was
    * already armed by npt_event_arm() above (the KMD signals it at GPU
    * completion) -- so this COM registration only needs to be ORDERED after
    * ARM_EVENT_FENCE on the same method ring, which async preserves, not
    * awaited. */
   npt_async_ID3D11Fence_SetEventOnCompletion(
      npt_device_method_ring(dev), npt_com_self_id(self), Value, hEvent);
   return S_OK;
}

static HRESULT NPT_STDMETHODCALLTYPE
dev5_CreateFence_override(void *self, UINT64 InitialValue,
                          D3D11_FENCE_FLAG Flags, REFIID ReturnedInterface,
                          void **ppFence)
{
   HRESULT hr = npt_id3d11device5_default_CreateFence(
      self, InitialValue, Flags, ReturnedInterface, ppFence);
   if (NPT_SUCCEEDED(hr) && ppFence && *ppFence) {
      npt_d3d11_fence_finalize_create(npt_com_self_device(self),
                                      *ppFence, InitialValue);
   }
   return hr;
}

void
npt_overrides_d3d11_fence_init(void)
{
   NPT_REGISTER_OVERRIDE_D3D11_FENCE(SetEventOnCompletion,
                                     fence_SetEventOnCompletion_override);
   NPT_REGISTER_OVERRIDE_D3D11_FENCE(GetCompletedValue,
                                     fence_GetCompletedValue_override);
   NPT_REGISTER_OVERRIDE_D3D11_DEVICE5(CreateFence,
                                       dev5_CreateFence_override);
   npt_com_register_family(fence_tiers,
                           sizeof(struct npt_d3d11_fence_aux),
                           npt_d3d11_fence_aux_init);
}
