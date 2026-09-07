# PresentEx and scanout implementation

## Result and proof boundary

The current integrated source has one complete Triton Present path. It preserves the rendered source identity through the UMD, KMD, QEMU, and renderer.

This is a source and host-build result. It is not a Vista guest result.

The last public run used deployment `b4bf74b765f6875bdf4542a14d0257f0715b7968afc637d2844bed4dfbeb9fb4`. Its probe process was PID `000008a4`, with build marker `20260816`.

That run stopped at `Clear D24S8 depth` with `0x80004005`. It did not start the `PresentEx` stage.

These facts come from `notes/unlazy/offline-evidence.md:13,220-263` and `notes/unlazy/diagnosis.md:30-32`. Therefore, no public Present pass or failure exists yet.

The final x64 UMD has SHA-256 `160b586cce6967badaed8cc012ed39529ab1453842367c23516f230f9bf9f696`. The final x86 UMD has SHA-256 `60b4566ae0baffdc7a83213aaf8551a7df0a1b1e05bb4198e2a75372a2b61339`.

Those values do not match the installed scoped UMD values in `notes/unlazy/architecture-audit.md:78-96`. Current source is not deployed-era source.

The deployed KMD, QEMU, virglrenderer, and DXMT source identities are also unknown. This report does not infer current behavior from those artifacts.

## Scope and provenance

`PLAN.md:30` assigns this leaf its exact UMD, KMD, QEMU, and virglrenderer files. No source outside that list changed for this leaf.

The repository heads during the final pass were:

| Repository | HEAD |
|---|---|
| `triton-umd` | `7432d34c2bc10c602d72b1ad4058cde98549f98c` |
| `triton-kmd` | `74cb98d15f6cb9ca44d9b6ecc6e47a2236eeb2d3` |
| `triton-qemu` | `7311c3651c3a2cbc3d32e6eae262c60339f28d79` |
| `triton-virglrenderer` | `65cc14eb896f121ffc5130ce04815a923a03c41d` |

The KMD owned files had large changes before this leaf started. QEMU's virgl and GL files also had earlier changes.

The UMD shared-texture helper and virglrenderer shared helper were clean at that boundary. The current worktree combines this leaf with earlier integrated work.

For this reason, a complete `git diff` is not a leaf-only patch. The cited current symbols are the implementation authority.

## End-to-end Triton path

### Vista runtime and UMD

Fact: The Vista runtime calls the UMD `triton9Present` symbol. It is in `triton-umd/src/virtio/neptune/vista-d3d9/triton9_resource.c:1778-1851`.

The UMD validates source and destination ownership at lines `1787-1795`. It materializes host resources at lines `1796-1811`.

It rejects missing allocations, callbacks, contexts, and host objects at lines `1812-1826`. It never converts those cases to success.

The UMD flushes D3D11 work at line `1829`. It drains ordered Neptune and GPU work at lines `1830-1839`.

It then creates the kernel context and calls `pfnPresentCb` at lines `1840-1848`. `tritonSharedBridgeDrain` defines the ordered drain in `triton-umd/src/virtio/neptune/triton/tritonSharedBridge.c:40-57`.

### Runtime callback and KMD Present

Fact: `DriverEntry` registers `VioGpu3DPresent` in `triton-kmd/viogpu/viogpu3d/driver.cpp:151`. The wrapper is at lines `1299-1329`.

`VioGpuDevice::Present` starts at `viogpu_device.cpp:894`. It clears recycled private data before every exit at lines `914-924`.

The flip form accepts only flag value `0x4`. It validates the primary source at lines `926-961`.

The blit form accepts only flag value `0x1`. It requires DMA storage and dirty rectangles at lines `976-980`.

Color fill, color keys, linear-to-sRGB, rotation, and no-wait flip return `STATUS_NOT_SUPPORTED`. See lines `1109-1125`.

The KMD validates both allocation handles and device ownership at lines `1001-1013`. It requires both output patch entries before writing either entry.

The patch capacity check is at lines `983-999`. The two patch entries are written at lines `1026-1036`.

`AttachAllocations` holds source and destination lifetime through command retirement. See `viogpu_command.cpp:597-659`.

### Rectangles, pitch, and format

`GenerateBltPresent` starts at `viogpu_device.cpp:120`. It requires positive source, destination, and dirty rectangles.

It supports translation-only copies. The source and destination extents must match at lines `170-201`.

Every dirty rectangle must fit the destination rectangle and resource. Its translated source must also fit, as lines `203-227` require.

The direct-host path requires a non-blob standard primary destination. It also requires an exact-size host presentation source at lines `237-267`.

Other Vista blits preserve the dirty region. Fixed-primary copies validate each row in `viogpu_allocation.cpp:1867-1990`.

Shared source descriptors accept one linear plane. They accept only supported 32-bit formats, exact pitch, and bounded offsets.

The KMD validates those fields in `IsValidVistaSharedTexture`, at `viogpu_allocation.cpp:235-328`. The UMD validates export identity at `npt_shared_texture.c:27-114`.

QEMU validates blob width, height, format, pitch, offset, bounds, and unused planes. See `virtio-gpu.c:725-785`.

### DMA, host completion, and status

`VioGpuCommand::Run` starts at `viogpu_command.cpp:204`. It validates each private command size before execution at lines `209-248`.

Submit and transfer commands return to the worker only after their host callbacks. See lines `253-312` and `QueueRunningCb` at lines `703-743`.

Local allocation or enqueue failures retain their exact `NTSTATUS`. `RecordIssueFailure` corrects the callback's generic status at lines `137-151`.

Host response types map to `NTSTATUS` in `QueueRunningCb`, at lines `703-727`. The first error wins and faults the DMA fence.

Successful work reports `DXGK_INTERRUPT_DMA_COMPLETED`. Failed work reports `DXGK_INTERRUPT_DMA_FAULTED` with the fence status.

That conversion is in `VioGpuCommand::NotifyCompletion`, at `viogpu_command.cpp:672-701`. No failed command reports a completed fence.

### Scanout ownership and synchronization

The direct-host source changes only after its Present DMA retires. The KMD records that completion at `viogpu_device.cpp:1082-1105`.

The command applies the source before releasing allocation references and reporting the fence. See `viogpu_command.cpp:550-590`.

An authoritative VidPN change advances a source generation. `SetVidPnSourceAddress` does this at `viogpu_vidpn.cpp:2941-2986`.

A delayed Present can apply only within its captured generation. The guarded change is at `viogpu_vidpn.cpp:3040-3095`.

This guard prevents an old Present from replacing a recreated or reset source. Ordered Presents in one generation can still advance normally.

`TryPromoteFlip` holds a source reference while it programs scanout. It starts at `viogpu_vidpn.cpp:2717`.

`FlushToScreen` validates the complete scanout descriptor at `viogpu_allocation.cpp:1118-1212`. It sends `RESOURCE_FLUSH` before `SET_SCANOUT`.

Each control command waits for its actual host response. `VioGpuQueueControlSync` is at `viogpu_allocation.cpp:977-1029`.

The helper retains its wait context and buffer after timeout. Flush and scanout each use a half-second wait, so each promotion attempt has a bounded wait.

`TryPromoteFlip` publishes the displayed address only after both host replies succeed. See `viogpu_vidpn.cpp:2757-2798`.

`Flip` reports the displayed address, not an unprogrammed latch. See `viogpu_vidpn.cpp:2800-2841`.

### QEMU and renderer backend

QEMU owns each scanout through a resource bitmask. `virtio_gpu_update_scanout` changes old and new ownership at `virtio-gpu.c:573-595`.

`virtio_gpu_disable_scanout` clears the bit, surface, GL state, geometry, framebuffer, and resource identity. See lines `375-395`.

Resource destruction disables all attached scanouts before freeing storage. The common path is at lines `397-416`.

The virgl path performs the same operation before renderer unreference. See `virtio-gpu-virgl.c:282-311` and lines `419-461`.

Neptune scanout uses a format-correct QEMU surface. It reads the selected host resource through `virtio_gpu_neptune_readback_surface` at lines `521-604`.

The first scanout reads the complete surface before publication. `virtio_gpu_neptune_create_surface` implements this at lines `606-631`.

Later resource flushes clip the update to the active scanout. They propagate renderer readback errors at lines `633-714`.

Standard and blob scanout publish the new surface only after validation and readback. See lines `716-840` and `1325-1407`.

QEMU maps renderer errors to virtio response errors in `virgl_status_to_virtio_error`, at lines `30-41`. KMD maps those responses to `NTSTATUS`.

The renderer validates shared D3D11 descriptions in `triton-virglrenderer/src/neptune/npt_shared.c:95-137`. Unsupported layouts fail.

OPEN_RES duplicates the resource file descriptor while holding the resource lock. See `npt_shared_wait_and_dup_resource` at lines `139-187`.

It validates the device, descriptor, resource identity, import, and minted object. See `npt_shared_open_res` at lines `360-477`.

## Present contract matrix

| Field | Current contract | Failure result |
|---|---|---|
| Flags | Exact blit `0x1` or exact flip `0x4` | `STATUS_NOT_SUPPORTED` |
| Rectangles | Positive, bounded, translation-only, and every dirty rectangle mapped | `STATUS_INVALID_PARAMETER` or `STATUS_NOT_SUPPORTED` |
| Color | Resource format is preserved; no Present color-fill operation | `STATUS_NOT_SUPPORTED` |
| Rotation | No rotation path is advertised by Present | `STATUS_NOT_SUPPORTED` |
| Source | Same device, valid allocation, retained through retirement | `STATUS_INVALID_PARAMETER` |
| Destination | Same device and exact primary identity for direct scanout | `STATUS_INVALID_PARAMETER` |
| Patch list | Two entries required before either entry is written | `STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER` |
| Shared handle | One supported linear plane with a stable duplicated file descriptor | HRESULT or virtio error |
| Pitch and offset | Full-row pitch and last-row bounds checked at each boundary | Invalid argument or parameter |
| Host scanout status | Renderer error becomes a virtio error and then `NTSTATUS` | Displayed state is not published; retry remains armed |

The direct-host path uses a full-surface flush after the UMD drain. It does not lose pixels outside the public dirty list.

The fixed-primary path copies only validated dirty rectangles. Its first primary update can bootstrap one complete frame.

The scanout response arrives on the flip thread after Present DMA retirement. It cannot change an earlier public Present result. It returns an exact `NTSTATUS`, blocks displayed-state publication, records the failure, and keeps the retry armed.

## Reset, loss, and teardown

`VioGpuVidPN::Powerdown` stops the flip thread, advances the generation, and disables host scanout. See `viogpu_vidpn.cpp:294-328`.

It clears source and displayed identities before it destroys framebuffer state. Delayed Present completions from an older generation cannot relatch them.

QEMU reset disables every scanout in `virtio_gpu_virgl_reset_scanout`, at `virtio-gpu-virgl.c:1688-1695`. GL reset calls it at `virtio-gpu-gl.c:105-120`.

Resource unreference removes the scanout before renderer ownership ends. Recreate failures leave the previous scanout published.

The UMD marks transport-drain or host-device failure as device removal. See `triton9_resource.c:1828-1839`.

Fact: These paths are present in current source. Inference: they prevent stale source publication when the integrated binaries use this source.

Falsification: a repeated guest loop must change sources, reset the device, and destroy resources while checking every Present result.

## Focused checks

The following command rebuilt the QEMU x86-64 target:

```text
meson compile -C triton-qemu/build qemu-system-x86_64
```

Result: pass. The edited `virtio-gpu-virgl.c` compiled and the target linked.

The following command ran the display test:

```text
meson test -C triton-qemu/build --no-rebuild --print-errorlogs 'qemu:qtest-x86_64/display-vga-test'
```

Result: `1/1` pass, with `6` subtests.

The following command rebuilt virglrenderer:

```text
meson compile -C triton-virglrenderer/build-arm64
```

Result: pass. It produced 28 existing generated-helper unused-parameter warnings. The edited file added no warning.

`meson test -C triton-virglrenderer/build-arm64 --no-rebuild --print-errorlogs` reported `No tests defined`.

Both UMD suites ran with these commands:

```text
meson test -C triton-umd/build-vista-x64-unified --no-rebuild --print-errorlogs
meson test -C triton-umd/build-vista-x86-unified --no-rebuild --print-errorlogs
```

Each suite reported four passes, zero failures, and one expected zlib skip.

Both PE audit routes passed for both architectures:

```text
triton-umd/build-support/audit-vista-d3d9-pe.sh x64 triton-umd/build-vista-x64-unified/src/virtio/neptune/vista-d3d9/neptune_d3d9.dll
triton-umd/build-support/audit-vista-d3d9-pe.sh x86 triton-umd/build-vista-x86-unified/src/virtio/neptune/vista-d3d9/neptune_d3d9.dll
python3 triton-kmd/viogpu/tools/check_vista_pe.py --kind umd --arch x64 triton-umd/build-vista-x64-unified/src/virtio/neptune/vista-d3d9/neptune_d3d9.dll
python3 triton-kmd/viogpu/tools/check_vista_pe.py --kind umd --arch x86 triton-umd/build-vista-x86-unified/src/virtio/neptune/vista-d3d9/neptune_d3d9.dll
```

`python3 triton-kmd/viogpu/tools/check_vista_kmd_source.py` passed after the final KMD changes. All nested repository `git diff --check` commands passed.

The local host has no `msbuild` command. Therefore, this leaf did not produce new x86 or x64 KMD binaries.

The required Windows commands remain:

```text
msbuild viogpu_vista.sln /m /p:Configuration="Vista x86" /p:Platform=Win32
msbuild viogpu_vista.sln /m /p:Configuration="Vista x64" /p:Platform=x64
```

The documented route is `triton-kmd/viogpu/BUILDING_VISTA.md:30-38`. The parent must run that route after shared source integration settles.

## Adversarial passes

The Vista D3D9 reread traced the runtime callback, both Present forms, patch entries, dirty rectangles, and HRESULT-to-NTSTATUS boundaries.

The return-path hunt found and fixed generic local queue errors. It also found and fixed stale post-reset source relatching.

The ownership hunt verified source references, displayed-address publication, QEMU scanout bitmasks, and shared file-descriptor duplication.

The free-polish pass rebuilt QEMU and virglrenderer. It reran both UMD suites, PE audits, the KMD source audit, and whitespace checks.

No owned-file probe shortcut remains. No Windows component patch exists. VirtualBox remains a behavioral reference only.

No VM control action occurred. This leaf did not start, stop, drive, mutate, or inspect a VM or disk.

## Remaining decisive proof

Two proof items remain outside the local host result:

1. Build and audit the current KMD for Vista x86 and x64 through the Windows route.
2. Run the unchanged guest-owned public probe after clear and triangle pass.

The public transcript must record `PresentEx` success and additional-swap-chain success. It must then repeat Present across reset, loss, and teardown cases.

Any failure must retain the exact HRESULT, KMD fault status, virtio response, and renderer return. A timeout or stale surface falsifies completion.

Until those checks pass, gates G4 and G5 remain open. This report makes no Aero, glass, or visible-rendering claim.
