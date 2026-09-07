/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * DXGI.dll entry points: forward to the generated host call and wrap
 * the returned host pointer.
 */

#include "npt_com.h"
#include "npt_device.h"

#include "neptune-protocol/npt_protocol_guest_toplevel.h"
#include "neptune-protocol/npt_protocol_defs.h"

/* Failure paths release both the singleton acquire and the host ref
 * on `raw`, so the use-count stays balanced. */
static HRESULT
finish_create_dxgi_factory(struct npt_device *dev, const GUID *iid,
                           void *raw, HRESULT call_hr, void **ppFactory)
{
   if (NPT_FAILED(call_hr) || !raw) {
      if (raw)
         npt_com_send_release(dev, (uint64_t)(uintptr_t)raw);
      npt_device_release(dev);
      return NPT_FAILED(call_hr) ? call_hr : NPT_E_FAIL;
   }
   void *wrapper = npt_com_get_or_wrap_or_release(
      dev, iid, (uint64_t)(uintptr_t)raw, NULL);
   if (!wrapper) {
      npt_device_release(dev);
      return NPT_E_OUTOFMEMORY;
   }
   atomic_fetch_add_explicit(
      &((struct npt_com_base *)wrapper)->base.device_ref_holds,
      1, memory_order_relaxed);
   *ppFactory = wrapper;
   return NPT_S_OK;
}

HRESULT NPT_API
CreateDXGIFactory(REFIID riid, void **ppFactory)
{
   if (!ppFactory) return NPT_E_INVALIDARG;
   *ppFactory = NULL;

   npt_com_init();
   /* Top-level: device_ref_holds carries the acquire onto the wrapper. */
   struct npt_device *dev = npt_device_acquire();
   if (!dev) return NPT_E_FAIL;

   void *raw = NULL;
   HRESULT hr = npt_call_CreateDXGIFactory(dev->ring, riid, &raw);
   /* Wrap as base IDXGIFactory regardless of riid; QI handles tiers. */
   return finish_create_dxgi_factory(dev, &NPT_IID_IDXGIFactory, raw, hr,
                                     ppFactory);
}

HRESULT NPT_API
CreateDXGIFactory1(REFIID riid, void **ppFactory)
{
   if (!ppFactory) return NPT_E_INVALIDARG;
   *ppFactory = NULL;

   npt_com_init();
   struct npt_device *dev = npt_device_acquire();
   if (!dev) return NPT_E_FAIL;

   void *raw = NULL;
   HRESULT hr = npt_call_CreateDXGIFactory1(dev->ring, riid, &raw);
   return finish_create_dxgi_factory(dev, &NPT_IID_IDXGIFactory1, raw, hr,
                                     ppFactory);
}

HRESULT NPT_API
CreateDXGIFactory2(UINT flags, REFIID riid, void **ppFactory)
{
   if (!ppFactory) return NPT_E_INVALIDARG;
   *ppFactory = NULL;

   npt_com_init();
   struct npt_device *dev = npt_device_acquire();
   if (!dev) return NPT_E_FAIL;

   void *raw = NULL;
   HRESULT hr = npt_call_CreateDXGIFactory2(dev->ring, flags, riid, &raw);
   return finish_create_dxgi_factory(dev, &NPT_IID_IDXGIFactory2, raw, hr,
                                     ppFactory);
}
