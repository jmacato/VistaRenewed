# Upstream sources and licenses

Triton and Neptune originate in osy's graphics acceleration work for QEMU and UTM.
QEMU and UTM run virtual machines.
This repository ports the driver to Vista and adapts the Linux host configuration.
Upstream means the original project from which these source files came.

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

Most directories arrived as source exports with available submodule files.
These exports did not contain their original Git history.
The `handoff/repositories.json` file records their origins and revisions.
Adjacent patches describe the earlier transfer state.
Do not apply those patches over the current source exports.
Transfer records also identify missing source files.
This repository is not a complete upstream mirror.

DXVK retains its Git history.
The `patches/dxvk-neptune.bundle` file contains the additional commit and requires the recorded upstream base.
The `handoff/triton-dxvk-linux.patch` file presents the same change as readable text.

Each component and dependency retains its copyright and license notices.
No repository-wide license replaces those terms.
Read the component `LICENSE`, `COPYING`, `COPYING.LIB` and per-file notices.
Read the shader converter's third-party notices.
New standalone development helpers have no project-wide license grant.
Before you advertise one license for the repository, resolve this missing grant and the component terms.
