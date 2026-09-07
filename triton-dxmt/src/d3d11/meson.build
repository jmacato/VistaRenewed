d3d11_res = wrc_generator.process('version.rc')

d3d11_src = [
  'd3d11_class_linkage.cpp',
  'd3d11_device.cpp',
  'd3d11_input_layout.cpp',
  'd3d11_inspection.cpp',
  'd3d11_query.cpp',
  'd3d11_shader.cpp',
  'd3d11_state_object.cpp',
  'd3d11_swapchain.cpp',
  'd3d11_texture.cpp',
  'd3d11_buffer.cpp',
  'd3d11.cpp',
  'd3d11_resource_helper.cpp',
  'd3d11_resource_view_helper.cpp',
  'd3d11_texture_device.cpp',
  'd3d11_texture_shared_native.cpp',
  'd3d11_texture_linear.cpp',
  'd3d11_texture_dynamic.cpp',
  'd3d11_resource_staging.cpp',
  'd3d11_pipeline.cpp',
  'd3d11_pipeline_gs.cpp',
  'd3d11_pipeline_ts.cpp',
  'd3d11_enumerable.cpp',
  'd3d11_pipeline_cache.cpp',
  'd3d11_context_imm.cpp',
  'd3d11_context_def.cpp',
  'd3d11_multithread.cpp',
  'd3d11_fence.cpp',
]

d3d10_src = [
  '../d3d10/d3d10_buffer.cpp',
  '../d3d10/d3d10_device.cpp',
  '../d3d10/d3d10_state_object.cpp',
  '../d3d10/d3d10_texture.cpp',
  '../d3d10/d3d10_util.cpp',
  '../d3d10/d3d10_view.cpp',
]

d3d11_ld_args      = ['-L' + windows_native_install_dir] # a nice trick to force relink
d3d11_link_depends = []

d3d11_deps = [
  dxgi_dep,
  dxbc_parser_dep,
  airconv_forward_dep,
  dxmt_dep,
  winemetal_dep,
  util_dep ]

if dxmt_crossbuild
  d3d11_lib = shared_library('d3d11', d3d11_src, d3d10_src, d3d11_res,
    name_prefix         : '',
    dependencies        : d3d11_deps,
    include_directories : [ dxmt_include_path ],
    install             : true,
    install_dir         : windows_native_install_dir,
    vs_module_defs      : 'd3d11.def',
    link_args           : d3d11_ld_args,
    link_depends        : [ d3d11_link_depends ],
  )
else
  # Native: linked into the single dylib (see src/dxmt-native).
  d3d11_lib = static_library('d3d11', d3d11_src, d3d10_src, d3d11_res,
    dependencies        : d3d11_deps,
    include_directories : [ dxmt_include_path ],
  )
endif

d3d11_dep = declare_dependency(
  link_with           : [ d3d11_lib ],
  include_directories : [ dxmt_include_path ],
)

if wine_builtin_dll
custom_target('postprocess_libd3d11',
  input : d3d11_lib,
  output: 'd3d11.dll.postproc',
  command : [ winebuild, '--builtin', '@INPUT@' ],
  depends : d3d11_lib,
  build_by_default : true
)
endif
