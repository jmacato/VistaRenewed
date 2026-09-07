/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * X11 DRI3/Present WSI for nptunix.so.
 *
 * Path 1 (DRI3 >= 1.4 explicit sync): wrap the per-Present sync_file
 * in a syncobj, xcb_dri3_import_syncobj it into the X server, and
 * submit via xcb_present_pixmap_synced.  X waits in its present queue
 * with no host stall.
 *
 * Path 2 (older DRI3): drop the fence and submit with
 * wait_fence=XCB_NONE.  Cold-start tear risk is negligible since
 * blit-to-dmabuf takes microseconds.
 *
 * Both paths consume the fence fd and report wait_fence_consumed=1.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <xcb/xcb.h>
#include <xcb/dri3.h>
#include <xcb/present.h>

#include <xf86drm.h>

#include "npt_unix_internal.h"
#include "npt_unixlib.h"

static struct {
   xcb_connection_t *conn;
   xcb_window_t window;
   uint32_t width, height;
   uint32_t num_images;
   struct {
      xcb_pixmap_t pixmap;
      int fd;
#ifdef NPT_HAVE_XCB_DRI3_SYNCOBJ
      /* Persistent X XID, rebound each Present. */
      xcb_dri3_syncobj_t acquire_syncobj;
#endif
   } images[NPT_WSI_MAX_IMAGES];
   uint32_t serial;
#ifdef NPT_HAVE_XCB_DRI3_SYNCOBJ
   bool dri3_has_explicit_sync;
   int drm_fd;
#endif
   xcb_present_event_t present_eid;
   xcb_special_event_t *special_event;
} g_wsi;

NTSTATUS npt_do_wsi_init(void *args)
{
   struct npt_unix_wsi_init_params *p = args;
   p->result = -1;

   if (!p->num_images || p->num_images > NPT_WSI_MAX_IMAGES)
      return STATUS_SUCCESS;

   memset(&g_wsi, 0, sizeof(g_wsi));
#ifdef NPT_HAVE_XCB_DRI3_SYNCOBJ
   g_wsi.drm_fd = -1;
#endif
   for (uint32_t i = 0; i < NPT_WSI_MAX_IMAGES; i++) {
      g_wsi.images[i].fd = -1;
#ifdef NPT_HAVE_XCB_DRI3_SYNCOBJ
      g_wsi.images[i].acquire_syncobj = XCB_NONE;
#endif
   }

   g_wsi.conn = xcb_connect(NULL, NULL);
   if (xcb_connection_has_error(g_wsi.conn)) {
      fprintf(stderr, "neptune unixlib wsi: failed to connect to X\n");
      g_wsi.conn = NULL;
      return STATUS_SUCCESS;
   }

   /* DRI3 >= 1.4 unlocks path 1. */
   xcb_dri3_query_version_reply_t *dri3 = xcb_dri3_query_version_reply(
      g_wsi.conn, xcb_dri3_query_version(g_wsi.conn, 1, 4), NULL);
   if (!dri3) {
      fprintf(stderr, "neptune unixlib wsi: DRI3 not available\n");
      xcb_disconnect(g_wsi.conn); g_wsi.conn = NULL;
      return STATUS_SUCCESS;
   }
#ifdef NPT_HAVE_XCB_DRI3_SYNCOBJ
   g_wsi.dri3_has_explicit_sync = (dri3->major_version > 1) ||
                                  (dri3->major_version == 1 && dri3->minor_version >= 4);
#endif
   free(dri3);

#ifdef NPT_HAVE_XCB_DRI3_SYNCOBJ
   /* Need a render node to make syncobjs from sync_files. */
   if (g_wsi.dri3_has_explicit_sync) {
      g_wsi.drm_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
      if (g_wsi.drm_fd < 0) {
         fprintf(stderr, "neptune unixlib wsi: open(/dev/dri/renderD128) failed (%s); "
                 "falling back to no-wait-fence path\n", strerror(errno));
         g_wsi.dri3_has_explicit_sync = false;
      }
   }
#endif

   xcb_present_query_version_reply_t *pres = xcb_present_query_version_reply(
      g_wsi.conn, xcb_present_query_version(g_wsi.conn, 1, 2), NULL);
   if (!pres) {
      fprintf(stderr, "neptune unixlib wsi: Present not available\n");
      xcb_disconnect(g_wsi.conn); g_wsi.conn = NULL;
      return STATUS_SUCCESS;
   }
   free(pres);

   g_wsi.window = p->x11_window;
   if (!g_wsi.window) {
      fprintf(stderr, "neptune unixlib wsi: no X11 window supplied\n");
      xcb_disconnect(g_wsi.conn); g_wsi.conn = NULL;
      return STATUS_SUCCESS;
   }
   g_wsi.width = p->width;
   g_wsi.height = p->height;
   g_wsi.num_images = p->num_images;

   for (uint32_t i = 0; i < p->num_images; i++) {
      if (p->fds[i] < 0) continue;

      g_wsi.images[i].fd = dup(p->fds[i]);
      g_wsi.images[i].pixmap = xcb_generate_id(g_wsi.conn);

      /* PixmapFromBuffer takes ownership of the fd via SCM_RIGHTS, so
       * dup again so our stored fd survives for the buffer lifetime. */
      int xcb_fd = dup(g_wsi.images[i].fd);
      xcb_void_cookie_t cookie = xcb_dri3_pixmap_from_buffer_checked(
         g_wsi.conn, g_wsi.images[i].pixmap, g_wsi.window,
         p->width * p->height * 4,
         p->width, p->height, p->strides[i],
         24, 32, xcb_fd);

      xcb_generic_error_t *err = xcb_request_check(g_wsi.conn, cookie);
      if (err) {
         fprintf(stderr, "neptune unixlib wsi: DRI3 PixmapFromBuffer failed for image %u (error %d)\n",
                 i, err->error_code);
         free(err);
         g_wsi.images[i].pixmap = 0;
         continue;
      }
      fprintf(stderr, "neptune unixlib wsi: pixmap 0x%x for image %u (%ux%u)\n",
              g_wsi.images[i].pixmap, i, p->width, p->height);

#ifdef NPT_HAVE_XCB_DRI3_SYNCOBJ
      if (g_wsi.dri3_has_explicit_sync) {
         /* XID is rebound to a fresh sync_file each Present. */
         g_wsi.images[i].acquire_syncobj = xcb_generate_id(g_wsi.conn);
      }
#endif
   }

   g_wsi.present_eid = xcb_generate_id(g_wsi.conn);
   xcb_present_select_input(g_wsi.conn, g_wsi.present_eid, g_wsi.window,
                            XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY |
                            XCB_PRESENT_EVENT_MASK_IDLE_NOTIFY);
   g_wsi.special_event = xcb_register_for_special_xge(
      g_wsi.conn, &xcb_present_id, g_wsi.present_eid, NULL);
   xcb_flush(g_wsi.conn);

#ifdef NPT_HAVE_XCB_DRI3_SYNCOBJ
   const int explicit_sync_log = (int)g_wsi.dri3_has_explicit_sync;
#else
   const int explicit_sync_log = 0;
#endif
   fprintf(stderr, "neptune unixlib wsi: initialized for window 0x%x "
                   "(%u images, %ux%u, explicit_sync=%d)\n",
           g_wsi.window, p->num_images, p->width, p->height,
           explicit_sync_log);
   p->result = 0;
   return STATUS_SUCCESS;
}

static int
image_index_for_pixmap(xcb_pixmap_t pixmap)
{
   for (uint32_t i = 0; i < g_wsi.num_images; i++) {
      if (g_wsi.images[i].pixmap == pixmap)
         return (int)i;
   }
   return -1;
}

/* Per-image counts (NOT a bitmask): pending_presents = presents -
 * releases on the host, and a single decrement can't undo multiple
 * lost increments. */
static void
drain_idle_notify(uint32_t *out_counts)
{
   xcb_generic_event_t *ev;
   while ((ev = xcb_poll_for_special_event(g_wsi.conn, g_wsi.special_event))) {
      xcb_present_generic_event_t *pe = (xcb_present_generic_event_t *)ev;
      if (pe->evtype == XCB_PRESENT_EVENT_IDLE_NOTIFY) {
         xcb_present_idle_notify_event_t *ie =
            (xcb_present_idle_notify_event_t *)ev;
         int idx = image_index_for_pixmap(ie->pixmap);
         if (idx >= 0 && (uint32_t)idx < NPT_WSI_MAX_IMAGES)
            out_counts[idx]++;
      }
      free(ev);
   }
}

#ifdef NPT_HAVE_XCB_DRI3_SYNCOBJ
/* Caller still owns sync_file_fd; returned fd is consumed by
 * xcb_dri3_import_syncobj. */
static int
sync_file_to_syncobj_fd(int sync_file_fd)
{
   uint32_t handle = 0;
   if (drmSyncobjCreate(g_wsi.drm_fd, 0, &handle) != 0)
      return -1;
   if (drmSyncobjImportSyncFile(g_wsi.drm_fd, handle, sync_file_fd) != 0) {
      drmSyncobjDestroy(g_wsi.drm_fd, handle);
      return -1;
   }
   int syncobj_fd = -1;
   if (drmSyncobjHandleToFD(g_wsi.drm_fd, handle, &syncobj_fd) != 0) {
      drmSyncobjDestroy(g_wsi.drm_fd, handle);
      return -1;
   }
   /* Destroy the handle; the exported fd keeps the syncobj alive. */
   drmSyncobjDestroy(g_wsi.drm_fd, handle);
   return syncobj_fd;
}
#endif /* NPT_HAVE_XCB_DRI3_SYNCOBJ */

/*
 * ASYNC: Mutter doesn't deliver vblanks for composited windows, so
 *        the default Present mode wedges after a few frames.
 * COPY:  copy into backing store instead of page-flipping.
 */
static const uint32_t kPresentOptions =
   XCB_PRESENT_OPTION_ASYNC | XCB_PRESENT_OPTION_COPY;

NTSTATUS npt_do_wsi_present(void *args)
{
   struct npt_unix_wsi_present_params *p = args;
   p->result = -1;
   p->wait_fence_consumed = 0;

   if (!g_wsi.conn || p->image_index >= g_wsi.num_images)
      return STATUS_SUCCESS;

   xcb_pixmap_t pixmap = g_wsi.images[p->image_index].pixmap;
   if (!pixmap) return STATUS_SUCCESS;

   const uint32_t this_serial = ++g_wsi.serial;

#ifdef NPT_HAVE_XCB_DRI3_SYNCOBJ
   if (g_wsi.dri3_has_explicit_sync &&
       g_wsi.images[p->image_index].acquire_syncobj != XCB_NONE) {
      xcb_dri3_syncobj_t acquire_xid =
         g_wsi.images[p->image_index].acquire_syncobj;

      if (p->wait_fence_fd >= 0) {
         int syncobj_fd = sync_file_to_syncobj_fd(p->wait_fence_fd);
         /* libdrm doesn't take ownership of imported sync_files. */
         close(p->wait_fence_fd);
         p->wait_fence_consumed = 1;
         if (syncobj_fd >= 0) {
            /* import_syncobj rebinds acquire_xid; SCM_RIGHTS the fd. */
            xcb_dri3_import_syncobj(g_wsi.conn, acquire_xid,
                                    g_wsi.window, syncobj_fd);
         } else {
            fprintf(stderr, "neptune unixlib wsi: sync_file -> syncobj failed for image %u\n",
                    p->image_index);
            /* Fall through: acquire_xid still has the previous (signaled)
             * syncobj, so the X server won't block. */
         }
      }

      /* Binary syncobj: acquire_point/release_point=0.  Release
       * syncobj is XCB_NONE since IDLE_NOTIFY + IMAGE_RELEASE
       * already covers the host-side release path. */
      xcb_present_pixmap_synced(g_wsi.conn, g_wsi.window, pixmap,
                                this_serial, 0, 0, 0, 0,
                                XCB_NONE,
                                acquire_xid, XCB_NONE,
                                /*acquire_point=*/0, /*release_point=*/0,
                                kPresentOptions,
                                0, 0, 0, 0, NULL);
      xcb_flush(g_wsi.conn);
      p->result = 0;
      return STATUS_SUCCESS;
   }
#endif /* NPT_HAVE_XCB_DRI3_SYNCOBJ */

   /* Path 2 (libxcb < 1.16): no DRI3-1.4 syncobj path available, so we
    * can't pass the wait fence to xcb_present_pixmap.  Instead, block
    * here on the sync_file ourselves: the X server's copy must not
    * start until the host GPU has finished writing the dmabuf, and
    * cross-VM implicit dmabuf sync doesn't propagate through virtio-gpu
    * blob.
    *
    * 30 s cap: defensive only.  A heavy frame can legitimately take
    * tens of ms; releasing the handshake too early causes the X server
    * to copy a partially-written dmabuf.  Bounded above by the host's
    * own retire-as-device-lost backstop. */
   if (p->wait_fence_fd >= 0) {
      struct pollfd pfd = { .fd = p->wait_fence_fd, .events = POLLIN };
      int rc;
      do {
         rc = poll(&pfd, 1, 30000);
      } while (rc < 0 && errno == EINTR);
      close(p->wait_fence_fd);
      p->wait_fence_consumed = 1;
   }

   xcb_present_pixmap(g_wsi.conn, g_wsi.window, pixmap,
                      this_serial, 0, 0, 0, 0,
                      XCB_NONE, XCB_NONE, XCB_NONE,
                      kPresentOptions,
                      0, 0, 0, 0, NULL);
   xcb_flush(g_wsi.conn);

   p->result = 0;
   return STATUS_SUCCESS;
}

NTSTATUS npt_do_wsi_drain(void *args)
{
   struct npt_unix_wsi_drain_params *p = args;
   for (uint32_t i = 0; i < NPT_WSI_MAX_IMAGES; i++)
      p->released_count[i] = 0;
   p->result = -1;

   if (!g_wsi.conn || !g_wsi.special_event)
      return STATUS_SUCCESS;

   if (xcb_connection_has_error(g_wsi.conn))
      return STATUS_SUCCESS;

   /* Steady-state path; poll(2) below covers cold-start. */
   drain_idle_notify(p->released_count);
   bool any = false;
   for (uint32_t i = 0; i < NPT_WSI_MAX_IMAGES; i++)
      if (p->released_count[i]) { any = true; break; }
   if (any) {
      p->result = 0;
      return STATUS_SUCCESS;
   }

   const int conn_fd = xcb_get_file_descriptor(g_wsi.conn);
   if (conn_fd < 0) return STATUS_SUCCESS;

   struct pollfd pfd = { .fd = conn_fd, .events = POLLIN };
   const int rc = poll(&pfd, 1, (int)p->timeout_ms);
   if (rc < 0) {
      if (errno != EINTR && errno != EAGAIN)
         p->result = -2;
      return STATUS_SUCCESS;
   }
   drain_idle_notify(p->released_count);
   p->result = 0;
   return STATUS_SUCCESS;
}

NTSTATUS npt_do_wsi_destroy(void *args)
{
   (void)args;
   if (!g_wsi.conn) return STATUS_SUCCESS;

   for (uint32_t i = 0; i < g_wsi.num_images; i++) {
#ifdef NPT_HAVE_XCB_DRI3_SYNCOBJ
      if (g_wsi.images[i].acquire_syncobj != XCB_NONE)
         xcb_dri3_free_syncobj(g_wsi.conn, g_wsi.images[i].acquire_syncobj);
#endif
      if (g_wsi.images[i].pixmap)
         xcb_free_pixmap(g_wsi.conn, g_wsi.images[i].pixmap);
      if (g_wsi.images[i].fd >= 0)
         close(g_wsi.images[i].fd);
   }
   if (g_wsi.special_event)
      xcb_unregister_for_special_event(g_wsi.conn, g_wsi.special_event);
#ifdef NPT_HAVE_XCB_DRI3_SYNCOBJ
   if (g_wsi.drm_fd >= 0)
      close(g_wsi.drm_fd);
#endif
   xcb_disconnect(g_wsi.conn);
   memset(&g_wsi, 0, sizeof(g_wsi));
   return STATUS_SUCCESS;
}
