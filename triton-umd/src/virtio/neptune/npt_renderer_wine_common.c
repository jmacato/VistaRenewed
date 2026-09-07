/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 */

#ifdef __WINE__

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winternl.h>

#include <string.h>

#include "npt_common.h"
#include "npt_renderer.h"
#include "npt_renderer_wine_common.h"
#include "nptunix/npt_unixlib.h"

#include "virtio-gpu/virgl_hw.h"

void npt_wine_unixlib_close(int fd)
{
   struct npt_unix_close_params p = { .fd = fd };
   npt_wine_unix_call(npt_unix_close, &p);
}

int npt_wine_unixlib_dup(int fd)
{
   if (fd < 0)
      return -1;
   if (npt_unixlib_ensure_init() != 0)
      return -1;
   struct npt_unix_dup_params p = { .fd = fd, .result = -1 };
   if (npt_wine_unix_call(npt_unix_dup, &p) != 0)
      return -1;
   return p.result;
}

void npt_wine_unixlib_munmap(void *addr, size_t size)
{
   struct npt_unix_munmap_params p = { .addr = addr, .size = size };
   npt_wine_unix_call(npt_unix_munmap, &p);
}

static int unixlib_wsi_init(uint32_t num_images, const int *fds,
                            const uint32_t *strides, uint32_t virgl_format,
                            uint32_t width, uint32_t height, HWND hwnd)
{
   if (num_images > NPT_WSI_MAX_IMAGES) {
      npt_log("unixlib_wsi_init: num_images %u exceeds cap %u",
              num_images, (unsigned)NPT_WSI_MAX_IMAGES);
      return -1;
   }

   /* Wine's winex11.drv publishes the X11 Window XID for every Wine
    * HWND under this property (set in winex11 window.c: NtUserSetProp
    * with whole_window_prop).  Returns NULL for HWNDs without a backing
    * X11 window (composition swapchains, message-only windows, drivers
    * other than winex11). */
   uint32_t x11_window = 0;
   if (hwnd) {
      HANDLE prop = GetPropW(hwnd, L"__wine_x11_whole_window");
      x11_window = (uint32_t)(uintptr_t)prop;
   }
   if (!x11_window) {
      npt_log("unixlib_wsi_init: no X11 window for hwnd %p", (void *)hwnd);
      return -1;
   }

   struct npt_unix_wsi_init_params p;
   memset(&p, 0, sizeof(p));
   p.x11_window = x11_window;
   p.num_images = num_images;
   for (uint32_t i = 0; i < p.num_images; i++) {
      p.fds[i] = fds[i];
      p.strides[i] = strides[i];
   }
   p.virgl_format = virgl_format;
   p.width = width;
   p.height = height;
   npt_wine_unix_call(npt_unix_wsi_init, &p);
   return p.result;
}

static void unixlib_wsi_present(uint32_t image_index, int wait_fence_fd)
{
   struct npt_unix_wsi_present_params p;
   memset(&p, 0, sizeof(p));
   p.image_index = image_index;
   p.wait_fence_fd = wait_fence_fd;
   npt_wine_unix_call(npt_unix_wsi_present, &p);
   /* Close the fd unless xcb took it via SCM_RIGHTS. */
   if (wait_fence_fd >= 0 && !p.wait_fence_consumed)
      npt_wine_unixlib_close(wait_fence_fd);
}

static void unixlib_wsi_destroy(void)
{
   npt_wine_unix_call(npt_unix_wsi_destroy, NULL);
}

static int unixlib_wsi_drain(uint32_t timeout_ms,
                             uint32_t out_released_count[NPT_WSI_MAX_IMAGES])
{
   struct npt_unix_wsi_drain_params p;
   memset(&p, 0, sizeof(p));
   p.timeout_ms = timeout_ms;
   npt_wine_unix_call(npt_unix_wsi_drain, &p);
   if (out_released_count) {
      for (uint32_t i = 0; i < NPT_WSI_MAX_IMAGES; i++)
         out_released_count[i] = p.released_count[i];
   }
   return p.result;
}

static bool
wine_wsi_init(struct npt_renderer *r, uint32_t num_images,
              const int *fds, const uint32_t *strides,
              uint32_t virgl_format, uint32_t width, uint32_t height,
              uint64_t hwnd)
{
   (void)r;
   return unixlib_wsi_init(num_images, fds, strides, virgl_format,
                           width, height, (HWND)(uintptr_t)hwnd) == 0;
}

static void
wine_wsi_present(struct npt_renderer *r, uint32_t image_index,
                 int wait_fence_fd)
{
   (void)r;
   unixlib_wsi_present(image_index, wait_fence_fd);
}

static void
wine_wsi_destroy(struct npt_renderer *r)
{
   (void)r;
   unixlib_wsi_destroy();
}

static int
wine_wsi_drain(struct npt_renderer *r,
               uint32_t timeout_ms,
               uint32_t out_released_count[NPT_WSI_MAX_IMAGES])
{
   (void)r;
   return unixlib_wsi_drain(timeout_ms, out_released_count);
}

void npt_wine_install_wsi_ops(struct npt_renderer_ops *ops)
{
   ops->wsi_init = wine_wsi_init;
   ops->wsi_present = wine_wsi_present;
   ops->wsi_destroy = wine_wsi_destroy;
   ops->wsi_drain = wine_wsi_drain;
}

#endif /* __WINE__ */
