/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * IDXGIFactoryMedia overrides.  Composition-surface and decode swapchains
 * have no guest-fabricated implementation and the host owns no swapchains,
 * so both creation methods return DXGI_ERROR_UNSUPPORTED rather than
 * forwarding to the host.
 */

#include "npt_com.h"
#include "npt_overrides.h"
#include "npt_device.h"

#include "neptune-protocol/npt_protocol_client_idxgifactorymedia.h"
#include "neptune-protocol/npt_protocol_defs.h"

static HRESULT NPT_STDMETHODCALLTYPE
facm_CreateSwapChainForCompositionSurfaceHandle_override(
   void *self, IUnknown *pDevice, HANDLE hSurface,
   const DXGI_SWAP_CHAIN_DESC1 *pDesc, IDXGIOutput *pRestrictToOutput,
   IDXGISwapChain1 **ppSwapChain)
{
   (void)self; (void)pDevice; (void)hSurface; (void)pDesc;
   (void)pRestrictToOutput;
   if (ppSwapChain)
      *ppSwapChain = NULL;
   return NPT_DXGI_ERROR_UNSUPPORTED;
}

static HRESULT NPT_STDMETHODCALLTYPE
facm_CreateDecodeSwapChainForCompositionSurfaceHandle_override(
   void *self, IUnknown *pDevice, HANDLE hSurface,
   DXGI_DECODE_SWAP_CHAIN_DESC *pDesc, IDXGIResource *pYuvDecodeBuffers,
   IDXGIOutput *pRestrictToOutput, IDXGIDecodeSwapChain **ppSwapChain)
{
   (void)self; (void)pDevice; (void)hSurface; (void)pDesc;
   (void)pYuvDecodeBuffers; (void)pRestrictToOutput;
   if (ppSwapChain)
      *ppSwapChain = NULL;
   return NPT_DXGI_ERROR_UNSUPPORTED;
}

static const GUID *const factorymedia_tiers[] = {
   &NPT_IID_IDXGIFactoryMedia, NULL,
};

void
npt_overrides_dxgi_factorymedia_init(void)
{
   NPT_REGISTER_OVERRIDE(idxgifactorymedia,
      CreateSwapChainForCompositionSurfaceHandle,
      facm_CreateSwapChainForCompositionSurfaceHandle_override);
   NPT_REGISTER_OVERRIDE(idxgifactorymedia,
      CreateDecodeSwapChainForCompositionSurfaceHandle,
      facm_CreateDecodeSwapChainForCompositionSurfaceHandle_override);

   npt_com_register_family(factorymedia_tiers, 0, NULL);
}
