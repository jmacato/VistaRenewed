/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * D3D11.dll entry points: forward to the generated host call and wrap
 * the returned host pointers via the runtime IID -> ctor table.
 */

#include "npt_com.h"
#include "npt_device.h"
#include "npt_swapchain.h"

#include "neptune-protocol/npt_protocol_guest_toplevel.h"
#include "neptune-protocol/npt_protocol_defs.h"

/* Cross-TU entry helpers: Triton's tritonDDI.c declares matching
 * externs, and the D3D11* exports below wrap them. */
HRESULT
npt_d3d11_create_device_internal(IDXGIAdapter *pAdapter,
                  D3D_DRIVER_TYPE DriverType,
                  HMODULE Software,
                  UINT Flags,
                  const D3D_FEATURE_LEVEL *pFeatureLevels,
                  UINT FeatureLevels,
                  UINT SDKVersion,
                  ID3D11Device **ppDevice,
                  D3D_FEATURE_LEVEL *pFeatureLevel,
                  ID3D11DeviceContext **ppImmediateContext);
HRESULT
npt_d3d11on12_create_device_internal(IUnknown *pDevice,
                      UINT Flags,
                      const D3D_FEATURE_LEVEL *pFeatureLevels,
                      UINT FeatureLevels,
                      const IUnknown **ppCommandQueues,
                      UINT NumQueues,
                      UINT NodeMask,
                      ID3D11Device **ppDevice,
                      ID3D11DeviceContext **ppImmediateContext,
                      D3D_FEATURE_LEVEL *pChosenFeatureLevel);
HRESULT
npt_d3d11_create_device_and_swap_chain_internal(IDXGIAdapter *pAdapter,
                              D3D_DRIVER_TYPE DriverType,
                              HMODULE Software,
                              UINT Flags,
                              const D3D_FEATURE_LEVEL *pFeatureLevels,
                              UINT FeatureLevels,
                              UINT SDKVersion,
                              const DXGI_SWAP_CHAIN_DESC *pSwapChainDesc,
                              IDXGISwapChain **ppSwapChain,
                              ID3D11Device **ppDevice,
                              D3D_FEATURE_LEVEL *pFeatureLevel,
                              ID3D11DeviceContext **ppImmediateContext);

HRESULT
npt_d3d11_create_device_internal(IDXGIAdapter *pAdapter,
                  D3D_DRIVER_TYPE DriverType,
                  HMODULE Software,
                  UINT Flags,
                  const D3D_FEATURE_LEVEL *pFeatureLevels,
                  UINT FeatureLevels,
                  UINT SDKVersion,
                  ID3D11Device **ppDevice,
                  D3D_FEATURE_LEVEL *pFeatureLevel,
                  ID3D11DeviceContext **ppImmediateContext)
{
   /* Guest-side spec check: Software is stripped on the wire, so the
    * host can no longer enforce the `|| Software` clause itself. */
   if (pAdapter && (DriverType != D3D_DRIVER_TYPE_UNKNOWN || Software))
      return NPT_E_INVALIDARG;

   npt_com_init();
   /* Top-level: device_ref_holds carries the acquire onto the wrapper. */
   struct npt_device *dev = npt_device_acquire();
   if (!dev) return NPT_E_FAIL;

   ID3D11Device *raw_device = NULL;
   ID3D11DeviceContext *raw_context = NULL;
   D3D_FEATURE_LEVEL fl = 0;

   /* Encoder pulls host_id via npt_object_get_id(pAdapter); forward the
    * live wrapper directly (NULL-safe inside the helper). */
   HRESULT hr = npt_call_D3D11CreateDevice(
      dev->ring,
      pAdapter,
      DriverType,
      (HMODULE)0,
      Flags,
      pFeatureLevels,
      FeatureLevels,
      SDKVersion,
      &raw_device,
      pFeatureLevel ? &fl : NULL,
      &raw_context);

   if (NPT_FAILED(hr) || !raw_device) {
      if (raw_device)
         npt_com_send_release(dev, (uint64_t)(uintptr_t)raw_device);
      if (raw_context)
         npt_com_send_release(dev, (uint64_t)(uintptr_t)raw_context);
      npt_device_release(dev);
      return NPT_FAILED(hr) ? hr : NPT_E_FAIL;
   }

   ID3D11Device *device_wrapper = (ID3D11Device *)
      npt_com_get_or_wrap_or_release(dev, &NPT_IID_ID3D11Device,
                                     (uint64_t)(uintptr_t)raw_device, NULL);
   if (!device_wrapper) {
      if (raw_context)
         npt_com_send_release(dev, (uint64_t)(uintptr_t)raw_context);
      npt_device_release(dev);
      return NPT_E_OUTOFMEMORY;
   }
   npt_device_cache_workarounds(dev);

   /* Counter so multiple entry-point calls collapsing onto one
    * wrapper each contribute a balanced acquire/release. */
   atomic_fetch_add_explicit(
      &((struct npt_com_base *)device_wrapper)->base.device_ref_holds,
      1, memory_order_relaxed);

   ID3D11DeviceContext *ctx_wrapper = NULL;
   if (raw_context) {
      ctx_wrapper = (ID3D11DeviceContext *)
         npt_com_get_or_wrap_or_release(dev, &NPT_IID_ID3D11DeviceContext,
                                        (uint64_t)(uintptr_t)raw_context,
                                        (struct npt_com_base *)device_wrapper);
      if (!ctx_wrapper) {
         npt_com_default_release(device_wrapper);
         return NPT_E_OUTOFMEMORY;
      }
   }

   if (ppDevice)
      *ppDevice = device_wrapper;
   else
      npt_com_default_release(device_wrapper);

   if (ppImmediateContext)
      *ppImmediateContext = ctx_wrapper;
   else if (ctx_wrapper)
      npt_com_default_release(ctx_wrapper);

   if (pFeatureLevel) *pFeatureLevel = fl;
   return hr;
}

HRESULT
npt_d3d11on12_create_device_internal(IUnknown *pDevice,
                      UINT Flags,
                      const D3D_FEATURE_LEVEL *pFeatureLevels,
                      UINT FeatureLevels,
                      const IUnknown **ppCommandQueues,
                      UINT NumQueues,
                      UINT NodeMask,
                      ID3D11Device **ppDevice,
                      ID3D11DeviceContext **ppImmediateContext,
                      D3D_FEATURE_LEVEL *pChosenFeatureLevel)
{
   if (!pDevice || (NumQueues && !ppCommandQueues))
      return NPT_E_INVALIDARG;

   npt_com_init();
   struct npt_device *dev = npt_device_acquire();
   if (!dev) return NPT_E_FAIL;

   ID3D11Device *raw_device = NULL;
   ID3D11DeviceContext *raw_context = NULL;
   D3D_FEATURE_LEVEL fl = 0;

   HRESULT hr = npt_call_D3D11On12CreateDevice(
      dev->ring,
      pDevice, Flags,
      pFeatureLevels, FeatureLevels,
      ppCommandQueues, NumQueues, NodeMask,
      &raw_device,
      &raw_context,
      pChosenFeatureLevel ? &fl : NULL);

   if (NPT_FAILED(hr) || !raw_device) {
      if (raw_device)
         npt_com_send_release(dev, (uint64_t)(uintptr_t)raw_device);
      if (raw_context)
         npt_com_send_release(dev, (uint64_t)(uintptr_t)raw_context);
      npt_device_release(dev);
      return NPT_FAILED(hr) ? hr : NPT_E_FAIL;
   }

   ID3D11Device *device_wrapper = (ID3D11Device *)
      npt_com_get_or_wrap_or_release(dev, &NPT_IID_ID3D11Device,
                                     (uint64_t)(uintptr_t)raw_device, NULL);
   if (!device_wrapper) {
      if (raw_context)
         npt_com_send_release(dev, (uint64_t)(uintptr_t)raw_context);
      npt_device_release(dev);
      return NPT_E_OUTOFMEMORY;
   }
   atomic_fetch_add_explicit(
      &((struct npt_com_base *)device_wrapper)->base.device_ref_holds,
      1, memory_order_relaxed);

   ID3D11DeviceContext *ctx_wrapper = NULL;
   if (raw_context) {
      ctx_wrapper = (ID3D11DeviceContext *)
         npt_com_get_or_wrap_or_release(dev, &NPT_IID_ID3D11DeviceContext,
                                        (uint64_t)(uintptr_t)raw_context,
                                        (struct npt_com_base *)device_wrapper);
      if (!ctx_wrapper) {
         npt_com_default_release(device_wrapper);
         return NPT_E_OUTOFMEMORY;
      }
   }

   if (ppDevice)
      *ppDevice = device_wrapper;
   else
      npt_com_default_release(device_wrapper);

   if (ppImmediateContext)
      *ppImmediateContext = ctx_wrapper;
   else if (ctx_wrapper)
      npt_com_default_release(ctx_wrapper);

   if (pChosenFeatureLevel) *pChosenFeatureLevel = fl;
   return hr;
}

HRESULT
npt_d3d11_create_device_and_swap_chain_internal(IDXGIAdapter *pAdapter,
                              D3D_DRIVER_TYPE DriverType,
                              HMODULE Software,
                              UINT Flags,
                              const D3D_FEATURE_LEVEL *pFeatureLevels,
                              UINT FeatureLevels,
                              UINT SDKVersion,
                              const DXGI_SWAP_CHAIN_DESC *pSwapChainDesc,
                              IDXGISwapChain **ppSwapChain,
                              ID3D11Device **ppDevice,
                              D3D_FEATURE_LEVEL *pFeatureLevel,
                              ID3D11DeviceContext **ppImmediateContext)
{
   /* See D3D11CreateDevice. */
   if (pAdapter && (DriverType != D3D_DRIVER_TYPE_UNKNOWN || Software))
      return NPT_E_INVALIDARG;

   npt_com_init();
   struct npt_device *dev = npt_device_acquire();
   if (!dev) return NPT_E_FAIL;

   ID3D11Device *raw_device = NULL;
   ID3D11DeviceContext *raw_context = NULL;
   D3D_FEATURE_LEVEL fl = 0;

   HRESULT hr = npt_call_D3D11CreateDevice(
      dev->ring,
      pAdapter,
      DriverType, (HMODULE)0, Flags,
      pFeatureLevels, FeatureLevels, SDKVersion,
      &raw_device, pFeatureLevel ? &fl : NULL, &raw_context);

   if (NPT_FAILED(hr) || !raw_device) {
      if (raw_device)
         npt_com_send_release(dev, (uint64_t)(uintptr_t)raw_device);
      if (raw_context)
         npt_com_send_release(dev, (uint64_t)(uintptr_t)raw_context);
      npt_device_release(dev);
      return NPT_FAILED(hr) ? hr : NPT_E_FAIL;
   }

   ID3D11Device *device_wrapper = (ID3D11Device *)
      npt_com_get_or_wrap_or_release(dev, &NPT_IID_ID3D11Device,
                                     (uint64_t)(uintptr_t)raw_device, NULL);
   if (!device_wrapper) {
      if (raw_context)
         npt_com_send_release(dev, (uint64_t)(uintptr_t)raw_context);
      npt_device_release(dev);
      return NPT_E_OUTOFMEMORY;
   }
   atomic_fetch_add_explicit(
      &((struct npt_com_base *)device_wrapper)->base.device_ref_holds,
      1, memory_order_relaxed);

   ID3D11DeviceContext *ctx_wrapper = NULL;
   if (raw_context) {
      ctx_wrapper = (ID3D11DeviceContext *)
         npt_com_get_or_wrap_or_release(dev, &NPT_IID_ID3D11DeviceContext,
                                        (uint64_t)(uintptr_t)raw_context,
                                        (struct npt_com_base *)device_wrapper);
      if (!ctx_wrapper) {
         npt_com_default_release(device_wrapper);
         return NPT_E_OUTOFMEMORY;
      }
   }

   IDXGISwapChain *sc_wrapper = NULL;
   if (ppSwapChain && pSwapChainDesc) {
      HRESULT sc_hr = npt_guest_swapchain_create_legacy(
         device_wrapper, pSwapChainDesc, NULL, (void **)&sc_wrapper);
      /* E_NOTIMPL = no guest presentation backend (native Win32):
       * return a NULL swapchain with a successful device.  Real
       * failures fail the call. */
      if (NPT_FAILED(sc_hr) && sc_hr != NPT_E_NOTIMPL) {
         if (ctx_wrapper)
            npt_com_default_release(ctx_wrapper);
         npt_com_default_release(device_wrapper);
         return sc_hr;
      }
   }

   if (ppDevice)
      *ppDevice = device_wrapper;
   else
      npt_com_default_release(device_wrapper);

   if (ppImmediateContext)
      *ppImmediateContext = ctx_wrapper;
   else if (ctx_wrapper)
      npt_com_default_release(ctx_wrapper);

   if (ppSwapChain)
      *ppSwapChain = sc_wrapper;
   else if (sc_wrapper)
      npt_com_default_release(sc_wrapper);

   if (pFeatureLevel) *pFeatureLevel = fl;
   return hr;
}

/* Exported entry points: thin trampolines so callers reach the COM
 * factory through the Windows runtime's DLL lookup.  Triton statically
 * links the *_internal helpers above and does not go through these. */

#ifndef NPT_NO_D3D11_ENTRYPOINTS
HRESULT NPT_API
D3D11CreateDevice(IDXGIAdapter *pAdapter,
                  D3D_DRIVER_TYPE DriverType,
                  HMODULE Software,
                  UINT Flags,
                  const D3D_FEATURE_LEVEL *pFeatureLevels,
                  UINT FeatureLevels,
                  UINT SDKVersion,
                  ID3D11Device **ppDevice,
                  D3D_FEATURE_LEVEL *pFeatureLevel,
                  ID3D11DeviceContext **ppImmediateContext)
{
   return npt_d3d11_create_device_internal(pAdapter, DriverType, Software,
                                           Flags, pFeatureLevels,
                                           FeatureLevels, SDKVersion,
                                           ppDevice, pFeatureLevel,
                                           ppImmediateContext);
}

HRESULT NPT_API
D3D11On12CreateDevice(IUnknown *pDevice,
                      UINT Flags,
                      const D3D_FEATURE_LEVEL *pFeatureLevels,
                      UINT FeatureLevels,
                      const IUnknown **ppCommandQueues,
                      UINT NumQueues,
                      UINT NodeMask,
                      ID3D11Device **ppDevice,
                      ID3D11DeviceContext **ppImmediateContext,
                      D3D_FEATURE_LEVEL *pChosenFeatureLevel)
{
   return npt_d3d11on12_create_device_internal(pDevice, Flags, pFeatureLevels,
                                               FeatureLevels, ppCommandQueues,
                                               NumQueues, NodeMask, ppDevice,
                                               ppImmediateContext,
                                               pChosenFeatureLevel);
}

HRESULT NPT_API
D3D11CreateDeviceAndSwapChain(IDXGIAdapter *pAdapter,
                              D3D_DRIVER_TYPE DriverType,
                              HMODULE Software,
                              UINT Flags,
                              const D3D_FEATURE_LEVEL *pFeatureLevels,
                              UINT FeatureLevels,
                              UINT SDKVersion,
                              const DXGI_SWAP_CHAIN_DESC *pSwapChainDesc,
                              IDXGISwapChain **ppSwapChain,
                              ID3D11Device **ppDevice,
                              D3D_FEATURE_LEVEL *pFeatureLevel,
                              ID3D11DeviceContext **ppImmediateContext)
{
   return npt_d3d11_create_device_and_swap_chain_internal(
      pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels,
      SDKVersion, pSwapChainDesc, ppSwapChain, ppDevice, pFeatureLevel,
      ppImmediateContext);
}
#endif /* NPT_NO_D3D11_ENTRYPOINTS */
