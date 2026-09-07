project('dxmt', ['c', 'cpp'], version : 'v0.1', meson_version: '>=1.3.0', default_options : [ 
  'cpp_std=c++20', 
  'warning_level=2', 
  'build.cpp_std=c++20',
  'b_ndebug=if-release',
])

fs = import('fs')

dxmt_crossbuild = host_machine.system() == 'windows'

cpp = meson.get_compiler('cpp')
cc = meson.get_compiler('c')
clang = meson.get_compiler('c', native: true)
clangpp = meson.get_compiler('cpp', native: true)

cpu_family = host_machine.cpu_family()

dxmt_include_dirs = [
  './include',
  './libs'
]

dxmt_native_include_dirs = [
  './include/native/windows',
  './include/native/directx',
]

if not dxmt_crossbuild
  dxmt_include_dirs += dxmt_native_include_dirs
endif

source_prefix_path = fs.relative_to(
    meson.project_source_root(),
    meson.project_build_root()
)

compiler_args = [
  '-Wimplicit-fallthrough',
  # gcc
  '-Wno-missing-field-initializers',
  '-Wno-unused-parameter',
  '-Wno-cast-function-type', # Needed for GetProcAddress.
  # clang
  '-Wno-unused-private-field',
  '-Wno-microsoft-exception-spec',
  '-Wno-extern-c-compat',
  '-Wno-unused-const-variable',
  '-Wno-missing-braces',
  '-fblocks',
  '-fmacro-prefix-map=' + source_prefix_path + '/=',
]

if cpu_family == 'x86'
  compiler_args += [
    '-msse',
    '-msse2',
    '-msse3',
    '-mfpmath=sse',
  ]
endif

native_compiler_args = [
  '-Wimplicit-fallthrough',
  '-Wno-unused-parameter',
  '-Wno-unused-private-field',
  '-Wno-unused-const-variable',
  '-Wno-missing-braces',
  '-fblocks',
]

link_args = []

compiler_args += [
  '-DNOMINMAX',
  '-D_WIN32_WINNT=0xa00',
]

link_args += [
  '-static',
  # -static is sufficient for clang, while c runtime will be linked to api-ms-win-crt-* (ucrt)
  # '-static-libgcc',
  # '-static-libstdc++',
  # We need to set the section alignment for debug symbols to
  # work properly as well as avoiding a memcpy from the Wine loader.
  '-Wl,--file-alignment=4096',
]

# Wine's built-in back traces only work with dwarf4 symbols
if get_option('debug')
  compiler_args += [
    '-gdwarf-4',
  ]
endif

dxmt_include_path = include_directories(dxmt_include_dirs)
dxmt_native_include_path = include_directories(dxmt_native_include_dirs)

if not fs.is_file('include/native/directx/d3d11.h')
  error('Don\'t forget to clone submodules. `git submodule update --init --recursive`')
endif

if dxmt_crossbuild
  res_ext = '.o'
  wrc = find_program('windres')
  wrc_generator = generator(wrc,
    output    : [ '@BASENAME@' + res_ext ],
    arguments : [ '-i', '@INPUT@', '-o', '@OUTPUT@' ],
  )
else
  wrc           = find_program('touch')
  wrc_generator = generator(wrc, output : [ '@BASENAME@_ignored.h' ], arguments : [ '@OUTPUT@' ] )
endif

# Which Apple platform we are building for.  Note host_machine.subsystem() is
# fatal when a cross file leaves it out -- it is only auto-detected for native
# builds -- so every Apple cross file used with this project has to declare it.
apple_subsystem = host_machine.system() == 'darwin' ? host_machine.subsystem() : ''

# metal_sdk: the xcrun SDK the shader compiler runs against.
# air_platform: the OS component of the AIR triple, which must match the SDK --
#   an AIR module built for one platform will not load on another.
# metal_min: lowest OS carrying everything we use (bindless argument buffers,
#   mesh shaders, MetalFX).  visionOS is Metal 3.1 from 1.0, and the compiler
#   clamps up to AIR 2.6 there whatever -std says.
apple_platforms = {
  'macos'              : { 'metal_sdk' : 'macosx',          'air_platform' : 'macos',           'metal_min' : '13.0' },
  'ios'                : { 'metal_sdk' : 'iphoneos',        'air_platform' : 'ios',             'metal_min' : '16.0' },
  'ios-simulator'      : { 'metal_sdk' : 'iphonesimulator', 'air_platform' : 'ios',             'metal_min' : '16.0', 'simulator' : true },
  'tvos'               : { 'metal_sdk' : 'appletvos',       'air_platform' : 'tvos',            'metal_min' : '16.0' },
  'tvos-simulator'     : { 'metal_sdk' : 'appletvsimulator','air_platform' : 'tvos',            'metal_min' : '16.0', 'simulator' : true },
  'visionos'           : { 'metal_sdk' : 'xros',            'air_platform' : 'xros',            'metal_min' : '1.0'  },
  'visionos-simulator' : { 'metal_sdk' : 'xrsimulator',     'air_platform' : 'xros',            'metal_min' : '1.0',  'simulator' : true },
}

if not dxmt_crossbuild
  if apple_subsystem == 'watchos' or apple_subsystem == 'watchos-simulator'
    error('watchOS ships no Metal framework, so DXMT cannot target it')
  endif
  if not apple_platforms.has_key(apple_subsystem)
    error('unsupported Apple subsystem \'' + apple_subsystem + '\'')
  endif
  apple_platform = apple_platforms[apple_subsystem]
  metal_sdk = apple_platform['metal_sdk']
  # Simulator AIR is a distinct target from the device's, hence the suffix.
  metal_air_target = '--target=air64-apple-' + apple_platform['air_platform'] + apple_platform['metal_min'] + (
    apple_platform.get('simulator', false) ? '-simulator' : '')
else
  # Cross build: the shaders still have to run on the host's macOS.
  metal_sdk = 'macosx'
  metal_air_target = '--target=air64-apple-macos13.0'
endif

xcrun = find_program('xcrun')

metalir_generator = generator(xcrun,
  output    : [ '@BASENAME@.air'],
  arguments : [ '-sdk', metal_sdk, 'metal', '-o', '@OUTPUT@', '-c', '@INPUT@', '@EXTRA_ARGS@'],
)

metallib_generator = generator(xcrun,
  output    : [ '@BASENAME@.metallib'],
  arguments : [ '-sdk', metal_sdk, 'metallib', '-o', '@OUTPUT@', '@INPUT@'],
)

xxd = find_program('xxd')

hexdump_generator = generator(xxd,
  output    : [ '@BASENAME@.h'],
  arguments : [ '-n', '@BASENAME@', '-i', '@INPUT@', '@OUTPUT@'],
)

dxmt_debug = get_option('dxmt_debug')

if dxmt_debug
add_project_arguments('-DDXMT_DEBUG=1', language: 'cpp')
endif

add_project_arguments('-DDXMT_PAGE_SIZE=4096', language: 'cpp')

dxmt_version = vcs_tag(
  command: ['git', 'describe', '--always'],
  input:  'version.h.in',
  output: 'version.h',
)

if dxmt_crossbuild
  wine_builtin_dll = get_option('wine_builtin_dll')

  if cpu_family == 'x86'
    # Enable stdcall fixup on 32-bit
    link_args += [
      '-Wl,--enable-stdcall-fixup',
      '-Wl,--kill-at',
    ]
    # Fix stack alignment issues with mingw on 32-bit
    compiler_args += [
      '-mpreferred-stack-boundary=2'
    ]
    if wine_builtin_dll
      windows_native_install_dir = 'i386-windows'
    else
      windows_native_install_dir = 'syswow64'
    endif
    windows_builtin_install_dir = 'i386-windows'
  elif cpu_family == 'x86_64'
    if wine_builtin_dll
      windows_native_install_dir = 'x86_64-windows'
    else
      windows_native_install_dir = 'system32'
    endif
    windows_builtin_install_dir = 'x86_64-windows'
  elif cpu_family == 'aarch64'
    if wine_builtin_dll
      windows_native_install_dir = 'aarch64-windows'
    else
      windows_native_install_dir = 'system32'
    endif
    windows_builtin_install_dir = 'aarch64-windows'
  else
    error('invalid cpu family')
  endif

  unix_install_dir = cpu_family + '-unix'
else
  wine_builtin_dll = false
  # Native: every module links into the single libdxmt-native.dylib, which is
  # the only installed artifact (see src/dxmt-native).
  windows_native_install_dir = get_option('libdir')
  # Each module normally carries its own Logger instance, one per DLL.  In a
  # single image that would be several definitions of one symbol, so the
  # modules drop theirs and dxmt-native defines the only one.
  add_project_arguments('-DDXMT_NATIVE=1', language : ['c', 'cpp'])
endif

native_compiler_arch = cpu_family == 'aarch64' ? 'arm64' : 'x86_64'

add_project_arguments(cpp.get_supported_arguments(compiler_args), language: 'cpp')
add_project_arguments(clangpp.get_supported_arguments(native_compiler_args), language: 'cpp', native: true)
add_project_arguments(cc.get_supported_arguments(compiler_args), language: 'c')
add_project_arguments(clang.get_supported_arguments(native_compiler_args), language: 'c', native: true)
add_project_link_arguments(cpp.get_supported_link_arguments(link_args), language: 'cpp')
add_project_link_arguments(cc.get_supported_link_arguments(link_args), language: 'c')
if dxmt_crossbuild
  add_project_arguments(['-arch', native_compiler_arch], language: 'c',  native: true)
  add_project_arguments(['-arch', native_compiler_arch], language: 'cpp',  native: true)
  add_project_link_arguments(['-arch', native_compiler_arch], language: 'cpp',  native: true)
endif

subdir('libs')
subdir('src')

if get_option('enable_tests')
subdir('tests')
endif
