/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * IDXGIAdapter{,1..4} overrides.  Each tier's wrapper carries its own
 * vtbl since QI goes through npt_com_get_or_wrap with the wider IID.
 * EnumOutputs is overridden to return guest-fabricated IDXGIOutput
 * wrappers, one per local DRM connector (or one synthetic fallback when
 * no DRM is available).  All other adapter methods pass through to the
 * host -- the host DXGI is itself DXVK, which already supplies the
 * vendor-keyed UMD versions etc.
 */

#include "npt_com.h"
#include "npt_display.h"
#include "npt_env.h"
#include "npt_overrides.h"
#include "npt_device.h"

#include "neptune-protocol/npt_protocol_client_idxgiadapter.h"
#include "neptune-protocol/npt_protocol_client_idxgioutput.h"
#include "neptune-protocol/npt_protocol_defs.h"

#if defined(__WINE__)
#include "nptunix/npt_unixlib.h"
#endif

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef TRUE
#define TRUE  ((BOOL)1)
#endif
#ifndef FALSE
#define FALSE ((BOOL)0)
#endif

struct npt_adapter_aux {
   mtx_t lock;
   bool populated;
   struct npt_output_list outputs;
};

static void
adp_aux_destroy(void *p)
{
   struct npt_adapter_aux *aux = p;
   if (!aux)
      return;
   mtx_destroy(&aux->lock);
   free(aux);
}

static void
adp_aux_init(struct npt_com_base *com, struct npt_device *dev, uint64_t host_id)
{
   (void)dev; (void)host_id;
   struct npt_adapter_aux *aux = com->aux;
   mtx_init(&aux->lock, mtx_plain);
   com->aux_destroy = adp_aux_destroy;
}

static void
fill_synthetic_output(struct npt_output_info *o)
{
   memset(o, 0, sizeof(*o));
   snprintf(o->name, sizeof(o->name), "Synthetic-1");
   o->current_width  = 1920;
   o->current_height = 1080;
   o->num_modes      = 1;
   o->modes[0].width        = 1920;
   o->modes[0].height       = 1080;
   o->modes[0].refresh_num  = 60;
   o->modes[0].refresh_den  = 1;
   o->modes[0].bpp          = 32;
   o->modes[0].preferred    = true;
}

static void
fetch_output_list(struct npt_output_list *list)
{
   memset(list, 0, sizeof(*list));
#if defined(__WINE__)
   if (npt_unixlib_ensure_init() == 0) {
      struct npt_unix_enumerate_outputs_params p;
      memset(&p, 0, sizeof(p));
      p.expose_all_modes = NPT_DEBUG(EXPOSE_ALL_MODES) ? 1 : 0;
      npt_wine_unix_call(npt_unix_enumerate_outputs, &p);
      *list = p.list;
   }
#endif
   if (list->num_outputs == 0) {
      /* DXVK dmabuf-backend behavior: at least one synthetic output so
       * D3D mode-enumeration paths see something. */
      fill_synthetic_output(&list->outputs[0]);
      list->num_outputs = 1;
   }
}

static bool
adp_ensure_outputs(struct npt_adapter_aux *aux)
{
   mtx_lock(&aux->lock);
   if (!aux->populated) {
      fetch_output_list(&aux->outputs);
      aux->populated = true;
   }
   bool ok = aux->outputs.num_outputs > 0;
   mtx_unlock(&aux->lock);
   return ok;
}

static HRESULT NPT_STDMETHODCALLTYPE
adp_EnumOutputs_override(void *self, UINT Output, IDXGIOutput **ppOutput)
{
   if (!ppOutput)
      return NPT_E_POINTER;
   *ppOutput = NULL;

   struct npt_com_base *adp = self;
   struct npt_adapter_aux *aux = adp->aux;
   if (!aux || !adp_ensure_outputs(aux))
      return NPT_DXGI_ERROR_NOT_FOUND;
   if (Output >= aux->outputs.num_outputs)
      return NPT_DXGI_ERROR_NOT_FOUND;

   /* Synthetic id is unique per (parent adapter, connector index). */
   uint64_t out_id = npt_com_make_guest_id(
      NPT_GUEST_KIND_OUTPUT,
      (adp->base.id << 8) ^ Output);

   struct npt_device *dev = npt_com_self_device(self);
   void *wrapper = npt_com_get_or_wrap(dev, &NPT_IID_IDXGIOutput6,
                                       out_id, adp);
   if (!wrapper)
      return NPT_E_OUTOFMEMORY;

   npt_output_aux_populate(wrapper, adp, Output,
                           &aux->outputs.outputs[Output]);
   *ppOutput = (IDXGIOutput *)wrapper;
   return NPT_S_OK;
}

static HRESULT NPT_STDMETHODCALLTYPE
adp3_RegisterHardwareContentProtectionTeardownStatusEvent_override(
   void *self, HANDLE hEvent, DWORD *pdwCookie)
{
   (void)self; (void)hEvent;
   if (pdwCookie) *pdwCookie = 0;
   return NPT_E_NOTIMPL;
}

static void NPT_STDMETHODCALLTYPE
adp3_UnregisterHardwareContentProtectionTeardownStatus_override(
   void *self, DWORD dwCookie)
{
   (void)self; (void)dwCookie;
}

#define NPT_REGISTER_OVERRIDE_DXGI_ADAPTER4(m, f) \
   NPT_REGISTER_OVERRIDE(idxgiadapter4, m, f)
#define NPT_REGISTER_OVERRIDE_DXGI_ADAPTER3(m, f) \
   NPT_REGISTER_OVERRIDE(idxgiadapter3, m, f); \
   NPT_REGISTER_OVERRIDE_DXGI_ADAPTER4(m, f)
#define NPT_REGISTER_OVERRIDE_DXGI_ADAPTER2(m, f) \
   NPT_REGISTER_OVERRIDE(idxgiadapter2, m, f); \
   NPT_REGISTER_OVERRIDE_DXGI_ADAPTER3(m, f)
#define NPT_REGISTER_OVERRIDE_DXGI_ADAPTER1(m, f) \
   NPT_REGISTER_OVERRIDE(idxgiadapter1, m, f); \
   NPT_REGISTER_OVERRIDE_DXGI_ADAPTER2(m, f)
#define NPT_REGISTER_OVERRIDE_DXGI_ADAPTER(m, f) \
   NPT_REGISTER_OVERRIDE(idxgiadapter, m, f); \
   NPT_REGISTER_OVERRIDE_DXGI_ADAPTER1(m, f)

static const GUID *const adapter_tiers[] = {
   &NPT_IID_IDXGIAdapter,  &NPT_IID_IDXGIAdapter1, &NPT_IID_IDXGIAdapter2,
   &NPT_IID_IDXGIAdapter3, &NPT_IID_IDXGIAdapter4, NULL,
};

void
npt_overrides_dxgi_adapter_init(void)
{
   NPT_REGISTER_OVERRIDE_DXGI_ADAPTER(
      EnumOutputs, adp_EnumOutputs_override);

   NPT_REGISTER_OVERRIDE_DXGI_ADAPTER3(
      RegisterHardwareContentProtectionTeardownStatusEvent,
      adp3_RegisterHardwareContentProtectionTeardownStatusEvent_override);
   NPT_REGISTER_OVERRIDE_DXGI_ADAPTER3(
      UnregisterHardwareContentProtectionTeardownStatus,
      adp3_UnregisterHardwareContentProtectionTeardownStatus_override);

   npt_com_register_family(adapter_tiers,
                           sizeof(struct npt_adapter_aux),
                           adp_aux_init);
}
