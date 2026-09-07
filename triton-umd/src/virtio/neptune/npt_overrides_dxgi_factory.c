/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * IDXGIFactory{,1..7} overrides.  Each tier carries its own vtbl;
 * the methods below mirror the host D3D library's behaviour
 * (S_OK / hWnd=NULL pass-through, UNSUPPORTED for software adapter,
 * NOTIMPL for stereo / occlusion / hardware-content-protection).
 */

#include "npt_com.h"
#include "npt_overrides.h"
#include "npt_swapchain.h"
#include "npt_device.h"

#include "neptune-protocol/npt_protocol_client_idxgifactory.h"
#include "neptune-protocol/npt_protocol_client_idxgiswapchain.h"
#include "neptune-protocol/npt_protocol_defs.h"

/* A swap chain created with a fullscreen desc must transition to fullscreen
 * immediately.  The swap chain wrapper's SetFullscreenState override does the
 * guest-side window management. */
static void
npt_swapchain_apply_initial_fullscreen(void *swapchain)
{
   if (!swapchain)
      return;
   const struct npt_idxgiswapchain_client_vtbl *v =
      (const void *)((struct npt_com_base *)swapchain)->lpVtbl;
   v->SetFullscreenState(swapchain, TRUE, NULL);
}

static HRESULT NPT_STDMETHODCALLTYPE
fac_MakeWindowAssociation_override(void *self, HWND WindowHandle, UINT Flags)
{
   (void)self; (void)WindowHandle; (void)Flags;
   return NPT_S_OK;
}

static HRESULT NPT_STDMETHODCALLTYPE
fac_GetWindowAssociation_override(void *self, HWND *pWindowHandle)
{
   (void)self;
   if (pWindowHandle)
      *pWindowHandle = (HWND)(uintptr_t)0;
   return NPT_S_OK;
}

static HRESULT NPT_STDMETHODCALLTYPE
fac_CreateSoftwareAdapter_override(void *self, HMODULE Module, IDXGIAdapter **ppAdapter)
{
   (void)self; (void)Module;
   if (ppAdapter)
      *ppAdapter = NULL;
   return NPT_DXGI_ERROR_UNSUPPORTED;
}

static HRESULT NPT_STDMETHODCALLTYPE
fac2_RegisterStereoStatusWindow_override(void *self, HWND WindowHandle, UINT wMsg, DWORD *pdwCookie)
{
   (void)self; (void)WindowHandle; (void)wMsg;
   if (pdwCookie) *pdwCookie = 0;
   return NPT_E_NOTIMPL;
}

static HRESULT NPT_STDMETHODCALLTYPE
fac2_RegisterStereoStatusEvent_override(void *self, HANDLE hEvent, DWORD *pdwCookie)
{
   (void)self; (void)hEvent;
   if (pdwCookie) *pdwCookie = 0;
   return NPT_E_NOTIMPL;
}

static void NPT_STDMETHODCALLTYPE
fac2_UnregisterStereoStatus_override(void *self, DWORD dwCookie)
{
   (void)self; (void)dwCookie;
}

static HRESULT NPT_STDMETHODCALLTYPE
fac2_RegisterOcclusionStatusWindow_override(void *self, HWND WindowHandle, UINT wMsg, DWORD *pdwCookie)
{
   (void)self; (void)WindowHandle; (void)wMsg;
   if (pdwCookie) *pdwCookie = 0;
   return NPT_E_NOTIMPL;
}

static HRESULT NPT_STDMETHODCALLTYPE
fac2_RegisterOcclusionStatusEvent_override(void *self, HANDLE hEvent, DWORD *pdwCookie)
{
   (void)self; (void)hEvent;
   if (pdwCookie) *pdwCookie = 0;
   return NPT_E_NOTIMPL;
}

static void NPT_STDMETHODCALLTYPE
fac2_UnregisterOcclusionStatus_override(void *self, DWORD dwCookie)
{
   (void)self; (void)dwCookie;
}

static HRESULT NPT_STDMETHODCALLTYPE
fac2_GetSharedResourceAdapterLuid_override(void *self, HANDLE hResource, LUID *pLuid)
{
   (void)self; (void)hResource;
   if (pLuid) { pLuid->LowPart = 0; pLuid->HighPart = 0; }
   return NPT_E_NOTIMPL;
}

/* Register fails NOTIMPL but Unregister returns S_OK so cleanup paths
 * conditionally guarded on hr don't log spuriously. */
static HRESULT NPT_STDMETHODCALLTYPE
fac7_RegisterAdaptersChangedEvent_override(void *self, HANDLE hEvent, DWORD *pdwCookie)
{
   (void)self; (void)hEvent;
   if (pdwCookie) *pdwCookie = 0;
   return NPT_E_NOTIMPL;
}

static HRESULT NPT_STDMETHODCALLTYPE
fac7_UnregisterAdaptersChangedEvent_override(void *self, DWORD dwCookie)
{
   (void)self; (void)dwCookie;
   return NPT_S_OK;
}

/* =========================================================================
 * Swapchain creation + IDXGIOutput-related overrides
 *
 * Swapchains are guest-fabricated on Wine (the host refuses to create
 * one); see npt_guest_swapchain_create.  On native Win32 the helper
 * returns E_NOTIMPL, matching the host's refusal, and the wire forward
 * is skipped entirely.
 *
 * pRestrictToOutput would be a guest-fab IDXGIOutput (see
 * npt_overrides_dxgi_output.c); it never reaches the host.
 * ========================================================================= */

static HRESULT NPT_STDMETHODCALLTYPE
fac_CreateSwapChain_override(void *self, IUnknown *pDevice,
                             DXGI_SWAP_CHAIN_DESC *pDesc,
                             IDXGISwapChain **ppSwapChain)
{
   if (!pDevice || !pDesc || !ppSwapChain)
      return NPT_DXGI_ERROR_INVALID_CALL;
   HRESULT hr = npt_guest_swapchain_create_legacy(pDevice, pDesc, self,
                                                  (void **)ppSwapChain);
   if (NPT_SUCCEEDED(hr) && !pDesc->Windowed)
      npt_swapchain_apply_initial_fullscreen(*ppSwapChain);
   return hr;
}

static HRESULT NPT_STDMETHODCALLTYPE
fac2_CreateSwapChainForHwnd_override(void *self, IUnknown *pDevice, HWND hWnd,
                                     const DXGI_SWAP_CHAIN_DESC1 *pDesc,
                                     const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFs,
                                     IDXGIOutput *pRestrictToOutput,
                                     IDXGISwapChain1 **ppSwapChain)
{
   (void)pRestrictToOutput;
   if (!pDevice || !pDesc || !ppSwapChain)
      return NPT_DXGI_ERROR_INVALID_CALL;
   HRESULT hr = npt_guest_swapchain_create(pDevice, pDesc, pFs, hWnd, self,
                                           (void **)ppSwapChain);
   /* pFs == NULL means windowed (per DXGI); only enter fullscreen when the
    * app explicitly asked for it. */
   if (NPT_SUCCEEDED(hr) && pFs && !pFs->Windowed)
      npt_swapchain_apply_initial_fullscreen(*ppSwapChain);
   return hr;
}

/* CoreWindow/composition have no HWND for the X11 WSI to bind. */
static HRESULT NPT_STDMETHODCALLTYPE
fac2_CreateSwapChainForCoreWindow_override(void *self, IUnknown *pDevice,
                                           IUnknown *pWindow,
                                           const DXGI_SWAP_CHAIN_DESC1 *pDesc,
                                           IDXGIOutput *pRestrictToOutput,
                                           IDXGISwapChain1 **ppSwapChain)
{
   (void)self; (void)pDevice; (void)pWindow; (void)pDesc;
   (void)pRestrictToOutput;
   if (ppSwapChain)
      *ppSwapChain = NULL;
   return NPT_DXGI_ERROR_INVALID_CALL;
}

static HRESULT NPT_STDMETHODCALLTYPE
fac2_CreateSwapChainForComposition_override(void *self, IUnknown *pDevice,
                                            const DXGI_SWAP_CHAIN_DESC1 *pDesc,
                                            IDXGIOutput *pRestrictToOutput,
                                            IDXGISwapChain1 **ppSwapChain)
{
   (void)self; (void)pDevice; (void)pDesc; (void)pRestrictToOutput;
   if (ppSwapChain)
      *ppSwapChain = NULL;
   return NPT_DXGI_ERROR_INVALID_CALL;
}

#define NPT_REGISTER_OVERRIDE_DXGI_FACTORY7(m, f) \
   NPT_REGISTER_OVERRIDE(idxgifactory7, m, f)
#define NPT_REGISTER_OVERRIDE_DXGI_FACTORY6(m, f) \
   NPT_REGISTER_OVERRIDE(idxgifactory6, m, f); \
   NPT_REGISTER_OVERRIDE_DXGI_FACTORY7(m, f)
#define NPT_REGISTER_OVERRIDE_DXGI_FACTORY5(m, f) \
   NPT_REGISTER_OVERRIDE(idxgifactory5, m, f); \
   NPT_REGISTER_OVERRIDE_DXGI_FACTORY6(m, f)
#define NPT_REGISTER_OVERRIDE_DXGI_FACTORY4(m, f) \
   NPT_REGISTER_OVERRIDE(idxgifactory4, m, f); \
   NPT_REGISTER_OVERRIDE_DXGI_FACTORY5(m, f)
#define NPT_REGISTER_OVERRIDE_DXGI_FACTORY3(m, f) \
   NPT_REGISTER_OVERRIDE(idxgifactory3, m, f); \
   NPT_REGISTER_OVERRIDE_DXGI_FACTORY4(m, f)
#define NPT_REGISTER_OVERRIDE_DXGI_FACTORY2(m, f) \
   NPT_REGISTER_OVERRIDE(idxgifactory2, m, f); \
   NPT_REGISTER_OVERRIDE_DXGI_FACTORY3(m, f)
#define NPT_REGISTER_OVERRIDE_DXGI_FACTORY1(m, f) \
   NPT_REGISTER_OVERRIDE(idxgifactory1, m, f); \
   NPT_REGISTER_OVERRIDE_DXGI_FACTORY2(m, f)
#define NPT_REGISTER_OVERRIDE_DXGI_FACTORY(m, f) \
   NPT_REGISTER_OVERRIDE(idxgifactory, m, f); \
   NPT_REGISTER_OVERRIDE_DXGI_FACTORY1(m, f)

static const GUID *const factory_tiers[] = {
   &NPT_IID_IDXGIFactory,  &NPT_IID_IDXGIFactory1, &NPT_IID_IDXGIFactory2,
   &NPT_IID_IDXGIFactory3, &NPT_IID_IDXGIFactory4, &NPT_IID_IDXGIFactory5,
   &NPT_IID_IDXGIFactory6, &NPT_IID_IDXGIFactory7, NULL,
};

void
npt_overrides_dxgi_factory_init(void)
{
   NPT_REGISTER_OVERRIDE_DXGI_FACTORY (MakeWindowAssociation,
                                       fac_MakeWindowAssociation_override);
   NPT_REGISTER_OVERRIDE_DXGI_FACTORY (GetWindowAssociation,
                                       fac_GetWindowAssociation_override);
   NPT_REGISTER_OVERRIDE_DXGI_FACTORY (CreateSoftwareAdapter,
                                       fac_CreateSoftwareAdapter_override);
   NPT_REGISTER_OVERRIDE_DXGI_FACTORY2(RegisterStereoStatusWindow,
                                       fac2_RegisterStereoStatusWindow_override);
   NPT_REGISTER_OVERRIDE_DXGI_FACTORY2(RegisterStereoStatusEvent,
                                       fac2_RegisterStereoStatusEvent_override);
   NPT_REGISTER_OVERRIDE_DXGI_FACTORY2(UnregisterStereoStatus,
                                       fac2_UnregisterStereoStatus_override);
   NPT_REGISTER_OVERRIDE_DXGI_FACTORY2(RegisterOcclusionStatusWindow,
                                       fac2_RegisterOcclusionStatusWindow_override);
   NPT_REGISTER_OVERRIDE_DXGI_FACTORY2(RegisterOcclusionStatusEvent,
                                       fac2_RegisterOcclusionStatusEvent_override);
   NPT_REGISTER_OVERRIDE_DXGI_FACTORY2(UnregisterOcclusionStatus,
                                       fac2_UnregisterOcclusionStatus_override);
   NPT_REGISTER_OVERRIDE_DXGI_FACTORY2(GetSharedResourceAdapterLuid,
                                       fac2_GetSharedResourceAdapterLuid_override);
   NPT_REGISTER_OVERRIDE_DXGI_FACTORY7(RegisterAdaptersChangedEvent,
                                       fac7_RegisterAdaptersChangedEvent_override);
   NPT_REGISTER_OVERRIDE_DXGI_FACTORY7(UnregisterAdaptersChangedEvent,
                                       fac7_UnregisterAdaptersChangedEvent_override);
   NPT_REGISTER_OVERRIDE_DXGI_FACTORY (CreateSwapChain,
                                       fac_CreateSwapChain_override);
   NPT_REGISTER_OVERRIDE_DXGI_FACTORY2(CreateSwapChainForHwnd,
                                       fac2_CreateSwapChainForHwnd_override);
   NPT_REGISTER_OVERRIDE_DXGI_FACTORY2(CreateSwapChainForCoreWindow,
                                       fac2_CreateSwapChainForCoreWindow_override);
   NPT_REGISTER_OVERRIDE_DXGI_FACTORY2(CreateSwapChainForComposition,
                                       fac2_CreateSwapChainForComposition_override);

   npt_com_register_family(factory_tiers, 0, NULL);
}
