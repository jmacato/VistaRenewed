# Copyright 2026 Turing Software LLC
# SPDX-License-Identifier: MIT

triton9_src = files(
  'triton9_cpu_layout.c',
  'triton9_draw_contract.c',
  'triton9_ddi.c',
  'triton9_resource.c',
  'triton9_format.c',
  'triton9_unsupported.c',
  'triton9_output.c',
  'triton9_blit.c',
  'triton9_query.cpp',
  'triton9_state.cpp',
  'triton9_shader.cpp',
  '../triton/tritonDxbc.c',
  '../triton/tritonSharedBridge.c',
)

triton9_shaderconv_src = files(
  'third_party/d3d9on12-shaderconverter/ShaderBinary/ShaderBinary.cpp',
  'third_party/d3d9on12-shaderconverter/ShaderConv/ShaderBinaryEx.cpp',
  'third_party/d3d9on12-shaderconverter/ShaderConv/ShaderConv.cpp',
  'third_party/d3d9on12-shaderconverter/ShaderConv/context.cpp',
  'third_party/d3d9on12-shaderconverter/ShaderConv/disasm20.cpp',
  'third_party/d3d9on12-shaderconverter/ShaderConv/gsconv.cpp',
  'third_party/d3d9on12-shaderconverter/ShaderConv/psconv.cpp',
  'third_party/d3d9on12-shaderconverter/ShaderConv/tlvsconv.cpp',
  'third_party/d3d9on12-shaderconverter/ShaderConv/translator.cpp',
  'third_party/d3d9on12-shaderconverter/ShaderConv/vsconv.cpp',
)

triton9_incs = [
  npt_incs,
  include_directories('.'),
  include_directories('..'),
  include_directories('../triton'),
  include_directories('third_party/d3d9on12-shaderconverter/Inc'),
  include_directories('third_party/d3d9on12-shaderconverter/ShaderConv'),
  include_directories('third_party/d3d9on12-shaderconverter/ShaderBinary'),
]

# The numeric definitions are duplicated in triton9.h because d3dukmdt.h
# consumes them before d3dumddi.h is parsed.  Passing them here ensures every
# TU, including the shared bridge, sees the same Vista layout.
triton9_c_args = npt_c_args + [
  '-DD3D_UMD_INTERFACE_VERSION=0x000C',
  '-DDXGKDDI_INTERFACE_VERSION=0x1052',
  '-D_WIN32_WINNT=0x0600',
  '-DWINVER=0x0600',
  '-DNPT_D3D9_RUNTIME_DDI',
]

triton9_module_defs = 'neptune_d3d9.def'
# GCC before 14 cannot select a CRT with -mcrtdll. Debian/Ubuntu MinGW
# already targets MSVCRT; reject a UCRT default instead of changing it silently.
triton9_msvcrt_args = cc.get_supported_link_arguments('-mcrtdll=msvcrt')
if triton9_msvcrt_args.length() == 0
  if not cc.compiles('''
    #include <stdlib.h>
    #ifdef _UCRT
    #error Vista requires MSVCRT
    #endif
    int main(void) { return 0; }
  ''', name : 'Vista toolchain defaults to MSVCRT')
    error('Vista needs an MSVCRT MinGW toolchain or -mcrtdll=msvcrt support')
  endif
endif
if host_machine.cpu_family() == 'x86'
  # The Vista D3D9 entry point uses stdcall on 32-bit Windows.  Export its
  # decorated object symbol under the undecorated name the runtime loads.
  # Relying on GNU ld's stdcall fixup works by accident and hides ABI drift
  # behind a linker warning.
  triton9_module_defs = 'neptune_d3d9_x86.def'
endif

libneptune_d3d9 = shared_library(
  'neptune_d3d9',
  [npt_common_src, triton9_src, triton9_shaderconv_src,
   files('../npt_entry_d3d11.c')],
  include_directories : triton9_incs,
  dependencies : npt_deps,
  c_args : triton9_c_args + ['-DNPT_NO_D3D11_ENTRYPOINTS'],
  cpp_args : triton9_c_args,
  # dxguid supplies IID_ID3D11Device1 and IID_ID3D11DeviceContext1.  There is
  # deliberately no d3d11.lib/dxgi.lib here: the UMD only uses Neptune's
  # guest-side proxy creator and must not import the OS D3D11 stack on Vista.
  link_args : triton9_msvcrt_args + ['-static', '-ldxguid', '-Wl,--subsystem,windows:6.0'],
  vs_module_defs : triton9_module_defs,
  name_prefix : '',
  gnu_symbol_visibility : 'hidden',
  install : true,
)

# This target deliberately does not link a runnable program.  The probe
# references private UMD symbols and exists to make each Vista cross-build
# compile the ABI assertions and DDI callback assignments.
static_library(
  'triton9_abi_compile',
  files('tests/triton9_abi_test.c'),
  include_directories : triton9_incs,
  c_args : triton9_c_args,
)

# Vista owns the backing buffer of D3DDDIPOOL_SYSTEMMEM resources.  Exercise
# that padded-pitch copy contract on the build host so a private-shadow
# regression is caught before a driver package reaches the guest.
triton9_cpu_layout_test = executable(
  'triton9_cpu_layout_test',
  files('triton9_cpu_layout.c', 'tests/triton9_cpu_layout_test.c'),
  native : true,
  install : false,
)
test('triton9-cpu-layout', triton9_cpu_layout_test)

# The clear rectangle contract is platform-neutral.  Compile the production
# normalization code in portable mode and exercise every DDI rectangle shape.
triton9_clear_contract_test = executable(
  'triton9_clear_contract_test',
  files('tests/triton9_clear_contract_test.c'),
  native : true,
  install : false,
)
test('triton9-clear-contract', triton9_clear_contract_test)

# Vista's UP callbacks split the pointer, byte offsets, and element counts
# across separate DDI calls.  Keep those overflow and normalization rules in a
# host test so both cross-builds consume the same proven arithmetic.
triton9_draw_contract_test = executable(
  'triton9_draw_contract_test',
  files('triton9_draw_contract.c', 'tests/triton9_draw_contract_test.c'),
  native : true,
  install : false,
)
test('triton9-draw-contract', triton9_draw_contract_test)

# Vista's public runtime validates SM2 declaration-info parameter tokens
# before it enters the UMD.  Pin the exact fxc-era encodings used by the probe.
triton9_shader_token_contract_test = executable(
  'triton9_shader_token_contract_test',
  files('tests/triton9_shader_token_contract_test.c'),
  native : true,
  install : false,
)
test('triton9-shader-token-contract', triton9_shader_token_contract_test)

# Keep the public-runtime bring-up probe in both Vista cross-builds.  It
# dynamically resolves Direct3DCreate9Ex, so a successful PE audit also
# proves that the test did not accidentally link against a newer D3D DLL.
executable(
  'triton9_runtime_probe',
  files('tests/triton9_runtime_probe.c'),
  c_args : [
    '-D_WIN32_WINNT=0x0600',
    '-DWINVER=0x0600',
  ],
  link_args : triton9_msvcrt_args + [
    '-static',
    '-ladvapi32',
    '-luser32',
    '-Wl,--subsystem,console:6.0',
  ],
  install : false,
)
