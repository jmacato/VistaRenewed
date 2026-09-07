# Copyright 2026 Turing Software LLC
# SPDX-License-Identifier: MIT

triton_src = files(
  'triton.c',
  'tritonDDI.c',
  'tritonResource.c',
  'tritonView.c',
  'tritonState.c',
  'tritonShader.c',
  'tritonDxbc.c',
  'tritonQuery.c',
  'tritonDxgi.c',
  'tritonPresent.c',
  'tritonSharedBridge.c',
)

triton_incs = [
  npt_incs,
  include_directories('.'),
  include_directories('..'),
]

libneptune_umd = shared_library(
  'neptune_umd',
  [npt_common_src, triton_src,
   files('../npt_entry_d3d11.c', '../npt_entry_dxgi.c')],
  include_directories : triton_incs,
  dependencies : npt_deps,
  c_args : npt_c_args,
  # d3d10umddi.h redeclares many D3D10_* numeric constants as file-scope
  # `const UINT` variables on top of the #define forms in d3d10.h.  In C
  # those have external linkage, so every TU that includes the umddi
  # header emits an identical symbol and the linker raises LNK2005.
  # Both definitions agree, so picking the first instance is safe.
  # dxguid.lib provides IID_ID3D11Device2/3 + IID_ID3D11DeviceContext2/3.
  link_args : ['/FORCE:MULTIPLE', '/IGNORE:4006', 'dxguid.lib'],
  vs_module_defs : 'neptune_umd.def',
  name_prefix : '',
  gnu_symbol_visibility : 'hidden',
  install : true,
)

if with_tests
  # Self-contained: #includes tritonDxbc.c and loads d3dcompiler_47.dll at
  # runtime as the reference compiler.
  test(
    'triton-dxbc',
    executable(
      'triton_dxbc_test',
      files('tests/tritonDxbcTest.c'),
      include_directories : triton_incs,
      c_args : npt_c_args,
    ),
    suite : ['neptune'],
  )
endif
