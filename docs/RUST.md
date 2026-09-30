# Rust feasibility assessment

> Development record retained with the public desktop sources. Local capture,
> VM and `.unlazy/` paths refer to non-shipped development artifacts. See
> [current public entry points and status](DESKTOP-MODULES.md).


Assessed on 2026-09-08 against this repository and the linked toolchain
documentation. An incremental Rust component is plausible. A complete Vista
graphics-stack port is a much larger project, and ordinary Windows Rust
support does not establish Vista compatibility. No Rust component has been
built or run as part of this assessment.

## Platform constraints

Current Rust MSVC Windows targets require Windows 10 / Server 2016 or newer.
They produce PE/COFF, but that binary format alone does not establish old-OS
compatibility. Their documented C ABI is cdecl on i686 and the Windows x64 ABI
on x86_64. This matters because this repository's 32-bit driver callbacks use
stdcall. [Rust MSVC target documentation](https://doc.rust-lang.org/rustc/platform-support/windows-msvc.html)

The separate Windows 7 GNU targets are Tier 3 and require building target
artifacts; they are not a documented Vista target. Substituting a Win7 triple
does not prove that its runtime imports are available on Vista.
[Rust Windows 7 GNU documentation](https://doc.rust-lang.org/rustc/platform-support/win7-windows-gnu.html)

Microsoft's Rust driver project supplies WDK integration, but its documented
published-crate KMDF support is v1.33. It is not a ready-made binding layer for
this repository's WDK 7.1 display miniport and WDDM 1.0 contracts.
[windows-drivers-rs](https://github.com/microsoft/windows-drivers-rs)

## Candidate components

### Community routes for older Windows

Official target support is not the limit of what Rust can run on. Two concrete
user-mode options deserve a prototype before ruling out a Vista service or UMD:

- [thunk / thunk-rs](https://github.com/felixmaker/thunk) integrates VC-LTL5
  and YY-Thunks to supply a compatible CRT and replacements for missing Windows
  APIs. Its support list explicitly includes Vista x86 and x64, and the crate
  exposes a `vista` feature. Compatibility still depends on the program and
  dependencies; this has not been tested with Triton.
- [rust9x](https://github.com/rust9x/docs) ports the Rust standard library to
  legacy Windows and documents Linux-hosted MSVC builds. It lists an
  `x86_64-rust9x-windows-msvc` target intended for XP x64 and newer, explicitly
  marked untested. Crates that call Windows APIs directly may need patches.

These make a user-mode Rust port more plausible than the official support
matrix alone suggests. They do not supply Vista kernel-mode WDK bindings or
establish WDDM compatibility. Evaluate a small Vista x64 executable and DLL
with thunk first, including imports and guest execution; retain the `no_std`
experiment below for portable driver logic and eventual kernel use.

| Component | Feasibility | Main constraint |
| --- | --- | --- |
| Host-only tooling | High | Adds Cargo beside small existing Python tools; limited payoff from rewriting working scripts |
| CPU layout, region validation, packet validation | Best first driver experiment | Preserve integer widths, overflow behavior, and C ABI; avoid OS imports |
| Neptune host command handling | Plausible incrementally | Existing renderer ownership, callbacks, generated dispatch, and build integration |
| Vista deployment service | Possible with substantial platform work | Vista API imports, CRT, service recovery, signature APIs, and reboot state |
| Full D3D9 UMD | Difficult | DDI/COM ABI, shared resource lifetimes, shader converter C++, MinGW linking |
| Vista KMD | Highest risk | WDK 7.1 ABI, IRQL, allocation, kernel runtime, interrupt/DMA synchronization |
| QEMU, DXVK, or shader converter replacement | Poor initial scope | Large upstream projects with their own maintenance and integration requirements |

## Recommended experiment

Start with the arithmetic in `triton9_cpu_layout.c`, whose header already
avoids Windows types and has native contract tests. Implement a small
`#![no_std]` core with checked arithmetic and no allocator. Keep raw pointers
and exported ABI wrappers thin; do not treat foreign pointers as valid Rust
references until their validity and aliasing contract is established.

Use a C wrapper for the existing Windows callbacks and expose a small C ABI
from a Rust static library. Use explicit C-compatible layouts for shared
records and retain fixed-width integers in wire structures. Never allow Rust
unwinding to cross the C boundary. Rust's FFI guidance covers layout, calling
conventions, ownership, and unwinding constraints.
[Rust FFI guide](https://doc.rust-lang.org/nomicon/ffi.html)

`no_std` narrows runtime dependencies; it does not automatically make compiler
output Vista-compatible. Select and pin a toolchain only after checking its
generated helper calls, panic behavior, CPU features, and linkage. Keep the
existing C implementation selectable until the experiment passes:

1. Differential native tests against C for zero dimensions, padded rows,
   exact bounds, overflow, and failed operations leaving outputs unchanged.
2. x86 and x64 layout/calling-convention probes, including `size_t` width.
3. Linking into both current UMD builds without new unsupported imports,
   runtime libraries, or entry-point changes; pass the existing PE audits.
4. The signed package checks and Vista guest load, graphics, and reboot tests.

Only then consider resource or protocol ownership modules. Keep the KMD and
Windows DDI/COM boundary in C/C++ during the first stages. These are engineering
recommendations inferred from the inspected interfaces, not demonstrated Rust
compatibility results.
