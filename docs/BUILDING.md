# Linux development setup

These instructions list the existing development scripts in dependency order.
The tested host uses Fedora, rootless Podman and an Intel graphics processor.
Podman runs the Ubuntu 24.04 build container without host administrator privileges.

For an installer without a persistent signing key, use the [continuous integration (CI) ISO workflow](CI.md).
An ISO file contains a disc image.
The following instructions configure the persistent local development environment.

## Source and container

Run commands from the repository root.
Install Git, Python 3.11 or newer, Podman and QEMU's `qemu-img` utility on the host.
Give the build account access to `/dev/kvm` and the graphics render device.
Kernel-based Virtual Machine (KVM) supplies hardware virtualization.

If the `vista-driver-builder` container does not exist, run these commands:

```sh
python3 scripts/bootstrap_sources.py
podman build -f scripts/linux-driver.Containerfile \
  -t localhost/vista-driver-builder:20260907 scripts
podman run -d --name vista-driver-builder \
  --security-opt label=disable --device /dev/dri/renderD128 \
  --group-add keep-groups -v "$PWD:/workspace" \
  localhost/vista-driver-builder:20260907 sleep infinity
```

If the container already exists, reuse it with `podman start`.
Do not create a second container with the same name.
The scripts require the `/workspace` mount.

## SDK and WDK inputs

The Software Development Kit (SDK) and Windows Driver Kit (WDK) supply build headers and libraries.
The user-mode driver runs outside the Windows kernel.
Download its pinned header packages:

```sh
podman exec vista-driver-builder python3 scripts/fetch_vista_sdk_headers.py
```

The kernel driver also requires WDK 7.1.
Put the original `GRMWDK_EN_7600_1.ISO` file at `driver/toolchains/wdk71/GRMWDK_EN_7600_1.ISO`.
The expected SHA-256 file hash is `5edc723b50ea28a070cad361dd0927df402b7a861a036bbcf11d27ebba77657d`.
The `scripts/extract_archived_wdk71.py` file records the archive source address.
The extractor verifies the media before it extracts headers and libraries.

After you supply the ISO, run this command:

```sh
podman exec vista-driver-builder python3 scripts/extract_archived_wdk71.py
```

Git does not contain Microsoft downloads or guest installation files.

## Build and check

After you restore the build inputs, run these commands in order:

```sh
podman exec vista-driver-builder bash scripts/build_linux_graphics.sh
podman exec vista-driver-builder bash scripts/build_linux_qemu.sh
podman exec vista-driver-builder bash scripts/build_vista_umd_linux.sh
podman exec vista-driver-builder python3 scripts/build_vista_kmd_linux.py --arch x64
podman exec vista-driver-builder python3 scripts/build_vista_kmd_linux.py --arch x86
podman exec vista-driver-builder bash scripts/build_vista_service_linux.sh
podman exec vista-driver-builder bash scripts/test_linux_graphics.sh
```

The scripts check Portable Executable (PE) files, imported functions and the renderer.
The kernel-mode driver (KMD) logs warnings and errors by default.
For command-level serial diagnostics, pass `--verbose-trace` to its build script.
Serial tracing can slow the virtual machine (VM).

## Test signing and deployment

The `scripts/sign_vista_linux_package.sh` script assembles and signs the x64 package.
It includes native and 32-bit compatibility user-mode drivers (UMDs).
It creates a signed catalog and stages a development ISO.
A catalog records signed package membership.

Before you use this script on a new machine, complete these steps:

1. Create a development signing identity outside the repository.
2. Put `linux-vista-test.key` and `linux-vista-test.pem` in the directory specified by `VISTA_SIGNING_DIRECTORY`.
3. Export the DER certificate to `driver/signing/triton-vista-linux-signing.cer`.
4. Set the deployment service's certificate hash and thumbprint pins to match this certificate.
5. Create a Python environment with `pefile`, `asn1crypto` and `signify`.
6. Set `VISTA_PYTHON` to that environment's interpreter.
7. Install `xorriso` on the host for ISO staging.
8. With those variables set, run `bash scripts/sign_vista_linux_package.sh`.

Distinguished Encoding Rules (DER) define the certificate's binary format.
A certificate pin identifies the signing certificate that the service accepts.
The service source is `packaging/vista-driver-deploy-service.c`.
Do not remove certificate verification to accept a mismatched identity.
The default signing directory is `~/.local/share/triton-vista-signing`.
The historical Python default uses a local `/tmp` environment.
The repository supplies no private signing key.

For installation, follow [the ISO instructions](INSTALL-ISO.txt).
The guest must use the matching Neptune-enabled QEMU and renderer built above.
