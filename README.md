# Triton for Windows Vista

A port of osy's Triton graphics driver to Windows Vista's WDDM 1.0 and
Direct3D 9 interfaces. The Linux host path uses Neptune, DXVK and Vulkan
to render a Vista guest in QEMU. Aero Glass works in the tested VM,
including DWM's transparency and blur.

![Aero Glass running in the Vista development VM](docs/evidence/glass-boot2.png)

This is experimental driver development. The verified guest is **Vista Ultimate
SP2 x64, checked build 6002.18005**, running with four KVM vCPUs on a Linux
host with Intel Iris Xe graphics. Both x64 and x86 drivers compile; the x86
guest runtime has not been validated. Recent development also encountered
boot/shutdown blue screens. See the [status and limits](docs/STATUS.md).

## Start here

- [Build and development setup](docs/BUILDING.md)
- [CI-built driver installer ISO](docs/CI.md)
- [Architecture and source map](docs/ARCHITECTURE.md)
- [Verification and known limits](docs/STATUS.md)
- [Detailed Aero evidence](notes/AERO_GLASS_VERIFICATION.md)
- [Upstream sources and licenses](docs/UPSTREAM.md)

Clone without `--recurse-submodules`, then run:

```sh
python3 scripts/bootstrap_sources.py
```

DXVK's local change is supplied as a Git bundle and readable patch. The
bootstrap restores the exact recorded commit before fetching its dependencies.
Ordinary recursive submodule initialization cannot fetch that commit from
osy's upstream repository yet.

## What changed

The port adds a Vista kernel display driver path and a Direct3D 9 user-mode
driver, including shader translation, resource sharing, synchronization and
presentation. Host changes carry the operations through Neptune to the Linux
renderer. A public D3D9 probe and pixel measurements check the resulting output.

One bug kept Glass from blurring: the shader converter mapped all eight of
DWM's texture-coordinate inputs to TEXCOORD0. Initializing the identity mapping
restored distinct samples. The regression checks the average of eight different
gray texels, then the desktop test checks actual transmitted and softened edges.

## Repository layout

The `triton-*` directories retain their upstream layouts and license files.
Build and deployment tools live in `scripts/`; development service and probe
support sources remain in `test-artifacts/` for compatibility with existing
builds. `notes/` and `handoff/` retain the investigation and transfer history.

VM disks, Windows installation media, SDK/WDK downloads, signing identities and
generated packages are local inputs and are excluded from Git. Supply your own
guest. CI produces an experimental test-signed installer ISO; no certified
driver release is included in Git.
