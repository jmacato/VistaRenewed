# Driver ISO builds

Continuous integration (CI) runs automated build checks.
An ISO file contains a disc image.
The Vista driver ISO workflow uses GitHub Actions to build an installer image.
It runs on pull requests, pushes to `main`, version tags and manual requests.
The workflow uploads downloadable build artifacts.
It does not publish a GitHub release or control a virtual machine (VM).

## Package contents

The x64 installer contains the kernel display driver and Direct3D 9 (D3D9) dynamic-link libraries (DLLs).
It includes native and Windows-on-Windows 64-bit (WoW64) DLLs.
WoW64 runs 32-bit applications on 64-bit Windows.
The installer also contains the deployment service, graphics probe, installation information (INF) file, signed catalog and public test certificate.
A catalog records signed package membership.
The disc includes `install.cmd`, license notices and installation instructions.
It is a data disc, not a bootable Windows image.
It requires the matching Neptune host graphics software.

## Build checks

The build uses an Ubuntu container with a fixed base image hash.
It verifies hashes for Software Development Kit (SDK) and Windows Driver Kit (WDK) inputs.
It compiles both driver architectures and checks Portable Executable (PE) files and the INF.
It signs the x64 package with a development certificate and verifies catalog membership.
It extracts the completed ISO and compares every file against the staging manifest.
CI needs no graphics processor, hardware virtualization device, Windows guest or persistent signing secret.

The WDK download depends on Internet Archive availability.
A failed download or hash check stops the build.
The build does not substitute different inputs.

## Local build

Use a separate checkout to protect existing development binaries.
Docker builds and runs containers.
From that checkout, run these commands:

```sh
docker build -f scripts/linux-driver.Containerfile -t vista-driver-ci scripts
docker run --rm -v "$PWD:/workspace" \
  -e VISTA_SOURCE_REVISION="$(git rev-parse HEAD)" \
  vista-driver-ci bash scripts/ci_build_driver_iso.sh
```

Podman also supports these commands.
On Security-Enhanced Linux (SELinux) hosts, add `--security-opt label=disable` for the workspace mount.
SELinux controls process access to host resources.
An existing WDK ISO with the expected hash avoids another download.
Its required path is `driver/toolchains/wdk71/GRMWDK_EN_7600_1.ISO`.

The output directory contains `dist/triton-vista-x64.iso`, `SHA256SUMS`, `build-info.json` and `INSTALL.txt`.
GitHub retains installer artifacts for 14 days and failure logs for seven days.
For installation, follow [the ISO instructions](INSTALL-ISO.txt).

## Signing and installation limits

Each build generates a self-signed development certificate with one year of validity.
Distinguished Encoding Rules (DER) define its binary format.
The deployment service contains the certificate's SHA-256 file hash and SHA-1 thumbprint.
A thumbprint is a certificate hash.
These certificate pins identify the accepted signing certificate.
The private key exists only in a temporary directory.
The packager removes this directory after packaging.
The ISO contains the public certificate.

Without `VISTA_DEPLOY_CERT`, service builds retain the existing local certificate pins.
The CI build does not change the local development certificate.
The package retains Vista-compatible SHA-1 signatures and the existing checked-build deployment behavior.
The service skips its redundant WinVerifyTrust signature check.
It checks the certificate pin and manifest, then configures development boot integrity settings.
The host packager verifies signatures and catalog membership.
The installer has no Windows Hardware Quality Labs (WHQL) certification or production signature.

Before each new build test, restore a VM snapshot from before installation.
Automatic upgrades between temporary signing identities are not available.
CI does not boot Vista.
Build metadata sets `guest_runtime_tested` to false.
Earlier Aero evidence does not establish runtime correctness for a new ISO.

The scripts repeat the build procedure.
They do not produce identical ISO bytes.
Certificate keys, signatures, catalog identifiers and timestamps change between builds.
