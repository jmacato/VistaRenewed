# Upstream sources and licenses

Triton and Neptune come from osy's graphics work for QEMU and UTM. This fork
adds Vista drivers and Linux host support.

| Local tree | Upstream | Recorded base |
| --- | --- | --- |
| `triton-kmd` | https://github.com/osy/kvm-guest-drivers-windows | `74cb98d15f6cb9ca44d9b6ecc6e47a2236eeb2d3` |
| `triton-umd` | https://github.com/osy/virtio-win-mesa | `7432d34c2bc10c602d72b1ad4058cde98549f98c` |
| `triton-qemu` | https://github.com/utmapp/qemu | `7311c3651c3a2cbc3d32e6eae262c60339f28d79` |
| `triton-virglrenderer` | https://github.com/utmapp/virglrenderer | `65cc14eb896f121ffc5130ce04815a923a03c41d` |
| `triton-dxvk` | https://github.com/osy/dxvk | `404240fdacf47470b02c76d6e684639a95dc7387` |

These are the base revisions. The local trees also contain this project's changes.

## DXVK sources

`patches/dxvk-neptune.bundle` contains the Neptune commit
[c6bb6d57](https://github.com/jmacato/osy-dxvk/commit/c6bb6d57fac2b6cae7f6adbbc521eb949849815e)
on our `vista-neptune` branch. The submodule and build scripts fetch our fork;
the bundle also keeps a copy of this commit. The source revisions and patch hashes
are listed in `patches/sources.json`.

`patches/dxvk-vista.patch` adds the Vista backend changes.
`patches/dxbc-spirv-vista.patch` adds the shader compiler changes, based on
dxbc-spirv revision `0e79a703db8b23004c77dbabacf25ed2d41f0bd9`.
Run `python3 scripts/bootstrap_sources.py` to restore the sources and apply
both patches.

## Licenses

Read each component's `LICENSE`, `COPYING`, `COPYING.LIB` and file notices.
The D3D9 shader converter also includes third-party notices. SDK/WDK files
and guest applications have separate terms.

Original project contributions use the [MIT grant](../LICENSE.md).
The [MIT text](../LICENSES/MIT-original.txt) is included in source and guest
binary notices.

## Attribution still to check

Eight UMD files carry Turing Software LLC copyright labels that have not
been independently verified. Paths below are relative to `triton-umd/`:

- `src/virtio/neptune/npt_runtime_binding.h`
- `src/virtio/neptune/triton/tritonD3D10.c` and `.h`
- `src/virtio/neptune/triton/tritonDitherControl.h`
- `src/virtio/neptune/vista-d3d10/meson.build`
- `src/virtio/neptune/vista-d3d9/triton9_fixed.cpp` and `.h`
- `src/virtio/neptune/vista-d3d9/third_party/d3d9on12-shaderconverter/Inc/ShaderValidation.h`

The existing labels remain in place. `ShaderValidation.h` was added locally,
separate from the Microsoft shader-converter import. The MIT grant covers
the maintainer's work and leaves these ownership questions open.

## Original projects

[Neptune announcement](https://blog.getutm.app/2026/introducing-neptune-direct3d-virtualization-for-qemu/)
and [Triton announcement](https://blog.getutm.app/2026/introducing-triton-directx-11-driver-for-qemu/).
