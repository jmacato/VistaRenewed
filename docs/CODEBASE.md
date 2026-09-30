# Working in the codebase

> Development record retained with the public desktop sources. Local capture,
> VM and `.unlazy/` paths refer to non-shipped development artifacts. See
> [current public entry points and status](DESKTOP-MODULES.md).


Start with [architecture](ARCHITECTURE.md) for the guest-to-host path and
[building](BUILDING.md) for the build sequence. This repository contains several
upstream projects; the Vista-specific work is concentrated in a few directories.
Moving all `triton-*` projects into a new hierarchy would touch build scripts,
submodule paths, includes, and upstream integration without simplifying the
driver itself.

## Where to make a change

| Concern | Start here |
| --- | --- |
| D3D9 entry points, device lifetime, callback registration | `triton-umd/src/virtio/neptune/vista-d3d9/triton9_ddi.c` |
| Resource ownership, lock/unlock, sharing, presentation | `triton9_resource.c` in that same directory |
| Portable CPU layouts and bounds checks | `triton9_cpu_layout.c` and `.h` |
| Draw validation, shader conversion, state | `triton9_draw_contract.c`, `triton9_shader.cpp`, `triton9_state.cpp` |
| Display output and mode handling | `triton9_output.c` |
| Kernel command handling and scanout | `triton-kmd/viogpu/viogpu3d/viogpu_command.cpp`, `viogpu_vidpn.cpp` |
| Host readback and retained-primary copies | `triton-qemu/hw/display/virtio-gpu-virgl.c` |
| Neptune host dispatch | `triton-virglrenderer/src/neptune/` |
| Guest installation and reboot recovery | `packaging/vista-driver-deploy-service.c` |
| Signed ISO assembly | `scripts/package_vista_ci_iso.py`, `create_vista_catalog.py`, `stage_vista_linux_media.py` |
| Presentation and installer regression tests | [tests/vista](../tests/vista/README.md) |

Use scoped searches, for example:

```sh
rg -n 'triton9Present|triton9WaitForPresent' triton-umd/src/virtio/neptune/vista-d3d9
rg -n 'CompletePendingFlip|TryPromoteFlip' triton-kmd/viogpu/viogpu3d
git ls-files scripts packaging docs tests
```

`build/`, `dist/`, `driver/`, `vista-kvm/`, and `host-linux/` are ignored local
artifacts, development inputs, or VM state. They are not additional source
projects. `triton-kmd/build/` is an exception: it contains tracked build support.
See [upstream sources](UPSTREAM.md) before reorganizing vendored code.

## Readability assessment and next boundaries

At the September 2026 review, the installer service has about 2,600 lines and
`triton9_resource.c` about 2,400. The difficulty is the number of responsibilities
and shared state, beyond their length. Three dense tests now keep native
fixtures separate from Python compilation and extraction code.

The next changes should be independently reviewable:

1. Split installer authentication, BCD/process helpers, and installation state
   transitions into separate modules. First describe which functions can run
   in Safe Mode and which transitions persist state before reboot. Preserve
   certificate pinning, manifest checks, and interrupted-install recovery.
2. Separate UMD presentation synchronization from resource allocation and
   locking. Preserve the `shaderLock` ownership across GPU completion, Present,
   and the consumption marker; preserve the narrower `kmContextLock` scope.
   Avoid exporting resource internals just to shorten a file.
3. Give the ISO scripts `main()` entry points and a shared architecture payload
   description. Today file lists recur in packaging and staging; the Windows
   service must still independently enforce the expected authenticated payload.
4. Move the remaining embedded native test bodies into readable fixtures.
   Keep extracting the implementation under test until production helpers have
   an interface that allows direct linking.

Test-layout changes need the native regression suite. Installer changes need
both service builds, package verification, and guest recovery tests. Changes to
presentation need both UMD builds, the host tests, and x64/x86 guest evidence.
Reformatting or moving vendor trees in bulk would make those reviews harder.

For language changes, see the [Rust feasibility assessment](RUST.md).
