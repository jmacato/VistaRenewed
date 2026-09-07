# Upstream sources and licensing

Triton and Neptune originate in osy's work on graphics acceleration for QEMU
and UTM. This repository ports that work to Vista and adapts the Linux host
route; it does not originate the whole graphics stack.

| Local tree | Upstream | Recorded base |
| --- | --- | --- |
| `triton-kmd` | https://github.com/osy/kvm-guest-drivers-windows | `74cb98d15f6cb9ca44d9b6ecc6e47a2236eeb2d3` |
| `triton-umd` | https://github.com/osy/virtio-win-mesa | `7432d34c2bc10c602d72b1ad4058cde98549f98c` |
| `triton-qemu` | https://github.com/utmapp/qemu | `7311c3651c3a2cbc3d32e6eae262c60339f28d79` |
| `triton-virglrenderer` | https://github.com/utmapp/virglrenderer | `65cc14eb896f121ffc5130ce04815a923a03c41d` |
| `triton-dxvk` | https://github.com/osy/dxvk | `404240fdacf47470b02c76d6e684639a95dc7387` |
| `triton-dxmt` | https://github.com/utmapp/dxmt | `822ae637a39512ddbd8e0bbd7744af514670c9d3` |
| `triton-angle` | https://github.com/utmapp/WebKit | `ed78ab6e1a37f4f11583a0bd038f22ec91f3ff10` |
| `triton-libepoxy` | https://github.com/utmapp/libepoxy | `bf98587477fe68d07b93319ece7b40a7d0e2eabe` |

Most trees arrived as source exports, with available submodule contents, rather
than Git checkouts. `handoff/repositories.json` records the transfer provenance.
The patches next to it describe the earlier transfer state; do not reapply them
over the current source exports. The repository also retains source omissions
recorded during that transfer; it is not a pristine upstream mirror.

DXVK retains its Git history. `patches/dxvk-neptune.bundle` contains only the
additional commit and requires the upstream base above. The same change is
readable in `handoff/triton-dxvk-linux.patch`.

Each component and bundled dependency retains its own copyright and license
notices. There is no blanket license replacing those terms. See the components'
`LICENSE`, `COPYING`, `COPYING.LIB` and per-file notices, including the shader
converter's third-party notices. New standalone development helpers do not yet
have a project-wide license grant; resolve that before advertising the whole
repository under one license.
