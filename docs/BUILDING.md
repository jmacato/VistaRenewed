# Build the developer preview

Run commands from the repository root on an x86-64 Linux host. Git, Python 3.11+
and Podman or Docker are needed for source restoration and container builds.
Compilation runs without a GPU or VM. Graphics tests need Vulkan/EGL support
and a Vista VM with the matching host components.

## 1. Restore source and create the build environment

Clone without `--recurse-submodules`, then run:

```sh
python3 scripts/bootstrap_sources.py
python3 scripts/bootstrap_qemu_sources.py
bash scripts/dev-container.sh build
```

Bootstrap restores DXVK and its dependencies, then applies the project patches.
Local edits to those sources stop the bootstrap.

The QEMU bootstrap restores the four C dependencies at the revisions in its
checked-in Meson wrap files and applies their source overlays. It checks
existing files before making changes. Missing dependencies stop the QEMU build.

`dev-container.sh run COMMAND ...` starts a disposable container with this
checkout mounted at `/workspace`. Each command gets a new container. Podman is preferred when available; set `CONTAINER_ENGINE=docker` to
choose Docker. Set `VISTA_BUILD_IMAGE` to select a different image tag. Podman
uses the invoking user's identity; Docker's default root process can leave
root-owned build files. The workspace mount uses disabled SELinux labeling. The build container has
no graphics or KVM devices.

The base image and direct Python package versions are pinned. APT packages
and indirect Python dependencies are resolved when the image builds, so builds
can differ. Record the container image ID when sharing a package.

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

If the ISO is missing, `--download` fetches it from the archive URL in the
script. You need permission to use these Microsoft files. Downloads, Windows
media and VM disks stay outside source control.

## 3. Build matching Linux host components

```sh
bash scripts/dev-container.sh run bash scripts/build_linux_graphics.sh
bash scripts/dev-container.sh run bash scripts/build_linux_qemu.sh
```

The first command builds and installs native DXVK plus Neptune-enabled
virglrenderer into `host-linux/`, then runs the renderer initialization test.
The second produces `triton-qemu/build-linux/qemu-system-x86_64`; it enables GTK,
OpenGL, virglrenderer, KVM, SLIRP and PipeWire. The container includes the
corresponding development packages. The QEMU executable stays in the checkout.

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
`MESON` selects an alternative Meson executable. Stop the VM before rebuilding libraries it uses, or build to separate paths.

## 4. Build both guest architectures

```sh
bash scripts/dev-container.sh run bash scripts/build_vista_umd_linux.sh
bash scripts/dev-container.sh run python3 scripts/build_vista_kmd_linux.py --arch x64
bash scripts/dev-container.sh run python3 scripts/build_vista_kmd_linux.py --arch x86
```

The UMD script builds x64 and x86 by default; `--arch x64` or `--arch x86`
selects one. Package matching D3D9/D3D10 DLLs, KMD, deployment service, INF/catalog and host
components together. Verbose KMD tracing (`--verbose-trace`) can slow the guest.

## 5. Test, package and install

The VM launcher carries the host timezone into QEMU for Vista's local-time RTC.
Set `TZ` explicitly if the host timezone cannot be detected. Confirm that the
guest clock is correct before installing: a shifted clock can reject the
development certificate as not yet valid or expired.

Run the CPU tests after source bootstrap:

```sh
bash scripts/dev-container.sh run python3 scripts/test_public.py
```

`python3 scripts/test_public.py --list` lists the CPU tests.
See [the test index](../tests/README.md) for suite entry points and
[the Vista test guide](../tests/vista/README.md) for individual CPU regression commands,
native GPU tests and guest probes. Run GPU tests on the host with the built
libraries and access to the chosen GPU. Keep them separate from VM performance
captures.

For x64 and x86 installers with temporary development signing identities:

```sh
bash scripts/dev-container.sh run bash scripts/ci_build_driver_iso.sh
```

This also repeats prerequisite extraction and guest builds. Its archive download
is unnecessary when the verified WDK ISO is already supplied. See [CI](CI.md)
for outputs and signing details. The x64 ISO includes WoW64 DLLs; the x86 ISO
targets a 32-bit guest.

Install only into a disposable Vista test VM, using [INSTALL-ISO.txt](INSTALL-ISO.txt)
and the matching QEMU/backend from this source revision. Keep installation media and VM disks outside version control. The Podman
launcher uses `VISTA_DISK` for your writable qcow2 image;
inspect `bash run-vm.sh --help` before launching. Set `VISTA_RENDER_NODE` for
the intended host GPU and configure NVIDIA CDI when applicable. Audio defaults
to `VISTA_AUDIO=auto`, which uses PipeWire when available; `none` disables it
and `pipewire` requires it.
QEMU draw diagnostics are disabled by default; set `VISTA_DISPLAY_STATS=1`
to log display draw counts.
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
media under `dist/persistent/`.
Use `VISTA_SIGNING_DIRECTORY` inside the container to select a different mounted
input directory. Keep private keys outside source control.

## 6. Check the result

Test the installed package with its matching host build. See
[current limits](STATUS.md) for compatibility and display status.
