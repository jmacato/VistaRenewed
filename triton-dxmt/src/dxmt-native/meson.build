# The single native image: every D3D module and the Metal backend link in
# here, so this is the only dylib the native build installs.

# link_whole, not link_with: nothing inside the image references the D3D
# entry points, so the linker would otherwise drop the objects defining them.
dxmt_native_whole = [ dxgi_lib, d3d11_lib, d3d10core_lib, winemetal_lib ]
dxmt_native_deps = [
  dxgi_dep,
  d3d11_dep,
  d3d10core_dep,
  winemetal_dep,
  dxmt_dep,
  util_dep,
  airconv_dep_darwin,
]

dxmt_native_export_lists = [ files('dxmt_native.exports') ]

if get_option('enable_d3d12')
  dxmt_native_whole += d3d12_lib
  dxmt_native_deps += d3d12_dep
  dxmt_native_export_lists += files('dxmt_native_d3d12.exports')
endif

# One list per module built into the image, concatenated: ld64 takes a single
# -exported_symbols_list and errors on any name it cannot find.
dxmt_native_exports = custom_target('dxmt-native-exports',
  input   : dxmt_native_export_lists,
  output  : 'dxmt-native.exports',
  command : [ 'cat', '@INPUT@' ],
  capture : true,
)

dxmt_native_umbrella = shared_library('dxmt-native',
  'dxmt_native.cpp',
  link_whole          : dxmt_native_whole,
  dependencies        : dxmt_native_deps,
  include_directories : [ dxmt_include_path ],
  link_args           : [ '-Wl,-exported_symbols_list,' + join_paths(meson.current_build_dir(), 'dxmt-native.exports') ],
  link_depends        : [ dxmt_native_exports ],
  install             : true,
)

install_headers('dxmt_native.h', subdir : 'dxmt-native')

pkgconf_data = configuration_data()
pkgconf_data.set('prefix', get_option('prefix'))
pkgconf_data.set('libdir', get_option('libdir'))
pkgconf_data.set('includedir', get_option('includedir'))
pkgconf_data.set('version', meson.project_version())

configure_file(
  input : 'dxmt-native.pc.in',
  output : 'dxmt-native.pc',
  configuration : pkgconf_data,
  install : true,
  install_dir : join_paths(get_option('libdir'), 'pkgconfig'),
)
