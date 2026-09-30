# Supermium-based CEF: Vista control bring-up

> Development record retained with the public desktop sources. Local capture,
> VM and `.unlazy/` paths refer to non-shipped development artifacts. See
> [current public entry points and status](DESKTOP-MODULES.md).


## Status

Source preparation, all CEF patches, dependency hooks, Linux MSVC/SDK smoke
tests and GN generation have completed. The first `libcef`/`cefclient` compilation
has started. No new `libcef.dll` has been verified or deployed.
The installed MSHTML adapter and Vista VM are unchanged by this experiment.
The first runtime milestone is an offscreen CEF control on Vista, with input,
resize and clean shutdown, and no independent browser window at any point.
Only after that milestone should the MSHTML backend be replaced.

## Exact source pair

| Component | Repository | Commit |
| --- | --- | --- |
| Supermium v144 | https://github.com/win32ss/supermium | `82756ad44eee3166e8ac4fa7605632690ac70bc9` |
| CEF 7559 | https://github.com/chromiumembedded/cef | `0b1a01255cca9c6a000582ed81afcb3b67b2043d` |

Both identify Chromium `144.0.7559.256`. This matches version declarations,
not a claim that the two source trees are otherwise identical. CEF branch tip
at inspection expected `.262`; using that tip would introduce avoidable drift.

Checkouts are `third_party/supermium-cef/src` (full working tree) and
`third_party/supermium-cef/src/cef` (detached at the pin). The sibling `cef`
path is a symlink for audit-script compatibility. CEF must physically live
inside `src` because its tools resolve real paths. Dependencies have been
synchronized; revisions are recorded in `third_party/supermium-cef/sync-result.json`.
Do not run CEF automation with defaults that replace the
Supermium checkout with upstream Chromium.

Re-run source verification and the non-mutating patch audit:

```sh
python3 scripts/supermium_cef_source.py verify
python3 scripts/supermium_cef_source.py prefetch
python3 scripts/supermium_cef_source.py audit
python3 -m unittest discover -s tests -p test_supermium_cef_source.py
```

The audit applies patches in declared order into a disposable Git index,
using CEF's zero-component path stripping. It never modifies the source
working tree or its normal index. A conflicting patch is atomic and remains
unapplied, so later conflicts can be dependent failures. Dependency patches
are reported as untested. The audit exits nonzero if either category exists;
it is not a build-success check. Its generated report is
`build/supermium-cef-patch-audit.json`.

The initial correct-path audit found 101 applying patches, six conflicts:

- `views_1749_2102_3330`: `ui/gfx/render_text.h`
- `chrome_runtime`: `chrome/browser/chrome_browser_main.cc`
- `chrome_runtime_views`: `chrome/browser/ui/views/toolbar/toolbar_view.cc`
- `embedder_product_override`: `components/embedder_support/user_agent_utils.cc`
- `mac_chrome_locale_3623`: `chrome/browser/chrome_resource_bundle_helper.cc`
- `linux_gtk_theme_3610`: `ui/gtk/gtk_ui.cc`

The six conflicts were context-merged in
`scripts/supermium_cef_patch_context.json`, preserving Supermium-specific
changes. The merged audit applies all 107 root patches. Root patches and
dependency patches `v8_build`, `tarball_gclient`, and `angle_commit_config`
have now been checked and applied. Do not infer that a platform-labelled conflict can be
ignored: shared files and the chosen build configuration must be inspected.
The `viz_osr_2575` and `osr_win_remove_keyed_mutex_2575` patches apply, but have
not been compiled or exercised.

## Local build toolchain

The existing `vista-driver-builder` supplies MinGW and legacy driver/signing
tools. Microsoft components have now been downloaded into the isolated
`third_party/supermium-cef/toolchain` directory after the user's explicit
license acceptance. No Windows build machine was required.

Installed MSVC headers/libraries: `14.43.34808`; Windows SDK layout:
`10.0.26100.0`. The selected SDK package is `10.0.26100.15`, not proof of the
exact servicing level mentioned by Supermium's toolchain comments.

Downloader: `mstorsjo/msvc-wine` commit
`514f8ea34842cd6d831804d0e9658d3a32870ae1`.
Saved Microsoft manifest: `third_party/supermium-cef/17.14.40.manifest`, SHA256
`b60efac8768e31b4b0bb74d312a3fd3145de9bef7f794c050d498d079e540e11`.
The downloader validates package hashes from that manifest. Installation used
the existing builder container's `msiextract`; host extraction initially failed
because that utility was not installed on the host, then resumed from the cache.
Only temporary extraction files were removed by the installer; the downloaded
package cache remains available.

Repeat locally (installation requires explicit license consent):

```sh
python3 scripts/supermium_cef_toolchain.py install --accept-license
python3 scripts/supermium_cef_toolchain.py verify
python3 -m unittest discover -s tests -p 'test_supermium_cef*.py'
```

Host Clang/LLD 19.1.7 compiled and linked x86 and x64 C++ executables using
Windows API and STL headers. PE machine types were checked. A Clang VFS overlay
and build-local lowercase library symlinks preserve original package contents.
The pinned Supermium `setup_toolchain.py` also successfully consumed generated
`SetEnv.x86.json` and `SetEnv.x64.json` files for both architectures. Transcripts
and artifact hashes are in `build/supermium-cef-toolchain/`.

The smoke tests alone are not a CEF build or a Vista runtime test.
Chromium's pinned Clang/Rust, dependencies and Linux host sysroot are now
prepared. GN generated 29,724 targets successfully. Full SDK tool execution
and compiler integration still need validation through compilation. The
local SDK consumes about 3.5 GiB plus 1.1 GiB retained download cache.

The pinned Supermium `build/vs_toolchain.py` describes Visual Studio 2022
17.13.4 and Windows SDK 10.0.26100.4654. This is the source's stated toolchain,
not evidence that the published Supermium release used exactly that setup.
Its checked-in Windows compiler settings include an NT 5.0 subsystem version;
that alone does not establish runtime compatibility for CEF's entry points.

The pinned `docs/win_cross.md` supports Linux-to-Windows cross-compilation
using a Microsoft toolchain packaged from a Windows installation. Thus a
separate Windows build VM is optional. The locally downloaded components now
provide a candidate toolchain without one; retain Vista as the execution target.

At initial inspection this filesystem had about 208 GiB available; immediately
before compilation it had about 167 GiB. The build wrapper stops compilation
if available space falls below 15 GiB or Linux reports less than 2 GiB available
memory. Microsoft components are workspace-local. The ongoing build was raised
from six to twelve jobs after measuring roughly 20 GiB available host memory.

## Build stages

```sh
python3 scripts/supermium_cef_source.py audit-merged
python3 scripts/supermium_cef_build.py patch
python3 scripts/supermium_cef_build.py hooks
python3 third_party/supermium-cef/src/build/linux/sysroot_scripts/install-sysroot.py --arch=amd64
python3 scripts/supermium_cef_build.py configure
python3 scripts/supermium_cef_build.py build --jobs 6
```

The root audit still exits nonzero for dependency patches it does not test;
the `patch` stage separately checks those against the synchronized dependencies.
The `.gclient` sets `source_tarball=False`, required by the CEF DEPS patch,
and checks out both Windows and Linux dependencies: Windows-only checkout
omits Fontconfig sources needed by Linux host-side generators.
Build log: `build/supermium-cef-compile.log`. Output directory:
`third_party/supermium-cef/src/out/Release_CEF_Vista_x64`.

Cross-build patches provide case-insensitive SDK header/library lookup and
disable Windows-only CDM host verification in the Linux host-tool toolchain.
Resource preprocessing receives the same SDK overlay through
`scripts/supermium_cef_rc.patch`. LLD uses a separate case-insensitive library
overlay; the toolchain smoke test explicitly links mixed-case `Cfgmgr32.lib`
with debug information and linker warnings as errors. The local library view
includes ATL libraries and companion CRT/ATL PDBs, required for debug linking.
Clang explicitly selects MSVC `14.43.34808`; automatic newest-version selection
otherwise picks an incomplete optional-tool directory installed by the manifest.
`scripts/supermium_cef_compile.patch` removes an unreachable return after
`NOTREACHED()` and repairs the allocator's fixed-size pool function guard for
Linux host compilation. The Windows dynamic legacy-OS pool sizing is unchanged.
The same patch declares the generated buildflag header dependencies required
by Supermium's custom vector-icon loading through `chrome_paths.h`.
The build selects `libcef` and `cefclient` directly; CEF's umbrella `cef` target
also builds unit tests and additional samples not needed for initial bring-up.
The installer also fetches Microsoft's manifest-pinned
`Microsoft.VisualStudio.Debugger.DbgHelp.Win8` package; configure stages its
x64 DLLs under the SDK debugger path. These debugger DLLs and the selected
14.42 CRT redistributables have **not** been certified for Vista deployment.
The configure stage regenerates LASTCHANGE and GPU list metadata with an
empty commit-message filter, recording the real Supermium commit even though
it lacks Chromium's Change-Id footer. No upstream commit position is invented.

## Remaining implementation order

1. Establish and pin the actual Windows toolchain and dependency revisions,
   including Supermium's compatibility runtime requirements.
2. Resolve the six source conflicts preserving both CEF integration and
   Supermium compatibility changes; apply and verify dependency patches.
3. Build `libcef` and a minimal native test host. Audit its imports and CEF
   subprocess startup rather than relying only on the PE subsystem version.
4. On Vista, create a windowless browser via `SetAsWindowless`; paint `OnPaint`
   BGRA buffers in the host-owned view. Verify input, resize and shutdown.
   Validate the no-standalone-window monitor against a deliberately visible
   positive control before accepting the startup test.
5. Add cross-process shared buffers with explicit ownership, generation IDs,
   dimensions/stride validation and resize acknowledgement. Keep pixels out
   of the command pipe and preserve process sandbox boundaries.
6. Connect this runtime to the existing MSHTML COM/OLE implementation, keeping
   native activation, travel logs, resource URLs and popup routing in scope.
   ActiveX remains out of scope. Re-run the existing consumer tests plus
   startup visibility tests before durable deployment.

CEF embedding replaces the current external Chrome-window lifecycle. It does
not by itself provide MSHTML COM compatibility, native security UI, or an
accessibility bridge for arbitrary MSHTML consumers.
