/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Device-level kernel-context / virtio-context plumbing for the
 * present path.  See tritonPresent.h.
 */

#include "tritonPresent.h"
#include "triton_log.h"

void
tritonPresentEnsureKernelContext(PTRITON_DEVICE pD)
{
   if (pD->hKMContext || !pD->KTCallbacks.pfnCreateContextCb)
      return;
   /* Serialise the check-and-create under kmCtxLock (the HCONTEXT-domain
    * lock, see triton.h): concurrent present threads — and a primary-create
    * on another thread — could each see hKMContext==NULL and create a
    * duplicate kernel context. */
   if (!pD->presentLockInit)
      return;
   EnterCriticalSection(&pD->kmCtxLock);
   if (!pD->hKMContext) {
      D3DDDICB_CREATECONTEXT ctx;
      memset(&ctx, 0, sizeof(ctx));
      HRESULT hr = pD->KTCallbacks.pfnCreateContextCb(pD->hRTDevice.handle, &ctx);
      if (SUCCEEDED(hr))
         pD->hKMContext = ctx.hContext;
      else
         TR_LOG("present: pfnCreateContextCb failed 0x%08lx", hr);
   }
   LeaveCriticalSection(&pD->kmCtxLock);
}

HRESULT
tritonPresentEscape(PTRITON_DEVICE pD, VIOGPU_ESCAPE *esc)
{
   if (!pD->KTCallbacks.pfnEscapeCb)
      return E_NOTIMPL;
   D3DDDICB_ESCAPE cb;
   memset(&cb, 0, sizeof(cb));
   cb.hDevice               = pD->hRTDevice.handle;
   cb.pPrivateDriverData    = esc;
   cb.PrivateDriverDataSize = sizeof(*esc);
   return pD->KTCallbacks.pfnEscapeCb(pD->pAdapter->hRTAdapter.handle, &cb);
}

void
tritonPresentEnsureRuntimeCtx(PTRITON_DEVICE pD)
{
   if (pD->RuntimeCtxInited)
      return;
   /* Serialise the check-and-set under kmCtxLock: racing first-uses would
    * double-issue VIOGPU_CTX_INIT.  pfnEscapeCb is in the HCONTEXT-callback
    * family (see triton.h). */
   if (!pD->presentLockInit)
      return;
   EnterCriticalSection(&pD->kmCtxLock);
   if (!pD->RuntimeCtxInited) {
      VIOGPU_ESCAPE esc;
      memset(&esc, 0, sizeof(esc));
      esc.Type             = VIOGPU_CTX_INIT;
      esc.DataLength       = sizeof(esc.CtxInit);
      esc.CtxInit.CapsetID = 7;
      esc.CtxInit.NumRings = 1;
      /* Latch only on success: every later blob create and context attach
       * depends on this context existing, so a failure must stay retryable
       * rather than silently disabling them for the device's lifetime. */
      HRESULT hr = tritonPresentEscape(pD, &esc);
      if (SUCCEEDED(hr))
         pD->RuntimeCtxInited = TRUE;
      else
         TR_LOG("present: VIOGPU_CTX_INIT failed 0x%08lx", (unsigned long)hr);
   }
   LeaveCriticalSection(&pD->kmCtxLock);
}
