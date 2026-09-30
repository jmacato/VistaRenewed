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

The component directories retain their upstream source layouts. The table above
records their base revisions; this repository includes subsequent Vista and Linux
changes and is not a complete upstream mirror.

DXVK retains its Git history. `patches/dxvk-neptune.bundle` supplies commit
`c6bb6d57fac2b6cae7f6adbbc521eb949849815e` above the recorded upstream base.
`patches/sources.json` records the bundle, pinned sources and patch hashes.
`patches/dxvk-vista.patch` and `patches/dxbc-spirv-vista.patch` carry the curated
backend and shader-compiler changes. The latter applies to dxbc-spirv revision
`0e79a703db8b23004c77dbabacf25ed2d41f0bd9`. Run
`python3 scripts/bootstrap_sources.py` to restore and apply the complete series;
do not combine it with historical manual patch commands.

## Licenses and attribution

Each component and dependency retains its copyright and license notices.
No repository-wide license replaces those terms. Read component `LICENSE`,
`COPYING`, `COPYING.LIB` and per-file notices, including the D3D9 shader
converter's third-party notices. The Windows SDK/WDK and guest applications
have their own terms and are not source components of this repository.

**Unresolved publication decision:** standalone development helpers without an
existing license notice do not have a project-wide license grant. A rights
holder must identify the authorship of those files and authorize a specific
license before the repository can claim they are freely redistributable.
Likewise, copied copyright wording on newly authored files is not evidence of
ownership. Preserve genuine upstream notices; do not invent a grant, assign
third-party ownership or add contributor sign-offs on someone's behalf.

This is a release-readiness limitation. A public source preview must not claim a
single repository-wide open-source license while it remains unresolved.

## Contribution and review boundaries

Read each destination's current contribution instructions before preparing an
upstream submission. UTM's [contribution guidelines](https://github.com/utmapp/UTM/blob/main/CONTRIBUTING.md)
require human testing; local agent substitution does not satisfy that upstream
requirement. QEMU's [code provenance policy](https://www.qemu.org/docs/master/devel/code-provenance.html#use-of-ai-generated-content)
declines AI-generated contributions. Do not represent agent-authored changes as
eligible upstream submissions or supply false provenance/DCO statements.

The public fork can retain local review evidence without claiming upstream
acceptance. Formatting/checkpatch results and fresh adversarial reviews are
bounded checks, not exceptions to those policies.

Original design context: [Neptune announcement](https://blog.getutm.app/2026/introducing-neptune-direct3d-virtualization-for-qemu/)
and [Triton announcement](https://blog.getutm.app/2026/introducing-triton-directx-11-driver-for-qemu/).
Those releases describe their own stack; they are not evidence of this Vista
preview's compatibility or actual QEMU-window frame rate.
