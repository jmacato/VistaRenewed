# Ranked diagnosis and implementation ownership

## Result

The first proved failure is the public D24S8 depth-clear call. It returned `0x80004005` after D24S8 creation, binding, and target-only clear succeeded. The evidence does not prove a raw DDI `NumRect` value. It also does not prove that Vista entered the deployed `triton9Clear` function.

Rank 1 is a positive DDI rectangle count that reaches the deployed D24S8 rectangle rejection. This rank is a hypothesis, not a fact.

The decisive next action is an unchanged public-probe run with entry-first UMD records. The records must include raw arguments and every early return. This report makes no DWM, Aero, rendering, or glass claim.

## Proven public boundaries

### Clear/readback boundary

The public probe reached `IDirect3DDevice9Ex::Clear` for D24S8 depth. That call returned `0x80004005` and stopped the probe. The target-only clear immediately before it returned `S_OK`. D24S8 creation and `SetDepthStencilSurface` also returned `S_OK`.

The probe did not enter `Clear readback`. Therefore, no clear pixel exists, and no readback failure is proved.

The earliest proved internal range is narrower than the complete Triton path. The failure is one of these two cases:

1. Vista returned before the deployed UMD clear entry.
2. The deployed UMD returned before its first depth proof record.

The persisted files do not select one case. They do not prove protocol encode, host dispatch, the host proxy, or DXMT depth-clear entry.

### Triangle/readback boundary

The triangle stage did not start. The prior public D24S8 depth-clear failure blocked it at `triton9_runtime_probe.c:2028-2033`. No triangle-specific failure is proved. No draw call, triangle-center pixel, or outside pixel exists for the scoped process.

### PresentEx boundary

The `PresentEx` stage did not start. The prior public D24S8 depth-clear failure blocked it before `triton9_runtime_probe.c:2474-2478`. No PresentEx-specific failure is proved. No device-present result or additional-swap-chain result exists for the scoped process.

## Cross-check of prerequisite reports

All five prerequisite gate files report `ALL MET`. The current status command reported `6`, `6`, `6`, `6`, and `7` met gates.

### `architecture-audit.md`

The report defines the required probe order at `notes/unlazy/architecture-audit.md:30-47`. Direct source agrees at `triton-umd/src/virtio/neptune/vista-d3d9/tests/triton9_runtime_probe.c:2022-2127,2462-2515,2651-2697`. The direct source calls `GetRenderTargetData` and `LockRect` at lines `930-954` of that file. Thus, a returned pixel requires real readback.

The DDI table binds `pfnClear` at `triton-umd/src/virtio/neptune/vista-d3d9/triton9_ddi.c:1023-1026`. The clear code is at `triton-umd/src/virtio/neptune/vista-d3d9/triton9_output.c:263-376`. The direct architecture remains Triton. It uses the Vista D3D9 UMD, WDDM miniport, Neptune protocol, host proxy, and DXMT backend.

### `build-baseline.md`

The current renderer failure appears at `notes/unlazy/build-baseline.md:95-130`. Direct source defines `HANDLE` as `uint64_t` and assigns `NULL` to it. The exact files are `triton-virglrenderer/src/neptune/neptune-protocol/npt_protocol_directx_types.h:87` and `triton-virglrenderer/src/neptune/neptune-protocol/npt_protocol_common_types.h:13631`.

Both generated common-type files have SHA-256 `fa12bf7f8dac7d78c85f58c1992d4e2b7823d5e4ab07a2be67381e4f6b85258f`. `otool -L` shows that QEMU uses `host-triton/lib/libvirglrenderer.1.dylib`. It does not use the current renderer build-tree output.

The active x64 diagnostic INF passed its audit. The seven reported x64 INF failures apply only to the inactive standard template. The direct commands returned `ACTIVE_INF_EXIT=0` and `INACTIVE_INF_EXIT=1`.

See `notes/unlazy/build-baseline.md:178-218` for the same result. These build facts are separate from the scoped runtime cause. The older linked renderer ran during the guest result, but current source cannot reproduce it yet.

### `runtime-evidence.md`

The runtime inventory found no host public-probe result. It selected the newer inactive qcow2 for later read-only inspection. See `notes/unlazy/runtime-evidence.md:9-30,324-371`. The later offline report performed that selected inspection and found the decisive guest transcript.

The launcher defines read-only media at `scripts/run_vista_guest_deploy.sh:44-66`. It states that it sends no monitor input at lines `4-6`. The current probe source writes the D3D9 gate before the Aero path. It skips the Aero child after a D3D9 failure.

### `offline-evidence.md`

The latest deployment ID and complete probe transcript are at `notes/unlazy/offline-evidence.md:12-18,220-255`. The transcript size was `4,158` bytes.

The transcript ends with these results:

```text
Clear offscreen target         hr=0x00000000 PASS
Clear D24S8 depth              hr=0x80004005 FAIL
TRITON9-PROBE FAIL
```

The report proves absent clear pixels, triangle pixels, and PresentEx results. It also excludes DWM evidence because the public D3D9 gate failed. The parent gate records a direct read-only spot-check of the same call order. See `PLAN.md:67` and `gates/leaf-1.1.2.2.md`.

### `depth-clear-trace.md`

The report maps the exact deployed return range at `notes/unlazy/depth-clear-trace.md:190-258`. Direct disassembly reproduced that map. The deployed native artifact has `triton9Clear` at `0x277fa7520`. Its first depth proof block starts at `0x277fa76ea`.

Positive rectangle handling occurs before that block. A non-D16 depth format returns `0x8876086a` at `0x277fa79b5`. The deployed artifact has the depth proof strings but lacks `CLEAR-ENTER` and `BADFLAGS`. Five other proof records worked for the scoped PID.

The host artifacts that match the installed guest UMDs have these SHA-256 values:

- Native: `2dbb366ca08f6605cf4cf4d279463930be6ca98c2d789fc82e55918b4d9a35f7`.
- WoW64: `589337d20788960810ab3a987b99e90efcf16e2b412880fc399d7f5d80df9d36`.

The current outputs have different values. They are `2047dba5...dc81` and `1f83e22e...2d2`.

The Microsoft DDI contract defines four rectangle shapes. `NumRect==0` with `D3DCLEAR_COMPUTERECTS` means a viewport clear. The public count of zero does not prove the raw DDI count. The official callback contract is also recorded in `notes/unlazy/depth-clear-trace.md:118-148`.

## Ranked root-cause hypotheses

Ranks state explanatory fit. No rank becomes a fact without its named result.

### Rank 1: a positive DDI rectangle count reached the deployed D24S8 rejection

Supporting evidence:

- This is the only depth-format-specific return before the first deployed depth proof block.
- It separates the successful target clear from the failed D24S8 clear.
- It also explains the absence of every deployed depth proof record.
- The pure-device path can add `D3DCLEAR_COMPUTERECTS` to the DDI flags.

Contradicting evidence:

- The public call supplied count `0` and pointer `NULL`.
- The DDI contract defines `NumRect==0` with COMPUTERECTS as a viewport clear.
- No persisted record proves a positive DDI count.
- No persisted record proves UMD entry.

Falsification check:

Record raw `Flags`, `NumRect`, and the rectangle pointer at the first UMD instruction. `NumRect==0` falsifies Rank 1 for that scoped call.

### Rank 2: Vista returned before UMD entry

Supporting evidence:

- The deployed UMD has no entry-first marker.
- No depth marker exists in either scoped proof file.
- A runtime return and a pre-marker UMD return are observationally equal in the persisted files.

Contradicting evidence:

- The same device completed D24S8 creation, binding, viewport setup, and target clear.
- The device table installs the real `triton9Clear` callback.
- No raw runtime trace records an internal Vista rejection.

Falsification check:

Record one marker before pointer validation in `triton9Clear`. A matching PID marker falsifies pure pre-entry failure.

### Rank 3: another deployed UMD early return produced the failure

Supporting evidence:

- Pointer validation, device loss, host setup, and flag validation all precede the first depth marker.
- `triton9EnsureHostDevice` can preserve an `E_FAIL` result before that marker.
- The persisted public result is `E_FAIL`, while the raw DDI result is absent.

Contradicting evidence:

- Vista owns the callback pointers and supplied valid adjacent calls.
- The target clear passed immediately before the depth clear.
- Host-proxy setup and DSV binding already succeeded for the same device.
- Expected raw flags `0x2` or `0xA` pass the deployed range test.

Falsification check:

Record a unique raw HRESULT before each early return. One tag identifies this rank, and no tag excludes it.

### Rank 4: a deferred transport, host, or backend failure surfaced at the depth call

Supporting evidence:

- Both D3D11 clear methods are asynchronous and return `void`.
- The target public pass does not prove host or backend completion.
- `npt_ring_notify` discards the renderer submit result at `npt_ring.c:414-426`.
- The deployed device-health proxy returns success without a host reply.

Contradicting evidence:

- A zero-rectangle depth command is downstream from the missing depth marker.
- A downstream void method cannot directly return the same-call public `E_FAIL`.
- This rank needs a prior deferred failure or an unproved proof-sink fault.

Falsification check:

Count guest method `46`, host method `46`, DSV lookup, DXMT entry, and ordered completion. The first missing counter selects the failed boundary.

### Rank 5: artifact identity or the proof sink hid a reached depth block

Supporting evidence:

- A sink can theoretically lose one tag while other records survive.
- Later root logs contain unrelated PIDs, so incorrect PID selection can mislead an audit.

Contradicting evidence:

- Installed hashes match the disassembled `test-artifacts/vista-unified-umd` pair.
- Five proof records succeeded for build `20260816` and PID `000008a4`.
- The service captured a complete public transcript for the same child.

Falsification check:

Make the guest service compare every installed payload hash. Then emit adjacent entry and depth records through the same sink.

## Decisive check

Use a guest-owned signed package with one observation-only UMD change. Keep the public probe binary and its call order unchanged.

The first instruction of `triton9Clear` must record these values:

- deployment ID, UMD hash, build marker, and PID
- raw `Flags`, raw `NumRect`, and raw rectangle pointer
- device-lost state and cached-host state
- one unique raw HRESULT before every early return
- one record before protocol encode

Use matching counters for guest method `46`, host method `46`, DSV lookup, DXMT entry, and ordered completion.

Interpret one scoped result as follows:

1. No UMD entry record selects Rank 2.
2. Entry plus positive `NumRect` and the D24S8 reject selects Rank 1.
3. Entry plus `NumRect==0` falsifies Rank 1.
4. An early-return tag selects Rank 3.
5. Successful early records plus a missing downstream counter selects Rank 4.
6. Missing adjacent sink records with matching hashes selects Rank 5.

This is one decision tree, not five probe variants. It prevents a probe-specific driver bypass.

## File ownership for implementation

The following assignment is exact and disjoint. No path appears under two owners.

### Owner `1.2.1.1`: public D3D9 contracts

Files:

- `triton-umd/src/virtio/neptune/vista-d3d9/triton9_ddi.c`
- `triton-umd/src/virtio/neptune/vista-d3d9/triton9_format.c`
- `triton-umd/src/virtio/neptune/vista-d3d9/triton9_unsupported.c`
- `triton-umd/src/virtio/neptune/vista-d3d9/tests/triton9_abi_test.c`

Interface invariants:

- Caps and FORMATOP bits must describe implemented behavior.
- The callback table must keep the real clear, draw, and present functions.
- Unsupported functions must return an exact error and must not report success.
- D24S8 support must remain advertised only with complete creation, clear, and readback behavior.

Expected gate: `gates/leaf-1.2.1.1.md` G2-G5, then root `GATES.md` G7.

### Owner `1.2.1.2`: resource, state, transport status, and synchronization

Files:

- `triton-umd/src/virtio/neptune/vista-d3d9/triton9.h`
- `triton-umd/src/virtio/neptune/vista-d3d9/triton9_resource.c`
- `triton-umd/src/virtio/neptune/vista-d3d9/triton9_state.cpp`
- `triton-umd/src/virtio/neptune/vista-d3d9/triton9_query.cpp`
- `triton-umd/src/virtio/neptune/vista-d3d9/triton9_cpu_layout.c`
- `triton-umd/src/virtio/neptune/vista-d3d9/triton9_cpu_layout.h`
- `triton-umd/src/virtio/neptune/vista-d3d9/tests/triton9_cpu_layout_test.c`
- `triton-umd/src/virtio/neptune/npt_ring.c`
- `triton-umd/src/virtio/neptune/npt_ring.h`
- `triton-umd/src/virtio/neptune/npt_renderer_virtgpu_win32.c`
- `triton-umd/src/virtio/neptune/triton/tritonSharedBridge.c`
- `triton-umd/src/virtio/neptune/neptune-protocol/npt_protocol_common_types.h`
- `triton-virglrenderer/src/neptune/neptune-protocol/npt_protocol_common_types.h`

Interface invariants:

- The generated integer `HANDLE` decoder must write integer zero, not pointer `NULL`.
- Both generated common-type files must remain byte-identical.
- Every failed submit must become a stable transport or device failure.
- Readback must wait for ordered completion before CPU mapping.
- Pitch, region, shadow, and lifetime rules must return real rendered bytes.
- This owner also controls the UMD `triton9Present` function inside `triton9_resource.c`.

Expected gate: `gates/leaf-1.2.1.2.md` G2-G5. The build fix targets root G1 and G7.

The synchronization and readback changes also target root G2, G3, and G4.

### Owner `1.2.2.1`: clear semantics and D24S8 backend behavior

Files:

- `triton-umd/src/virtio/neptune/vista-d3d9/triton9_output.c`
- `triton-umd/src/virtio/neptune/vista-d3d9/meson.build`
- `triton-umd/src/virtio/neptune/vista-d3d9/tests/triton9_clear_contract_test.c` (new)
- `triton-umd/src/virtio/neptune/npt_transport_defs.h`
- `triton-umd/src/virtio/neptune/npt_dispatch.c`
- `triton-umd/src/virtio/neptune/neptune-protocol/npt_protocol_defs.h`
- `triton-virglrenderer/src/neptune/npt_transport_defs.h`
- `triton-virglrenderer/src/neptune/npt_dispatch.c`
- `triton-virglrenderer/src/neptune/npt_context.c`
- `triton-virglrenderer/src/neptune/neptune-protocol/npt_protocol_defs.h`
- `triton-dxmt/src/d3d11/d3d11_context_impl.cpp`
- `triton-dxmt/src/dxmt/dxmt_command.cpp`
- `triton-dxmt/src/dxmt/dxmt_command.hpp`

Interface invariants:

- Entry diagnostics must occur before every early return and must not change behavior.
- COMPUTERECTS must obey the viewport and scissor contract for all formats and flags.
- A zero count without COMPUTERECTS must remain a no-op.
- Full D24S8 clears must use the existing full-view path.
- Partial D24S8 depth clears must preserve stencil.
- Partial D24S8 stencil clears must preserve depth.
- A private rectangle command must carry DSV identity, flags, depth, stencil, rectangles, and a synchronized result.
- Both transport definitions and wire-version values must match exactly.
- Generated standard COM headers must not receive hand-written changes.

Expected gate: `gates/leaf-1.2.2.1.md` G2-G5, then root G2 and G7.

### Owner `1.2.2.2`: draw and shader translation

Files:

- `triton-umd/src/virtio/neptune/vista-d3d9/triton9_shader.cpp`
- `triton-umd/src/virtio/neptune/vista-d3d9/triton9_draw_contract.c`
- `triton-umd/src/virtio/neptune/vista-d3d9/triton9_draw_contract.h`
- `triton-umd/src/virtio/neptune/vista-d3d9/triton9_shader_token_contract.h`
- `triton-umd/src/virtio/neptune/vista-d3d9/tests/triton9_draw_contract_test.c`
- `triton-umd/src/virtio/neptune/vista-d3d9/tests/triton9_shader_token_contract_test.c`
- `triton-umd/src/virtio/neptune/vista-d3d9/tests/triton9_shaderconv_test.cpp`

Interface invariants:

- This owner consumes state from `triton9_state.cpp` without changing that file.
- Vertex data, transforms, constants, topology, viewport, and scissor must reach the selected target.
- Triangle readback must use the shared resource and synchronization path.
- No triangle-fan or fixed-function capability can remain advertised without implementation.

Expected gate: `gates/leaf-1.2.2.2.md` G2-G5, then root G3 and G7.

### Owner `1.2.2.3`: present, scanout, and present-fence behavior

Files:

- `triton-umd/src/virtio/neptune/npt_shared_texture.c`
- `triton-umd/src/virtio/neptune/npt_shared_texture.h`
- `triton-kmd/viogpu/viogpu3d/driver.cpp`
- `triton-kmd/viogpu/viogpu3d/driver.h`
- `triton-kmd/viogpu/viogpu3d/viogpu_device.cpp`
- `triton-kmd/viogpu/viogpu3d/viogpu_device.h`
- `triton-kmd/viogpu/viogpu3d/viogpu_adapter.cpp`
- `triton-kmd/viogpu/viogpu3d/viogpu_adapter.h`
- `triton-kmd/viogpu/viogpu3d/viogpu_command.cpp`
- `triton-kmd/viogpu/viogpu3d/viogpu_command.h`
- `triton-kmd/viogpu/viogpu3d/viogpu_allocation.cpp`
- `triton-kmd/viogpu/viogpu3d/viogpu_allocation.h`
- `triton-kmd/viogpu/viogpu3d/viogpu_vidpn.cpp`
- `triton-kmd/viogpu/viogpu3d/viogpu_vidpn.h`
- `triton-qemu/hw/display/virtio-gpu-virgl.c`
- `triton-qemu/hw/display/virtio-gpu.c`
- `triton-qemu/hw/display/virtio-gpu-gl.c`
- `triton-virglrenderer/src/neptune/npt_shared.c`
- `triton-virglrenderer/src/neptune/npt_shared.h`

Interface invariants:

- The rendered source and standard primary must remain distinct resources.
- Present flags and rectangles must accept the legal Vista forms or return exact errors.
- The present-fence token must pair with the correct call and thread.
- Host completion must occur before scanout consumes the source.
- Allocation references must survive submission, scanout, reset, and teardown.
- This owner uses `triton9Present` through the interface owned by `1.2.1.2`.

Expected gate: `gates/leaf-1.2.2.3.md` G2-G5, then root G4 and G7.

### Reserved deployment owners

Owner `1.3.1.1` controls package and read-only-media files. The active diagnostic INF is its only x64 INF authority unless root changes the route. The inactive `triton-kmd/viogpu/viogpu3d/viogpu3d_vista_x64.inx` must not block the active route. Its seven failures remain a standard-route defect.

Owner `1.3.1.2` controls `test-artifacts/vista-driver-deploy-service.c`. It must add post-reboot hash proof for all installed payloads.

Expected gate: package work targets root G1. Guest-owned deployment and repeated proof target root G6 and G7. The unchanged `triton9_runtime_probe.c` is an acceptance oracle. No implementation owner can change it during gates G2-G4.

## Required cross-owner interface invariants

1. The current-source UMD, renderer, QEMU dependency, KMD package, and guest files must share recorded hashes.
2. The guest and host Neptune wire versions must match. A private command change requires a version change on both sides.
3. Every async failure must become visible at a synchronized public boundary. Missing status must never become success.
4. Clear, draw, readback, and present must use the same real resources. No path can fabricate pixels or handles.
5. The public probe must fail closed on any missing value. Its source and call order remain unchanged.
6. The active diagnostic INF remains the package input until root explicitly activates the standard template.
7. The guest service must prove final installed hashes before it starts the public probe.

## Triton and execution boundaries

Triton-only correction: all implementation work stays in Triton UMD, KMD, transport, QEMU, renderer, DXMT, and package code. Guest-owned boundary: the guest service owns installation, reboot, recovery, probe start, and result persistence.

Windows patch: none. Host input: none. Aero shortcut: none.

VirtualBox remains a Vista-era behavior reference only. Its transport and object model are not implementation targets.

The host can build signed media and expose it read-only. It cannot select guest boot state or send guest input. The public probe must pass clear/readback, triangle/readback, and PresentEx before any passive PNG or Aero analysis.

## Adversarial review

The strongest refutation of Rank 1 is the public count of zero. The Microsoft contract also defines a valid zero-count viewport clear. This refutation prevents any claim that Vista supplied a positive DDI count. It does not falsify Rank 1 because raw DDI arguments remain absent.

The missing depth records also support Rank 2. The deployed artifact lacked an entry marker, so no-callback and early-return remain equal.

The successful target clear weakens Rank 2 and Rank 3. It does not prove host completion because the clear transport is asynchronous. The current renderer compile failure cannot explain the scoped guest result. QEMU linked the older `host-triton` renderer for that result.

The inactive INF failures also cannot explain it. The active x64 diagnostic INF passes its audit.

SetDepthStencil success disproves a resource-creation explanation. It does not prove the later clear callback entry. Artifact mismatch is weak because the installed hashes match the disassembled pair. Five scoped proof records also reached the same sink.

Ranking after review: Rank 1 remains first, but only as the narrowest structural match. Rank 2 remains a close alternative. Ranks 3 through 5 remain lower because adjacent success or direct artifact evidence contradicts them. The decisive entry-first result can change this order.

## No-claim summary

No clear pixel exists. No triangle pixel exists. No PresentEx result exists. No public D3D9 pass exists. No Aero or glass claim is allowed from the persisted evidence.
