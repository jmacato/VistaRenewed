dxbc_parser_src = files([
  'DXBCUtils.cpp',
  'BlobContainer.cpp',
  'ShaderBinary.cpp',
])

dxbc_parser_args = [
  '-Wno-extern-c-compat',
  '-Wno-unknown-pragmas',
]

dxbc_parser_lib = static_library('DXBCParser', dxbc_parser_src,
  include_directories : [ dxmt_include_path ],
  cpp_args            : [ dxbc_parser_args ],
)

dxbc_parser_dep = declare_dependency(
  link_with           : [ dxbc_parser_lib ],
  include_directories : [ dxmt_include_path ],
)

if not dxmt_crossbuild

# Native: dxmt_include_path already contains the native headers, so a separate
# DXBCParserNative would be a byte-identical second copy of every symbol —
# which collides once both consumers link into one dylib.
dxbc_parser_native_lib = dxbc_parser_lib
dxbc_parser_native_dep = dxbc_parser_dep

elif cpu_family == 'x86_64' or cpu_family == 'aarch64'

dxbc_parser_native_lib = static_library('DXBCParserNative', dxbc_parser_src,
  include_directories : [ dxmt_native_include_path ],
  cpp_args            : [ dxbc_parser_args ],
  native              : dxmt_crossbuild,
)

dxbc_parser_native_dep = declare_dependency(
  link_with           : [ dxbc_parser_native_lib ],
  include_directories : [ dxmt_native_include_path ],
)

endif