metallib_writer_test = executable('metallib_writer_test',
  ['metallib_writer_test.cpp'],
  include_directories : [ dxmt_include_path, include_directories('../../src/airconv'), llvm_include_path_darwin ],
  cpp_args            : [ airconv_args ],
  link_with           : [ airconv_lib_darwin ],
  link_args           : [ llvm_ld_flags_darwin, llvm_deps ],
  native              : dxmt_crossbuild
)

test('metallib_writer', metallib_writer_test)
