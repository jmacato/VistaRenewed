/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Per-connector DRM enumeration backing the guest IDXGIOutput surface.
 * Picks the viogpu primary node first; falls back to any KMS device
 * (dev-host / vtest).
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "../npt_display.h"
#include "npt_unix_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

#define NPT_VIRTGPU_PCI_VENDOR_ID 0x1af4
#define NPT_VIRTGPU_PCI_DEVICE_ID 0x1050

static void
npt_drm_mode_to_npt(const drmModeModeInfo *m, struct npt_display_mode *out)
{
   out->width  = m->hdisplay;
   out->height = m->vdisplay;
   /* Integer rational so DXGI sees exact 60000/1001 etc. */
   uint32_t vscan = m->vscan ? m->vscan : 1;
   out->refresh_num = m->clock * 1000u;
   out->refresh_den = (uint32_t)m->htotal * (uint32_t)m->vtotal * vscan;
   out->bpp         = 32;
   out->preferred   = (m->type & DRM_MODE_TYPE_PREFERRED) != 0;
   out->interlaced  = (m->flags & DRM_MODE_FLAG_INTERLACE) != 0;
}

static int
npt_mode_compare(const void *a, const void *b)
{
   const struct npt_display_mode *ma = a;
   const struct npt_display_mode *mb = b;
   if (ma->width != mb->width)
      return (ma->width < mb->width) ? -1 : 1;
   if (ma->height != mb->height)
      return (ma->height < mb->height) ? -1 : 1;
   uint64_t la = (uint64_t)ma->refresh_num * mb->refresh_den;
   uint64_t lb = (uint64_t)mb->refresh_num * ma->refresh_den;
   if (la != lb)
      return (la < lb) ? -1 : 1;
   return 0;
}

static bool
npt_mode_dedup_in(const struct npt_display_mode *m,
                  const struct npt_display_mode *list, uint32_t n)
{
   for (uint32_t i = 0; i < n; i++) {
      const struct npt_display_mode *e = &list[i];
      if (e->width == m->width && e->height == m->height &&
          (uint64_t)e->refresh_num * m->refresh_den ==
             (uint64_t)m->refresh_num * e->refresh_den &&
          e->interlaced == m->interlaced)
         return true;
   }
   return false;
}

static void
npt_connector_name(drmModeConnector *conn, char out[32])
{
   const char *type = drmModeGetConnectorTypeName(conn->connector_type);
   if (!type)
      type = "Unknown";
   snprintf(out, 32, "%s-%u", type, conn->connector_type_id);
}

/* Resolution the compositor currently has programmed on this connector's
 * CRTC.  The guest cannot modeset, so whatever the KMS master put on the CRTC
 * is the real scanout ceiling regardless of the larger modes the connector
 * advertises.  Returns 0x0 if the connector is not driven by a CRTC with a
 * valid mode. */
static void
npt_active_crtc_mode(int drm_fd, const drmModeConnector *conn,
                     uint32_t *out_width, uint32_t *out_height)
{
   *out_width = 0;
   *out_height = 0;
   if (!conn->encoder_id)
      return;
   drmModeEncoder *enc = drmModeGetEncoder(drm_fd, conn->encoder_id);
   if (!enc)
      return;
   if (enc->crtc_id) {
      drmModeCrtc *crtc = drmModeGetCrtc(drm_fd, enc->crtc_id);
      if (crtc) {
         if (crtc->mode_valid) {
            *out_width  = crtc->mode.hdisplay;
            *out_height = crtc->mode.vdisplay;
         }
         drmModeFreeCrtc(crtc);
      }
   }
   drmModeFreeEncoder(enc);
}

static bool
npt_output_from_connector(int drm_fd, drmModeConnector *conn,
                          struct npt_output_info *out, bool expose_all_modes)
{
   memset(out, 0, sizeof(*out));
   npt_connector_name(conn, out->name);

   /* Cap the advertised list to the active CRTC scanout.  The connector lists
    * modes larger than the compositor's active mode (e.g. 5120x2160 while the
    * session runs smaller); since the guest cannot scan those out, reporting
    * them through IDXGIOutput::GetDisplayModeList would let an app pick a
    * resolution the display can never show and size its swapchain past it.
    * expose_all_modes leaves the raw KMS list in place for modeset work. */
   uint32_t cap_w = 0, cap_h = 0;
   if (!expose_all_modes)
      npt_active_crtc_mode(drm_fd, conn, &cap_w, &cap_h);

   for (int i = 0; i < conn->count_modes &&
                   out->num_modes < NPT_DISPLAY_MAX_MODES; i++) {
      struct npt_display_mode tmp;
      npt_drm_mode_to_npt(&conn->modes[i], &tmp);
      if (!tmp.refresh_den)
         continue;
      /* Drop anything the compositor cannot actually present. */
      if (cap_w && cap_h && (tmp.width > cap_w || tmp.height > cap_h))
         continue;
      if (npt_mode_dedup_in(&tmp, out->modes, out->num_modes))
         continue;
      out->modes[out->num_modes++] = tmp;
   }

   if (cap_w && cap_h) {
      /* Current mode is the active scanout.  Reported even if no entry of the
       * capped list matches it. */
      out->current_width  = cap_w;
      out->current_height = cap_h;
   } else {
      /* No CRTC bound, or exposing all modes: fall back to
       * preferred-or-largest. */
      const struct npt_display_mode *picked = NULL;
      for (uint32_t i = 0; i < out->num_modes; i++) {
         const struct npt_display_mode *m = &out->modes[i];
         if (m->preferred) { picked = m; break; }
         if (!picked || (uint64_t)m->width * m->height >
                        (uint64_t)picked->width * picked->height)
            picked = m;
      }
      if (picked) {
         out->current_width  = picked->width;
         out->current_height = picked->height;
      } else {
         out->current_width  = 1024;
         out->current_height = 768;
      }
   }

   if (out->num_modes > 1)
      qsort(out->modes, out->num_modes, sizeof(out->modes[0]),
            npt_mode_compare);

   return out->num_modes > 0;
}

static bool
npt_enumerate_outputs_drm(int drm_fd, struct npt_output_list *list,
                          bool expose_all_modes)
{
   /* Not required for connector queries; EACCES on read-only fds ignored. */
   drmSetClientCap(drm_fd, DRM_CLIENT_CAP_ATOMIC, 1);

   drmModeRes *res = drmModeGetResources(drm_fd);
   if (!res)
      return false;

   int32_t x_offset = 0;
   for (int i = 0; i < res->count_connectors &&
                   list->num_outputs < NPT_OUTPUT_MAX; i++) {
      drmModeConnector *c = drmModeGetConnector(drm_fd, res->connectors[i]);
      if (!c)
         continue;
      if (c->connection != DRM_MODE_CONNECTED || c->count_modes == 0) {
         drmModeFreeConnector(c);
         continue;
      }
      struct npt_output_info *info = &list->outputs[list->num_outputs];
      if (npt_output_from_connector(drm_fd, c, info, expose_all_modes)) {
         /* Synthetic horizontal-stack desktop arrangement; the
          * windowed Neptune model has no real CRTC layout. */
         info->desktop_x = x_offset;
         info->desktop_y = 0;
         x_offset += (int32_t)info->current_width;
         list->num_outputs++;
      }
      drmModeFreeConnector(c);
   }

   drmModeFreeResources(res);
   return list->num_outputs > 0;
}

static bool
npt_drm_dev_is_pci_virtgpu(const drmDevicePtr dev)
{
   return dev->bustype == DRM_BUS_PCI &&
          dev->deviceinfo.pci != NULL &&
          dev->deviceinfo.pci->vendor_id == NPT_VIRTGPU_PCI_VENDOR_ID &&
          dev->deviceinfo.pci->device_id == NPT_VIRTGPU_PCI_DEVICE_ID;
}

int
npt_pick_drm_device_paths(char primary[], size_t primary_sz,
                          char render[], size_t render_sz)
{
   if (primary && primary_sz)  primary[0] = 0;
   if (render && render_sz)    render[0] = 0;

   drmDevicePtr devs[16];
   int count = drmGetDevices2(0, devs, (int)(sizeof(devs)/sizeof(devs[0])));
   if (count < 0)
      return -ENOENT;

   int virtgpu_seen = 0;
   for (int i = 0; i < count; i++)
      if (npt_drm_dev_is_pci_virtgpu(devs[i]))
         virtgpu_seen++;

   static int s_warned_multi;
   if (virtgpu_seen > 1 &&
       !__atomic_exchange_n(&s_warned_multi, 1, __ATOMIC_RELAXED)) {
      fprintf(stderr,
              "neptune: %d virtio-gpu devices detected; using the first\n",
              virtgpu_seen);
   }

   int ret = -ENOENT;
   for (int pass = 0; pass < 2 && ret != 0; pass++) {
      for (int i = 0; i < count && ret != 0; i++) {
         drmDevicePtr dev = devs[i];
         bool is_vio = npt_drm_dev_is_pci_virtgpu(dev);
         if (pass == 0 && !is_vio) continue;
         if (pass == 1 &&  is_vio) continue;
         if (!(dev->available_nodes & (1 << DRM_NODE_PRIMARY)))
            continue;
         int probe = open(dev->nodes[DRM_NODE_PRIMARY], O_RDWR | O_CLOEXEC);
         if (probe < 0) continue;
         close(probe);
         if (primary && primary_sz)
            snprintf(primary, primary_sz, "%s", dev->nodes[DRM_NODE_PRIMARY]);
         if (render && render_sz &&
             (dev->available_nodes & (1 << DRM_NODE_RENDER)))
            snprintf(render, render_sz, "%s", dev->nodes[DRM_NODE_RENDER]);
         ret = 0;
      }
   }

   drmFreeDevices(devs, count);
   return ret;
}

bool
npt_enumerate_outputs(struct npt_output_list *list, bool expose_all_modes)
{
   if (!list)
      return false;
   memset(list, 0, sizeof(*list));

   char primary[256];
   if (npt_pick_drm_device_paths(primary, sizeof(primary), NULL, 0) != 0)
      return false;

   int fd = open(primary, O_RDWR | O_CLOEXEC);
   if (fd < 0)
      return false;

   bool ok = npt_enumerate_outputs_drm(fd, list, expose_all_modes);
   close(fd);
   return ok;
}
