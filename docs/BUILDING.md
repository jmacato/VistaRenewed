# Build the developer preview

Run commands from the repository root on an x86-64 Linux host. Git, Python 3.11+
and Podman or Docker are needed for source restoration and container builds.
No GPU or VM is needed to compile. GPU tests and Vista validation are separate
steps and require a working Vulkan/EGL host and compatible QEMU runtime setup.

## 1. Restore source and create the build environment

Clone without `--recurse-submodules`, then run:

```sh
python3 scripts/bootstrap_sources.py
python3 scripts/bootstrap_qemu_sources.py
bash scripts/dev-container.sh build
```

Bootstrap restores the pinned DXVK source bundle, nested sources and downstream
patch series. Do not manually apply older development patches afterward. Keep
local edits out of restored dependencies when checking reproducibility.

The QEMU bootstrap restores the four C dependencies at the revisions in its
checked-in Meson wrap files and applies their source overlays. It validates
existing contents before making changes. QEMU compilation keeps downloads
disabled so an absent dependency fails explicitly.

`dev-container.sh run COMMAND ...` starts a disposable container with this
checkout mounted at `/workspace`. It does not require a preexisting named
container. Podman is preferred when available; set `CONTAINER_ENGINE=docker` to
choose Docker. Set `VISTA_BUILD_IMAGE` to select a different image tag. Podman
uses the invoking user's identity; Docker's default root process can leave
root-owned build files. The wrapper disables SELinux labeling for the workspace
mount and gives the container no graphics or KVM devices.

The base image is pinned by digest and direct Python package versions are pinned.
APT packages and transitive Python dependencies are resolved during image build;
this is a repeatable source procedure, **not a fully locked or bit-identical
binary build**. Record the resulting container image ID with release evidence.

## 2. Supply Microsoft build inputs

Fetch the hash-verified SDK/WDK header packages used by the user-mode build:

```sh
bash scripts/dev-container.sh run python3 scripts/fetch_vista_sdk_headers.py
```

The kernel driver also needs WDK 7.1 media. Supply
`driver/toolchains/wdk71/GRMWDK_EN_7600_1.ISO` yourself. Its expected SHA-256 is
`5edc723b50ea28a070cad361dd0927df402b7a861a036bbcf11d27ebba77657d`.
Then extract its verified build inputs:

```sh
bash scripts/dev-container.sh run python3 scripts/extract_archived_wdk71.py
```

The extractor's explicit `--download` option uses the archive URL recorded in
its source when the ISO is absent. Network availability and rights to use these
Microsoft inputs are external prerequisites. The source repository does not
contain those downloads, a Windows installation, or a VM disk.

## 3. Build matching Linux host components

```sh
bash scripts/dev-container.sh run bash scripts/build_linux_graphics.sh
bash scripts/dev-container.sh run bash scripts/build_linux_qemu.sh
```

The first command builds and installs native DXVK plus Neptune-enabled
virglrenderer into `host-linux/`, then runs the renderer initialization test.
The second produces `triton-qemu/build-linux/qemu-system-x86_64`; it enables GTK,
OpenGL, virglrenderer, KVM, SLIRP and PipeWire. The container includes the
corresponding development packages. It does not install a system QEMU.

Defaults preserve the component build paths used by tests. For isolated builds,
set absolute `TRITON_HOST_PREFIX`, `TRITON_DXVK_BUILD`,
`TRITON_RENDERER_BUILD` and `TRITON_QEMU_BUILD` paths inside the container:

```sh
bash scripts/dev-container.sh run env TRITON_HOST_PREFIX=/workspace/build/host \
  TRITON_DXVK_BUILD=/workspace/build/dxvk \
  TRITON_RENDERER_BUILD=/workspace/build/renderer bash scripts/build_linux_graphics.sh
```

Pass the same prefix when building QEMU. Individual test runners may require
explicit paths when using these overrides. `JOBS=4` is the default parallelism;
`MESON` selects an alternative Meson executable. Never rebuild libraries mapped
by a running VM; stop that VM or use separate output paths.

## 4. Build both guest architectures

```sh
bash scripts/dev-container.sh run bash scripts/build_vista_umd_linux.sh
bash scripts/dev-container.sh run python3 scripts/build_vista_kmd_linux.py --arch x64
bash scripts/dev-container.sh run python3 scripts/build_vista_kmd_linux.py --arch x86
```

The UMD script builds x64 and x86 by default; `--arch x64` or `--arch x86`
selects one. Keep D3D9/D3D10 DLLs, KMD, deployment service, INF/catalog and host
protocol revision together. PE/import validation is a build check; it does not
prove that either guest architecture boots or renders correctly. Verbose KMD
serial tracing (`--verbose-trace`) is diagnostic and can distort performance.

## 5. Test, package and install

The VM launcher carries the host timezone into QEMU for Vista's local-time RTC.
Set `TZ` explicitly if the host timezone cannot be detected. Confirm that the
guest clock is correct before installing: a shifted clock can reject the
development certificate as not yet valid or expired.

Run the default CPU regression manifest after source bootstrap:

```sh
bash scripts/dev-container.sh run python3 scripts/test_public.py
```

`python3 scripts/test_public.py --list` lists the explicit CPU-only selection.
See [the test index](../tests/README.md) for suite entry points and
[the Vista test guide](../tests/vista/README.md) for individual CPU regression commands,
explicit native GPU suites and guest probes. Native tests need the freshly built
host libraries and real device access. The build wrapper intentionally does not
pretend to provide a host NVIDIA driver or a display server. Do not run native
GPU tests during game performance captures.

For x64 and x86 installers with temporary development signing identities:

```sh
bash scripts/dev-container.sh run bash scripts/ci_build_driver_iso.sh
```

This also repeats prerequisite extraction and guest builds. Its archive download
is unnecessary when the verified WDK ISO is already supplied. See [CI](CI.md)
for artifact paths, signature limits and reproducibility details. Both KMD architectures are compiled and packaged. The x64 ISO includes x86
compatibility DLLs; the separate x86 ISO targets a 32-bit guest. Neither package
build establishes runtime compatibility for its guest architecture.

Install only into a disposable Vista test VM, using [INSTALL-ISO.txt](INSTALL-ISO.txt)
and the matching QEMU/backend from this source revision. Keep the VM's licensed
installation media and disk outside version control. The Podman runtime launcher uses `VISTA_DISK` for your writable qcow2 image;
inspect `bash run-vm.sh --help` before launching. Set `VISTA_RENDER_NODE` for
the intended host GPU and configure NVIDIA CDI when applicable. Audio defaults
to `VISTA_AUDIO=auto`, which uses PipeWire when available; `none` disables it
and `pipewire` requires it. Building a package does not start or modify a VM.
QEMU draw diagnostics are disabled by default; set `VISTA_DISPLAY_STATS=1`
when investigating the display path. Those counters do not establish visible
presentation performance.
For nondefault build locations, map the build settings to the launcher's
`VISTA_HOST_PREFIX` and `VISTA_QEMU_BUILD_DIR` overrides.

For a persistent x64 development signing identity, place your existing
`linux-vista-test.key` and `linux-vista-test.pem` in the ignored
`driver/signing/` input directory, then run:

```sh
bash scripts/dev-container.sh run bash scripts/sign_vista_linux_package.sh
```

The helper reads the key in place, derives the public certificate and service
pins from that identity, verifies signatures/catalog membership, and stages
media under `dist/persistent/`. It does not change a VM deployment pointer.
Use `VISTA_SIGNING_DIRECTORY` inside the container to select a different mounted
input directory. Keep private keys outside source control.

## 6. Check the result

Build and package checks do not prove Vista application compatibility or smooth
output in QEMU. Test the installed package with its matching host build and
read the [current limits](STATUS.md).
