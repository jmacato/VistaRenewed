# Vista Triton handoff — 2026-09-07

## Destination update — Linux/KVM, September 7

The newly supplied `winvista-3.qcow2` supersedes the fresh-install instructions
below. It is a standalone 40 GiB image and passed `qemu-img check` with zero
check errors. Keep it as the unchanged backing image.

Read-only inspection of its SOFTWARE hive identifies **Windows Vista Ultimate
SP2 x64 checked**, build `6002.18005.amd64chk.lh_sp2rtm.090410-1830`, with
`CurrentType=Multiprocessor Checked`. The installed `ntoskrnl.exe`, `hal.dll`,
and `kernel32.dll` all report version `6.0.6002.18005` and have `VS_FF_DEBUG`
set. Exact hashes and metadata are in `vista-kvm/base-build-evidence.json`.
This is different from the RTM installation attempted on the original host.

`scripts/run_vista_kvm.sh` now boots `vista-kvm/work.qcow2`, a copy-on-write
overlay of the supplied base, using system QEMU, KVM, one Core 2 Duo vCPU
with MONITOR disabled, IDE storage, standard VGA, and no network. KVM was
confirmed enabled through QMP. Run records and local QMP/VNC sockets are
under `vista-kvm/latest`. No driver media is attached.

The guest passed the unclean-shutdown countdown and reached **Set Up Windows:
Choose a user name and picture**. OOBE and first desktop remain incomplete;
no clean-install snapshot or graphics success is claimed. Complete OOBE and
shut down cleanly, then preserve a clean checkpoint before driver changes.

The source transfer verifier reports 89 mismatches: 76 symlink entries and
13 missing files. The full list is `handoff/destination-transfer-failures.json`.
No source exports or manifest entries were repaired or overwritten.
The Linux graphics backend, custom QEMU, and both Vista UMDs have since been
built and tested; see `notes/LINUX_DRIVER_RESUME.md` for the implementation,
validation, and reproducible commands. The Windows KMD build/signing route
and guest bootstrap still need resolution before Triton deployment. Seven
QEMU subproject symlinks from the original mismatch list have been repaired.

## Current state

The user requested a source transfer to an x86 machine. The Vista QEMU process, PID 22660, stopped with SIGTERM on September 7. This was a host stop, not a completed guest shutdown.

Vista installation was incomplete. The last inspected screen showed **Installing features**. Copying and expanding files were complete. No first-logon or completion checkpoint appeared. CPU usage and disk growth did not establish success.

**No clean-install snapshot exists.** The user requested a snapshot immediately after a successful installation, before drivers or other changes. That requirement still applies on the destination machine.

The incomplete QCOW2, Vista DVD, SDK downloads, compiled outputs, and Git object databases are now in macOS Trash. Nothing in this transfer is a bootable installed Vista disk.

## Transfer contents

The source directories contain the local edits, including untracked source files. They are source exports, not Git checkouts. The seven Triton repositories and their available submodule source trees remain.

`handoff/repositories.json` records repository URLs, revisions, branches, status, and submodule revisions before cleanup. The adjacent patches record tracked changes against HEAD. The source directories also contain untracked files that patches do not include.

**Do not apply these patches over the transferred source.** Those edits are already present. Patches support reconstruction in separate upstream checkouts.

`handoff/removed-files.json` records the final removal batch and the original paths. `handoff/cleanup-result.json` records the Trash location. Earlier cleanup batches also remain in macOS Trash on the original Mac. Trash contents do not travel with this folder.

The cleanup preserved all 579 files from the current build-source manifest without changes. Some large upstream data files were removed. Their paths appear in the removal record. Files of at least 10 MiB were classified as large. Smaller downloads and generated output directories were also removed.

## First steps on the x86 machine

1. Copy this entire folder, including `handoff/` and `recovery-vista/`.
2. Run `python3 handoff/verify-transfer.py` to check the transferred files.
3. Select the destination operating system and its supported hardware virtualization accelerator.
4. Obtain the Vista installation DVD again.
5. Create a new QCOW2 disk and complete a fresh installation.
6. Inspect the guest desktop and installation result before declaring success.
7. Shut down Vista, then create and inspect a clean-install snapshot before any driver changes.
8. Rebuild the required host backend and Vista driver packages.
9. Run the public D3D9 probe before any Aero claim.

The original host used Apple Silicon, QEMU TCG, and one virtual CPU. That combination made checked Vista installation very slow. Hardware virtualization on x86 is the intended next step. No destination accelerator or destination build was tested here.

## Host backend constraint

The current rendering path uses `triton-dxmt`, a Direct3D-to-Metal backend. It requires macOS and Metal. Moving to an x86 Windows or Linux host does not make that backend portable.

An x86 host can accelerate Vista installation independently of the Triton renderer. For full graphics work, the next owner must select a compatible host backend. Intel macOS still requires a fresh build and runtime checks. Windows or Linux requires a backend port or replacement plan.

The old host binaries were ARM macOS builds. They were removed. Existing launch scripts also use Cocoa, macOS paths, zsh, and host libraries. They are references, not ready-to-run destination launchers.

## Source map

| Path | Purpose |
|---|---|
| `triton-kmd/viogpu/viogpu3d/` | Vista display miniport |
| `triton-umd/src/virtio/neptune/vista-d3d9/` | Public D3D9 driver, contracts, shader conversion, and probe |
| `triton-umd/src/virtio/neptune/` | Guest transport, resources, shared textures, and synchronization |
| `triton-qemu/hw/display/` | Virtio GPU device and host presentation |
| `triton-virglrenderer/src/neptune/` | Host Neptune dispatch and shared resources |
| `triton-dxmt/` | Native Metal backend and clear implementation |
| `triton-angle/`, `triton-libepoxy/` | Host graphics dependencies |
| `scripts/` | Build, package, deployment, audit, and pixel tools |
| `test-artifacts/` | Retained build inputs, service source, and manifests |
| `notes/unlazy/` | Earlier implementation reports and known limitations |
| `recovery-vista/` | Setup answer file, diagnostic records, and original diagnostic modules |

`triton-kmd/build/` contains build source files. It is not a disposable output directory.

## Rebuild prerequisites

The host build uses Meson, Ninja, native compilers, and the dependencies of each project. DXMT also requires the Apple and LLVM tools described in `triton-dxmt/docs/DEVELOPMENT.md`.

The Vista UMD uses MinGW cross-compilers and an MSVCRT-compatible runtime. The profiles are `triton-umd/build-support/vista-x64.ini` and `vista-x86.ini`. `bootstrap-vista-crt.sh` retrieves the pinned CRT packages. The profiles reference SDK and WDK headers under `driver/sdk/`; those downloaded files were removed.

The previous miniport build used Visual Studio 2022 Build Tools and WDK 7.1 in a Tiny11 VM. See `notes/unlazy/signing-build-route.md` for the exact historical tools and paths. The Windows batch files remain under `test-artifacts/`.

`scripts/build_deploy_vista_driver.sh` previously drove that VM through Parallels. The `Y:` share, Tiny11 name, SDK paths, and macOS commands need destination-specific changes. No transport cleanup changed those scripts.

The previous signing key was non-exportable and remained inside the old Tiny11 VM. This folder contains its public certificate, not that private key. A new signing identity requires corresponding changes to certificate pins and package authentication.

Generated package directories now contain only residual inputs and metadata. They are not valid deployable packages. The last publication ID and configuration remain in `handoff/previous-deployment/` for reference.

## Proven setup failures and working retry

The DVD was `en_windows_vista_x64_check_dvd_x13-31669.iso`. Its image identified itself as x64 checked Vista RTM, build `6000.16386`. It was an unstaged image.

The two-CPU TCG boot stopped with `0x7E` and breakpoint exception `0x80000003`. The original kernel called `DbgBreakPoint` because `MTRR_MSR_DEFAULT` differed between processors. `KiInitializeMTRR` starts at RVA `0xb92000`; its breakpoint call is at RVA `0xb92266`. See `recovery-vista/crash-2cpu-evidence.json`.

The one-CPU Core 2 Duo boot then stopped with assertion exception `0xc0000420`. `intelppm.sys` read a zero size from CPUID leaf 5. It called `MmAllocateContiguousMemorySpecifyCache` with that size. The driver call is at RVA `0x2371`; the kernel assertion is at RVA `0x91131`. See `recovery-vista/crash-1cpu-evidence.json`.

The configuration `-smp 1 -cpu core2duo,monitor=off` passed those two failures under TCG. This is historical evidence, not a hardware-virtualization CPU prescription. Original kernel and Setup modules remain under `recovery-vista/original-*`.

Setup then rejected the WinPE bootstrap settings. Removing the optional COM2 `RunSynchronous` diagnostic command allowed installation to start. `recovery-vista/setup-media/Autounattend.xml` contains that revision. `answer-v2.iso` contains the same answer file.

The answer file wipes disk 0. Attach only a new disposable guest disk for unattended installation. It creates the `VistaTest` administrator account without a password and requests one automatic logon. The historical VM had networking disabled.

The remaining specialize and first-logon commands write to COM2. Those phases were not reached or tested. Review those diagnostic commands before reuse. `setup-command-v2.json` records the old absolute paths and CPU configuration only.

## Driver acceptance and next investigation

The earlier implementation reports cover D3D9 admission, clear semantics, shader translation, resource ownership, fences, and presentation. They report local build and regression results from August 26. This transfer did not rerun those builds or tests.

The older Vista disk disappeared before this reinstall. The fresh disk never reached a completed installation or Triton deployment. There is no current guest proof of clear/readback, triangle/readback, PresentEx, or genuine Aero glass.

The next graphics milestone is a guest-owned public D3D9 probe. Clear/readback, triangle/readback, and PresentEx must pass with decisive values. Aero pixel evidence and a repeat after reboot come later.

A fresh guest lacks the automatic `TritonVistaDeploy` service. Its bootstrap must precede the guest-owned deployment cycle. See `test-artifacts/VISTA_GUEST_DEPLOY.md` and the service source. The old Safe Mode blocker report describes the missing previous disk, not this fresh installation.

## Working constraints

Preserve the source changes. Inspect original binaries or disassembly before reproducing native behavior. Record the module, function, address, and call path for each native claim.

Correct Triton driver contracts instead of patching Windows graphics binaries. Keep guest deployment and recovery under guest control. Do not restore the removed host-input automation as the deployment route.

Do not create raw disk derivatives or raw framebuffer captures. Do not create or register launchd jobs. After installation succeeds, preserve the clean-install snapshot before further changes.
