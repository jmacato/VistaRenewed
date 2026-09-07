# Runtime and persisted-evidence audit

Audit time: `2026-08-26T05:00:48+0800` through `2026-08-26T05:14:32+0800`.
Scope: read-only host, process, repository, and qcow2 metadata discovery. This
report does not establish rendering, a D3D9 probe pass, DWM composition, Aero,
or glass.

## Executive result

- No QEMU system process was running, no candidate Vista qcow2 was open, and
  no relevant TCP listener or Vista QMP socket was present during the audit.
- The current guest-owned launcher selects
  `/Users/jumar/winvistachecked/winvista-3.shrunk.qcow2`. Two dormant historical
  overlays also exist. `winvista-work.qcow2` has that base. `winvista-aero.qcow2`
  has a missing raw backing file and is not a usable complete chain.
- There is no host-side `TRITON9-PROBE PASS` or `TRITON9-PROBE FAIL` result and
  no host runtime DWM result. The current probe source deliberately flushes
  results to the Vista disk and COM2 instead of uploading them to a host web
  server. The exact probe revision installed in the guest remains unverified.
- The expected current transfer/media/result root,
  `/Users/jumar/aaaaa/vista-signing-transfer`, is absent. A similarly named
  repository directory exists, but the current scripts do not resolve to it.
- The current service source no longer follows the Safe-Mode/SetupAPI lifecycle
  described by `test-artifacts/VISTA_GUEST_DEPLOY.md`. It verifies media in
  normal mode, schedules four payload replacements for the next reboot, and
  starts a one-shot public D3D9 probe after that reboot. Its Safe Mode code is a
  recovery path for previously persisted state, not a path that a new
  deployment arms.
- The exact disk has a later mtime (`2026-08-17`) than every host runtime log
  found (`2026-08-14` or earlier for a nonempty graphics log). Therefore the
  durable guest logs are the best candidate for the latest decisive evidence.
  Their content was not read in this leaf because disk inspection was expressly
  reserved for a later read-only offline action.

## Free space

At `2026-08-26T05:00:48+0800`, `df -h .` reported:

```text
Filesystem      Size    Used   Avail Capacity  Mounted on
/dev/disk3s5   926Gi   813Gi    77Gi    92%   /System/Volumes/Data
```

The exact `df -k .` value was `80,909,356 KiB` available out of
`971,350,180 KiB`. No VM artifact, cache, disk copy, overlay, screenshot, or
framebuffer was created.

## QEMU process and exact Vista process

The live process checks were:

```sh
ps -axo pid=,ppid=,lstart=,stat=,command= | \
  rg -i '[q]emu-system|[q]emu-kvm|[q]emu-storage-daemon'
pgrep -afil '[q]emu-system|[q]emu-kvm|[q]emu-storage-daemon'
lsof -nP -- /Users/jumar/winvistachecked/winvista-3.shrunk.qcow2 \
  /Users/jumar/winvistachecked/winvista-work.qcow2 \
  /Users/jumar/winvistachecked/winvista-aero.qcow2
```

All three produced no target row. The live exact-command-line set was empty.

**QEMU process:** none observed. **Exact Vista process:** none observed. The
If it starts later, the process must match this configured identity:

- executable:
  `/Users/jumar/winvistachecked/triton-qemu/build/qemu-system-x86_64`
- VM name: `vista-aero-guest-deploy`
- disk argument:
  `file=/Users/jumar/winvistachecked/winvista-3.shrunk.qcow2,format=qcow2,if=ide`.

The configured command from `scripts/run_vista_guest_deploy.sh`, expanded with
its defaults, is below. It was not executed:

```text
/Users/jumar/winvistachecked/triton-qemu/build/qemu-system-x86_64
  -name vista-aero-guest-deploy -accel tcg -machine q35 -m 768M -smp 1
  -drive file=/Users/jumar/winvistachecked/winvista-3.shrunk.qcow2,format=qcow2,if=ide
  -drive file=/Users/jumar/aaaaa/vista-signing-transfer/vista-deploy-media.iso,format=raw,if=none,id=triton_deploy_media,readonly=on
  -device ide-cd,drive=triton_deploy_media -vga none
  -device virtio-vga-gl,vgamem_mb=128,hostmem=1G,max_hostmem=1G,blob=true,neptune=true
  -nic user,model=rtl8139 -device qemu-xhci,id=vista_xhci
  -device usb-tablet,id=vista_tablet -display cocoa,gl=on
  -qmp unix:/private/tmp/triton-vista-deploy-qmp.sock,server=on,wait=off
  -serial tcp:0.0.0.0:2020,server=on,wait=off
  -serial file:/Users/jumar/aaaaa/vista-signing-transfer/vista-deploy-status.log
  -debugcon file:/dev/null -global isa-debugcon.iobase=0xe9
```

No listener existed on TCP `2020`, `8088`, or `8089`. No relevant socket was
found under `/private/tmp`. The configured QMP endpoint was not opened or used.

**Safe-to-stop assessment:** no stop is necessary now because there is no
current PID. A later stop is conditionally safe only for a process that matches
all three identity fields above. Stop only that PID before offline read-only
inspection. A match on only `qemu`, a port, or a socket is not sufficient.
**Process stopped: no.**

## qcow2 and overlay inventory

The bounded home scan (through depth 5) found only these three Vista-named
qcow2 files. QEMU source-tree test fixtures and unrelated Android emulator
qcow2 files were excluded because no Vista launcher or report references them.

| Absolute path | Role and references | Host bytes / allocated | Guest virtual size | Backing metadata | State and mtime |
|---|---|---:|---:|---|---|
| `/Users/jumar/winvistachecked/winvista-3.shrunk.qcow2` | Current exact Vista disk in `scripts/run_vista_guest_deploy.sh`, `TRITON_VISTA_AERO_AUDIT.md`, and `VISTA_D3D9_UMD_TASKLIST.md` | `21,728,985,088` / `21,728,985,088` | `42,949,672,960` | none | qcow2 1.1, 64-KiB clusters, dirty=false, corrupt=false. Mtime `2026-08-17T01:48:36+0800` |
| `/Users/jumar/winvistachecked/winvista-work.qcow2` | Dormant overlay used by `run_qemu_1cpu.sh`, `run_qemu_ahci.sh`, and `run_qemu_diag.sh` | `328,466,432` / `328,466,432` | `42,949,672,960` | `winvista-3.shrunk.qcow2`, format qcow2. Chain resolves | qcow2 1.1, dirty=false, corrupt=false. Mtime `2026-06-03T17:57:54+0800` |
| `/Users/jumar/winvistachecked/winvista-aero.qcow2` | Dormant overlay used only by legacy `run_qemu.sh` | `515,178,496` / `515,178,496` | `42,949,672,960` | `winvista-flat.raw`, format raw. Backing file is missing | qcow2 1.1, dirty=false, corrupt=false at overlay level. Mtime `2026-06-03T20:11:07+0800`. Full chain cannot open |

Metadata came from `stat`, `du`, and `qemu-img info --force-share --output=json`.
`qemu-img info --backing-chain` resolved the work overlay to the base and failed
for the aero overlay with `Could not open 'winvista-flat.raw': No such file or
directory`. No `winvista-flat.raw` exists in the workspace.

The current read-only optical media default is
`/Users/jumar/aaaaa/vista-signing-transfer/vista-deploy-media.iso`. Both that
file and its parent transfer root are missing. Old ISOs under the repository
are historical media. None is the path selected by the current guest-owned
launcher. This audit did not stage or attach media.

## Guest-owned automation map

### Media and launcher

`scripts/stage_vista_deploy_media.sh` is intended to verify a signed package,
copy the public x64 probe, write `triton-deploy.ini`, and create an immutable
ISO9660/Joliet image. `scripts/run_vista_guest_deploy.sh` exposes that ISO as
`readonly=on`, keeps the same virtio GPU topology across guest reboots, and
does not send QMP/HMP input. The launcher maps COM1 to TCP `2020` and COM2 to
the output-only host file `vista-deploy-status.log`.

All three current scripts compute:

```text
workspace=/Users/jumar/winvistachecked
${workspace:h}=/Users/jumar
transfer_root=/Users/jumar/aaaaa/vista-signing-transfer
```

That transfer root is absent. The existing
`/Users/jumar/winvistachecked/aaaaa/vista-signing-transfer` contains only
`inspect.bat` and `kd-trace/driver.cpp` and is not selected by those scripts.

The current probe source is dated `2026-08-16T13:47:47+0800`. Matching x64 and
x86 build-tree executables are dated `2026-08-16T13:48:31+0800`. The convenient
`test-artifacts/triton9_runtime_probe_{x64,x86}.exe` copies are older
(`2026-08-13T13:43:29+0800`), while the staging script expects a separate x64
copy under the absent transfer root. Consequently, the source protocol mapped
below is the intended current protocol, but neither current media contents nor
the guest-installed probe revision can be verified from host files alone. The
source path is
`triton-umd/src/virtio/neptune/vista-d3d9/tests/triton9_runtime_probe.c`.

The older `scripts/build_deploy_vista_driver.sh` still contains host QMP,
screen, artifact-server, and exact-process control functions. Its mode switch
rejects `--all`, `--deploy-only`, `--runtime-only`, and `--probe-only` before
those functions can run. Only `--build-only` and `--restage-only` remain
available. This leaf did not run that script or any of its control functions.

### Installation mechanism

There are two layers:

1. The one-time x64 package bootstrap uses
   `test-artifacts/windows11_stage_vista_pnp_package.bat`, which takes
   `test-artifacts/vista-driver-x64-kd-serialtrace/viogpu3d-diagnostic.inf` and
   `test-artifacts/vista-deploy-service/triton-vista-deploy.exe`. That INF
   copies the service to `%SystemRoot%\System32`, registers the automatic
   LocalSystem service `TritonVistaDeploy` with command
   `%11%\triton-vista-deploy.exe --service`, and registers both SafeBoot keys.
   A legacy image can instead run `triton-vista-deploy.exe --install` once in
   the guest. It copies itself to
   `%ProgramFiles%\TritonVistaDeploy\triton-vista-deploy.exe` and registers the
   same automatic LocalSystem service and SafeBoot entries.
2. For each new immutable media ID, the current service source
   (`test-artifacts/vista-driver-deploy-service.c`) scans guest drives `D:`
   through `Z:` for `triton-deploy.ini`, verifies it, copies changed bytes to
   `%SystemRoot%\Temp\TritonDeploy\*.triton-pending`, and calls `MoveFileExW`
   with `MOVEFILE_REPLACE_EXISTING | MOVEFILE_DELAY_UNTIL_REBOOT` for:
   `System32\drivers\viogpu3d.sys`, `System32\neptune_d3d9.dll`,
   `SysWOW64\neptune_d3d9_wow.dll`, and
   `System32\triton-vista-deploy.exe`. It then stages the one-shot probe
   service and requests a guest reboot.

The current per-update path does **not** call SetupAPI, `SetupCopyOEMInf`, or
`UpdateDriverForPlugAndPlayDevices`. The SetupAPI lifecycle described in the
guide and source-file header is stale. The file-replacement design assumes the
device and services were registered by the one-time bootstrap package.

There is also a bootstrap-path split: the package registers the service binary
from `System32`, while standalone `--install` registers the Program Files copy.
Per-update code always stages the replacement service into `System32`. Thus an
image bootstrapped only through the standalone path will keep executing its
Program Files service unless some separate package transition changes the SCM
binary path. Persisted SCM/registry state is needed to identify which path the
exact disk uses.

### Reboot mechanism

`TritonVistaDeploy`, running as LocalSystem, enables `SeShutdownPrivilege` and
calls `InitiateSystemShutdownExW` with a two-second timeout, forced application
closure, reboot enabled, and a planned operating-system reconfiguration reason.
The normal update requests this reboot after staging replacement bytes. The
legacy recovery path retries guest reboot requests until the service is stopped.
No host boot selection or reset is required by this mechanism.

### Safe Mode mechanism

The package and standalone installer create:

```text
HKLM\SYSTEM\CurrentControlSet\Control\SafeBoot\Minimal\TritonVistaDeploy = Service
HKLM\SYSTEM\CurrentControlSet\Control\SafeBoot\Network\TritonVistaDeploy = Service
```

The service detects Safe Mode through
`HKLM\SYSTEM\CurrentControlSet\Control\SafeBoot\Option\OptionValue`. Its
recovery code can run `bcdedit /set {current} safeboot minimal`, perform the
same verified delayed-replacement installation in Safe Mode, run
`bcdedit /deletevalue {current} safeboot`, and request the return reboot.

However, the current new-deployment path writes `SafeModeOwned=0`. No current
code writes `SafeModeOwned=1` or an `ARMED` result. Therefore a fresh media ID
does not enter Safe Mode. The Safe Mode branch is reachable only if compatible
service-owned recovery state already exists in
`HKLM\SOFTWARE\Triton\VistaDeploy`. This contradicts the six-step Safe Mode
description in `test-artifacts/VISTA_GUEST_DEPLOY.md` and must not be described
as an exercised current lifecycle without persisted registry evidence.

### Verification mechanism

Before changing guest state, the service requires the SHA-256 of
`package-manifest.sha256` to equal the 64-hex deployment ID, verifies every
manifest-listed file hash, requires the INF, catalog, KMD, native UMD, and WoW
UMD entries, and separately verifies the optional public probe hash.

The current code compares source and destination bytes before deciding whether
to schedule each replacement. It then writes `LastSuccessId` immediately after
the replacements are scheduled. It has no post-reboot byte-for-byte verification of
the four final destinations. Thus, the guide's installed-file verification
claim is not implemented in this source revision.

For runtime verification, the service copies the public probe to
`%SystemRoot%\Temp\TritonD3D9Probe_<first-12-media-id>.exe`, registers an
automatic one-shot LocalSystem service with the correspondingly scoped name,
and starts it on the post-update normal boot. The probe launches its D3D9 child
with a duplicated Winlogon token on `winsta0\Winlogon`. It uses public
`Direct3DCreate9Ex`/`IDirect3DDevice9Ex` interfaces and records, in order:

- clear/readback: `clear pixel=0x........ expected=0xff112233 PASS|FAIL`
- triangle/readback: a near-red center and
  `triangle outside=... expected=0xff112233 PASS|FAIL`
- device presentation: the exact `PresentEx` HRESULT
- an additional-swapchain present
- terminal `TRITON9-PROBE PASS` or `TRITON9-PROBE FAIL`.

Only after this D3D9 gate passes does the one-shot service start UxSms and the
separate DWM API probe on `winsta0\Default`. Even its success marker is
`TRITON9-AERO API-PATH PASS VISUAL-UNVERIFIED`. It is not glass evidence.

### Result retrieval

The durable result locations on the exact Vista disk are:

```text
C:\Windows\Temp\triton-deploy.log
C:\Windows\Temp\triton9-service.log
C:\Windows\Temp\triton9-probe.log
C:\Windows\Temp\triton9-aero-probe.log
C:\Windows\Temp\triton9-winsat-dwm.log
C:\Windows\Temp\triton9-ddi.log
C:\Windows\Temp\triton9-d3d9-proof.log
C:\triton9-ddi.log
HKLM\SOFTWARE\Triton\VistaDeploy
C:\Windows\System32\Config\SOFTWARE
```

The final path is the on-disk registry hive that contains the listed state key.

Deployment and probe checkpoints are also written output-only to COM2, which
the current launcher maps to
`/Users/jumar/aaaaa/vista-signing-transfer/vista-deploy-status.log`.

The repository still contains the legacy
`scripts/vista_artifact_server.py` POST endpoints
`/__triton9_probe__` and `/__triton9_service__`, but current
`probeUploadTo()` does not make HTTP requests. It only calls
`FlushFileBuffers(g_log)`. No `.triton9-probe-result`,
`.triton9-service-result`, `.vista-install-complete`, or
`vista-deploy-status.log` appeared in the bounded depth-five `/Users/jumar`
scan. A depth-three `/private/tmp` scan also found no Vista, Triton, Neptune,
D3D9, or DWM result file.

## Evidence chronology

The workspace contains 46 `*.log` files. The chronological inventory below
covers all 23 logs in `test-artifacts`, both relevant `_dxmt.log` files, and
the root Vista serial log. It also covers the text status documents that contain
later runtime claims. The other 20 logs are build, QEMU configuration, Ghidra,
WIC, corpus-build, or Mesa reference logs. They contain no Vista guest runtime
result.

| Time | Host evidence | Decisive runtime lines or result |
|---|---|---|
| `2026-06-03T20:05:48+0800` | `vista-serial.log` (`0` bytes) | Empty. No D3D9 or DWM evidence. |
| `2026-08-10T16:45:15` to `16:47:12+0800` | `test-artifacts/vista-triton/serial.log`, `recovery-serial.log` (both `0` bytes) | Empty. |
| `2026-08-11T00:30:46` to `01:58:10+0800` | Ten `vista-neptune*serial.log`/QEMU log attempts: `vista-neptune-serial`, install-direct QEMU/serial, install-lowmem QEMU/serial, install-session serial, install-safe serial, sigprep serial, sigenforce-off serial, and testsigned serial | All `0` bytes. |
| `2026-08-11T03:07:49+0800` | `w11-virtiolib-build-diag.log` | Build-only failure: `error C1083: Cannot open include file: 'ntddk.h'`. Not guest runtime evidence. |
| `2026-08-11T03:13:09` and `03:19:41+0800` | `w11-vista-kmd-build.log`, `w11-vista-kmd-rebuild2.log` | Build-only failures: `error MSB3030` for missing PackOne `viogpu3d.sys`, `.pdb`, and `.inf`. Not guest runtime evidence. |
| `2026-08-11T03:38:53+0800` | `vista-neptune-kd-qemu.log` (`0` bytes) | Empty. |
| `2026-08-11T03:41:03+0800` | `vista-neptune-kd-headless-qemu.log` | `The display backend does not have OpenGL support enabled`. QEMU did not reach guest D3D9. |
| `2026-08-11T03:41:23+0800` | `Display 'egl-headless' is not available` | QEMU did not reach guest D3D9. |
| `2026-08-11T07:08:08+0800` | `vista-neptune-serialtrace-qemu.log` (`0` bytes) | Empty. |
| `2026-08-11T07:32:53` to `07:51:46+0800` | `vista-neptune-serialtrace-strings-tail.txt`, `vista-neptune-serialtrace-com1.log` | KMD packet capture contains `DriverEntry`, `VioGpu3DAddDevice`, and repeated `Break to debug, Ignore (bi)?`. No D3D9, public probe, PresentEx, or DWM marker. |
| `2026-08-11T08:16:43+0800` | `vista-neptune-reverted-com1.log` | Same KMD AddDevice/debugger-prompt boundary. No D3D9/DWM marker. |
| `2026-08-11T08:29:19+0800` | `vista-neptune-debugcon.log` | Repeats `DriverEntry` through `<--- VioGpu3DAddDevice ppDeviceContext = ...`. It does not reach `StartDevice`, D3D9, or DWM in the recorded strings. |
| `2026-08-11T10:34:52+0800` | `vista-neptune-debugcon-v3.log` | Repeats the same AddDevice sequence. No D3D9/DWM marker. |
| status `2026-08-12`, file mtime `2026-08-12T05:41:49+0800` | `TRITON_VISTA_AERO_AUDIT.md` | Lines 9 and 31-33 say no test opened the new Vista D3D9 UMD or rendered a Neptune frame, and Vista driver start, D3D9Ex draw, and DWM composition were not run. |
| checkpoint `2026-08-13`, file mtime `2026-08-13T02:15:27+0800` | `VISTA_D3D9_UMD_TASKLIST.md` | Lines 186-206 say Vista accepted the LDDM caps and created primary resource 5. Readback was near-black. D3D9Ex device-open, triangle, Present, DWM, and `DwmIsCompositionEnabled` tasks remained incomplete. |
| `2026-08-14T23:06:56+0800` | `triton-dxmt/_dxmt.log` | `D3DKMTOpenAdapterFromLuid not implemented` and `Failed to open D3DKMT adapter`. This D3D host-proxy error is not a Vista public-probe transcript. |
| `2026-08-16T03:24:08+0800` | root `_dxmt.log` (`0` bytes) | Empty. |
| `2026-08-17T01:48:36+0800` | exact Vista qcow2 mtime | Later than all host runtime evidence. This metadata does not prove which guest files changed. |
| `2026-08-26` current audit | exhaustive host marker search | No non-source host log contained `TRITON9-PROBE PASS`, `TRITON9-PROBE FAIL`, clear-pixel, triangle-center, PresentEx-result, `TRITONDEPLOY`, or runtime DWM markers. |

There are also `1,185` top-level historical PNGs and `1,183` top-level raw PPM
files under `test-artifacts`. `24` PNG/PPM pairs have `result` in their names.
They predate this task and are user-owned. They were not opened or modified in
this leaf. Filename-level screenshots cannot supply the required public D3D9
values or glass proof, and raw PPM evidence is forbidden by the current
contract.

### Decisive runtime lines

```text
qemu-system-x86_64: ... The display backend does not have OpenGL support enabled
qemu-system-x86_64: Display 'egl-headless' is not available.
<--- VioGpu3DAddDevice ppDeviceContext = FFFFFA8003775000
warn:  D3DKMTOpenAdapterFromLuid not implemented.
warn:  Failed to open D3DKMT adapter
```

**D3D9 conclusion:** no host result proves clear/readback, triangle/readback, or
PresentEx. **DWM conclusion:** no host runtime result proves DWM composition.
the latest text checkpoint leaves DWM start and composition unchecked. These
negative findings supersede any inference from a desktop/login screenshot.

## Offline inspection decision

**Offline inspection decision:** inspect
`/Users/jumar/winvistachecked/winvista-3.shrunk.qcow2` later, read-only. It is
the current launcher's exact disk, it is newer than the host transcripts, and
the current service/probe intentionally persist their decisive logs there.

At the time of this audit there is no exact Vista process to stop. Immediately
before later inspection, repeat the exact executable/name/disk process match
and the `lsof` inspection. If the exact process is then present, stop only that
PID. Then wait until the disk has no open holder. If it remains absent, no stop is
necessary. Then use a read-only offline filesystem reader against the existing
qcow2—no mount with write access, no overlay, no raw conversion, and no disk
copy—to extract only the listed log files and the SOFTWARE registry state.

The first offline pass should record timestamps and decisive lines for:

1. deployment ID/state and `VERIFY_*`, `INSTALL_*`, `DEPLOY_*`, reboot, and
   `PROBE_LAUNCHED` records in `triton-deploy.log` and the registry key
2. the exact last completed public D3D9 boundary, HRESULTs, clear pixel,
   triangle inside/outside pixels, PresentEx, and terminal probe marker
3. the bounded DDI/proof-event tail around that boundary
4. DWM/Aero API output only if the D3D9 gate passed, retaining the explicit
   `VISUAL-UNVERIFIED` qualification.

**Boot cycle avoided:** yes. A new Vista boot or UI retrieval is neither needed
nor permitted. These persisted files can be read from the stopped existing
qcow2.

## Four-pass review and safety record

1. Complete inventory: measured capacity and enumerated processes, ports,
   sockets, all Vista disk candidates, media paths, service/probe protocols,
   all runtime logs, result markers, and historical image categories.
2. Expert reread: distinguished the current normal-mode delayed replacement
   path from the stale Safe-Mode/SetupAPI guide and distinguished D3D9 API proof
   from any Aero claim.
3. Omission/defect hunt: found the missing transfer root, the broken aero
   backing chain, the unarmed Safe Mode path, the missing post-reboot payload
   verification, the split service-binary bootstrap paths, the staged-probe
   revision gap, the disabled legacy host-driven modes, and the absence of host
   public-probe/DWM results.
4. Free polish: normalized absolute paths, byte counts, timestamps, exact
   markers, and the safe later offline action. A reread found no unsupported
   runtime-success statement.

Host VM control: none.  
VM mutation: none.  
Raw derivative: none.  
Raw framebuffer: none.  
Source files modified: none.  
Process stopped: no.  
PNG created: none.  
Disk mounted: no.
