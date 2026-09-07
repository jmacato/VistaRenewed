# Native offscreen D3D11 smoke suite

Headless tests for `libdxmt-native.dylib`: no swapchain, no window, every test
renders/computes into ordinary resources and reads back through a STAGING
copy, so the same binary runs on macOS and on the headless Apple platforms
(iOS, tvOS, ...).  Output is line-oriented (`[PASS]/[FAIL]/[CRC]/[INFO]`) so
runs are diffable across platforms; the `[CRC]` lines are the rendered-image
checksums to compare.  Exit code is the number of failures.

Covers: device creation + feature levels, CheckFormatSupport,
buffer/texture round-trips (DEFAULT/DYNAMIC/IMMUTABLE/STAGING, mips, partial
copies), draw (basic / deferred-context / cbuffer / textured / instanced),
depth test, additive blending, 4x MSAA resolve, compute (structured buffer +
raw-view atomics), EVENT + OCCLUSION queries, ID3D11Fence with dxmt_event_*,
sRGB render targets, and BC1 through every upload path (IMMUTABLE initial
data, UpdateSubresource, staging copy -- including one written after being
recorded on a deferred context -- DYNAMIC initial data / Map(DISCARD) on the
immediate and deferred contexts, the R32G32_UINT reinterpret-alias upload,
staging-to-staging copies, and the FORMAT_SUPPORT2 mask).  All BC tests run
against hardware BC or CPU emulation, whichever the device has;
`DXMT_FORCE_BC_EMULATION=1` forces the emulated path on capable devices and
must produce CRC-identical images.

Shaders are compiled offline with fxc (SM 5.0) and embedded as DXBC
(`shaders_dxbc.h`, regenerate with `xxd -n dxbc_<name> -i <name>.cso`); there
is no d3dcompiler at run time on iOS.  The `.cso` blobs are checked in because
fxc only runs on Windows.

## macOS

```
clang++ -std=c++20 -O1 \
  -I../../include/native/windows -I../../include/native/directx \
  d3d11_test.cpp -o d3d11_test \
  -L../../build-native/src/dxmt-native -ldxmt-native \
  -Wl,-rpath,$PWD/../../build-native/src/dxmt-native
./d3d11_test results.txt
```

## iOS device

Build against the build-ios dylib, wrap in a minimal .app (Info.plist with
`UIFileSharingEnabled`, dylib in `Frameworks/`, rpath
`@executable_path/Frameworks`), sign with a development identity + wildcard
provisioning profile, install and launch with pymobiledevice3
(`apps install` / `developer dvt launch`).  With no argv the binary writes
`Documents/dxmt_results.txt` (and mirrors stderr to `Documents/dxmt_stderr.txt`
-- Metal validation aborts print the reason there, syslog redacts it); pull
both via house_arrest.  If `dvt launch` hangs after a previous instance was
watchdog-killed, reboot the device (`pymobiledevice3 diagnostics restart`).
