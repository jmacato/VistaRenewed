/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * ID3D11Buffer has no per-method overrides; Map/Unmap go through the
 * immediate-context override.  This file only registers aux_size /
 * aux_init for the runtime.
 */

#include "npt_com.h"
#include "npt_overrides.h"
#include "npt_resource.h"

#include "neptune-protocol/npt_protocol_defs.h"

static const GUID *const buffer_tiers[] = { &NPT_IID_ID3D11Buffer, NULL };

void
npt_overrides_d3d11_buffer_init(void)
{
   npt_com_register_family(buffer_tiers,
                           sizeof(struct npt_d3d11_buffer_aux),
                           npt_d3d11_buffer_aux_init);
}
