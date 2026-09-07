# Driver ISO builds

The **Vista driver ISO** GitHub Actions workflow runs on pull requests, pushes
to `main`, version tags and manual dispatch. It produces a downloadable Actions
artifact; it does not publish a GitHub release or touch a VM.

The x64 installer contains the miniport, native and WoW64 D3D9 DLLs, deployment
service, public graphics probe, INF, signed catalog, public test certificate,
`install.cmd` and installation instructions. It is a data CD, not a bootable
Windows image. It requires the matching Neptune host stack.

The build runs in the pinned Ubuntu container, restores hash-checked SDK/WDK
inputs, compiles both driver architectures, runs the PE/INF checks, test-signs
the x64 package and verifies its catalog members. It extracts the finished ISO
and compares every file with the staging manifest before exporting artifacts.
No GPU, KVM device, Windows guest or persistent signing secret is needed in CI.
The WDK download depends on Internet Archive availability; a failed download or
hash check fails the build instead of silently choosing different inputs.

## Run the same job locally

Use a separate checkout so the CI build does not replace development binaries:

```sh
docker build -f scripts/linux-driver.Containerfile -t vista-driver-ci scripts
docker run --rm -v "$PWD:/workspace" \
  -e VISTA_SOURCE_REVISION="$(git rev-parse HEAD)" \
  vista-driver-ci bash scripts/ci_build_driver_iso.sh
```

Podman can run the same commands (add `--security-opt label=disable` for the
workspace mount on SELinux hosts). A preexisting, correctly hashed WDK ISO at
`driver/toolchains/wdk71/GRMWDK_EN_7600_1.ISO` avoids downloading it again.

Outputs are `dist/triton-vista-x64.iso`, `SHA256SUMS`, `build-info.json`, and
`INSTALL.txt`. The GitHub artifact retains these for 14 days; failure logs are
retained for seven days. Installation is described in [the ISO instructions](INSTALL-ISO.txt).

## Signing and installation limits

Each build generates a one-year self-signed development certificate. Its DER
SHA-256 and SHA-1 thumbprint are compiled into the deployment service; the key
exists only in a temporary directory and is removed after packaging. The ISO
contains the public certificate. The local development certificate and default
service pins are unchanged when building without `VISTA_DEPLOY_CERT`.

Vista-compatible SHA-1 signing and the existing checked-build deployment
behavior are retained. The service skips its redundant WinVerifyTrust pass,
checks the pinned certificate and manifest, and configures development boot
integrity settings. Host packaging verifies the signatures and catalog. This
is a test installer, not a WHQL or production signing pipeline.

Use a pre-install VM snapshot for each new build: automatic upgrades across
ephemeral signing identities are not implemented. CI does not boot Vista;
`guest_runtime_tested` is explicitly false in the build metadata. The earlier
Aero evidence does not automatically apply to every newly built ISO.

The scripts reproduce the build procedure, not byte-identical ISOs: certificate
keys, signatures, catalog identifiers and timestamps change between builds.
