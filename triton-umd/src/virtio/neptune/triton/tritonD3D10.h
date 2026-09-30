/* Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT */
#ifndef TRITON_D3D10_H
#define TRITON_D3D10_H
#include "triton.h"
void tritonFillD3D11DeviceFuncs(D3D11DDI_DEVICEFUNCS *funcs);
void tritonFillD3D10DeviceFuncs(D3D10DDI_DEVICEFUNCS *funcs);
void tritonFillD3D10_1DeviceFuncs(D3D10_1DDI_DEVICEFUNCS *funcs);
HRESULT APIENTRY OpenAdapter10(D3D10DDIARG_OPENADAPTER *args);
void *tritonD3D10BuildSOAlias(const void *entries, UINT count, UINT stride, SIZE_T *bytes);
void *tritonD3D10BlitBytecode(const void *data, SIZE_T size, SIZE_T *bytes);
#endif
