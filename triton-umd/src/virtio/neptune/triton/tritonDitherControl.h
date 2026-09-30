/* Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 * Private D3D11 context control ABI. Keep the guest and host copies identical.
 */
#ifndef TRITON_DITHER_CONTROL_H
#define TRITON_DITHER_CONTROL_H
#include <stdint.h>
#define TRITON_DITHER_VERSION 1u
#define TRITON_DITHER_NATIVE 1u
#define TRITON_DITHER_CAPS_GUID {0x7d40f743,0x4f61,0x4a49,{0xa3,0x25,0xe9,0x75,0x36,0xf7,0x03,0x98}}
#define TRITON_DITHER_STATE_GUID {0x4b245d9d,0x2e17,0x4c38,{0x91,0xb2,0x67,0x84,0xdd,0x4a,0x2b,0xd6}}
/* CAPS is read-only. STATE Get/Set uses version=1 and enabled=0 or 1.
 * GetPrivateData follows the standard size-query/MORE_DATA convention.
 * Invalid schemas and interface-valued writes return E_INVALIDARG.
 * Enabling without NATIVE support returns DXGI_ERROR_UNSUPPORTED.
 */
struct TritonDitherCaps { uint32_t version; uint32_t flags; };
struct TritonDitherState { uint32_t version; uint32_t enabled; };
#endif
