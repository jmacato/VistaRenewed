# Triton for Windows Vista — developer preview

Triton Vista is an experimental Windows Vista bringup project. It includes a
QEMU/Linux graphics stack with WDDM 1.0 and Direct3D 9/10 drivers, IE/MSHTML
integration experiments, restored Sidebar data providers, Supermium/CEF build
tooling, and desktop control and Media Center diagnostics. The original
Direct3D 11 route and macOS source components remain in the tree.

This is experimental source for developers. Full Direct3D 9/10 compatibility,
smooth Aero, and acceptable game performance are **not established**. See
[validation requirements and known presentation limits](docs/RELEASE-VALIDATION.md).
Tests of an older development build do not validate a newly built installer.

## Start here

1. Clone normally, without `--recurse-submodules`.
2. Restore the pinned host sources with `python3 scripts/bootstrap_sources.py`
   and `python3 scripts/bootstrap_qemu_sources.py`.
3. Build the toolchain image: `bash scripts/dev-container.sh build`.
4. Follow [BUILDING](docs/BUILDING.md) for the host, x64/x86 drivers and packages.

The bootstrap restores the bundled DXVK commit and applies the curated DXVK and
shader-compiler patches. A recursive submodule checkout alone is insufficient.
Microsoft SDK/WDK inputs and a licensed Vista installation are separate inputs;
no Windows installation media, VM disk or private signing key is included.

- [Desktop modules: IE, Sidebar, Supermium and diagnostics](docs/DESKTOP-MODULES.md)
- [Build and test workflow](docs/BUILDING.md)
- [Architecture and paired-component contracts](docs/ARCHITECTURE.md)
- [Installer/CI behavior](docs/CI.md) and [installation instructions](docs/INSTALL-ISO.txt)
- [Upstream provenance and licensing decisions](docs/UPSTREAM.md)
- [Test entry points and evidence locations](tests/README.md)
- [Release validation requirements](docs/RELEASE-VALIDATION.md)
- [Contributor/agent presentation requirements](AGENTS.md)

## Source layout

| Path | Contents |
| --- | --- |
| `triton-kmd/` | Vista kernel display driver |
| `triton-umd/` | Guest user-mode drivers and transport |
| `triton-qemu/`, `triton-virglrenderer/` | Matching VM, renderer and presentation code |
| `triton-dxvk/`, `patches/` | Pinned host backend and downstream source patches |
| `triton-dxmt/`, `triton-angle/`, `triton-libepoxy/` | Retained upstream platform components |
| `tools/` | IE/MSHTML adapters, desktop control, Sidebar updates and diagnostics |
| `packaging/vista-sidebar-gadgets/` | RSS, weather and currency providers and host relay |
| `scripts/`, `packaging/` | Bootstrap, builds, validators and installer source |
| `tests/`, component test directories | [CPU, native GPU and guest tests](tests/README.md) |
| `docs/` | Build, architecture, installation and validation documentation |

Upstream component layouts and licenses are preserved. Build output belongs in
ignored `build/`, component build directories, `host-linux/` and `dist/`; test
captures belong in `test-artifacts/`. Original project contributions use the
[scoped MIT grant](LICENSE.md); see [UPSTREAM](docs/UPSTREAM.md) for component
terms and remaining attribution questions.
