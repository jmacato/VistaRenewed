#!/usr/bin/env python3
from pathlib import Path
from harness import extract_section, run_harness
source = Path(__file__).resolve().parents[2] / 'triton-umd/src/virtio/neptune/npt_overrides_d3d11_fence.c'
helper = extract_section(source, 'static UINT64 NPT_STDMETHODCALLTYPE\nfence_GetCompletedValue_override(', '\nstatic HRESULT NPT_STDMETHODCALLTYPE\nfence_SetEventOnCompletion_override(')
helper += extract_section(source.with_name('npt_overrides_d3d11_device.c'), 'static HRESULT NPT_STDMETHODCALLTYPE\ndev_GetDeviceRemovedReason_override(', '\nvoid\nnpt_overrides_d3d11_device_init(')
run_harness(Path(__file__).with_suffix('.c'), helper)
