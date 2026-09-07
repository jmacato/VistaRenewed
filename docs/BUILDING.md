# Linux development setup

These are the existing development scripts, organized into their dependency
order. The tested host is Fedora with rootless Podman and an Intel Vulkan GPU;
build tools run in Ubuntu 24.04. This is not an unattended installer.

## Source and container

Run from the repository root. Host prerequisites include Git, Python 3.11 or
newer, Podman, QEMU's `qemu-img`, and access to `/dev/kvm` and the GPU render node.

```sh
python3 scripts/bootstrap_sources.py
podman build -f scripts/linux-driver.Containerfile \
  -t localhost/vista-driver-builder:20260907 scripts
podman run -d --name vista-driver-builder \
  --security-opt label=disable --device /dev/dri/renderD128 \
  --group-add keep-groups -v "$PWD:/workspace" \
  localhost/vista-driver-builder:20260907 sleep infinity
```

If a container with that name already exists, reuse it with `podman start`
instead of creating a second one. The scripts expect the `/workspace` mount.

## SDK and WDK inputs

Fetch the pinned header packages for the user-mode driver:

```sh
podman exec vista-driver-builder python3 scripts/fetch_vista_sdk_headers.py
```

The kernel driver additionally needs WDK 7.1. Place the original
`GRMWDK_EN_7600_1.ISO` at `driver/toolchains/wdk71/GRMWDK_EN_7600_1.ISO`.
The expected SHA-256 is
`5edc723b50ea28a070cad361dd0927df402b7a861a036bbcf11d27ebba77657d`.
The archive provenance URL is recorded in `scripts/extract_archived_wdk71.py`.
The extractor verifies the media before extracting its headers and libraries:

```sh
podman exec vista-driver-builder python3 scripts/extract_archived_wdk71.py
```

Microsoft downloads and guest installation files are not part of this repository.

## Build and check

```sh
podman exec vista-driver-builder bash scripts/build_linux_graphics.sh
podman exec vista-driver-builder bash scripts/build_linux_qemu.sh
podman exec vista-driver-builder bash scripts/build_vista_umd_linux.sh
podman exec vista-driver-builder python3 scripts/build_vista_kmd_linux.py --arch x64
podman exec vista-driver-builder python3 scripts/build_vista_kmd_linux.py --arch x86
podman exec vista-driver-builder bash scripts/build_vista_service_linux.sh
podman exec vista-driver-builder bash scripts/test_linux_graphics.sh
```

Build scripts run the associated PE/import and renderer checks. KMD logging
defaults to warnings/errors; pass `--verbose-trace` only when per-command
serial tracing is needed. It can be expensive in a VM.

## Test signing and deployment

`scripts/sign_vista_linux_package.sh` assembles the x64 package with the native
and WoW64 UMDs, signs it, creates the catalog, and stages a development ISO.
Before using it on a new machine:

1. Create your own development signing identity outside the repository. The
   script reads `linux-vista-test.key` and `linux-vista-test.pem` from
   `VISTA_SIGNING_DIRECTORY` (default `~/.local/share/triton-vista-signing`).
2. Export its DER certificate to
   `driver/signing/triton-vista-linux-signing.cer`. Update the deployment
   service's certificate hash and thumbprint pins consistently; see
   `test-artifacts/vista-driver-deploy-service.c`. Do not remove verification
   to make a mismatched identity work.
3. Provide a Python environment with `pefile`, `asn1crypto` and `signify`, and
   pass its interpreter as `VISTA_PYTHON`. The historical default points to a
   local `/tmp` environment. Host ISO staging also needs `xorriso`.
4. Build/sign using `bash scripts/sign_vista_linux_package.sh`, with those
   environment variables set. No private signing key is supplied here.

Use a separately installed Vista development guest with a working overlay at
`vista-kvm/work.qcow2`. Keep its backing image unchanged. Configure development
driver signing inside the guest and run the generated media's `bootstrap.cmd`
as administrator once. The guest service then owns installation and reboots;
see [deployment details](../test-artifacts/VISTA_GUEST_DEPLOY.md).

With the overlay and an immutable publication under `vista-kvm/deploy-current`:

```sh
VISTA_DISPLAY=gtk VISTA_ACCEL=kvm python3 scripts/run_vista_neptune_linux.py
```

Native GTK requires `DISPLAY` and an existing `XAUTHORITY` file. The launcher
defaults to four vCPUs, 2 GiB RAM and a USB tablet. `VISTA_CPUS` changes the CPU
count. The current GPU selection targets Intel and `/dev/dri/renderD128`;
other hosts need explicit adaptation and testing. The default display without
`VISTA_DISPLAY=gtk` is EGL headless with a local VNC socket.

## Historical tools

`build_deploy_vista_driver.sh`, `build_vista_deploy_service.sh` and the older
Windows batch files describe the previous macOS/Parallels build route. They
remain for provenance and are not the Linux setup above. The lengthy handoff
and resume notes contain intermediate failures as well as later fixes.
