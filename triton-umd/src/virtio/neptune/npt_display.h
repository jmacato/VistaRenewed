/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * POD describing per-connector outputs of the guest viogpu (or, in
 * dev-host / vtest mode, the local KMS device).  Kept libdrm-free so
 * both npt_renderer.h and npt_unixlib.h can include it; the
 * libdrm-backed enumeration lives in nptunix/npt_display_drm.c.
 */

#ifndef NPT_DISPLAY_H
#define NPT_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#define NPT_DISPLAY_MAX_MODES    128
#define NPT_OUTPUT_MAX           8

struct npt_display_mode {
   uint32_t width;
   uint32_t height;
   /* DXGI_RATIONAL = num/den; integer rational from drm clock/htotal/vtotal. */
   uint32_t refresh_num;
   uint32_t refresh_den;
   uint32_t bpp;
   bool     preferred;
   bool     interlaced;
};

struct npt_output_info {
   /* DRM connector type name + id (e.g. "Virtual-1"); UTF-8, NUL-terminated. */
   char     name[32];
   int32_t  desktop_x, desktop_y;
   uint32_t current_width, current_height;
   uint32_t num_modes;
   struct npt_display_mode modes[NPT_DISPLAY_MAX_MODES];
};

struct npt_output_list {
   uint32_t num_outputs;
   uint32_t pad;
   struct npt_output_info outputs[NPT_OUTPUT_MAX];
};

#endif /* NPT_DISPLAY_H */
