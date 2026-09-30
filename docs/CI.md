# Driver installer builds

[GitHub Actions](../.github/workflows/driver-iso.yml) builds installers on pull
requests, branch and tag pushes, and manual runs. The workflow uploads build
artifacts and logs.

The `host-and-cpu` job restores source, runs CPU tests, builds the IE/MSHTML
adapter, and compiles the Linux renderer and QEMU. The driver packaging job
runs after that job passes. Vista runtime testing is a separate step.

## Build locally

From the repo root:

```sh
bash scripts/dev-container.sh build
bash scripts/dev-container.sh run bash scripts/ci_build_driver_iso.sh
```

The build downloads the SDK/WDK headers and WDK 7.1 archive, checking their
hashes. You can supply the WDK ISO yourself; see [BUILDING](BUILDING.md).
Missing or mismatched inputs stop the build.

The pipeline compiles x64 and x86 drivers, checks PE imports and INF entries,
and packages both installers. To package x86 again after building the drivers:

```sh
bash scripts/dev-container.sh run python3 scripts/package_vista_ci_iso.py --arch x86
```

The x64 output is `dist/triton-vista-x64.iso`, with checksums, build information
and install instructions in `dist/`. The x86 output is in `dist/x86/`.

## Package contents

The x64 ISO contains the kernel driver, native and WoW64 D3D9/D3D10 DLLs,
probes, deployment service, INF, signed catalog and public test certificate.
The x86 ISO contains the 32-bit drivers. Both need the matching QEMU,
renderer and DXVK builds from this project.

`NOTICES/SOURCE-NOTICES.txt` collects copyright and SPDX comments from the guest
source, packaging and tests. The packages also include component license texts,
`LICENSE-scope.md` and `MIT-original.txt`.

Packaging checks signatures and catalog entries, then extracts the ISO and
compares it with the staged files. Build information records
`guest_runtime_tested: false` because this workflow does not run Vista.

## Signing

Each CI build creates a temporary self-signed test certificate. Its deployment
service checks the matching certificate and package signatures. Packaging
removes the temporary private key. These are development packages without
Microsoft certification.

Restore a pre-install VM snapshot before testing a build with another test
certificate. Keep the service and certificate from the same package together.
See the ISO's install instructions for changes to Vista's boot settings.

Package resolution, signing keys and timestamps can vary between builds.
Keep the source revision, image ID and checksums with the package.
For runtime limitations, see [current limits](STATUS.md).
