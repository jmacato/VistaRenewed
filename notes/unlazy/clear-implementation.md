# Clear and readback implementation

Date: 2026-08-26

## Result

Leaf 1.2.2.1 is complete for local source, build, and native execution evidence. The implementation does not bypass an HRESULT.

The D3D9 clear callback now implements all four documented rectangle shapes. It records raw entry data before it validates the call.

Full clears use the existing whole-view calls. Partial D16 clears use standard `ID3D11DeviceContext1::ClearView` behavior.

Partial D24S8 clears use a new private synchronized Neptune command. DXMT preserves each unselected depth or stencil plane.

The native regression reads an RGBA8 result after real depth and stencil tests. It does not read a fabricated D24S8 value.

No Vista guest run occurred in this leaf. This report makes no public guest, PresentEx, or Aero claim.

## D3D9 clear contract

`triton9_output.c:443-472` writes the raw flags, rectangle count, and pointer values before the first validation return. Each later return records a stage and HRESULT.

`triton9_output.c:73-122` supplies one rectangle normalizer for the target and depth resources. It implements these rules:

- Zero rectangles without `D3DCLEAR_COMPUTERECTS` produce a no-op.
- Zero rectangles with `D3DCLEAR_COMPUTERECTS` use the viewport.
- Positive rectangles with `D3DCLEAR_COMPUTERECTS` clip to the viewport, enabled scissor, and resource bounds.
- Positive rectangles without `D3DCLEAR_COMPUTERECTS` must already fit the resource bounds.

The no-op returns before host-device creation. Invalid flags, depth values, rectangles, missing views, and formats return specific failure codes.

The target and depth paths normalize against their own resource dimensions. ARGB color conversion uses all four packed byte channels.

One full rectangle uses `ClearRenderTargetView` or `ClearDepthStencilView`. A partial D16 rectangle uses standards-compliant `ClearView`.

Partial D24S8 calls `npt_dispatch_clear_depth_stencil_rects`. The dispatcher splits very large lists into synchronized batches of 4,096 rectangles.

## Private synchronized command

Guest and host use transport subgroup 7 and method 0. The fixed body contains these fields:

- Command header
- Context object identity
- DSV object identity
- Depth and stencil selection flags
- Depth value
- Rectangle count
- Stencil value
- Normalized signed rectangles after the fixed body

Both transport headers assert a 16-byte rectangle and a 56-byte fixed command. Both generated protocol headers use wire version 2.

The guest waits for a reply after each batch. It returns the host HRESULT and stops after the first failed batch.

The renderer accepts this command only from a ring dispatch. It checks the size, flags, value range, rectangles, and object types.

Invalid object identities return `E_INVALIDARG`. A non-DXMT backend or missing private export returns `E_NOTIMPL`.

The renderer loads `dxmt_d3d11_clear_depth_stencil_rects` only for DXMT. D3DMetal keeps the standard interface and does not receive this convention.

## DXMT behavior

DXMT exports `dxmt_d3d11_clear_depth_stencil_rects` from `libdxmt-native.dylib`. The export queries DXMT's private immediate-context interface.

The private method checks the view, flags, depth, format planes, and every rectangle. It records a rectangle render pass and waits for GPU completion.

The render pass loads and stores both D24S8 planes. Its depth-stencil state writes only the selected plane.

A depth-only clear leaves stencil loaded and stored. A stencil-only clear leaves depth loaded and stored.

Standard `ID3D11DeviceContext1::ClearView` rejects a DSV with a stencil plane. It remains valid for depth-only resources.

The native D16 regression proves that a partial standard `ClearView` changes only the requested rectangle.

## Capability dependency

The implemented symbol is:

```c
BOOL triton9HasCompleteD24S8ClearContract(void);
```

`triton9_output.c:146` defines the symbol only with the complete clear implementation. The caps owner declared and calls the external symbol.

Both Vista DLLs contain the symbol. The successful links prove a real source dependency instead of a cosmetic macro.

## Readback evidence

The native D24S8 test cannot map D24S8 as ordinary CPU data. It therefore converts comparisons into an ordinary RGBA8 target.

White pixels passed the requested depth or stencil test. Black pixels failed it. The test copies RGBA8 into staging memory and maps it.

The test first clears D24S8 to depth `0.25` and stencil `0x55`. It then clears depth to `0.75` inside `[2,2,6,6)`.

Pixels prove the new depth inside and the old depth outside. A stencil comparison proves `0x55` remains inside and outside.

The test then clears stencil to `0xaa` inside `[3,3,5,5)`. Depth comparisons prove that both depth regions remain unchanged.

Stencil comparisons prove `0xaa` inside and `0x55` outside. A standard D24S8 `ClearView` call leaves the result unchanged.

Existing D3D9 readback code copies the host resource into staging memory before it maps. It then copies rows with the runtime-owned pitch.

The two padded-pitch CPU tests also pass. They cover full and partial row copies with nonzero padding.

## Exact source ownership

`PLAN.md:24-34` assigns these source files only to leaf 1.2.2.1:

- `triton-umd/src/virtio/neptune/vista-d3d9/triton9_output.c`
- `triton-umd/src/virtio/neptune/vista-d3d9/meson.build`
- `triton-umd/src/virtio/neptune/vista-d3d9/tests/triton9_clear_contract_test.c`
- `triton-umd/src/virtio/neptune/npt_transport_defs.h`
- `triton-umd/src/virtio/neptune/npt_dispatch.c`
- `triton-umd/src/virtio/neptune/npt_dispatch.h`
- `triton-umd/src/virtio/neptune/neptune-protocol/npt_protocol_defs.h`
- `triton-virglrenderer/src/neptune/npt_transport_defs.h`
- `triton-virglrenderer/src/neptune/npt_dispatch.c`
- `triton-virglrenderer/src/neptune/npt_context.c`
- `triton-virglrenderer/src/neptune/npt_library.c`
- `triton-virglrenderer/src/neptune/npt_library.h`
- `triton-virglrenderer/src/neptune/neptune-protocol/npt_protocol_defs.h`
- `triton-dxmt/src/d3d11/d3d11_context_impl.cpp`
- `triton-dxmt/src/d3d11/d3d11_context_imm.cpp`
- `triton-dxmt/src/d3d11/d3d11_device.hpp`
- `triton-dxmt/src/dxmt/dxmt_command.cpp`
- `triton-dxmt/src/dxmt/dxmt_command.hpp`
- `triton-dxmt/src/dxmt-native/dxmt_native.h`
- `triton-dxmt/src/dxmt-native/dxmt_native.exports`
- `triton-dxmt/tests/native/d3d11_test.cpp`

`npt_context.c` required no change because its typed object lookup already supplied the needed interface. No sibling-owned source was edited.

The public D3D9 acceptance probe was not changed. No package, service, VM, disk, or framebuffer state was changed.

## Verification

Both portable clear tests passed through the Meson targets:

```text
meson test -C triton-umd/build-vista-x64-unified triton9-clear-contract --print-errorlogs
1/1 mesa:triton9-clear-contract OK
Ok: 1  Fail: 0

meson test -C triton-umd/build-vista-x86-unified triton9-clear-contract --print-errorlogs
1/1 mesa:triton9-clear-contract OK
Ok: 1  Fail: 0
```

The direct portable run reported its exact case count:

```text
cc -std=c11 -Wall -Wextra -Werror triton-umd/src/virtio/neptune/vista-d3d9/tests/triton9_clear_contract_test.c -o /tmp/triton9_clear_contract_test
/tmp/triton9_clear_contract_test
TRITON9_CLEAR_CONTRACT_TEST PASS cases=20
```

Both Vista UMD targets built and linked:

```text
PYTHONPATH=/opt/homebrew/lib/python3.13/site-packages ninja -C triton-umd/build-vista-x64-unified -j1 src/virtio/neptune/vista-d3d9/neptune_d3d9.dll
exit 0; [127/127] Linking target src/virtio/neptune/vista-d3d9/neptune_d3d9.dll

PYTHONPATH=/opt/homebrew/lib/python3.13/site-packages ninja -C triton-umd/build-vista-x86-unified -j1 src/virtio/neptune/vista-d3d9/neptune_d3d9.dll
exit 0; [127/127] Linking target src/virtio/neptune/vista-d3d9/neptune_d3d9.dll
```

Both padded-pitch test targets passed:

```text
meson test -C triton-umd/build-vista-x64-unified triton9-cpu-layout --print-errorlogs
1/1 mesa:triton9-cpu-layout OK

meson test -C triton-umd/build-vista-x86-unified triton9-cpu-layout --print-errorlogs
1/1 mesa:triton9-cpu-layout OK
```

The renderer built and linked:

```text
ninja -C triton-virglrenderer/build-arm64 -j1 src/libvirglrenderer.1.dylib
exit 0; [8/8] Linking target src/libvirglrenderer.1.dylib
```

DXMT built, exported the private symbol, and passed its native suite:

```text
ninja -C triton-dxmt/build-native -j1 src/dxmt-native/libdxmt-native.dylib
exit 0

nm -gU triton-dxmt/build-native/src/dxmt-native/libdxmt-native.dylib
000000000005d0e4 T _dxmt_d3d11_clear_depth_stencil_rects

/tmp/triton_d3d11_test /tmp/triton_d3d11_results.txt
[SUMMARY] passed=174 failed=0
```

The D16 section contributed six passes. The D24S8 section contributed 20 passes for plane preservation and standard `ClearView` behavior.

## Adversarial review

The review checked zero rectangles, null arrays, inverted rectangles, clipped-away rectangles, disabled scissors, and out-of-bounds runtime rectangles. All expected results passed.

The review checked a viewport-only clear and positive computed rectangles. It also checked full-resource detection and all four packed color channels.

Large D24S8 lists cannot exceed one command limit. The guest splits them into ordered synchronous batches.

Malformed wire sizes, invalid flags, invalid depth, empty lists, invalid rectangles, and stale object identities fail closed on the host.

Unsupported depth formats return `D3DDDIERR_NOTAVAILABLE`. Stencil on D16 returns `D3DDDIERR_INVALIDCALL`.

The DXMT test destroys its COM resources after the checks. Typed host lookup prevents a stale or wrong object type from reaching DXMT.

No path changes a clear failure into success. The D3D9 callback maps a real transport or device failure back to its caller.

## Remaining integration check

The next guest-owned package must contain these new UMD, renderer, and DXMT components. Wire version 2 requires matched guest and host components.

The guest-owned public D3D9 probe must still prove clear/readback. That later result controls the root clear gate.

This change does not prove triangle/readback, PresentEx, desktop composition, or Aero glass.
