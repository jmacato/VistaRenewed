# Host build and test baseline

This report covers only host-side builds, non-VM tests, static audits, and
artifact inspection. It does not start, stop, drive, or modify a VM. It does
not establish guest rendering, D3D9 probe success, PresentEx success, Aero, or
glass.

## Pre-build capacity

- Measurement time: `2026-08-26T05:01:30+0800`.
- Free space before build: `df -k .` reported `80897152` KiB available on
  `/System/Volumes/Data` (the human-readable observation immediately before
  inventory was about `77 GiB`).
- Existing UMD footprints: Vista x86 `63604` KiB, Vista x64 `69908` KiB, and
  the Vista CRT cache `460980` KiB.
- Existing host footprints: virglrenderer `12216` KiB and QEMU `226472` KiB.
- Projected artifact growth: all selected builds reuse existing configured
  build directories. A conservative upper bound is `1 GiB`, larger than the
  combined `372200` KiB current build-directory footprint (excluding the
  already-populated `460980` KiB CRT cache). No VM artifact, package medium,
  dependency download, or new build tree is planned.

Exact capacity commands (exit `0`):

```sh
date '+%Y-%m-%dT%H:%M:%S%z'
uname -a
df -k . | tail -1
du -sk triton-umd/build-vista-x86-unified triton-umd/build-vista-x64-unified triton-umd/.cache triton-virglrenderer/build-arm64 triton-qemu/build
```

Decisive output:

```text
2026-08-26T05:01:30+0800
Darwin Mac.local 24.6.0 ... RELEASE_ARM64_T6041 arm64
/dev/disk3s5 971350180 852416028 80897152 92% ... /System/Volumes/Data
63604  triton-umd/build-vista-x86-unified
69908  triton-umd/build-vista-x64-unified
460980 triton-umd/.cache
12216  triton-virglrenderer/build-arm64
226472 triton-qemu/build
```

## Build/test inventory

The repository exposes the following directly relevant entry points.

| Surface | Entry point | Selection | Reason |
|---|---|---|---|
| Vista x86 D3D9 UMD | `triton-umd/build-vista-x86-unified/build.ninja` | Selected | This is the configured x86 `npt_vista_d3d9` cross-build and produces the x86 package UMD. |
| Vista x64 D3D9 UMD | `triton-umd/build-vista-x64-unified/build.ninja` | Selected | This is the configured x64 `npt_vista_d3d9` cross-build and produces the x64 package UMD. |
| Host renderer | `triton-virglrenderer/build-arm64/build.ninja` | Selected | QEMU's Triton display path depends on the modified Neptune virglrenderer backend. |
| Host QEMU | `triton-qemu/build/build.ninja` | Selected | The modified `virtio-gpu` device contracts compile into this host binary. No QEMU process will start. |
| Vista KMD/package | `triton-kmd/viogpu/viogpu_vista.sln` and `BUILDING_VISTA.md` | Selected as an expected-failure prerequisite baseline | The build requires a Vista-compatible WDK, matching `virtiolib.lib`, staged UMDs, `Inf2Cat`, and a private signing certificate. The two `msbuild` commands expose the missing-host-tool failure. No package-build pass is claimed. |
| Vista package audits | `triton-kmd/viogpu/tools/check_vista_*.py` | Selected where inputs exist | These are repository-authored source, PE, and INF static gates and do not require a guest. |
| Vista UMD audit | `triton-umd/build-support/audit-vista-d3d9-pe.sh` | Selected | This script inspects Vista PE imports and the subsystem for both architectures. It does not run the DLLs. |
| UMD contract tests | Meson tests in both Vista UMD build trees | Selected | They exercise the Triton D3D9 draw, CPU-layout, and shader-token contracts without executing a guest. |
| virglrenderer tests | Meson tests in `triton-virglrenderer/build-arm64` | Selected | This configured test target does not start QEMU. It reports no tests because `tests=false`. |
| QEMU unit tests | Meson `unit` suite in `triton-qemu/build` | Selected | These are host unit binaries. The display-adjacent `test-virtio-dmabuf` is also rerun alone. |
| QEMU qtests/functional tests | Meson qtest and functional suites | Excluded | They can launch QEMU system emulation, which is outside this leaf's no-VM contract. |
| Diff hygiene | `git diff --check` in each of the four modified Triton repositories | Selected | This is a portable static check for malformed whitespace in the pre-existing source changes. |

## Selected commands

Selection is based on direct inclusion in the Triton Vista display path and
permission to run without guest or VM state. The host is macOS arm64. Tool
versions were Meson `1.11.1`, Ninja `1.13.2`, Python `3.14.6`, and MinGW GCC
`15.2.0` for both `i686-w64-mingw32-gcc` and
`x86_64-w64-mingw32-gcc`.

The UMD build configuration has `platforms=[windows]`, `neptune=true`,
`min-windows-version=6`, and `buildtype=debugoptimized`. The renderer has
`neptune=true`, `tests=false`, and `buildtype=release`. QEMU has
`virglrenderer=enabled`, `cocoa=enabled`, and `tcg=enabled`. Its configure log
resolves virglrenderer `1.3.0` from `host-triton`.

## Build executions

### Vista x86 D3D9 UMD

- Build command: `meson compile -C triton-umd/build-vista-x86-unified`
- Exit code: `0`.
- Decisive output: `[4/4] Linking target src/virtio/neptune/vista-d3d9/neptune_d3d9.dll`.
- Result: passed. This command relinked the x86 DLL.

### Vista x64 D3D9 UMD

- Build command: `meson compile -C triton-umd/build-vista-x64-unified`
- Exit code: `0`.
- Decisive output: `[1/1] Generating src/git_sha1.h with a custom command`.
- Result: Ninja considered the existing x64 DLL current and did not relink it.
  Its independent PE audits and contract tests passed below.

### Neptune virglrenderer host library

- Build command: `meson compile -C triton-virglrenderer/build-arm64`
- Exit code: `1`.
- Decisive output:

  ```text
  FAILED: [code=1] src/libvirgl.a.p/neptune_npt_queue.c.o
  ../src/neptune/neptune-protocol/npt_protocol_common_types.h:13631:14:
  error: incompatible pointer to integer conversion assigning to 'HANDLE'
  (aka 'unsigned long long') from 'void *' [-Wint-conversion]
  *val = NULL;
  ninja: build stopped: subcommand failed.
  ```

  The same error occurred in multiple Neptune translation units. The host
  protocol defines `HANDLE` as `uint64_t`, while the shared decoder assigns
  `NULL` on its overflow path. The UMD and renderer copies of this header are
  byte-identical at SHA-256
  `fa12bf7f8dac7d78c85f58c1992d4e2b7823d5e4ab07a2be67381e4f6b85258f`.

### Triton QEMU host build

- Build command: `meson compile -C triton-qemu/build`
- Exit code: `0`.
- Decisive output: the build progressed through the complete regenerated
  graph and ended at `[1088/1088] Linking target tests/qtest/netdev-socket`.
- Result: passed as an incremental build. It compiled and linked host tools
  and test binaries but did not update the already-current
  `qemu-system-x86_64` timestamp.
- Build command: `meson compile -C triton-qemu/build qemu-system-x86_64`
- Exit code: `0`.
- Decisive output: `[1/6] Generating qemu-version.h with a custom command`.
- Result: the targeted dependency check passed and did not relink the binary.
  `otool -L` proves that binary resolves
  `/Users/jumar/winvistachecked/host-triton/lib/libvirglrenderer.1.dylib`, not
  the renderer build-tree output that failed above.

### Vista x86 and x64 KMD/package

Working directory for both commands was `triton-kmd/viogpu`.

- Build command: `msbuild viogpu_vista.sln /m '/p:Configuration=Vista x86' /p:Platform=Win32`
- Exit code: `127`.
- Decisive output: `zsh:1: command not found: msbuild`.
- Build command: `msbuild viogpu_vista.sln /m '/p:Configuration=Vista x64' /p:Platform=x64`
- Exit code: `127`.
- Decisive output: `zsh:1: command not found: msbuild`.
- Result: neither KMD nor package was built. `command -v msbuild` exited `1`,
  and `env | rg '^VISTA_(WDK|VIRTIOLIB|D3D9)'` exited `1` with no matching
  prerequisite variables. The documented build requires a Windows host with
  the Vista WDK and matching `virtiolib.lib`.

## Non-VM tests and static checks

### UMD contract suites

- Test command: `meson test -C triton-umd/build-vista-x86-unified --no-rebuild --print-errorlogs`
- Test exit code: `0`.
- Test result: `Ok: 3`, `Fail: 0`, `Skipped: 1`. The three Triton contract
  tests passed. The zlib example skipped with status `77`.
- Test command: `meson test -C triton-umd/build-vista-x64-unified --no-rebuild --print-errorlogs`
- Test exit code: `0`.
- Test result: `Ok: 3`, `Fail: 0`, `Skipped: 1`, with the same three Triton
  contract tests passing and the zlib example skipped.

### Vista UMD PE audits

- Test command: `sh triton-umd/build-support/audit-vista-d3d9-pe.sh x86 triton-umd/build-vista-x86-unified/src/virtio/neptune/vista-d3d9/neptune_d3d9.dll`
- Test exit code: `0`.
- Test result: `Vista D3D9 PE audit passed: x86 ...neptune_d3d9.dll`.
- Test command: `sh triton-umd/build-support/audit-vista-d3d9-pe.sh x64 triton-umd/build-vista-x64-unified/src/virtio/neptune/vista-d3d9/neptune_d3d9.dll`
- Test exit code: `0`.
- Test result: `Vista D3D9 PE audit passed: x64 ...neptune_d3d9.dll`.
- Test command: `python3 triton-kmd/viogpu/tools/check_vista_pe.py --kind umd --arch x86 triton-umd/build-vista-x86-unified/src/virtio/neptune/vista-d3d9/neptune_d3d9.dll`
- Test exit code: `0`.
- Test result: `Vista PE audit passed (umd, x86)`.
- Test command: `python3 triton-kmd/viogpu/tools/check_vista_pe.py --kind umd --arch x64 triton-umd/build-vista-x64-unified/src/virtio/neptune/vista-d3d9/neptune_d3d9.dll`
- Test exit code: `0`.
- Test result: `Vista PE audit passed (umd, x64)`.

Both artifacts are Windows GUI PE files with subsystem version `6.0`. They
import only `GDI32.dll`, `KERNEL32.dll`, `msvcrt.dll`, and `USER32.dll`.
Both auditors also require the `OpenAdapter` export.

### KMD source and INF audits

- Test command: `python3 triton-kmd/viogpu/tools/check_vista_kmd_source.py`
- Test exit code: `0`.
- Test result: `/Users/jumar/winvistachecked/triton-kmd/viogpu: Vista KMD source audit passed`.
- Test command: `python3 triton-kmd/viogpu/tools/check_vista_inf.py --arch x86 triton-kmd/viogpu/viogpu3d/viogpu3d_vista_x86.inx`
- Test exit code: `0`.
- Test result: `Vista INF audit passed (x86)`.
- Test command: `python3 triton-kmd/viogpu/tools/check_vista_inf.py --arch x64 triton-kmd/viogpu/viogpu3d/viogpu3d_vista_x64.inx`
- Test exit code: `1`.
- Test result: failed all seven deployment-package requirements reported by
  the checker:

  ```text
  SourceDisksFiles does not match required files: neptune_d3d9.dll,
    neptune_d3d9_wow.dll, triton-vista-deploy.exe, viogpu3d.sys
  user-mode destination directories do not match the Vista architecture profile
  deployment-service copy section is invalid
  deployment-service copy does not use the PnP-stop flag
  deployment service is not installed by the display package
  deployment service does not have the Vista auto-start contract
  deployment service is not registered in both SafeBoot lists
  ```

The parent audit also checked the active x64 packaging input with
`python3 triton-kmd/viogpu/tools/check_vista_inf.py --arch x64 test-artifacts/vista-driver-x64-kd-serialtrace/viogpu3d-diagnostic.inf`.
That command exited zero with `Vista INF audit passed (x64)`. Therefore, the
seven failures above apply only to the inactive standard x64 template. They
do not block the active serial-trace package route.

### Renderer test registration

- Test command: `meson test -C triton-virglrenderer/build-arm64 --no-rebuild --print-errorlogs`
- Test exit code: `0`.
- Test result: `No tests defined.` This is consistent with the configured
  `tests=false` option and is a coverage gap, not a test pass.

### QEMU host unit suite

- Test command: `meson test -C triton-qemu/build --no-rebuild --suite unit --print-errorlogs`
- Test exit code: `1`.
- Test result: `Ok: 101`, `Fail: 1`, `Skipped: 1` out of `103` tests. The
  failure was `unit - qemu:test-error-report`, killed by `SIGABRT`, because
  `/error-report/glog` stderr did not match
  `test-error-report: info: gmessage*`.
- Test command: `meson test -C triton-qemu/build --no-rebuild --print-errorlogs 'qemu:test-error-report'`
- Test exit code: `1`.
- Test result: the same `/error-report/glog` mismatch reproduced in `0.03s`.
  The result was `Ok: 0`, `Fail: 1`.
- Test command: `meson test -C triton-qemu/build --no-rebuild --print-errorlogs 'qemu:test-virtio-dmabuf'`
- Test exit code: `0`.
- Test result: `5 subtests passed`. The result was `Ok: 1`, `Fail: 0`.

No qtest or functional suite was executed, because those suites can launch a
QEMU system process. Two initial isolated-rerun filters incorrectly used the
human display labels `unit - qemu:test-error-report` and
`unit - qemu:test-virtio-dmabuf`. Both exited `1` with `test name does not
match any test`. The canonical `qemu:...` filters above corrected the operator
error and produced the decisive results. This invocation failure is not
silently treated as a product failure or pass.

### Diff hygiene

The following Test commands each exited `0` with no output:

```sh
git -C triton-kmd diff --check
git -C triton-umd diff --check
git -C triton-virglrenderer diff --check
git -C triton-qemu diff --check
```

Test exit code: `0` for all four. Test result: no whitespace errors in the
pre-existing tracked diffs.

## Artifact inventory

All timestamps below are local time (`+0800`). SHA-256 values came from
`shasum -a 256`. Sizes and timestamps came from `stat`.

| Artifact identity | SHA-256 | Timestamp | Size | Signature |
|---|---|---:|---:|---|
| `triton-umd/build-vista-x86-unified/src/virtio/neptune/vista-d3d9/neptune_d3d9.dll` | `1f83e22ec188ac38002b300b156d60620ea31ed35e799a01159305b24ec2b2d2` | `2026-08-26T05:02:40+0800` | `19,123,386` bytes | PE Security Directory is `0,0`: no embedded Authenticode signature. |
| `triton-umd/build-vista-x64-unified/src/virtio/neptune/vista-d3d9/neptune_d3d9.dll` | `2047dba5f0d5e4a898d3f2d0875cacf4d5f5f3dbd44f9b2c51d19780b4bbdc81` | `2026-08-22T03:11:27+0800` | `20,955,198` bytes | PE Security Directory is `0,0`: no embedded Authenticode signature. |
| `triton-virglrenderer/build-arm64/src/libvirglrenderer.1.dylib` (stale because the current rebuild failed) | `fde6a2034ce9311a2a15b9f47dde3864df292a178fb65953cd8b60d342ebe396` | `2026-08-10T16:51:57+0800` | `1,654,152` bytes | `codesign --verify --verbose=4` passed. Ad-hoc linker signature with CDHash `ede8776f0b36b4d8041c71a12066838df39ce5d0`. |
| `host-triton/lib/libvirglrenderer.1.dylib` (the distinct installed copy that QEMU links) | `3682df45f931e3075e2ce1063ebf299d1d2a6ac3262cbe9b90263692e2089822` | `2026-08-10T16:51:58+0800` | `1,654,152` bytes | `codesign --verify --verbose=4` passed. Ad-hoc linker signature with CDHash `91bb039cb4a7845bca7b3bbe036c2fc98ff984db`. |
| `triton-qemu/build/qemu-system-x86_64` | `3194a666ab1d7310b41d4519e1682899cd9b1d8a7b5f3ecd221928f415f489aa` | `2026-08-16T03:30:11+0800` | `27,385,224` bytes | `codesign --verify --verbose=4` passed. Ad-hoc linker signature with CDHash `1fe13a86ffd3239a33c6f88dc853fa93eb5eefb2`. |

The UMDs are normally covered by the driver package catalog rather than by
individual embedded signatures. No current catalog exists here, so the zero
PE certificate tables do not establish package trust. These required package
artifacts were all missing:

```text
triton-kmd/viogpu/Install/Vista/x86/viogpu3d-vista-x86.cat
triton-kmd/viogpu/Install/Vista/amd64/viogpu3d-vista-x64.cat
triton-kmd/viogpu/objfre_vista_x86/i386/viogpu3d.sys
triton-kmd/viogpu/objfre_vista_amd64/amd64/viogpu3d.sys
```

Therefore no Vista KMD/catalog Signature can be inspected and no signed display
package was produced. `osslsigncode` is missing. The executable named
`/opt/homebrew/bin/signtool` is a Mach-O NSS utility, not Microsoft SignTool.
Its `--help` probe exited `255` with `SEC_ERROR_BAD_DATABASE`. Thus, this tool
does not give evidence for Windows catalog signing.

The source provenance includes pre-existing changes. Thus, commit IDs alone
cannot identify the tested source:

| Repository | HEAD | Pre-existing tracked modifications | Diff SHA-256 before and after |
|---|---|---:|---|
| `triton-kmd` | `74cb98d15f6cb9ca44d9b6ecc6e47a2236eeb2d3` | 32 | `ba69a34427252ceecf5dde2e96aa2b357333f4d5e857203f6e828cf27670c0b0` |
| `triton-umd` | `7432d34c2bc10c602d72b1ad4058cde98549f98c` | 15 | `6fa8038abadc46ffa8978d1cbab8520c57c2a015d48eb9dded00c0b9680cd626` |
| `triton-virglrenderer` | `65cc14eb896f121ffc5130ce04815a923a03c41d` | 1 | `13d30914d741c4ecda8d4ed671b9fa3fc1a7f5003a86563e471a82e2f2e2d6de` |
| `triton-qemu` | `7311c3651c3a2cbc3d32e6eae262c60339f28d79` | 4 | `841687ff4297d2d3e24fc85753af7a5cebd4fbf9c5aac3b11e180d056ff36d33` |

The identical before/after diff hashes and modification counts are the basis
for the no-source-change statement below.

## Second review

The expert reread separated an incremental QEMU success from the failed
renderer that QEMU actually consumes. It also rejected the tempting but false
conclusion that passing PE audits imply a signed or deployable package.

Build/test blockers:

1. Current Neptune virglrenderer source does not compile. The shared decoder
   assigns pointer constant `NULL` to integer `HANDLE` at
   `npt_protocol_common_types.h:13631`, and host Clang treats it as an error.
2. QEMU links the older, separately installed `host-triton` renderer. Its
   successful incremental build therefore does not prove that the current
   renderer source compiles or integrates into the QEMU binary.
3. The inactive standard x64 Vista INF template fails seven
   deployment-service/package invariants. The active serial-trace diagnostic
   INF passes the x64 audit. Future standard-route use would require a fix,
   but this mismatch does not block the active package route.
4. This macOS host lacks `msbuild`, a Vista-compatible WDK environment,
   matching `virtiolib.lib`, `Inf2Cat`, and the private catalog-signing
   context. The x86/x64 KMDs and catalogs are absent. Thus, this host cannot
   reproduce or inspect a signed driver package.
5. QEMU's non-VM unit baseline has one reproducible failure:
   `qemu:test-error-report` aborts on a GLib stderr-pattern mismatch. The
   display-adjacent `qemu:test-virtio-dmabuf` passes, but the suite is not
   wholly green.
6. The configured renderer has `tests=false` and reports `No tests defined`,
   so there is no renderer unit-test result to offset its build failure.
7. QEMU qtests and functional tests were intentionally not run because they
   can start QEMU system emulation, which this leaf is prohibited from doing.
   This is an explicit coverage boundary, not a pass.

Source files modified: none by this leaf. Build outputs and Meson test logs
changed normally. The four tracked-diff hashes and modification counts were
identical before and after all build/test commands.

At `2026-08-26T05:07:33+0800`, final free space was `80242012` KiB. The run
consumed `655140` KiB of available space, within the pre-recorded `1 GiB`
projection. No VM artifact, disk derivative, deployment medium, or framebuffer
capture was created.

This baseline makes no claim of guest rendering, successful clear/readback,
triangle/readback, PresentEx, Aero, or glass.
