/* Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Older MinGW dxguid archives lack the D3D11 fence interface IDs. Emit
 * the header definitions locally so both Vista architectures can link
 * the Neptune fence path without importing the Windows D3D11 runtime.
 */
#include <initguid.h>
#include <d3d11_4.h>
