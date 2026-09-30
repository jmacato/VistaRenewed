# Copyright 2026 Turing Software LLC
# SPDX-License-Identifier: MIT

npt_common_src = files(
  'npt_com.c',
  'npt_cs.c',
  'npt_device.c',
  'npt_dispatch.c',
  'npt_env.c',
  'npt_event.c',
  'npt_profile.c',
  'npt_ring.c',
  'npt_tls.c',
  'npt_shmem_pool.c',
  'npt_resource.c',
  'npt_shared_texture.c',
  'npt_swapchain.c',
  'npt_overrides_d3d11_buffer.c',
  'npt_overrides_d3d11_context.c',
  'npt_overrides_d3d11_device.c',
  'npt_overrides_d3d11_fence.c',
  'npt_overrides_d3d11_query.c',
  'npt_overrides_d3d11_texture.c',
  'npt_overrides_d3d11_view.c',
  'npt_overrides_dxgi_adapter.c',
  'npt_overrides_dxgi_factory.c',
  'npt_overrides_dxgi_factorymedia.c',
  'npt_overrides_dxgi_output.c',
)

# Generated client-side COM sources from neptune-protocol.  One out-of-line
# .c per interface family + a global IID -> default-ctor table.  These are
# committed in-tree under neptune-protocol/ alongside the encode/decode
# headers, regenerated from the upstream `neptune-protocol/builddir`
# whenever the protocol changes.
#
# Each .c file holds the default thunk bodies, the fill_default_vtbl
# populator, and the default constructor for one interface family.  Hand-
# written wrapper files (npt_overrides_<family>.c) override individual
# slots via npt_overrides_<family>_init() functions called from
# npt_com_init().
#
# The family list is read from neptune-protocol/npt_client_families.txt
# which the generator emits alongside the .c/.h files, so a protocol
# update only requires re-running the generator and copying in the new
# files -- no hand edit to this meson.build.
npt_client_families = import('fs').read(
  'neptune-protocol/npt_client_families.txt',
).strip().split('\n')

foreach family : npt_client_families
  npt_common_src += files(
    'neptune-protocol/npt_protocol_client_' + family + '.c'
  )
endforeach
npt_common_src += files(
  'neptune-protocol/npt_protocol_client_table.c'
)

npt_incs = [
  inc_include,
  inc_src,
  inc_virtio,
]

npt_deps = [
  dep_thread,
  idep_mesautil,
]

npt_c_args = [
  no_override_init_args,
]

# The vendored protocol output intentionally carries GCC diagnostic pragmas
# around generated inline helpers.  They are inert under MSVC, which otherwise
# emits C4068 for every push/ignored/pop directive and floods native builds.
if cc.get_argument_syntax() == 'msvc'
  npt_c_args += ['/wd4068']
endif

npt_is_windows = host_machine.system() == 'windows'

# vtest + virtgpu backends.  The Windows build defaults to the Wine PE
# pair (which routes through the nptunix unixlib bridge); flip
# -Dnpt_wine=false to target native Windows.
if npt_is_windows
  if get_option('npt_wine')
    npt_common_src += files(
      'npt_renderer_vtest_wine.c',
      'npt_renderer_vtest_protocol.c',
      'npt_renderer_virtgpu_wine.c',
      'npt_renderer_wine_common.c',
      'npt_common_wine.c',
    )
    npt_c_args += ['-D__WINE__']
  else
    npt_common_src += files(
      'npt_renderer_vtest_protocol.c',
      'npt_renderer_virtgpu_win32.c',
      'npt_common_win32.c',
    )
  endif
endif

if not npt_is_windows or get_option('npt_wine')
  subdir('nptunix')
endif

if npt_is_windows and get_option('npt_umd') != 'off'
  subdir('triton')
endif

# D3D9's DDI table is selected at compile time.  Keep its Vista build in a
# separate target so Mesa's modern Windows minimum version and Triton's
# D3D10/11 DDI interface version cannot change its ABI by accident.
if npt_is_windows and get_option('npt_vista_d3d9')
  subdir('vista-d3d9')
endif

if npt_is_windows and get_option('npt_vista_d3d10')
  subdir('vista-d3d10')
endif

if npt_is_windows and not get_option('npt_vista_d3d9') and not get_option('npt_vista_d3d10')
  # Windows: produce d3d11.dll and dxgi.dll as native DLLs for Wine
  # (MinGW cross-compile, installed to Wine prefix system32 like dxvk).
  # The nptunix.dll helper builtin provides unixlib access.

  libd3d11_npt = shared_library(
    'd3d11',
    [npt_common_src, files('npt_entry_d3d11.c')],
    include_directories : npt_incs,
    dependencies : npt_deps,
    c_args : npt_c_args,
    vs_module_defs : 'd3d11.def',
    name_prefix : '',
    gnu_symbol_visibility : 'hidden',
    install : true,
  )

  libdxgi_npt = shared_library(
    'dxgi',
    [npt_common_src, files('npt_entry_dxgi.c')],
    include_directories : npt_incs,
    dependencies : npt_deps,
    c_args : npt_c_args,
    vs_module_defs : 'dxgi.def',
    name_prefix : '',
    gnu_symbol_visibility : 'hidden',
    install : true,
  )
endif
# Linux build only produces libnptunix above -- d3d11.dll/dxgi.dll are
# Wine PE artifacts.
