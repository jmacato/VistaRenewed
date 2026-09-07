# Linux driver continuation — September 7, 2026

The supplied `winvista-3.qcow2` remains the base. Its working overlay is
`vista-kvm/work.qcow2`; the baseline VM reached account creation in OOBE.
The base contains Vista Ultimate SP2 x64 checked, build 6002.18005.
No driver was installed, and no completed-install snapshot has been claimed.

## Completed implementation

Added `triton-dxvk` from [osy/dxvk](https://github.com/osy/dxvk), revision
`404240fdacf47470b02c76d6e684639a95dc7387`, with its pinned submodules.
This fork provides native Linux D3D11, Headless WSI, and the shared-resource
descriptor extension that Neptune requires. The transferred Triton source
exports were preserved; no handoff patch was reapplied.

Implemented `dxvk_d3d11_clear_depth_stencil_rects` and connected it to the
existing Neptune wire-version-2 clear command. The export validates the whole
batch, device ownership, immediate-context type, flags, depth, writable
planes, and rectangle bounds. It copies up to 4,096 rectangles into owned
storage, preserving pixels outside the rectangles and every unselected plane.
Owned storage avoids exceeding DXVK's 16 KiB inline command chunk.

The operation drains the command-stream worker and waits for the GPU queue.
It propagates a failed idle wait or device status instead of reporting success.
The native extension contract is in `triton-dxvk/include/native/dxvk_neptune.h`.
Missing backend support continues to return `E_NOTIMPL` from Neptune.

Fixed DXVK's ordinary `ClearView` path to reject depth/stencil resources and
read-only depth views. Microsoft's [ClearView contract](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_1/nf-d3d11_1-id3d11devicecontext1-clearview)
admits depth-only resources for this operation. The private operation supplies
the separate depth/stencil selection needed by Vista's D3D9 UMD.

Fixed the renderer's default Linux library names to match DXVK's actual
outputs, a GCC C-label compile error, and its stale wire-version test.
Fixed QEMU's Linux SPICE build: missing local dimensions and an obsolete
cursor-blend argument. Restored seven QEMU subproject symlinks from the
transfer manifest after comparing any existing regular files with their targets.
The repair list is `handoff/destination-repaired-symlinks.json`.

Added Linux MinGW profiles for both Vista architectures. Older GCC versions
that lack `-mcrtdll` may use their MSVCRT default; a UCRT default is rejected.
Corrected `ShaderConv.hpp` to the actual `shaderconv.hpp` filename.
The Microsoft SDK/WDK NuGet packages are pinned by SHA-256 and only their
headers are extracted. They do not supply the missing Vista KMD libraries.
The public Vista D3D9 runtime probe source was not changed.

## Validation

The final hardware run used Intel Iris Xe (ADL GT2), Mesa 25.2.8 Vulkan:

- 234 native D3D11 checks passed, zero failed. These include partial D16 and
  D24S8 clears, depth/stencil preservation, malformed requests, a maximum-size
  batch, and an immediately complete event query after the synchronous call.
- A linear dma-buf texture was exported and reopened on a second D3D11
  device. Red pixels survived import; green pixels written by the consumer
  were read back by the producer. This was a same-process, two-device test,
  not the guest cross-process lifetime matrix.
- Software Vulkan passed 212 checks; dma-buf sharing was explicitly skipped.
- Four portable UMD contract suites passed with address/undefined-behavior
  sanitizers. Their Meson targets also passed.
- The Linux renderer built and its Neptune initialization test passed.
- Custom QEMU built and realized `virtio-vga-gl,neptune=true` with KVM and
  hardware EGL, paused with **no guest disk attached**. This does not prove
  rendering through the guest driver. QEMU warned that `/dev/udmabuf` is absent;
  guest blob behavior remains to be exercised.
- x64 and x86 UMD DLLs, both public probes, and the x64 deployment service
  compiled and passed their Vista PE audits. The probe's GDI import is allowed
  explicitly because its controlled evidence scene uses GDI. The service's
  recovery-specific import checks passed. The KMD source audit passed.

Evidence is under `test-artifacts/linux-build/`, especially
`final-hardware-tests.log`, `d3d11-runtime.log`, `qemu-smoke.json`,
`umd-rebuild.log`, `umd-native-contracts.log`, and `service-build.log`.
`handoff/linux-driver-build.json` records source and artifact hashes.

## Rebuild

The active build container is `vista-driver-builder`. A reusable local image
is `localhost/vista-driver-builder:20260907`. Sources are bind-mounted at
`/workspace`; installed host libraries are under `host-linux/`.
The container recipe is `scripts/linux-driver.Containerfile`.

To prepare a new container image and fetch the pinned SDK headers:

```sh
podman build -f "$PWD/scripts/linux-driver.Containerfile" -t localhost/vista-driver-builder:20260907 scripts
python3 scripts/fetch_vista_sdk_headers.py
```

Inside a build container with this directory mounted at `/workspace`, run:

```sh
export PATH=/opt/vista-build-tools/bin:$PATH
scripts/build_linux_graphics.sh
scripts/build_linux_qemu.sh
scripts/build_vista_umd_linux.sh
scripts/build_vista_service_linux.sh
```

Hardware tests from the workspace root:

```sh
podman run --rm --security-opt label=disable \
  --device /dev/dri/renderD128 --group-add keep-groups \
  -v "$PWD:/workspace" -w /workspace \
  -e MESON=/opt/vista-build-tools/bin/meson \
  -e DXVK_FILTER_DEVICE_NAME=Intel -e TRITON_TEST_SHARED=1 \
  -e TRITON_TEST_LABEL=hardware \
  localhost/vista-driver-builder:20260907 bash scripts/test_linux_graphics.sh
```

For QEMU's diskless smoke check, use the same mount/device options, add
`--device /dev/kvm`, and run `python3 scripts/check_linux_neptune_qemu.py`.

## Next deployment steps

The rebuilt Windows artifacts are **unsigned**, not a deployable package.
The old Windows build VM and its non-exportable signing key were not
transferred. A Windows build route with WDK 7.1 headers and Vista libraries
is still needed for the KMD. A new signing identity will need matching
certificate pins in the service and package; the current service still pins
the old identity. Do not sign unrelated bytes with an assumed old identity.

Complete Vista OOBE, shut down cleanly, and preserve the clean-install
checkpoint before driver changes. Bootstrap the deployment service inside
the guest, then deploy a matched KMD/UMD package through the guest-owned
route. Keep the original qcow2 as the backing image.

The next acceptance gate remains the public Vista D3D9 probe: decisive
clear/readback, triangle/readback, and PresentEx values. No guest D3D9 pass,
DWM composition, Aero glass, SPICE cursor runtime behavior, or reboot repeat
has been proved by this Linux work.

## Archived WDK acquisition (2026-09-07)

The original WDK 7.1 ISO was recovered and verified from Internet Archive item
`en_windows_driver_kit_version_7.1.0_x86_x64_ia64_dvd_496758` into
`driver/toolchains/wdk71/GRMWDK_EN_7600_1.ISO`.
`archive-metadata.json` preserves the archive file metadata.
Run `python3 scripts/extract_archived_wdk71.py` in the build container after
download completion: it requires the original ISO SHA-256 recorded in
`notes/unlazy/signing-build-route.md`, checks SHA-1 too, extracts the x86/x64
Vista libraries, headers, and build/driver tools without executing installers,
and writes a per-file SHA-256 inventory to `provenance.json`.
This recovers the missing WDK inputs; it does not recover the old signing key.

Acquisition completed: 649,877,504 ISO bytes, SHA-256
`5edc723b50ea28a070cad361dd0927df402b7a861a036bbcf11d27ebba77657d`,
SHA-1 `de6abdb8eb4e08942add4aa270c763ed4e3d8242`. The extraction completed
successfully with 2,112 hashed files. WDK root:
`driver/toolchains/wdk71/extracted/WinDDK/7600.16385.win7_wdk.100208-1538`.
Both `lib/wlh/{i386,amd64}` contain ntoskrnl/hal/wmilib/displib.
Original x86-hosted x86/x64 `cl.exe` and `link.exe`, `SignTool.exe`, and
`bin/selfsign/Inf2Cat.exe` are available.

A first Linux Clang 18 kernel compilation trial exposed Windows header/source
filename casing and legacy SAL token-pasting compatibility requirements.
`test-artifacts/linux-build/kmd-x64/wdk-vfs.json` maps the WDK headers using
Clang's case-insensitive virtual filesystem without modifying extracted bytes.
`/clang:-Wno-invalid-token-paste` permits the old SAL annotations to preprocess.
The remaining first-unit failure is the source include `virtio.h` referring to
`VirtIO.h`; extend the same VFS to the source tree. No new KMD has been built
or deployed yet. The log is `test-artifacts/linux-build/kmd-x64/first-compile.log`.

## KMD build and debugging VM updates (2026-09-07)

Both x64 and x86 KMDs now build using `scripts/build_vista_kmd_linux.py`
inside `vista-driver-builder` and pass the Vista PE audit. The script retains
compiler commands, per-unit logs, resource preprocessing, and linker commands
under `test-artifacts/linux-build/kmd-{x64,x86}`. The WDK extraction now also
includes common libs packages (2,962 files), supplying Vista BufferOverflowK;
the link enters through GsDriverEntry so its cookie initialization runs.
Clang compatibility code is in `triton-kmd/viogpu/build-support/`: builtin
offsetof preserves static assertions; x86 WDK CAS-loop names avoid builtin
collisions; port-I/O and CR8 intrinsics use the actual CPU instructions and WDK
signatures. `vp_notify` now returns void to match the queue callback contract.
Source audit passes. Actual guest load/graphics are still unproved.

New local test certificate: `driver/signing/triton-vista-linux-signing.cer`.
Its hashes are in `driver/signing/identity.json`; the service's filename,
SHA-256 pin and SHA-1 thumbprint match it. The private key is outside the
workspace under the owner's `~/.local/share/triton-vista-signing/` directory.
Service rebuilt and PE audit passed; package signing remains incomplete.

The user explicitly requested signature-enforcement bypass and more vCPUs.
OOBE account creation completed with local account Triton. The guest reported
Vista Ultimate RETAIL, Notification/grace-expired; built-in `slmgr /rearm`
completed successfully and requested restart. BCD testsigning and
nointegritychecks are Yes, loadoptions DDISABLE_INTEGRITY_CHECKS. These BCD
values alone do not prove unsigned-load acceptance. For the first 4-vCPU boot,
advancedoptions was enabled and the actual F8 Disable Driver Signature
Enforcement menu item was selected. This bypass lasts that boot session.
The debugging settings and first CPU result are saved in
`test-artifacts/linux-build/vista-1cpu-status.log`.

The VM shut down through guest `shutdown /s /t 0`; its process exited, and
`qemu-img check` passed. Snapshot `setup-before-4cpu` is an internal snapshot
of `vista-kvm/work.qcow2` (not the unchanged base). It is before first verified
desktop and must not be described as a completed-desktop checkpoint.
The launcher now defaults to 4 vCPUs (one socket, four cores, one thread each),
with VISTA_CPUS=1..16 override. The four CPUs are confirmed in QMP evidence
`test-artifacts/linux-build/vista-4cpu-qmp.json`.
Current VM PID 62192, run `vista-kvm/runs/boot-LYYH3wKS`, was at Welcome at
last inspection. Revalidate before any further guest actions. One-vCPU WinSAT
CPU LZW compression was 167.86 MB/s; four-vCPU comparison remains pending.
Temporary `/tmp/vista_setup_control.py` was used for OOBE and the user's
explicitly requested boot/performance configuration, not driver deployment.

The four-vCPU boot reached the first verified desktop. Guest licensing status
is Initial grace period with 43,200 minutes (30 days) remaining after rearm.
This is a temporary grace period, not activation. KMS cannot activate this
Ultimate Retail installation (Vista volume KMS editions are Business/Enterprise).
The guest confirms NUMBER_OF_PROCESSORS=4. WinSAT CPU LZW compression increased
from 167.86 MB/s (1 vCPU) to 618.14 MB/s (4 vCPUs), 3.68x in these single runs;
this is not a graphics/FPS measurement. Evidence: `vista-debug-perf.json`,
`vista-{1,4}cpu-status.log`, and `vista-first-desktop.png` in linux-build.

After the guest shut down normally and the exact QEMU process exited, the
working qcow2 passed check. Created internal snapshot
`clean-desktop-before-triton` on 2026-09-07 at 04:31:50 local time. The base
remains unchanged; no Triton driver changes were made in the guest.
Restarted with four vCPUs into run `vista-kvm/runs/boot-agYLr2Mg` and again
selected the actual F8 Disable Driver Signature Enforcement item before boot.
Revalidate current PID from `vista-kvm/latest/qemu.pid` and guest screen.

### Live fresh-device binding diagnosis (2026-09-07 continuation)

Custom Neptune VM remains live, run `vista-kvm/runs/neptune-15u2n1az`, QEMU PID
71839 revalidated. Original service only copied binaries; public D3D9 failed
with KMT 0xc000007a / Create9Ex 0x8876086a and no KMD COM1 trace.
Added guest-owned initial PnP install to deployment service: inspect actual PCI
Enum Service value; dynamically load newdev UpdateDriverForPlugAndPlayDevicesW
(FORCE|NONINTERACTIVE), preserve standalone SCM executable path after legacy
INF installation, reboot before committing/probing. Existing safe replacement
workflow retained. Compiler warnings-as-errors, Vista PE/INF checks, embedded
signature and catalog PE member verification passed.

Published and attached immutable package
`306aba27214a63a33110f5a9aff8f7bb258a5a488d7ad2c16eea419960683246`.
Guest service performed safe-mode replacement/reboot and verified all bytes;
now reports actual GPU `VEN_1AF4&DEV_1050&SUBSYS_11001AF4&REV_01` service=vga.
UpdateDriver fails 0xe0000247 (driver store add failure). Elevated read-only
`findstr /c:"!!!" C:\Windows\inf\setupapi.dev.log` screenshot at run/current.png
shows inner 0xe000024b: INF hash not present in catalog; package appears tampered.
Thus Python-generated catalog's INF subject/hash representation is not accepted
by Vista, even though osslsigncode verifies PE members. Next: derive the actual
Vista INF SIP/hash/subject format and fix create_vista_catalog.py, regenerate,
let existing service redeploy. Do not count catalog PE verification as INF
acceptance. GPU is still Standard VGA; Aero is unproven. Service retries binding
about every 30 seconds. Latest screenshot is diagnostic, not Aero proof.
Temporary input helper /tmp/vista_setup_control.py now maps ! to shift-1.
Attempted type setupapi.dev.log > COM2 got access denied; use screenshot or add
service-owned log-tail diagnostics. Current elevated cmd remains open.

### Catalog accepted; first real Vista public D3D9 pass

Corrected catalog generation against archived WDK `tools/drvCov/amd64/drvcov.cat`:
NUL-terminate NameValue payloads, explicitly encode NULL algorithm parameters,
canonical CRLF INF before hashing, sorted catalog member identifiers, and PE
page hashes (`osslsigncode -ph`). Package b49696f7f749491106f4addaa26e2801571c8d90b3844d5f40f3bb85eac75205
is now attached and installed. Previous a9ddd/3259/72b34 packages verified INF
but setupapi rejected viogpu3d.sys. Final sorted/page-hash package accepted:
GPU_BIND_OK reboot=1 error=0 at guest04:19:27; following guest-owned reboot
GPU_BINDING service=VioGpu3D and POST_REBOOT_COMMIT at04:19:51.
Service now logs native catalog hashes and WinVerifyTrust per member plus a
bounded setupapi.dev.log tail on binding failure. All six native member checks
passed. Generic PE verification alone did not predict DriverStore acceptance.

Real public D3D9 probe PASS, nonce
`e178c456514d83a97f83966d2d0efce4945093ae65af4220c8e71ab15ab3b7d8`:
clear/readback, triangle/readback, texture/bitmap UpdateSurface readback passed;
PresentEx and additional-chain Present returned S_PRESENT_OCCLUDED 0x08760878,
so no visible-present claim. Host DXVK initializes Intel backend and renderer
exports shared blobs. COM1 still zero; do not infer driver unloaded from that
now that actual D3D9 and PnP evidence prove the working path.

At last observation the Aero child PID2736 is live on desktop (screenshot
current.png): UxSms running, DwmIsCompositionEnabled=false, successful enable
requests but composition still false at attempt100. Transition timeout120sec,
then probe invokes WinSAT DWM and retries. Parent service PID probe tree live,
QEMU PID71839 remains live in neptune-15u2n1az. Do not restart while observing.
Next read current screenshot/status for WinSAT/Aero results; goal still requires
visible controlled Glass proof and second guest-owned reboot verification.

### Elevated WinSAT reaches a kernel stall (next investigation)

Old Aero child exited code1: WinSAT CreateProcess error740 (requires elevation).
Fixed probeLaunchAeroDesktop to use Explorer user's linked administrator token
when TokenElevationTypeLimited, preserving same user/session/Default desktop.
WinSAT log handle now inheritable. x64 and x86 probe builds pass; signed media
`4b60fdc47361bb934db3950480a843dfeba3c4dc4cc2dae7f9937fb5cbfc8bd8`
installed by guest service. Public D3D9 passes again, parent logs same-user
elevated PASS, child2636 desktop1728. After120sec composition wait, WinSAT
launches successfully but desktop freezes (clock21:27); renderer exports shared
1280x1024 blob109 ctx15. No GUI response to Win+R/cmd. QEMU71839 still running,
all vCPUs executing CPL0, CPUs0/2/3 RIP fffff800018b81bf; CPU1 RIP
fffffa6001af4ffe. DO NOT treat host process as exited or restart without first
preserving/diagnosing fault.

Read-only QMP CPU registers saved run/winsat-stall-registers.json (correct
cpu-index argument; separate HMP `cpu` command does NOT persist in QMP).
Stack/code reads in run/winsat-stall-memory.json. Match live code bytes to KMD
SYS: base fffffa6001ad1000, RIP RVA23ffe =
VioGpuCommander::CancelRenderEvents+0xd6 (confirmed PDB publics, now saved
kmd-x64/publics.txt). Stack includes VioGpuAdapter::ResetDevice+0x2e (RVAa8be)
and 0x119 (possible VIDEO_SCHEDULER_INTERNAL_ERROR; not yet conclusively decoded).
CPU1 loops submitted-list scan, with R14=head fffffa800552b068 and R15 pointing
to running sentinel fffffa800552b078, which points to itself. Submitted head
Flink=fffffa8002b22b68 Blink=fffffa80031c2b08. This indicates malformed queue
traversal; identify initial scheduler failure and queue corruption/race before
changing code. Source queue ops in viogpu_command.cpp1135–1188; callback703–743;
CancelRenderEvents854+. Other CPUs in kernel spin likely fault rendezvous.
Kernel is unmodified. COM1 trace remains empty despite real driver execution.

Also added Linux /proc executable+FD ownership checks to passive capture script
(macOS lsof branch retained). Existing 11 negative controls pass; live QEMU
identity+QMP socket/statuslog ownership check passes. No Aero proof capture yet.

### Confirmed scheduler bugcheck and deferred-preemption fix

Located live checked kernel base fffff8000185d000 and KiBugCheckData export
(RVA7506a0) using original base's unmodified ntoskrnl.exe. Read-only QMP proves
bugcheck119 parameters {1,2e3a,2e3c,2e3c}: invalid fence ID. Saved
winsat-bugcheck.txt. Additional winsat-stall-queue.json/commands.json show
submitted fence2e3b linking to a freed command2e3c, then running sentinel;
active command2e3a was in NotifyCompletion when scheduler bugchecked.

Source VioGpu3DDdiPreemptCommand immediately acknowledged preemption while
host commands remained queued/in flight. That permits scheduler reuse before
driver ownership ends. Replaced with QueuePreemption, a fence-only FIFO marker;
worker drains preceding submissions before reporting DMA_PREEMPTED with the
actual final LastCompletedFenceId. Marker does not advance ordinary completed
fences. Also removed CancelRenderEvents from bugcheck ResetDevice, which must
not traverse queues/signal events at HIGH_LEVEL with other CPUs frozen.
Source audit plus x64/x86 Clang18 container builds and Vista PE audits pass.
(Initial host build hit missing llvm-rc-18; reran all objects in correct
vista-driver-builder container, passed.)

Signed package3307afabbb03409c92124106a29c1f9783718cd4947a48d6aab205b40cefad76
attached. Confirmed-crashed guest recovered via one QMP system_reset after
saving fault evidence; reason in run/bugcheck-recovery.json. No base change or
rollback. Guest service detected media, completed safe-mode deployment/reboots,
verified bytes; public D3D9 PASS again. Aero child2760 same-user elevated,
last screenshot at initial transition attempt90; WinSAT will launch at120.
QEMU71839/runneptune-15u2n1az remains live. Need observe actual WinSAT result;
preemption fix not yet runtime-proven against failing assessment.

Follow-up runtime result: WinSAT(DWM) now exits0 PASS and desktop remains
responsive. Screenshot current.png shows subsequent composition attempts10–30
return0x80263001 (DWM_E_COMPOSITIONDISABLED). Deferred preemption therefore
passes the previously crashing assessment. WinSAT log is676bytes and appears
UTF-16 (probeAppendFileTail prints only W due embeddedNUL); improve decoding or
read guest log as Unicode. Aero child still live retrying composition; no Glass
proof yet. Next diagnose WinSAT assessment output/theme/DWM admission rather
than relaunching a running probe.

### DWM fault isolated to missing runtime primary surface

Read WinSAT log via Notepad: graphics541.25F/s, memory28641.20MB/s,18.16s.
Aero theme applied through stock Theme Settings; no Glass. Event log command
`wevtutil qe Application /c:8 /rd:true /f:text > \\.\COM2` works when probe
parent is not holding COM2. Dwm.exe crashes with c0000005 at d3d9.dll+bc132.
Do not infer a policy/admission failure from DwmIsCompositionEnabled=false.

Extracted original base System32/d3d9.dll into linux-build/base-d3d9.dll.
Downloaded exact matching Microsoft symbols to base-d3d9.pdb from
https://msdl.microsoft.com/download/symbols/d3d9.pdb/9151054F49D34B909DB85AB4E17960531/d3d9.pdb
PDB publics at base-d3d9-publics.txt. RVA bc132 is D3D9SetDisplayModeLH+0xa6,
`bt dword ptr [rbx+48],15`. Earlier assertion says 0 != pPrimarySurf.

Added development UMD vectored exception telemetry in triton9_ddi.c: observes
first access violation, records PC/RBX/RDI/SP,d3d9base and48stackwords, returns
EXCEPTION_CONTINUE_SEARCH; registered at DLL attach, removed on detach. Does
not patch or handle Windows code. Both arch builds passed. Deployed immutable
`d9565b6b976bbbfd70ae51523cb78fda0e6a5f5d9c8c40ee1fc21e0a46e7d541` via guest
service. Public D3D9 stillPASS, Aero endsFAIL. After parent exited, elevated
`net stop uxsms` / `net start uxsms` reproduced fault and captured telemetry;
`findstr TRITON9-FAULT C:\Windows\Temp\triton9-ddi.log > \\.\COM2` relayed it.
run/dwm-exception.txt is last captured record:
PC7fefa1dc132 d3d9base7fefa120000 RBX=0 RDI3129850 SP2c6e200.
Stack return d3d9+dac06 = CBaseDevice::EnableFullscreen+0x2da. DWM never reaches
UMD SetDisplayMode; missing runtime primary surface precedes it.

DWM oldPID974/624 logs in status around7869–7951 show CreateDevice Flags0,
then just one CreateResource X8R8G8B8 Pool3 Flags0x81,800x600 successful;
no primaryCreateResource/OpenStandardPrimary follows. Current code only marks
needsPresentAllocation for DiscardRenderTarget; DWM uses non-discard RT0x81.
Also host/KMD backing is lazy until use, and allocation callback hResource is
NULL for nonshared render targets. Microsoft docs permit nonshared resource
association and recommend it for related allocations; current comment saying
only SharedResource can have KMD resource group is overly strict. Hypothesis
for next fix: support non-discard swap-chain targets and ensure allocation/
runtime association before fullscreen initialization. Verify exact lifetime
handling (deallocation by hResource when associated) and avoid claiming cause
proven until actual DWM run. No resource fix made yet.

Separately fixed probeAppendFileTail UTF16LE decoding so WinSAT output is not
truncated to W. Compiled x64 successfully; this probe-only decoding change is
NOT yet repackaged/deployed (active d956 uses previous probe). x86 needs rebuild
for that last edit. Current guest is responsive, elevated CMD open, UxSms was
restarted, no live Aero probe after itsFAIL. QEMU71839/neptune-15u2n1az live.

### Eager backing experiment and Vista transport ownership fix

Experimental resource changes now include nontexture nondiscard color RTs in
needsPresentAllocation, eagerly EnsureResourceHost before CreateResource return,
and associate their AllocateCb hResource with hRTResource (track association for
matching DeallocateCb). This targets DWM's flags0x81 surface. It is not yet
proven to fix DWM. UTF16 probe log decoding is now built/deployed both arches.
Packages03a286... and e1ff87aed91f5b383319353594a5d8e24b98ef6ae64fff999e8bb1a2be75fcc3
regressed the public test at CreateRenderTarget, following two successful
CreateDeviceEx calls. Exception module telemetry added via VirtualQuery and
GetModuleFileNameA maps the fault to Neptune RVA18e598, virtgpu_map_blob_op at
npt_renderer_virtgpu_win32.c:539 (memset of runtime-owned allocation list).
This is separate from DWM's earlier null-primary crash. Latest fault telemetry
is in run/status.log around61400; preferred PE base277e10000 for addr2line.

The public probe creates HWP and PURE devices concurrently, THEN releases HWP
before rendering on PURE (tests/triton9_runtime_probe.c:2253). Eager backing
causes both to acquire Neptune. npt_device_acquire was a process singleton,
so PURE reused the HWP owner's Vista callback handles/context/DMA buffers.
Destroying HWP invalidates those buffers while PURE still references them.
Source previously described release order incorrectly; see actual probe code.

Changed NPT_D3D9_RUNTIME_DDI builds to acquire a separate refcounted transport
per outer D3D9 device. Release now takes its owning npt_device pointer; COM
wrappers save that pointer before free. Explicit retain pins the existing
transport for guest swapchains. Other builds preserve singleton acquisition.
Backend workaround flags cache after synchronous D3D11 device reply, without
keeping a pointer to an arbitrary transport. Both x64/x86 full builds passed.
Signed/catalog-audited package
9003f33489c7893b5aac0a72939f221eb1b1d34fd223fb0c2a51d627490374a9
published and attached to live QEMU71839/neptune-15u2n1az via QMP media swap.
Guest service owns deployment and reboots. Runtime result pending; Glass is
still unverified. Last fully passing public/WinSAT package remains d9565b...

Runtime9003 result: public D3D9 PASS (both HWP/PURE coexist, first released,
second clears/draws/reads/presents) confirms transport lifetime regression fixed.
WinSAT exits0:680.37F/s,36003.00MB/s,16.54s. Stock UxSms stop/start was issued
from elevated CMD while Aero child waited. Composition subsequently reports1
and ExtendFrame/EnableBlur both succeed. This is a FALSE visual success: after
minimizing diagnostic CMDs and Welcome Center, the proof glass rectangle shows
stale Welcome Center pixels, not translucent/blurred blue/yellow stripes.
Gated passive PNGs run/transport-first-aero.png (Welcome obscures scene) and
run/transport-proof-stale.png (proof unobscured) saved. Neither proves Glass.
Fresh Application Error at guest22:19:41: Dwm.exe PID960, c0000005,
d3d9.dll+bc132. Eager backing/resource association did NOT fix null primary.

Probe's API-PATH-only result0 triggered service-owned REPROBE_REBOOT at
status line69305. Second public probe alsoPASS; current Aero child2776 is
running after reboot (same liveQEMU71839), so do not restart just for a wait.
New extended exception diagnostics compiled x64/x86 and signed/catalog-audited
package bfc0acb5d83bafedb8581b29c3903819967aedfb3069955ab221acc3acb4d181
is published in deploy-current but NOT ATTACHED; active CD/deployment-id9003.
Attach after current probe terminal. Diagnostics add RSI/R13/R15 and bounded
ReadProcessMemory copies for exact D3D9+bc132 only: RSI points to swapchain slot,
swapchain+48/+50 surface objects, surface+38 vtable. EnableFullscreen disasm
atD3D9+dabbD..dac06 calls that vtable+90 getter, which supplies null R8 to
D3D9SetDisplayModeLH. This should identify the runtime object/getter responsible
without patching Windows or changing exceptions. Glass goal remains incomplete.

### Saved caller frame for the primary-surface fault

9003 second boot Aero exited1 (status69481): composition stayeddisabled after
WinSAT688.30F/s,36422.50MB/s,19.11s. Installed bfc0 diagnostic after terminal.
Public gatePASS. Restarted stockUxSms from elevatedCMD for fresh DWM attempt.
COM1 is usable for read-only diagnostic output while probe ownsCOM2:
`findstr TRITON9-FAULT C:\Windows\Temp\triton9-ddi.log > \\.\COM1`.
Also read root log separately (old entries there). This intentionally adds
text to run/com1.log; it is no longer an exclusively KMD trace or empty file.
New faultPID8c4 saved to run/dwm-bfc0-exception.txt:
d3d9base7fefa1e0000 PC7fefa29c132 RBX0 RDI2e19850 RSI0 R133b00720.
First object diagnostic incorrectly used currentRSI. D3D9SetDisplayModeLH
reusesESI for a callbackHRESULT; prologue saves callerRSI atcurrentRSP+f0.
Existing stack index1e confirms savedRSI3b038d0, consistent with caller
CBaseDevice base3b000c0, swapchain-array offset3810. Corrected bounded object
read to dereference savedRSI from stack, not currentRSI. Botharchbuilds passed;
signed/audited ccd6f138daec7ba771e8598ae5758e0b947dadcdaa15b4e39dda8f1f8df13a52
published and attached; service owns installation/reboots. Revalidate progress.
Read-only base symbols show CSurface's CBaseSurface vtable slot90 is
CSurface::KernelHandle (RVAf4e84), returns[rcx-18], i.e.surface-object+20.
Temporary /tmp/vista_d3d9_symbols.py maps RVA or preferredVA to nearest public.
No Windows code patched. Primary null remains actual blocker under diagnosis.

ccd6 capture run/dwm-ccd6-exception.txt: savedRSI47938d0 -> swapchain4795640 ->
surface[48]=7c93a0, surface[50]=NULL. Actualsurface vtable RVA319e0 is
CDriverSurface::CBaseSurface, slot90 KernelHandle RVAf4e84. Surface+20 is0.
Thus this is an allocated CDriverSurface wrapper lacking runtime kernelobject.
Read-only disassembly reveals constructor conditional atRVAf3be0 checks
CEnum+adapter*308+354; if nonzero and creationflag20, skips kernelcreation.
Other references (CSwapChain::CreateFullscreen) include checked diagnostic
"DDraw support is required for extended primary surface creation". Our UMD
explicitly rejects Interface7, observed in every DWM startup. This likely
causes that DDraw-unavailable branch. Microsoft full-screen-mode docs describe
shared-primary OpenResource for legacyD3D9 and primary CreateResource for9L:
https://learn.microsoft.com/en-us/windows-hardware/drivers/display/full-screen-mode-behavior
https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dumddi/ns-d3dumddi-_d3dddiarg_openadapter

Changed adapter/device admission to accept DirectDraw7 alongsideD3D9.
Implemented D3DDDICAPS_DDRAW and DDRAW_MODE_SPECIFIC: baseline resourceDDI,
only optionalcapDYNAMICTEXTURES, no unsupported color-key/overlay/mirrorflags.
Botharchbuilds pass; signed/audited package
768b7c2558cf5feb5740c825d888d7497122c042305abe25de6755f80cc0f91a
published/attached viaQMP; service owns deployment. Runtime result pending.
No claim this fixes DWM until fresh guest run. Last fault diagnosis is stronger
than earlier eager-allocation hypothesis; retain lifetime fix independently.

768b runtime: Interface7 adapter AND device creation succeed, callback mask0.
DDraw capability Types1/2 accepted. It then queries Type8 sizec8 (200bytes) and
Typeb size74 (116bytes), both rejected; DWM stillnull-primary. Microsoft caps
reference maps these to D3DHAL_GLOBALDRIVERDATA and D3DHAL_D3DEXTENDEDCAPS, NOT
application D3DDEVICEDESC7:
https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dumddi/ne-d3dumddi-_d3dddicaps_type

Added triton9_legacy_caps.h private wire layouts to avoid legacy d3d.h vs
modern d3d9types.h conflicts. Implemented GETD3D3CAPS/GETD3D7CAPS from existing
D3DCAPS9 (legacy filter/blend bits explicitly mapped; depthbits from actual
D16/conditionalD24S8 formats). Built botharches. Independent SDK-header ABI
compile test test-artifacts/linux-build/legacy-caps-abi.c includesddrawi/d3dhal
and verifies sizes/keyoffsets against private layouts; x64+x86PASS.
Signed/audited installed package
fad76477d4e4234254b233f3e34f18b9b7ce8e7382417cfb44628a147e020ab7.
Public D3D9PASS again, Aerochild2756 active atlastinspection, QEMU71839alive.
Restarted UxSms via elevatedCMD; legacy caps nowaccepted (COM1lines37258–37359).
Still fresh DWM5c4 null-primary atd3d9+bc132, saved
run/dwm-fad764-exception.txt. Thus DirectDraw admission/caps fixes are real gaps
but have NOT yet resolved runtime's primary-creation skip.

COM1 now also includes full copies of both guestDDIlogs; root log is LARGE
(~160k+lineshostcom1, live copies may take seconds). Prefer narrow findstr when
possible. Current elevatedCMD open; rootlog can contain current fault too,
so inspect both root andWindowsTemp. Read-only regqueries for HKLM DirectDraw/
Direct3D performed; DirectDraw output mostlystockappcompat, no demonstrated
activation/emulation override. No registrychangesmade for those queries.
Next useful diagnostic: read CBaseDevice (swapchain+10) -> CEnum(+3900), its
adapter selector at+354, determine which real condition keeps primaryhandle0.
Offline disasm /tmp/vista-d3d9-full.asm, /tmp/vista-driver-surface.asm,
/tmp/vista-create-surface-lh.asm available. CDriverSurface skip atRVAf3be0,
CreateSurfaceLH atbc290, RegisterDDrawCaps ata4174. A guessed ADAPTERINFO+2fc
xref led to unrelatedDXVA; do not treat that as selector assignment evidence.

2026-09-07 resumed primary-surface diagnosis (no agents): last activation-only
turn was no progress on Aero; live QEMU71839 revalidated via process and QMP.
Package80b4e289339d72ccab9a25710b5fed7f4aa87d4c264dee099e134592080c2e50
had added read-only VEH telemetry for CBaseDevice+3900 -> CEnum and adapter
selector; actual selector+354 is1. Correct adapter table base is CEnum+68,
stride308, so selector is ADAPTERINFO+2ec (not2fc).
GetDX8HALCaps a4b44 writes it at a4dc9/a4dd2: selector1 if internal caps+8
has DDCAPS_NOHARDWARE(02000000) or lacks DDCAPS_BLT(40). Embedded checked
messages are 'No ddraw acceleration is available in the current mode' and
'DDraw HAL does not support blt'.
Added cached-driver-caps read-only telemetry, built botharches, signed and
service-deployed bd1f4d3cfd8decc16ff005122f149247c81d1b32e0a7063e87ea63f9680a2ba4.
Public D3D9PASS; restarted UxSms and captured actual cached Caps=80000000,
Caps2=a4488800, proving missing BLT, not NOHARDWARE. Evidence saved in latest
run/dwm-bd1f-capability-evidence.txt. QueryLHDDICaps RVA c5cf0 calls UMD
GetCaps(Type0d,size130) directly into internal drivercaps at c5e46 then merely
sets Caps bit31 at c5e77. Thus our Caps=0 propagates into legacy admission.
Changed triton9FillCaps Caps to DDCAPS_BLT wirevalue40, matching existing
triton9Blt implementation. Botharches compilePASS. Signing/deployment and
actual primary-creation/visual outcome pending; do not call this Aero success.

BLT-capability package32123ce35cb887091fa695c8919f1f0bf4361690ff3763b122210e3dca2ba7c0
signed/published/attached; guest POST_REBOOT_COMMIT at06:00:27Z and public
D3D9PASS. Aerochild2776 nonce803a7fa782eecdccfab5dab18f6e3afea2299150b253ab3b5d0702acc510542c
reported initialcomposition0, WinSAT685.18F/s36257.40MB/s18.10sPASS, then
compositiontransition WAIT. Screenshotcurrent.png shows basic desktop, not
Glass. RepeatedsuccessfulPresent lines in rootlog are NOT proven DWM (WinSAT
and publicprobe append there too); earlier commentary inferred too much.
Old bc132 fault9a8 in WindowsTemp log belongs to previous bd1f capture and
cannot establish 32123failure. COM1 now~360k+lines due whole-log copies.
To isolate fresh restart, preserving both guestlogs by move to same-directory
triton9-ddi-pre321-restart.log while UxSms stopped, then starting UxSms and
reading new logs. Hostautomation session36876 active atthisnote; inspect its
completion and newCOM1tail. Originalguestlogs preserved, baseunchanged.
No visualPASS and no across-two-boots proof yet. Goal remains active.

Clean restart COMPLETED(session36876 exit0): fresh source caps logged40,
DWM now calls CreateResource Format16(X8R8G8B8),Pool3,Flags00008081,
SurfCount2,800x600. This is an actual primary flip chain (Primarybit8000),
and existing triton9CreateResource rejects SurfCount!=1 before allocating.
Thus BLT capability fix makes concrete runtime progress past skippedprimary;
next implement real two-surface primary/flip-chain semantics, not ignore
SurfCount or fakeoneallocation. Evidence latest/dwm-32123-primary-chain-rejection.txt.
Aerochild completedFAIL composition0 afterWinSAT. No freshvisualcaptureproof.
Current elevatedCMD open; UxSmsrunning aftercleanrestart, QEMU71839lastverified.
BotholdguestDDIlogs preserved under triton9-ddi-pre321-restart.log; newly
createdlogs now much smaller, use them for current failures. Source nextentry
triton9_resource.c CreateResource around1099 SurfCountguard, standardprimary
allocator538 and SetDisplayMode1759; surfaceCount currentlyhardcoded1.

2026-09-07 primary-chain implementation: previous goal turn was progress.
Added TRITON9_RESOURCE chainSurfaces and indexed resolver. CreateResource now
supports equal-sized Primary+RenderTarget nontexture chains of2..4 entries;
entry0 uses standard nonblob primary allocation, later entries have distinct
exportable render-target allocations. Chain allocations are independent
(hResourceNULL on AllocateCb) to allow per-entry destruction without releasing
siblings. Handles publish only after all entries create; destroy visits owned
children, retaining unfinished entries if destroy fails. Indexed SetRenderTarget,
Blt,ColorFill,SetDisplayMode,Present,Lock/Unlock/Async/Rename route selectedentry.
Single-surface behavior preserved; texture mip/array resources stillunsupported.
Source triton9.h/triton9_resource.c/triton9_output.c. BotharchbuildsPASS;
existing cpu-layout/clear-contract/draw-contract tests3/3PASS. These do NOT
prove chain runtime or Glass; guest validation stillrequired.
Signed/published/attached09ff1635cab452740a94b5f86aba1c6526e15e4e2a6fae63f9dbb6f7604d0728.
Service emitted REBOOT_TO_SAFE_MODE06:09:08Z, but no installation orpost-reboot
commit yet. Guest boot returned toWindowsErrorRecovery repeatedly. Selected
SafeMode, verified its desktopcornerlabels once, but subsequentserial emitted
NORMAL_READY06:12:06Z and06:13:10Z with noinstall. Returned again toRecovery.
Nohostreset performed. BriefQMPstop/readregs/cont used to synchronizeKVMstate
(reading registers while running can give stale saved register values).
CurrentQEMU71839alive; diskreadstats5127935ops36313242624bytes,0failedreads at
lastsample. BIOS/bootmanager32-bitCPU0 observed; secondaryCPUsHLT, not kernel
bugcheck evidence. Attempting F8 to disable autorestart/capture nextfailure,
notyetconfirmed. QMPsock allows onlyonecontrolclient: eventmonitor65076 ended,
so do not hold anotherclient whileusingcontrolhelper. LastF8inputsession69516
pending atthisnote. NoGlassclaim; newpackage runtimepending duebootrecovery.

2026-09-07 boot recovery diagnostic progress: captured actual0x3b with exception
80000003 at nt!DebugPrompt (1bced8), not an unexplained boot delay. Paused on
blue screen before autorestart to preserve live context. Oldrunneptune-15u2n1az
has boot-bugcheck.png, boot-3b-live-context.json, boot-crash-modules.json,
boot-present-rejection.json. Kernelbasefffff8000185c000; KiBugCheckData7506a0
=>3b,80000003,fffff80001a18ed8,fffffa600d1f6dc0. Caller watchdog!WdLogEvent5
noncriticalerror; DXGCONTEXT::Present+2176(dxgkrnlRVA6f372) rejects C000000D
beforeKMD callback. Allocation/resource0550ffa0 points+30 to055096b0 whose
flagsDWORD+4=0; kernel demandsbit1or2. Wdrecordlevel2 atfffffa800680e800:
caller3675372,argsstatusC000000D,05522d80,0550ffa0,055096b0,4000fdc0.
Need investigate resource association/Present destination semantics if recurs.
Exact Microsoftsymbols downloaded: base-ntkrnlmp.pdb/publics,
base-dxgkrnl.pdb/publics,base-watchdog.pdb/publics. Tools/tmp/vista_read_guest.py
(readonlyQMPmem/modulelist/pdbidentity), /tmp/vista_kernel_symbols.py and
/tmp/vista_pdb_symbols.py (publicRVAmapping). Firstscript's __main__ hardcodes
THIScrashedkernelbase; revalidateASLR before reuse. capstone installed in
/tmp/vista-inspect-venv for readonly disassembly of guestmemory.
ClosedconfirmedcrashedQEMU71839 viaQMPquit. qemu-imgcheckPASS, saved internal
snapshot boot-3b-before-vga-recovery. Originalbaseunchanged. Added optional
VISTA_RECOVERY_VGA=1 to launcher for temporary standardVGA boot, defaultstill
virtio-vga-gl. Recoveryrunneptune-ukorm4m6 PID133971 booted successfully and
verified09ffinstalled bytes/POST_REBOOT_COMMIT06:24:29Z. That is fileproof,
NOT virtioGPUrenderproof (GPU_BINDING registryentrycanbe stale with VGA).
Normalguest shutdown /s /t0 completed and processgone, then launched ordinary
virtioGPU VM via defaultscript: newlatestneptune-bw65rrmx PID134620.
No basechanges, noWindowsbinarypatches, noofflineguestregistryedits.
Current09ffvalidation pending on actualvirtio boot. Observation session6484
polls newstatuslog and wasactiveatthisnote. Need inspect currentVMbeforeusing
keys. Work stillrequiresvisibleGlass andtwo guest-owned successfulreboots.

Virtio retest09ff reproduced same0x3b; newrunneptune-bw65rrmx has captured
boot-bugcheck.png,boot-bugcheck.txt,boot-current-stack.txt,
boot-crash-modules.json,present-rejection-disasm.txt,boot-caller.txt.
ConfirmedcallerDWM PID62c by watchdogeventETHREAD43ea060 ->process45a7c10,
ImageFileNameoffset238. IMPORTANT correction: DxgkPresent+6f372 validates
FLIP SOURCE eligibility (hSource atD3DKMT_PRESENT+14); earlierdescription
'destination' was wrong. Flagsat+58 hasFlip4, branchrequiresprimary/scannable
source flags. KMD flipcallback independently demands allocation.IsPrimary().
09ff chain incorrectly clearedPrimary on backbuffers; hence even after
chaincreation worked, DWM's firstflip caused checked watchdogDebugPrompt.
Corrected chain: preservePrimary for EVERYentry, use independent exportable
host-backed primary allocations for everychainmember (includinginitialfront).
needsPresentAllocation nowincludes primarychainentries; onlystandalonestandard
primary remainsnonblob. RegisterSharedTexture permitsflipprimaries, setsboth
options.primary and ALLOCATIONINFO.Flags.Primary fromresource.isPrimary.
This alignsrendered texture with actualscannedallocation for allindices.
BotharchesbuiltPASS, signedpublished ae5a602aff42fab237ac97ff42ef7dc4543961d3b69c8ba685801e3300703b1c.
Subsequentchangesonlyupdatecomments toreflectflipbehavior; binariesreflect
allfunctionalchanges. NoWindowsbinarypatches.
Closedconfirmedcrashed134620 viaQMPquit; checkPASS. StartedtemporaryVGA
recoveryrunneptune-sl_wd9pt PID137549 onae5amedia. Currentlatest=thisVGA run,
NOT virtio. ServiceNORMAL_READY06:30:38Z andpackageaccepted; SafeModeinstall
inprogress. Pollsession75013liveatnote. Mustwaitinstalledbytes, normalguest
shutdown, thenrestoredefaultvirtioGPU andvalidateactualDWM/Glass. Do notcall
VGAbootorregistryGPU_BINDING anAero success. No rawofflineguestdiskedits.

2026-09-07 blob scanout diagnosis (no agents): ae5a committed in VGA at
06:31:34Z, normal guest shutdown completed, default virtio run
neptune-l4hxdepb PID138815 committed ae5a at06:33:10Z. Boot no longer
hit checked flip-source rejection, but display inactive and host repeatedly
rejected RESOURCE_FLUSH104 with INVALID_PARAMETER1205.
Confirmed source cause: virgl_renderer_resource_get_info returns zero pixel
dimensions for untyped exported blobs (no pipe_resource). QEMU incorrectly
validated blob flushes against those dimensions. Updated virgl_cmd_resource_flush
to retain overflow/empty checks but apply texture bounds only to nonblob,
matching simple GPU blob semantics. Blob framebuffer size already validated
against blob_size by SET_SCANOUT_BLOB; updates clipped to scanout.
Second host issue: Neptune blob branch used transfer_read_iov(ctx0), which
requires pipe_resource. Route DMA-BUF-backed Neptune blobs through existing
virtio_gpu_update_dmabuf/EGL import and GL flush instead. Ordinary textures
retain software readback. QEMU build PASS (existing warnings).
ACPI system_powerdown requested; PID138815 exited normally. qemu-img check
PASS. Updated QEMU launched default virtio runneptune-5ccbsqan PID141748,
ae5a bytecommit06:39:17Z. Recurrent1205 gone; one104/1203 invalid-resource
observed. Screen currently stale Windows boot orb, not Glass proof.
KMD blob FlushToScreen still flushed BEFORE selecting scanout, so updates
can miss newly selected buffer. Changed to SET_SCANOUT_BLOB then FLUSH;
TryPromoteFlip rearms on failure, identical scanout retry is idempotent.
Both KMD arches rebuilt/PE audit PASS. Signing newpackage currently pending
session36678. Need attach newmedia, wait guest-owned install/reboot, inspect
actual pixels and fresh probes. QEMU141748 remains live; no forced reset.

Published/attached110de459f9a33d4f622c4922bd5de8e5205c7f54c57fec54cb3ed944419d166d.
Guest service requested SafeMode reboot06:40:58Z. During reboot QEMU141748
exited139, terminal confirmed podman inspect (not timeout). Hostlog retained
in neptune-5ccbsqan/host.log: egl-headless.c:151 assertion destination format
PIXMAN_x8r8g8b8. Software scanout can leave otherformat at identicalsize;
qemu_console_resize skipsallocation. Added canonical XRGB surface replacement
on egl_scanout_texture when destination absent/wrongformat. Host rebuiltPASS.
Firstlaunchcheckfound2 leakedqcowclusters, no corruption. Attempted reflink
backup unsupported (emptyfile removed); qemu-img check -r leaks freed exactly
those2unreferencedclusters, subsequentcheckPASS. No base edits.
Defaultvirtio VM nowneptune-5k7lmrxq PID144360, latest points here,110demedia.
Currentobserver43121 pollsbytecommit, live atnote. Need verifyactual install,
freshscreens andpublic/Aero probe; nopixelsuccessclaimed. Otherobserversdone.

Current110de guest installed bytes verified POST_REBOOT_COMMIT06:43:28Z on
actualvirtio. KMD0ef10e94c77cd372bb2eae71c8315cf50cbf21e168d17cdb874b6841757c5f79.
PublicD3D9 freshnonce d5831a9bf5c35f159413720c85369276989e99c044f358784c08544b769de699
PASS includingclear/triangles/bitmapreadback, PresentExoccluded08760878.
AerochildPID2820 launched againstdesktoppid1636; currentpixelsallblack.
No ctrlerrors/assertions in currenthostlog atlastcheck. VM144360live.
Thushostscanouterrorfixes verified beyondpreviousfailures but GlassNOTproved.
Read-onlyWin+r attempt tocopy WindowsTempDDIlogtoCOM1 producednothing;
maynotreachshellwhileblack. Do notassumeCOM1containsfreshDWM evidence.
Observer58238 waitingAeroAPI/exit (40smaximum), noreset/relaunch justified by
blackimagealone. Need followcurrentAerochild and captureDDI/gueststate to
resolveblackoutput. Allfunctionalchanges builtanddeployed, baseunchanged.

2026-09-07 continued110de diagnosis: previousgoalturn progress. VM144360
stilllive. CtrlAltDel rendered correctsecuritydesktop, Esc/CtrlShiftEsc
restored responsive BASICdesktop. Saved screenshotscurrent.png (notGlass).
Aerochild completedFAIL,composition0 before/afterWinSAT,80263001 throughout
both120sattempts. WinSATexit0,639.09F/s,33818.70MB/s,notvisualproof.
Saved aero-110de-failure.txt fromstatus. Eventlogdwm-events.txt provesDWM
fatal9020/exit9009code88980406 at23:48:50, alsoearlier23:43:28.
Fresh UxSms stop, preserved root/tempDDIlogs as triton9-ddi-pre110-restart.log,
thenstart. New DWMpid514 created primarychain, shaders anddraws, stoppedafter
resource10081creation (368x289). NoAVorloggedDDIreject. Logcaptured in
neptune-5k7lmrxq/dwm-110de-restart.log. UnprivilegedCMDcouldnotreadTemp;
elevatedCMD viaStartcmdCtrlShiftEnterAltC works. BothrootandTemplogs now
copiedCOM1 (containsoldandnew logs; usefresh-ddi-offset). COM1-clean strips
NULpaddingfromoldlogs; do notshowrawNULtailinoutput. QMPrelative mouse only,
noabsdevice; hmpmouse_move/buttonwork. Currentfocus elevatedCMD.
Added failure-only wrappers for22resource/output/queryDDIcallbacks in
triton9_failure_trace.h, installedfromtriton9_ddi.c. LogsFAIL-PIDandoperation
HRESULT, preservesargs/results. BotharchesbuiltPASS. Signingcurrently82285,
needattachnewpackageandletguestserviceinstall/reboot, thenretrieveFAILlines
fromTemp/root with elevatedCMD. Noagents,noWindowsbinarypatches.

1530cd959a6ec97dc2a157b9e627f13202fa1b4d741eedccaa0a340879181a0b
installedverified06:52:08Z; failure-onlytrace caught Lock8876086c PID57c
(DWMinitialization) and804. AddedLockarg/state detail thenbuiltbotharches,
published073d6d0e366e5533f42352157320e19a1fa4e7144f2a71c1e4453cb3c9f0956d,
installedverified06:55:00Z. Current073dnonce02af38c5fb7f45ed5f7739cf40282fa68df9daa295c8095e99a349a6c4bdca9a.
Detailedcapture lock-073d-details.txt proves PID57c locks flags94
(NotifyOnly|RangeValid|NoOverwrite),pool1SYSTEMMEM,format64VB,width9c420,
state54(buffer,writeOnly,shadow,NOTlocked). Rangesoffset0,80,100,180,200,800
andvalidsizes80/600/3400. Rejectedsolelybecause codeforbidsNoOverwrite on
SYSTEMMEM. CorrectedLock: allowNoOverwrite onvalidbuffer/rangeincluding
SYSTEMMEM; retainReadOnly/range/boundsvalidationandSYSTEMMEMDiscardreject.
BackingaliasesruntimepSysMem, existingUnlockuploadusesexactregion.
OtherPID868failureflags82pool1format16,state5(alreadylocked) mayneedseparate
investigation; do nottreatasDWMsamecause. NewfixbotharchesbuiltPASS;
signing3443currentlypending. NeedattachnewmediaandverifyDWMpastthispoint.
VM144360samecurrentrunlive; nohostrestarts thisturn,allguestserviceowned.
Nootherobserversrunning. NoGlassproofyet.

Lockfixpublished8da392255993ae5c7be6325b5e81518490bb690255eb2f6fe3bf90be8abcc23f,
attachedliveQEMU144360. Observer5747 pollsinstalledbytecommit (45smax).
NeedwaitsameVMandverifyactualnewpackage beforejudgingfix. Lastscreenblack
again; old073dAeroFAILlogged. Do notresethealthyVMmerelyforblackdisplay.

8da3actualbytecommit06:57:15Z confirmed,newnonce
f9bfbd63a0f61ffc33e6c8d488fd6bd00337b6cfcf03b25b7574806f63b9f0fc.
PublicD3D9PASS, Aerochild2788 launched(desktoppid1628). Aftersecuritydesktop
roundtrip,elevatedCMD retrievedfilteredTempFAIL/PROOF-PID into
post-lockfix-failures.txt. NewDWMPID5e0hasNO94vertexlockfailure; newPID8ec
stillhas82NotifyOnlyWriteOnly bitmap130x100state5(alreadylocked). Do not
conflate thisotherprocesswithDWM. NeedinvestigateDWMremainingfatal/path,
allblackbeforesecurityroundtrip, notGlass. Latest screenshotcurrent.png.
CurrentVM144360live, same neptune-5k7lmrxq,8da3installedandmediaattached.
Alltoolsessionsended. Currentfocus elevatedCMD; useitforread-onlylogs.
GoalprogressactualLockcontractfixedandruntimefailuredisappeared; fullGlass
andtwo visualproofrebootsstillunverified. Keepgoalactive.

2026-09-07 remaining8daDWMerror diagnosis: priorgoalturnprogress; liveVM144360
unchanged. ReadTempDDIandApplicationevents,dwm8da-all.txt confirmsDWMexits
88980406aftervertexlockfix. Addedboundedgueststartupdebugobserver source
scripts/vista_dwm_observer.c, buildMinGWstaticx64subsystem6.0,2minwatchfor
DWM,DebugActiveProcess,DebugSetProcessKillOnExit(FALSE)afterattach, logs
OutputDebugString/DLLbases/exceptions; onlyinitialattachbreakconsumed,
applicationexceptionsDBG_EXCEPTION_NOT_HANDLED. NeverkillsDWM; detachesonlimit.
Transferredvia temporarydwm-observer.iso intoC:\Windows\Temp, thenrestored
8damedia. AutorunExplorerstealsfocusonmediachange; alwaysreopen/focusCMDafter.
OneerranttypingintoExplorerlaunchedE:\triton9_runtime_probe_x64.exe; exclude
thatmanualprobe fromverification. Realnonce/serviceproofremainsindependent.
ObserverstartedbeforeUxSmsrestart; attachedDWMpid2152(868), captured
completeexitDONE into neptune-5k7lmrxq/dwm-observer.txt. Guestobserverlog
C:\Windows\Temp\dwm-observer.log. Noobserverleftattached.
EXACTROOT: DWMpassedMILhardwaredevicetest, thenDDILockfailed8876086c;
flags84 NotifyOnly|NoOverwrite, INDEXBUFFERformat65,pool1,width1d4c8,
state54(unlocked,writeonly,shadow,isbuffer), noexplicitRangeValid.
CheckedD3D9assertlhddi.c5159, onlyOOM/DEVICEREMOVEDexpected, followed
MILintermediaterendererrorand88980406exit. Originald3d9disassembly
DdLockLH+2b9(RVAbd691)confirmsassertafterpfnLockreturn.
IMPORTANT correction toearliercommentary: thisspecificfatal isWHOLEINDEX
BUFFERlock84, NOTrepeatedbitmap82. RemovedincorrectNoOverwrite=>RangeValid
requirement; absentregionusesexistingwholebufferupload. Preserverangebounds,
flagconflictsandreadonlychecks. BotharchesbuiltPASS,signing89552pending.
MicrosoftDDIargsdocumentation listsflagconflictsbutnotRangeValidrequirement;
actualVistacheckedcallisevidence. NeedattachnewpackageandverifyDWMpastlock84.
CurrentCMD elevated,media8da,VM144360alive. NoWindowsbinarypatches,noagents.

Wholeindexlockfix55c00be913573888895b59fa18b6e309ea54185815731ffb29fc365661598726
installedverified07:05:01Z, publicPASS,nonce
c1549a995d92d98083c038240e1359d68dc846ccf2eb9f4a485d321eefbc657b,
Aerochild2840(desktoppid1456). Screenblack; securityroundtriprestoredbasic.
Copiedobserverguestlogto dwm-observer-8da.log thenranobserveragainbefore
UxSmsrestart. ObserverattachedDWM2244(8c4),completeEXIT88980406/DONE.
Capture neptune-5k7lmrxq/dwm-observer-55c.txt. NOwholeindexLockfailure now!
New exact D3D9error: Driver does not support D3DTEXF_LINEAR when shrinking.
StretchRect fails. ThenMILintermediaterendererror8876086c,renderthreadfailure.
Authoritativesource: triton9FillCaps StretchRectFilterCaps=0; triton9Blt in
triton9_output.c rejectsALL Flags.Value,unequalsrc/dstrectdimensionsanduses
CopySubresourceRegion only. MustIMPLEMENTlinear/pointfilteredstretch before
advertisingfiltercaps; donotjusttogglecaps. Needsactualscaledpixeltests,
thenDWMvisualproof. LikelyGPUshaderblitwithisolated/restoredpipeline; inspect
existingtransport/shaderhelperavailability. NoWindowsbinarypatchneeded.
Allbuilds/sourcecurrent,55cmediaattached; VM144360samecurrentrunalive.
Noobserverattached (DONE); alltoolssessionsended. ElevatedCMDcurrentfocus.
GoalstillrequiresrealvisibleGlassandtworebootpixelproofs,noneachievedyet.

2026-09-07 filteredStretchRect implementation: previousgoalturnprogress.
Added triton9_blit.c, perdevicecached deferredcontext,VS/PS,point+linear
samplers,immutableUVconstants. Reusesexisting triton/tritonBlitShaders.h
(sourceHLSLinrepo). Eachblit GPUcopiescroppedsourcetotempSRVtexture,
drawsfullscreentriangleintotempRTVtexture, copiesresulttodestinationrect.
Croptextureprovidesproperedgeclamping; snapshotsavoidselfoverlap/readwrite
hazard. Deferred FinishCommandList(FALSE) resetsrecorder; immediate
ExecuteCommandList(TRUE) preservesallcallerhoststate. Resourcesreleased
anddevicedestructionreleasescachedblitstate. NoCPUscalingfallback.
triton9Blt acceptsPoint/Linearflags(mutuallyexclusive),validatesrects, routes
unequalsizesfornonSYSTEMMEM/nondepthsameformatimages toGPUstretch;equal
rectskeepcopy. StretchRectFilterCaps advertisesmin/magpoint+linearwithimpl.
Added publicprobe6goldenpixelcases:4x4->2x2linear/point, interior2x2->3x3
linear/point,cropedges,destinationoutsideSentinel, postblitoriginalRTclear,
anddrawsame texturedtrianglewithoutrebindingtoverifyshaderstatepreserved.
BotharchesbuildPASS. Publishedsignedc18b9efd207499315c55daf1e6dfbc8a2730831676082d7cb73d2d5e7e6b0418,
attachedtoVM144360samecurrentrun. Observer39594pollsbytecommit45smax;
pixeltestsNOTyetexecutedatnote. NeedactualpublicpixelPASSbeforeclaiming
scalingworking,andthenDWMvisualproof/tworeboots. Noagents,noWindowspatches.

c18b installedverified07:14:53Z nonce7b01590d65a4b7ecb80344948191369ad882abeddf641f16799d4d1509a14875.
PublicprobePASS includesALL6StretchRectgoldenpixelsEXACT,notjusttolerance:
ff181818,ff787878,ff303030,ff484848,ff303030,ff606060; outsideff112233
unchangedinallcases. OriginalRTclearandpostblittexturedDrawbothPASS.
Evidence stretch-c18b-pixels.txt. ThusactualGPUdeferredcontext+shaders work.
Screenstillblack;Aerochild2884desktoppid1768started. Startingfreshbounded
DWMobserver beforeUxSmsrestart viaelevatedCMD, toolsession48634pending.
Oldobserverguestlogcopied dwm-observer-55c.log. Needretrieve newobserverlog,
identifyDWMnextfailure. CurrentVM144360samecurrentrunalive. Norebootrequired
forobserver; nohostreset. NoGlassclaimed.

2026-09-07 goal resumed after activation query (previous turn no Aero progress).
Revalidated QEMU144360 live. Read c18 observer: DWM3044/be4 no StretchRect
failure, then checked MIL assertion HwRenderStateManager.h440:
dwStage <= m_nMaxTextureBlendStage. Observer passes exception through,
DWM exits ff, DONE. Exact evidence dwm-observer-c18.txt.
Source advertised only2 fixedblend/simultaneoustexture/FVF coords. Expanded
actual fixed shader path to8: VS accepts/emits8UVs, PS stage loops8,
perstage texture uses reusable r1 (CURRENT r0), fog input moved from v4 to
v10 avoiding UVcollision. Statecallback allows8activefixedstages, common
TRITON9_FIXED_TEXTURE_STAGES8 constant. Capabilities now8.
Added public probeEightTextureStages: objectspace HWP XYZ+diffuse+TEX8,
8samples gray224 modulated CURRENT, independentexpected ff5a5a5a,
readback64,64, saves/restores D3DSBT_ALL, requirespixelPASS.
BotharchesbuildPASS. Signedpublished0af2c80fbe8213b304be92e440dcfa855fd0693be59ea9220d48901e5e00d54e
attached sameVM; guestinstall/publicpixelverification pending session81776.
NoGlassproof or tworebootproof yet. Noagents,noWindowsbinarypatches.

0af installedverified07:23:39Z noncea2876e12d3fc5f72a109c9ceff8eaf45199806cb93393a4c639139d0f9716bfa.
PublicprobePASS: eightstagepixelEXACTff5a5a5a, all6StretchRectexact and
postblitstatepreservationPASS. Evidence eight-stages-0af-pixels.txt.
Aerochild2788 desktoppid1592; screenblack. Observerrestart tool4102 pending,
oldguestlogcopieddwm-observer-c18.log, currentCOM1offsetobserver0af-offset.
Needretrievecompleteobserver and determine nextDWMfailure. Goalactive.

IMPORTANT0af aftermath: attemptedobservercapture4102 producedEMPTYCOM1;
screenshot showedWindowsErrorRecovery, commandsdidnotreachCMD. Guest
normalstartup nowrepeatedlyreturnstorecovery without hostreset. PolledCPU
registers boot realmode->kernel->realmode; normalcommit07:25:12 and07:26:00
happenedbutdesktopnotstayingup. CtrlAltDelattempt occurredbeforeloopwas
recognized; nofurtherCAD. F8interceptedrecovery; selectedSafeMode(up3Ret)
tool34912pending. Needcapturecrash/minidump, disableAutoRebootinSafeMode
forvisiblebugcheck. 0af observerEMPTY, donotclaimassertfixedyet solelypixel.
CurrentVM144360stillalive. NoGlassproof. Newbootfailureisnextdiagnostic.

SafeMode reacheddesktopstable. C:\Windows\Minidump contains4new268352byte
Mini090726-01..04.dmp at00:25/26/27. COM1notavailableinSafeMode; attempts
redirectreturnedpathnotfound, crash0af-events.txtEMPTY. Firstregcommand
wascreg(UACalt-cunneededSafeMode). RetypedcorrectlyandVERIFIEDregistry
AutoRebootREG_DWORD0x0 screenshotautoreboot-disabled.png. Guestshutdown/r/t0
issuednormalrestart tool24813 pending30s. Needinspectcurrent.png afterwards;
crashshouldnowremainvisibleandavailableforQMPread-onlymemorydiagnosis.
Noobserverattachedon0af; donotusec18logasnew. VM144360unchanged.

Normalreboot24813 finishedinBSOD, stableAutoReboot0. Screenshotcrash0af-bsod.png:
0x3b exception80000003 breakpoint,ntaddrfffff80001a23ed8,
CONTEXTfffffa600c8fea90. Capturedcrash0af-context.bin+stack.bin read-only,
QMPbriefstop/cont. Basekernel01867000 exactPDBmatched,boot-kernel-base.txt
andboot-crash-modules.json. dxgkrnlbasefffffa6003401000,watchdog350f000.
Stack returndxgkrnl+70c57 = DXGCONTEXT::SubmitPresent+5bf, watchdogassert.
Guestdisasm70c2f cmp[rdi],r13; JA skipsassert; failingassertline8a4;
nextcheckcompares[rdi]to r13+DMAcapacity. Thus pDmaBuffer didNOTadvance.
KMDsourceviogpu_device.cpp PresentFlip returnsSUCCESSwithoutDMApacket
(line~960), unlikeBlt directhostpathwhichwritesNOP. SubmitCommandREQUIRES
nonnullVioGpuCommand+SetDmaBuf fornonemptyranges, sofixmustallocatecommand,
writeNOP,storeprivatehandle,handleinsufficientDMA,notmerelyadvancepointer.
RetainSetVidPnSourceAddressauthoritativeflip latch; noearlyscanoutchange.
Needcorrectflipcommandallocation/lifetime/fencepathandvalidatecheckedboot.
NoKMDeditmadeforthisnextfixyet. VM144360crashedguestbutQEMUalive; preserve
crashforfurtherread-onlyevidence. Noobserver0af captured. UMD8stagepixel
verifiedbutDWMGlassstillunproven. Alltoolssessionsfinished.

2026-09-07 nextgoalturnprogress: fixedKMDPresentFlip emptyDMAassert.
VioGpuDevice::Present validatesDMAheadercapacity+1patchslot,allocates
VioGpuCommand,AttachAllocations sourceindex+1,SetDmaBuf, emitszeroedNOP,
sourcepatchmetadata,storesprivatehandle,advancesDMAandpatchpointers.
Errorsdisposecommand; sourceheldbusy throughscheduler. Keepsauthoritative
SetVidPnSourceAddresslatch. BothKMDarchesbuildPASS,signingPASSpublished
19a50f3845d35917b758c7bb01aa55b5bf7eab7e678b004c40fbb5cdb50b7fcc.
AttachedsameVM144360, reset ONLYterminalcapturedBSOD; F8andSafeModeup3Ret.
SafeModedesktopstable. ServiceSAFE_IDLEneedsownedattempt,soarm4registry
valuesAttemptId19a,InstallOutcomePENDING,ResultINSTALLING_SAFE,SafeModeOwned1,
then netstartTritonVistaDeploy. Tool26620pending. Immutablemediastillverified
byserviceinSafeModebeforecopy. Needactualnormalpostrebootbytes+pixelPASS,
DWMcrashabsenceandGlassproof. Noagents. Previousgoalturnprogress.

19a installednormalbytesverified,nonce035334520cbe1f19d398a283dae51d81c0f52fbc66b005b4e8defdb7f7b5cd99 publicPASS,
Aerochild2900desktoppid1624. DWMcompositionbefore/after1, proofpatternAPI
startedhold240sec, butactualscreenshotBSOD0x7e c0000420. Nopixelproof.
Newcrash19a-context.bin/stack.bin andbsodPNG inoldrunneptune-5k7lmrxq.
ntbase01810000 dxgbase3402000 KMD1ad3000. ExactassertExAcquireResourceExclusiveLite
+3c (nt+19be78 int2c) checksKPCRgs35deword DpcRoutineActive/DpcThreadActive.
CallerVidSchiSetFlipDevice+88(dxg22568) fromVidSchiClearFlipDevice+38 from
VidSchiProcessDpcCompletedPacket+4a0. DO NOT moveNotifyDpc toPASSIVE: official
callbackrequiresDISPATCH_LEVEL. Disasmcrash19a-dpc-disasm.txt showsclearflip
onlyifpacketstate13(vsnormal11),likelyfaultedpacket, sofindFIRSTdriverfailure.
KMDverbosewritesPORTe9 notserial, launcherhadNOdebugcon! KDprintbufferempty.
Added-debugconfile:kmd-debugcon.log,-globalisa-debugcon.iobase=0xe9 tolauncher.
HotaddattemptrejectedISAnohotplug; stoppedONLYterminalBSODVM144360 viaQMPquit.
qemu-imgcheckPASSzeroerrors, relaunchedsameoverlay19a normalvirtio4CPU,
newrunneptune-22gmrwll currentPID163048. Tool72069waitsnormalboot25sec and
capturesdriverdebugcon/screenshot. Mustreadkmd-debugcon.log fororiginalfault
beforechangingcompletion/DPCcontract. Allpriorcrashartefactsoldrunretained.

Currentrun22gmrwll debugcon captured. InitiallyREALdesktopwithglasslikeborder
andvisibleblur/artifacts WelcomeCenter, butNOcontrolledproof. UIeventually
BSOD0x1e c0000420 sameExAcquireResourceassert. VM163048terminalcrash.
Hostlog savedlatest/host.log. FIRSTCAUSALFAULT: Neptunecontext5 objecttable
unregisteredID0x2ce expectedtype49 ID3D11BlendState, cmdff004c1c=
ID3D11DeviceContext::OMSetBlendState. Hostdispatcherfatal killscontext5,
virtio207SUBMIT3D fails1200/fence2248, guestDMAschedulerfaultpathasserts.
Oneearlier10d/1203 invalidresourceSET_SCANOUT_BLOB atstartupretries; desktop
thenrendered. DoNOT"fix"DxgkCbNotifyDpcIRQL; faultisupstreamhostblendstate.
Source triton9CreateBlendState copiedsrc/dstCOLOR factors toalpha slots
when separatealphadisabled. DXVKNormalizeDesc rejectsCOLORalpha factors.
Implementedtriton9AlphaBlendFactor: SRC_COLOR->SRC_ALPHA,INV likewise,
DEST_COLOR->DEST_ALPHA,INV likewise,SRC_ALPHA_SAT->ONE. Applybothalphaslots.
Add goldencombinedblendcaseafter8stagepixel: SRC_COLOR/ZERO ADD,noseparate
alpha,expectedff202020 (90*90/255). BothUMDarchesbuildPASS; signing11754
finishednewpackage(checkdeploy-current). NOTdeployed/pixeltestedyet!
Nextattachnewmedia, resetonlyterminalBSOD, F8SafeMode, armowneddeploy4
registryvaluesasprevious(noteSafeModeCMDnoalt-cneeded), netstartservice.
CurrentVM163048withdebugconenabled, latest22gmrwll. Newpackagecarries19aKMD
NOPflipfix. NeedverifyblendpixelPASS, DWMcontrolledblur, twoGUESTrebootproofs.

Blendfixpublishedc1fb3999191c7127866dc83fe8f68b31d6af3a219f5c9cebe5c8d37b4d368bc1.
AttachedVM163048, resetterminalBSODandselectedSafeMode recoveryup3Ret.
Tool5086pending25sec; needinspectscreenbeforetypingownedattemptandnetstart.

c1fb installedVERIFIED07:49:21Z nonce8bcfdb223b72df1f2ba2657752e0b83ea6e59ccb777cdf548c12fe3273b3042a.
PublicPASS includes8stagesEXACTff5a5a5a +combinedSRC_COLORblendEXACTff202020,
allstretchcasesPASS. Evidenceblend-c1fb-probe.txt. Aerochild2904desktoppid1744,
UxSmspid1008, compositionAPIbefore/after1, controlledpatternhold240s.
ActualSCREEN nowvisiblepatternblue/yellowstripes BUTglassplateBLACKOPAQUE.
CapturegatepassivePNGglass-c1fb-first.png evidenceboundcurrentnonce/bytes.
VerifierhadPillowget_flattened_dataunsupported; switchedcrop.getdata(),
selftestPASSbluracceptBasic/transparent/decoy/undersizereject. ActualFAIL:
backdropsignalfalse,platecontrast0,sourcecontrast161.77. JSONsaved.
ThusNOGlassproof, no2rebootproof. CurrentVM163048on22gmrwllc1fb stillrunning
lastscreenshotnotcrashed. NeedobserveDWM currentprocess/debugtrace whyplate
opaque, checkhostlogfornewobjecterrors, not APIclaims. Toolsallsessionsended.
Currentdesktophasproofwindows/CMDparent, focusunknown. Noobserverstarted
c1fb yet. Existingguestobserverexe persists;copyoldlogthenUxSmsrestartwith
observerbeforestartwhenappropriate. CarefulQMPcapturegateabsoluteREALpaths
required(notlatestsymlink). COM1availableNORMALmodeonly. Noagents.

2026-09-07 goalturnprogress: c1fbcurrentVM163048live. StartedboundedDWM
observerbeforeUxSmsrestart,capturedcurrentdwm-observer-c1fb.txt fromCOM1.
DWMthread2984 repeatedlyPresentsSUCCESS, noassert atcapture butblackscanout.
Hostcontext13newcreation, onlySET_SCANOUT_BLOB10d/1203startupinvalidresretry.
Rootnewfinding: Vista capsFlipCaps.Value0 =>DMAflips, but PresentFlip NOP
hadnoscansourcecompletion, assumingSetVidPnSourceAddressdoesallflips.
OfficialDxgkDdiPresent docs confirmSetVidPnSourceAddress executesMMIOflips;
DMAflipmustencodeactualflip. SourcecapscommentswrongaboutVistaonlyblts.
ImplementedDMAflipretirement: PresentFlip SetScanoutSourceCompletion(src,
generation,TRUE), prepatchsrcm_SegmentAddresswhenSegmentIdnonzero; Patch
updateslatestaddressasbefore. VioGpuCommandnewbooldmaFlip initializedFALSE,
completionpassesflagtoVidPN; publicSetScanoutSourceIfGeneration overload
usescurrentallocationSegmentAddressforDMAflips,zero/preserveforhostBlt.
Modegenerationguardsremain,sourceheldbusy/owneduntilretirement,actualflip
threadflush+vsyncreportsdisplayedaddress. Noearlytranslationlatch.
Firstbuildfailedmisseddefinitionarg,corrected; bothKMDarchesbuildPASS.
Signed987cfc4c2fb1daefce4792674449ef16e57de181ceeb840830003398aec47d7f
attachedVM163048same22gmrwll; serviceinstallpoll62344pending45s. NOTyet
verifiedinstalledorGlass. NoresetforhealthyblackVM. Observerbounded120s
mayfinished; fullendlognotyetretrieved. c1fbproofwasblackFAILEDvisual.

987installedverified07:56:18Z nonce593634860d399fcf2afeaeee415471483b45eb36029f37d1e39c8b0417e510f0,
publicPASS/Aerochild2992desktoppid1656composition1,proofplateSTILLblack.
RestartedUxSmswithboundedobserver. dwm-observer-987.txtpartialongoing:
DWMthread904PresentsSUCCESS noerror exceptexpectedinitialattachbreakpoint.
AfterrestartACTUALbordertranslucencyvisible(CMDandWelcomeCenter) butghost/
greenartifacts; screenshotdmaflip-987-border.png. Controlledplatewascovered
byCMD, donotclaimproof. Newharnessbugfound: WM_PAINTonlylabel,neverinitializes
clientalpha, WM_ERASEreturns1; docsrequirezerotransparentblackpixels for
extendedframe. AddedFillRectBLACK_BRUSH entireclient beforelabel inproof
WM_PAINT, perMicrosoftcustomframe example. DoesNOTsimulateblur/backdrop,
onlycorrectlyinitializeswindowpixels. BothprobebuildsPASS; newpublished
801143dc9b47c6d5689e23342185f54dbd9f379a455da47f6c07ee3ee5ee9f36
attached163048same22gmrwll. Serviceinstallpoll31292pending45s. Need actual
firstproofPNG afterinstallednonce/publicPASS/newscene, verifier metrics;
then2guestownedrebootproofs. NoGlasssuccessyet. Noagents/noWindowspatches.

2026-09-07 goal resumed after activation response (previousgoalturnnoAeroProgress).
RevalidatedPID163048alive same22gmrwll. 801installedverified07:59:46Z,
nonce2e7b5d3745d98f7a82c3c360539a373285b0c63c2080afce02ff8611e78be892.
PublicprobePASS butAerochildFAILED, composition0/80263001 both120sec
enableattempts; WinSATexit0. Actualscreenblack; CtrlAltDel/ESCresponsive
restoredBasicdesktopwithoutreset. Evidenceaero-801-failed.txt copiedCOM1.
RestartedUxSmswithboundedobserver: translucentbordersbutduplicated/displaced
content; eventuallyDWMexited88980406. Completecausaltrace
vista-kvm/latest/dwm-observer-801-full.txt: tid2312(pid0xa00) LockINVALIDCALL
8876086c flags98 (NotifyOnly|Discard|RangeValid), state54 (shadow,writeOnly,
buffer), SYSTEMMEMpool1,VERTEXDATAformat64,width9c420,rangeoffset0size3400.
D3D9lhddi.c5159assert thenMILfatal. Existingtriton9Lock explicitly rejected
systemMemory&&Discard. Removedthatban, preservedflagconflict/boundschecks.
RuntimeownsCPUstorage; existingorderedUpdateSubresourceatUnlockuploadsrange,
noCPUrename/readbacknecessary. MicrosoftLock/LOCKFLAGSdocsconfirmedNotifyOnly
runtimeownsbacking andDiscardpermitsignoringoldcontents.
BothUMDarchesbuiltPASS, signedpublished46896ab6af80387d1c354cb33f334aed0eba187a02156891798d770a27d7a1fe.
AttachedsameVMviaQMPchange-medium; installpoll67435pending. Needverified
install/publicPASS andactualGlassproof; stillNONE. ManualdiagnosticlaunchD:
failedfilemissing(probablyCDanotherletter); nofakeproofnonceusedsuccessfully.
GuestcurrentlyelevatedCMD, observerDONE(exitedDWM), Basicdesktop. Noagents.

468installedVERIFIED08:08:47Z nonce2fa657c9b31163d635cf59207438c87c90f0736b5af9ce1b872f253e3a4e333f.
PublicprobePASS/API1/newscene. Gatedglass-468-first.png/json actualFAILopaque
platecontrast0,source161.77. DWMalreadyexitedbeforeobserverattached(WAIT).
StartedUxSmswithwaitingobserver, successfulPresents/visibletranslucentborders
butdistorted/duplicatedcontent. Completeobserverdwm-observer-468-full.txt:
newfirstcausalSetTextureINVALIDCALL8876086c, thenfallbackTSSstage0state18hex
(TEXTURETRANSFORMFLAGS) value2(COUNT2) NOTAVAILABLE8876086a, MILfatal88980406.
NoLock98failureinthistrace; notclaimfullregressionproofwithoutexactlockseen.
SetTexture disallowsresource==currentRT atsettertime; mayberuntimelegalstate
ordering butneedarguments. Addedfailure-onlystage/IS-RT/format/pool/bindtrace
in triton9_failure_trace.h, NOTYETBUILT/SIGNED/DEPLOYED. Currentpackage468still.
VM163048alive, Basicdesktop afterDWMexit, elevatedCMDopen. ObserverDONE.
Alltoolsessionsended. Needbuildbotharches/sign/deploynewdiagnosticthenDWM
observeexactSetTexturecause. NoactualGlassproof/nosecondrebootproofyet.

SetTexturediagnosticbotharchesbuildPASS, signed44e279f194ba53be5ac8e1336dc92a8bf6dc5cf4448a6b4ac07eafd6db782491 attachedsameVM163048. Needverifyinstalled, thenobserverSetTexturefailurearguments. Goalstillactive.

2026-09-07 currentgoalturnprogress: 44einstalled08:14:54Z nonce
 df79da71dfd96ca3a598f4bc8ce9ea68d269dea5b594ee73105321786db79f45.
PublicgateFAILEDthisbootHWP+PURECreateDeviceE_OUTOFMEMORY contextcreate;
Aerochildskipped. EarlierstatusDDIfailtailcontainsold468eventswithoutnewfields;
donotattributehistoricaltailto44e. RestartedUxSmsobserver44e, DWMrunning.
ManuallylaunchedC:\Windows\Temp\TritonD3D9Probe_44e279f194ba.exe
--aero-probe --result-nonce44e0000000000000000000000000000000000000000000000000000000000001.
Actualglass-44e-manual-translucent.png showsbackdropthroughplate, butedges
sharpanddisplaced. OLDverifierfalsepositivePASS:source161.77,plate132.14,
edge83.67. Strengthenedverify_aero_glass_pixels.py withnormalizedmaximum
singlepixelgradient aroundeachstripeedge+/-12 (avoids tint/shiftfalsepass),
requireaverage<=.35. Actualsharpness.517->FAILblur,signal/attenuationtrue.
Addedtintedanddisplacedsharpnegativemodels; selftestsPASS. NoGlassproof!
Observerdwm-observer-44e-full.txt boundedDONE/DETACHresult0, noSetTexturefail
inthatcapture. Startednewobserverwithoutrestart, dwwthread2928 now.
GuestUIcommandsdelayed/droppedfirstcharacters ifimmediateafterESC; inspect
visibleCMDbeforeassuminginputexecuted. CurrentelevatedCMDstartnotepadpending
thenaltF4attempt; queuedtypeobservertoCOM1. NeedpollspecificVM163048stilllive,
readCOM1/currentPNG thennewobserverfailure. Noagents/nobinarypatches.

44eUIobserverreproducedFIRSTSetTexturefailure: dwwpid530 tid2928 stage0,
IS-RT=1 format15(A8R8G8B8) pool2 DEFAULT bind28(RTV|SRV). Fulltrace
 dww-observer-44e-ui.txt (actualfilename dwm-observer-44e-ui.txt) contains
SetTextureINVALIDCALL thenfallbackTSS_COUNT2failure/MILexit88980406/DONE.
RemovedprematureSetTextureRTconflict rejection; retainedPreparePipelineState
feedbackcheckatdraw. Setteronlystoresstate/nohostSRVbind. Addedpublic8stage
pixelteststateorder: SetRTtexture,Cleargray,SetTexture8stages,SetRTtarget,
thenexisting8stagegolden90+blendgolden32. RestoresRTalsoonfailure.
BotharchitecturesbuildPASS. Signsession75521pending/resultinspect.
TemporaryQMPhelpernowcheckssendkeyHMPreturnederrorratherthanignore;
uppercaseF4wasinvalid! usealt-f4 lowercase. Notepadclosedwithoutsave.
44emanualproofNOTgatedandsharpnessFAIL. no2rebootblurproof.

da8833ebff7f0faad19b307e38a5b2d19a9678f2fd94558b27e2e59b4c9b4059
published/attached/installedVERIFIED08:25:26Z nonce
2d50fdf0defbc02c7a34f9482a3316e48517b72b154f2cebc25e2064ba9f4063.
SameVM163048same22gmrwll, securechild2564. Probe/scene poll7493pending35sec.
Needinspectresult thenpassivegatecaptureda8first withnewstrictverifier.
Noactualblurproofyet. Observerold44eendedwithDWMexit; nonealiveafterboot.

da8probecompleted: newTexturebindingbeforeoutputswitchPASS,8stageff5a5a5a
andcombinedblendff202020 EXACTPASS. FullgateFAILstrictHWP+PURECreateDevice
E_OUTOFMEMORY (sameas44e), baseHWPdiagnosticsPASS,Aeroskipped. Evidence
probe-da8.txt newestcommitsegment, bewareappendedDDIhistoricaltail.
triton9EnsureKernelContext in triton9_ddi.c890 invokeszeroed
D3DDDICB_CREATECONTEXT/pfnCreateContextCb ->8007000e. KMDCreateContext
 driver.cpp1229 allocatesVioGpuDxContext nonpaged, advertises1MB DMA,
1024allocation/patchentries. Noerror-specifictrace yet; needdiagnoseactual
callbackfailure ratherthanrelaxpuregate. CurrentVM163048alive/same22gmrwll,
noobserverrunningafterboot. Alltoolssessionsendedexceptold81880poll? likely
completed30sec, inspectonlyifneeded. No2bootproof. Lastdriverda8installed.

2026-09-07 goalturnprogress: revalidatedVM163048alive/same22gmrwll.
Investigatingda8strictpureOOM. Addedoptional--launch exe args to
scripts/vista_dwm_observer.c: DEBUG_ONLY_THIS_PROCESS capturesstartup;
bounded120sec, passesapplicationexceptions, KillOnExitFALSE; launchmode
writesruntime-observer.log inCWD (ordinaryuser), defaultdwmobserverunchanged.
Builtx64runtime-observer.exe utilityISO runtime-observer2.iso, attachedE:,
launchedE:\runtime-observer.exe --launch E:\runtime-probe.exe --secure-probe
--result-nonce da80000000000000000000000000000000000000000000000000000000000001
fromNORMALuserCMD C:\Users\Triton. ThisisDEFAULTdesktopdiagnosticNOTgated.
FULLPUBLICPASS: HWPandHWP+PURE BOTHsuccess,8stage/binding/blend/stretchallPASS.
Evidenceprobe-da8-default.txt extractedlatestnonceCOM1afterserialtransfer.
Observerruntime-observer-da8.txt reachedDETACHresult0/DONE, noOOM; checked
D3D9dxgcreat.cpp6428 assertionPoolDEFAULT appears (noapplicationexception).
ThusOOMnotpermanentpureprofileincompatibility; maystartup/securedesktop.
RestoredattachedISOda8 deploy-current. ActualDWMwentBasicduringdiagnostics,
Vistaofferedperformancecolorschemeprompt; intendedkeepcurrentnobasicchoice.
Needservicefreshsecuretestaftersettled: elevatedCMDcurrentlyreg add
HKLM\SOFTWARE\Triton\VistaDeploy ProbeLaunchId=PENDING entered; tool54449
pending/resultinspect thennewnonce/publicgate. NoWindowsbinarypatches/noagents.
QMPhelper /tmp/vista_setup_control.py nowkeyhold50ms/sleep80ms/checkHMPerrors;
criticalUIstatecanlag>10secs, inspectbeforetyping; F4mustlowercasef4.
UAC: Left thenReturn fromCancel reliablyContinue. CDletterE:, autoExplorer
stealsfocus. COM1transferalsoasynchronous; toolread2sec canemptybutlogarrives
later, pollwholefilebyfreshnonce/markerbeforeclaimmissing.

da8freshSERVICEreprobeafterdesktopsettled nonce4e8fc42f392326dc931c31d4ea4cf4d683ccfed50d31e6640345a37e05209c70 FULLPUBLICPASS includingpure.
ThusstartupOOMtransientalsosecuredesktop(notonlyDefault), notpermanent
pureunsupported. Aerochild228 initialcomposition0 despiteenableS_OK; started
boundedoldDWMobserverandnetstop/startUxSms fromadmin; childtransition1/scene.
SceneobscuredbyExplorer/consoles; manualminimizewindowsultraslow UI>DWMlag.
NoPNGblurproof. Updatedharness: bothproofwindowsWS_EX_TOPMOST soautoopened
media/consolescannotocclude boundedscene; DWMstillprovidesallblur. Pure
CreateDeviceprofilebounded6attemptsONLYE_OUTOFMEMORY5secbackoff, logsall
retryHRs, finalstrictsameprofile+allpixeldrawsrequired, anyotherfailureimmediate.
BothprobearchesbuildPASS; signstartedpendingtoolresult. Needpublish/install
thenautomaticfirstproofwithstrictpixelsharpnesschecker. Noagents.

Published1ac915b8add6c89b60c823ca161574236685a77511767a7d6bfcc7a3ff66900c
attachedsameVM163048/same22gmrwll. ServiceREBOOT_TO_SAFE_MODE08:46:34Z.
Installpoll11769pending45sec; inspectcompletionstatus thennewnonce/public
retrycount/scene. OldDWMobserver bounded120 nowrebootwillterminateifnotdone.
Lastactualblurproofnone; strongerverifierstillrequirednormalizedsharpness<=.35.

1acinstalledVERIFIED08:47:14Z noncecfd14a7f9dd592f5e2d3ce2be103089e3db6e7fe1544af641fe290d674fb7edc.
FULLPUBLICPASSincludingpureFIRSTATTEMPT(noOOMretryusedthisboot). Automatic
AeroAPI1/scene, topmostvisiblewithoutmanualUIintervention. Gatedpassive
 glass-1ac-first.png/json captured, actualbackdroptranslucentbutBLURFAIL:
sourcecontrast161.77 plate132.14 edge83.67 normalizedsharpness.517>.35.
ThisisfirstautomaticvisibletranslucencybutNOadequateblur. Evidenceprobe-1ac.txt.
CurrentVM163048alive same22gmrwll; scenehold240sec thenserviceownedsecond
rebootexpected (doNOTclaimtwoGOODproofs). Alltoolsessionscompleted. Need
nextdiagnoseblur actualshader/filter/sampling orWindowsDWMblurconfiguration;
noBlurregistryoverrideinnotes search. DWMtitlebaralsoflipped/ghostedcontent.
Noagents/noWindowsbinarypatches. Goalnotachieved. Utilitymediarestored1ac.

2026-09-07 currentgoalturnprogress: VM163048alive22gmrwll1ac. Blurstill.517
sharpnessFAIL. Foundconcreteconstantownershipbugs in triton9_shader.cpp:
triton9ApplyInlineConstants overwrotecanonicaldevicefloat/int/boolregisters;
fixedfunctionuploadvertex/pixelalsooverwroteglobalruntimefloatconstants.
FixedinlineDEF/DEFI/DEFB via4096byteuploadscratchcopy, leavecanonicaldata
unchanged, markruntimebufferdirtyafterinlineupload so nextshaderrestores.
FixedfunctionnewdevicefixedVertexFloatConstants/fixedPixelFloatConstants;
fixeduploadfunctionswriteprivatebuffers, BindStageConstants selectsprivate
slot0onlyforactualfixedshaderobjects, extendsReleaseConstantBufferscleanup.
Publicregression aftertexturedSM2: setruntimec0once, alternateinlineDEFconstant
PS(ff4080bf) andtexturedruntimePS(ff603020) twice; thenfixedPS_TFACTOR(ff204080)
andreturnruntimePS(ff603020) withNOnewSetPixelShaderConstantF. All6pixels
mustmatch. BotharchesfullbuildandABIcompilePASS. Initialinline-onlyab55
publishedbutNEVERATTACHED; fullfixed+inlinepackage signingpendingtoolresult.
Needdeploynewfullpackage/verify6pixelsandallpublicPASS thenactualGlasssharpness.
Noagents/noWindowsbinarypatches. Currentbaseunchangedworkingoverlay.

Fullconstantfixpublishedc8c0b9c05894136393076dc8ffc95e462e168f8c361d0f5ecf42146bbedfac25
attachedsameVM163048. ServiceREBOOT_TO_SAFE_MODE08:56:33Z. Installpoll9691
pending45sec/resultinspect. Prior1acfirstactualblurfailed. Needc8c6pixelpasses
thenactualgatedPNG blurmeasurement (notjusttint). Newfixedbufferscleanup
locatedReleaseConstantBuffers4048; ABIcompilebothPASS. ab55neverinstalled.

2026-09-07 goal continuation: preceding activation reply no Aero progress.
Revalidated live QEMU163048 same22gmrwll. c8c installed; all6 shaderconstant
pixelsPASS plusfullpublicPASS nonce09a3c3c90931c7f45521144afc28a9ed8892e01312970d461c3e82d683a5c957.
Gated glass-c8c-first.png actual unchangedblurFAIL sharpness.517, contrast132.14.
Serviceownedsecondboot09:01:59+POST_REBOOT_COMMIT09:02 nonce
d53cdcb80935bdd8815cb23e1cb66f1f798446bf94b7fa02518967875df26b5a also
publicPASS+APIscene. Two-passcaptureREJECTED missingREPROBE_LAUNCHED.
Foundtelemetryownershipissue: probe g_telemetry holdsCOM2, deployemit_status
openssameportpermessage; launchmarkerafterStartServicecanbelostwhileprobe
holdsport. Durableguest C:\Windows\Temp\triton-deploy.log maycontainit.
DoNOTweakenfinaltwo-bootproof; needfixtelemetryorvalidateauthoritative
nonce/start/child/publicorder withdurablelogs. Noactualblurproofyet.
AddedboundedtemporaryDWM-onlyshadercapture in triton9_shader.cpp:
triton9CaptureDwmShader recordslegacytokens,convertedDXBC,linkedVSlegacy,
PID/serial/epoch/stage in C:\Windows\Temp\triton9-dwm-shaders-PID.log.
Onlyactualconversion, max64records/4MiBperprocess/64KiBperblob; failures
ignoredforrendering. BothUMDarchesbuildPASS; signedpublication
f8d6e6220150489fea4bdb45ca21f87cfb35df56a186150bd0d831258e7d591d attached.
Needverifyinstalled then retrieveDWMshaderlog viaCOM1 andinspectactualblur
shader/convertedinputmapping. Noagents/Windowsbinarypatches. Goalactive.

BREAKTHROUGH f8d DWMshadercapture: installed09:05:26 firstnonce1d11605b34ddfe14006cc4a85f5f81ffc852ef70adc81e8a3f6f23d936630dc7;
secondguestboot09:10:08 noncefb1e49a20e169207ed73a42bd61b8def8e39d402175f73494dd56c9ceb30afd5 fullpublicPASS.
Ordinaryuserexport AccessDenied; elevatedCMD Startcmd CtrlShiftReturn,
actualUACLeftReturn, then type C:\Windows\Temp\triton9-dwm-shaders-*.log
> \\.\COM1 2>&1 SUCCEEDED38completerecords fromDWM1516/1576.
Parsed/disassembledin latest/dwm-shaders-f8d. Hosttoolbuiltusingbuilderg++
from triton-dxvk/subprojects/dxbc-spirv/tools/dxbc_disasm.cpp linkedexisting
build-linux/subprojects/dxbc-spirv/libdxbc_spv.a, output
 test-artifacts/linux-build/dxbc-disasm (runpodmanexecbuilder /workspace/...)
Include build-linux/subprojects/dxbc-spirv/tools for ../config.h resolution.
ACTUAL1516-ps-9-1-legacy.txt DWM8tapblur: texldusingt0,t1,...t7 withweighted
sum. Converted1516-ps-9-1-dxbc.txt declaresONLYv3.xy and ALL8samplesv3.xyyy!
Root: ShaderConv::RasterStates defaults TCIMapping=0, UpdateInputDecls for
SM<3 mapsallsemanticsTC0. Driverneverinitializedmapping. Shaderconstructor
nowsets TCIMapping=ShaderConv::TCIMASK_PASSTHRU (0x76543210); onlyidentity
TEXCOORDINDEXisacceptedbydriver, so consistentwithsupportedstate.
NewpublicprobeSm2DistinctCoordinates: fixedVS8FVFcoords, realPS2eighttexld,
8x1texturegray0,32,...224, eachUVuniqueconstanttexelcenter;average8 samples
expectedff707070; collapsedTC0wouldblack. Stateblocksrestored/resourcesreleased.
BothUMD/probearchesbuildPASS.
Durableguest triton-deploy.log exportedviaelevatedCMD toCOM1 confirms
REPROBE_LAUNCHEDc8c09:02:00/f8d09:10:08exists butlostserial. Fixedbothowned
servicesCOM2writes: sharedGlobal\TritonVistaTelemetrymutex +exclusiveopen
andcloseperwrite, noheldprobehandle. GateUNCHANGED. Sourcesprobe.c and
 test-artifacts/vista-driver-deploy-service.c. BuildbothPASS, sign/package
currentlypending13422 log/tmp/vista-sign-tci.log. Needattachnewpublication,
check8coordinatepixel +fullPASS thenactualblurPNG/two-rebootgate.
VM163048healthy same22gmrwll; elevatedCMDopen, currentf8dsecondproofscene.
Noagents, baseunchanged, goalNOTcomplete. Shaderdump instrumentationretained
boundedpendingrealvisualverification; removeonceblurdefectresolved.

TCIfix+8coordinatepixelregression+serialmutex package351af10358f4fed67fc2418157b2947427ea23710e76ad3f7fe7f699461ffe6d
published/signedauditPASS, attachedsameVM163048. ServiceREBOOT_TO_SAFE_MODE
09:17:40Z. Installpoll4293pending35sec. Needverifyfreshnonce/publicpass,
SM2eightdistinctcoordinatesff707070, thenactualgatedPNGblur. Shaderdump
retainedinthispackageforconfirmation; removeafterfirstrealblurproofbefore
finaltwo-bootverificationifpossible. Durablelaunchmarkersnowexpectedserial.

351af installed09:18:20 nonce66fb96b647bb097341efe8b91159940ca49821b962496eac697ec87b1308b7ef.
SerialPROBE_LAUNCHEDnowarrivesPASStelemetryfix. New8coordtestINVALIDCALL
beforepixel;gatecorrectlyskippedAero. AddedCOORD_CHECKfailure#calllogging
package0b137a14426a8af28262ff29179e735967d5dbc7ef5c1e1005ada85336becf6e
installed09:21:22 and CONFIRMED CreatePixelShaderINVALIDCALL.
OurtestusedMULdirectoC0 illegalSM2 (Microsoft OutputColorRegister doc
https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx9-graphics-reference-asm-ps-registers-output-color
requiresMOVonly). Correctedtest MULr0 thenMOVoC0r0, allother8tapsunchanged.
Bothbuilds/signauditsPASS; newd417f8cec7890b6f22d078dfd7874c63199129977c000dc09de0ea4fd53d6b7e
attachedsameVM163048. Installpoll35024pending45sec. Needfresh8coordpixel
PASS/fullpublicPASS,thenactualblurPNG. TCImappingfix unchanged since351af.

d417 installednonce8c645f3a9573114718ce83eb77cb9720cae65fd1fc1306824938df1978346176;
stillCreatePixelShaderINVALIDCALL. SecondSM2testbug: reusingr1asTEXLDdest7times
countsdependentreadorders (>3limit); Microsofttexld-ps2docconfirmsdestreuse
countsasdependence. Corrected8samplesintor0..r7FIRST, thenADDsumr0+r1..r7,
MULr0weight thenMOVoC0r0. MatchesDWMindependentsampleregisters, keepssame
8distinctcoords/expectedff707070. BothbuildsPASS +signauditPASS, published
1147e8377b773337473802972e32a6823d840d3d966dfc8792913a6d52fb1cdc attached.
Installpoll71362pending45sec. Allprevioussessionscompleted. VM163048live.
ActualAeroTCImappingfixawaitsvalidnewtestbeforevisualgate; donotclaimblurPASS.
Hoststrictpixel/gate--self-test bothPASS thisturn.

1147 installed and FULLPUBLICPASS including NEW8distinctcoordinatepixel
EXACTff707070, sixconstantpixels/allpriorregressionsPASS. probe-1147.txt saved.
Gatedglass-1147-first.png/json ACTUALVISIBLEBLUR nowpresent! normalizededge
sharpness .199 (was.517), edges_blurredTRUE, plateedge107.49/source161.77.
OverallstrictverifierSTILLFAILS separatecontrastattenuation: plate133.23 /
source161.77=.8236, currentarbitrarycutoff.82. Realimage visiblyblurredstripes
andghostedtitletextnowblurred. DoNOTclaimfullgoalcomplete: needreviewcontrast
criterion basedonactualrequirement/themes (notblindlyloosencutoff), strict
negativecontrolsshouldremain; thenfinalsamepackage2bootproof. Servicewill
rebootafter240sec currentfirstscene;secondshouldnowincludeREPROBE_LAUNCHED
becausemutexfix. Needcapturetimely; noUIinterferencecurrently.
Shaderdump instrumentationstillretained1147; removeafterconfirmeddiagnosis
ifdesiredandrerunfinalpackage2boots. ActualrootTCIMappingfixverifiablefrom
DWMold8texld allv3 mapping andnew8coordpixelPASS. NoWindowspatches/noagents.
VM163048same22gmrwll alive. Allsessionscompletedexcept48188pollmayalready
complete; checkifneeded. Goalactive, NOTblocked. Latestcommentaryreported
realblurbutoverallvisualgatecontrastfail andremainingtwo-bootverification.
TRITON9-RUN nonce=f8a567efa3e3eb4e0c46172c96ac3483d4f28084652f2db0eb85111137be3188

2026-09-07 continuation: previousgoalturnPROGRESS(rootTCImappingfixed,
real8coordinatepixelPASS,visibleblur). Revisedvisualverificationcriterion:
oldplateaucontrast/raw<=.82measuredtintstrength, notblur; wideplateaus
remainunderblur. Nowedgecontrast/ownplateaucontrast <=.90*(sourceedge/
sourceplateau), withunchangedlocalnormalizedsharpness<=.35. Keepsbackdrop
correlation+independentsharpedgechecks. 12Gaussianradius3/5/7 x tint0/.12/.32/.6
positivemodelsPASS, allBasic/transparent/tinted/displaced/decoynegativesFAIL.
Sameverifier oldc8cfirstFAILsharpness.517; corrected1147firstPASSsharpness.199.
Reports glass-{c8c,1147}-first-review.json saved; earliernotesclaimingfirst.json
wereinaccurate (capturetoolonlywritesPNG). RemovedtemporaryDWMshadercapture
fromUMD, keptsourcefix and8coordinateregression/serialmutex.
Finalpackageea971975b1d65c3fc7a59f52d66e552c38ac26d79746062cb82052569e17c261
bothbuildsPASS/signauditPASS attached;installed09:29:13firstnonce
5239a53b0551fe71f0bb240aab84799895a56d063c92ea1ca470c803e7f87172.
FullpublicPASS/PURE/sixconstants/eightcoordinatesff707070PASS.
Watcherfirstcapturetooearly(noactualpatternyet): glass-ea97-boot1.png/json
correctlyFAILandPRESERVED. Settledcaptureglass-ea97-boot1-settled.png PASS
actualblur. Saved matching.json withmetrics/script/PNG/statushashes and
.status.log; previewedvisuallycorrect. Boot1realsharpness.199, plate133.23,
edge107.49, sources161.77. NoUIinputforfinalpackage.
Watcher /tmp/watch_vista_glass_final.py resumed ONLYboot2, waitssecondcommit
+publicPASS+scene then30secs foractualdisplay, invokesUNCHANGEDminpasses2
gate, strictpixelchecker, savesglass-ea97-boot2.{png,json,status.log}.
LIVEexecsession93515 waiting; VM163048same22gmrwllhealthy. Needpollwatcher,
previewsecondPNG, independentlyvalidate2bootnonce/hash/rebootordering,
writefinalverificationreport, thenonlymarkgoalcompleteifallPASS. Goalactive.

FINAL GOAL EVIDENCE: ea97 secondbootVERIFIED09:33:50 UTC; guestownedreboot
09:33:29, REPROBE_LAUNCHED09:33:50nowpresentserial. Nonce32b2e76c34129d3fe74f86d6a87f5fd6ba3aeb073dfbe85c85ca518ccefd5f9a.
FullpublicPASS,6constantsPASS,eightcoordsff707070PASS; gatedsecondPNG
glass-ea97-boot2.png strictvisualPASS sharpness.199, samefirstsettledmetrics.
Previewedbothactualimages. Independentaudit rehashedPNG/log/script, reran
verifier, unchangedcapturegate2bootorderingPASS, bothbootsfourinstalledfile
hashesmatchpublishedtree. Saved notes/AERO_GLASS_VERIFICATION.md and
latest/aero-glass-verification.json + final-package-audit.log. Noagents,
noWindowsbinarypatches, temporaryshaderdumpremovedfinalpackage. CurrentVM
163048same22gmrwllhealthy, secondproofwindowauto-closesafter240sec. Watcher
93515finishedexit0TWO_BOOT_GLASS_PASS. Alltoolsessionscomplete. Goalachieved
forcurrentVistax64VM; finalgoalstatusupdatefollowscompletionaudit.

User requested native QEMU window. Added VISTA_DISPLAY=gtk|egl-headless and
VISTA_ACCEL=kvm|tcg options to scripts/run_vista_neptune_linux.py. GTK uses
X11 socket/Xauthority mounts and gl=on; omit VNC when GTK GL is enabled
(QEMU rejects GTK GL+VNC). Literal TCG attempt blue-screened early STOP7E/
80000003, no diagnostic logs. User explicitly approved restoring KVM while
keeping native window. Exited terminal BSOD via QMPquit; qemu-imgcheckPASS.
Started VISTA_DISPLAY=gtk VISTA_ACCEL=kvm python3 scripts/run_vista_neptune_linux.py.
Current live PID226490 runneptune-t_wc_4ai; finalea97publicationsamebytes
verified11:55:55UTC. NativeGTKdesktop+Glass visuallyconfirmed viaX11window
capture latest/native-window.png. QMPscreendump underGTKGL staysatbootlogo;
doNOTmistakeitforstalledguest. xprop-rootclientlist ->QEMUwindow0xa00007,
ImageMagick import-window capturesnativewindow correctly. UserisusingVM;
donotinjectinput/reboot. Priorheadlessproofartifactsremain22gmrwll.

2026-09-07 userreportedslow+mousecapturefailure;explicitdisableunnecessary
logging. Activegraphics sampleea97native: QEMU127.5%aggregatevCPU andE9
370617bytes/sec; idlelatersample7.07%CPU/219Bsec, soNOTcontrolledbeforeafter.
SourceInitializeDebugPrints nowVista defaultsTRACE_LEVEL_WARNING andvirtio
level0; retainsdiagnosticerrors, expensiveverboseopt-inVIOGPU_VERBOSE_TRACE.
Added scripts/build_vista_kmd_linux.py --verbose-trace. BothKMDarchesbuild/
PEPASS. Signedpublicationb6761e646d46a1bb789181deecae5b98774be448352cf52bcca0a0d2b3496119
attachedt_wc_4aiPID226490. USBUHCI+tabletinitiallyhotplugged(currentfalse);
addedpersistent -devicepiix3-usb-uhci,id=vista-usb andusb-tablet busvista-usb.0
launcher. NativeGTKpreferredexplicit VISTA_DISPLAY=gtk VISTA_ACCEL=kvm.
ServicehadrequestedSafeModereboot11:59:12buttelemetrywasdelayed; Ialsoissued
ACPIpowerdown beforeseeingit. VM STOPC000021A criticalsessionprocessduring
shutdown. Userreported; savedt_wc_4ai/bsod-mouse-shutdown.png +existinglogs.
CauseNOTproven; don'toverlapownshutdownwithserviceownedrebootsagain.
QuieterdriverNOTinstalledatcrash. QuitterminalBSOD;diskcheckPASS, relaunched
KVMGTK withUSBfromboot. CurrentPID228918/runneptune-38lg5vfl,latestthere.
Boot~100sec thenserviceinstalledb676 VERIFIED12:03:52KMDhash
53a324d3bcc09b83bf7b8b69481f6a96df3b89af6f9b8b6618758f716db0ee48.
FullpublicPASS incl8coordsff707070; AEROAPI1+actualnativeGlassproofvisible
quiet-desktop.png. query-miceNOWactiveQEMU HID TabletabsoluteTRUE,PS2inactive.
After15secsample13.33%QEMUCPU/0E9bytespersec; startup+testdebuglog65KB.
Don'tclaimFPSspeedupfromnonidenticalworkloads. performance-after.jsonsaved;
previousbaselineperformance-before.jsonint_wc_4ai. NativewindowXID0xa00007
captureviaimport-window works;QMPPNGstaleunderGTKGL. Usercanuseabsolutemouse
withoutcapture. Servicefirstsceneholds240sec thenoneautomaticverification
rebootremaining; needobservehealthysecondbootwithoutmanualshutdownrequests.
Alltoolssessionsdoneexceptold31453pollmayalreadyfinished. GoalGlasspreviously
complete, thisislogging/mousefollowup. Noagents/noWindowsbinarypatches.
