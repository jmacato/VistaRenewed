# Copyright 2026 Turing Software LLC
# SPDX-License-Identifier: MIT

# nptunix.{so,dll} -- Wine unixlib bridge for the mesa Wine PE DLLs.
#
# nptunix.so is the unix side: a plain Linux ELF (xcb DRI3/Present, AF_UNIX
# sockets, SCM_RIGHTS, memfd) loaded by the PE d3d11.dll inside Wine via the
# nptunix.dll helper-builtin (mmdevapi pattern).  Sources are npt_unix.c +
# npt_unix_wsi.c + npt_unix_virtgpu.c + npt_display_drm.c.
# Built in BOTH builddirs because the native test apps and the Wine PE
# pipeline both need it -- in the Wine PE cross build we still want a Linux
# ELF, so we ask meson to use the build-machine compiler via `native : true`.
#
# nptunix.dll is the PE carrier: a tiny stub whose only purpose is to give
# Wine a builtin DLL whose unixlib handle (NtQueryVirtualMemory(
# MemoryWineUnixFuncs)) the PE d3d11.dll can grab via LoadLibraryA().
# Source is nptunix_pe.c.  Wine recognizes builtins by the literal
# "Wine builtin DLL" string at offset 0x40 of the DOS stub, which neither
# winegcc nor MinGW writes there -- we have to patch it post-link.  The
# patching is 32 bytes and is done by a one-shot inline python invocation.
# Only built in the Wine PE cross build because it requires a PE compiler.
#
# Path 1 (DRI3 1.4 explicit sync, used at runtime when the X server reports
# >= 1.4) needs libxcb-dri3 1.16+ which is newer than what Ubuntu 24.04
# ships.  Point pkg-config at a sysroot that has it, e.g.:
#
#     PKG_CONFIG_PATH=/path/to/sysroot/lib/pkgconfig:$PKG_CONFIG_PATH \
#         meson setup builddir-npt
#
# meson then resolves the deps from the sysroot's .pc files and we bake
# the resulting libdir into nptunix.so as an rpath so the runtime loader
# (even inside Wine) finds the sysroot libxcb-dri3 instead of the system one.
# Path 2 (xshmfence) needs xcb-dri3 / xcb-present at any version that ships
# xcb_dri3_fence_from_fd, which has been there forever.  Path 1 (DRI3 1.4
# explicit sync via xcb_dri3_import_syncobj + xcb_present_pixmap_synced)
# needs the libxcb *release* >= 1.16 -- the libxcb release where the DRI3
# 1.4 protocol header lands.  Probe for it; if found we compile both paths
# in and the runtime picks based on what the X server reports, if not found
# we compile only path 2 and the runtime negotiation transparently sticks
# with the xshmfence fallback.
#
# Ubuntu 24.04 ships libxcb 1.15.  To enable path 1 there, install a newer
# libxcb to a custom prefix and point meson at it via:
#     meson setup -Dpkg_config_path=/path/to/sysroot/lib/pkgconfig builddir-npt
#     meson setup -Dbuild.pkg_config_path=/path/to/sysroot/lib/pkgconfig builddir-win64
# (cross builds don't propagate PKG_CONFIG_PATH from the calling env to the
# build-machine pkg-config; the `build.` prefix targets the build machine.)
npt_dep_xcb_dri3_new = dependency('xcb-dri3',
                                   version : '>= 1.16', native : true,
                                   required : false)
npt_have_explicit_sync = npt_dep_xcb_dri3_new.found()

if npt_have_explicit_sync
  npt_dep_xcb_dri3    = npt_dep_xcb_dri3_new
  npt_dep_xcb_present = dependency('xcb-present', version : '>= 1.16',
                                    native : true)
else
  npt_dep_xcb_dri3    = dependency('xcb-dri3',    native : true)
  npt_dep_xcb_present = dependency('xcb-present', native : true)
endif

npt_dep_xcb = dependency('xcb', native : true)

# Bake the xcb-dri3 libdir as an rpath so nptunix.so resolves the
# pkg-config-resolved libxcb-dri3.so.0 at runtime even when loaded inside
# Wine, where LD_LIBRARY_PATH may not be propagated.  When the system
# libxcb is being used this is just /usr/lib/<triple> and is harmless;
# when a custom prefix is in play this is what makes the loader find it.
npt_xcb_dri3_libdir = npt_dep_xcb_dri3.get_variable(pkgconfig : 'libdir')

npt_nptunix_deps = [
  npt_dep_xcb, npt_dep_xcb_dri3, npt_dep_xcb_present,
  dependency('libdrm', native : true),
]
npt_nptunix_c_args = []

if npt_have_explicit_sync
  npt_nptunix_c_args += '-DNPT_HAVE_XCB_DRI3_SYNCOBJ'
endif

libnptunix = shared_library(
  'nptunix',
  files('npt_unix.c', 'npt_unix_wsi.c', 'npt_unix_virtgpu.c',
        'npt_display_drm.c'),
  name_prefix : '',
  native : true,
  include_directories : inc_include,
  c_args : npt_nptunix_c_args,
  dependencies : npt_nptunix_deps,
  build_rpath : npt_xcb_dri3_libdir,
  gnu_symbol_visibility : 'default',
  # `install : true` is incompatible with `native : true` in a cross build
  # (meson refuses to install a build-machine binary into the cross prefix)
  # and the run scripts copy from the build dir directly anyway.
  install : false,
)

if npt_is_windows
  # ----- nptunix.dll (PE carrier, marker-patched) -----
  #
  # The shared_library is named "nptunix_unpatched" so its output filename
  # ("nptunix_unpatched.dll") doesn't collide with the patched custom_target
  # output ("nptunix.dll").  But that name is also embedded in the PE
  # export directory's Name field, which Wine reads to locate the matching
  # unixlib .so (it looks for "<embedded_name_without_.dll>.so" in
  # WINEDLLPATH/x86_64-unix/).  So the patch step must:
  #   (1) write the "Wine builtin DLL" marker into the DOS stub at 0x40
  #   (2) rewrite the embedded export name from "nptunix_unpatched.dll"
  #       to "nptunix.dll", NUL-padded to preserve the original byte length
  #       so PE offsets stay valid.  The export Name field is
  #       NUL-terminated, so the trailing NUL padding is harmless.
  nptunix_dll_unpatched = shared_library(
    'nptunix_unpatched',
    files('nptunix_pe.c'),
    name_prefix : '',
    gnu_symbol_visibility : 'hidden',
    install : false,
  )
  nptunix_dll = custom_target(
    'nptunix.dll',
    input  : nptunix_dll_unpatched,
    output : 'nptunix.dll',
    command : [
      find_program('python3'), '-c',
      'import sys; d = open(sys.argv[1], "rb").read(); ' +
      'old = b"nptunix_unpatched.dll" + bytes(1); ' +
      'new = b"nptunix.dll" + bytes(len(old) - len(b"nptunix.dll")); ' +
      'assert d.count(old) >= 1, "embedded module name not found"; ' +
      'd = d.replace(old, new); ' +
      'open(sys.argv[2], "wb").write(' +
        'd[:0x40] + b"Wine builtin DLL" + bytes(16) + d[0x60:])',
      '@INPUT@', '@OUTPUT@',
    ],
    build_by_default : true,
    install : true,
    install_dir : get_option('libdir'),
  )
endif
