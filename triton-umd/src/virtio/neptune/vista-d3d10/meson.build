# Copyright 2026 Turing Software LLC
# SPDX-License-Identifier: MIT
triton10_src = files(
  '../triton/triton.c', '../triton/tritonDDI.c', '../triton/tritonD3D10.c',
  '../triton/tritonResource.c', '../triton/tritonView.c', '../triton/tritonState.c',
  '../triton/tritonShader.c', '../triton/tritonDxbc.c', '../triton/tritonQuery.c',
  '../triton/tritonDxgi.c', '../triton/tritonPresent.c', '../triton/tritonSharedBridge.c',
  '../vista-d3d9/triton9_guids.c', '../npt_entry_d3d11.c',
)
triton10_args = npt_c_args + ['-DD3D_UMD_INTERFACE_VERSION=0x000C',
  '-DDXGKDDI_INTERFACE_VERSION=0x1052', '-D_WIN32_WINNT=0x0600', '-DWINVER=0x0600',
  '-Wno-missing-prototypes', '-DNPT_D3D10_RUNTIME_DDI', '-DNPT_NO_D3D11_ENTRYPOINTS']
triton10_defs = 'neptune_d3d10.def'
if host_machine.cpu_family() == 'x86'
  triton10_defs = 'neptune_d3d10_x86.def'
endif
libneptune_d3d10 = shared_library('neptune_d3d10', [npt_common_src, triton10_src],
  include_directories : [npt_incs, include_directories('..', '../triton')],
  dependencies : npt_deps, c_args : triton10_args,
  link_args : ['-static', '-ldxguid', '-Wl,--subsystem,windows:6.0', '-Wl,--allow-multiple-definition'],
  vs_module_defs : triton10_defs, name_prefix : '', gnu_symbol_visibility : 'hidden', install : true)

# Public runtime probes deliberately load Microsoft's D3D10 entrypoints at
# runtime, so an absent runtime is a reported failure rather than loader loss.
triton10_probe_args = ['-D_WIN32_WINNT=0x0600', '-DWINVER=0x0600',
  '-Wno-unused-function', '-Wno-missing-prototypes']
triton10_runtime_probe = executable('triton10_runtime_probe',
  '../../../../../tests/vista/test-d3d10-runtime.c',
  c_args : triton10_probe_args,
  link_args : ['-static', '-ldxguid', '-Wl,--subsystem,console:6.0'], install : false)
triton10_present_probe = executable('triton10_present_probe',
  '../../../../../tests/vista/test-d3d10-present-runtime.c',
  c_args : triton10_probe_args,
  link_args : ['-static', '-ldxguid', '-luser32', '-lgdi32', '-Wl,--subsystem,console:6.0'], install : false)
