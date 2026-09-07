# Vista driver build and signing route

Date: 2026-08-26

## Result

The Tiny11 build VM can build and sign fresh Vista display miniports for x64 and x86. The route does not reuse the old transfer binaries.

The host exposes the source as a read-only shared directory. The VM writes build products only to the repository transfer directory.

This result is package preparation. It does not prove a Vista installation, a public D3D9 result, or Aero glass.

## Installed tools

Visual Studio 2022 Build Tools is installed at `C:\BuildTools`. The developer prompt reports version 17.14.39.

The C++ compiler reports version 19.44.35228. MSBuild reports version 17.14.51.32402.

WDK 7.1 is installed at `C:\WinDDK\7600.16385.win7_wdk.100208-1538`. The installation includes Vista x64 and x86 headers and libraries.

The source WDK ISO has this SHA-256 value:

```text
5edc723b50ea28a070cad361dd0927df402b7a861a036bbcf11d27ebba77657d
```

The task removed its temporary copy of the mounted ISO after installation. The original repository ISO was not changed.

The build uses the repository copies of Inf2Cat and SignTool. It does not depend on an unknown system PATH entry.

## Signing identity

The VM created a non-exportable RSA code-signing key in the local-machine certificate store. The private key never enters the host repository.

The certificate subject is:

```text
O=Local Development Only, CN=Triton Vista Unlazy Test Signing 20260826
```

The certificate thumbprint is:

```text
2464DC7241B33AF0E6D333ED6D7542ADAD59DC1C
```

The certificate is valid from 2026-08-25 22:17:23 UTC through 2036-08-25 22:27:24 UTC.

The public certificate is `aaaaa/vista-signing-transfer/triton-vista-unlazy-signing.cer`. Its SHA-256 value is:

```text
d7ab5e9f4f1e271d102f9a4a70d1078ad914f3fbbecb369957584cbf61d34ff1
```

The build VM trusts this exact certificate in its local-machine Root and TrustedPublisher stores. The Vista deployment service imports the same public certificate.

The deployment service checks the certificate file hash and signer thumbprint. It rejects packages that use another signing identity.

## Fresh miniport proof

This command built both miniports from the current source:

```text
Y:\test-artifacts\windows11_build_vista_serialtrace_kmd.bat
```

Both MSBuild phases completed with zero errors. Each phase reported 13 known `CO_E_NOTINITIALIZED` macro redefinition warnings.

The script then signed and verified both files. The signed x64 miniport has this identity:

```text
size:   205320 bytes
sha256: 594afbd9fb4f6145de012390c79896e412c0121f6ef19485db6d84f15f546303
```

The signed x86 miniport has this identity:

```text
size:   176648 bytes
sha256: 82652ceff45cc871500a941f724e8863429a04f3fef209150c38b938e6df2a16
```

The Vista PE audit passed for both files. Their imported modules are only `ntoskrnl.exe` and `HAL.dll`.

SignTool `/pa /v` verified both signatures against the pinned certificate. The signatures do not have an external timestamp.

These hashes describe the route-validation build. The final package must rebuild and sign after all source owners finish.

## Package rules

`scripts/build_deploy_vista_driver.sh --build-only` rebuilds the current UMDs, service, probe, and both miniports. It then asks Tiny11 to create signed Vista catalogs.

The staging batch signs every executable payload before it creates each catalog. It then signs and verifies each catalog.

The package manifest includes the public probe. The deployment media also carries the exact public certificate and its SHA-256 value.

The service verifies the catalog signer, every catalog member, every embedded signature, and every package hash. A mismatch stops deployment.

After a guest-owned restart, the service compares the active binaries with the package. It writes `LastSuccessId` only after all checks pass.

`scripts/stage_vista_deploy_media.sh` creates ISO9660/Joliet optical media. It does not create a raw disk image or a writable VM disk derivative.

## Remaining gate

Leaf 1.3.1.1 stays open until the integrated source builds, signs, verifies, and stages without an error. The final package hashes will replace this route-validation evidence.
