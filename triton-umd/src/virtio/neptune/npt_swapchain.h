/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Guest-fabricated IDXGISwapChain construction for the Wine presentation
 * path.  The implementation lives in npt_swapchain.c.
 */

#ifndef NPT_SWAPCHAIN_H
#define NPT_SWAPCHAIN_H

#include <stdint.h>

#include "neptune-protocol/npt_protocol_directx_types.h"

/*
 * Build a guest-fabricated IDXGISwapChain (all tiers) over
 * device_unknown (any Neptune wrapper of the D3D11 device).  The
 * factory_wrapper (may be NULL) is retained for GetParent.  On Wine
 * this is the real presentation path (X11 DRI3/Present); elsewhere it
 * returns E_NOTIMPL, matching the host's refusal.  *out_swapchain is
 * the wrapper (usable as any IDXGISwapChain* tier) with one public
 * reference.
 */
HRESULT
npt_guest_swapchain_create(void *device_unknown,
                           const DXGI_SWAP_CHAIN_DESC1 *desc1,
                           const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fs_desc,
                           HWND hwnd,
                           void *factory_wrapper,
                           void **out_swapchain);

/* DXGI_SWAP_CHAIN_DESC (v0) convenience wrapper for CreateSwapChain /
 * D3D11CreateDeviceAndSwapChain. */
HRESULT
npt_guest_swapchain_create_legacy(void *device_unknown,
                                  const DXGI_SWAP_CHAIN_DESC *desc,
                                  void *factory_wrapper,
                                  void **out_swapchain);

#endif /* NPT_SWAPCHAIN_H */
