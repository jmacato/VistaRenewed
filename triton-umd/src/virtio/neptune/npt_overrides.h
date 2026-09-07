/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Each override family exposes an init() that patches vtbls and/or
 * registers with npt_com_register_family.  All inits run once from
 * npt_com_init_impl after the default-ctor populator.
 */

#ifndef NPT_OVERRIDES_H
#define NPT_OVERRIDES_H

#include <stdint.h>

void npt_overrides_d3d11_device_init(void);
void npt_overrides_d3d11_context_init(void);
void npt_overrides_d3d11_buffer_init(void);
void npt_overrides_d3d11_texture_init(void);
void npt_overrides_d3d11_view_init(void);
void npt_overrides_d3d11_fence_init(void);
void npt_overrides_d3d11_query_init(void);
void npt_overrides_dxgi_output_init(void);
void npt_overrides_dxgi_factory_init(void);
void npt_overrides_dxgi_factorymedia_init(void);
void npt_overrides_dxgi_adapter_init(void);

struct npt_com_base;
struct npt_output_info;

/* Stamp an output wrapper's aux from connector data; idempotent.
 * Called by IDXGIAdapter::EnumOutputs after npt_com_get_or_wrap. */
void
npt_output_aux_populate(struct npt_com_base *out_com,
                        struct npt_com_base *parent_adapter,
                        uint32_t connector_index,
                        const struct npt_output_info *info);

#endif /* NPT_OVERRIDES_H */
