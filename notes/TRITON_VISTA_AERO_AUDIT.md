# Triton Vista Aero Code Audit

Status date: 2026-08-12.

## Result

The Vista x86 and x64 packages now build, link, sign, and pass the static audits.

This result does not prove that Aero works. No current test has opened the new D3D9 UMD in Vista. No current test has rendered a frame through Neptune.

The review found three remaining design defects that block acceptance:

1. The KMD reports success for paging operations that do not copy or fill data.
2. The TDR restart callback reports success but does not restart the transport.
3. A shared-resource importer does not keep the exporting resource alive.

The first controlled guest run can still test driver startup and a basic D3D9Ex path. It cannot count as an acceptance run until these defects are fixed.

## Verified status

| Area | x86 | x64 | Result |
|---|---:|---:|---|
| `neptune_d3d9.dll` build | Pass | Pass | Static build gate complete |
| `viogpu3d.sys` build | Pass | Pass | Native WDK build complete |
| PE machine and subsystem 6.0 | Pass | Pass | Static audit complete |
| UMD exports and import allowlist | Pass | Pass | Static audit complete |
| KMD import allowlist | Pass | Pass | Static audit complete |
| INF registration | Pass | Pass | Static audit complete |
| Catalog generation and test signing | Pass | Pass | Package gate complete |
| Source and package manifests | Pass | Pass | Package gate complete |
| Vista driver start | Not run | Not run | Runtime gate open |
| D3D9Ex device and draw | Not run | Not run | Runtime gate open |
| DWM composition | Not run | Not run | Runtime gate open |

Build the complete package with:

```sh
./scripts/build_deploy_vista_driver.sh --build-only
```

The current packages are in:

- `/Users/jumar/aaaaa/vista-signing-transfer/vista-driver-x86-pnp-current`
- `/Users/jumar/aaaaa/vista-signing-transfer/vista-driver-x64-pnp-current`

## Audit scope

The review covers these components:

- the Vista WDDM 1.0 callback table;
- adapter, device, context, and allocation lifetime;
- VidPn, scanout, present, paging, interrupt, and DMA paths;
- virtqueue ownership and command retirement;
- the private UMD/KMD ABI and render-event packet;
- D3D9 resources, states, queries, draws, locks, and presentation;
- the D3D9On12 shader converter bridge;
- the guarded fixed-function fallback;
- x86, x64, and WoW64 build and package rules;
- direct-disk deployment and staged guest checks.

The review does not change the Neptune wire protocol.

## Corrections completed in this review

### Build and package

- The build now copies the complete KMD source set to Windows 11.
- One command builds both KMDs and both UMDs.
- The build creates and signs both Vista package catalogs.
- Each package contains a source manifest and an artifact manifest.
- Deployment replaces every package file and checks its hash.
- The x64 INF registers the native and WoW64 D3D9 UMDs only.
- The x86 INF registers the native D3D9 UMD only.
- The PE audits check architecture, subsystem, exports, and DLL imports.

### Native x86 KMD ABI

- The x86 KMD and VirtIO library now use the WDK standard-call convention.
- The x86 builds define `_X86_`; the x64 builds define `_AMD64_`.
- `VioGpu3DSetVidPnSourceAddress` now has the required `APIENTRY` convention.
- Allocation DPC and work-item callbacks now match the WDK callback types.
- The x86 response file links the WDK buffer-overflow support library.
- Static checks now protect these build and callback requirements.

These defects were hidden on x64 because its ABI uses one calling convention.

### Vista KMD contract

- The driver registers through `DxgkInitialize` with the Vista interface.
- The Vista callback table omits WDDM 1.3-only callbacks.
- Segment queries use the Vista `DXGKQAITYPE_QUERYSEGMENT` contract.
- Vista allocation wrappers use `NonPagedPool` and `ExAllocatePoolWithTag`.
- The private adapter query supports both the V1 and V2 layouts.
- V2 reports the private ABI version and render-event feature bit.
- The adapter-info output is cleared before the driver fills it.
- Escape payload sizes and ring indexes receive explicit validation.
- Source and target modes use the Vista-compatible sRGB color basis.
- The Vista caps report one node and finite allocation-list limits.

### DMA, queue, and event ownership

- Render and present packets receive exact size and boundary checks.
- Unknown commands and malformed fixed-size packets fail closed.
- Render honors `MultipassOffset` and commits output only after a full packet fits.
- Present checks rectangle counts, size arithmetic, and packet writes.
- Each WDDM context now owns a distinct KMD context object.
- A context can submit commands only through its owning device.
- The render-event packet contains a fixed 64-bit handle field.
- The KMD references the event in the submitting process context.
- Retirement, cancellation, reset, and removal release the reference once.
- Queue submission functions return failures to their callers.
- Failed submissions undo pending command and fence accounting.
- Queue scatter entries now honor page offsets and page boundaries.
- Backing attachment validates the complete MDL range.
- Teardown waits for the command and display workers to terminate.

### Allocation and resource handling

- Allocation creation handles every entry in the allocation array.
- A partial allocation failure rolls back earlier entries.
- Size arithmetic rejects values that cannot fit on native x86.
- Segment selection checks whether host shared memory exists.
- Resource and allocation private data have separate validation paths.
- The UMD cleans up failed KMD allocation and import operations.
- UMD resource callbacks check device ownership.
- Shared metadata uses the central format data for size and pitch checks.

### D3D9 UMD and shader bridge

- `OpenAdapter` is undecorated and reports the Vista D3D9 interface.
- The UMD requires the V2 ABI and render-event feature.
- It reports SM3, one render target, two texture stages, and tested stream limits.
- Resource, lock, rename, state, draw, clear, query, and present paths exist.
- Failed output binding restores the previous render and depth targets.
- The Win32 transport checks active reset and removal states.
- Failed render submission preserves the prior command-buffer state.
- A failed shared import drain now fails and cleans up the import.
- Device teardown checks all required destruction callbacks.
- The D3D9On12 converter builds as a static part of the UMD.
- The converter does not use the D3D12 Translation Layer backend.
- D3D9 token streams pass through the shared DXBC container builder.
- Normalized signature data feeds shader and input-layout creation.
- Shader variants include linked shader and declaration state in their keys.
- Unsupported active fixed-function stages fail with a diagnostic.

## Remaining acceptance blockers

### KMD-1: Paging commands report false success

`VioGpu3DBuildPagingBuffer` accepts operations that it does not execute.

- `DXGK_OPERATION_FILL` does not fill memory.
- `DXGK_OPERATION_TRANSFER` logs that content is lost.
- Physical read and write operations return success as no-ops.

VidMm can rely on these operations during memory pressure. A false success can expose stale data or lose allocation contents.

Required fix:

- implement each operation that Vista can request; or
- change the segment and allocation model so Vista cannot request it;
- reject any operation that remains unsupported.

Acceptance gate: force paging under memory pressure and verify resource contents after eviction and restore.

### KMD-2: TDR restart does not restart the device

`VioGpu3DDdiResetFromTimeout` cancels waits and resets the virtio device. That reset disables the queues.

`VioGpu3DDdiRestartFromTimeout` then returns success without rebuilding the transport, queues, workers, or device state.

Required fix:

- define one transport stop path and one transport start path;
- use them for PnP, power, and TDR recovery;
- restore queue interrupts and workers before restart returns success;
- invalidate or rebuild host objects after the virtio reset;
- make every pending UMD wait return device lost.

Acceptance gate: trigger a controlled timeout, recreate the D3D9Ex device, and render again without reboot.

### KMD-3: Shared imports do not retain the owner

An imported allocation adopts the exporter's host resource ID. It does not retain an adapter-owned shared-resource record.

The exporter can destroy the host resource while an importer still uses it. The resource ID can also return to the allocator too early.

Required fix:

- add an adapter-owned record for each shared resource;
- track the owner and every importer;
- destroy the host resource only after the last reference closes;
- remove stale records during process exit, reset, and device removal.

Acceptance gate: pass producer-first and consumer-first exit tests, stale-handle tests, and repeated open and close tests.

### UMD-1: The shader and proxy path has not executed

The converter and UMD pass compilation and PE checks only. No test has executed converted DXBC on the Neptune D3D11 proxy.

Required test coverage:

- valid and malformed SM1, SM2, and SM3 token streams;
- declaration and FVF input signatures;
- `POSITIONT` conversion and half-pixel behavior;
- float, integer, Boolean, and inline constants;
- pixel signatures linked to each vertex variant;
- premultiplied alpha and the two-stage fixed-function subset;
- null-stage rejection counts;
- shader cache lifetime and reset behavior.

Acceptance gate: all local DWM fixtures and independent synthetic fixtures create and draw correctly.

## Other open risks

These issues do not all block the first startup test. They do block the final acceptance run.

- Scaling blits are not implemented.
- Explicit mip chains are not implemented.
- Rectangle D24S8 and stencil clears are not implemented.
- Device reset does not yet recreate every UMD resource.
- Host command errors do not propagate to a specific DMA fence.
- Preemption completion can race with work that the host still executes.
- Shared-resource process-exit cleanup has no runtime coverage.
- The import audit checks DLL names, not every Vista SP2 symbol export.
- One generic queue payload owner uses an untyped array delete contract.
- Detach-backing teardown does not report a host failure to its caller.

Do not advertise a format or operation until its create, lock, draw, copy, and destruction cases pass.

## Controlled guest gate order

Use the exact disk at `/Users/jumar/winvistachecked/winvista-3.shrunk.qcow2`.

Run the single workflow with:

```sh
./scripts/build_deploy_vista_driver.sh --all
```

The workflow must stop at the first failed gate:

1. Package and installed-file hashes match.
2. `DriverEntry` completes.
3. `StartDevice` completes.
4. `CommitVidPn` selects an active mode.
5. The desktop remains usable with the Triton display driver.
6. A D3D9Ex HAL device opens through `neptune_d3d9.dll`.
7. A triangle renders and reads back with the expected pixels.
8. A private render event retires in submission order.
9. Present reaches the KMD scanout path.
10. Cross-process shared textures pass the lifetime matrix.
11. DWM starts and `DwmIsCompositionEnabled` returns true.
12. Theme, mode, thumbnail, Flip 3D, and stress tests pass.

Only gate 11 proves that Aero composition is enabled. A successful driver installation or desktop mode does not prove Aero.
