/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * DXGI DDI table installer.  The Microsoft DXGI runtime ships with the
 * OS; the user-mode driver only provides function pointers that the
 * runtime calls for Present, Blt, residency / priority, and the related
 * surface-management hooks.  tritonInstallDXGIFuncs fills whichever
 * DXGI*_DDI_BASE_FUNCTIONS struct the requested interface version asks
 * for, selected with the IS_DXGI*_BASE_FUNCTIONS macros from dxgiddi.h.
 */

#ifndef TRITON_DXGI_H_INCLUDED
#define TRITON_DXGI_H_INCLUDED

#include "triton.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Install Triton's DXGI DDI handlers into the DXGIBaseDDI union of the
 * incoming CREATEDEVICE arguments based on Interface / Version.  Logs
 * which tier was wired through TR_LOG. */
void tritonInstallDXGIFuncs(PTRITON_DEVICE pD,
                            const D3D10DDIARG_CREATEDEVICE *pArgs);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* TRITON_DXGI_H_INCLUDED */
