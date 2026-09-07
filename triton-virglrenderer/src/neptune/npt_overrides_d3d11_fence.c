/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * D3D11 fence dispatch overrides.  The DC4::Signal hook is the
 * lifecycle entry point for fence feedback; substrate poller and
 * resolve logic live in npt_feedback.c.
 */

#include "npt_context.h"
#include "npt_feedback.h"
#include "npt_overrides.h"

#include "neptune-protocol/npt_protocol_host_dispatch_types.h"
#include "neptune-protocol/npt_protocol_host_id3d11devicecontext.h"

/* Invoke the host's real Signal first so the queued advance is
 * visible to subsequent host D3D11 calls, then queue the per-fence
 * registry entry for the next between-commands poll.  args->pFence
 * is already resolved to the host pointer by the dispatcher. */
static HRESULT
npt_override_DC4_Signal(struct npt_dispatch_context *dctx,
                        struct npt_command_ID3D11DeviceContext4_Signal *args,
                        PFN_ID3D11DeviceContext4_Signal original)
{
   args->ret = original(args->_self, args->pFence, args->Value);

   if (NPT_SUCCEEDED(args->ret) && args->pFence) {
      npt_feedback_fence_mark_signal(npt_context_from_dispatch(dctx),
                                     args->pFence, args->Value);
   }

   return args->ret;
}

struct npt_dispatch_id3d11devicecontext4_overrides
npt_fence_dc4_overrides = {
   .Signal = npt_override_DC4_Signal,
};
