# Driver installer builds

The Vista driver ISO GitHub Actions workflow builds and checks development
installers on pull requests, configured branch/tag pushes and manual dispatch.
See [the workflow](../.github/workflows/driver-iso.yml) for its current triggers
and retention settings. It uploads artifacts; it does not publish a release,
boot Vista, or certify graphics compatibility.

## Build locally

From the checkout root:

```sh
bash scripts/dev-container.sh build
bash scripts/dev-container.sh run bash scripts/ci_build_driver_iso.sh
```

The build downloads hash-verified header inputs and, if absent, the pinned WDK
7.1 archive. To avoid the WDK download, supply the verified ISO described in
[BUILDING](BUILDING.md). Unavailable or mismatched inputs stop the build. SDK/WDK
usage rights and network availability are external prerequisites.

The pipeline compiles x64 and x86 guest drivers, validates PE imports and INF
registration, and packages both x64 and x86 installers. To repeat x86 packaging separately
after both builds:

```sh
bash scripts/dev-container.sh run python3 scripts/package_vista_ci_iso.py --arch x86
```

The x64 output is `dist/triton-vista-x64.iso`, `dist/SHA256SUMS`,
`dist/build-info.json` and `dist/INSTALL.txt`. The x86 outputs are under
`dist/x86/`. Package success is not proof of x86 guest compatibility.

## Contents and checks

The x64 installer contains the KMD, native and WoW64 D3D9/D3D10 user-mode DLLs,
runtime probes, deployment service, INF, signed catalog and public development
certificate. It includes an installation command, license notices and
instructions. This is a driver data disc, not bootable Windows media. It needs
a matching Neptune-enabled QEMU, renderer and DXVK build.

The installer includes `NOTICES/SOURCE-NOTICES.txt`, a deterministic collection
of existing copyright/SPDX source comment blocks from guest source components,
packaging and guest tests. This is a conservative attribution superset, not an
exact binary dependency inventory or a new license grant. Full existing license
texts remain alongside it; missing standalone helper grants stay unresolved.

Packaging validates catalog membership and signatures, extracts the finished
ISO and checks it against the staging manifest. No GPU, KVM device, guest VM
or persistent signing secret is needed. Build metadata records
`guest_runtime_tested: false`; leave that false unless separately verified
runtime evidence is attached to that exact artifact.

## Signing limitations

Each package build creates a temporary self-signed development identity and
embeds the public certificate's hash/thumbprint pins in its deployment service.
The packager removes temporary private-key material. It does not supply WHQL
certification or a production driver signature. The service and package retain
Vista-compatible SHA-1/development signing behavior; follow the architecture's
installation instructions and review the guest integrity-setting changes.

For a new package identity, restore a VM snapshot from before installation.
Automatic upgrades across temporary signing identities are not established.
Never substitute an unrelated certificate or weaken certificate-pin checks to
make a package install.

The container base digest and direct Python dependencies are pinned, but OS
package resolution, certificate keys, signatures and timestamps can vary.
Builds repeat the procedure; they do not promise identical ISO bytes. Record
the source revision, source patch manifest, image ID and generated checksums.

Compilation, signing and artifact validation remain separate from the actual
Vista [runtime and presentation gates](RELEASE-VALIDATION.md).
