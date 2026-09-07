/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * DXGI DDI handlers and base function table installer.
 *
 * MSDN: DXGI_DDI_BASE_FUNCTIONS / DXGI1_2_DDI_BASE_FUNCTIONS /
 * DXGI1_3_DDI_BASE_FUNCTIONS — function pointer signatures and the
 * IS_DXGI*_BASE_FUNCTIONS tier-detection macros live in d3d10umddi.h
 * and dxgiddi.h.
 */

#include "triton.h"
#include "triton_log.h"
#include "tritonDxgi.h"
#include "tritonPresent.h"
#include "npt_env.h"

#include <dxgiddi.h>

#include "tritonBlitShaders.h"

/* Lazily create the GPU-done fence (rationale: the present-fence block in
 * triton.h).  Caller holds presentLock.  Returns TRUE if fencing is
 * available; FALSE => unfenced (bare-Flush) present. */
static BOOL
tritonPresentEnsureFenceLocked(PTRITON_DEVICE pD)
{
   if (pD->presentFenceReady)    return TRUE;
   if (pD->presentFenceDisabled) return FALSE;
   pD->presentFenceDisabled = TRUE;   /* pessimistic; cleared only on full success */

   if (!pD->pDev1 || !pD->pCtx1) return FALSE;

   ID3D11Device5 *pDev5 = NULL;
   if (FAILED(ID3D11Device1_QueryInterface(pD->pDev1, &IID_ID3D11Device5,
                                           (void **)&pDev5)) || !pDev5) {
      TR_LOG("present-fence: no ID3D11Device5; unfenced present");
      return FALSE;
   }
   ID3D11DeviceContext4 *pCtx4 = NULL;
   if (FAILED(ID3D11DeviceContext1_QueryInterface(pD->pCtx1, &IID_ID3D11DeviceContext4,
                                                  (void **)&pCtx4)) || !pCtx4) {
      ID3D11Device5_Release(pDev5);
      TR_LOG("present-fence: no ID3D11DeviceContext4; unfenced present");
      return FALSE;
   }
   ID3D11Fence *pFence = NULL;
   HRESULT hr = ID3D11Device5_CreateFence(pDev5, 0, D3D11_FENCE_FLAG_NONE,
                                          &IID_ID3D11Fence, (void **)&pFence);
   ID3D11Device5_Release(pDev5);
   if (FAILED(hr) || !pFence) {
      ID3D11DeviceContext4_Release(pCtx4);
      TR_LOG("present-fence: CreateFence 0x%08lx; unfenced present", (unsigned long)hr);
      return FALSE;
   }
   pD->pCtx4                = pCtx4;
   pD->pPresentFence        = pFence;
   pD->presentFenceValue    = 0;
   pD->presentFenceDisabled = FALSE;
   pD->presentFenceReady    = TRUE;
   TR_LOG("present-fence: armed (async GPU-gated flips)");
   return TRUE;
}

/* Pop a reusable auto-reset event for one GPU-done wait (caller holds
 * presentLock).  Every concurrent waiter takes its own event, for the reason
 * given with the pool in triton.h.  The KMD signals it (via npt_event's
 * waiter) at GPU completion; WaitForSingleObject on it blocks the present
 * thread. */
static HANDLE
tritonPresentPopEvent(PTRITON_DEVICE pD)
{
   /* Reuse a pooled event only if its SetEventOnCompletion registration has
    * already FIRED -- i.e. the event is signalled (auto-reset).  The KMD
    * signals the event when the present fence retires, so a signalled event
    * means its present completed and re-arming it cannot cross-talk (reusing
    * a still-armed event lets an old registration wake a NEWER frame's wait
    * early -- the black-frame bug).  Do NOT judge retirement by
    * ID3D11Fence_GetCompletedValue: that reads a periodically-published
    * feedback copy that LAGS real completion, so the check fails spuriously
    * and the pop creates (then, once the pool fills, leaks) a fresh event
    * almost every frame.  WaitForSingleObject(h,0) both tests AND drains
    * the fired signal. */
   for (UINT i = 0; i < pD->presentEventPoolCount; i++) {
      HANDLE h = pD->presentEventPool[i];
      /* Reusable when no registration is outstanding (nothing can signal it
       * spuriously), or when the outstanding one has already fired -- the
       * WaitForSingleObject below both tests AND drains that signal. */
      if (!pD->presentEventPoolArmed[i] ||
          WaitForSingleObject(h, 0) == WAIT_OBJECT_0) {
         UINT last = --pD->presentEventPoolCount;
         pD->presentEventPool[i] = pD->presentEventPool[last];
         pD->presentEventPoolArmed[i] = pD->presentEventPoolArmed[last];
         return h;
      }
   }
   return CreateEventW(NULL, /*bManualReset*/ FALSE, /*bInitial*/ FALSE, NULL);
}

/* Discard a signal left on a pooled event by an earlier registration.
 * Events return to the pool with a SetEventOnCompletion registration still
 * outstanding (a wait normally exits on GetCompletedValue before the event
 * fires), and that registration signals the pooled handle later.  Draining
 * before every arm makes a subsequent wake mean this frame's completion
 * rather than a previous one's. */
static BOOL
tritonPresentDrainEvent(HANDLE hEvt)
{
   return hEvt && WaitForSingleObject(hEvt, 0) == WAIT_OBJECT_0;
}

/* Return an event after use (caller holds presentLock).  armed: a
 * SetEventOnCompletion registration is still outstanding on this event, so
 * on pool overflow it must be leaked rather than closed — the fence could
 * otherwise later signal a recycled handle value. */
static void
tritonPresentPushEvent(PTRITON_DEVICE pD, HANDLE hEvt, BOOL armed)
{
   if (!hEvt)
      return;
   if (pD->presentEventPoolCount < TRITON_PRESENT_EVENT_POOL) {
      pD->presentEventPoolArmed[pD->presentEventPoolCount] = armed;
      pD->presentEventPool[pD->presentEventPoolCount++] = hEvt;
   } else if (!armed) {
      CloseHandle(hEvt);
   } else {
      static LONG leakN;
      LONG n = InterlockedIncrement(&leakN);
      if (n == 1 || (n & 255) == 0)
         TR_LOG("present-fence: event pool overflow on armed event #%ld (leaking)", n);
   }
}

/* Take/drop presentLock where the caller tolerates it not existing yet: the
 * critical sections are created on the CreateDevice success path, and before
 * that there is no second thread on this device to serialise against. */
static void
tritonPresentLock(PTRITON_DEVICE pD)
{
   if (pD->presentLockInit)
      EnterCriticalSection(&pD->presentLock);
}

static void
tritonPresentUnlock(PTRITON_DEVICE pD)
{
   if (pD->presentLockInit)
      LeaveCriticalSection(&pD->presentLock);
}

/* ---- present stage timing (NPT_DEBUG=present_timing) ------------------
 *
 * A present that measures ~10ms of wall clock tells you nothing about which
 * of its four stages owns the time: the Signal+Flush ring submits, the fence
 * arm (a synchronous ring round-trip plus a D3DKMTEscape), the GPU-completion
 * wait, or the flip callback into dxgkrnl.  These counters split them.
 *
 * Microseconds are accumulated as integers with Interlocked adds because the
 * wait stage runs UNLOCKED and concurrent present threads (DWM has several)
 * would otherwise race a double.  Reporting is amortised over
 * TRITON_PT_REPORT presents so the OutputDebugString cost cannot itself
 * become the thing being measured. */
#define TRITON_PT_REPORT 120

static struct {
   LONG64 n, already, sig_us, arm_us, wait_us, cb_us, wait_max_us;
} g_pt;

static double
triton_pt_qpc_us(void)
{
   static double scale;   /* races are benign: every writer stores the same value */
   LARGE_INTEGER t;
   if (scale == 0.0) {
      LARGE_INTEGER f;
      QueryPerformanceFrequency(&f);
      scale = 1e6 / (double)f.QuadPart;
   }
   QueryPerformanceCounter(&t);
   return (double)t.QuadPart * scale;
}

/* Only the enabled path pays for QueryPerformanceCounter. */
#define TRITON_PT_NOW() (NPT_DEBUG(PRESENT_TIMING) ? triton_pt_qpc_us() : 0.0)

static void
triton_pt_account(double sig, double arm, double wait, BOOL already)
{
   if (!NPT_DEBUG(PRESENT_TIMING))
      return;
   InterlockedAdd64(&g_pt.sig_us,  (LONG64)sig);
   InterlockedAdd64(&g_pt.arm_us,  (LONG64)arm);
   InterlockedAdd64(&g_pt.wait_us, (LONG64)wait);
   if (already)
      InterlockedIncrement64(&g_pt.already);
   for (;;) {   /* max is a hint; a lost race only under-reports an outlier */
      LONG64 cur = g_pt.wait_max_us;
      if ((LONG64)wait <= cur ||
          InterlockedCompareExchange64(&g_pt.wait_max_us, (LONG64)wait, cur) == cur)
         break;
   }
   LONG64 n = InterlockedIncrement64(&g_pt.n);
   if (n % TRITON_PT_REPORT == 0) {
      LONG64 sigT  = InterlockedExchange64(&g_pt.sig_us, 0);
      LONG64 armT  = InterlockedExchange64(&g_pt.arm_us, 0);
      LONG64 waitT = InterlockedExchange64(&g_pt.wait_us, 0);
      LONG64 cbT   = InterlockedExchange64(&g_pt.cb_us, 0);
      LONG64 alr   = InterlockedExchange64(&g_pt.already, 0);
      LONG64 wmax  = InterlockedExchange64(&g_pt.wait_max_us, 0);
      TR_LOG("PT n=%lld avg_us sig=%lld arm=%lld wait=%lld cb=%lld total=%lld "
             "wait_max=%lld nowait=%lld/%d",
             n, sigT / TRITON_PT_REPORT, armT / TRITON_PT_REPORT,
             waitT / TRITON_PT_REPORT, cbT / TRITON_PT_REPORT,
             (sigT + armT + waitT + cbT) / TRITON_PT_REPORT,
             wmax, alr, TRITON_PT_REPORT);
   }
}

/* Flush the frame; when `arm` is set, also signal the present fence and arm
 * the GPU-completion event (whose transport arm is what submits the KMD's
 * flip-gate token); when `wait` is additionally set, block until the GPU
 * retires the marker before returning.  The flush is unconditional: the DXGI
 * DDI requires a present to submit all partially built command buffers, and
 * this is the only place the present path submits them.
 *
 * arm=TRUE  wait=FALSE   scanout primaries (pPrimaryDesc resources).  Per
 *   the MMIO-flip contract, ordering render->scanout is enforced KMD-side:
 *   DxgkDdiPresent peeks the newest token as the flip's render dependency
 *   and VioGpuVidPN withholds the scanout -- and the vsync address report
 *   that would retire the flip -- until it retires.  The present thread
 *   never waits for its own frame, only for the run-ahead bound (see
 *   TRITON_PRESENT_MAX_AHEAD); frames-in-flight is otherwise DXGI's job
 *   (it blocks the APPLICATION once MaximumFrameLatency presents are
 *   outstanding), which is what lets CPU frame N+1 overlap GPU frame N.
 *
 * arm=TRUE  wait=TRUE    DWM-composited surfaces (everything without
 *   pPrimaryDesc).  The consumer is dwm.exe sampling a shared texture from
 *   another process; the frame-ready signal is pfnPresentCb itself and no
 *   scanout-side gate can order that handoff, so the producer must not
 *   report the frame until the GPU finished it.
 *
 * arm=FALSE              presentation blts: the pixels travel in the DMA
 *   buffer's transfer commands, not in a fenced GPU render.
 *
 * The fence protocol and immediate-context use run under presentLock; the
 * long GPU wait runs unlocked on the thread's own pooled event and fence
 * reference, so concurrent present threads overlap their waits. */
static void
tritonPresentFlushAndGate(PTRITON_DEVICE pD, BOOL arm, BOOL wait)
{
   if (!pD->pCtx1)
      return;
   if (!pD->presentLockInit) {
      /* Locks not up (only reachable before a successful CreateDevice):
       * bare flush rather than race the fence protocol. */
      ID3D11DeviceContext1_Flush(pD->pCtx1);
      return;
   }
   if (!arm) {
      /* Submit only.  presentLock still covers it: the immediate context is
       * shared with the gating presents, whose Signal and Flush are separate
       * remoted commands that a bare flush must not land between. */
      EnterCriticalSection(&pD->presentLock);
      ID3D11DeviceContext1_Flush(pD->pCtx1);
      LeaveCriticalSection(&pD->presentLock);
      return;
   }

   ID3D11Fence *pFence = NULL;
   HANDLE       hEvt   = NULL;
   UINT64       v      = 0;
   UINT64       done   = 0;   /* present fence's completed value at arm time */
   UINT64       paceTarget = 0;
   const double pt_t0  = TRITON_PT_NOW();
   double       pt_sig = pt_t0, pt_arm = pt_t0;

   EnterCriticalSection(&pD->presentLock);
   if (tritonPresentEnsureFenceLocked(pD)) {
      /* Signal a completion marker after the frame's draws; the Flush
       * submits both the draws and the signal.  SetEventOnCompletion arms
       * an event-ring fence (npt_event_arm -> submit_present_fence); the
       * host defers that fence's used-ring response until real GPU
       * completion, and the KMD signals the event from its completion DPC
       * -- so the present thread SLEEPS (no spin). */
      v = ++pD->presentFenceValue;
      ID3D11DeviceContext4_Signal(pD->pCtx4, pD->pPresentFence, v);
      ID3D11DeviceContext1_Flush(pD->pCtx1);
      pt_sig = TRITON_PT_NOW();
      done = ID3D11Fence_GetCompletedValue(pD->pPresentFence);
      if (done < v) {
         hEvt = tritonPresentPopEvent(pD);
         tritonPresentDrainEvent(hEvt);
         /* Bounded run-ahead (KMD-gated presents only): nothing else paces
          * an app whose flips complete under sync-interval-0 frame-drop
          * semantics -- render DMA fences retire on host-accept, so neither
          * dxgkrnl nor DXGI ever throttles it.  Unpaced, the app runs
          * arbitrarily far ahead of GPU execution and every queued flip's
          * token stays unretired, so the pending-flip queue never holds a
          * displayable frame.  The caller sleeps until the GPU is within
          * the pacing depth.
          *
          * The pacing wait must NOT use SetEventOnCompletion: every arm on
          * this fence submits a fresh monotonic flip-gate token, and a
          * pacing arm fires when an OLDER fence value completes -- a
          * higher-numbered token retiring early.  The KMD's retired-token
          * watermark is a max, so one early retire marks every in-flight
          * frame up to v as done and their flips scan out unrendered
          * buffers.  The wait below polls GetCompletedValue instead. */
         if (!wait) {
            /* Depth = the app's own flip-chain length when known, capped at
             * the DXGI-default frame latency (rationale at the constant). */
            UINT64 maxAhead = pD->lastFlipChainLength;
            if (maxAhead == 0 || maxAhead > TRITON_PRESENT_MAX_AHEAD)
               maxAhead = TRITON_PRESENT_MAX_AHEAD;
            if ((UINT)maxAhead != pD->lastPaceDepthLogged) {
               pD->lastPaceDepthLogged = (UINT)maxAhead;
               TR_LOG("present pacing: depth %u (flip chain %u, cap %u)",
                      (UINT)maxAhead, pD->lastFlipChainLength,
                      TRITON_PRESENT_MAX_AHEAD);
            }
            if (v > maxAhead && done < v - maxAhead)
               paceTarget = v - maxAhead;
         }
         if (hEvt && SUCCEEDED(ID3D11Fence_SetEventOnCompletion(pD->pPresentFence, v,
                                                                hEvt))) {
            /* Keep the fence alive across the unlocked wait: teardown
             * releases the device's reference under presentLock, but this
             * waiter holds its own. */
            pFence = pD->pPresentFence;
            ID3D11Fence_AddRef(pFence);
         } else {
            if (hEvt) {
               /* Armed even though the call failed: the transport arms
                * npt_event before issuing the remoted SetEventOnCompletion,
                * so a failure still leaves a pending arm on this handle. */
               tritonPresentPushEvent(pD, hEvt, TRUE /* armed */);
               hEvt = NULL;
            }
            static LONG feN;
            LONG n = InterlockedIncrement(&feN);
            if (n == 1 || (n & 255) == 0)
               TR_LOG("present-fence: arm failed #%ld (ungated)", n);
         }
      }
   } else {
      ID3D11DeviceContext1_Flush(pD->pCtx1);
   }
   LeaveCriticalSection(&pD->presentLock);
   pt_arm = TRITON_PT_NOW();

   if (!pFence) {
      /* Either the fence was already retired when we looked (the frame's GPU
       * work finished during Signal+Flush) or arming failed. Both mean this
       * present paid no GPU wait; counting them separately keeps the wait
       * average from being diluted by frames that never waited. */
      triton_pt_account(pt_sig - pt_t0, pt_arm - pt_sig, 0.0, TRUE);
      return;
   }

   if (!wait) {
      /* Scanout primary: the KMD orders the flip (see the function comment),
       * so this present does not wait for its OWN frame -- only, when the
       * pacing registration above armed, for frame v-N, which bounds the
       * run-ahead without serialising CPU against GPU.  Sliced waits rather
       * than one full-deadline sleep: the value is published on a different
       * path than the event, so a lost wake must cost a slice, not the
       * deadline.  Past the deadline, present anyway (device-lost bailout
       * semantics, same as the full gate below). */
      if (paceTarget) {
         const ULONGLONG deadline = GetTickCount64() + TRITON_PRESENT_DEADLINE_MS;
         /* 1ms slices: the completed value is published by the host at a 1ms
          * cadence, so finer polling buys nothing and coarser adds pacing
          * latency straight onto the frame time.  A HIGH_RESOLUTION waitable
          * timer, not Sleep(1): Sleep quantizes to the process timer
          * resolution (15.6ms unless the app raised it with timeBeginPeriod),
          * which would turn the slice into a frame-scale stall. */
         HANDLE hTimer = CreateWaitableTimerExW(NULL, NULL,
                                                CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                TIMER_ALL_ACCESS);
         while (ID3D11Fence_GetCompletedValue(pFence) < paceTarget) {
            if (GetTickCount64() >= deadline) {
               static LONG paN;
               LONG n = InterlockedIncrement(&paN);
               if (n == 1 || (n & 255) == 0)
                  TR_LOG("present-fence: pacing wait timed out #%ld (presenting anyway)", n);
               break;
            }
            if (hTimer) {
               LARGE_INTEGER due;
               due.QuadPart = -10000;   /* 1ms, relative */
               if (SetWaitableTimer(hTimer, &due, 0, NULL, NULL, FALSE)) {
                  WaitForSingleObject(hTimer, 2);
                  continue;
               }
            }
            Sleep(1);
         }
         if (hTimer)
            CloseHandle(hTimer);
      }
      ID3D11Fence_Release(pFence);
      EnterCriticalSection(&pD->presentLock);
      tritonPresentPushEvent(pD, hEvt, TRUE /* armed */);
      LeaveCriticalSection(&pD->presentLock);
      triton_pt_account(pt_sig - pt_t0, pt_arm - pt_sig,
                        TRITON_PT_NOW() - pt_arm, FALSE);
      return;
   }

   /* Block until the GPU retires the marker, so the consumer (DWM) never
    * samples a half-written frame.  Completion is judged solely by
    * GetCompletedValue (a per-fence feedback-slot read, safe unlocked); the
    * event wake is only a hint.  The 1s deadline is a device-lost bailout,
    * not a latency budget. */
   ULONGLONG deadline = GetTickCount64() + TRITON_PRESENT_DEADLINE_MS;
   unsigned rounds = 0;
   unsigned extensions = 0;
   BOOL hitDeadline = FALSE;
   /* Whether a SetEventOnCompletion registration is still outstanding on
    * hEvt.  Only a wait that actually consumed the signal clears it. */
   BOOL regOutstanding = TRUE;
   while (ID3D11Fence_GetCompletedValue(pFence) < v) {
      const ULONGLONG now = GetTickCount64();
      if (now >= deadline) {
         /* Extend while the device still reports healthy (see the deadline
          * constants in triton.h). */
         if (extensions < TRITON_PRESENT_MAX_EXTENSIONS &&
             ID3D11Device1_GetDeviceRemovedReason(pD->pDev1) == S_OK) {
            extensions++;
            deadline = now + TRITON_PRESENT_DEADLINE_MS;
            static LONG exN;
            LONG n = InterlockedIncrement(&exN);
            if (n == 1 || (n & 255) == 0)
               TR_LOG("present-fence: GPU wait extended #%ld (device healthy)", n);
            continue;
         }
         static LONG toN;
         LONG n = InterlockedIncrement(&toN);
         hitDeadline = TRUE;
         if (n == 1 || (n & 255) == 0)
            TR_LOG("present-fence: GPU wait timed out #%ld (presenting anyway)", n);
         break;
      }
      /* Re-arm before every sleep after the first.  SetEventOnCompletion is a
       * ONE-SHOT registration and hEvt is auto-reset, so one arm delivers one
       * signal.  The event and the fence's completed value travel different
       * host paths -- the value is published by the host ring worker, the
       * signal comes from the GPU-completion notify -- so a wake can land
       * before the new value is visible here, and sleeping again on a spent
       * registration would wait out the whole deadline.  Arming an
       * already-complete fence signals immediately, so this is also safe
       * against the value landing between the check and the arm. */
      if (rounds++) {
         EnterCriticalSection(&pD->presentLock);
         tritonPresentDrainEvent(hEvt);
         HRESULT rehr = ID3D11Fence_SetEventOnCompletion(pFence, v, hEvt);
         LeaveCriticalSection(&pD->presentLock);
         if (SUCCEEDED(rehr))
            regOutstanding = TRUE;
         if (FAILED(rehr)) {
            static LONG raN;
            LONG n = InterlockedIncrement(&raN);
            if (n == 1 || (n & 255) == 0)
               TR_LOG("present-fence: re-arm failed 0x%08lx #%ld", (unsigned long)rehr, n);
            break;   /* GetCompletedValue remains the source of truth */
         }
         if (ID3D11Fence_GetCompletedValue(pFence) >= v)
            break;
      }
      /* Wait the full remaining deadline rather than polling, so a dropped
       * completion surfaces as the deadline instead of being hidden behind
       * repeated short polls. */
      DWORD r = WaitForSingleObject(hEvt, (DWORD)(deadline - now));
      if (r == WAIT_OBJECT_0)
         regOutstanding = FALSE;   /* fired; this wait consumed the signal */
      if (r != WAIT_OBJECT_0 && r != WAIT_TIMEOUT) {
         static LONG weN;
         LONG n = InterlockedIncrement(&weN);
         if (n == 1 || (n & 255) == 0)
            TR_LOG("present-fence: wait failed 0x%lx #%ld (presenting anyway)", r, n);
         break;
      }
   }

   /* The deadline expired with the fence still short, so presenting now hands
    * the consumer a frame the GPU may still be writing.  Give one fresh
    * registration a bounded grace period first.  This is the only recourse
    * when a single full-deadline wait consumed every round, since the re-arm
    * above runs only from the second round on. */
   if (hitDeadline && ID3D11Fence_GetCompletedValue(pFence) < v) {
      EnterCriticalSection(&pD->presentLock);
      tritonPresentDrainEvent(hEvt);
      HRESULT ghr = ID3D11Fence_SetEventOnCompletion(pFence, v, hEvt);
      LeaveCriticalSection(&pD->presentLock);
      if (SUCCEEDED(ghr)) {
         regOutstanding = TRUE;
         if (WaitForSingleObject(hEvt, TRITON_PRESENT_GRACE_MS) == WAIT_OBJECT_0)
            regOutstanding = FALSE;
      }
      if (ID3D11Fence_GetCompletedValue(pFence) < v) {
         static LONG ugN;
         LONG n = InterlockedIncrement(&ugN);
         if (n == 1 || (n & 255) == 0)
            TR_LOG("present-fence: still short after grace #%ld (presenting un-gated)", n);
      }
   }

   ID3D11Fence_Release(pFence);

   EnterCriticalSection(&pD->presentLock);
   tritonPresentPushEvent(pD, hEvt, regOutstanding);
   LeaveCriticalSection(&pD->presentLock);

   triton_pt_account(pt_sig - pt_t0, pt_arm - pt_sig,
                     TRITON_PT_NOW() - pt_arm, FALSE);
}

/* Submit the flip/blit to dxgkrnl under kmCtxLock: DWM's present threads
 * all share pD->hKMContext, and only one thread may drive an HCONTEXT at a
 * time (see kmCtxLock in triton.h). */
static HRESULT
tritonPresentSubmitCb(PTRITON_DEVICE pD, DXGIDDICB_PRESENT *cb)
{
   if (!pD->presentLockInit)
      return pD->pfnPresentCb(pD->hRTDevice.handle, cb);
   const double t0 = TRITON_PT_NOW();
   EnterCriticalSection(&pD->kmCtxLock);
   HRESULT hr = pD->pfnPresentCb(pD->hRTDevice.handle, cb);
   LeaveCriticalSection(&pD->kmCtxLock);
   if (NPT_DEBUG(PRESENT_TIMING))
      InterlockedAdd64(&g_pt.cb_us, (LONG64)(TRITON_PT_NOW() - t0));
   return hr;
}

static HRESULT APIENTRY
tritonDxgiPresent(DXGI_DDI_ARG_PRESENT *pArgs)
{
   if (!pArgs) return E_INVALIDARG;

   /* The DXGI DDI uses UINT_PTR handles whose runtime value is the same
    * pDrvPrivate pointer the D3D11 DDI installs, cast to integer. */
   PTRITON_DEVICE   pD  = (PTRITON_DEVICE)(uintptr_t)pArgs->hDevice;
   PTRITON_RESOURCE src = (PTRITON_RESOURCE)(uintptr_t)pArgs->hSurfaceToPresent;
   PTRITON_RESOURCE dst = (PTRITON_RESOURCE)(uintptr_t)pArgs->hDstResource;

   if (!pD || !src) return E_INVALIDARG;
   /* Pin the device for the whole present (see presentInFlight in triton.h). */
   InterlockedIncrement(&pD->presentInFlight);
   /* Windowed flip-model devices never created a primary, so the kernel
    * context may not exist yet; create it here or the present is lost. */
   if (!pD->hKMContext)
      tritonPresentEnsureKernelContext(pD);
   if (!pD->hKMContext || !pD->pfnPresentCb || !src->hKMAllocation) {
      /* Presents must not be dropped silently in steady state; a burst
       * here means a device lost its kernel context or an allocation-less
       * surface is being presented. */
      static LONG dropCount;
      LONG n = InterlockedIncrement(&dropCount);
      if (n == 1 || (n & 1023) == 0)
         TR_LOG("Present dropped #%ld (kmalloc=0x%x %ux%u)",
                n, src->hKMAllocation, src->Width, src->Height);
      InterlockedDecrement(&pD->presentInFlight);
      return S_OK;
   }
   /* Nothing host-side flushes the producing context on present: the
    * scanout / DWM samples the dmabuf directly, so an unflushed frame
    * would sit in the host D3D command buffer indefinitely and the
    * consumer would composite stale or initial-zero contents.  Flush and
    * arm the GPU-completion fence; scanout primaries return without
    * waiting for the frame (the KMD gates the flip on the token this arm
    * submits), composited surfaces block until the GPU finishes (DWM's
    * sampling cannot be ordered any other way).  See
    * tritonPresentFlushAndGate. */
   tritonPresentFlushAndGate(pD, TRUE /* arm */, !src->IsPresentable /* wait */);

   /* DXGIDDICB_PRESENT (MSDN):
    *   hSrcAllocation  the rendered back buffer to present (a flip-primary
    *                   KM allocation in source-0).
    *   hDstAllocation  the blit destination, or 0 for a flip-style present.
    *   hContext        the device's kernel context that retires the present.
    *   pDXGIContext    runtime cookie threaded through unchanged. */
   DXGIDDICB_PRESENT cb;
   memset(&cb, 0, sizeof(cb));
   cb.hSrcAllocation = src->hKMAllocation;
   cb.hDstAllocation = dst ? dst->hKMAllocation : 0;
   cb.pDXGIContext   = pArgs->pDXGIContext;
   cb.hContext       = pD->hKMContext;
   HRESULT hr = tritonPresentSubmitCb(pD, &cb);
   InterlockedDecrement(&pD->presentInFlight);
   return hr;
}

static HRESULT APIENTRY
tritonDxgiGetGammaCaps(DXGI_DDI_ARG_GET_GAMMA_CONTROL_CAPS *pArgs)
{
   if (!pArgs || !pArgs->pGammaCapabilities) return E_INVALIDARG;
   /* Identity gamma: no scale/offset support and no control points, so the
    * runtime does not attempt gamma programming on this adapter. */
   DXGI_GAMMA_CONTROL_CAPABILITIES *c = pArgs->pGammaCapabilities;
   c->ScaleAndOffsetSupported = FALSE;
   c->MaxConvertedValue       = 1.0f;
   c->MinConvertedValue       = 0.0f;
   c->NumGammaControlPoints   = 0;
   return S_OK;
}

static HRESULT APIENTRY
tritonDxgiSetDisplayMode(DXGI_DDI_ARG_SETDISPLAYMODE *pArgs)
{
   if (!pArgs)
      return E_INVALIDARG;

   /* Hand the new primary to dxgkrnl.  Without this callback the
    * kernel never adopts the resource's allocation as the VidPn
    * source's primary, and every subsequent flip present completes
    * as a no-op without reaching the display miniport. */
   PTRITON_DEVICE   pD = (PTRITON_DEVICE)(uintptr_t)pArgs->hDevice;
   PTRITON_RESOURCE r  = (PTRITON_RESOURCE)(uintptr_t)pArgs->hResource;
   if (!pD || !r)
      return E_INVALIDARG;
   if (!r->hKMAllocation || !pD->KTCallbacks.pfnSetDisplayModeCb)
      return E_INVALIDARG;

   D3DDDICB_SETDISPLAYMODE cb;
   memset(&cb, 0, sizeof(cb));
   cb.hPrimaryAllocation = r->hKMAllocation;
   return pD->KTCallbacks.pfnSetDisplayModeCb(pD->hRTDevice.handle, &cb);
}

static HRESULT APIENTRY
tritonDxgiSetResourcePriority(DXGI_DDI_ARG_SETRESOURCEPRIORITY *pArgs)
{
   (void)pArgs;
   /* Priority is advisory; the KMD WDDM scheduler is free to ignore it. */
   return S_OK;
}

static HRESULT APIENTRY
tritonDxgiQueryResourceResidency(DXGI_DDI_ARG_QUERYRESOURCERESIDENCY *pArgs)
{
   if (!pArgs || !pArgs->pStatus) return E_INVALIDARG;
   /* All allocations report FULLY_RESIDENT: there is no eviction tracking on
    * the UMD side. */
   for (UINT i = 0; i < pArgs->Resources; i++)
      pArgs->pStatus[i] = DXGI_DDI_RESIDENCY_FULLY_RESIDENT;
   return S_OK;
}

static HRESULT APIENTRY
tritonDxgiRotateResourceIdentities(DXGI_DDI_ARG_ROTATE_RESOURCE_IDENTITIES *pArgs)
{
   if (!pArgs)
      return E_INVALIDARG;
   PTRITON_DEVICE pD = (PTRITON_DEVICE)(uintptr_t)pArgs->hDevice;
   const UINT n = pArgs->Resources;
   UINT i;
   if (!pD || n <= 1 || !pArgs->pResources)
      return S_OK;

   for (i = 0; i < n; ++i) {
      PTRITON_RESOURCE r = (PTRITON_RESOURCE)(uintptr_t)pArgs->pResources[i];
      if (!r || !r->pResource) {
         TR_LOG("RotateResourceIdentities: buffer %u has no backing; "
                "flip chain left unrotated", i);
         return S_OK;
      }
   }

   /* Record the rotation-set size for the present pacing depth (see
    * TRITON_PRESENT_MAX_AHEAD in triton.h) -- but only for PRESENTABLE
    * chains.  A device rotates several chains (the runtime also rotates
    * composited windowed chains, and DWM rotates one per output on its one
    * device); pacing governs only the KMD-gated scanout chain, which is
    * exactly the set whose members carry pPrimaryDesc.  Composited presents
    * take the full GPU wait and never consult this. */
   {
      PTRITON_RESOURCE r0 = (PTRITON_RESOURCE)(uintptr_t)pArgs->pResources[0];
      if (r0->IsPresentable && pD->lastFlipChainLength != n) {
         TR_LOG("RotateResourceIdentities: presentable flip chain length %u (%ux%u)",
                n, r0->Width, r0->Height);
         pD->lastFlipChainLength = n;
      }
   }

   /* Flip-model back buffers (windowed shared textures AND scanout
    * primaries) are DISTINCT host textures, each with its own blob-
    * backed KM allocation that consumers have imported.  Left-rotate
    * the backing tuples (resource[i] adopts resource[i+1]'s storage;
    * MSDN: "0 <= 1, 1 <= 2, etc."), keeping (host texture, allocation,
    * import rig) together so present tokens keep naming the storage
    * that was actually rendered. */
   {
      PTRITON_RESOURCE r0 = (PTRITON_RESOURCE)(uintptr_t)pArgs->pResources[0];
      ID3D11Resource *tmpRes    = r0->pResource;
      D3DKMT_HANDLE   tmpAlloc  = r0->hKMAllocation;
      BOOL            tmpShared = r0->IsShared;
      D3DKMT_HANDLE   tmpImpA   = r0->hImportAlloc;
      D3DKMT_HANDLE   tmpImpR   = r0->hImportResKmt;
      for (i = 0; i + 1 < n; ++i) {
         PTRITON_RESOURCE a = (PTRITON_RESOURCE)(uintptr_t)pArgs->pResources[i];
         PTRITON_RESOURCE b = (PTRITON_RESOURCE)(uintptr_t)pArgs->pResources[i + 1];
         a->pResource     = b->pResource;
         a->hKMAllocation = b->hKMAllocation;
         a->IsShared      = b->IsShared;
         a->hImportAlloc  = b->hImportAlloc;
         a->hImportResKmt = b->hImportResKmt;
      }
      {
         PTRITON_RESOURCE last =
            (PTRITON_RESOURCE)(uintptr_t)pArgs->pResources[n - 1];
         last->pResource     = tmpRes;
         last->hKMAllocation = tmpAlloc;
         last->IsShared      = tmpShared;
         last->hImportAlloc  = tmpImpA;
         last->hImportResKmt = tmpImpR;
      }
   }

   /* Host views wrapping these resources still target the pre-rotation
    * textures; rebuild them in place. */
   for (i = 0; i < n; ++i)
      tritonResourceRecreateViews(pD,
         (PTRITON_RESOURCE)(uintptr_t)pArgs->pResources[i]);

   /* Have the runtime replay current bindings through the DDI so the
    * host context picks up the recreated views ("the driver might be
    * required to reapply currently bound views" — MSDN). */
   {
      const D3D11DDI_CORELAYER_DEVICECALLBACKS *cb = pD->pUMCallbacks;
      if (cb) {
         if (cb->pfnStateVsSrvCb)
            cb->pfnStateVsSrvCb(pD->hRTCoreLayer, 0,
                                D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT);
         if (cb->pfnStateGsSrvCb)
            cb->pfnStateGsSrvCb(pD->hRTCoreLayer, 0,
                                D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT);
         if (cb->pfnStatePsSrvCb)
            cb->pfnStatePsSrvCb(pD->hRTCoreLayer, 0,
                                D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT);
         if (cb->pfnStateHsSrvCb)
            cb->pfnStateHsSrvCb(pD->hRTCoreLayer, 0,
                                D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT);
         if (cb->pfnStateDsSrvCb)
            cb->pfnStateDsSrvCb(pD->hRTCoreLayer, 0,
                                D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT);
         if (cb->pfnStateCsSrvCb)
            cb->pfnStateCsSrvCb(pD->hRTCoreLayer, 0,
                                D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT);
         if (cb->pfnStateCsUavCb)
            cb->pfnStateCsUavCb(pD->hRTCoreLayer, 0,
                                D3D11_PS_CS_UAV_REGISTER_COUNT);
         if (cb->pfnStateOmRenderTargetsCb)
            cb->pfnStateOmRenderTargetsCb(pD->hRTCoreLayer);
      }
   }
   return S_OK;
}

/* Lazily create the stretch/convert blit objects (see the pBlit* block in
 * triton.h).  Caller holds presentLock.  A failed init latches
 * blitInitFailed so it is attempted once; partially created objects are
 * released by DestroyDevice. */
static BOOL
tritonBlitEnsureLocked(PTRITON_DEVICE pD)
{
   if (pD->pBlitVS && pD->pBlitPS && pD->pBlitSampler && pD->pBlitCB)
      return TRUE;
   if (pD->blitInitFailed)
      return FALSE;
   pD->blitInitFailed = TRUE;

   HRESULT hr = ID3D11Device1_CreateVertexShader(pD->pDev1,
                                                 g_tritonBlitVS,
                                                 sizeof(g_tritonBlitVS),
                                                 NULL, &pD->pBlitVS);
   if (FAILED(hr) || !pD->pBlitVS) {
      TR_LOG("blit: CreateVertexShader failed 0x%08lx", hr);
      return FALSE;
   }
   hr = ID3D11Device1_CreatePixelShader(pD->pDev1,
                                        g_tritonBlitPS,
                                        sizeof(g_tritonBlitPS),
                                        NULL, &pD->pBlitPS);
   if (FAILED(hr) || !pD->pBlitPS) {
      TR_LOG("blit: CreatePixelShader failed 0x%08lx", hr);
      return FALSE;
   }
   D3D11_SAMPLER_DESC sd;
   memset(&sd, 0, sizeof(sd));
   sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
   sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
   sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
   sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
   sd.MaxLOD   = D3D11_FLOAT32_MAX;
   hr = ID3D11Device1_CreateSamplerState(pD->pDev1, &sd, &pD->pBlitSampler);
   if (FAILED(hr) || !pD->pBlitSampler) {
      TR_LOG("blit: CreateSamplerState failed 0x%08lx", hr);
      return FALSE;
   }
   D3D11_BUFFER_DESC bd;
   memset(&bd, 0, sizeof(bd));
   bd.ByteWidth = 16;
   bd.Usage     = D3D11_USAGE_DEFAULT;
   bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
   hr = ID3D11Device1_CreateBuffer(pD->pDev1, &bd, NULL, &pD->pBlitCB);
   if (FAILED(hr) || !pD->pBlitCB) {
      TR_LOG("blit: CreateBuffer(cb) failed 0x%08lx", hr);
      return FALSE;
   }
   pD->blitInitFailed = FALSE;
   TR_LOG("blit: stretch/convert pipeline created");
   return TRUE;
}

/* GPU stretch/convert blit: draw the source box into the dst rect through a
 * fullscreen triangle.  CopySubresourceRegion cannot stretch, and the host
 * D3D stack silently copies ZEROS across format families -- the runtime's
 * blt-model exclusive-fullscreen present (R8G8B8A8 back buffer ->
 * B8G8R8A8 primary, Convert|Stretch) therefore left the primary
 * zero-filled and the display scanned out pure black.
 *
 * Pipeline state is driven directly on the shared immediate context under
 * presentLock; afterwards every touched binding is dirtied through the
 * runtime's pfnState*Cb callbacks so the runtime replays the app's state
 * through the ordinary DDI before its next draw (the same mechanism
 * RotateResourceIdentities uses above).  Predication is deliberately left
 * alone: there is no dirty callback for it, and a present-path blt under
 * active predication does not occur in practice. */
static HRESULT
tritonDxgiBltStretch(PTRITON_DEVICE pD,
                     PTRITON_RESOURCE dst, UINT DstSubresource,
                     UINT DstLeft, UINT DstTop, UINT DstRight, UINT DstBottom,
                     PTRITON_RESOURCE src, UINT SrcSubresource,
                     const D3D11_BOX *box)
{
   ID3D11ShaderResourceView *srv = NULL;
   ID3D11RenderTargetView   *rtv = NULL;
   HRESULT hr;

   const UINT srcMips  = src->MipLevels ? src->MipLevels : 1;
   const UINT srcMip   = SrcSubresource % srcMips;
   const UINT srcSlice = SrcSubresource / srcMips;
   const UINT dstMips  = dst->MipLevels ? dst->MipLevels : 1;
   const UINT dstMip   = DstSubresource % dstMips;
   const UINT dstSlice = DstSubresource / dstMips;

   /* Subresource 0 (every present blt): NULL view descs, so the views
    * inherit the host texture's real format -- primaries are created with
    * a host-normalised format that can differ from dst->Format. */
   if (SrcSubresource == 0) {
      hr = ID3D11Device1_CreateShaderResourceView(pD->pDev1, src->pResource,
                                                  NULL, &srv);
   } else {
      D3D11_SHADER_RESOURCE_VIEW_DESC sdc;
      memset(&sdc, 0, sizeof(sdc));
      sdc.Format = src->Format;
      if (src->ArraySize > 1) {
         sdc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
         sdc.Texture2DArray.MostDetailedMip  = srcMip;
         sdc.Texture2DArray.MipLevels        = 1;
         sdc.Texture2DArray.FirstArraySlice  = srcSlice;
         sdc.Texture2DArray.ArraySize        = 1;
      } else {
         sdc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
         sdc.Texture2D.MostDetailedMip = srcMip;
         sdc.Texture2D.MipLevels       = 1;
      }
      hr = ID3D11Device1_CreateShaderResourceView(pD->pDev1, src->pResource,
                                                  &sdc, &srv);
   }
   if (FAILED(hr) || !srv) {
      static LONG svN;
      LONG n = InterlockedIncrement(&svN);
      if (n == 1 || (n & 255) == 0)
         TR_LOG("blit: CreateShaderResourceView failed 0x%08lx #%ld (fmt=%d bind=0x%x)",
                hr, n, (int)src->Format, src->BindFlags);
      return FAILED(hr) ? hr : E_FAIL;
   }

   if (DstSubresource == 0) {
      hr = ID3D11Device1_CreateRenderTargetView(pD->pDev1, dst->pResource,
                                                NULL, &rtv);
   } else {
      D3D11_RENDER_TARGET_VIEW_DESC rdc;
      memset(&rdc, 0, sizeof(rdc));
      rdc.Format = dst->Format;
      if (dst->ArraySize > 1) {
         rdc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
         rdc.Texture2DArray.MipSlice        = dstMip;
         rdc.Texture2DArray.FirstArraySlice = dstSlice;
         rdc.Texture2DArray.ArraySize       = 1;
      } else {
         rdc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
         rdc.Texture2D.MipSlice = dstMip;
      }
      hr = ID3D11Device1_CreateRenderTargetView(pD->pDev1, dst->pResource,
                                                &rdc, &rtv);
   }
   if (FAILED(hr) || !rtv) {
      static LONG rvN;
      LONG n = InterlockedIncrement(&rvN);
      if (n == 1 || (n & 255) == 0)
         TR_LOG("blit: CreateRenderTargetView failed 0x%08lx #%ld (fmt=%d bind=0x%x)",
                hr, n, (int)dst->Format, dst->BindFlags);
      ID3D11ShaderResourceView_Release(srv);
      return FAILED(hr) ? hr : E_FAIL;
   }

   /* Quad UV [0,1]^2 -> source box in normalised source-texture coords. */
   UINT sw = src->Width  >> srcMip;  if (!sw) sw = 1;
   UINT sh = src->Height >> srcMip;  if (!sh) sh = 1;
   FLOAT consts[4];
   consts[0] = (FLOAT)(box->right - box->left) / (FLOAT)sw;
   consts[1] = (FLOAT)(box->bottom - box->top) / (FLOAT)sh;
   consts[2] = (FLOAT)box->left / (FLOAT)sw;
   consts[3] = (FLOAT)box->top  / (FLOAT)sh;

   tritonPresentLock(pD);
   if (!tritonBlitEnsureLocked(pD)) {
      tritonPresentUnlock(pD);
      ID3D11ShaderResourceView_Release(srv);
      ID3D11RenderTargetView_Release(rtv);
      return E_FAIL;
   }
   ID3D11DeviceContext1 *c = pD->pCtx1;
   ID3D11DeviceContext1_UpdateSubresource(c, (ID3D11Resource *)pD->pBlitCB,
                                          0, NULL, consts, 0, 0);
   ID3D11DeviceContext1_OMSetRenderTargets(c, 1, &rtv, NULL);
   ID3D11DeviceContext1_OMSetBlendState(c, NULL, NULL, 0xFFFFFFFFu);
   ID3D11DeviceContext1_OMSetDepthStencilState(c, NULL, 0);
   ID3D11DeviceContext1_RSSetState(c, NULL);
   {
      D3D11_VIEWPORT vp;
      vp.TopLeftX = (FLOAT)DstLeft;
      vp.TopLeftY = (FLOAT)DstTop;
      vp.Width    = (FLOAT)(DstRight - DstLeft);
      vp.Height   = (FLOAT)(DstBottom - DstTop);
      vp.MinDepth = 0.0f;
      vp.MaxDepth = 1.0f;
      ID3D11DeviceContext1_RSSetViewports(c, 1, &vp);
   }
   ID3D11DeviceContext1_IASetInputLayout(c, NULL);
   ID3D11DeviceContext1_IASetPrimitiveTopology(c, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
   ID3D11DeviceContext1_VSSetShader(c, pD->pBlitVS, NULL, 0);
   ID3D11DeviceContext1_PSSetShader(c, pD->pBlitPS, NULL, 0);
   ID3D11DeviceContext1_GSSetShader(c, NULL, NULL, 0);
   ID3D11DeviceContext1_HSSetShader(c, NULL, NULL, 0);
   ID3D11DeviceContext1_DSSetShader(c, NULL, NULL, 0);
   ID3D11DeviceContext1_PSSetShaderResources(c, 0, 1, &srv);
   ID3D11DeviceContext1_PSSetSamplers(c, 0, 1, &pD->pBlitSampler);
   ID3D11DeviceContext1_PSSetConstantBuffers(c, 0, 1, &pD->pBlitCB);
   ID3D11DeviceContext1_Draw(c, 3, 0);
   {
      /* Unbind our views before the runtime replays app state, so the
       * primary is never simultaneously bound as PS resource and render
       * target across the replay. */
      ID3D11ShaderResourceView *nullSrv = NULL;
      ID3D11RenderTargetView   *nullRtv = NULL;
      ID3D11DeviceContext1_PSSetShaderResources(c, 0, 1, &nullSrv);
      ID3D11DeviceContext1_OMSetRenderTargets(c, 1, &nullRtv, NULL);
   }
   tritonPresentUnlock(pD);

   ID3D11ShaderResourceView_Release(srv);
   ID3D11RenderTargetView_Release(rtv);

   /* Have the runtime replay the app's bindings for everything touched. */
   {
      const D3D11DDI_CORELAYER_DEVICECALLBACKS *cb = pD->pUMCallbacks;
      if (cb) {
         if (cb->pfnStateOmRenderTargetsCb)
            cb->pfnStateOmRenderTargetsCb(pD->hRTCoreLayer);
         if (cb->pfnStateOmBlendStateCb)
            cb->pfnStateOmBlendStateCb(pD->hRTCoreLayer);
         if (cb->pfnStateOmDepthStateCb)
            cb->pfnStateOmDepthStateCb(pD->hRTCoreLayer);
         if (cb->pfnStateRsRastStateCb)
            cb->pfnStateRsRastStateCb(pD->hRTCoreLayer);
         if (cb->pfnStateRsViewportsCb)
            cb->pfnStateRsViewportsCb(pD->hRTCoreLayer);
         if (cb->pfnStateIaInputLayoutCb)
            cb->pfnStateIaInputLayoutCb(pD->hRTCoreLayer);
         if (cb->pfnStateIaPrimitiveTopologyCb)
            cb->pfnStateIaPrimitiveTopologyCb(pD->hRTCoreLayer);
         if (cb->pfnStateVsShaderCb)
            cb->pfnStateVsShaderCb(pD->hRTCoreLayer);
         if (cb->pfnStatePsShaderCb)
            cb->pfnStatePsShaderCb(pD->hRTCoreLayer);
         if (cb->pfnStateGsShaderCb)
            cb->pfnStateGsShaderCb(pD->hRTCoreLayer);
         if (cb->pfnStateHsShaderCb)
            cb->pfnStateHsShaderCb(pD->hRTCoreLayer);
         if (cb->pfnStateDsShaderCb)
            cb->pfnStateDsShaderCb(pD->hRTCoreLayer);
         if (cb->pfnStatePsSrvCb)
            cb->pfnStatePsSrvCb(pD->hRTCoreLayer, 0, 1);
         if (cb->pfnStatePsSamplerCb)
            cb->pfnStatePsSamplerCb(pD->hRTCoreLayer, 0, 1);
         if (cb->pfnStatePsConstBufCb)
            cb->pfnStatePsConstBufCb(pD->hRTCoreLayer, 0, 1);
      }
   }
   return S_OK;
}

/* Shared body for Blt and Blt1.  Blt has no explicit source rectangle
 * (the whole source subresource is the implied src); Blt1 passes one. */
static HRESULT
tritonDxgiBltCommon(DXGI_DDI_HDEVICE hDevice,
                    DXGI_DDI_HRESOURCE hDst, UINT DstSubresource,
                    UINT DstLeft, UINT DstTop, UINT DstRight, UINT DstBottom,
                    DXGI_DDI_HRESOURCE hSrc, UINT SrcSubresource,
                    UINT SrcLeft, UINT SrcTop, UINT SrcRight, UINT SrcBottom,
                    DXGI_DDI_ARG_BLT_FLAGS Flags)
{
   PTRITON_DEVICE   pD  = (PTRITON_DEVICE)(uintptr_t)hDevice;
   PTRITON_RESOURCE dst = (PTRITON_RESOURCE)(uintptr_t)hDst;
   PTRITON_RESOURCE src = (PTRITON_RESOURCE)(uintptr_t)hSrc;
   if (!pD || !pD->pCtx1 || !dst || !src || !dst->pResource || !src->pResource)
      return E_INVALIDARG;

   /* Pin the device for the whole blt (see presentInFlight in triton.h): this
    * runs on the DXGI DDI alongside the present threads, and drives the same
    * immediate context DestroyDevice releases. */
   InterlockedIncrement(&pD->presentInFlight);

   /* MSAA resolve blt. */
   if (Flags.Resolve) {
      tritonPresentLock(pD);
      ID3D11DeviceContext1_ResolveSubresource(pD->pCtx1,
                                              dst->pResource, DstSubresource,
                                              src->pResource, SrcSubresource,
                                              (DXGI_FORMAT)dst->Format);
      tritonPresentUnlock(pD);
      if (Flags.Present)
         tritonPresentFlushAndGate(pD, FALSE /* arm */, FALSE /* wait */);
      InterlockedDecrement(&pD->presentInFlight);
      return S_OK;
   }

   const UINT dstW = (DstRight  > DstLeft) ? DstRight  - DstLeft : 0;
   const UINT dstH = (DstBottom > DstTop)  ? DstBottom - DstTop  : 0;

   D3D11_BOX box;
   box.left   = SrcLeft;
   box.top    = SrcTop;
   box.front  = 0;
   box.right  = SrcRight;
   box.bottom = SrcBottom;
   box.back   = 1;

   /* Stretching and cross-format blts cannot ride CopySubresourceRegion:
    * it has no stretch semantics, and the host silently copies ZEROS
    * between different format families (the blt-model exclusive-fullscreen
    * present is exactly such a blt -- see tritonDxgiBltStretch).  Route
    * them through the draw-based blit; same-format 1:1 blts (DWM's
    * redirection copies) keep the plain copy below. */
   {
      const UINT bw = box.right  - box.left;
      const UINT bh = box.bottom - box.top;
      const BOOL needStretch = Flags.Stretch || Flags.Convert ||
                               (dstW && bw != dstW) ||
                               (dstH && bh != dstH) ||
                               src->Format != dst->Format;
      if (needStretch && dstW && dstH && bw && bh &&
          src->SampleDesc.Count <= 1 && dst->SampleDesc.Count <= 1) {
         HRESULT shr = tritonDxgiBltStretch(pD, dst, DstSubresource,
                                            DstLeft, DstTop,
                                            DstRight, DstBottom,
                                            src, SrcSubresource, &box);
         if (SUCCEEDED(shr)) {
            if (Flags.Present)
               tritonPresentFlushAndGate(pD, FALSE /* arm */, FALSE /* wait */);
            InterlockedDecrement(&pD->presentInFlight);
            return S_OK;
         }
         /* Fall through to the clamped copy as a last resort. */
      }
   }

   /* Clamp the copied extent to the smaller of src box and dst rect (only
    * reachable when the draw-based blit is unavailable). */
   if (dstW && (box.right - box.left) != dstW) {
      TR_STUB("DxgiBlt stretch width (copy clamped)");
      if (box.right - box.left > dstW)
         box.right = box.left + dstW;
   }
   if (dstH && (box.bottom - box.top) != dstH) {
      TR_STUB("DxgiBlt stretch height (copy clamped)");
      if (box.bottom - box.top > dstH)
         box.bottom = box.top + dstH;
   }

   tritonPresentLock(pD);
   ID3D11DeviceContext1_CopySubresourceRegion(pD->pCtx1,
                                              dst->pResource, DstSubresource,
                                              DstLeft, DstTop, 0,
                                              src->pResource, SrcSubresource,
                                              &box);
   tritonPresentUnlock(pD);
   /* A presentation blt is the present: nothing later submits it. */
   if (Flags.Present)
      tritonPresentFlushAndGate(pD, FALSE /* arm */, FALSE /* wait */);
   InterlockedDecrement(&pD->presentInFlight);
   return S_OK;
}

static HRESULT APIENTRY
tritonDxgiBlt(DXGI_DDI_ARG_BLT *pArgs)
{
   if (!pArgs)
      return E_INVALIDARG;
   PTRITON_RESOURCE src = (PTRITON_RESOURCE)(uintptr_t)pArgs->hSrcResource;
   if (!src)
      return E_INVALIDARG;
   return tritonDxgiBltCommon(pArgs->hDevice,
                              pArgs->hDstResource, pArgs->DstSubresource,
                              pArgs->DstLeft, pArgs->DstTop,
                              pArgs->DstRight, pArgs->DstBottom,
                              pArgs->hSrcResource, pArgs->SrcSubresource,
                              0, 0, src->Width, src->Height,
                              pArgs->Flags);
}

static HRESULT APIENTRY
tritonDxgiResolveSharedResource(DXGI_DDI_ARG_RESOLVESHAREDRESOURCE *pArgs)
{
   (void)pArgs;
   /* No work: cross-process shared textures are resolved to host storage
    * when opened (tritonSharedBridge), so there is nothing to resolve here. */
   return S_OK;
}

static HRESULT APIENTRY
tritonDxgiBlt1(DXGI_DDI_ARG_BLT1 *pArgs)
{
   if (!pArgs)
      return E_INVALIDARG;
   return tritonDxgiBltCommon(pArgs->hDevice,
                              pArgs->hDstResource, pArgs->DstSubresource,
                              pArgs->DstLeft, pArgs->DstTop,
                              pArgs->DstRight, pArgs->DstBottom,
                              pArgs->hSrcResource, pArgs->SrcSubresource,
                              pArgs->SrcLeft, pArgs->SrcTop,
                              pArgs->SrcRight, pArgs->SrcBottom,
                              pArgs->Flags);
}

static HRESULT APIENTRY
tritonDxgiOfferResources(DXGI_DDI_ARG_OFFERRESOURCES *pArgs)
{
   (void)pArgs;
   /* DXGI 1.2 budget-aware paging hint; advisory. */
   return S_OK;
}

static HRESULT APIENTRY
tritonDxgiReclaimResources(DXGI_DDI_ARG_RECLAIMRESOURCES *pArgs)
{
   if (!pArgs) return E_INVALIDARG;
   /* No resources are ever discarded, so every reclaim succeeds. */
   if (pArgs->pDiscarded)
      for (UINT i = 0; i < pArgs->Resources; i++)
         pArgs->pDiscarded[i] = FALSE;
   return S_OK;
}

static HRESULT APIENTRY
tritonDxgiPresent1(DXGI_DDI_ARG_PRESENT1 *pArgs)
{
   if (!pArgs)
      return E_INVALIDARG;

   /* DXGI_DDI_ARG_PRESENT1 (MSDN):
    *   phSurfacesToPresent[0..SurfacesToPresent-1]  source surfaces (the
    *     first is always the primary plane; secondary planes are stereo).
    *   hDstResource                                  blit destination or 0. */
   PTRITON_DEVICE pD = (PTRITON_DEVICE)(uintptr_t)pArgs->hDevice;
   if (!pD || !pArgs->SurfacesToPresent || !pArgs->phSurfacesToPresent)
      return E_INVALIDARG;
   PTRITON_RESOURCE src =
      (PTRITON_RESOURCE)(uintptr_t)pArgs->phSurfacesToPresent[0].hSurface;
   PTRITON_RESOURCE dst = (PTRITON_RESOURCE)(uintptr_t)pArgs->hDstResource;
   if (!src) return E_INVALIDARG;
   InterlockedIncrement(&pD->presentInFlight);   /* see tritonDxgiPresent */
   if (!pD->hKMContext)
      tritonPresentEnsureKernelContext(pD);
   if (!pD->hKMContext || !pD->pfnPresentCb || !src->hKMAllocation) {
      static LONG dropCount1;
      LONG n = InterlockedIncrement(&dropCount1);
      if (n == 1 || (n & 1023) == 0)
         TR_LOG("Present1 dropped #%ld (kmalloc=0x%x %ux%u)",
                n, src->hKMAllocation, src->Width, src->Height);
      InterlockedDecrement(&pD->presentInFlight);
      return S_OK;
   }
   /* See tritonDxgiPresent: the frame must be submitted or it never reaches
    * the shared dmabuf.  Arm always -- the KMD's flip gate is keyed on the
    * token the arm submits; wait only for composited (non-primary)
    * surfaces. */
   tritonPresentFlushAndGate(pD, TRUE /* arm */, !src->IsPresentable /* wait */);

   DXGIDDICB_PRESENT cb;
   memset(&cb, 0, sizeof(cb));
   cb.hSrcAllocation = src->hKMAllocation;
   cb.hDstAllocation = dst ? dst->hKMAllocation : 0;
   cb.pDXGIContext   = pArgs->pDXGIContext;
   cb.hContext       = pD->hKMContext;
   HRESULT hr = tritonPresentSubmitCb(pD, &cb);
   InterlockedDecrement(&pD->presentInFlight);
   return hr;
}

static HRESULT APIENTRY
tritonDxgiCheckPresentDurationSupport(DXGI_DDI_ARG_CHECKPRESENTDURATIONSUPPORT *pArgs)
{
   if (!pArgs) return E_INVALIDARG;
   /* No custom present-duration support — runtime falls back to vblank. */
   pArgs->ClosestSmallerDuration = 0;
   pArgs->ClosestLargerDuration  = 0;
   return S_OK;
}

/* ---------- Installer ---------- */

/* Cover the lowest-tier base function table fields (DXGI_DDI_BASE_FUNCTIONS).
 * Each later variant strictly extends this prefix layout, so a tier-1
 * fill is the foundation for tier-2 / tier-3 / tier-4 etc. */
static void
triton_fill_base(DXGI_DDI_BASE_FUNCTIONS *p)
{
   p->pfnPresent                = tritonDxgiPresent;
   p->pfnGetGammaCaps           = tritonDxgiGetGammaCaps;
   p->pfnSetDisplayMode         = tritonDxgiSetDisplayMode;
   p->pfnSetResourcePriority    = tritonDxgiSetResourcePriority;
   p->pfnQueryResourceResidency = tritonDxgiQueryResourceResidency;
   p->pfnRotateResourceIdentities = tritonDxgiRotateResourceIdentities;
   p->pfnBlt                    = tritonDxgiBlt;
}

static void
triton_fill_1_1(DXGI1_1_DDI_BASE_FUNCTIONS *p)
{
   triton_fill_base((DXGI_DDI_BASE_FUNCTIONS *)p);
   p->pfnResolveSharedResource = tritonDxgiResolveSharedResource;
}

static void
triton_fill_1_2(DXGI1_2_DDI_BASE_FUNCTIONS *p)
{
   triton_fill_1_1((DXGI1_1_DDI_BASE_FUNCTIONS *)p);
   p->pfnBlt1                            = tritonDxgiBlt1;
   p->pfnOfferResources                  = tritonDxgiOfferResources;
   p->pfnReclaimResources                = tritonDxgiReclaimResources;
   /* Multiplane-overlay entries stay NULL: MPO is unsupported, and a
    * NULL pfnGetMultiplaneOverlayCaps is how the runtime detects that.
    * DWM then presents through the ordinary flip path (pfnPresent1 +
    * pfnRotateResourceIdentities). */
   p->pfnGetMultiplaneOverlayCaps        = NULL;
   p->pfnGetMultiplaneOverlayFilterRange = NULL;
   p->pfnCheckMultiplaneOverlaySupport   = NULL;
   p->pfnPresentMultiplaneOverlay        = NULL;
}

static void
triton_fill_1_3(DXGI1_3_DDI_BASE_FUNCTIONS *p)
{
   p->pfnPresent                = tritonDxgiPresent;
   p->pfnGetGammaCaps           = tritonDxgiGetGammaCaps;
   p->pfnSetDisplayMode         = tritonDxgiSetDisplayMode;
   p->pfnSetResourcePriority    = tritonDxgiSetResourcePriority;
   p->pfnQueryResourceResidency = tritonDxgiQueryResourceResidency;
   p->pfnRotateResourceIdentities = tritonDxgiRotateResourceIdentities;
   p->pfnBlt                    = tritonDxgiBlt;
   p->pfnResolveSharedResource  = tritonDxgiResolveSharedResource;
   p->pfnBlt1                   = tritonDxgiBlt1;
   p->pfnOfferResources         = tritonDxgiOfferResources;
   p->pfnReclaimResources       = tritonDxgiReclaimResources;
   /* Multiplane-overlay entries stay NULL (unsupported); see
    * triton_fill_1_2. */
   p->pfnGetMultiplaneOverlayCaps      = NULL;
   p->pfnGetMultiplaneOverlayGroupCaps = NULL;
   p->pfnReserved1                     = NULL;
   p->pfnPresentMultiplaneOverlay      = NULL;
   p->pfnReserved2                     = NULL;
   p->pfnPresent1                      = tritonDxgiPresent1;
   p->pfnCheckPresentDurationSupport   = tritonDxgiCheckPresentDurationSupport;
}

void
tritonInstallDXGIFuncs(PTRITON_DEVICE pD,
                       const D3D10DDIARG_CREATEDEVICE *pArgs)
{
   if (!pArgs || !pArgs->DXGIBaseDDI.pDXGIDDIBaseFunctions) {
      TR_LOG("InstallDXGIFuncs: no DXGI base table pointer (interface 0x%08x)",
             pArgs ? pArgs->Interface : 0);
      return;
   }

   /* Record the runtime's present callback. tritonDxgiPresent invokes it
    * (with the kernel context) to submit the flip/blit to dxgkrnl, which
    * drives the KMD's scanout. The base callbacks pointer is an IN field
    * the runtime asks us to retain (dxgiddi.h DXGI_DDI_BASE_ARGS). */
   if (pD && pArgs->DXGIBaseDDI.pDXGIBaseCallbacks)
      pD->pfnPresentCb = pArgs->DXGIBaseDDI.pDXGIBaseCallbacks->pfnPresentCb;

   /* Pick the tier matching the runtime-requested DDI interface.  The
    * SDK overlays the version-specific pointers as a union inside
    * DXGI_DDI_BASE_ARGS, so the chosen field aliases the same storage. */
   if (IS_DXGI1_3_BASE_FUNCTIONS(pArgs->Interface, pArgs->Version)) {
      triton_fill_1_3(pArgs->DXGIBaseDDI.pDXGIDDIBaseFunctions4);
      TR_LOG("InstallDXGIFuncs: tier DXGI1_3");
   } else if (IS_DXGI1_2_BASE_FUNCTIONS(pArgs->Interface, pArgs->Version)) {
      triton_fill_1_2(pArgs->DXGIBaseDDI.pDXGIDDIBaseFunctions3);
      TR_LOG("InstallDXGIFuncs: tier DXGI1_2");
   } else {
      /* DXGI 1.0/1.1 callers get the base table; the 1.1-only
       * pfnResolveSharedResource is a no-op, so nothing is lost. */
      triton_fill_base(pArgs->DXGIBaseDDI.pDXGIDDIBaseFunctions);
      TR_LOG("InstallDXGIFuncs: tier DXGI1_0");
   }
}
