/* Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT */
#ifndef NPT_RUNTIME_BINDING_H
#define NPT_RUNTIME_BINDING_H
/* Included after the runtime-specific WDK headers. Both D3D9 and D3D10
 * supply D3DDDI adapter/kernel callback tables at the selected WDDM ABI. */
void npt_renderer_bind_runtime(HANDLE adapter, HANDLE device,
    UINT interface_version, UINT runtime_version,
    const D3DDDI_ADAPTERCALLBACKS *adapter_callbacks,
    const D3DDDI_DEVICECALLBACKS *device_callbacks);
#endif
