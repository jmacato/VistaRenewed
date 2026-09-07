winemetal_src = [
  '../winemetal/winemetal_thunks.c',
  '../winemetal/unix/winemetal_unix.c',
  '../winemetal/unix/cache.c',
  '../winemetal/unix/dxmt_native_event.c',
]
winemetal_link_depends = []

winemetal_ld_args = [
  '-weak_framework', 'CoreFoundation', '-weak_framework', 'Metal',
  '-weak_framework', 'Foundation',
  '-weak_framework', 'CoreGraphics', '-weak_framework', 'QuartzCore',
  '-weak_framework', 'ColorSync',
  '-lsqlite3',
]

if apple_subsystem == 'macos'
  # AppKit, for the display and window queries the headless platforms compile out.
  winemetal_ld_args += [ '-weak_framework', 'Cocoa' ]
endif

if not apple_platform.get('simulator', false)
  # No simulator SDK ships MetalFX; the scalers are compiled out there.
  winemetal_ld_args += [ '-framework', 'MetalFX' ]
endif

winemetal_lib = static_library('winemetal', winemetal_src,
  dependencies        : [ airconv_dep_darwin ],
  include_directories : [ dxmt_include_path, include_directories('.') ],
  c_args              : ['-ObjC'],
)

# The frameworks ride along to whatever links this, i.e. the single dylib.
winemetal_dep = declare_dependency(
  link_with           : [ winemetal_lib ],
  include_directories : [ include_directories('../winemetal') ],
  link_args           : winemetal_ld_args,
)
