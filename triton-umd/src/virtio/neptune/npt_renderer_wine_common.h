/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Wine-PE shims for both wine backends (vtest + virtgpu).
 */
#ifndef NPT_RENDERER_WINE_COMMON_H
#define NPT_RENDERER_WINE_COMMON_H

#ifdef __WINE__

#include <stddef.h>
#include <stdint.h>

#include "nptunix/npt_unixlib.h"

struct npt_renderer;
struct npt_renderer_ops;

void npt_wine_unixlib_close(int fd);
int  npt_wine_unixlib_dup(int fd);
void npt_wine_unixlib_munmap(void *addr, size_t size);

/* wait_fence_fd ownership: closed here unless the unixlib reports
 * wait_fence_consumed=1 (xcb took it via SCM_RIGHTS). */
void npt_wine_install_wsi_ops(struct npt_renderer_ops *ops);

#endif /* __WINE__ */

#endif /* NPT_RENDERER_WINE_COMMON_H */
