/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Guest-fabricated IDXGISwapChain for the Wine path.  The host owns no
 * swapchains: presentable surfaces are exportable host textures bound to
 * virtio-gpu blob resources, presented through the X11 DRI3/Present unixlib
 * backend.
 *
 * The swapchain never touches the tier default vtbls: it swaps in its own
 * fully-populated vtbl at creation, so no wire method can ever see a
 * guest-fab object id (an unregistered id is a fatal host decode error).
 * Model:
 *
 *   backbuffer      one ordinary host texture (app's format, optimal
 *                   tiling); GetBuffer maps every index to it.
 *   present pool    NPT_SC_PRESENT_IMAGES host textures with
 *                   MISC_SHARED|LINEAR_EXPORT storage, exported once
 *                   via the SHARED transport, bound to guest blobs
 *                   whose dmabufs feed the X11 WSI.
 *   Present         CopyResource(pool[i], backbuffer) + Flush on the
 *                   DC/SC ring, then wsi_present(i).  Pool reuse is
 *                   gated on X IDLE_NOTIFY release counts drained by
 *                   a per-swapchain thread.
 */

#include "npt_swapchain.h"

#include "npt_com.h"
#include "npt_device.h"
#include "npt_env.h"
#include "npt_profile.h"
#include "npt_renderer.h"
#include "npt_ring.h"
#include "npt_shared_texture.h"

#include <string.h>

#include "neptune-protocol/npt_protocol_client_idxgiswapchain.h"
#include "neptune-protocol/npt_protocol_guest_idxgiswapchain.h"

#if defined(__WINE__)

#include "npt_event.h"
#include "npt_renderer_wine_common.h"
#include "nptunix/npt_unixlib.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "neptune-protocol/npt_protocol_client_id3d11device.h"
#include "neptune-protocol/npt_protocol_client_id3d11devicecontext.h"
#include "neptune-protocol/npt_protocol_client_id3d11texture2d.h"
#include "neptune-protocol/npt_protocol_client_idxgiadapter.h"
#include "neptune-protocol/npt_protocol_client_idxgidevice.h"
#include "neptune-protocol/npt_protocol_client_idxgifactory.h"
#include "neptune-protocol/npt_protocol_client_idxgioutput.h"
#include "neptune-protocol/npt_protocol_guest_id3d11devicecontext.h"
#include "neptune-protocol/npt_protocol_guest_id3d11fence.h"

/* DXGI constants the protocol types header doesn't carry. */
#define NPT_DXGI_PRESENT_TEST            0x00000001u
#define NPT_DXGI_STATUS_OCCLUDED         ((HRESULT)0x087A0001)
#define NPT_DXGI_USAGE_SHADER_INPUT      0x00000010u
#define NPT_DXGI_USAGE_RENDER_TARGET_OUTPUT 0x00000020u
#define NPT_DXGI_USAGE_UNORDERED_ACCESS  0x00000040u

/* Present-pool depth: enough for one on-screen + one queued + one
 * being written.  Must fit the unixlib's image table. */
#define NPT_SC_PRESENT_IMAGES 3u
static_assert(NPT_SC_PRESENT_IMAGES <= NPT_WSI_MAX_IMAGES,
              "present pool exceeds unixlib image table");

/* CPU-runahead cap: ring of dup'd GPU-done fence fds; a Present that
 * finds the ring full first waits on the oldest.  Mirrors DXGI's
 * MaximumFrameLatency semantics (default 3). */
#define NPT_SC_PRESENT_LATENCY 3u

/* Async WSI worker queue.  poll(fence) + xcb_present run on a per-
 * swapchain worker so the app's Present thread never blocks on the
 * host GPU; the latency ring above is the pacing bound. */
#define NPT_SC_WSI_QUEUE_SIZE 8u

/* Reuse-gate budget: how long Present waits for the compositor to
 * release the next pool image before proceeding anyway (a minimized
 * window never idles). */
#define NPT_SC_REUSE_WAIT_MS 100u

/* Frame-latency waitable-object semaphore max count (per DXGI). */
#define NPT_SC_MAX_SWAP_CHAIN_BUFFERS 16u

struct gsc_present_entry {
   uint32_t image_index;
   int      wait_fence_fd;   /* -1 = unfenced; ownership -> worker */
   uint64_t token;           /* event token to release after present */
};

struct npt_guest_swapchain {
   struct npt_com_base *com;

   /* Guest-authoritative description. */
   DXGI_SWAP_CHAIN_DESC1 desc;
   DXGI_SWAP_CHAIN_FULLSCREEN_DESC fs_desc;
   HWND hwnd;
   BOOL fullscreen;
   UINT max_frame_latency;
   DXGI_RGBA background;
   UINT present_count;

   /* Frame-latency waitable object (non-NULL only when the app created
    * the swapchain with FRAME_LATENCY_WAITABLE_OBJECT).  Released once
    * per Present so the app's wait paces it to max_frame_latency. */
   HANDLE frame_latency_sem;

   /* Held wire wrappers (one public ref each) + their host ids for
    * liveness-guarded release at teardown: the device-level force-
    * destroy drain frees wrappers in arbitrary order, and a release
    * on an already-freed wrapper is UAF. */
   void *factory;                /* creating IDXGIFactory*, may be NULL */
   uint64_t factory_id;
   void *device;                 /* ID3D11Device* */
   uint64_t device_id;
   void *dc;                     /* ID3D11DeviceContext* */
   uint64_t dc_id;
   void *backbuffer;             /* ID3D11Texture2D* */
   uint64_t backbuffer_id;

   /* Present pool (see file comment). */
   bool wsi_initialized;
   bool wsi_failed;
   void *present_tex[NPT_SC_PRESENT_IMAGES];
   uint64_t present_tex_id[NPT_SC_PRESENT_IMAGES];
   uint32_t next_image;
   /* ++ at present, -- per X IDLE_NOTIFY (drain thread). */
   _Atomic uint32_t image_inflight[NPT_SC_PRESENT_IMAGES];
   /* Auto-reset event the drain thread signals on each release so the
    * Present reuse-gate blocks instead of polling. */
   HANDLE image_release_evt;
   /* Consecutive presents whose image never idled (hidden/minimized
    * window): drives the DXGI_STATUS_OCCLUDED return so the app
    * throttles instead of spinning. */
   uint32_t occluded_streak;

   mtx_t present_mutex;

   /* GPU-done fencing: an ID3D11Fence signaled on the DC after each
    * present copy; per-Present event tokens turn its completion into
    * a guest sync_file (see npt_event_arm_token_fd). */
   bool no_gpu_fence;
   void *dc4;                    /* ID3D11DeviceContext4* */
   uint64_t dc4_id;
   void *fence;                  /* ID3D11Fence* */
   uint64_t fence_id;
   uint64_t fence_value;

   /* Latency ring (dup'd GPU-done fds). */
   int latency_fences[NPT_SC_PRESENT_LATENCY];
   uint32_t latency_count;

   /* Async WSI worker. */
   mtx_t wsi_mutex;
   cnd_t wsi_cond;
   struct gsc_present_entry wsi_queue[NPT_SC_WSI_QUEUE_SIZE];
   uint32_t wsi_head, wsi_tail;
   _Atomic int wsi_thread_exit;
   bool wsi_thread_started;
   HANDLE wsi_thread;

   /* X IDLE_NOTIFY release-count drain. */
   HANDLE drain_thread;
   _Atomic int drain_exit;
   bool drain_started;

   /* IDXGIOutput bookkeeping (guest-fab outputs never reach the wire). */
   mtx_t output_lock;
   IDXGIOutput *fullscreen_target;
   IDXGIOutput *containing_output;

   /* Fullscreen window management, done entirely guest-side.
    * The guest-fab swapchain owns a real HWND (s->hwnd), so all
    * styling/placement/mode-switching is done here with plain Win32 calls;
    * s->fullscreen is the authoritative windowed/fullscreen state.
    * mode_change_in_progress rejects a transition that overlaps another
    * (DXGI_STATUS_MODE_CHANGE_IN_PROGRESS); it does not guard these fields
    * against concurrent readers. */
   _Atomic int mode_change_in_progress;
   HMONITOR    fs_monitor;          /* real Win32 monitor while fullscreen */
   LONG        fs_saved_style;
   LONG        fs_saved_exstyle;
   RECT        fs_saved_rect;
   bool        fs_did_modeset;      /* we changed the display mode */
};

static inline struct npt_guest_swapchain *
gsc(void *self)
{
   return ((struct npt_com_base *)self)->aux;
}

static const GUID *const gsc_qi_chain[] = {
   &NPT_IID_IUnknown,
   &NPT_IID_IDXGIObject,
   &NPT_IID_IDXGIDeviceSubObject,
   &NPT_IID_IDXGISwapChain,
   &NPT_IID_IDXGISwapChain1,
   &NPT_IID_IDXGISwapChain2,
   &NPT_IID_IDXGISwapChain3,
   &NPT_IID_IDXGISwapChain4,
   NULL,
};

/* ------------------------------------------------------------------ */
/* held-wrapper helpers                                                */
/* ------------------------------------------------------------------ */

static void
gsc_release_held(struct npt_device *dev, void **slot, uint64_t *id)
{
   void *w = *slot;
   uint64_t host_id = *id;
   *slot = NULL;
   *id = 0;
   if (!w)
      return;
   if (!dev || !host_id ||
       npt_device_wrapper_cache_is_live(dev, host_id, w))
      npt_com_default_release(w);
}

/* ------------------------------------------------------------------ */
/* IDLE_NOTIFY drain thread                                            */
/* ------------------------------------------------------------------ */

static DWORD WINAPI
gsc_drain_thread(LPVOID arg)
{
   struct npt_guest_swapchain *s = arg;
   struct npt_renderer *renderer = s->com->base.device->renderer;
   while (!atomic_load(&s->drain_exit)) {
      uint32_t released[NPT_WSI_MAX_IMAGES] = { 0 };
      int rc = npt_renderer_wsi_drain(renderer, /*timeout_ms=*/100, released);
      if (rc == -1) {
         Sleep(100);
         continue;
      }
      if (rc < 0)
         break;
      bool any = false;
      for (uint32_t i = 0; i < NPT_SC_PRESENT_IMAGES; i++) {
         for (uint32_t k = 0; k < released[i]; k++) {
            uint32_t cur = atomic_load(&s->image_inflight[i]);
            /* Releases for a pre-resize generation can outnumber the
             * current inflight count; clamp at zero. */
            while (cur &&
                   !atomic_compare_exchange_weak(&s->image_inflight[i],
                                                 &cur, cur - 1))
               ;
            any = true;
         }
      }
      /* Wake a Present blocked in the reuse gate.  Auto-reset: an
       * unconsumed signal latches, so a release that lands between the
       * gate's inflight check and its wait is not lost. */
      if (any && s->image_release_evt)
         SetEvent(s->image_release_evt);
   }
   return 0;
}

static bool
gsc_drain_start(struct npt_guest_swapchain *s)
{
   if (s->drain_started)
      return true;
   atomic_store(&s->drain_exit, 0);
   s->drain_thread = CreateThread(NULL, 0, gsc_drain_thread, s, 0, NULL);
   if (!s->drain_thread) {
      npt_log("guest swapchain: drain CreateThread failed err=%lu",
              GetLastError());
      return false;
   }
   s->drain_started = true;
   return true;
}

static void
gsc_drain_stop(struct npt_guest_swapchain *s)
{
   if (!s->drain_started)
      return;
   atomic_store(&s->drain_exit, 1);
   WaitForSingleObject(s->drain_thread, INFINITE);
   CloseHandle(s->drain_thread);
   s->drain_thread = NULL;
   s->drain_started = false;
}

/* ------------------------------------------------------------------ */
/* WSI worker thread                                                   */
/* ------------------------------------------------------------------ */

/* Process-unique single-use event tokens.  Bit 63 keeps them disjoint
 * from real Wine HANDLE values in the host's token registry. */
static _Atomic uint64_t gsc_next_token_low = 1;

static uint64_t
gsc_make_token(void)
{
   return npt_com_make_guest_id(NPT_GUEST_KIND_SWAPCHAIN,
                                atomic_fetch_add(&gsc_next_token_low, 1));
}

/* Advance the frame-latency waitable object by one completed frame.
 * Called exactly once per Present (on the worker once the present
 * retires, or synchronously when the frame is dropped) so a waiting
 * app never deadlocks.  Saturates silently at the semaphore's max. */
static void
gsc_frame_latency_release(struct npt_guest_swapchain *s)
{
   if (s->frame_latency_sem)
      ReleaseSemaphore(s->frame_latency_sem, 1, NULL);
}

static DWORD WINAPI
gsc_wsi_thread(LPVOID arg)
{
   struct npt_guest_swapchain *s = arg;
   struct npt_device *dev = s->com->base.device;
   struct npt_renderer *renderer = dev->renderer;
   for (;;) {
      struct gsc_present_entry entry;
      mtx_lock(&s->wsi_mutex);
      while (s->wsi_head == s->wsi_tail &&
             !atomic_load_explicit(&s->wsi_thread_exit, memory_order_acquire))
         cnd_wait(&s->wsi_cond, &s->wsi_mutex);
      if (atomic_load_explicit(&s->wsi_thread_exit, memory_order_acquire)) {
         /* Drop remaining entries without presenting. */
         while (s->wsi_head != s->wsi_tail) {
            struct gsc_present_entry *e =
               &s->wsi_queue[s->wsi_head % NPT_SC_WSI_QUEUE_SIZE];
            if (e->wait_fence_fd >= 0)
               npt_wine_unixlib_close(e->wait_fence_fd);
            if (e->token)
               npt_event_release_token(dev, e->token);
            s->wsi_head++;
            gsc_frame_latency_release(s);
         }
         mtx_unlock(&s->wsi_mutex);
         break;
      }
      entry = s->wsi_queue[s->wsi_head % NPT_SC_WSI_QUEUE_SIZE];
      s->wsi_head++;
      /* Wake a producer waiting on a full queue. */
      cnd_broadcast(&s->wsi_cond);
      mtx_unlock(&s->wsi_mutex);

      /* Owned by entry now; the backend closes/consumes the fd. */
      npt_renderer_wsi_present(renderer, entry.image_index,
                               entry.wait_fence_fd);
      if (entry.token)
         npt_event_release_token(dev, entry.token);
      gsc_frame_latency_release(s);
   }
   return 0;
}

static bool
gsc_wsi_start(struct npt_guest_swapchain *s)
{
   if (s->wsi_thread_started)
      return true;
   atomic_store_explicit(&s->wsi_thread_exit, 0, memory_order_release);
   s->wsi_head = 0;
   s->wsi_tail = 0;
   s->wsi_thread = CreateThread(NULL, 0, gsc_wsi_thread, s, 0, NULL);
   if (!s->wsi_thread) {
      npt_log("guest swapchain: WSI worker CreateThread failed err=%lu",
              GetLastError());
      return false;
   }
   s->wsi_thread_started = true;
   return true;
}

static void
gsc_wsi_stop(struct npt_guest_swapchain *s)
{
   if (!s->wsi_thread_started)
      return;
   mtx_lock(&s->wsi_mutex);
   atomic_store_explicit(&s->wsi_thread_exit, 1, memory_order_release);
   cnd_broadcast(&s->wsi_cond);
   mtx_unlock(&s->wsi_mutex);
   WaitForSingleObject(s->wsi_thread, INFINITE);
   CloseHandle(s->wsi_thread);
   s->wsi_thread = NULL;
   s->wsi_thread_started = false;
}

static void
gsc_wsi_push(struct npt_guest_swapchain *s, uint32_t image_index,
             int wait_fence_fd, uint64_t token)
{
   mtx_lock(&s->wsi_mutex);
   /* The latency ring caps in-flight presents below the queue size;
    * blocking here is defensive (resize transitions). */
   while ((s->wsi_tail - s->wsi_head) >= NPT_SC_WSI_QUEUE_SIZE)
      cnd_wait(&s->wsi_cond, &s->wsi_mutex);
   s->wsi_queue[s->wsi_tail % NPT_SC_WSI_QUEUE_SIZE] =
      (struct gsc_present_entry){
         .image_index = image_index,
         .wait_fence_fd = wait_fence_fd,
         .token = token,
      };
   s->wsi_tail++;
   cnd_signal(&s->wsi_cond);
   mtx_unlock(&s->wsi_mutex);
}

/* ------------------------------------------------------------------ */
/* latency ring                                                        */
/* ------------------------------------------------------------------ */

static void
gsc_backpressure_wait(struct npt_guest_swapchain *s)
{
   UINT cap = s->max_frame_latency;
   if (cap > NPT_SC_PRESENT_LATENCY)
      cap = NPT_SC_PRESENT_LATENCY;
   if (s->latency_count < cap)
      return;
   int oldest = s->latency_fences[0];
   if (oldest >= 0) {
      if (npt_unixlib_ensure_init() == 0) {
         struct npt_unix_wait_fd_params p = {
            .fd = oldest, .timeout_ms = 1000, .result = -1,
         };
         npt_wine_unix_call(npt_unix_wait_fd, &p);
      }
      npt_wine_unixlib_close(oldest);
   }
   for (uint32_t i = 0; i + 1 < NPT_SC_PRESENT_LATENCY; i++)
      s->latency_fences[i] = s->latency_fences[i + 1];
   s->latency_fences[NPT_SC_PRESENT_LATENCY - 1] = -1;
   s->latency_count--;
}

static void
gsc_backpressure_record(struct npt_guest_swapchain *s, int fence_fd)
{
   if (fence_fd < 0)
      return;
   if (s->latency_count >= NPT_SC_PRESENT_LATENCY) {
      npt_wine_unixlib_close(fence_fd);
      return;
   }
   s->latency_fences[s->latency_count++] = fence_fd;
}

static void
gsc_backpressure_flush(struct npt_guest_swapchain *s)
{
   for (uint32_t i = 0; i < NPT_SC_PRESENT_LATENCY; i++) {
      if (s->latency_fences[i] >= 0) {
         npt_wine_unixlib_close(s->latency_fences[i]);
         s->latency_fences[i] = -1;
      }
   }
   s->latency_count = 0;
}

/* ------------------------------------------------------------------ */
/* present pool + WSI                                                  */
/* ------------------------------------------------------------------ */

static void
gsc_teardown_wsi(struct npt_guest_swapchain *s)
{
   struct npt_device *dev = s->com->base.device;
   gsc_wsi_stop(s);
   gsc_drain_stop(s);
   gsc_backpressure_flush(s);
   if (s->wsi_initialized)
      npt_renderer_wsi_destroy(dev->renderer);
   s->wsi_initialized = false;
   s->wsi_failed = false;
   for (uint32_t i = 0; i < NPT_SC_PRESENT_IMAGES; i++) {
      gsc_release_held(dev, &s->present_tex[i], &s->present_tex_id[i]);
      atomic_store(&s->image_inflight[i], 0);
   }
   s->next_image = 0;
}

/* First Present: build the exportable pool and hand its dmabufs to
 * the WSI backend.  The window must exist by now (the app rendered a
 * frame), which is why this is not done at creation time. */
static void
gsc_ensure_wsi(struct npt_guest_swapchain *s)
{
   struct npt_device *dev = s->com->base.device;
   struct npt_renderer *renderer = dev->renderer;

   if (!renderer->ops.wsi_init || !renderer->ops.create_host_blob) {
      npt_log("guest swapchain: transport has no WSI backend; "
              "presents will be dropped");
      s->wsi_failed = true;
      return;
   }

   const uint32_t fmt = npt_shared_texture_host_format(s->desc.Format);
   int fds[NPT_SC_PRESENT_IMAGES];
   uint32_t strides[NPT_SC_PRESENT_IMAGES];
   for (uint32_t i = 0; i < NPT_SC_PRESENT_IMAGES; i++)
      fds[i] = -1;

   for (uint32_t i = 0; i < NPT_SC_PRESENT_IMAGES; i++) {
      void *tex = npt_shared_texture_create_exportable(
         s->device, s->desc.Width, s->desc.Height, fmt);
      if (!tex)
         goto fail;
      s->present_tex[i] = tex;
      s->present_tex_id[i] = npt_com_self_id(tex);

      struct npt_shared_texture_desc exp;
      memset(&exp, 0, sizeof(exp));
      if (!npt_shared_texture_export_blob(tex, &exp))
         goto fail;
      if (exp.plane_count != 1 || exp.modifier != 0 /* LINEAR */)
         npt_log("guest swapchain: unexpected export (planes=%u "
                 "modifier=0x%llx)", exp.plane_count,
                 (unsigned long long)exp.modifier);
      strides[i] = (uint32_t)exp.planes[0].pitch;

      /* Page-align defensively; the blob create is page-granular. */
      const uint64_t size = (exp.allocation_size + 4095u) & ~4095ull;
      fds[i] = npt_renderer_create_host_blob(renderer, exp.blob_id, size);
      if (fds[i] < 0) {
         npt_log("guest swapchain: blob claim failed for image %u", i);
         goto fail;
      }
   }

   if (!npt_renderer_wsi_init(renderer, NPT_SC_PRESENT_IMAGES, fds, strides,
                              npt_shared_texture_virgl_format(fmt),
                              s->desc.Width, s->desc.Height,
                              (uint64_t)(uintptr_t)s->hwnd)) {
      npt_log("guest swapchain: WSI init failed (hwnd=%p %ux%u)",
              (void *)s->hwnd, s->desc.Width, s->desc.Height);
      goto fail;
   }

   /* The unixlib dup'd the fds; ours are done. */
   for (uint32_t i = 0; i < NPT_SC_PRESENT_IMAGES; i++) {
      npt_wine_unixlib_close(fds[i]);
      fds[i] = -1;
   }

   s->wsi_initialized = true;
   if (!gsc_drain_start(s) || !gsc_wsi_start(s)) {
      /* Without both workers a Present would enqueue frames nothing
       * consumes and eventually block forever on the full queue.  Drop
       * back to the no-WSI path (presents silently succeed) instead. */
      npt_log("guest swapchain: present worker startup failed; "
              "presents will be dropped");
      gsc_teardown_wsi(s);
      s->wsi_failed = true;
   }
   return;

fail:
   for (uint32_t i = 0; i < NPT_SC_PRESENT_IMAGES; i++) {
      if (fds[i] >= 0)
         npt_wine_unixlib_close(fds[i]);
      gsc_release_held(dev, &s->present_tex[i], &s->present_tex_id[i]);
   }
   s->wsi_failed = true;
}

/* ------------------------------------------------------------------ */
/* IUnknown / IDXGIObject / IDXGIDeviceSubObject                       */
/* ------------------------------------------------------------------ */

static HRESULT NPT_STDMETHODCALLTYPE
gsc_QueryInterface(void *self, REFIID riid, void **ppvObject)
{
   HRESULT hr = npt_com_default_query_interface_chain(
      self, riid, ppvObject, gsc_qi_chain);
   return hr == NPT_S_OK ? hr : NPT_E_NOINTERFACE;
}

static HRESULT NPT_STDMETHODCALLTYPE
gsc_SetPrivateData(void *self, const GUID *Name, UINT DataSize,
                   const void *pData)
{
   (void)self; (void)Name; (void)DataSize; (void)pData;
   return NPT_S_OK;
}

static HRESULT NPT_STDMETHODCALLTYPE
gsc_SetPrivateDataInterface(void *self, const GUID *Name,
                            const IUnknown *pUnknown)
{
   (void)self; (void)Name; (void)pUnknown;
   return NPT_S_OK;
}

static HRESULT NPT_STDMETHODCALLTYPE
gsc_GetPrivateData(void *self, const GUID *Name, UINT *pDataSize,
                   void *pData)
{
   (void)self; (void)Name; (void)pData;
   if (pDataSize)
      *pDataSize = 0;
   return NPT_DXGI_ERROR_NOT_FOUND;
}

/* Generic "QI a held wrapper for the caller's riid" helper: the held
 * ref stays ours; QI hands the caller its own. */
static HRESULT
gsc_qi_held(void *wrapper, const IID *riid, void **out)
{
   if (!out)
      return NPT_E_POINTER;
   *out = NULL;
   if (!wrapper)
      return NPT_E_FAIL;
   const struct npt_idxgiswapchain_client_vtbl *v =
      (const void *)((struct npt_com_base *)wrapper)->lpVtbl;
   return v->QueryInterface(wrapper, riid, out);
}

static HRESULT NPT_STDMETHODCALLTYPE
gsc_GetParent(void *self, const IID *riid, void **ppParent)
{
   struct npt_guest_swapchain *s = gsc(self);
   if (s->factory)
      return gsc_qi_held(s->factory, riid, ppParent);

   /* No creating factory recorded (D3D11CreateDeviceAndSwapChain):
    * walk device -> IDXGIDevice -> adapter -> factory. */
   if (!ppParent)
      return NPT_E_POINTER;
   *ppParent = NULL;

   IDXGIDevice *dxgi_dev = NULL;
   HRESULT hr = gsc_qi_held(s->device, &NPT_IID_IDXGIDevice,
                            (void **)&dxgi_dev);
   if (NPT_FAILED(hr) || !dxgi_dev)
      return NPT_E_FAIL;
   const struct npt_idxgidevice_client_vtbl *dv =
      (const void *)((struct npt_com_base *)dxgi_dev)->lpVtbl;

   IDXGIAdapter *adp = NULL;
   hr = dv->GetAdapter(dxgi_dev, &adp);
   if (NPT_FAILED(hr) || !adp) {
      dv->Release(dxgi_dev);
      return NPT_E_FAIL;
   }
   const struct npt_idxgiadapter_client_vtbl *av =
      (const void *)((struct npt_com_base *)adp)->lpVtbl;
   hr = av->GetParent(adp, riid, ppParent);
   av->Release(adp);
   dv->Release(dxgi_dev);
   return hr;
}

static HRESULT NPT_STDMETHODCALLTYPE
gsc_GetDevice(void *self, const IID *riid, void **ppDevice)
{
   return gsc_qi_held(gsc(self)->device, riid, ppDevice);
}

/* ------------------------------------------------------------------ */
/* Present                                                             */
/* ------------------------------------------------------------------ */

static HRESULT
gsc_present_common(void *self, UINT SyncInterval, UINT Flags)
{
   struct npt_guest_swapchain *s = gsc(self);

   if (Flags & NPT_DXGI_PRESENT_TEST)
      return (s->wsi_failed || s->occluded_streak >= 4)
                ? NPT_DXGI_STATUS_OCCLUDED : NPT_S_OK;

   /* Frame-time instrumentation (NPT_DEBUG=present_timing).  t0=entry,
    * t1=after backpressure, t2=after the host present work is issued,
    * t3=after the WSI flip is queued. */
   const bool prof = NPT_DEBUG(PRESENT_TIMING);
   uint64_t t0 = prof ? npt_profile_now_ns() : 0, t1 = t0, t2 = t0, t3 = t0;

   mtx_lock(&s->present_mutex);

   if (!s->wsi_initialized && !s->wsi_failed)
      gsc_ensure_wsi(s);
   if (s->wsi_failed) {
      s->present_count++;
      mtx_unlock(&s->present_mutex);
      /* No worker will complete this frame; release the latency object
       * synchronously so a waiting app is not stalled. */
      gsc_frame_latency_release(s);
      return NPT_S_OK;
   }

   struct npt_device *dev = s->com->base.device;

   /* CPU-runahead backpressure before issuing more work: blocks the
    * caller until the host GPU has progressed.  Combined with the
    * async WSI worker this bounds runahead at max_frame_latency
    * frames without the app thread ever polling a fence itself. */
   gsc_backpressure_wait(s);
   if (prof) t1 = npt_profile_now_ns();

   const uint32_t i = s->next_image;

   /* Reuse gate: block (bounded) until the drain thread reports X has
    * released this image, waking on its event rather than polling.  A
    * minimized window never idles; proceed anyway after the cap so the
    * app keeps running. */
   if (atomic_load(&s->image_inflight[i]) != 0) {
      ULONGLONG deadline = GetTickCount64() + NPT_SC_REUSE_WAIT_MS;
      while (atomic_load(&s->image_inflight[i]) != 0) {
         ULONGLONG now = GetTickCount64();
         if (now >= deadline)
            break;
         DWORD wait_ms = (DWORD)(deadline - now);
         if (s->image_release_evt)
            WaitForSingleObject(s->image_release_evt, wait_ms);
         else
            Sleep(wait_ms < 5 ? wait_ms : 5);
      }
   }
   bool idled = atomic_load(&s->image_inflight[i]) == 0;
   if (!idled) {
      /* The image is still on-screen after the wait: the compositor
       * isn't consuming frames (hidden/minimized).  After a few such
       * presents report OCCLUDED so the app backs off; still present
       * (some apps only re-render on OCCLUDED clear). */
      if (s->occluded_streak < 4)
         s->occluded_streak++;
   } else {
      s->occluded_streak = 0;
   }

   struct npt_ring *ring = npt_com_self_ring(s->dc);
   npt_async_ID3D11DeviceContext_CopyResource(
      ring, s->dc_id,
      (ID3D11Resource *)s->present_tex[i],
      (ID3D11Resource *)s->backbuffer);

   /* GPU-done fence: signal the fence on the DC after the copy, then
    * arm a fresh event token on its completion value and turn that
    * into a guest sync_file the WSI worker feeds to xcb_present as the
    * wait_fence.  Without it the X server can read the dmabuf while
    * the host renderer is still writing (cross-VM dmabuf has no
    * implicit reservation sync). */
   int wait_fence_fd = -1;
   uint64_t token = 0;
   uint64_t value = 0;
   if (!s->no_gpu_fence) {
      value = ++s->fence_value;
      /* Signal on the DC ring, after the copy, before the Flush, so
       * the fence signals only once the copy's GPU work retires. */
      npt_async_ID3D11DeviceContext4_Signal(ring, s->dc4_id,
                                            (ID3D11Fence *)s->fence, value);
   }

   /* Nothing else flushes on present, and the X server samples the
    * dmabuf directly: an unflushed frame would sit in the host D3D
    * command buffer forever.  Flush also submits the fence signal. */
   npt_async_ID3D11DeviceContext_Flush(ring, s->dc_id);

   if (!s->no_gpu_fence) {
      token = gsc_make_token();
      /* REGISTER + ARM (synchronous) must land before the host decodes
       * SetEventOnCompletion below: its hEvent -> eventfd substitution
       * only fires once the token's proxy exists (else the raw token
       * passes through as a bogus HANDLE). */
      wait_fence_fd = npt_event_arm_token_fd(dev, token);
      if (wait_fence_fd < 0) {
         token = 0;  /* arm failed: self-released */
      } else {
         /* SetEventOnCompletion on the same ring the arm used (method
          * ring): fence completion -> SetEvent(proxy eventfd) ->
          * ring_idx retire -> guest sync_file signals.  Order vs the
          * DC-ring Signal is immaterial (D3D fences fire immediately
          * if already past the value). */
         npt_async_ID3D11Fence_SetEventOnCompletion(
            npt_device_method_ring(dev), s->fence_id, value,
            (HANDLE)(uintptr_t)token);
         /* Dup for the latency ring before the worker consumes the
          * original; submit_present_fence must not run twice per
          * frame, so dup rather than re-arm. */
         gsc_backpressure_record(s, npt_wine_unixlib_dup(wait_fence_fd));
      }
   }
   if (prof) t2 = npt_profile_now_ns();

   if (NPT_DEBUG(PRESENT_ORDER))
      npt_profile_log_present_order(i, i, NPT_SC_PRESENT_IMAGES,
                                    wait_fence_fd);

   atomic_fetch_add(&s->image_inflight[i], 1);
   gsc_wsi_push(s, i, wait_fence_fd, token);

   s->next_image = (i + 1) % NPT_SC_PRESENT_IMAGES;
   s->present_count++;

   npt_profile_present_marker();
   if (prof) {
      t3 = npt_profile_now_ns();
      npt_profile_log_present_timing((unsigned)SyncInterval, t0, t1, t2, t3);
   }
   const bool occluded = s->occluded_streak >= 4;
   mtx_unlock(&s->present_mutex);
   return occluded ? NPT_DXGI_STATUS_OCCLUDED : NPT_S_OK;
}

static HRESULT NPT_STDMETHODCALLTYPE
gsc_Present(void *self, UINT SyncInterval, UINT Flags)
{
   return gsc_present_common(self, SyncInterval, Flags);
}

static HRESULT NPT_STDMETHODCALLTYPE
gsc_Present1(void *self, UINT SyncInterval, UINT PresentFlags,
             const DXGI_PRESENT_PARAMETERS *pPresentParameters)
{
   if (!pPresentParameters)
      return NPT_DXGI_ERROR_INVALID_CALL;
   /* Dirty/scroll rects are an optimization hint; full-frame present
    * is always correct. */
   return gsc_present_common(self, SyncInterval, PresentFlags);
}

/* ------------------------------------------------------------------ */
/* buffers / desc                                                      */
/* ------------------------------------------------------------------ */

static HRESULT NPT_STDMETHODCALLTYPE
gsc_GetBuffer(void *self, UINT Buffer, const IID *riid, void **ppSurface)
{
   struct npt_guest_swapchain *s = gsc(self);
   if (!ppSurface)
      return NPT_E_POINTER;
   *ppSurface = NULL;
   if (Buffer >= s->desc.BufferCount)
      return NPT_DXGI_ERROR_INVALID_CALL;
   /* Single-writable-buffer illusion: every index maps to the one
    * backbuffer.  Exact for DISCARD/FLIP_DISCARD (only index 0 is
    * legal); FLIP_SEQUENTIAL readback of previous frames is not
    * supported yet. */
   return gsc_qi_held(s->backbuffer, riid, ppSurface);
}

static HRESULT
gsc_create_backbuffer(struct npt_guest_swapchain *s)
{
   D3D11_TEXTURE2D_DESC d;
   memset(&d, 0, sizeof(d));
   d.Width = s->desc.Width;
   d.Height = s->desc.Height;
   d.MipLevels = 1;
   d.ArraySize = 1;
   d.Format = (DXGI_FORMAT)s->desc.Format;
   d.SampleDesc = s->desc.SampleDesc;
   d.Usage = D3D11_USAGE_DEFAULT;
   d.BindFlags = D3D11_BIND_RENDER_TARGET;
   if (s->desc.BufferUsage & NPT_DXGI_USAGE_SHADER_INPUT)
      d.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
   if (s->desc.BufferUsage & NPT_DXGI_USAGE_UNORDERED_ACCESS)
      d.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;

   const struct npt_id3d11device_client_vtbl *v =
      (const void *)((struct npt_com_base *)s->device)->lpVtbl;
   ID3D11Texture2D *tex = NULL;
   HRESULT hr = v->CreateTexture2D(s->device, &d, NULL, &tex);
   if (NPT_FAILED(hr) || !tex) {
      npt_log("guest swapchain: backbuffer %ux%u fmt=%u failed hr=0x%x",
              d.Width, d.Height, d.Format, hr);
      return NPT_FAILED(hr) ? hr : NPT_E_FAIL;
   }
   s->backbuffer = tex;
   s->backbuffer_id = npt_com_self_id(tex);
   return NPT_S_OK;
}

static void
gsc_query_window_size(struct npt_guest_swapchain *s,
                      UINT *width, UINT *height)
{
   RECT rect;
   if (s->hwnd && GetClientRect(s->hwnd, &rect)) {
      *width = (UINT)(rect.right - rect.left);
      *height = (UINT)(rect.bottom - rect.top);
   }
   if (!*width)
      *width = 1;
   if (!*height)
      *height = 1;
}

static HRESULT
gsc_resize_common(void *self, UINT BufferCount, UINT Width, UINT Height,
                  DXGI_FORMAT NewFormat, UINT SwapChainFlags)
{
   struct npt_guest_swapchain *s = gsc(self);
   struct npt_device *dev = s->com->base.device;
   (void)SwapChainFlags;

   mtx_lock(&s->present_mutex);

   /* Old pool (and its X pixmaps) die here; the new pool is built
    * lazily on the next Present. */
   gsc_teardown_wsi(s);

   if (BufferCount)
      s->desc.BufferCount = BufferCount;
   if (NewFormat != 0 /* DXGI_FORMAT_UNKNOWN */)
      s->desc.Format = NewFormat;
   if (Width && Height) {
      s->desc.Width = Width;
      s->desc.Height = Height;
   } else {
      UINT w = Width, h = Height;
      gsc_query_window_size(s, &w, &h);
      s->desc.Width = w;
      s->desc.Height = h;
   }

   /* DXGI requires the app to have dropped its buffer refs; be
    * lenient (refcounting keeps a straggler alive) but recreate. */
   gsc_release_held(dev, &s->backbuffer, &s->backbuffer_id);
   HRESULT hr = gsc_create_backbuffer(s);

   mtx_unlock(&s->present_mutex);
   return hr;
}

static HRESULT NPT_STDMETHODCALLTYPE
gsc_ResizeBuffers(void *self, UINT BufferCount, UINT Width, UINT Height,
                  DXGI_FORMAT NewFormat, UINT SwapChainFlags)
{
   return gsc_resize_common(self, BufferCount, Width, Height, NewFormat,
                            SwapChainFlags);
}

static HRESULT NPT_STDMETHODCALLTYPE
gsc_ResizeBuffers1(void *self, UINT BufferCount, UINT Width, UINT Height,
                   DXGI_FORMAT Format, UINT SwapChainFlags,
                   const UINT *pCreationNodeMask, IUnknown **ppPresentQueue)
{
   (void)pCreationNodeMask; (void)ppPresentQueue;
   return gsc_resize_common(self, BufferCount, Width, Height, Format,
                            SwapChainFlags);
}

/* -------------------------------------------------------------------------
 * Guest-side fullscreen window management.  The guest-fab swapchain owns the
 * app's OutputWindow, so the window is driven here with plain Win32 calls.
 * Real mode switches are gated on DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
 * without it we do borderless-fullscreen at the current desktop mode (the
 * common path, and the only realizable one in the composited model).
 * ------------------------------------------------------------------------- */
static bool
gsc_desktop_rect(HMONITOR mon, RECT *out)
{
   MONITORINFOEXW mi;
   memset(&mi, 0, sizeof(mi));
   mi.cbSize = sizeof(mi);
   if (!GetMonitorInfoW(mon, (MONITORINFO *)&mi))
      return false;
   *out = mi.rcMonitor;
   return true;
}

/* Best-effort ChangeDisplaySettings.
 * Returns true if the mode is now (or already was) w x h. */
static bool
gsc_set_display_mode(HMONITOR mon, uint32_t w, uint32_t h, uint32_t refresh)
{
   MONITORINFOEXW mi;
   memset(&mi, 0, sizeof(mi));
   mi.cbSize = sizeof(mi);
   if (!GetMonitorInfoW(mon, (MONITORINFO *)&mi))
      return false;

   DEVMODEW cur;
   memset(&cur, 0, sizeof(cur));
   cur.dmSize = sizeof(cur);
   if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &cur) &&
       cur.dmPelsWidth == w && cur.dmPelsHeight == h &&
       (!refresh || cur.dmDisplayFrequency == refresh))
      return true;

   DEVMODEW dm;
   memset(&dm, 0, sizeof(dm));
   dm.dmSize        = sizeof(dm);
   dm.dmFields      = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
   dm.dmPelsWidth   = w;
   dm.dmPelsHeight  = h;
   dm.dmBitsPerPel  = 32;
   if (refresh) {
      dm.dmFields |= DM_DISPLAYFREQUENCY;
      dm.dmDisplayFrequency = refresh;
   }
   LONG st = ChangeDisplaySettingsExW(mi.szDevice, &dm, NULL, CDS_FULLSCREEN, NULL);
   if (st != DISP_CHANGE_SUCCESSFUL) {
      dm.dmFields &= ~DM_DISPLAYFREQUENCY;
      st = ChangeDisplaySettingsExW(mi.szDevice, &dm, NULL, CDS_FULLSCREEN, NULL);
   }
   return st == DISP_CHANGE_SUCCESSFUL;
}

static void
gsc_restore_display_mode(HMONITOR mon)
{
   MONITORINFOEXW mi;
   memset(&mi, 0, sizeof(mi));
   mi.cbSize = sizeof(mi);
   if (!GetMonitorInfoW(mon, (MONITORINFO *)&mi))
      return;
   DEVMODEW dm;
   memset(&dm, 0, sizeof(dm));
   dm.dmSize = sizeof(dm);
   if (EnumDisplaySettingsW(mi.szDevice, ENUM_REGISTRY_SETTINGS, &dm))
      ChangeDisplaySettingsExW(mi.szDevice, &dm, NULL, CDS_FULLSCREEN, NULL);
   else
      ChangeDisplaySettingsExW(mi.szDevice, NULL, NULL, 0, NULL);
}

/* Strip decorations and stretch the window over the whole output. */
static bool
gsc_window_enter_fullscreen(struct npt_guest_swapchain *s, HWND hwnd,
                            HMONITOR mon)
{
   GetWindowRect(hwnd, &s->fs_saved_rect);
   LONG style   = GetWindowLongW(hwnd, GWL_STYLE);
   LONG exstyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
   s->fs_saved_style   = style;
   s->fs_saved_exstyle = exstyle;

   style   &= ~WS_OVERLAPPEDWINDOW;
   exstyle &= ~WS_EX_OVERLAPPEDWINDOW;
   SetWindowLongW(hwnd, GWL_STYLE, style);
   SetWindowLongW(hwnd, GWL_EXSTYLE, exstyle);

   RECT r;
   if (!gsc_desktop_rect(mon, &r))
      return false;
   SetWindowPos(hwnd, HWND_TOPMOST, r.left, r.top,
                r.right - r.left, r.bottom - r.top,
                SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOACTIVATE);
   return true;
}

/* Restore the pre-fullscreen style/rect.  Only touch the styles if the app
 * hasn't changed them itself, matching native DXGI. */
static void
gsc_window_leave_fullscreen(struct npt_guest_swapchain *s, HWND hwnd)
{
   LONG curStyle = GetWindowLongW(hwnd, GWL_STYLE)   & ~WS_VISIBLE;
   LONG curEx    = GetWindowLongW(hwnd, GWL_EXSTYLE) & ~WS_EX_TOPMOST;
   if (curStyle == (s->fs_saved_style   & ~(WS_VISIBLE    | WS_OVERLAPPEDWINDOW)) &&
       curEx    == (s->fs_saved_exstyle & ~(WS_EX_TOPMOST | WS_EX_OVERLAPPEDWINDOW))) {
      SetWindowLongW(hwnd, GWL_STYLE,   s->fs_saved_style);
      SetWindowLongW(hwnd, GWL_EXSTYLE, s->fs_saved_exstyle);
   }
   RECT r = s->fs_saved_rect;
   SetWindowPos(hwnd,
                (s->fs_saved_exstyle & WS_EX_TOPMOST) ? HWND_TOPMOST : HWND_NOTOPMOST,
                r.left, r.top, r.right - r.left, r.bottom - r.top,
                SWP_FRAMECHANGED | SWP_NOACTIVATE);
}

/* Resize a windowed swap chain's client area to w x h. */
static void
gsc_window_resize(HWND hwnd, uint32_t w, uint32_t h)
{
   RECT newRect = { 0, 0, (LONG)w, (LONG)h };
   RECT oldRect = { 0, 0, 0, 0 };
   GetWindowRect(hwnd, &oldRect);
   AdjustWindowRectEx(&newRect, GetWindowLongW(hwnd, GWL_STYLE), FALSE,
                      GetWindowLongW(hwnd, GWL_EXSTYLE));
   SetRect(&newRect, 0, 0, newRect.right - newRect.left,
           newRect.bottom - newRect.top);
   OffsetRect(&newRect, oldRect.left, oldRect.top);
   MoveWindow(hwnd, newRect.left, newRect.top,
              newRect.right - newRect.left, newRect.bottom - newRect.top, TRUE);
}

static HRESULT NPT_STDMETHODCALLTYPE
gsc_ResizeTarget(void *self, const DXGI_MODE_DESC *pNewTargetParameters)
{
   if (!pNewTargetParameters)
      return NPT_DXGI_ERROR_INVALID_CALL;
   struct npt_guest_swapchain *s = gsc(self);
   HWND hwnd = s->hwnd;
   if (!hwnd || !IsWindow(hwnd))
      return NPT_DXGI_ERROR_INVALID_CALL;

   uint32_t w = pNewTargetParameters->Width;
   uint32_t h = pNewTargetParameters->Height;

   if (!s->fullscreen) {
      /* Windowed: resize the client area to the requested size. */
      if (w && h)
         gsc_window_resize(hwnd, w, h);
      return NPT_S_OK;
   }

   /* Fullscreen: optionally re-program the mode, then re-cover the output.
    * Read fs_monitor once so a concurrent leave_fullscreen cannot NULL it
    * between uses; the transition itself is not excluded. */
   HMONITOR mon = s->fs_monitor;
   if (!mon)
      return NPT_S_OK;
   if ((s->desc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH) && w && h) {
      uint32_t rr = pNewTargetParameters->RefreshRate.Denominator
         ? pNewTargetParameters->RefreshRate.Numerator /
           pNewTargetParameters->RefreshRate.Denominator : 0;
      /* Record the modeset: this is the same display-mode change
       * gsc_enter_fullscreen makes, and both restore paths are gated on the
       * flag, so without it a mode programmed only here outlives the app. */
      if (gsc_set_display_mode(mon, w, h, rr))
         s->fs_did_modeset = true;
   }
   RECT r;
   if (gsc_desktop_rect(mon, &r))
      MoveWindow(hwnd, r.left, r.top, r.right - r.left, r.bottom - r.top, TRUE);
   return NPT_S_OK;
}

static HRESULT NPT_STDMETHODCALLTYPE
gsc_GetDesc(void *self, DXGI_SWAP_CHAIN_DESC *pDesc)
{
   struct npt_guest_swapchain *s = gsc(self);
   if (!pDesc)
      return NPT_E_POINTER;
   memset(pDesc, 0, sizeof(*pDesc));
   pDesc->BufferDesc.Width = s->desc.Width;
   pDesc->BufferDesc.Height = s->desc.Height;
   pDesc->BufferDesc.RefreshRate = s->fs_desc.RefreshRate;
   pDesc->BufferDesc.Format = s->desc.Format;
   pDesc->BufferDesc.ScanlineOrdering = s->fs_desc.ScanlineOrdering;
   pDesc->BufferDesc.Scaling = s->fs_desc.Scaling;
   pDesc->SampleDesc = s->desc.SampleDesc;
   pDesc->BufferUsage = s->desc.BufferUsage;
   pDesc->BufferCount = s->desc.BufferCount;
   pDesc->OutputWindow = s->hwnd;
   pDesc->Windowed = s->fs_desc.Windowed;
   pDesc->SwapEffect = s->desc.SwapEffect;
   pDesc->Flags = s->desc.Flags;
   return NPT_S_OK;
}

static HRESULT NPT_STDMETHODCALLTYPE
gsc_GetDesc1(void *self, DXGI_SWAP_CHAIN_DESC1 *pDesc)
{
   struct npt_guest_swapchain *s = gsc(self);
   if (!pDesc)
      return NPT_E_POINTER;
   *pDesc = s->desc;
   return NPT_S_OK;
}

static HRESULT NPT_STDMETHODCALLTYPE
gsc_GetFullscreenDesc(void *self, DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pDesc)
{
   struct npt_guest_swapchain *s = gsc(self);
   if (!pDesc)
      return NPT_E_POINTER;
   *pDesc = s->fs_desc;
   return NPT_S_OK;
}

static HRESULT NPT_STDMETHODCALLTYPE
gsc_GetHwnd(void *self, HWND *pHwnd)
{
   struct npt_guest_swapchain *s = gsc(self);
   if (!pHwnd)
      return NPT_E_POINTER;
   *pHwnd = s->hwnd;
   return NPT_S_OK;
}

static HRESULT NPT_STDMETHODCALLTYPE
gsc_GetCoreWindow(void *self, const IID *refiid, void **ppUnk)
{
   (void)self; (void)refiid;
   if (ppUnk)
      *ppUnk = NULL;
   return NPT_DXGI_ERROR_INVALID_CALL;
}

/* ------------------------------------------------------------------ */
/* fullscreen / outputs (guest-fab wrappers never reach the host)      */
/* ------------------------------------------------------------------ */

static inline const struct npt_idxgioutput_client_vtbl *
output_vtbl(IDXGIOutput *o)
{
   return (const void *)((struct npt_com_base *)o)->lpVtbl;
}

static void
gsc_assign_output(struct npt_guest_swapchain *s,
                  IDXGIOutput **slot, IDXGIOutput *new_val)
{
   IDXGIOutput *old;
   mtx_lock(&s->output_lock);
   old = *slot;
   if (new_val)
      output_vtbl(new_val)->AddRef(new_val);
   *slot = new_val;
   mtx_unlock(&s->output_lock);
   if (old)
      output_vtbl(old)->Release(old);
}

static IDXGIOutput *
gsc_load_output_addref(struct npt_guest_swapchain *s, IDXGIOutput **slot)
{
   IDXGIOutput *v;
   mtx_lock(&s->output_lock);
   v = *slot;
   if (v)
      output_vtbl(v)->AddRef(v);
   mtx_unlock(&s->output_lock);
   return v;
}

/* device -> IDXGIDevice -> adapter -> EnumOutputs(0); the adapter
 * override fabricates the output, so the result is identity-compatible
 * with EnumOutputs(0) callers.  Caller owns the returned ref. */
static IDXGIOutput *
gsc_resolve_first_output(struct npt_guest_swapchain *s)
{
   IDXGIDevice *dxgi_dev = NULL;
   if (NPT_FAILED(gsc_qi_held(s->device, &NPT_IID_IDXGIDevice,
                              (void **)&dxgi_dev)) || !dxgi_dev)
      return NULL;
   const struct npt_idxgidevice_client_vtbl *dv =
      (const void *)((struct npt_com_base *)dxgi_dev)->lpVtbl;

   IDXGIAdapter *adp = NULL;
   HRESULT hr = dv->GetAdapter(dxgi_dev, &adp);
   if (NPT_FAILED(hr) || !adp) {
      dv->Release(dxgi_dev);
      return NULL;
   }
   const struct npt_idxgiadapter_client_vtbl *av =
      (const void *)((struct npt_com_base *)adp)->lpVtbl;

   IDXGIOutput *out = NULL;
   hr = av->EnumOutputs(adp, 0, &out);
   av->Release(adp);
   dv->Release(dxgi_dev);
   return NPT_SUCCEEDED(hr) ? out : NULL;
}

/* Windowed -> fullscreen.  pTarget (may be NULL) is the app-requested output;
 * the real Win32 monitor is resolved from the window so guest-fab HMONITOR
 * cookies never reach GetMonitorInfoW. */
static HRESULT
gsc_enter_fullscreen(void *self, struct npt_guest_swapchain *s,
                     IDXGIOutput *pTarget)
{
   if (atomic_exchange(&s->mode_change_in_progress, 1))
      return NPT_DXGI_STATUS_MODE_CHANGE_IN_PROGRESS;

   HRESULT hr = NPT_DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
   HWND hwnd = s->hwnd;
   if (hwnd && IsWindow(hwnd)) {
      bool mode_switch =
         (s->desc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH) != 0;

      /* Hold a ref on the containing output for GetFullscreenState. */
      IDXGIOutput *out = pTarget;
      if (out)
         output_vtbl(out)->AddRef(out);
      else
         out = gsc_resolve_first_output(s);

      HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);

      if (mode_switch && out) {
         /* Snap the requested backbuffer size to a supported (capped) mode,
          * then program it.  Non-fatal: borderless still works on failure. */
         DXGI_MODE_DESC want, got;
         memset(&want, 0, sizeof(want));
         memset(&got, 0, sizeof(got));
         want.Width       = s->desc.Width;
         want.Height      = s->desc.Height;
         want.Format      = s->desc.Format;
         want.RefreshRate = s->fs_desc.RefreshRate;
         if (NPT_SUCCEEDED(output_vtbl(out)->FindClosestMatchingMode(out, &want,
                                                                     &got, NULL))) {
            uint32_t rr = got.RefreshRate.Denominator
               ? got.RefreshRate.Numerator / got.RefreshRate.Denominator : 0;
            if (gsc_set_display_mode(mon, got.Width, got.Height, rr))
               s->fs_did_modeset = true;
         }
      }

      if (gsc_window_enter_fullscreen(s, hwnd, mon)) {
         s->fs_monitor  = mon;
         s->fullscreen  = TRUE;
         gsc_assign_output(s, &s->fullscreen_target, out);
         RECT dr = { 0 };
         gsc_desktop_rect(mon, &dr);
         npt_log("guest swapchain %p entered fullscreen: window -> %ldx%ld (mode_switch=%d, modeset=%d)",
                 self, dr.right - dr.left, dr.bottom - dr.top,
                 mode_switch, s->fs_did_modeset);
         hr = NPT_S_OK;
      } else if (s->fs_did_modeset) {
         gsc_restore_display_mode(mon);
         s->fs_did_modeset = false;
      }
      if (out)
         output_vtbl(out)->Release(out);
   }
   atomic_store(&s->mode_change_in_progress, 0);
   return hr;
}

/* Fullscreen -> windowed. */
static HRESULT
gsc_leave_fullscreen(void *self, struct npt_guest_swapchain *s)
{
   if (atomic_exchange(&s->mode_change_in_progress, 1))
      return NPT_DXGI_STATUS_MODE_CHANGE_IN_PROGRESS;

   if (s->fs_did_modeset) {
      gsc_restore_display_mode(s->fs_monitor);
      s->fs_did_modeset = false;
   }
   s->fullscreen = FALSE;
   HWND hwnd = s->hwnd;
   if (hwnd && IsWindow(hwnd))
      gsc_window_leave_fullscreen(s, hwnd);
   gsc_assign_output(s, &s->fullscreen_target, NULL);
   s->fs_monitor = NULL;
   npt_log("guest swapchain %p left fullscreen", self);
   atomic_store(&s->mode_change_in_progress, 0);
   return NPT_S_OK;
}

static HRESULT NPT_STDMETHODCALLTYPE
gsc_SetFullscreenState(void *self, BOOL Fullscreen, IDXGIOutput *pTarget)
{
   struct npt_guest_swapchain *s = gsc(self);

   if (!Fullscreen && pTarget)
      return NPT_DXGI_ERROR_INVALID_CALL;

   /* Dispatch the windowed <-> fullscreen transition; both directions place
    * and style the app's own OutputWindow guest-side. */
   if (!s->fullscreen && Fullscreen)
      return gsc_enter_fullscreen(self, s, pTarget);
   if (s->fullscreen && !Fullscreen)
      return gsc_leave_fullscreen(self, s);
   return NPT_S_OK;
}

static HRESULT NPT_STDMETHODCALLTYPE
gsc_GetFullscreenState(void *self, BOOL *pFullscreen, IDXGIOutput **ppTarget)
{
   struct npt_guest_swapchain *s = gsc(self);
   if (pFullscreen)
      *pFullscreen = s->fullscreen;
   if (ppTarget)
      *ppTarget = s->fullscreen
                     ? gsc_load_output_addref(s, &s->fullscreen_target)
                     : NULL;
   return NPT_S_OK;
}

static HRESULT NPT_STDMETHODCALLTYPE
gsc_GetContainingOutput(void *self, IDXGIOutput **ppOutput)
{
   struct npt_guest_swapchain *s = gsc(self);
   if (!ppOutput)
      return NPT_E_POINTER;

   *ppOutput = gsc_load_output_addref(s, &s->containing_output);
   if (*ppOutput)
      return NPT_S_OK;

   IDXGIOutput *first = gsc_resolve_first_output(s);
   if (!first)
      return NPT_DXGI_ERROR_NOT_FOUND;
   /* assign_output AddRefs into the cache; the resolver's ref
    * transfers to the caller. */
   gsc_assign_output(s, &s->containing_output, first);
   *ppOutput = first;
   return NPT_S_OK;
}

static HRESULT NPT_STDMETHODCALLTYPE
gsc_GetRestrictToOutput(void *self, IDXGIOutput **ppRestrictToOutput)
{
   (void)self;
   if (!ppRestrictToOutput)
      return NPT_E_POINTER;
   *ppRestrictToOutput = NULL;
   return NPT_S_OK;
}

/* ------------------------------------------------------------------ */
/* stats / misc state                                                  */
/* ------------------------------------------------------------------ */

static HRESULT NPT_STDMETHODCALLTYPE
gsc_GetFrameStatistics(void *self, DXGI_FRAME_STATISTICS *pStats)
{
   (void)self;
   if (!pStats)
      return NPT_E_POINTER;
   memset(pStats, 0, sizeof(*pStats));
   return NPT_S_OK;
}

static HRESULT NPT_STDMETHODCALLTYPE
gsc_GetLastPresentCount(void *self, UINT *pLastPresentCount)
{
   struct npt_guest_swapchain *s = gsc(self);
   if (!pLastPresentCount)
      return NPT_E_POINTER;
   *pLastPresentCount = s->present_count;
   return NPT_S_OK;
}

static BOOL NPT_STDMETHODCALLTYPE
gsc_IsTemporaryMonoSupported(void *self)
{
   (void)self;
   return 0;
}

static HRESULT NPT_STDMETHODCALLTYPE
gsc_SetBackgroundColor(void *self, const DXGI_RGBA *pColor)
{
   struct npt_guest_swapchain *s = gsc(self);
   if (!pColor)
      return NPT_E_POINTER;
   s->background = *pColor;
   return NPT_S_OK;
}

static HRESULT NPT_STDMETHODCALLTYPE
gsc_GetBackgroundColor(void *self, DXGI_RGBA *pColor)
{
   struct npt_guest_swapchain *s = gsc(self);
   if (!pColor)
      return NPT_E_POINTER;
   *pColor = s->background;
   return NPT_S_OK;
}

static HRESULT NPT_STDMETHODCALLTYPE
gsc_SetRotation(void *self, DXGI_MODE_ROTATION Rotation)
{
   (void)self;
   return Rotation == DXGI_MODE_ROTATION_IDENTITY
             ? NPT_S_OK : NPT_DXGI_ERROR_INVALID_CALL;
}

static HRESULT NPT_STDMETHODCALLTYPE
gsc_GetRotation(void *self, DXGI_MODE_ROTATION *pRotation)
{
   (void)self;
   if (!pRotation)
      return NPT_E_POINTER;
   *pRotation = DXGI_MODE_ROTATION_IDENTITY;
   return NPT_S_OK;
}

static HRESULT NPT_STDMETHODCALLTYPE
gsc_SetSourceSize(void *self, UINT Width, UINT Height)
{
   struct npt_guest_swapchain *s = gsc(self);
   if (!Width || !Height ||
       Width > s->desc.Width || Height > s->desc.Height)
      return NPT_DXGI_ERROR_INVALID_CALL;
   if (Width != s->desc.Width || Height != s->desc.Height)
      npt_log("guest swapchain: SetSourceSize %ux%u ignored (full %ux%u)",
              Width, Height, s->desc.Width, s->desc.Height);
   return NPT_S_OK;
}

static HRESULT NPT_STDMETHODCALLTYPE
gsc_GetSourceSize(void *self, UINT *pWidth, UINT *pHeight)
{
   struct npt_guest_swapchain *s = gsc(self);
   if (!pWidth || !pHeight)
      return NPT_E_POINTER;
   *pWidth = s->desc.Width;
   *pHeight = s->desc.Height;
   return NPT_S_OK;
}

static HRESULT NPT_STDMETHODCALLTYPE
gsc_SetMaximumFrameLatency(void *self, UINT MaxLatency)
{
   struct npt_guest_swapchain *s = gsc(self);
   if (!MaxLatency || MaxLatency > NPT_SC_MAX_SWAP_CHAIN_BUFFERS)
      return NPT_DXGI_ERROR_INVALID_CALL;
   /* Waitable object: grow the semaphore by the delta and never shrink
    * it (matching DXGI -- shrinking could strand an app mid-wait). */
   if (s->frame_latency_sem && MaxLatency > s->max_frame_latency)
      ReleaseSemaphore(s->frame_latency_sem,
                       (LONG)(MaxLatency - s->max_frame_latency), NULL);
   s->max_frame_latency = MaxLatency;
   return NPT_S_OK;
}

static HRESULT NPT_STDMETHODCALLTYPE
gsc_GetMaximumFrameLatency(void *self, UINT *pMaxLatency)
{
   struct npt_guest_swapchain *s = gsc(self);
   if (!pMaxLatency)
      return NPT_E_POINTER;
   *pMaxLatency = s->max_frame_latency;
   return NPT_S_OK;
}

static HANDLE NPT_STDMETHODCALLTYPE
gsc_GetFrameLatencyWaitableObject(void *self)
{
   struct npt_guest_swapchain *s = gsc(self);
   if (!s->frame_latency_sem)
      return NULL;
   /* Hand the app its own handle (DXGI semantics); the swapchain keeps
    * the original and closes it at teardown. */
   HANDLE dup = NULL;
   DuplicateHandle(GetCurrentProcess(), s->frame_latency_sem,
                   GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS);
   return dup;
}

static HRESULT NPT_STDMETHODCALLTYPE
gsc_SetMatrixTransform(void *self, const DXGI_MATRIX_3X2_F *pMatrix)
{
   (void)self; (void)pMatrix;
   return NPT_DXGI_ERROR_INVALID_CALL;
}

static HRESULT NPT_STDMETHODCALLTYPE
gsc_GetMatrixTransform(void *self, DXGI_MATRIX_3X2_F *pMatrix)
{
   (void)self;
   if (!pMatrix)
      return NPT_E_POINTER;
   memset(pMatrix, 0, sizeof(*pMatrix));
   pMatrix->_11 = 1.0f;
   pMatrix->_22 = 1.0f;
   return NPT_S_OK;
}

static UINT NPT_STDMETHODCALLTYPE
gsc_GetCurrentBackBufferIndex(void *self)
{
   (void)self;
   return 0;
}

static HRESULT NPT_STDMETHODCALLTYPE
gsc_CheckColorSpaceSupport(void *self, DXGI_COLOR_SPACE_TYPE ColorSpace,
                           UINT *pColorSpaceSupport)
{
   (void)self;
   if (!pColorSpaceSupport)
      return NPT_E_POINTER;
   *pColorSpaceSupport =
      ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709
         ? DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT : 0;
   return NPT_S_OK;
}

static HRESULT NPT_STDMETHODCALLTYPE
gsc_SetColorSpace1(void *self, DXGI_COLOR_SPACE_TYPE ColorSpace)
{
   (void)self;
   return ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709
             ? NPT_S_OK : NPT_DXGI_ERROR_INVALID_CALL;
}

static HRESULT NPT_STDMETHODCALLTYPE
gsc_SetHDRMetaData(void *self, DXGI_HDR_METADATA_TYPE Type, UINT Size,
                   void *pMetaData)
{
   (void)self; (void)Type; (void)Size; (void)pMetaData;
   return NPT_S_OK;
}

/* ------------------------------------------------------------------ */
/* vtbl                                                                */
/* ------------------------------------------------------------------ */

static ULONG NPT_STDMETHODCALLTYPE
gsc_AddRef(void *self)
{
   return npt_com_default_addref(self);
}

static ULONG NPT_STDMETHODCALLTYPE
gsc_Release(void *self)
{
   return npt_com_default_release(self);
}

static const struct npt_idxgiswapchain4_client_vtbl gsc_vtbl = {
   .QueryInterface = gsc_QueryInterface,
   .AddRef = gsc_AddRef,
   .Release = gsc_Release,
   .SetPrivateData = gsc_SetPrivateData,
   .SetPrivateDataInterface = gsc_SetPrivateDataInterface,
   .GetPrivateData = gsc_GetPrivateData,
   .GetParent = gsc_GetParent,
   .GetDevice = gsc_GetDevice,
   .Present = gsc_Present,
   .GetBuffer = gsc_GetBuffer,
   .SetFullscreenState = gsc_SetFullscreenState,
   .GetFullscreenState = gsc_GetFullscreenState,
   .GetDesc = gsc_GetDesc,
   .ResizeBuffers = gsc_ResizeBuffers,
   .ResizeTarget = gsc_ResizeTarget,
   .GetContainingOutput = gsc_GetContainingOutput,
   .GetFrameStatistics = gsc_GetFrameStatistics,
   .GetLastPresentCount = gsc_GetLastPresentCount,
   .GetDesc1 = gsc_GetDesc1,
   .GetFullscreenDesc = gsc_GetFullscreenDesc,
   .GetHwnd = gsc_GetHwnd,
   .GetCoreWindow = gsc_GetCoreWindow,
   .Present1 = gsc_Present1,
   .IsTemporaryMonoSupported = gsc_IsTemporaryMonoSupported,
   .GetRestrictToOutput = gsc_GetRestrictToOutput,
   .SetBackgroundColor = gsc_SetBackgroundColor,
   .GetBackgroundColor = gsc_GetBackgroundColor,
   .SetRotation = gsc_SetRotation,
   .GetRotation = gsc_GetRotation,
   .SetSourceSize = gsc_SetSourceSize,
   .GetSourceSize = gsc_GetSourceSize,
   .SetMaximumFrameLatency = gsc_SetMaximumFrameLatency,
   .GetMaximumFrameLatency = gsc_GetMaximumFrameLatency,
   .GetFrameLatencyWaitableObject = gsc_GetFrameLatencyWaitableObject,
   .SetMatrixTransform = gsc_SetMatrixTransform,
   .GetMatrixTransform = gsc_GetMatrixTransform,
   .GetCurrentBackBufferIndex = gsc_GetCurrentBackBufferIndex,
   .CheckColorSpaceSupport = gsc_CheckColorSpaceSupport,
   .SetColorSpace1 = gsc_SetColorSpace1,
   .ResizeBuffers1 = gsc_ResizeBuffers1,
   .SetHDRMetaData = gsc_SetHDRMetaData,
};

/* ------------------------------------------------------------------ */
/* creation / destruction                                              */
/* ------------------------------------------------------------------ */

static void
gsc_aux_destroy(void *aux)
{
   struct npt_guest_swapchain *s = aux;
   struct npt_device *dev = s->com ? s->com->base.device : NULL;

   gsc_teardown_wsi(s);

   /* A swap chain destroyed while still fullscreen leaves the display in the
    * app's mode; restore the desktop mode like DXGI does on teardown. */
   if (s->fs_did_modeset) {
      gsc_restore_display_mode(s->fs_monitor);
      s->fs_did_modeset = false;
   }

   if (s->fullscreen_target)
      output_vtbl(s->fullscreen_target)->Release(s->fullscreen_target);
   if (s->containing_output)
      output_vtbl(s->containing_output)->Release(s->containing_output);

   gsc_release_held(dev, &s->backbuffer, &s->backbuffer_id);
   gsc_release_held(dev, &s->fence, &s->fence_id);
   gsc_release_held(dev, &s->dc4, &s->dc4_id);
   gsc_release_held(dev, &s->dc, &s->dc_id);
   gsc_release_held(dev, &s->device, &s->device_id);
   gsc_release_held(dev, &s->factory, &s->factory_id);

   cnd_destroy(&s->wsi_cond);
   mtx_destroy(&s->wsi_mutex);
   mtx_destroy(&s->present_mutex);
   mtx_destroy(&s->output_lock);
   /* Workers joined by gsc_teardown_wsi above, so these are unreferenced. */
   if (s->image_release_evt)
      CloseHandle(s->image_release_evt);
   if (s->frame_latency_sem)
      CloseHandle(s->frame_latency_sem);
   free(s);
}

/* QI the immediate context to ID3D11DeviceContext4 and the device to
 * ID3D11Device5, then create the GPU-done fence.  All three are
 * D3D11.3; on any miss the swapchain degrades to unfenced present
 * rather than failing creation. */
static void
gsc_setup_fence(struct npt_guest_swapchain *s)
{
   HRESULT hr = gsc_qi_held(s->dc, &NPT_IID_ID3D11DeviceContext4, &s->dc4);
   if (NPT_FAILED(hr) || !s->dc4) {
      s->dc4 = NULL;
      s->no_gpu_fence = true;
      npt_log("guest swapchain: no ID3D11DeviceContext4; unfenced present");
      return;
   }
   s->dc4_id = npt_com_self_id(s->dc4);

   void *device5 = NULL;
   hr = gsc_qi_held(s->device, &NPT_IID_ID3D11Device5, &device5);
   if (NPT_FAILED(hr) || !device5) {
      s->no_gpu_fence = true;
      npt_log("guest swapchain: no ID3D11Device5; unfenced present");
      return;
   }

   const struct npt_id3d11device5_client_vtbl *v5 =
      (const void *)((struct npt_com_base *)device5)->lpVtbl;
   ID3D11Fence *fence = NULL;
   hr = v5->CreateFence(device5, 0, D3D11_FENCE_FLAG_NONE,
                        &NPT_IID_ID3D11Fence, (void **)&fence);
   v5->Release(device5);
   if (NPT_FAILED(hr) || !fence) {
      s->no_gpu_fence = true;
      npt_log("guest swapchain: CreateFence failed hr=0x%x; unfenced present",
              hr);
      return;
   }
   s->fence = fence;
   s->fence_id = npt_com_self_id(fence);
}

HRESULT
npt_guest_swapchain_create(void *device_unknown,
                           const DXGI_SWAP_CHAIN_DESC1 *desc1,
                           const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fs_desc,
                           HWND hwnd,
                           void *factory_wrapper,
                           void **out_swapchain)
{
   if (!out_swapchain)
      return NPT_E_POINTER;
   *out_swapchain = NULL;
   if (!device_unknown || !desc1)
      return NPT_DXGI_ERROR_INVALID_CALL;
   if (!hwnd) {
      npt_log("guest swapchain: no output window");
      return NPT_DXGI_ERROR_INVALID_CALL;
   }

   struct npt_device *dev = npt_com_self_device(device_unknown);
   if (!dev)
      return NPT_E_FAIL;

   /* The caller's pDevice may be any interface of the D3D11 device;
    * resolve the ID3D11Device wrapper we render through. */
   void *device = NULL;
   {
      const struct npt_idxgiswapchain_client_vtbl *v =
         (const void *)((struct npt_com_base *)device_unknown)->lpVtbl;
      HRESULT hr = v->QueryInterface(device_unknown, &NPT_IID_ID3D11Device,
                                     &device);
      if (NPT_FAILED(hr) || !device)
         return NPT_DXGI_ERROR_INVALID_CALL;
   }

   const uint64_t guest_id = npt_com_make_guest_id(
      NPT_GUEST_KIND_SWAPCHAIN, npt_com_allocate_next_id());
   struct npt_com_base *com =
      npt_com_get_or_wrap(dev, &NPT_IID_IDXGISwapChain4, guest_id,
                          (struct npt_com_base *)device);
   if (!com) {
      npt_com_default_release(device);
      return NPT_E_OUTOFMEMORY;
   }

   /* The drain and WSI workers run detached and dereference the device and its
    * renderer, so both must outlive this swapchain.  Today that holds only as a
    * side effect of the wrapper graph pinning the device wrapper; make it a
    * reference this object owns, so the device cannot be destroyed underneath a
    * live swapchain no matter how the graph is refactored. */
   npt_device_retain(dev);
   atomic_fetch_add_explicit(&com->base.device_ref_holds, 1,
                             memory_order_relaxed);

   struct npt_guest_swapchain *s = calloc(1, sizeof(*s));
   if (!s) {
      npt_com_default_release(com);
      npt_com_default_release(device);
      return NPT_E_OUTOFMEMORY;
   }

   /* Not published to any other thread yet; safe to swap the vtbl and
    * attach aux without ordering concerns. */
   com->aux = s;
   com->aux_destroy = gsc_aux_destroy;
   com->lpVtbl = (const void **)&gsc_vtbl;

   s->com = com;
   mtx_init(&s->present_mutex, mtx_plain);
   mtx_init(&s->output_lock, mtx_plain);
   mtx_init(&s->wsi_mutex, mtx_plain);
   cnd_init(&s->wsi_cond);
   /* Auto-reset: an unconsumed signal latches, so a release that races
    * the reuse gate's inflight check is not lost. */
   s->image_release_evt = CreateEventW(NULL, FALSE, FALSE, NULL);
   for (uint32_t i = 0; i < NPT_SC_PRESENT_LATENCY; i++)
      s->latency_fences[i] = -1;

   s->desc = *desc1;
   if (fs_desc) {
      s->fs_desc = *fs_desc;
   } else {
      memset(&s->fs_desc, 0, sizeof(s->fs_desc));
      s->fs_desc.Windowed = 1;
   }
   s->hwnd = hwnd;
   /* Born windowed; a fullscreen creation desc is realized by the factory's
    * post-creation npt_swapchain_apply_initial_fullscreen -> SetFullscreenState,
    * which runs the guest-side window transition. */
   s->fullscreen = 0;
   /* Waitable swapchains default to 1 frame of latency (DXGI); others to
    * 3.  The waitable object is a semaphore released once per frame. */
   const bool waitable =
      (s->desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) != 0;
   s->max_frame_latency = waitable ? 1 : 3;
   if (waitable) {
      s->frame_latency_sem = CreateSemaphoreW(
         NULL, (LONG)s->max_frame_latency,
         (LONG)NPT_SC_MAX_SWAP_CHAIN_BUFFERS, NULL);
      if (!s->frame_latency_sem)
         npt_log("guest swapchain: frame-latency semaphore create failed "
                 "err=%lu; waitable object unavailable", GetLastError());
   }
   s->background.a = 1.0f;

   /* Normalize: DXGI fills zero dimensions from the window, defaults
    * the format, and needs at least one buffer. */
   if (!s->desc.Width || !s->desc.Height) {
      UINT w = s->desc.Width, h = s->desc.Height;
      gsc_query_window_size(s, &w, &h);
      s->desc.Width = w;
      s->desc.Height = h;
   }
   if (!s->desc.Format)
      s->desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
   if (!s->desc.BufferCount)
      s->desc.BufferCount = 1;
   if (!s->desc.SampleDesc.Count)
      s->desc.SampleDesc.Count = 1;

   s->device = device;
   s->device_id = npt_com_self_id(device);

   {
      const struct npt_id3d11device_client_vtbl *dv =
         (const void *)((struct npt_com_base *)device)->lpVtbl;
      ID3D11DeviceContext *dc = NULL;
      dv->GetImmediateContext(device, &dc);
      if (!dc) {
         npt_com_default_release(com);
         return NPT_E_FAIL;
      }
      s->dc = dc;
      s->dc_id = npt_com_self_id(dc);
   }

   if (factory_wrapper) {
      npt_com_default_addref(factory_wrapper);
      s->factory = factory_wrapper;
      s->factory_id = npt_com_self_id(factory_wrapper);
   }

   gsc_setup_fence(s);

   HRESULT hr = gsc_create_backbuffer(s);
   if (NPT_FAILED(hr)) {
      npt_com_default_release(com);
      return hr;
   }

   npt_log("guest swapchain: created %ux%u fmt=%u buffers=%u hwnd=%p",
           s->desc.Width, s->desc.Height, s->desc.Format,
           s->desc.BufferCount, (void *)hwnd);

   *out_swapchain = com;
   return NPT_S_OK;
}

#else /* !__WINE__ */

/* Native Win32: the OS DXGI runtime + Triton own presentation, so
 * COM-level swapchain creation is unimplemented here. */
HRESULT
npt_guest_swapchain_create(void *device_unknown,
                           const DXGI_SWAP_CHAIN_DESC1 *desc1,
                           const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fs_desc,
                           HWND hwnd,
                           void *factory_wrapper,
                           void **out_swapchain)
{
   (void)device_unknown; (void)desc1; (void)fs_desc; (void)hwnd;
   (void)factory_wrapper;
   if (out_swapchain)
      *out_swapchain = NULL;
   return NPT_E_NOTIMPL;
}

#endif /* __WINE__ */

HRESULT
npt_guest_swapchain_create_legacy(void *device_unknown,
                                  const DXGI_SWAP_CHAIN_DESC *desc,
                                  void *factory_wrapper,
                                  void **out_swapchain)
{
   if (!desc)
      return NPT_DXGI_ERROR_INVALID_CALL;

   DXGI_SWAP_CHAIN_DESC1 d1;
   memset(&d1, 0, sizeof(d1));
   d1.Width = desc->BufferDesc.Width;
   d1.Height = desc->BufferDesc.Height;
   d1.Format = desc->BufferDesc.Format;
   d1.SampleDesc = desc->SampleDesc;
   d1.BufferUsage = desc->BufferUsage;
   d1.BufferCount = desc->BufferCount;
   d1.SwapEffect = desc->SwapEffect;
   d1.Flags = desc->Flags;

   DXGI_SWAP_CHAIN_FULLSCREEN_DESC fs;
   memset(&fs, 0, sizeof(fs));
   fs.RefreshRate = desc->BufferDesc.RefreshRate;
   fs.ScanlineOrdering = desc->BufferDesc.ScanlineOrdering;
   fs.Scaling = desc->BufferDesc.Scaling;
   fs.Windowed = desc->Windowed;

   return npt_guest_swapchain_create(device_unknown, &d1, &fs,
                                     desc->OutputWindow, factory_wrapper,
                                     out_swapchain);
}
