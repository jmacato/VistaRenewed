# Vista D3D9 UMD and Shader Bridge Task List

Status date: 2026-08-12.

The source and package foundation is complete. The D3D9 and shader paths still need executable tests.

See `TRITON_VISTA_AERO_AUDIT.md` for the defect analysis and guest gate order.

## Current status

- [x] Build both KMD architectures in the Windows 11 build VM.
- [x] Build both D3D9 UMD architectures from the same source state.
- [x] Generate and test-sign the x86 and x64 packages.
- [x] Pass architecture, subsystem, export, import, INF, and manifest checks.
- [x] Advertise the implemented D3D9 SM3 and draw subset.
- [ ] Execute the converter and shader bridge against the host proxy.
- [ ] Fix KMD paging operations that report false success.
- [ ] Implement complete TDR transport restart.
- [ ] Retain shared-resource owners across importing processes.
- [ ] Pass the controlled Vista D3D9Ex probe.
- [ ] Enable and verify DWM composition.

## Build and package foundation

- [x] Add isolated Vista x86 and x64 UMD targets.
- [x] Add isolated Vista x86 and x64 KMD targets.
- [x] Use the Vista DDI and WDDM 1.0 interface values.
- [x] Use subsystem version 6.0 for the UMD.
- [x] Use the Vista-compatible static support libraries.
- [x] Remove system `d3d11.dll` and `dxgi.dll` imports from the UMD.
- [x] Reject `api-ms-win-*` imports.
- [x] Export undecorated `OpenAdapter` only.
- [x] Return `D3D_UMD_INTERFACE_VERSION_VISTA`.
- [x] Synchronize the complete KMD source tree before each Windows build.
- [x] Build both UMD files during the package workflow.
- [x] Build and sign both native KMD files.
- [x] Generate and sign both package catalogs.
- [x] Generate source and artifact manifests.
- [x] Register only the D3D9 UMD names in each INF.
- [x] Add x86 callback-convention and linker regression checks.
- [x] Add one build and deployment entry point.

Use these commands:

```sh
./scripts/build_deploy_vista_driver.sh --build-only
./scripts/build_deploy_vista_driver.sh --deploy-only
./scripts/build_deploy_vista_driver.sh --all
```

## Private UMD/KMD ABI

- [x] Keep the existing V1 adapter-info prefix.
- [x] Add the sized `VIOGPU_ADAPTERINFO_V2` structure.
- [x] Add a private ABI version and feature bits.
- [x] Add `VIOGPU_FEATURE_RENDER_EVENT`.
- [x] Select V1 or V2 from the caller's private-data size.
- [x] Add a fixed 64-bit event-handle field to the signal packet.
- [x] Add x86 and x64 size and offset assertions.
- [x] Validate packet size and event ownership in the KMD.
- [x] Store a referenced event object instead of the user handle.
- [x] Signal and release the event when the serialized command retires.
- [x] Wake the waiter during cancellation, reset, removal, and teardown.
- [x] Use a finite UMD wait and map reset or removal to device lost.
- [ ] Run ordered completion tests on x86, x64, and WoW64.
- [ ] Run cancellation, reset, and repeated drain tests.

## Vista WDDM 1.0 KMD

- [x] Register with `DXGKDDI_INTERFACE_VERSION_VISTA`.
- [x] Use the Vista callback table.
- [x] Remove WDDM 1.3-only callbacks from the Vista path.
- [x] Use `DXGKQAITYPE_QUERYSEGMENT`.
- [x] Use Vista pool allocation APIs and flags.
- [x] Report one scheduler node and finite allocation-list limits.
- [x] Validate render and present packet boundaries.
- [x] Support render multipass input and checked output writes.
- [x] Process all entries in an allocation-create request.
- [x] Roll back partial allocation failures.
- [x] Reject x86 size truncation and invalid segment selection.
- [x] Use one KMD object per WDDM context.
- [x] Enforce device and context ownership.
- [x] Build page-correct dynamic scatter lists.
- [x] Propagate queue submission failures.
- [x] Drain command and display workers during teardown.
- [ ] Implement or prevent every requested paging operation.
- [ ] Rebuild queues, workers, and device state after TDR reset.
- [ ] Propagate host command errors to the correct DMA fence.
- [ ] Prove safe preemption behavior.
- [ ] Replace the untyped queue payload deletion contract.
- [ ] Report detach-backing failures where recovery is possible.

## D3D9 adapter and device

- [x] Use driver-owned adapter and device handles.
- [x] Require the V2 ABI and render-event feature.
- [x] Fill the Vista adapter and device callback tables.
- [x] Report SM3, stream limits, one render target, and two texture stages.
- [x] Keep video, overlay, and patch feature families disabled.
- [x] Return `D3DDDIERR_NOTAVAILABLE` for unsupported required operations.
- [x] Create Neptune D3D11 device and context proxies internally.
- [x] Avoid host swapchain creation.
- [x] Check device reset and removal at flush and presentation boundaries.
- [ ] Recreate all required UMD state and resources after a device reset.
- [ ] Execute a native x86, native x64, and WoW64 D3D9Ex device probe.

## Resources and sharing

- [x] Use a central D3D9-to-DXGI format table.
- [x] Use the table for pitch, allocation, lock, and view checks.
- [x] Map default resources to host resources.
- [x] Use CPU shadows and staging resources for lockable data.
- [x] Support readback and partial write preservation.
- [x] Support discard rename for eligible resources.
- [x] Add render-target, depth, texture, buffer, and primary resources.
- [x] Add same-format full-surface and rectangle copies.
- [x] Add color clears and rectangle D16 depth clears.
- [x] Add linear auto-mipmap generation for hidden mip chains.
- [x] Validate imported shared metadata with the format data.
- [x] Clean partial resource and import failures.
- [x] Check resource ownership in UMD callbacks.
- [ ] Add an adapter-owned shared-resource lifetime record in the KMD.
- [ ] Add scaling blits.
- [ ] Add explicit mip-chain handling.
- [ ] Add rectangle D24S8 and stencil clears.
- [ ] Test create, lock, render, copy, and destruction for each advertised format.
- [ ] Test producer-first and consumer-first shared-resource exit.
- [ ] Test repeated open, close, stale handle, reset, and process exit.

## Draw and state paths

- [x] Add vertex declarations and FVF conversion.
- [x] Add standard stream and index-buffer binding.
- [x] Add reusable dynamic buffers for UP draws.
- [x] Add indexed and non-indexed draws.
- [x] Add instanced stream-frequency handling.
- [x] Add render-target and depth-target binding.
- [x] Restore prior outputs after a failed bind.
- [x] Add viewport, depth range, and scissor state.
- [x] Add cached blend, depth, rasterizer, sampler, and texture-stage state.
- [x] Add event and occlusion queries.
- [x] Send presentation through `pfnPresentCb`.
- [ ] Test every advertised primitive type and stream pattern.
- [ ] Test state-cache keys and object lifetime.
- [ ] Test clear, copy, lock, rename, query, completion, and present order.

## Shader bridge

- [x] Vendor Microsoft's ShaderConverter under its MIT notice.
- [x] Remove the D3D12 Translation Layer backend dependency.
- [x] Build the converter statically into `neptune_d3d9.dll`.
- [x] Convert D3D9 VS and PS 1.x through 3.0 token streams.
- [x] Wrap converted tokens with the shared DXBC builder.
- [x] Use one normalized DXBC signature description.
- [x] Derive input signatures from declarations and FVFs.
- [x] Build matching D3D11 input layouts.
- [x] Use `ConvertTLShader` for `POSITIONT`.
- [x] Upload float, integer, and Boolean constants.
- [x] Preserve inline `DEF`, `DEFI`, and `DEFB` values.
- [x] Include declaration and linked-shader inputs in cache keys.
- [x] Bound and release shader variant caches.
- [ ] Execute valid SM1, SM2, and SM3 converter cases.
- [ ] Execute malformed-token and size-overflow cases.
- [ ] Verify signatures, relative addressing, and constant ranges.
- [ ] Verify `POSITIONT`, half-pixel, and premultiplied-alpha behavior.
- [ ] Execute converted shaders through the Neptune host proxy.
- [ ] Run local DWM fixtures without adding their blobs to source control.

## Fixed-function safety net

- [x] Do not advertise hardware transform and light.
- [x] Do not advertise pure-device support.
- [x] Add null-stage vertex passthrough for `POSITIONT`.
- [x] Add WVP transformation for XYZ positions.
- [x] Forward diffuse, specular, and texture coordinates 0 and 1.
- [x] Support two texture stages.
- [x] Support `DISABLE`, `SELECTARG`, and `MODULATE`.
- [x] Support `DIFFUSE`, `TEXTURE`, `CURRENT`, and `TFACTOR` arguments.
- [x] Support the guarded alpha-test subset.
- [x] Reject unsupported active stages and states with a diagnostic.
- [ ] Execute each supported fixed-function truth-table case.
- [ ] Prove zero unsupported fixed-function requests during DWM startup.

## Verification before Aero acceptance

Current checkpoint (2026-08-13): the Vista-era VirtualBox-style D3D9 caps
profile is advertised by `triton9FillCaps`. Vista accepts it as an LDDM
device and creates the accelerated desktop primary (resource 5), but the
primary readback remains near-black apart from the watermark. The remaining
failure is therefore in composition/render/present behavior, not the milcore
caps acceptance gate.

- [x] Compile x86 and x64 private-ABI probes.
- [x] Link and PE-audit both UMDs.
- [x] Link, sign, and PE-audit both KMDs.
- [x] Audit both INFs and package manifests.
- [ ] Run native shader and host-proxy tests.
- [x] Install and start the x64 package on the exact Vista disk.
- [x] Verify `StartDevice`, VidPn commit, and the active 800x600 mode.
- [ ] Open a D3D9Ex HAL device.
- [ ] Render and read back a known triangle.
- [ ] Verify ordered render-event completion.
- [ ] Verify present reaches KMD scanout.
- [ ] Run the cross-process sharing matrix.
- [ ] Start DWM and select an Aero theme.
- [ ] Verify `DwmIsCompositionEnabled` after reboot.
- [ ] Test thumbnails, transparency, blur, Alt-Tab, and Flip 3D.
- [ ] Run ten cold boots for each architecture.
- [ ] Run twenty mode and theme transitions.
- [ ] Run a 30-minute present and resize stress test under Driver Verifier.

Use `/Users/jumar/winvistachecked/winvista-3.shrunk.qcow2` directly. Do not clone it. Use the Triton QEMU command line and the HTTP deployment path.
