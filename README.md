# Windows Vista projects — developer preview

A collection of Windows Vista development work: graphics drivers, browser
integration, desktop service restoration, and tools for building, testing and
investigating Vista applications. The main showcases are **Triton**, **Internet
Explorer/MSHTML**, and **Windows Sidebar**.

## Main showcases

### Triton graphics stack

An adaptation of osy's Triton/Neptune virtual graphics stack for Vista guests in
QEMU. It pairs Vista WDDM 1.0 and Direct3D 9/10 drivers with a QEMU/Neptune host
renderer and native DXVK on Linux/Vulkan. The original Direct3D 11 route and
macOS source components remain in the tree.

See the [build workflow](docs/BUILDING.md) and
[graphics architecture](docs/ARCHITECTURE.md). Full Direct3D compatibility and
smooth visible presentation remain under development; see the
[validation requirements and known limits](docs/RELEASE-VALIDATION.md).

### Internet Explorer and MSHTML

Experimental work connecting Vista's IE/MSHTML interfaces to a modern browser
backend. This includes COM and document adapters, browser transport, activation
and navigation probes, reversible per-user installation tooling, and supporting
Supermium/CEF source and runtime staging workflows.

These are prototypes; building the adapters does not establish full IE
compatibility. The full Supermium/CEF source build remains unfinished. See the
[desktop module guide](docs/DESKTOP-MODULES.md),
[MSHTML investigation](docs/MSHTML_CEF_SCOUT.md), and
[Supermium/CEF workflow](docs/SUPERMIUM_CEF_BUILD.md).

### Windows Sidebar

Replacement data providers for Vista's RSS, weather, location search and
currency gadgets, with a host relay that connects them to current services.
The updater backs up the original guest files and preserves the shipped gadget
HTML. The public VM launcher includes the relay; updating the guest is a
separate step.

See the [Sidebar documentation](docs/VISTA_SIDEBAR_GADGETS.md) and
[public setup instructions](docs/DESKTOP-MODULES.md#sidebar).

## Supporting work

The repository also includes desktop control and file transfer tools, UI
automation, tracing, Media Center diagnostics, source bootstrap helpers, and
x64/x86 build and packaging workflows. These support the individual Vista
projects and their investigation records.

This is experimental source for developers. Build and CPU test results are
separate from validation of the applications running in Vista. Historical
reports describe the builds tested at the time.

## Start here

Clone normally, without `--recurse-submodules`, then choose the project you
want to work on:

- **Triton:** follow [BUILDING](docs/BUILDING.md). Restore the pinned host sources
  with `python3 scripts/bootstrap_sources.py` and
  `python3 scripts/bootstrap_qemu_sources.py` before building the host stack.
- **IE/MSHTML and Sidebar:** follow [DESKTOP-MODULES](docs/DESKTOP-MODULES.md)
  for build commands, guest setup and current implementation status.
- **Tests and diagnostics:** start with the [test index](tests/README.md) and
  [desktop control guide](docs/VISTA_CONTROL.md).

Build the shared toolchain image with `bash scripts/dev-container.sh build`
when needed by your chosen workflow. For Triton, the source bootstrap restores
bundled DXVK and applies the curated backend and shader-compiler patches;
a recursive submodule checkout alone is insufficient.

Microsoft SDK/WDK inputs and a licensed Vista installation are separate inputs.
No Windows installation media, VM disk or private signing key is included.

Additional references:

- [Installer/CI behavior](docs/CI.md) and [driver installation](docs/INSTALL-ISO.txt)
- [Upstream provenance and licensing decisions](docs/UPSTREAM.md)
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
