# Vista D3D9 public-contract implementation

## Scope and source identity

This leaf changes only the four files assigned to owner 1.2.1.1. The ownership map is in `PLAN.md:20-30`. The exact assignment is at `PLAN.md:26`.

The four source files were untracked in the nested `triton-umd` worktree before this leaf. I preserved their existing content and the concurrent sibling changes. The following SHA-1 values identify the reviewed start and end states.

| File | Start SHA-1 | Final SHA-1 |
| --- | --- | --- |
| `triton9_ddi.c` | `5fd50af823010ac1c71eaaa8bddefbf5bf1a040d` | `50e9773359dd87f43b07b6f0859b45cca0a901de` |
| `triton9_format.c` | `9d2c0356d92a13b95711c44d0d71d748d7fc9569` | `de92282c17e14f1168990ed8d4db51bd6724111a` |
| `triton9_unsupported.c` | `af793a2b3a03c97d9e7fd4210b930d7a2f07c47f` | `20701efd85b01cc4e7f7d3b9e326fd551d9cbef0` |
| `tests/triton9_abi_test.c` | `90ef91ca1c64aee7cfe09988ca866095679f1940` | `65d0d088d4007e2ff41565cf9aa510cee8104939` |

This result applies to the current source tree. It does not claim that an installed guest DLL has these bytes. This leaf did not build a package or operate a VM.

## Adapter and device contracts

The public D3D9 interface value is `9`. The Vista UMD `DriverVersion` is `0x000c`. The runtime `Version` field is an opaque build identifier. The code accepts each runtime build value. See `triton9_ddi.c:145-148`, `triton9_ddi.c:1053-1068`, and `triton9_ddi.c:1081-1087`.

This distinction follows the `D3DDDIARG_OPENADAPTER` field contract. The local WDK names `Version` as runtime input and `DriverVersion` as UMD output. See `driver/sdk/microsoft.windows.wdk.x64/c/Include/10.0.28000.0/um/d3dumddi.h:4694-4707`.

`OpenAdapter` rejects a null argument, runtime handle, table, or required adapter callback. It rejects a non-D3D9 interface. Allocation failure returns `E_OUTOFMEMORY`. See `triton9_ddi.c:1054-1073`.

`OpenAdapter` writes the function table and driver handle only after all failure points. The success path publishes three Vista functions and the heap-owned adapter handle. See `triton9_ddi.c:1081-1088`.

`CreateDevice` validates the runtime handle, callback table, output table, and D3D9 interface. It also validates each callback that the Triton path consumes. See `triton9_ddi.c:162-190` and `triton9_ddi.c:927-950`.

Only `AllowMultithreading` and `AllowFlipBatching` can be set in Vista. The other 30 bits are reserved. See `d3dumddi.h:4619-4631`. The implementation rejects reserved bits at `triton9_ddi.c:939-945`.

The two allowed bits grant permission. They do not require Triton to start a worker thread. The implementation records the runtime identifiers and uses its existing serialized proxy path. See `triton9_ddi.c:939-962` and `triton9_ddi.c:665-733`.

`CreateDevice` clears and fills the Vista table before it publishes the driver handle. The real clear, draw, render-target, depth, present, resource, state, shader, and query functions remain installed. See `triton9_ddi.c:971-1033`.

The private KMD compatibility query remains deferred. Loader-time use can deadlock the runtime. The first host operation runs the query and returns its failure without a fallback. See `triton9_ddi.c:698-706` and symbol `triton9EnsureHostDevice`.

The deferred query is not a silent software path. It requires the Neptune capset, shared memory, private ABI V2, and render-event support. See `triton9_ddi.c:243-264` and symbol `triton9QueryPrivateAdapterInfo`.

## Capability contract

`triton9FillCaps` is the only `D3DCAPS9` writer. See `triton9_ddi.c:301-494`. The audit compared every positive group with the resource, state, shader, output, and query paths.

The implementation removed claims for a scanline counter, gamma ramps, a hardware cursor, and managed-resource support. It also removed the synthetic interval-one claim. The remaining timing claim is immediate presentation. See `triton9_ddi.c:337-349`.

The implementation keeps the hardware transform, raster, and pure-device bits. The public strict profile needs these bits. Triton uses the same real draw path for pure and non-pure devices. See `triton9_ddi.c:350-376`.

The shader path translates D3D9 vertex and pixel shaders. It also builds the supported fixed vertex and pixel programs. See symbols `triton9ConvertShader`, `triton9BuildFixedVertexTokens`, and `triton9BuildFixedPixelTokens` in `triton9_shader.cpp:662-744`, `triton9_shader.cpp:1475`, and `triton9_shader.cpp:1700`.

The primitive and raster masks now omit fog, dither, stipple, depth-bias, and multiple-render-target claims. The output count is one. See `triton9_ddi.c:377-403` and `triton9_ddi.c:473`.

The positive comparison, blend, stencil, address, and point-or-linear filter masks map to host states. See symbols `triton9MapBlend`, `triton9MapBlendOp`, `triton9MapComparison`, `triton9MapStencilOperation`, `triton9MapAddress`, and `triton9MapFilter` in `triton9_state.cpp:43-220`.

The texture contract excludes cube, volume, anisotropic, stretch-filter, vertex-texture, and explicit mip-filter claims. The public maximum anisotropy is one. See `triton9_ddi.c:400-427`.

The fixed-function contract has two texture stages and two simultaneous textures. It has no active lights, clip planes, or vertex blending. See `triton9_ddi.c:433-448`.

The shader contract now advertises Shader Model 2.0. The two Shader Model 3.0 slot fields are zero. See `triton9_ddi.c:453-493`.

The declaration-type mask matches the explicit D3D9-to-DXGI mappings. See `triton9_ddi.c:468-472` and `triton9_shader.cpp:286-325`.

Stencil operations are conditional on the D24S8 format contract. D16 alone does not cause stencil claims. See `triton9_ddi.c:415-419`.

Multisampling has zero quality levels because resource creation rejects it. Video, decode, and extension counts are zero. Gamma and content-protection outputs are also zero. See `triton9_ddi.c:574-617` and `triton9_resource.c:1079-1092`.

Event and occlusion are the only query types. Their real create, issue, and result paths are in `triton9_query.cpp:63-231`. The count and data are at `triton9_ddi.c:586-600`.

All fixed-size `GetCaps` requests now require the Vista ABI size. A wrong size returns `E_INVALIDARG`. An unknown caps type returns `D3DDDIERR_NOTAVAILABLE`. See `triton9_ddi.c:511-621`.

## FORMATOP contract

The base table has four formats. A8R8G8B8 and X8R8G8B8 are 32-bit color formats. A8 is texture-only. D16 is depth-stencil-only. See `triton9_format.c:13-37`.

The color entries no longer claim volume, cube, conversion, sRGB, vertex-texture, or mip-map operations. The depth entries no longer claim texture sampling. See `triton9_format.c:13-22` and `triton9_format.c:33-42`.

X8R8G8B8 is the only display format. A8R8G8B8 keeps its alpha-compatible render-target operation. Static assertions protect both rules. See `triton9_format.c:17-31`.

The 32-bit color formats keep `FORMATOP_OFFSCREENPLAIN`. The resource sibling gives ordinary B8G8R8A8 and B8G8R8X8 textures an RTV binding. See `triton9_resource.c:220-233`.

The existing same-size blit and color-fill paths then implement that claim. See `triton9_output.c:628-758` and symbol `triton9ColorFill` at `triton9_output.c:1014-1055`.

D24S8 is an all-or-nothing entry. `triton9FormatCount` excludes it when the complete clear contract returns false. Both format lookup functions use that count. See `triton9_format.c:38-84`.

The dependency is a required link symbol, not a comment or probe exception. The declaration is at `triton9.h:267-270`. The implementation is at `triton9_output.c:145-151`.

The UMD dispatches a partial D24S8 clear and checks host completion at `triton9_output.c:600-624`. The native test proves both selected-plane writes and unselected-plane preservation at `triton-dxmt/tests/native/d3d11_test.cpp:813-892`.

The sibling native run reported `passed=174 failed=0`. See `notes/unlazy/clear-implementation.md:184-197`. The x86 and x64 DLLs both contain `triton9HasCompleteD24S8ClearContract`.

## Callback contract

The Vista table starts as zero. `triton9InstallUnsupportedDeviceFuncs` installs exact-signature functions for regular-table operations that are present but unsupported. Real functions then replace supported slots. See `triton9_ddi.c:971-1030` and `triton9_unsupported.c:169-194`.

The common unsupported result is exact and fail-closed. Bad inputs return `E_INVALIDARG`. A lost device returns `D3DDDIERR_DEVICEREMOVED`. A valid unsupported call returns `D3DDDIERR_NOTAVAILABLE`. See `triton9_unsupported.c:12-29`.

State sets now use this failure path. Triton cannot return a valid state-set handle, so the old success no-op was incorrect. See `triton9_unsupported.c:55-58`.

Patch draws, volume blits, palette calls, lights, convolution, compose-rects, depth-fill, and capture remain error callbacks. None returns success. See `triton9_unsupported.c:47-58`, `triton9_unsupported.c:74-78`, `triton9_unsupported.c:98-104`, and `triton9_unsupported.c:158-167`.

Four successful state or bookkeeping callbacks are implemented contracts, not unsupported stubs. `UpdateWInfo` validates required publication bookkeeping. `SetPriority` accepts a valid VidMm hint. `SetMaterial` stores the value. `SetClipPlane` accepts only inert initialization while public clip-plane capacity is zero. See `triton9_unsupported.c:31-46`, `triton9_unsupported.c:60-72`, `triton9_unsupported.c:79-97`, and `triton9_unsupported.c:105-120`.

`GetInfo` supports only the exact vertex-cache query and size. Other identifiers return not available. See `triton9_unsupported.c:122-157`.

Optional video, decode, extension, and overlay slots stay null. Their public counts and caps are zero. The zeroed table and installed range are at `triton9_ddi.c:971-1030`.

## ABI protection

The ABI compile test pins Vista UMD version `0x000c`, the D3D9 caps size, FORMATOP size, and caps query values. See `tests/triton9_abi_test.c:14-33`.

The test pins the three-slot adapter table and the 99-slot device table. It includes clear, draw, state-set, output, capture, async-lock, and rename offsets. See `tests/triton9_abi_test.c:34-115`.

The test pins the 22-slot runtime callback table and each callback that Triton requires. See `tests/triton9_abi_test.c:116-151`.

The test pins x86 and x64 argument sizes and offsets. It covers open-adapter, create-device, flags, caps, clear, and present. See `tests/triton9_abi_test.c:152-190`.

Typed assignments verify the stdcall signatures for the real public paths. They also verify the D24S8 dependency signature. See `tests/triton9_abi_test.c:227-280`.

## Verification

Both full UMD targets linked after the final owned-source changes.

```text
ninja -C triton-umd/build-vista-x86-unified src/virtio/neptune/vista-d3d9/neptune_d3d9.dll
exit 0: Linking target src/virtio/neptune/vista-d3d9/neptune_d3d9.dll

ninja -C triton-umd/build-vista-x64-unified src/virtio/neptune/vista-d3d9/neptune_d3d9.dll
exit 0: Linking target src/virtio/neptune/vista-d3d9/neptune_d3d9.dll
```

Both ABI compile targets rebuilt after the final offset assertions.

```text
ninja -C triton-umd/build-vista-x86-unified src/virtio/neptune/vista-d3d9/libtriton9_abi_compile.a
exit 0: [1/2] Compiling triton9_abi_test.c.obj; [2/2] Linking libtriton9_abi_compile.a

ninja -C triton-umd/build-vista-x64-unified src/virtio/neptune/vista-d3d9/libtriton9_abi_compile.a
exit 0: [1/2] Compiling triton9_abi_test.c.obj; [2/2] Linking libtriton9_abi_compile.a
```

The focused contract tests passed in both builds.

```text
meson test -C triton-umd/build-vista-x86-unified --no-rebuild --print-errorlogs \
  triton9-cpu-layout triton9-clear-contract triton9-draw-contract \
  triton9-shader-token-contract
exit 0: Ok: 4; Fail: 0

meson test -C triton-umd/build-vista-x64-unified --no-rebuild --print-errorlogs \
  triton9-cpu-layout triton9-clear-contract triton9-draw-contract \
  triton9-shader-token-contract
exit 0: Ok: 4; Fail: 0
```

An initial no-rebuild run found that the new sibling clear target was not built. That was a missing prerequisite, not a failed test. The final runs above used built targets.

Both PE audits passed.

```text
audit-vista-d3d9-pe.sh x86 .../neptune_d3d9.dll
exit 0: Vista D3D9 PE audit passed: x86

audit-vista-d3d9-pe.sh x64 .../neptune_d3d9.dll
exit 0: Vista D3D9 PE audit passed: x64
```

Both linked DLLs expose `triton9FormatCount`, `triton9FormatLookup`, and `triton9HasCompleteD24S8ClearContract`. Cross-target `nm` checks returned exit zero.

## Four review passes

**Implementation audit.** I read every owned function and compared each positive claim with its consumer. I removed positive claims that had no complete path.

**Vista D3D9 expert reread.** I checked interface, runtime version, driver version, flags, sizes, table slots, handle publication, and callback lifetime. The ABI assertions encode the results.

**Return-path and contradiction hunt.** I searched every successful callback in `triton9_unsupported.c`. Each success now has state, output, or required bookkeeping semantics. Every true stub fails.

I also mapped each remaining positive cap to state, shader, resource, output, or query code. No positive cap in the owned files routes directly to an error stub.

**Free polish.** I clarified the runtime-version and flag rules. I also changed the size diagnostic to `TRITON9-CAPS-SIZE-REJECT`. Both targets were rebuilt after this pass.

No remaining contract mismatch was found in the four owned files. This statement does not cover sibling source or guest execution.

## Limits and sibling dependencies

D24S8 advertisement depends on `triton9HasCompleteD24S8ClearContract`. Removing the sibling implementation causes a link failure. Returning false removes D24S8 from count, lookup, creation, and stencil caps.

The retained 32-bit `OFFSCREENPLAIN` claim depends on the sibling RTV binding at `triton9_resource.c:224-231`. A sibling regression there must fail the combined contract gate.

This leaf did not run the Vista public probe. Later leaves must prove clear/readback, triangle/readback, and PresentEx in the guest. No VM, Windows component, package, service, or INF changed here.

VirtualBox supplied only two Vista-era behavioral checks in existing comments. Triton's callback, proxy, and renderer architecture did not change. See `triton9_ddi.c:531-535` and `triton9_unsupported.c:122-131`.
