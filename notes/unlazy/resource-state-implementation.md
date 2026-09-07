# Resource, state, status, and synchronization implementation

Date: 2026-08-26

## Result

The assigned implementation is complete in the owned files. The final x64 and x86 UMD builds pass. The focused native tests and the renderer build also pass.

The changes preserve actual resource bytes and report uncertain completion as failure. They do not generate replacement pixels for failed readback or transport work.

No VM was started, stopped, inspected, or changed. This report does not claim public-probe success, DWM success, or Aero success.

## Scope and ownership

`PLAN.md:24-30` defines all implementation owners. `PLAN.md:27` assigns this leaf these 13 source files:

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

The source edits stay in that list. This leaf also owns this report and `gates/leaf-1.2.1.2.md`.

## Four completed passes

Pass 1 implemented the generated-header, resource, state, query, ring, renderer, and bridge corrections. It also added focused CPU-layout cases.

Pass 2 traced the Vista D3D9 resource contract again. It compared the UMD descriptors with the KMD-generated descriptors and transport lifetime.

Pass 3 searched for stale-shadow writes, unchecked completion, cross-device handles, integer overflow, partial teardown, and unbounded waits. It corrected each owned defect.

Pass 4 removed stale comments, made both protocol copies identical, checked whitespace, rebuilt both UMDs, and reran all focused tests.

## Generated HANDLE decoder

Fact: `npt_decode_HANDLE` now uses the integer-zero expression `(HANDLE)0` on overflow. The UMD copy is at `triton-umd/src/virtio/neptune/neptune-protocol/npt_protocol_common_types.h:13626-13635`.

Fact: The renderer copy has the same code at `triton-virglrenderer/src/neptune/neptune-protocol/npt_protocol_common_types.h:13626-13635`.

Fact: `cmp -s` returns zero. Both files have SHA-256 `9b0b39f5ae50ce463c845a97f73b712cb10b5c015d8deb334a6df789b0cb7790`.

Observable failure prevented: the generated C decoder no longer assigns pointer-only `NULL` to an integer HANDLE definition.

## Resource declaration and layout

Fact: Each resource stores its owner device, host transaction state, and exact lock region. See `TRITON9_RESOURCE` in `triton-umd/src/virtio/neptune/vista-d3d9/triton9.h:86-156`.

Fact: `triton9ResourceBelongsToDevice` compares the stored owner with the active device. See `triton-umd/src/virtio/neptune/vista-d3d9/triton9.h:251-257`.

Fact: `TRITON9_CPU_LAYOUT` records logical row bytes, physical row pitch, logical row count, slice pitch, and total bytes. See `triton-umd/src/virtio/neptune/vista-d3d9/triton9_cpu_layout.h:20-26`.

Fact: `triton9CpuLayout2D` uses checked multiplication and addition. It rejects a short row pitch and a short slice. See `triton-umd/src/virtio/neptune/vista-d3d9/triton9_cpu_layout.c:53-83`.

Fact: `triton9CpuRegionFits` checks logical rows and logical row bytes. Slice padding cannot become a fabricated row or pixel. See `triton-umd/src/virtio/neptune/vista-d3d9/triton9_cpu_layout.c:85-109`.

Fact: SYSTEMMEM shadows alias Vista's `pSysMem`. Driver-owned shadows use private heap storage. See `triton9CreateShadow` in `triton-umd/src/virtio/neptune/vista-d3d9/triton9_resource.c:82-146`.

Observable failure prevented: a padded runtime surface keeps its supplied pitch. Copies cannot write into row padding or an extra padded slice.

## Allocation and host materialization

Fact: `triton9CreateResource` rejects unsupported surface counts, explicit mip chains, volume resources, and multisampling. It preserves one declared subresource. See `triton-umd/src/virtio/neptune/vista-d3d9/triton9_resource.c:1052-1209`.

Fact: Primary creation uses one exact 2D allocation descriptor. It preserves `VidPnSourceId` and uses exact `width * 4 * height` storage. See `triton9AllocateStandardPrimary` at `triton-umd/src/virtio/neptune/vista-d3d9/triton9_resource.c:536-597`.

Fact: The KMD generates the same target, depth, array, mip, sample, pitch, and size values. See `triton-kmd/viogpu/viogpu3d/viogpu_allocation.cpp:1092-1203`.

Fact: Shared allocation export validates format, plane count, pitch, offset, allocation size, and page rounding. See `triton9RegisterSharedTexture` at `triton-umd/src/virtio/neptune/vista-d3d9/triton9_resource.c:424-528`.

Fact: The KMD applies the same Vista shared-texture limits. See `IsValidVistaSharedTexture` at `triton-kmd/viogpu/viogpu3d/viogpu_allocation.cpp:240-327`.

Fact: `triton9EnsureResourceHost` treats host creation, KMD registration, initial upload, and status checking as one transaction. See `triton-umd/src/virtio/neptune/vista-d3d9/triton9_resource.c:254-347`.

Fact: `hostReady` becomes true only after the complete transaction. If cleanup is uncertain, the code retains dependent objects and marks the device lost.

Fact: BGRA8 and BGRX8 ordinary color textures include `D3D11_BIND_RENDER_TARGET`. This implements ColorFill for advertised OFFSCREENPLAIN resources. See `triton9CreateHostTexture` at `triton-umd/src/virtio/neptune/vista-d3d9/triton9_resource.c:203-251`.

Observable failure prevented: a default-pool off-screen color surface no longer fails RTV creation during ColorFill.

## OpenResource and resource identity

Fact: `triton9OpenResource` accepts one allocation and two known private descriptor types. See `triton-umd/src/virtio/neptune/vista-d3d9/triton9_resource.c:1854-1883`.

Fact: A standard primary must match the exact KMD target, bind, flag, dimension, sample, and size contract. See `triton-umd/src/virtio/neptune/vista-d3d9/triton9_resource.c:1891-1949`.

Fact: A non-primary standard 3D resource must match the generated staging or shadow shape. The size must equal `width * 4 * height`. See `triton-umd/src/virtio/neptune/vista-d3d9/triton9_resource.c:1952-2000`.

Fact: Shared descriptors validate every plane and reject unknown dimensions, formats, usage, bind flags, and extra plane data. See `triton-umd/src/virtio/neptune/vista-d3d9/triton9_resource.c:2004-2044`.

Fact: Opened resources are not lockable because they have no valid UMD or runtime CPU backing. See `triton-umd/src/virtio/neptune/vista-d3d9/triton9_resource.c:2046-2075`.

Fact: The bridge import and host open must both succeed before publication. A host-device check follows the open. See `triton-umd/src/virtio/neptune/vista-d3d9/triton9_resource.c:2076-2129`.

Observable failure prevented: malformed private data cannot create a synthetic resource with invented pitch, size, dimensions, or CPU bytes.

## Lock, rename, upload, and readback

Fact: `triton9Lock` validates flags, resource ownership, subresource zero, exact range or area, and SYSTEMMEM NotifyOnly rules. See `triton-umd/src/virtio/neptune/vista-d3d9/triton9_resource.c:1383-1476`.

Fact: A NoOverwrite buffer lock now requires `RangeValid`. This requirement prevents a later full stale-shadow upload.

Fact: ReadOnly locks and read-write locks read back real GPU bytes. WriteOnly, NoOverwrite, and Discard locks do not consume stale shadow bytes.

Fact: Unlock uploads only the active byte range or rectangle. See `triton9UploadLockedShadow` at `triton-umd/src/virtio/neptune/vista-d3d9/triton9_resource.c:736-777`.

Fact: LockAsync is write-only. Discard uses a rename cookie, creates a replacement, uploads first, rebinds all live views, and then retires the old object. See `triton-umd/src/virtio/neptune/vista-d3d9/triton9_resource.c:1518-1754`.

Fact: Buffers with `MightDrawFromLocked` upload their exact active lock before each draw. See `triton9SynchronizeLockedBuffers` at `triton-umd/src/virtio/neptune/vista-d3d9/triton9_resource.c:820-860`.

Fact: GPU readback uses Copy, Flush, an ordered bridge drain, a host-device check, and Map. See `triton9MapStagingForRead` at `triton-umd/src/virtio/neptune/vista-d3d9/triton9_resource.c:906-943`.

Fact: Surface readback validates both source and destination layouts. It copies only requested rows and bytes. See `triton-umd/src/virtio/neptune/vista-d3d9/triton9_resource.c:945-1001`.

Observable failure prevented: an asynchronous Clear or Copy cannot return old CPU bytes as successful readback.

## Destruction and imported-resource lifetime

Fact: `triton9DeallocateResource` distinguishes shared resource-group lifetime from ordinary allocation lifetime. It clears ownership only after callback success. See `triton-umd/src/virtio/neptune/vista-d3d9/triton9_resource.c:599-626`.

Fact: `triton9DestroyResource` first removes live bindings. It then proves KMD deallocation before it frees UMD storage. See `triton-umd/src/virtio/neptune/vista-d3d9/triton9_resource.c:1260-1347`.

Fact: Imported teardown releases the host wrapper, drains the primary ring, and checks import deallocation. It retains handles after uncertain failure. See `triton-umd/src/virtio/neptune/vista-d3d9/triton9_resource.c:1348-1379`.

Fact: Failed OpenResource cleanup uses the same primary-ring barrier and checked import release. See `triton-umd/src/virtio/neptune/vista-d3d9/triton9_resource.c:2131-2152`.

Observable failure prevented: the host cannot keep using an imported allocation after the UMD frees its tracking record.

## Render state and replay

Fact: Blend, depth-stencil, rasterizer, and sampler creation is transactional. A failed D3D11 create does not replace the last valid state. See `triton-umd/src/virtio/neptune/vista-d3d9/triton9_state.cpp:279-500`.

Fact: D3D9 clockwise faces remain the D3D11 front face. `D3DCULL_CW` culls front faces, and CCW stencil maps to `BackFace`. See `triton-umd/src/virtio/neptune/vista-d3d9/triton9_state.cpp:335-453`.

Fact: Render-state indices are bounds-checked before array access. Legal values are validated before storage. See `triton9ValidateRenderState` at `triton-umd/src/virtio/neptune/vista-d3d9/triton9_state.cpp:558-836`.

Fact: Fog state accepts FOGENABLE and vertex NONE, LINEAR, EXP, or EXP2. Table fog and range fog fail closed. See `triton-umd/src/virtio/neptune/vista-d3d9/triton9_state.cpp:689-744`.

Fact: Fog defaults are disabled, NONE, color zero, start zero, end one, and density one. See `triton-umd/src/virtio/neptune/vista-d3d9/triton9_state.cpp:895-903`.

Fact: The draw leaf reads these stored values for each draw. See `triton-umd/src/virtio/neptune/vista-d3d9/triton9_shader.cpp:1108-1158` and `:1520-1650`.

Fact: DITHERENABLE accepts both Boolean values only for BGRA8 or BGRX8 render targets. See `triton-umd/src/virtio/neptune/vista-d3d9/triton9_cpu_layout.c:111-123` and `triton9_state.cpp:1319-1325`.

Inference: Dither is output-neutral for these two 32-bit targets. A future lower-bit target will fail until its quantization behavior exists.

Fact: BLENDFACTOR defaults to all ones. Viewport changes preserve an explicit Z range. See `triton-umd/src/virtio/neptune/vista-d3d9/triton9_state.cpp:893` and `:1250-1289`.

Fact: `triton9SetRenderState` marks only translated D3D11 objects dirty. Fog stays in the shared state array for shader conversion. See `triton-umd/src/virtio/neptune/vista-d3d9/triton9_state.cpp:970-1031`.

Fact: `triton9PreparePipelineState` rebuilds dirty objects and replays every binding before draw. It checks transport and device state at return. See `triton-umd/src/virtio/neptune/vista-d3d9/triton9_state.cpp:1309-1363`.

Observable failure prevented: a dirty-state creation failure cannot silently keep the new D3D9 value with an unrelated D3D11 object.

## Queries and completion status

Fact: Query creation rejects unsupported types before host-device startup. It publishes the query only after host creation and a device check. See `triton-umd/src/virtio/neptune/vista-d3d9/triton9_query.cpp:62-109`.

Fact: Query handles belong to an explicit per-device list. Issue validates Begin and End ordering. See `triton-umd/src/virtio/neptune/vista-d3d9/triton9_query.cpp:30-52` and `:141-193`.

Fact: Event `S_OK` with a false completion value becomes `S_FALSE`. Query results stay local until the final transport-health check passes. See `triton-umd/src/virtio/neptune/vista-d3d9/triton9_query.cpp:195-250`.

Fact: A local feedback result still calls `triton9CheckHostDevice`. A poisoned ring cannot become successful query completion.

Observable failure prevented: an asynchronous transport error cannot leave a successful event or occlusion result in the caller's output.

## Ring transport and renderer status

Fact: The ring requires lock-free 32-bit atomics. Fatal status, submit failure, or timeout poisons the ring. See `triton-umd/src/virtio/neptune/npt_ring.c:24-25` and `:58-89`.

Fact: Command bytes become visible before the tail. Cross-thread diagnostics read the atomic published tail instead of non-atomic `cur`. See `triton-umd/src/virtio/neptune/npt_ring.c:247-299`.

Fact: Ring-space waits, full drains, and sequence waits have a 15-second limit. They return false or `UINT32_MAX` and poison the ring. See `triton-umd/src/virtio/neptune/npt_ring.c:316-347` and `:519-594`.

Fact: COM release drains all peer rings before it submits on the primary ring. Its owned API returns the real drain and submit status. See `triton-umd/src/virtio/neptune/npt_ring.c:411-482` and `npt_ring.h:189-198`.

Fact: A fresh shared-memory resource requires a checked virtqueue roundtrip. Both the synchronous submit and the ring wait must succeed. See `triton-umd/src/virtio/neptune/npt_ring.c:656-697`.

Fact: Raw submit APIs and force-roundtrip now return Boolean status. See `triton-umd/src/virtio/neptune/npt_ring.h:130-187` and `npt_ring.c:748-898`.

Fact: Reply allocation, reply bounds, command submission, and completion waits all fail closed. See `triton-umd/src/virtio/neptune/npt_ring.c:900-1062`.

Fact: Failed ring CREATE or DESTROY keeps host-visible mappings. It removes the guest-only profile record and both guest mutexes. See `triton-umd/src/virtio/neptune/npt_ring.c:91-106` and `:1064-1240`.

Fact: The Win32 renderer preserves exact callback failure status. It validates replacement command buffers after every Render. See `triton-umd/src/virtio/neptune/npt_renderer_virtgpu_win32.c:215-222` and `:255-337`.

Fact: The renderer event marker is terminal and has a finite wait. See `triton-umd/src/virtio/neptune/npt_renderer_virtgpu_win32.c:340-404`.

Fact: The KMD accepts that marker only as the terminal packet. It signals after preceding serialized work. See `triton-kmd/viogpu/viogpu3d/viogpu_device.cpp:1294-1326` and `viogpu_command.cpp:383-396`.

Fact: Blob teardown drains before deallocation. Unlock, unmap, and destroy failures retain remaining backing. See `triton-umd/src/virtio/neptune/npt_renderer_virtgpu_win32.c:783-861`.

Fact: Import release has a checked Boolean entry point. Async and sync renderer submits return their real status. See `triton-umd/src/virtio/neptune/npt_renderer_virtgpu_win32.c:928-950` and `:1021-1052`.

Fact: The bridge drains the exact wrapper or primary ring and then drains renderer GPU work. See `triton-umd/src/virtio/neptune/triton/tritonSharedBridge.c:39-105`.

Observable failure prevented: a renderer submit error or bounded-wait timeout cannot become a later successful D3D9 operation.

## D24S8 link dependency

Fact: The owned header declares `BOOL triton9HasCompleteD24S8ClearContract(void);`. See `triton-umd/src/virtio/neptune/vista-d3d9/triton9.h:267-270`.

Fact: The format owner calls that external symbol before it counts D24S8. See `triton-umd/src/virtio/neptune/vista-d3d9/triton9_format.c:79-84`.

Fact: The clear owner defines the symbol in `triton-umd/src/virtio/neptune/vista-d3d9/triton9_output.c:145-151`.

Fact: `nm` shows an undefined reference in each format object and a definition in each output object. Both final UMD links pass.

Observable failure prevented: a build without the complete clear implementation cannot silently advertise D24S8 through a header-only macro.

## Fail-closed summary

| Failure point | Returned or retained result | Evidence |
| --- | --- | --- |
| Host materialization fails | `hostReady` stays false; uncertain dependencies stay allocated | `triton9_resource.c:254-347` |
| Readback drain fails | Device becomes removed; Map does not expose bytes | `triton9_resource.c:906-943` |
| Lock has no exact NoOverwrite range | Lock returns `D3DDDIERR_INVALIDCALL` | `triton9_resource.c:1434-1438` |
| Imported deallocation fails | Import handles and record stay live | `triton9_resource.c:1348-1374` |
| D3D11 state creation fails | The prior valid state object stays installed | `triton9_state.cpp:279-500` |
| Query is incomplete or transport health fails | Output stays unchanged; the call returns non-success | `triton9_query.cpp:216-250` |
| Ring submit or wait fails | Ring stays poisoned; later health checks fail | `npt_ring.c:58-89`, `:519-594` |
| Ring create or destroy is uncertain | Host-visible mappings remain allocated | `npt_ring.c:91-106`, `:1191-1230` |
| Blob teardown is uncertain | Allocation or mapping remains allocated | `npt_renderer_virtgpu_win32.c:783-861` |

## Verification evidence

All commands below ran from `/Users/jumar/winvistachecked`, unless a working directory is stated.

UMD build, x64 working directory `triton-umd/build-vista-x64-unified`:

```text
meson compile -C .
```

Result: exit 0. The final rebuild compiled the ring and query changes and linked `neptune_d3d9.dll`.

UMD build, x86 working directory `triton-umd/build-vista-x86-unified`:

```text
meson compile -C .
```

Result: exit 0. The final rebuild compiled the ring and query changes and linked `neptune_d3d9.dll`.

Focused x64 tests, working directory `triton-umd`:

```text
meson test -C build-vista-x64-unified triton9-cpu-layout triton9-clear-contract triton9-draw-contract triton9-shader-token-contract --no-rebuild --print-errorlogs
```

Result: 4 passed, 0 failed, exit 0.

Focused x86 tests used the same command with `build-vista-x86-unified`. Result: 4 passed, 0 failed, exit 0.

The direct CPU-layout test binaries both print `triton9 CPU layout contract: PASS` and return zero.

```text
./triton-umd/build-vista-x64-unified/src/virtio/neptune/vista-d3d9/triton9_cpu_layout_test
./triton-umd/build-vista-x86-unified/src/virtio/neptune/vista-d3d9/triton9_cpu_layout_test
```

The CPU test covers ownership, padded pitch, partial rows, logical bounds, overflow rejection, and the dither format dependency. See `triton-umd/src/virtio/neptune/vista-d3d9/tests/triton9_cpu_layout_test.c:12-188`.

Renderer build, working directory `triton-virglrenderer`:

```text
meson compile -C build-arm64
```

Result: exit 0. Ninja reports no work because the renderer output is newer than both owned renderer inputs.

Both Vista PE audits pass with exit 0:

```text
sh triton-umd/build-support/audit-vista-d3d9-pe.sh x64 triton-umd/build-vista-x64-unified/src/virtio/neptune/vista-d3d9/neptune_d3d9.dll
sh triton-umd/build-support/audit-vista-d3d9-pe.sh x86 triton-umd/build-vista-x86-unified/src/virtio/neptune/vista-d3d9/neptune_d3d9.dll
```

Final artifact SHA-256 values:

```text
cd74c493d39e8e93c0594a17413163973435910f7e34832da22485916a73add4  x64 neptune_d3d9.dll
6fb45929654ecca83b71cb9a8159c70223638bd3565a0215314854b1062010e3  x86 neptune_d3d9.dll
1587a08a69ee844983353613e8659381e282892502da8766f684013e9dbd1ac3  x64 triton9_cpu_layout_test
d928de615916bdafdebfb747c4a786c88fdc5cd93ea430d53390c546517ea868  x86 triton9_cpu_layout_test
d896f189b1416236f74383f8f21f2d85cda2750873a54a92632740e548002e43  libvirglrenderer.1.dylib
feef713e435812e55b0228f53ca9e8e3900db825e7c56b507551895bf2ded529  virgl_render_server
d349b0c45c08ddde2aa0a44be7c60ab47ace31c86a020e46a27650cb195d0448  virgl_test_server
```

`ninja -n` reports no pending renderer work. Each UMD build has only its always-run `git_sha1.h` custom command.

The owned-source trailing-whitespace search returned no matches. The tracked owned-file `git diff --check` commands also returned zero.

## Cross-owner integration requirements

The following items need an explicit ownership expansion before edits. They are not independently correctable inside this leaf.

1. `triton-umd/src/virtio/neptune/npt_tls.c:90-145`, symbol `npt_tls_get_ring`, ignores the Boolean result from `npt_ring_wait_all` at line 114. A failed primary drain can still return a ring pointer.

2. `triton-umd/src/virtio/neptune/npt_overrides_d3d11_query.c:125-149` ignores `npt_ring_force_roundtrip` at line 144. Registration continues after a failed resource-table barrier.

3. `triton-umd/src/virtio/neptune/npt_overrides_d3d11_fence.c:95-115` ignores `npt_ring_force_roundtrip` at line 111. Fence feedback registration has the same failure mode.

4. `triton-umd/src/virtio/neptune/triton/tritonSharedBridge.h:68-69` declares import release as `void`. `triton-umd/src/virtio/neptune/npt_renderer.h:88-119` also defines a void release callback.

5. `triton-umd/src/virtio/neptune/triton/tritonResource.c:461-475` clears both import handles after an unchecked release. A teardown failure cannot be retried.

6. `triton-umd/src/virtio/neptune/triton/tritonResource.c:642-651` does the same during failed OpenResource cleanup.

7. The owned compatibility wrappers must ignore status while those two interfaces stay void. See `tritonSharedBridge.c:149-159` and `npt_renderer_virtgpu_win32.c:945-950`.

8. `triton-umd/src/virtio/neptune/npt_com.c:469-475`, symbol `npt_com_send_release`, ignores the Boolean result from `npt_ring_send_com_release`. Its void interface cannot report a failed primary-ring release.

9. Runtime-DDI `virtgpu_drain` cannot query device execution state. Its event wake returns true at `npt_renderer_virtgpu_win32.c:385-390`.

The D3D9 path closes item 9 for readback and query completion. It calls `triton9CheckHostDevice` after each ordered bridge drain.

The general D3D11 path still needs an explicit status consumer. An event wake alone can also represent reset or removal.

The output and draw owners were asked to move resource ownership checks before their first field access. Current review locations are:

- `triton-umd/src/virtio/neptune/vista-d3d9/triton9_output.c:303-390`, `triton9SetRenderTarget` and `triton9SetDepthStencil`.
- `triton-umd/src/virtio/neptune/vista-d3d9/triton9_output.c:627-915`, `triton9Blt`, `triton9BufBlt`, and `triton9TexBlt`.
- `triton-umd/src/virtio/neptune/vista-d3d9/triton9_output.c:1013-1078`, `triton9ColorFill` and `triton9GenerateMipSubLevels`.
- `triton-umd/src/virtio/neptune/vista-d3d9/triton9_shader.cpp:3260-3283`, `triton9SetStreamSource`.
- `triton-umd/src/virtio/neptune/vista-d3d9/triton9_shader.cpp:3391-3415`, `triton9SetIndices`.

Observable failure before sibling integration: a valid resource handle from another device can be read before the owned helper rejects it. SYSTEMMEM copies need direct checks.

## Final source sweep

Files searched: 13 owned source files.

The sweep read all owned functions and searched for unchecked waits, ignored checked teardown, stale-shadow upload, invalid padding, and cross-device resource use.

No independently fixable owned-file issue remains. The exact cross-owner interfaces are listed above. Omitted relevant owned surfaces: none.

The report separates verified source facts from the one stated dither inference. It makes no claim from desktop visibility or opaque frames.
