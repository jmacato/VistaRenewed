# Triton for Windows Vista

This project ports osy's Triton graphics driver to Windows Vista with WDDM 1.0
and Direct3D 9 interfaces. Neptune sends guest graphics commands through QEMU
to the Linux host, where native DXVK translates Direct3D 11 operations to Vulkan.

The driver is experimental. Aero Glass has worked in the Vista Ultimate SP2 x64
checked development guest on an Intel Vulkan host. Both x64 and x86 drivers
compile; x86 guest compatibility and complete Direct3D 9 conformance are not
established. Boot and shutdown blue screens have occurred. CI builds and checks
installer packages but does not boot Vista or certify the driver.

## Build

- [Driver installer ISO and CI](docs/CI.md)
- [Linux host and driver build instructions](docs/BUILDING.md)
- [Installer instructions](docs/INSTALL-ISO.txt)
- [Architecture and source map](docs/ARCHITECTURE.md)
- [Upstream sources and licenses](docs/UPSTREAM.md)

Clone without `--recurse-submodules`. To build the Linux host backend, restore
the pinned DXVK source and dependencies from the repository root:

```sh
python3 scripts/bootstrap_sources.py
```

`patches/dxvk-neptune.bundle` contains the additional DXVK source commit needed
by the host build. Ordinary recursive submodule initialization cannot fetch that
commit from upstream. The guest driver ISO build does not require DXVK.

## Layout

- `triton-*`: driver, host renderer and dependency sources, with upstream licenses.
- `scripts/`: source bootstrap, build, validation, signing and packaging tools.
- `packaging/`: installer service source, Windows resources and package INF.
- `tests/`: native host graphics test source.
- `docs/`: build, installation, architecture and licensing documentation.
- `.github/workflows/`: automated driver ISO build.

Build intermediates go in ignored `build/` and component build directories;
installer outputs go in ignored `dist/`. VM disks, downloaded development kits,
local signing identities and generated binaries are excluded from Git.
