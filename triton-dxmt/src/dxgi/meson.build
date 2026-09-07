dxgi_res = wrc_generator.process('version.rc')

dxgi_src = [
  'dxgi_adapter.cpp',
  'dxgi_factory.cpp',
  'dxgi_output.cpp',
  'dxgi_options.cpp',
  'dxgi.cpp',
]

dxgi_ld_args      = ['-L' + windows_native_install_dir] # a nice trick to force relink
dxgi_link_depends = []

dxgi_deps = [ dxmt_dep, util_dep, winemetal_dep ]

if dxmt_crossbuild
  dxgi_lib = shared_library('dxgi', dxgi_src, dxgi_res,
    name_prefix         : '',
    dependencies        : dxgi_deps,
    include_directories : [ dxmt_include_path ],
    install             : true,
    install_dir         : windows_native_install_dir,
    vs_module_defs      : 'dxgi.def',
    link_args           : dxgi_ld_args,
    link_depends        : [ dxgi_link_depends ],
  )
else
  # Native: linked into the single dylib (see src/dxmt-native).
  dxgi_lib = static_library('dxgi', dxgi_src, dxgi_res,
    dependencies        : dxgi_deps,
    include_directories : [ dxmt_include_path ],
  )
endif

dxgi_dep = declare_dependency(
  link_with           : [ dxgi_lib ],
  include_directories : [ dxmt_include_path, include_directories('.') ],
)

if wine_builtin_dll
custom_target('postprocess_libdxgi',
  input : dxgi_lib,
  output: 'dxgi.dll.postproc',
  command : [ winebuild, '--builtin', '@INPUT@' ],
  depends : dxgi_lib,
  build_by_default : true
)
endif
