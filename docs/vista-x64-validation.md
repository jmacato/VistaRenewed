# Vista x64 adaptation — 2026-09-08

This adaptation builds on `8df0bf6d0`. The presentation timeline, same-context
consumption, KMD copy ordering, and QEMU retained-primary readback changes are
shared by x86 and x64. The x64 package includes the rebuilt x86 WoW64 UMD.

Older Linux MinGW dxguid archives lack the D3D11 fence interface IDs used by
the new timeline code. `triton9_guids.c` emits the SDK header definitions for
both architectures without adding an operating-system D3D11 dependency.
The ISO packager now checks certificate extensions directly because OpenSSL
3.0 does not expose the code-signing purpose label used by the x86 update.

The x64 installer keeps Test Mode enabled for the self-signed kernel driver,
sets `nointegritychecks off`, and removes legacy
`DDISABLE_INTEGRITY_CHECKS` loadoptions. The x86 normal-signing policy remains
in place. Microsoft documents the x64 requirement in
[Introduction to Test-Signing](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/introduction-to-test-signing).

Local Linux validation:

- x64 KMD build and Vista PE audit passed.
- x64 and x86 UMDs, probes, and ABI compile targets built; PE audits passed.
- Matching Neptune QEMU built successfully.
- All six `tests/vista/test-*.py` scripts passed, including both signing policies.
- Host graphics tests reported 212 passes and zero failures; the optional
  dma-buf shared-texture test was skipped.
- `dist/triton-vista-x64.iso` was generated with an ephemeral development
  certificate. Embedded signatures, catalog membership, the x64 INF, and
  independently extracted ISO contents passed the package checks.

The separate x86 Clang varargs audit could not compile on this case-sensitive
WDK extraction (`specstrings.h` was not found). The KMD build uses its own
case-insensitive VFS overlay and passed.

This build has not been booted in a Vista x64 guest. Driver loading, installation
reboots, Aero, and visual flicker still require guest validation; the earlier
x86 guest results do not establish these outcomes for x64.
