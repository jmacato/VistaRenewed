# Triton Vista presentation audit — 2026-09-08

> Development record retained with the public desktop sources. Local capture,
> VM and `.unlazy/` paths refer to non-shipped development artifacts. See
> [current public entry points and status](DESKTOP-MODULES.md).


This audit followed the active Vista D3D9 path from draw preparation through
Neptune submission, host dispatch, GPU completion, WDDM Present, scanout copy,
and synthetic vblank. It also checked allocation references, error propagation,
worker start/stop, and the recent performance fixes. It is a focused review of
that path, not a certification of every API in the generated D3D11/D3D12 stack.
The comparison point is the working tree used for VM package 7.14.1.6.

The strongest findings are blocking pixel transfers on the thread responsible
for vblank, and failure paths that hide errors. The initial hypothesis that the
per-draw device-health query made synchronous round trips was wrong: inspection
of the generated client found an asynchronous thunk that always returned success.
The correction below makes the query truthful and keeps that now-synchronous call
out of the draw path.

| Priority | Finding and trigger | Change |
| --- | --- | --- |
| P1 | The generated `GetDeviceRemovedReason` sends an asynchronous command and returns `S_OK` without reading the host result. This treats a status query like a state setter. | An override for all device interface tiers returns the real host HRESULT and detects transport failure before/after the call. Hot DDI checks inspect transport health locally; Present and Flush perform the synchronous host check. |
| P1 | `FlipThread` both promoted sources and reported vblank. Promotion takes `m_flipSubmitMutex` and waits for synchronous scanout/flush completion. A slow copy stalls the display clock; source-ready wakes can also occupy that same thread. | A separate vblank worker performs bounded clock/interrupt work. The copy worker and command worker retain their completion mutex. Worker creation failure and shutdown stop/join both workers. |
| P1 | A timer wake always emitted a vblank before checking its retained deadline. A sufficiently early wake could report an extra tick without advancing that deadline. | The clock uses QPC and tests the deadline before reporting. Early wakes re-arm the remainder; late wakes skip missed deadlines. This remains a synthetic clock, independent of the physical monitor's scanout. |
| P1 | My previous ring fix used a sequentially consistent store to the WC tail. On x86 this can compile to `XCHG` on the mapping, contradicting the code's existing restriction on WC read-modify-write under translated x86/ARM. | Publish with a release store between full fences. Payload-before-tail and tail-before-status ordering remain; no read-modify-write of the tail is required. Native x64/x86 builds were checked; translated ARM execution was not tested. |
| P1 | Both non-Windows direct submission paths suppressed an IDLE doorbell if another had been sent within 1 ms. A feedback/spurious wake can let the host sleep again within that interval, leaving new work without a notification. | Notify on every observed IDLE state in both contiguous and split submission paths. The host mutex and publish-before-check handshake remain in place. |
| P1 | Encoder overflow silently discarded a write, and encoder fatal marking was a no-op. Submission could still send an incomplete packet. An allocation/sizing failure in an asynchronous thunk could also silently discard an operation. | Encoder failure is sticky. Submission rejects fatal or incomplete encoding and poisons the ring. Command allocation/sizing failures also poison it. The earlier null-encoder guard remains. |
| P2 | Fence feedback returned the cached completion value even after transport failure; the synchronous fallback could fail while returning a non-removal value. | Check primary/method ring health and return the D3D11 device-removal sentinel on failure, including a failure during the fallback RPC. |
| P2 | Present created and closed a kernel event every frame. | Reuse a device-owned auto-reset event under `shaderLock`. Only one Present is outstanding; a failed wait poisons the device and cannot reuse a late signal. Device destruction closes the handle. |

Relevant implementations:

- `triton-umd/src/virtio/neptune/vista-d3d9/triton9.h`: local versus boundary health checks.
- `triton-umd/src/virtio/neptune/vista-d3d9/triton9_resource.c`: GPU and source-consumption waits.
- `triton-kmd/viogpu/viogpu3d/viogpu_vidpn.cpp`: workers, deadline handling, displayed-address publication.
- `triton-umd/src/virtio/neptune/npt_ring.c` and `npt_cs.h`: publication and command failure handling.
- `triton-umd/src/virtio/neptune/npt_overrides_d3d11_fence.c`: cached fence health.

The ownership barriers are intentional. The UMD waits for its frame's GPU fence,
then submits Present and a consumption marker on the same WDDM context. The KMD
does not complete a blob flip until the host has copied its source. The displayed
address is published only after that succeeds. Removing these waits without
introducing independently owned in-flight images would allow Windows to overwrite
or release a source still being read. The separate clock worker only reports the
last completed address; it never treats a pending copy as complete.

The QEMU path still downloads the DMA-BUF into CPU memory and uploads the display
surface to OpenGL. It exports/maps/synchronizes/unmaps the buffer on each flush.
The earlier surface-reuse change removed a duplicate download and repeated texture
creation, but it did not create direct GPU scanout. Present also remains synchronous
and uses a 1 ms sleep when polling incomplete GPU work. These are remaining limits
on throughput and latency. Reaching hardware-limited presentation needs an owned
frame queue and explicit completion for imported GPU images, followed by measurement
of actual frame presentation. Raising the advertised refresh rate cannot supply that.

Validation includes all native `tests/vista/test-*.py` fixtures, x64/x86 KMD and UMD
builds with Vista PE checks, the renderer initialization test, portable graphics
contract checks with sanitizers, and the Linux D3D11 suite (174 and 212 assertions
in its two reported groups, zero failures). New fixtures exercise actual production
helpers for blocked-copy vblank, early wakes, event reuse and late-failure exclusion,
device health, encoder failure, repeated IDLE publication, and cached fence failure.
Mocks establish those contracts, not hardware memory-model correctness or Windows
runtime compatibility; live VM validation is recorded separately below.

The timing review used Microsoft's [Timer Accuracy documentation](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/timer-accuracy).
The publication review follows the publish/barrier/check ordering explained in the
Linux kernel's [memory-barrier documentation](https://www.kernel.org/doc/html/latest/core-api/wrappers/memory-barriers.html).

Live validation used signed package **7.14.1.8** on Vista Ultimate x64 RTM 6000,
in normal boot with test signing enabled. Binary comparisons matched the installed
KMD and both native/WOW UMDs to the signed media. Device Manager reported code 0.
A 180-second DWM window stress completed 20,324 updates with zero failures and
retained DWM PID 1652. This tests composition stability, not 3D benchmark throughput.

The subsequent native x64 `winsat formal -v` rerun reported **234.20 F/s** for
Graphics Performance and **12,393 MB/s** for Video Memory Throughput. It was **not
a successful formal assessment**: the D3D9 texture-load device failed with
`0x8876086a`, Media Foundation topology creation timed out (`0x800705b4`), CPU
assessments selected zero threads and rejected their parameters, and the memory
assessment crashed with `0xc0000005` in `ntdll.dll` at offset `0x4ef98`. WinSAT
reported 12 CPUs but 16 cores per processor and zero CPUs per core, suggesting a
legacy topology-detection issue; this has not established the memory crash's cause.
These partial throughput numbers do not measure physical display scanout or unique
QEMU frames. Logs and a crash screenshot are retained under `vista-kvm/x64-base/`
as `winsat-rerun.log`, `winsat-detail.log`, and `winsat-rerun-crash.png`.

Actual 3D benchmark testing used **3DMark06 1.2.1 Advanced**, installed with its
bundled DirectX December 2005, OpenAL and VC++ components. UL provides the legacy
installer and free Advanced key on its [legacy benchmarks page](https://benchmarks.ul.com/legacy-benchmarks).
The first runs used 1280x720, no AA, Optimal filtering, VS/PS 2_a, one repetition,
and `-nosysteminfo`. The public runtime independently reported VS/PS 2.0 and the
correct native/WOW Triton DLL in both process architectures. Both architectures
also successfully executed SSE2; the installer's missing-SSE warning was false.

These runs produced **no valid 3DMark score**:

- Return to Proxycon failed loading `hangar_cube_rightcorner.dds`, reporting
  `D3DXCreateCubeTextureFromFileInMemoryEx` / `D3DERR_NOTAVAILABLE`.
- The Pixel Shader feature test also failed loading its texture, reporting the
  same cube-texture loader error.
- The Single-Texturing Fill Rate test reported `IDirect3DDevice9::Present`
  `D3DERR_NOTAVAILABLE`. The failure log identified a deferred texture-stage state
  rejection: stage 0, state 24 (`TEXTURETRANSFORMFLAGS`), value 2 (`COUNT2`).

This exposes substantial API limitations beyond the presentation audit. Resource
creation explicitly rejects cube/volume textures and explicit mip chains. The
format table contains only ARGB, XRGB, A8 and the supported depth formats, without
DXT or floating-point colour textures. The fixed vertex shader passes texture
coordinates through and the state setter rejects non-disabled texture transforms.
SM3 is deliberately unadvertised. Correct support requires the corresponding
resource, view, lock/copy, shader and capability work; setting capability bits or
ignoring these states would invalidate rendering and benchmark results.

An isolated native x64 `winsat d3d -alushader -time 15 -v` completed with exit
code 0 and no reported shader/resource failure. It did not print an FPS result;
this is a successful limited assessment, not an overall WinSAT or 3DMark pass.
The log is `vista-kvm/x64-base/winsat-alushader.log`.

After testing, Vista was shut down normally and QEMU relaunched. Its live QMP
`query-display-options` confirmed `{"gl":"on","zoom-to-fit":false,"type":"gtk"}`.
The guest returned to the interactive desktop; the persistent launcher retains
that setting. CPU and RAM remain 12 vCPUs and 16 GiB.

### Texture-state and POSITIONT follow-up: packages 9 and 10

The next WinSAT formal rerun on package 8 measured 238.80 F/s desktop graphics
and 12,636.50 MB/s video memory, then reproduced the texture-device failure,
media timeout, zero-thread CPU tests, and `ntdll.dll+0x4ef98` access violation
during memory assessment. These are partial measurements, with no formal score.
See `winsat-latest.log` and `winsat-latest-crash.png` in the VM directory.
Before that run QEMU was paused with block I/O status `nospace`; resuming after
space became available restored the guest. This pause was a host storage failure.

Package 9 accepts nonprojected COUNT2–COUNT4 texture-transform state, stores eight
texture matrices, and emits the corresponding fixed-function matrix operations
for FLOAT1–FLOAT4 texture inputs. The production emitter and constant upload pass
96 native cases. COUNT1, projected coordinates, automatic coordinate generation,
and transformed non-FLOAT declarations remain unsupported.

Guest pixel readback exposed a separate draw-selection bug. Ordinary texture
transforms already worked on package 8 through Vista's runtime-generated vertex
shaders. Package 9 removed the deferred COUNT2 rejection but POSITIONT draws
rendered black. Draw preparation incorrectly used a bound shader for POSITIONT
unless the shader binding was null. Microsoft's
[POSITIONT declaration contract](https://learn.microsoft.com/en-us/windows/win32/direct3d9/mapping-fvf-codes-to-a-directx-9-declaration)
requires vertex processing to be bypassed for these declarations.

Package 10 selects the transformed-vertex path whenever the declaration contains
POSITIONT, without modifying the saved application shader binding. The expanded
guest test fails two pixel cases per architecture on package 9 and passes all
eight on package 10, in both native x64 and WOW32. It checks default coordinates,
matrix changes, disabled transforms, bypass with a runtime-generated shader,
bypass with an explicitly bound shader, and the shader's effect after returning
to ordinary vertices. Logs: `audit9-texture-expanded-baseline.log` and
`audit10-texture-runtime.log`. This establishes the tested rendering behaviour;
it does not establish full fixed-function or benchmark compatibility.

The catalog generator also needed a packaging correction: typed SPC copying
produced inconsistent container lengths, while canonicalizing the typed image
fields removed explicit flags needed by Authenticode page-hash verification.
It now preserves each signed SPC sequence opaquely while rebuilding container
lengths. Before writing, it strictly reparses the catalog, visits every member,
and verifies each SPC sequence byte-for-byte against its source. A fresh six-member
catalog passed this check; signed package 10 passed osslsigncode catalog membership
verification for the KMD and both UMDs. Vista loaded the new UMDs in normal mode.

3DMark06 Single-Texturing Fill Rate now completes at 1280x720 and reports
9,979.732 MTexels/s, but its fullscreen output is visibly corrupted. This number
is **not accepted as a validated benchmark result**. Screenshots are retained as
`audit10-fillrate-corruption.png` and `audit10-fillrate-result.png`; the host log
is `audit10-fillrate-host.log`. There is still no overall 3DMark score.

The new `test-fullscreen-present-runtime.c` diagnostic separates GPU readback
from scanout. On package 10 its first clear reads back red (`ffff0000`) correctly,
and Present returns success, but QEMU displays black. Its next Present fails
`8876086a`; the verbose trace reports a failing Blt. It is a presentation test,
not a performance benchmark. Evidence is in `audit10-fullscreen-diagnostics.log`
and `audit10-fullscreen-red.png`.

The verbose resource trace shows one 1280x720 XRGB render target with flags
`08009081`: RenderTarget, NotLockable, DiscardRenderTarget, Primary, and
MatchGdiPrimary. `triton9CreateSingleResource` excludes a single primary from
`needsPresentAllocation`, creates its KMD standard allocation, and
`triton9EnsureResourceHost` renders into a separate private D3D11 texture. No
exported texture is reported for that process. The disconnected rendering and
scanout storage is a concrete remaining defect. Fixing it must preserve the
KMD-created desktop primary imported through OpenResource, bootstrap allocation
ordering, and runtime allocation identity; simply changing a capability bit or
exporting the destination desktop primary is not a sufficient fix.


## Fullscreen isolation and WinSAT on package 13

Single UMD-created primary render targets now use exportable presentation
allocations (package 11). KMD allocation creation no longer changes Vista's
scanout source or invalidates the generation of already queued flips (package
13). Allocation creation is not a display ownership transition. The extracted
native primary-allocation regression and the full native suite pass.

These changes have not yet fixed fullscreen output. The diagnostic now owns its
window procedure and offers `--present-only`, which omits readback entirely.
On package 13 all three Clear/Present calls succeed in that mode, but the held
red, green, and blue frames are black on QEMU. The UMD trace alternates between
an exported primary and an opened standard GDI primary. The latter still uses
a private host texture disconnected from the KMD allocation. In readback mode,
the first red readback is correct; a later deferred Blt rejects host formats
87 to 88 (ARGB to XRGB). That API failure is distinct from the black scanout:
presentation-only reproduces black without the Blt failure. Evidence:
`audit13-present-only-frames/samples.json`, package 12 serial archive, and
`audit11-scanout-trace.log` under `vista-kvm/x64-base`.

A fresh `winsat formal -v` identifies driver 7.14.1.13 and reports graphics
245.57 F/s and video-memory throughput 12,994.90 MB/s. Texture-device creation
still fails 0x8876086a, media topology times out 0x800705b4, CPU tests select zero
threads, and the memory test crashes with 0xc0000005 in ntdll.dll+0x4ef98.
There is no completed formal assessment or validated overall score. These
throughput numbers do not measure monitor refresh. Aero was restored afterward.
Evidence: `winsat-package13.log` and `winsat-package13-crash.png`.


## No-wait flip rejection isolated with package 14

The bounded per-device KMD trace shows desktop flips reaching command retirement
with `armed=1`, while the fullscreen diagnostic produces only translation-entry
records: `ctx=9 stage=0 flags=c`. Its three flips never reach the enqueue or
retirement trace. The production branch explicitly rejects every flip flag value
except 0x4. The extra 0x8 is `FlipWithNoWait`, documented in
[DXGK_PRESENTFLAGS](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_presentflags).
Vista RTM is observed submitting it for this immediate fullscreen workload despite
our zero FlipCaps. Successful application Present return values did not expose
this kernel rejection. All held frames remained black on package 14.

The fix accepts Flip with optional FlipWithNoWait through the existing ordered
DMA path, which performs no vblank wait. It continues to reject every other
modifier. `test-flip-flags.py` extracts the whole production flip branch and
verifies allocation retention requests, source/generation association, emitted
NOP and patch consumption for both combinations, all unrelated modifier bits,
insufficient-buffer retry, and non-primary rejection. The native regression and
full native suite pass; the x64 KMD passes its Vista PE audit. Package 15 contains
this fix and the bounded trace. Live display validation is recorded separately.


Package 15 live validation: the same fullscreen context now logs flags=0xc at
translation and enqueue, followed by retirement with armed=1 for both source
allocations. The kernel rejection is fixed. Nevertheless, independent captures
show all three presentation-only holds remain black. A second run with readback
still reads the first red pixel correctly; QEMU now receives SET_SCANOUT_BLOB and
RESOURCE_FLUSH for that exported source (resource 0x27). The subsequent ARGB/XRGB
Blt failure remains. Exported-image visibility therefore needs investigation in
addition to the known disconnected standard-primary storage. No fullscreen or
benchmark success is claimed. Evidence: `audit15-{com1,kmd-debugcon}.log`,
`audit15-present-only-frames/samples.json`, and `audit15-scanout-trace.log`.
The installed kernel file matches the package byte-for-byte after normal reboot;
WMI reports device error code 0 and binary driver version 1.20.300.5800 (the INF
package version is 7.14.1.15). Aero desktop glass is visible afterward. QEMU trace
events were disabled after capture; the 24-record per-device KMD diagnostic
budget remains temporarily enabled to continue isolating the display defect.


## Audit 16: deferred shared clears were absent from submitted GPU work

The QEMU pixel trace samples its CPU display buffer after resource readback.
On the old backend, the exported fullscreen surface contains zero at its centre,
matching the black screen. A new real-GPU test in `tests/linux-d3d11-test.cpp`
reproduces this outside the VM: export 1280x720 ARGB/XRGB images before rendering,
clear them, wait for an event query plus Flush, then map the DMA-BUF with
DMA_BUF_IOCTL_SYNC. Both formats read black on the first clear and stale colours
subsequently. A staging GPU readback *before* the CPU sample hides this defect,
which explains why the existing GPU-to-GPU shared-resource test missed it.

`DxvkContext::prepareSharedImages` only visited `m_nonDefaultLayoutImages`.
Deferred clears do not necessarily change an image's layout, so they could be
left entirely out of the submitted command list. Signalling completion therefore
did not imply that shared-image clear pixels had been written. The fix collects
shared-image entries directly from `m_deferredClears`, removes them before
executing the batch, then restores shared-image layouts. Private deferred clears
remain queued. This also removes the old overwritten `hasSharedClear` result.

The native regression interleaves two shared textures with a private texture,
uses a different colour on each, and repeats for three phases. It samples *all*
CPU buffers before any staging readback, then checks all GPU images including
the private image. Old installed backend: 299 assertions pass, 6 CPU pixel checks
fail. Rebuilt backend: 305 pass, 0 fail. The rest of the D3D11 acceptance suite
runs in the same executable. Evidence: `audit16-export-mixed-{baseline,fixed}.log`.

The patch is applied to the DXVK submodule working tree at base
`c6bb6d57fac2b6cae7f6adbbc521eb949849815e` and preserved as
`patches/dxvk-shared-clear-flush.patch` for clean source bootstraps. Applying it to
a temporary copy of the pinned source reproduced the current implementation
byte-for-byte. README documents the one-time patch step. Backend libraries were
installed while the VM was cleanly shut down, then QEMU was recreated. Library
hashes and validation details are in `audit16-backend.json`. The guest INF package
remains 7.14.1.15; audit 16 is a host-backend change.

Live validation now shows solid red and blue covering the full QEMU surface in
the presentation-only fullscreen diagnostic. QEMU records the same red/blue
centre pixels. The middle green phase still displays black when Vista renders
into the opened standard GDI primary, whose host texture is disconnected from
its KMD storage. Thus the exported-primary path is verified, but fullscreen as a
whole still fails. The ARGB-to-XRGB Blt limitation remains separate. Evidence:
`audit16-fixed-present-only-frames/samples.json` and
`audit16-fixed-pixel-trace.log`. x64 and x86 texture/POSITIONT regressions each pass
all eight pixel cases after the backend change. Aero desktop remains visible.
QEMU pixel/scanout tracing was disabled after capture. No new performance or
complete 3DMark/WinSAT result is claimed.


## Audit 17: authoritative metadata for standard-primary GPU sharing

The standard primary is a legacy VirGL GL texture. Proxy resource attachment
can already export its DMA-BUF, but the attachment protocol only carries the
resource id, fd type, and allocation size. It loses image format, tiling modifier,
pitch, and offset. The UMD's special standard-primary branch currently avoids
import entirely and creates disconnected private D3D storage. Guessing linear
BGRA metadata for this allocation would also be incorrect.

`tests/linux-primary-interop.cpp` creates the exact KMD standard-primary resource
kind and bind flags through real VirGL, uploads red, exports the underlying
storage, imports it into DXVK, then writes green/blue/red through DXVK and reads
each through VirGL. It tests 1280x720 and 800x600. The initial test failed because
`vrend_renderer_export_query` only implemented GBM-backed resource metadata;
GL-backed EGL exports were deliberately omitted even though legacy fd export
was available.

The renderer now queries an EGL image for authoritative format, modifier, and
plane layouts and exports the corresponding fds. It packs distinct fds while
retaining per-plane stride/offset entries, and closes temporary descriptors for
metadata-only calls and every failure path. The handling follows the
[EGL_MESA_image_dma_buf_export specification](https://registry.khronos.org/EGL/extensions/MESA/EGL_MESA_image_dma_buf_export.txt).
The native helper fixture tests fd reuse across planes, ownership transfer,
metadata-only cleanup, partial export failure, bad layouts, missing extensions,
and EGL failures. The full `tests/vista/test-*.py` suite passes.

The final real-GPU interoperability test passes **48 checks**, including
metadata-only/export consistency and pixels in both directions. On this Intel
GPU the primary exports as DRM AB24 (RGBA storage), modifier
`0x0100000000000002`, pitch 5120 at 1280x720 and **3584** at 800x600. The logical
VirGL format is B8G8R8A8; the external D3D import correctly uses R8G8B8A8. The
800-pixel pitch is not width*4. Tests preserve both channel order and padded
layout. Evidence: `audit17-primary-interop.log` (initial failure),
`audit17-primary-interop-fixed.log`, `audit17-native-tests.log`, and
`audit17-primary-interop.json`.

This establishes a direct GPU-sharing implementation path; it is not yet wired
into the Vista standard-primary branch. The rebuilt renderer was tested from
its build directory and **has not been installed into the running VM**. The VM
still uses package 15 plus the audit16 shared-clear backend fix. Its middle green
fullscreen phase remains uncorrected. Next work must forward these authoritative
layout fields through proxy/server resource attachment, then open the existing
primary in the UMD while preserving its logical format and kernel allocation
identity. It must not invent a linear pitch or replace the primary with another
private allocation. No benchmark or full fullscreen pass is claimed here.

### Audit 18: standard-primary allocation sharing installed and exercised

The proxy/server resource-attachment message now carries the authoritative EGL
image layout alongside its DMA-buffer descriptor. The Neptune worker stores it
under the resource-table mutex before publishing the attachment. Non-image and
Venus attachments retain zero layout metadata. Proxy and server were rebuilt and
installed together after a clean Vista shutdown; this internal protocol change
requires matching binaries.

A synchronous SHARED_QUERY_LAYOUT transport method returns the attached image's
actual DXGI format, dimensions, modifier, allocation size and plane layout. It
uses the same bounded attachment wait as shared opening, copies metadata under
the detach mutex, and rejects unsupported formats/planes and invalid row bounds.
The native fixture covers RGBA/BGRA/BGRX mappings, padded tiled rows, exact end
bounds, missing and delayed attachments, detach after unlock, and timeout. It
also compares host and guest wire definitions. All native Vista fixtures pass;
the real-GPU GL/D3D interoperability test still passes all 48 checks.

The Vista UMD now lazily imports the actual standard primary instead of creating
a disconnected private D3D11 texture. Separate transport-import handles retain
the attachment until resource destruction, without replacing the runtime's
primary handle or changing its ownership. The host format comes from the query;
the logical D3D9 format remains unchanged. Failed opens reuse an existing
attachment rather than overwriting its handles. Both UMD architectures build and
pass their Vista PE audits.

Signed package **7.14.1.18** was installed from the normal desktop and rebooted.
Installed native and WOW DLLs compare byte-for-byte with the signed media, and
Windows reports ConfigManagerErrorCode 0. The presentation-only fullscreen test
exits 0 and independently captured frames now show **red, green and blue**. The
middle standard-primary frame was black in audit16; it is now green across
99.8857% of the screen. The remaining area is an opaque black rectangle around
the busy cursor, recorded explicitly rather than counted as perfect output.
Exported red and blue frames each cover 100% of their captured surface.

The readback-enabled variant still fails on its next Present: the deferred Blt
now reports actual source format 28 (RGBA) and destination 88 (BGRX), and rejects
that conversion with 0x8876086a. This needs a real format-converting GPU blit;
copying incompatible formats blindly is not a fix. Eight texture pixel cases
pass on each guest architecture after deployment, and Aero remains visible.
Evidence is in `vista-kvm/x64-base/audit18-runtime.log`,
`audit18-fullscreen-frames/samples.json`, `audit18-native-tests.log`,
`audit18-primary-interop.log`, and `audit18-installed.json`.

The last explicit WinSAT rerun preceded this deployment (package15/audit16):
239.62 F/s graphics and 12680 MB/s video-memory throughput. Texture creation still
failed, media playback timed out, CPU topology produced zero worker threads,
and the memory assessment crashed in ntdll.dll at offset 0x4ef98 with c0000005.
No overall score or full display-refresh performance is established. Further
work remains on format conversion, cursor transparency, benchmark feature
coverage, and the presentation throughput ceiling.

### Audit 19: GPU format conversion; remaining stale guest readback

The existing stretch-blit helper always created its output texture in the source
format. It now renders into the destination format and supports CPU uploads to
its input snapshot and copies to a staging destination for CPU readback. Blt
routes RGBA/BGRA/BGRX conversions through that shader path, then completes the
staging-to-runtime-memory copy. Format selection occurs after lazy primary
materialization, because the imported host format can differ from the logical
D3D9 format. Same-format copies retain their existing path. The deferred context
and ExecuteCommandList(TRUE) preserve application pipeline state.

`tests/run-native-blit.py` runs the production helper against native DXVK, adapting
C COM calls to the C++ ABI and replacing the Windows heap calls. Its patterned
pixels exercise all nine source/destination format pairs across GPU-to-GPU,
CPU upload, and CPU readback; checks cover crop offsets, untouched destination
borders, alpha semantics and viewport preservation. It also clears the source
and reuses the destination to check fresh readback after deferred clear. All
**270 checks pass**. The original helper produces four pixel failures in the
GPU-to-GPU subset (50 passed, 4 failed). The native graphics script includes the
new test; all existing Vista native fixtures and both UMD PE audits also pass.

Package **7.14.1.19** was installed in normal mode and rebooted. Both installed
UMD DLLs match the signed media and Windows reports device error code 0. The
fullscreen readback variant now makes every Present successfully and independent
captures show red, green and blue. However, CPU readback for its green phase is
**stale red** (`ffff0000` instead of `ff00ff00`), while red and blue read back
correctly. Thus the application exits 1 and this is not a complete readback pass.
The native deferred-clear/reused-destination test does not reproduce this,
pointing to a difference in the guest transport/runtime path; the exact cause is
not yet established. The opaque cursor rectangle remains in the green frame.
Eight texture cases pass on both guest architectures afterwards; Aero remains
visible.

Evidence: `audit19-runtime.log`, `audit19-fullscreen-readback-frames/samples.json`,
`audit19-gpu-blit.log`, `audit19-gpu-blit-baseline.log`, `audit19-native-tests.log`,
`audit19-umd-build.log`, `audit19-catalog-sign.log` and `audit19-installed.json`,
all under `vista-kvm/x64-base`. Next investigation must trace when the runtime
submits the converting Blt, when its destination is mapped/copied to pSysMem,
and ordering between the deferred-context command list and immediate-context
readback. Map's cached pitch fast path only handles writes; READ already takes a
synchronous MAP_RESOURCE, so a blanket read-cache invalidation is not justified.

### Audit 20: fix deferred command-list resource tracking in DXVK

A bounded guest trace established that the converting Blt copied stale red into
Vista's actual SYSTEMMEM shadow before the application's read lock returned.
Thus the stale value was not caused by the application reading before the DDI
copy or by a cached read-map shortcut. A comparison with NPT_PERF=multi_ring
still returned stale red; the temporary process setting was then removed.

The native blit test had a material coverage hole: its readback utility copied
the destination into a *fresh* staging texture before mapping. That extra GPU
operation hid the original destination's missing completion dependency. The
fixture now maps a staging destination directly. On the prior backend it
reproduced **17 failures (253 passed)**, including unchanged sentinel pixels and
stale data after a new clear. Earlier audit19 native passes did not prove that
this direct-map path was correct.

DXVK's deferred-context `m_chunkId` means the index of the last emitted chunk.
It started at zero both on construction and after FinishCommandList, so
GetCurrentChunkId assigned resources in the first pending chunk to chunk 1.
AddChunk actually numbered that first chunk 0. EmitToCsThread therefore missed
its resource sequence-number updates, and Map could observe no pending GPU
write and return old memory. Initializing and resetting the last-chunk index to
`~0ull` makes the first pending chunk index zero while preserving subsequent
chunk and nested-command-list indexing. The patch is preserved separately in
`patches/dxvk-deferred-resource-tracking.patch`; applying it to the pinned source
was verified to reproduce the worktree exactly. README bootstrap instructions
now include it alongside the shared-clear patch.

With the fix, all **270 direct-map blit checks pass**, including destination reuse
after deferred clear. The freshly rebuilt broader native graphics suite passes
**305 checks**, including the earlier external shared-clear regression. The
backend was installed only after Vista shut down cleanly. Guest diagnostic
package 7.14.1.20 retains package19's conversion code plus bounded readback traces.

After restarting with the fixed backend, the guest fullscreen readback test
returns `ffff0000`, `ff00ff00`, and `ff0000ff` for the three expected colours and
exits **0**. Independent screen captures show all three colours. The green
frame's opaque cursor rectangle remains (99.89% green), so cursor transparency
is still an open defect; this is not a full-refresh or overall benchmark claim.

Evidence under `vista-kvm/x64-base`: `audit20-runtime.log` (stale shadow trace),
`audit20-multiring.log`, `audit20-direct-map.log` (17 failures),
`audit20-direct-map-fixed.log` (270 passes), `audit20-dxvk-regression.log`
(305 passes), `audit20-fixed-runtime.log` and
`audit20-fixed-readback-frames/samples.json` (guest success), plus build/install
logs. Next work remains on cursor transparency, real benchmark feature coverage,
and reducing presentation copies and synchronization overhead without weakening
these correctness checks.


### Audit 21: synchronize standard-primary CPU backing (guest verification pending)

The driver does not implement hardware cursor shapes. During a green fullscreen
frame, a QMP dump of the entire 128 MiB VGA BAR contained only 78 green pixels;
most bytes were zero although the displayed GPU surface was green. This is
strong evidence of stale CPU backing implicated in the software cursor's black
background, but does not yet prove the proposed fix resolves the cursor defect.
Evidence: `audit21-cursor-frames/green-vram.bin` and `green-vram.json` under
`vista-kvm/x64-base`.

Vista standard-primary flips now queue a VirGL transfer from host before flip
completion. The packet uses the allocation's actual transfer layout; blob flips
retain their existing path. VirGL attachment setup is shared with blit presents,
and invalid layout, insufficient DMA space, and attachment failures propagate
without publishing a flip. This adds a full-primary GPU-to-CPU copy, so it is a
correctness candidate with a performance cost, not a performance improvement.
Hardware cursor support and reducing presentation copies remain open work.

The extended production flip fixture covers both ordinary and no-wait flips,
transfer fields, completion association, and failure paths. The native Vista
suite and x64 KMD build/PE checks passed (`audit21-native-tests.log` and
`audit21-kmd-build.log`). Package 7.14.1.21 was built and signed, and its
normal-mode installation command returned to the guest prompt. At this commit,
reboot, installed KMD byte comparison, device status, cursor verification, and
performance measurement remain pending. Package 20 remains the last verified
guest baseline; do not infer package 21 runtime success from the installer exit.


### Audit 22: isolate host-window waits from renderer work (runtime test pending)

During package21 verification, a warm reboot developed severe input/clock
slowdown. QMP showed running vCPUs, but KUSER_SHARED_DATA InterruptTime and
SystemTime advanced only 10.5 ms over about 11 host seconds. A later paired
sample showed the HPET counter advancing at its expected 100 MHz while IRQ0
advanced only 65 times across about five seconds. These observations establish
a timing problem; they do not by themselves identify a Vista HAL or HPET bug.
The VM eventually shut down normally. A fresh QEMU process initially showed
about 4.9 guest seconds over five host seconds, but display stalls recurred.

Opt-in GTK timing diagnostics then measured roughly 998 ms per draw. A read-only
scan of code pointers on the blocked QEMU main-thread stack found
`xcb_wait_for_special_event`, `loader_dri3_get_buffers`, `driBindContext`,
`gd_egl_make_current`, `virtio_gpu_virgl_process_cmd`, and the QEMU main-loop
callers. This is a stack-pointer scan rather than a fully unwound backtrace.
Source inspection confirms the renderer callback binds the GTK window surface
for every forced VirGL context switch, even though renderer commands target
FBOs. Such a native-window acquisition can delay the device main loop.

The candidate host fix uses QEMU's existing surfaceless EGL helper for renderer
context binding and a surfaceless context for CPU texture uploads. The window
remains bound for actual drawing. It also requests swap interval zero after
window-surface creation, leaving redraw scheduling to GTK rather than waiting
for host vblank inside QEMU. This does not remove producer/consumer fences or
establish visible monitor FPS. The GTK translation unit and full QEMU executable build successfully. The
production-helper regression passes; the prior implementation compiles against
the same fixture and fails on its window-surface acquisition. Runtime comparison
remains pending while the old VM shuts down normally. The old executable inode
is preserved as `qemu-system-x86_64.audit22-before` so the rebuild does not
overwrite code mapped by that process.

Evidence under `vista-kvm/x64-base`: `audit21-clock-samples.json`,
`audit21-hpet-samples.json`, `audit21-cold-clock.json`, and
`audit22-main-stack-candidates.txt`. Package21 installed-binary, cursor and
fullscreen verification remain incomplete. Research and later optimization
requirements are recorded in `docs/PRESENTATION_OPTIMIZATION_RESEARCH.md`.


### Audit 21/22 runtime follow-up and persistent guest control

After booting the rebuilt QEMU, the installed package21 KMD compared byte-for-byte
with its media copy and the display device reported ConfigManagerErrorCode 0.
The host display diagnostics no longer showed the approximately 998 ms draw
stalls in the observed interval; idle updates took roughly 0.07–0.1 ms per draw.
This is not a measurement of unique visible frames or a complete performance
validation. Evidence: `audit22-driver-verification.log`,
`audit22-before-display.log`, and `audit22-after-display.log`.

The new host control service launched the fullscreen test in the user's actual
console session. It exited 0 and read back all three RGB values correctly;
independent captures showed red, green and blue. During green, CPU-visible VGA
memory now contained 920625 green pixels instead of 78, demonstrating that the
package21 download updates CPU backing. However, the green display still has
the cursor rectangle (99.8942% green). Thus CPU coherence improved, but the
cursor defect was not resolved; the full-primary copy cost still needs review.
Evidence: `control-fullscreen-proof.log` and `audit21-fixed-cursor-frames`.

`TritonVistaControl` provides persistent LocalSystem commands over COM2/private
host Unix socket, with an interactive-user launch option and durable job results.
It is separate from the disabled deployment helper. Minimal Safe Mode testing
first exposed a missing serial transport dependency: the service was running but
Serial was stopped. Whitelisting the driver filenames, Serenum filter and Ports
class made the channel work. The updated executable was then tested in Minimal
Safe Mode with SYSTEM identity, OptionValue 1, and both services running.
The normal boot entry was preserved using one-time Safe Mode boot entries.
See `docs/VISTA_CONTROL.md`, `control-safe-mode-final.log` and
`control-integration-results.json` for operation and verification. Proxy setup
was explicitly requested by the user to replace routine GUI command automation.

## Audit23: WinSAT D24X8 admission failure

On package21/host22, `winsat d3d -texshader -time 15` and the ALU variant
completed with XML FPS values 298.57 and 290.90, respectively. These are
render rates; neither proves visible presentation throughput. The graphics
assessment measured 257.44 F/s and 13623 MB/s, then failed device creation
for the texture phase. Vista reports process exit zero and even XML
HRESULT zero for these failures: `Valid=0` and the textual error are decisive.

Isolation: `-noalpha` alone changes the otherwise successful texture test
to `0x8876086a` at device creation; explicit 1280x1024 alone succeeds.
Adding `-z16` makes the no-alpha test succeed. Read-only inspection of the
guest WinSAT executable shows its depth-format default is 77 (D24X8) at
VA 0x100151b25; the `z16` option writes 80 at 0x100152a0d. The format table
advertised D16 and D24S8 but omitted D24X8. The malformed `C(500` argument
in the built-in texture command is a separate observation: correcting it
does not fix this rejection. No WinSAT binary was modified.

D24X8 now uses the existing D24_UNORM_S8_UINT host depth image while retaining
its logical D3D9 format. Depth view and clear validation recognize it, colour
views/mipmap generation reject it, and stencil clears remain invalid. Host
stencil testing is enabled only for a bound logical D24S8 surface. Binding or
destroying a depth surface invalidates the cached depth/stencil state, so
switching formats cannot expose padding as stencil. Shared depth resources
remain rejected by the existing resource contract. Canonical reverse host
format lookup still returns D24S8.

`tests/vista/test-d24x8-runtime.c` fails format admission on package21. With
package23 it passes eight rendered-pixel checks on each of x64 and x86:
far-depth rejection, near-depth pass, rectangular depth clear, preserved
depth outside the rectangle, and repeated stencil-enabled S8/X8 switches.
Both UMD builds pass Vista PE/import checks. Normal-mode pnputil installation
completed and both installed DLLs match the signed package byte-for-byte.
The package catalog signatures verify. The old INF checker still rejects
legacy deploy-helper StartType=4 because it demands auto-start; disabled is
intentional for this VM, now managed by TritonVistaControl.

Format reference: [Microsoft D3DFORMAT](https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dformat).
Evidence: `audit23-depth-baseline.log`, `audit23-depth-fixed.log`,
`audit23-byte-check.log`, `audit23-umd-build.log`, `audit23-sign.log`, and
`audit23-winsat-results.json` under `vista-kvm/x64-base`.

After a normal reboot, the exact failing `winsat d3d -texshader -noalpha
-time 5 -v` command completes: XML Valid=1, 756 frames, 85.09 render FPS
(74.87 effective). This is a coverage result, not a visible-FPS claim or
a comparison against the different alpha-enabled workload. Installed DLL
bytes still match the package, device error code is zero and DWM is running.

The broader graphics assessment now gets past the previous device-creation
rejection and instead fails to create a required texture. Its partial results
are 235.47 F/s desktop graphics, 12460.50 MB/s memory throughput and 211.60 F/s
alpha blend. It subsequently shows APPCRASH c0000374, StackHash_976c, offset
c87b7; the service timeout ends the job. A separate run with the corrected
`C(500)` argument reports the texture failure and exits normally. This does
not establish the heap-corruption cause. The verbose UMD trace for that
corrected run contains no failing resource-create DDI; the missing texture
capability still needs isolation. WinSAT fullscreen device creation also
still returns 0x8876086a, even at 1280x720. No overall score or full-refresh
performance fix is claimed. Evidence: `audit23-fixed-noalpha.{txt,xml}`,
`audit23-reboot-graphics.{txt,xml}`, `audit23-winsat-crash.png`,
`audit23-texture-ddi.log`, `audit23-final-fullscreen.{txt,xml}` and
`audit23-reboot-verification.log`.

## Audit24: signed texture support

WinSAT's texture setup cycles seven requested formats. Read-only inspection
of the guest binary locates the table at RVA 0x2a098: ARGB, XRGB, R5G6B5,
DXT5, A4R4G4B4, A2W10V10U10 and A2B10G10R10. With package23,
`winsat d3d -texshader -noalpha -totaltex 5 -time 5 -v` completes,
whereas six textures fail during texture creation before a resource-create DDI
failure appears. Earlier `-time 1` trials were invalid argument tests and are
not evidence about texture support.

The driver now advertises Q8W8V8U8 texture sampling using native
R8G8B8A8_SNORM storage. Signed interpretation happens before filtering; no
shader-side reinterpretation or CPU conversion is needed. This does not
advertise render-target support, legacy bump mapping or native A2W10V10U10
support. WinSAT's existing format fallback can now complete its six-texture
workload. The broader workload still requires separate validation.

`test-signed-texture-runtime.c` first rejects format admission on package23.
On package24 it passes five rendered ARGB pixel checks on each architecture:
signed endpoints including the fourth channel, linear filtering across the
sign boundary, partial update, and preservation of another row. A readonly
lock also verifies the original encoded bytes, including both -128 and -127.
All checks pass on x64 and x86. An ARGB target makes the fourth channel
independently observable through ordinary shader output and SYSTEMMEM readback.
Both UMD builds pass Vista PE/import checks and package signatures verify.

Evidence under `vista-kvm/x64-base`: `audit24-signed-baseline.log`,
`audit24-signed-fixed.log`, `audit24-fixed-six.{txt,xml}`,
`audit24-umd-build.log`, and `audit24-sign.log`.

After normal reboot both installed UMDs match package24 byte-for-byte, the
display device has error code zero, UxSms is running and dwm.exe is in the
console session. Both signed-texture tests pass again.
`winsat graphicsformal -wddm -v` now completes with all four XML Valid fields
set to 1 and HRESULT zero: desktop 261.118 F/s, alpha 227.26 F/s, texture
152.69 F/s and ALU 148.12 F/s; video memory throughput is 13817.60 MB/s.
Composition restarts normally and there is no observed assessment crash.
The built-in malformed-looking batch argument is unchanged, further showing
it was not the texture admission blocker. These are workload render rates,
not proof of screen-delivered refresh or a complete formal system score.
An initial `winsat graphics` invocation was an invalid assessment name; only
the subsequent `graphicsformal` result is counted. Evidence:
`audit24-reboot-verification.log`, `audit24-post-reboot-run.log`,
`audit24-graphicsformal.{txt,xml}` and `audit24-winsat-results.json`.

A separate package24 fullscreen run at 1280x720 still returns 0x8876086a
at device creation (`audit24-fullscreen.{txt,xml}`). Signed texture support
does not resolve that path. Full-refresh presentation and the fullscreen
cursor rectangle also remain open.

## Audit25: fullscreen mode negotiation

A new D3D9/D3D9Ex mode probe isolates WinSAT's 0x8876086a failure:
package24 enumerates only 300 Hz, creates at default/300 Hz, and rejects
explicit 60 Hz. Package25 adds 60 Hz target and monitor tuples alongside
the preferred EDID cadence, including a pinned source raster. CommitVidPn
reads the pinned target rate and the vblank worker uses that committed rate.
This preserves the preferred desktop cadence while allowing legacy clients
to request 60 Hz. Both target and monitor enumeration avoid duplicate 60 Hz
entries when it is already preferred.

The x64 KMD builds and passes Vista PE checks. After normal install/reboot,
the probe creates and presents in all six combinations (D3D9/D3D9Ex,
default/60/300 Hz). The previously failing `winsat d3d -texshader -fullscreen
-width 1280 -height 720 -time 5 -v` now completes setup and its render workload.
Evidence: `audit25-fullscreen-modes.log` (baseline),
`audit25-fullscreen-fixed.log`, `audit25-fullscreen.txt`,
`audit25-kmd-build.log`, and `audit25-sign.log`. Refresh throughput and
cursor validation remain separate work.

## Audit26: independent color cursors

The visible/hidden cursor comparison isolates the opaque rectangle: package25
has 920547 correct green pixels of 921600 with the animated cursor visible,
and 921600 correct pixels with it hidden. The fullscreen frame itself is
correct. The KMD now advertises 64x64 color cursors, validates ARGB shape
bounds/pitch/hotspot, clears padding on smaller shapes, waits for upload
completion before publishing the cursor and for cursor consumption before
reusing backing. Motion and visibility use the separate cursor queue.
Unsupported monochrome/masked shapes retain the software fallback.

Timeouts retain backing until device reset, with heap-owned wait state for
late callbacks. The cursor queue now handles page-crossing commands and
negative queue errors instead of reporting unconditional success. Its DPC
fires completion callbacks exactly once. Both x64/x86 KMDs pass Vista PE
checks; the native production queue test passes split-page and full/closed
queue cases. Package26 was installed and rebooted normally.

The same visible and hidden fullscreen tests now have all 921600 correct
pixels in red, green and blue, including motion across the green frame.
Host traces confirm UPDATE_CURSOR/MOVE_CURSOR with a nonzero resource and
resource zero for hiding. QMP screenshots exclude the hardware cursor layer;
they verify that the primary stays clean, rather than proving cursor shape
quality. Evidence: `audit25-cursor-comparison/results.json`,
`audit26-cursor-comparison/results.json`, `audit26-cadence-host-baseline.log`,
`audit26-kmd-build.log`, `audit26-kmd-x86-build.log`, `audit26-sign.log`.

The host control client also rejects concurrent connections before opening
the serial chardev, preventing independent requests from interfering. Six
protocol tests pass, including this exclusion. Two cadence attempts timed
out and are not accepted throughput measurements; the diagnostic now bounds
its message pump so incoming messages cannot starve rendering.

## Audit27–28: publication order and display storage reuse

Normal package28 contains the mode and cursor fixes, the corrected cursor
hotspot coordinates, and bind-before-flush for ordinary primaries. The old
ordinary path flushed an unbound flip-chain resource before selecting it;
the host publishes RESOURCE_FLUSH only to an already bound scanout. Blob
primaries already used the correct order. Both x64/x86 KMD builds pass Vista
PE checks; installed x64 KMD and both UMD DLLs match the signed package bytes.
The device reports error code zero and UxSms is running.

The host now reuses display storage for ordinary scanouts as well as blobs,
and performs its readback at flush. A trace found alternating pixman
0x20028888/0x20020888 surfaces despite the same raster. Display-only storage
now uses opaque XRGB/XBGR for the corresponding ARGB/ABGR layouts, avoiding
texture reallocation on each transition. Blit snapshots retain their source
alpha semantics. The production-helper test verifies reuse over 100 alpha/
padding transitions and separate allocation for channel-layout/size changes.

The 30-second changing-frame diagnostic improved from 114.00 guest FPS and
about 61 host redraws/s to 149.75 guest FPS and 149.8–158.0 host redraws/s.
Steady surface switches fell to zero. The intermediate bind-order run reached
about 114 host redraws/s while still reallocating surfaces. These are guest
completion and host draw rates, not a measurement of physical monitor frame
delivery. The host monitor is approximately 299.73 Hz; full 300 Hz delivery
remains unverified and the continuous workload is still below that rate.

Final validation on KMD28/host28: all six D3D9/D3D9Ex default/60/300 Hz
creation and Present cases succeed. Fullscreen texture WinSAT has Valid=1
and 210.79 FPS (effective 202.47). All four graphicsformal phases have Valid=1:
graphics 254.683, alpha 220.98, texture 156.67, ALU 145.66 FPS. All held RGB
frames contain 921600/921600 expected pixels with visible and hidden cursors,
including cursor motion. Host cursor motion now reports exactly the requested
519,200 position rather than subtracting its hotspot.

Evidence in `vista-kvm/x64-base`: `audit28-verify.log`,
`audit28-wow-verify.log`, `audit28-modes.txt`, `audit28-final-fullscreen.xml`,
`audit28-graphicsformal.xml`, `audit28-reuse-cadence.txt`,
`audit28-host-final.log`, `audit28-cursor-hotspot.log`, and
`audit28-cursor-comparison/results.json`. Host binary SHA256:
`13e5b9fd3991a20dc8658c6174f687a7b5fec55d3cda640416ccacac1f24bf05`.

Separate unresolved behavior: legacy windowed D3D9 can wait before entering
the UMD Present callback. A suspended-thread stack locates the wait inside
dwmapi called by d3d9, with composition enabled; minimizing Media Center did
not resolve it. The bounded message pump therefore did not fix that wait.
See `audit27-window-isolated-stack.txt` and `audit27-dwm-timing.txt`.
One warm reboot required restarting the host-control service through the
guest's elevated console; the subsequent cold boot restored it normally.
