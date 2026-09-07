# Triton Vista WDDM and D3D9 architecture audit

## Scope, result, and evidence rules

This report maps the current source path. It does not report a VM result. The audit did not start, stop, drive, or mutate a VM.

`Fact` means that the cited source states or implements the claim. `Inference` means that the claim follows from two or more source facts.

### Citation path convention

A path-qualified citation fixes the exact file. Later short citations use these fixed directories:

| Short citation | Exact directory |
|---|---|
| `triton9_*`, `triton9.h`, `neptune_d3d9*.def` | `triton-umd/src/virtio/neptune/vista-d3d9/` |
| `triton9_runtime_probe.c` | `triton-umd/src/virtio/neptune/vista-d3d9/tests/` |
| `npt_renderer_virtgpu_win32.c`, `npt_shared_texture.c` | `triton-umd/src/virtio/neptune/` |
| `tritonSharedBridge.c` | `triton-umd/src/virtio/neptune/triton/` |
| `driver.cpp`, `driver.h`, `viogpu_*.cpp` | `triton-kmd/viogpu/viogpu3d/` |
| `virtio-gpu.c`, `virtio-gpu-gl.c`, `virtio-gpu-virgl.c` | `triton-qemu/hw/display/` |
| `virglrenderer.c` | `triton-virglrenderer/src/` |
| `proxy_renderer.c`, `proxy_server.c` | `triton-virglrenderer/src/proxy/` |
| `npt_renderer.c`, `npt_context.c`, `npt_shared.c` | `triton-virglrenderer/src/neptune/` |
| `render_state.c` | `triton-virglrenderer/server/` |
| Unqualified deployment, batch, and INF names | `test-artifacts/`, except for a stated nested directory |

Line ranges are inclusive. A symbol name is the evidence anchor when a statement names the symbol instead of a line range.

The source contains a coherent Triton path from Vista D3D9 to a host D3D11 backend. It also contains public-capability contradictions and transport assumptions. These items can fail after adapter admission. They require Triton miniport or Triton UMD changes, not Windows changes.

The required evidence order is exact:

1. The public guest probe must prove clear and readback.
2. The same probe must prove a triangle and readback.
3. The same probe must prove `PresentEx`.
4. Only then can passive PNG evidence prove Aero glass.

The probe enforces this order in `probeServiceMain`. It skips the Aero path after a D3D9 failure. See `triton-umd/src/virtio/neptune/vista-d3d9/tests/triton9_runtime_probe.c:2651-2697`.

The probe itself calls `GetRenderTargetData` and `LockRect` in `probeReadPixel`. See `triton9_runtime_probe.c:930-954`. It verifies clear pixels at lines 2022-2043. It verifies triangle pixels at lines 2045-2127. It calls `PresentEx` at lines 2462-2478. Its terminal pass also requires the strict hardware-plus-pure profile and an additional swap-chain present. See lines 1880-1907 and 2480-2515.

The later DWM API path labels its own output `VISUAL-UNVERIFIED`. See `triton9_runtime_probe.c:805-825`. Therefore, an API return, a title, or an opaque window is not glass evidence.

## Component and build inventory

### Build entry points

- `scripts/build_deploy_vista_driver.sh` is the package orchestrator. Its public modes only build or restage. It rejects host-driven deployment at lines 64-87.
- The orchestrator builds x64 and x86 UMDs and both public probes at `scripts/build_deploy_vista_driver.sh:1467-1492`. It builds the guest service at lines 1494-1497.
- The orchestrator audits package binaries and INF files at `scripts/build_deploy_vista_driver.sh:1518-1543`. It audits source contracts at lines 1545-2044.
- The orchestrator builds, signs, stages, and hashes both packages at `scripts/build_deploy_vista_driver.sh:2046-2143`.
- `triton-umd/src/virtio/neptune/vista-d3d9/meson.build:4-17` lists the D3D9 sources. Lines 42-78 define the Vista target and DLL. Lines 80-140 define ABI, CPU, draw, shader, and probe targets.
- `triton-umd/src/virtio/neptune/meson.build:114-119` selects the isolated Vista D3D9 subdirectory. `triton-umd/meson.options:251-255` defines its build option.
- `test-artifacts/windows11_build_vista_serialtrace_kmd.bat:3-24` mirrors the KMD source and selects the diagnostic project. Lines 34-64 build x64 and x86 KMDs.
- `test-artifacts/windows11_stage_vista_pnp_package.bat:14-36` stages both packages. Lines 38-46 version, catalog, and sign them.
- `triton-kmd/viogpu/viogpu_vista.sln:5-16` and `triton-kmd/viogpu/viogpu3d/viogpu3d.vcxproj:86-105` define the standard Vista solution route. The project outputs are at lines 256-266. Its UMD staging contract is at lines 699-722.
- `scripts/build_vista_deploy_service.sh:4-43` builds and audits the x64 guest service.
- `scripts/stage_vista_deploy_media.sh:14-64` verifies the package and creates immutable media metadata. Lines 66-92 create and report the read-only ISO.
- `triton-qemu/hw/display/meson.build:67-83` builds the virtio-gpu and virgl display modules. Lines 104-142 build the PCI and VGA variants.
- `triton-virglrenderer/meson.build:353-363` enables Neptune and the render server. Lines 396-418 define Neptune and the server path.
- `triton-virglrenderer/src/meson.build:110-128` lists Neptune sources. Lines 172-190 list proxy sources. Lines 251-268 add Neptune. Lines 295-336 build the libraries.
- `triton-virglrenderer/server/meson.build:4-35` builds `virgl_render_server`.
- `triton-dxmt/meson.build:227-233` selects one native DXMT image. `triton-dxmt/src/meson.build:1-29` selects its D3D11, DXGI, Metal, and native-image sources.
- `triton-dxmt/src/dxmt-native/meson.build:1-42` links those modules into `libdxmt-native.dylib`.

The active package path differs from the standard solution route. The active Windows batch replaces `viogpu3d.vcxproj` with `test-artifacts/viogpu3d-vista-serialtrace.vcxproj`. See `windows11_build_vista_serialtrace_kmd.bat:8-24`. The gap inventory records this split.

### Driver binaries

- `viogpu3d.sys` is the Vista WDDM 1.0 display miniport. `DriverEntry` registers the Vista SP1 table in `triton-kmd/viogpu/viogpu3d/driver.cpp:85-210`.
- `neptune_d3d9.dll` is the native D3D9 UMD. `triton-umd/src/virtio/neptune/vista-d3d9/neptune_d3d9.def:1-3` exports only `OpenAdapter`.
- `neptune_d3d9_wow.dll` is the x86 UMD in the x64 package. The build stages both names at `scripts/build_deploy_vista_driver.sh:1480-1491`.
- The x86 export decoration is explicit in `triton-umd/src/virtio/neptune/vista-d3d9/neptune_d3d9_x86.def:1-3`.
- `triton-qemu/build/qemu-system-x86_64`, `host-triton/lib/libdxmt-native.dylib`, and `host-triton/libexec/virgl_render_server` are host runtime components. Their expected paths appear in `scripts/run_vista_guest_deploy.sh:13-16`.

### Active UMD artifact provenance

An independent read-only archive stream inspected `/Users/jumar/winvistachecked/winvista-3.shrunk.qcow2`. This audit did not open or mutate that disk.

The prior offline inventory records the same installed paths and byte sizes. See `notes/unlazy/offline-evidence.md:209-220`.

The installed guest files have these SHA-256 values:

| Guest path | SHA-256 |
|---|---|
| `C:\Windows\System32\neptune_d3d9.dll` | `2dbb366ca08f6605cf4cf4d279463930be6ca98c2d789fc82e55918b4d9a35f7` |
| `C:\Windows\SysWOW64\neptune_d3d9_wow.dll` | `589337d20788960810ab3a987b99e90efcf16e2b412880fc399d7f5d80df9d36` |

Those values exactly match `test-artifacts/vista-unified-umd/neptune_d3d9.dll` and `test-artifacts/vista-unified-umd/neptune_d3d9_wow.dll`.

They do not match the current outputs. The x64 file at `triton-umd/build-vista-x64-unified/src/virtio/neptune/vista-d3d9/neptune_d3d9.dll` has SHA-256 `2047dba5f0d5e4a898d3f2d0875cacf4d5f5f3dbd44f9b2c51d19780b4bbdc81`. The x86 file at `triton-umd/build-vista-x86-unified/src/virtio/neptune/vista-d3d9/neptune_d3d9.dll` has SHA-256 `1f83e22ec188ac38002b300b156d60620ea31ed35e799a01159305b24ec2b2d2`.

The independent build baseline records those current output identities and sizes at `notes/unlazy/build-baseline.md:249-258`.

This is artifact evidence, not source evidence. C34 records the observable version split.

### Probe binaries

- `triton9_runtime_probe_x64.exe` is the native public guest probe.
- `triton9_runtime_probe_x86.exe` is the WoW64 public guest probe.
- Both binaries come from `triton9_runtime_probe` in `scripts/build_deploy_vista_driver.sh:1480-1491`.
- The probe source is `triton-umd/src/virtio/neptune/vista-d3d9/tests/triton9_runtime_probe.c`.

### Guest service

`triton-vista-deploy.exe` is the guest-owned deployment service. Its source is `test-artifacts/vista-driver-deploy-service.c`.

The service verifies the immutable manifest and required files in `verify_package_manifest`. See lines 366-456. It verifies the optional probe hash at lines 487-505.

The service stages and starts a deployment-specific one-shot probe service at lines 508-605. It schedules protected payload replacement at lines 614-710.

Normal-mode orchestration is in `install_normal_mode` and `process_normal_mode`. See `vista-driver-deploy-service.c:997-1110`. Safe-mode recovery is in `process_safe_mode` at lines 910-990.

`service_main` selects normal or safe processing at `vista-driver-deploy-service.c:1151-1181`. `install_self`, `uninstall_self`, and `wmain` are at lines 1183-1280.

`test-artifacts/VISTA_GUEST_DEPLOY.md:1-5` states the guest-owned rule. Some implementation details in that document are stale. C26 records the exact differences. The service source is the current deployment authority in this map.

### Verification tools

- `triton-kmd/viogpu/tools/check_vista_kmd_source.py:54-249` audits Vista callback, pool, scheduler, render-event, and submission invariants. Lines 252-271 provide its entry point.
- `triton-kmd/viogpu/tools/check_vista_pe.py:160-204` verifies architecture, subsystem, imports, and exports. Lines 229-254 provide its entry point.
- `triton-kmd/viogpu/tools/check_vista_inf.py:112-209` verifies package layout and registration. Its x64 and x86 requirements start at lines 22-68.
- `triton-umd/build-support/audit-vista-d3d9-pe.sh:5-54` verifies the UMD architecture, Vista subsystem, imports, and sole `OpenAdapter` export.
- `triton-umd/src/virtio/neptune/vista-d3d9/meson.build:80-140` builds ABI, CPU-layout, draw-contract, shader-token, and runtime-probe targets.
- `scripts/verify_aero_glass_pixels.py:1-15` defines the PNG-only glass rule. Lines 95-177 compute the signal, attenuation, blur, and result.
- The pixel tool has an in-memory positive and negative self-test at `verify_aero_glass_pixels.py:180-204`. Its command exit rule is at lines 207-219.
- `scripts/build_deploy_vista_driver.sh:1370-1407` hashes the reviewed source set. Lines 2077-2083 reject source changes during a package build.

## Triton architecture

The end-to-end path is Triton-specific:

```text
Vista application or DWM
  -> public d3d9.dll / D3D9Ex runtime
  -> neptune_d3d9.dll (Triton D3D9 UMD)
  -> Vista runtime callbacks
  -> viogpu3d.sys (Triton WDDM 1.0 miniport)
  -> virtio-gpu control queue
  -> Triton QEMU virtio-gpu-virgl
  -> virglrenderer proxy and virgl_render_server
  -> Neptune renderer and generated D3D11 protocol
  -> libdxmt-native.dylib host D3D11 backend
```

The UMD embeds a generated Neptune D3D11 client. This is not a dependency on Windows D3D11. The target forbids OS D3D11 and DXGI imports at `triton-umd/src/virtio/neptune/vista-d3d9/meson.build:62-78`.

QEMU exposes the `neptune` and `neptune-capset` properties at `triton-qemu/hw/display/virtio-gpu-gl.c:147-165`. Device realization requires blob and host memory at `virtio-gpu.c:1556-1566`.

QEMU sends three-dimensional command buffers to virglrenderer in `virtio-gpu-virgl.c:685-722`. It maps Neptune scanout to a CPU display surface at lines 578-683. It reads flushed scanout bytes at lines 478-575.

Virglrenderer routes Neptune capsets and contexts through its proxy. See `triton-virglrenderer/src/virglrenderer.c:182-289` and lines 590-620. It initializes the proxy at lines 967-974.

The proxy creates a server and client in `triton-virglrenderer/src/proxy/proxy_renderer.c:22-44`. It obtains Neptune caps at lines 64-80.

The server uses an external process or a same-process thread. See `triton-virglrenderer/src/proxy/proxy_server.c:104-174` and lines 177-249.

The render server selects Neptune in `triton-virglrenderer/server/render_state.c:141-148`. It initializes Neptune at lines 186-240. It routes contexts, commands, fences, and resources at lines 243-440.

The Neptune renderer publishes its wire version at `triton-virglrenderer/src/neptune/npt_renderer.c:25-35`. It routes context, command, fence, and resource operations at lines 98-223.

The host context owns rings, resources, events, protocol objects, and teardown. See `triton-virglrenderer/src/neptune/npt_context.c:340-517`. Command and fence dispatch is at lines 740-863. Resource operations are at lines 865-1079.

On macOS, the worker selects the arm64 DXMT slice when `NPT_BACKEND=dxmt`. See `triton-virglrenderer/server/render_worker.c:508-525`.

The Neptune loader selects `libdxmt-native.dylib`, calls `dlopen`, and resolves `D3D11CreateDevice`. See `triton-virglrenderer/src/neptune/npt_library.c:16-70` and lines 127-158.

The top-level override calls the resolved backend entry point at `triton-virglrenderer/src/neptune/npt_overrides_toplevel.c:74-98`.

The native export list publishes D3D11, DXGI, and event symbols. See `triton-dxmt/src/dxmt-native/dxmt_native.exports:1-30`. DXMT implements `D3D11CreateDevice` at `triton-dxmt/src/d3d11/d3d11.cpp:115-217`. It creates the Metal-backed device at lines 100-108 and `triton-dxmt/src/dxmt/dxmt_device.cpp:28-83`.

## End-to-end lifecycle

### Adapter creation

The Windows loader resolves `OpenAdapter` from the UMD definition file. `OpenAdapter` allocates `TRITON9_ADAPTER` and copies the runtime adapter callbacks. It publishes `GetCaps`, `CreateDevice`, and `CloseAdapter`. See `triton-umd/src/virtio/neptune/vista-d3d9/triton9_ddi.c:1058-1086`.

The UMD does not query the miniport during loader-time enumeration. It defers the private ABI query to the first host operation. See `triton9_ddi.c:190-244` and lines 699-740.

The miniport starts at `DriverEntry`. It registers the Vista SP1 WDDM 1.0 table with `DxgkInitialize`. See `triton-kmd/viogpu/viogpu3d/driver.cpp:85-210`.

`VioGpu3DQueryAdapterInfo` reaches the adapter implementation. `VioGpuAdapter::QueryAdapterInfo` publishes the V2 private ABI, 3D, shared memory, Neptune capset, and render-event feature. See `triton-kmd/viogpu/viogpu3d/viogpu_adapter.cpp:678-762`.

### Device creation

`triton9CreateDevice` copies the runtime device callbacks and initializes three locks. It first installs exact-signature fallbacks. It then installs all implemented D3D9 callbacks. See `triton9_ddi.c:955-1045`.

The UMD delays the host proxy until the first real operation. `triton9EnsureHostDevice` verifies the private KMD contract, binds runtime callbacks, and creates embedded D3D11 wrappers. See `triton9_ddi.c:699-805`.

The binding requires query, escape, render, context, allocation, deallocation, lock, and unlock callbacks. See `triton-umd/src/virtio/neptune/npt_renderer_virtgpu_win32.c:120-213`.

The renderer verifies the KMD V2 ABI and Neptune wire version. It performs `CTX_INIT` and creates the runtime context. See `npt_renderer_virtgpu_win32.c:1250-1369`.

`triton9EnsureRuntimeContext` creates a kernel context and requests capset 7. See `triton9_ddi.c:658-680` and lines 808-851.

The miniport wrappers create the device and context in `driver.cpp:1161-1207` and lines 1229-1277. Their implementation objects own the WDDM context and command buffers.

### Resource allocation

`triton9CreateResource` accepts one surface, one explicit mip level, no multisampling, and no cube or volume shape. It creates CPU shadow state first. See `triton9_resource.c:915-1071`.

The UMD allocates a standard primary immediately through `triton9AllocateStandardPrimary`. See `triton9_resource.c:458-523` and lines 1072-1080.

Other host objects are lazy. `triton9EnsureResourceHost` creates a D3D11 buffer, local texture, or exportable texture. See `triton9_resource.c:231-278`.

An exported present or shared texture uses `triton9RegisterSharedTexture`. That symbol exports the texture and calls `pfnAllocateCb`. See `triton9_resource.c:353-455`.

The KMD wrapper is `VioGpu3DCreateAllocation` in `triton-kmd/viogpu/viogpu3d/driver.cpp:717-737`. `VioGpuAllocation::DxgkCreateAllocation` verifies allocation types and segment rules at `viogpu_allocation.cpp:1119-1455`.

The KMD supports 3D, blob, import, and shared private types at `viogpu_allocation.cpp:1186-1278`. It publishes a primary only after all allocations succeed at lines 1430-1438.

The host shared-texture bridge creates one exportable texture at `triton-umd/src/virtio/neptune/npt_shared_texture.c:123-156`. Host export and import are in `triton-virglrenderer/src/neptune/npt_shared.c:92-235` and lines 238-290.

### Clear path

`triton9SetRenderTarget` supports only target zero and binds an `ID3D11RenderTargetView`. See `triton-umd/src/virtio/neptune/vista-d3d9/triton9_output.c:170-217`.

`triton9SetDepthStencil` binds D16 or D24S8 through a D3D11 depth view. See `triton9_output.c:219-260` and `triton9_resource.c:305-327`.

`triton9Clear` verifies flags and rectangles, then calls the D3D11 clear methods. See `triton9_output.c:262-376`. Rectangular stencil clear is unavailable. Rectangular depth clear only supports D16 at lines 296-302.

The generated Neptune wrappers serialize the D3D11 calls. The UMD later uses `tritonSharedBridgeDrain` for ordered CPU or present boundaries. See `triton-umd/src/virtio/neptune/triton/tritonSharedBridge.c:30-49`.

### Draw path

State callbacks preserve or translate D3D9 state in `triton9_state.cpp`. `triton9PrepareDraw` creates and binds the host pipeline at `triton9_shader.cpp:2227-2310`.

The translator supports point, line, triangle-list, and triangle-strip topology. It rejects triangle fans. See `triton9_shader.cpp:2098-2143`.

The draw callbacks are `triton9DrawPrimitive`, `triton9DrawIndexedPrimitive`, `triton9DrawPrimitive2`, and `triton9DrawIndexedPrimitive2`. See `triton9_shader.cpp:3357-3667`.

The UMD sends generated protocol packets through `pfnRenderCb`. `virtgpu_render` builds that runtime callback at `npt_renderer_virtgpu_win32.c:258-340`.

The KMD translates the UMD private command stream in `VioGpuDevice::Render`. See `triton-kmd/viogpu/viogpu3d/viogpu_device.cpp:1100-1365`.

`VioGpuCommander` dispatches the translated command asynchronously. It signals the terminal event and retires the WDDM fence in `viogpu_command.cpp:162-499` and lines 576-629.

QEMU submits the result to virglrenderer at `virtio-gpu-virgl.c:685-722`. The proxy and Neptune server then dispatch the generated D3D11 call.

### Readback and lock

`triton9ReadbackShadow` copies a host resource to a staging resource. See `triton9_resource.c:889-913`.

`triton9MapStagingForRead` flushes the host context and waits for an ordered Neptune marker. It maps only after that marker retires. See `triton9_resource.c:781-810`.

`triton9CopyStagingSurfaceToShadow` copies staging rows into the runtime-owned shadow. See `triton9_resource.c:812-866`.

`triton9Blt` completes video-memory to system-memory copies before return. See `triton9_output.c:380-510`. It supports only equal-sized, same-format copies without flags.

`triton9Lock` refreshes the shadow before a read or partial write. See `triton9_resource.c:1204-1290`. `triton9Unlock` uploads modified data at lines 1293-1325.

The public probe exercises this route through `GetRenderTargetData` and `LockRect`. See `triton9_runtime_probe.c:930-954`.

### Present path

`triton9Present` materializes the source, drains host work, creates the KMD context, and calls `pfnPresentCb`. See `triton9_resource.c:1568-1638`.

The miniport wrapper calls `VioGpuDevice::Present`. See `triton-kmd/viogpu/viogpu3d/driver.cpp:1299-1329`.

`VioGpuDevice::Present` accepts an exact flip or exact blit form. The flip route is at `viogpu_device.cpp:863-915`. The blit route is at lines 929-1078.

The blit route can issue the direct host scanout command at `viogpu_device.cpp:1040-1067`. Other flag combinations return unsupported at lines 1081-1097.

`VioGpuCommander::Patch` and `SubmitCommand` attach physical allocation state and queue the DMA command. See `viogpu_command.cpp:847-980`.

QEMU handles Neptune scanout at `virtio-gpu-virgl.c:578-683`. Resource flush reads host bytes into the QEMU display surface at lines 478-575.

### Destruction

`triton9DestroyResource` unbinds all references, releases imports, calls the KMD deallocation callback, and releases host objects. See `triton9_resource.c:1122-1200`.

`triton9DeallocateResource` distinguishes shared-resource groups from ordinary allocations. See `triton9_resource.c:525-549`.

The KMD closes and destroys allocations in `driver.cpp:761-877`. `VioGpuAllocation` uses reference counts for asynchronous users.

`triton9DestroyDevice` destroys queries and the KMD context, then releases all host state. See `triton9_ddi.c:917-951`.

The KMD device and context destroy wrappers are at `driver.cpp:1211-1225` and lines 1282-1295.

`triton9CloseAdapter` frees the UMD adapter at `triton9_ddi.c:1048-1055`. Host Neptune context teardown is at `npt_context.c:455-517`.

## D3D9 contract inventory

### Caps

`triton9FillCaps` is the single D3DCAPS9 authority. See `triton-umd/src/virtio/neptune/vista-d3d9/triton9_ddi.c:288-530`.

The main advertised groups are:

- HAL, adapter zero, read-scanline, windowed rendering, shared resources, managed resources, and dynamic textures. See `triton9_ddi.c:318-331`.
- Immediate and one-vblank presentation intervals. See `triton9_ddi.c:332-333`.
- Hardware transform, hardware rasterization, pure devices, and legacy float transformed vertices. See `triton9_ddi.c:334-359`.
- Multiple-render-target, fog, dither, stipple, and depth-bias bits. See `triton9_ddi.c:373-399`.
- Cube maps, mipmaps, point and linear filtering, and all address modes. See `triton9_ddi.c:408-440`.
- A maximum texture size of 4096, a maximum volume extent of 256, and maximum anisotropy of 16. See `triton9_ddi.c:438-449`.
- Fourteen fixed texture operations and eight blend stages. See `triton9_ddi.c:451-469`.
- Lighting, tweening, four vertex-blend matrices, and eight active lights. See `triton9_ddi.c:470-483`.
- Shader Model 2.0 for vertex and pixel shaders. The Shader Model 3.0 slot counts are zero. See `triton9_ddi.c:489-529`.
- Four simultaneous render targets. See `triton9_ddi.c:503-509`.

`triton9GetCaps` exposes format count and data, the D3D8-compatible prefix, full D3D9 caps, and multisample quality zero. It exposes event and occlusion queries. It reports zero decode, video, extension, gamma, and content-protection capabilities. See `triton9_ddi.c:532-655`.

### Formats

`triton9_format.c` contains five public entries. The table and exported operations are at lines 13-50 and 74-94.

| D3D9 format | Host format | Intended current use |
|---|---|---|
| `A8R8G8B8` | `B8G8R8A8_UNORM` | Texture, render target, shared color |
| `X8R8G8B8` | `B8G8R8X8_UNORM` | Display mode, color, primary |
| `A8` | `A8_UNORM` | Alpha texture |
| `D16` | `D16_UNORM` | Depth |
| `D24S8` | `D24_UNORM_S8_UINT` | Vista DWM depth and stencil |

The operation mask advertises more than these implemented uses. The contradiction inventory gives each difference and its failure mode.

### Callbacks

The adapter callback table contains `GetCaps`, `CreateDevice`, and `CloseAdapter`. See `triton9_ddi.c:1079-1085`.

The implemented device table is exact at `triton9_ddi.c:983-1042`. It includes these groups:

- Resource create, destroy, open, lock, unlock, rename, residency, and display mode.
- Present, flush, render-target, depth, clear, copy, color-fill, and mip generation.
- Vertex declaration, vertex shader, pixel shader, constants, streams, indices, and four draw forms.
- Render state, transform, texture-stage state, texture, viewport, depth range, and scissor.
- Event and occlusion query create, issue, data, and destroy.

`triton9_unsupported.c` installs exact-signature fallback callbacks at lines 167-191. This avoids x86 stack corruption.

These callbacks are validated state-only or bookkeeping no-ops:

- `triton9UpdateWInfo`, lines 19-34.
- `triton9StateSet`, lines 43-60.
- `triton9SetPriority`, lines 61-75.
- `triton9SetMaterial`, lines 81-99.
- `triton9SetClipPlane`, lines 107-122. Public clip-plane capacity remains zero.
- `triton9GetInfo`, lines 124-155. Only the vertex-cache query succeeds.

These callbacks are explicit error stubs. Each returns `D3DDDIERR_NOTAVAILABLE` through the macro at `triton9_unsupported.c:12-17`:

- `triton9DrawRectPatch` and `triton9DrawTriPatch`, lines 35-40.
- `triton9VolBlt`, lines 41-42.
- `triton9UpdatePalette` and `triton9SetPalette`, lines 76-80.
- `triton9SetLight`, `triton9CreateLight`, and `triton9DestroyLight`, lines 100-106.
- `triton9SetConvolutionKernelMono`, `triton9ComposeRects`, and `triton9DepthFill`, lines 156-161.
- `triton9GetCaptureAllocationHandle` and `triton9CaptureToSysMem`, lines 162-165.

Decode, video-process, and overlay callbacks remain null. The public counts for these optional families are zero. See `triton9.h:421-426` and `triton9_ddi.c:638-644`.

The miniport registers the complete Vista SP1 baseline table at `triton-kmd/viogpu/viogpu3d/driver.cpp:112-200`. Several registered slots are explicit stubs:

- A nonzero private display-format attribute returns `STATUS_NOT_SUPPORTED` at `driver.cpp:303-343`.
- Palette, capture-stop, and overlay operations return `STATUS_NOT_SUPPORTED` at `driver.cpp:348-428`.
- ACPI notification and interface queries return `STATUS_NOT_SUPPORTED` at `driver.cpp:431-469`.
- An unhandled paging-buffer operation returns `STATUS_NOT_SUPPORTED` at `driver.cpp:1076-1082`.
- Swizzling-range acquire and release return `STATUS_NOT_SUPPORTED` at `driver.cpp:1086-1109`.
- Pointer-shape requests return `STATUS_NOT_IMPLEMENTED` at `driver.cpp:643-692`.

The header states that the driver exposes no palette, swizzling, capture, or overlay capability. See `triton-kmd/viogpu/viogpu3d/driver.h:112-142`. Thus, those optional stubs do not contradict a positive miniport cap. C31 records their observable limits.

The paging path also has one success fallback. It accepts physical read and write operations without moving data. See `driver.cpp:1050-1061`. C32 records this assumption.

### PresentEx and DWM-facing contracts

The UMD declares the Vista interface in `OpenAdapter`. See `triton9_ddi.c:1083-1085`. Its present callback reaches WDDM through `pfnPresentCb` at `triton9_resource.c:1630-1638`.

The primary and the rendered DWM source are different objects. The primary is a non-blob KMD scanout allocation. The rendered source is an exported host texture. See `triton9_resource.c:458-462` and lines 976-994.

The present boundary first drains generated host work. `tritonSharedBridgeDrain` waits for ring consumption and submits an ordered empty command. See `tritonSharedBridge.c:30-49`.

The miniport accepts only exact flip and blit forms. See `viogpu_device.cpp:863-915` and lines 929-1097. This is an unverified DWM-facing assumption. C30 records its failure mode.

The public probe creates a base hardware device and then a hardware-plus-pure device. See `triton9_runtime_probe.c:1880-1907`. A final pass requires the pure profile at lines 2510-2515.

The probe verifies both device-level `PresentEx` and the additional swap-chain present used by MIL. See `triton9_runtime_probe.c:2462-2508`.

The controlled Aero window only creates a measurement pattern. Its API result remains visual-unverified. See `triton9_runtime_probe.c:780-825`.

The PNG verifier requires three independent facts. It requires a backdrop signal, lower contrast through the plate, and smoother edges. See `scripts/verify_aero_glass_pixels.py:95-177`.

## Contract gaps and observable effects

Each entry states whether the source proves the gap or only supports an inference. Each entry also states the expected visible or API effect.

### C1 — Format operation mask exceeds resource and pipeline support

**Fact, contradiction.** `TRITON9_SURFACE_OPS` advertises volume textures, cube textures, conversion, sRGB read and write, and vertex textures. See `triton9_format.c:13-19`.

`triton9CreateResource` rejects volume and cube flags at `triton9_resource.c:949-957`. The state path rejects enabled sRGB at `triton9_state.cpp:655-662` and lines 1135-1137. Texture resources bind only to the pixel stage at lines 1254-1303. `triton9Blt` requires the same host format at `triton9_output.c:404-419`.

**Observable failure:** A capability-guided cube, volume, sRGB, vertex-texture, or conversion request can return unavailable. A vertex-texture request can also lack a vertex-stage binding.

### C2 — Cube and mip caps exceed creation support

**Fact, contradiction.** `TextureCaps` advertises cube maps and mipmaps. The caps also publish cube and volume filters and `MaxVolumeExtent`. See `triton9_ddi.c:408-440`.

Creation rejects cube and volume flags. It also rejects an explicit chain with more than one surface or mip. See `triton9_resource.c:949-963`.

**Observable failure:** `CreateCubeTexture`, `CreateVolumeTexture`, or an explicit multi-level texture can fail after caps admission.

### C3 — Stretch filtering exceeds blit support

**Fact, contradiction.** `StretchRectFilterCaps` advertises point and linear filters at `triton9_ddi.c:417-420`.

`triton9Blt` rejects any source and destination size difference at `triton9_output.c:412-419`.

**Observable failure:** A caps-guided `StretchRect` scaling call can return unavailable.

### C4 — Texture operation caps exceed the fixed-function translator

**Fact, contradiction.** `TextureOpCaps` advertises fourteen operations at `triton9_ddi.c:451-464`.

`triton9ValidFixedOperation` accepts only disable, select-argument-one, select-argument-two, and modulate. See `triton9_state.cpp:173-178`.

**Observable failure:** An application can select `MODULATE2X`, `ADD`, `SUBTRACT`, `DOTPRODUCT3`, or `LERP` from caps and receive unavailable.

### C5 — Fixed-function stage count exceeds implementation

**Fact, contradiction.** The caps report eight blend stages and eight simultaneous textures at `triton9_ddi.c:465-469`.

The fixed-function state path permits active operations only on stages zero and one. See `triton9_state.cpp:1033-1044`.

**Observable failure:** A fixed-function blend operation on stage two or later can fail. This claim does not limit programmable pixel-shader texture slots.

### C6 — Anisotropy cap exceeds sampler support

**Fact, cap-field contradiction.** The caps report `MaxAnisotropy = 16` at `triton9_ddi.c:444`. The texture-filter mask does not advertise anisotropic filtering. See lines 303-306.

Sampler creation requires `MAXANISOTROPY == 1`. See `triton9_state.cpp:437-475`. The state setter also preserves only one at lines 1126-1128.

**Observable failure:** A consumer that trusts the maximum field can set a value above one and receive unavailable before a draw. A consumer that also reads the filter mask will not select anisotropic filtering.

### C7 — Lighting caps have no light-object implementation

**Fact, contradiction.** The caps advertise directional lights, positional lights, local viewer, and eight active lights. See `triton9_ddi.c:470-483`.

The light create, set, and destroy callbacks are error stubs at `triton9_unsupported.c:100-106`. The fixed-function shader path rejects enabled lighting at `triton9_shader.cpp:1986-2002`.

The default state enables lighting in `triton9_state.cpp:815-887`. Applications must disable it before the supported unlit path.

**Observable failure:** Light creation fails. A fixed-function draw with lighting enabled also fails before host drawing.

### C8 — Vertex blend and tween caps exceed state support

**Fact, contradiction.** The caps advertise tweening and four blend matrices at `triton9_ddi.c:470-483`.

Tween factor is preservation-only at `triton9_state.cpp:599-609`. Indexed vertex blending only accepts false at lines 675-681. Vertex blend only accepts disabled at lines 748-751.

**Observable failure:** A caps-guided vertex blend or tween request can fail. A preserved tween value does not change rendering.

### C9 — Raster caps exceed enabled-state support

**Fact, contradiction.** Raster caps advertise dither, vertex fog, stipple, depth bias, and slope-scale depth bias. See `triton9_ddi.c:386-399`.

Enabled fog and dither return unavailable at `triton9_state.cpp:655-662`. Stipple is preservation-only at lines 563-594. Nonzero depth bias fails at lines 786-788 and in rasterizer creation at lines 410-412.

**Observable failure:** An application can select an advertised raster feature and receive unavailable. A preserved stipple pattern has no raster effect.

### C10 — Multiple-render-target caps exceed output support

**Fact, contradiction.** The caps report four simultaneous render targets and MRT bits. See `triton9_ddi.c:373-385` and line 509.

`triton9SetRenderTarget` rejects every index other than zero at `triton9_output.c:170-180`.

**Observable failure:** `SetRenderTarget` for slot one, two, or three fails after caps admission.

### C11 — UMD cursor caps do not match KMD pointer caps

**Fact, cross-layer contradiction.** D3D9 caps advertise color and low-resolution cursors at `triton9_ddi.c:331`.

The KMD does not advertise hardware pointer support at `viogpu_adapter.cpp:854-860`. Position disable calls succeed, but pointer-shape calls return not implemented at `driver.cpp:643-692`.

**Observable failure:** A cursor-cap consumer can select a hardware-oriented path that the miniport cannot provide. The D3D9 runtime can mask this with software cursors.

### C12 — Full-screen gamma bit conflicts with zero gamma caps

**Fact, contradiction.** `Caps2` includes `D3DCAPS2_FULLSCREENGAMMA` at `triton9_ddi.c:323`.

`D3DDDICAPS_GETGAMMARAMPCAPS` returns a zero structure at `triton9_ddi.c:645-651`.

**Observable failure:** A client can see full-screen gamma support but receive no gamma-ramp capability from the DDI query.

### C13 — Pure-device source comment contradicts the published bit

**Fact, source contradiction.** The comment says that Triton does not claim a pure device at `triton9_ddi.c:334-338`. The actual `DevCaps` includes `D3DDEVCAPS_PUREDEVICE` at lines 351-356.

The public probe requires the pure profile at `triton9_runtime_probe.c:1891-1907` and lines 2510-2515. The implementation, not the stale comment, controls runtime behavior.

**Observable failure:** The stale comment can cause a later caps edit or audit to remove a required bit. The current probe, not this comment, verifies execution.

### C14 — Scanline and interval caps use a synthetic timing fallback

**Fact, fallback.** The UMD advertises read-scanline and two presentation intervals at `triton9_ddi.c:322-333`.

`VioGpu3DDdiGetScanLine` always reports vblank and scanline zero on Vista. See `triton-kmd/viogpu/viogpu3d/driver.cpp:1573-1587`.

**Observable failure:** Timing, pacing, or statistics based on the reported scanline can be synthetic. A successful `PresentEx` does not verify real vblank semantics.

### C15 — Present-fence handles use a signed integer transport

**Fact, unverified assumption.** `npt_vgw32_submit_present_fence` creates a Win32 event and returns it as `int`. Its own comment identifies the bit-31 hazard. See `npt_renderer_virtgpu_win32.c:989-1017`.

**Observable failure:** A valid event handle with bit 31 set becomes a negative result. The caller can treat it as failure and leak or stall the present wait.

### C16 — Present-fence pairing assumes one caller thread

**Fact, unverified assumption.** The KMD stamps the token against the current thread. Its comment requires the UMD arm and present on that thread. See `viogpu_adapter.cpp:1384-1398`.

The flip path consumes the token by current thread at `viogpu_device.cpp:891-903`.

**Observable failure:** An interleaved or thread-switched present can consume no token or the wrong token. Scanout can occur early or wait for the wrong frame.

### C17 — Runtime-DDI drain treats a reset wake as completion

**Fact, fallback and unverified assumption.** The general drain verifies device execution state after its event. See `npt_renderer_virtgpu_win32.c:343-407`.

The runtime-DDI route returns true immediately after the event because Vista exposes no device-state callback there. See lines 383-394.

**Observable failure:** A reset or removal wake can look like successful retirement at this boundary. The next runtime callback must detect the lost device.

### C18 — Shared-resource attach uses a bounded polling race

**Fact, fallback.** The host notes that attach commands and ring commands lack ordering. It polls for at most 1000 ms. See `triton-virglrenderer/src/neptune/npt_shared.c:83-90` and lines 255-269.

**Observable failure:** A valid but delayed attach can make `OPEN_RES` return invalid argument. A missing attach fails in one second instead of wedging the ring.

### C19 — The active x64 INF is not the canonical x64 INX

**Fact, build-source contradiction.** The active stage batch selects `test-artifacts/vista-driver-x64-kd-serialtrace/viogpu3d-diagnostic.inf`. See `windows11_stage_vista_pnp_package.bat:14-19`.

The active INF copies the service, starts it automatically, and registers SafeBoot. See `viogpu3d-diagnostic.inf:16-29`, lines 46-57, and lines 65-81.

The canonical x64 INX registers the KMD and UMDs without this service. See `triton-kmd/viogpu/viogpu3d/viogpu3d_vista_x64.inx:15-26` and lines 43-63.

The source manifest hashes the active diagnostic INF at `scripts/build_deploy_vista_driver.sh:1386-1397`. This protects the current build but does not remove the split authority.

**Observable failure:** A change to only the canonical x64 INX does not change the active package. Reviewers can approve one registration contract while the build ships another.

### C20 — The published direct-QEMU procedure violates the current evidence contract

**Fact, workflow contradiction.** `triton-kmd/viogpu/BUILDING_VISTA.md:91-110` opens the base qcow2 directly and states that it writes the image. Line 112 accepts only `DwmIsCompositionEnabled`.

The current guest-owned launcher states that it sends no input at `scripts/run_vista_guest_deploy.sh:4-6`. It mounts immutable optical media at lines 44-66.

**Observable failure:** Following the old procedure mutates the base disk. It can also report composition without public D3D9 proof or glass pixels.

### C21 — Legacy host-input tools remain in the repository

**Fact, out-of-contract artifact.** `scripts/qmpinput.py:1-7` describes QMP input. It sends input at lines 33-42 and requests a screendump at lines 61-73.

`scripts/vmkey.py:10-16` sends HMP commands. It sends keys and screendumps at lines 28-38.

**Observable failure:** Use of either tool can cross a guest input boundary. It can make an interactive desktop appear to be autonomous guest evidence.

### C22 — Legacy milcore replacement can bypass driver admission

**Fact, prohibited Windows-component artifact.** `test-artifacts/aero_install.bat:3-12` copies a patched `milcore` and forces DWM registry values. `aero_restore.bat:1-6` restores the original file.

**Observable failure:** A rejected Triton driver can appear to enable composition after a Windows component bypass. Such a result cannot prove a correct driver contract.

### C23 — The legacy network artifact server is not the current control plane

**Fact, out-of-contract artifact.** `scripts/vista_artifact_server.py:1-7` serves artifacts over the guest network. Lines 37-84 write host-side completion and probe result files.

The current guest deployment document assigns the complete lifecycle to `TritonVistaDeploy`. See `test-artifacts/VISTA_GUEST_DEPLOY.md:1-25`.

**Observable failure:** A workflow can accept an old network marker that is not tied to the immutable media identifier or current guest service state.

### C24 — Explicit callback stubs remain part of the regular D3D9 table

**Fact, stub surface.** The exact stub list appears in the Callbacks section. The common macro returns `D3DDDIERR_NOTAVAILABLE` at `triton9_unsupported.c:12-17`.

**Observable failure:** Patch draws, volume blits, palettes, lights, convolution, compose-rects, depth-fill, and capture calls fail deterministically.

This result is safer than silent success. It is still incomplete if caps or a DWM path selects one of these callbacks.

### C25 — Rectangular depth and stencil clear support is partial

**Fact, incomplete callback.** `triton9Clear` rejects every rectangular stencil clear. It accepts rectangular depth only for D16. See `triton9_output.c:296-302`.

Full-surface D24S8 depth and stencil clears use the D3D11 depth view at lines 344-365.

**Observable failure:** A rectangular D24S8 depth clear or any rectangular stencil clear returns unavailable. Full-surface D24S8 clear remains supported.

### C26 — The guest deployment document does not match the active service

**Fact, documentation contradiction.** `test-artifacts/VISTA_GUEST_DEPLOY.md:9-14` says that every update enters Safe Mode and uses SetupAPI. Lines 21-24 name one fixed `TritonD3D9ProbeV2` service.

The active service schedules direct payload replacement at `test-artifacts/vista-driver-deploy-service.c:614-710`. Its normal path invokes that route at lines 992-1035. It generates `TritonD3D9Probe_<id>` at lines 508-533.

The source audit rejects SetupAPI entry points in this service. See `scripts/build_deploy_vista_driver.sh:2040-2043`.

**Observable failure:** An operator can wait for an obsolete Safe Mode phase or fixed service name. The operator can then miss the current normal-mode result and deployment-specific probe service.

### C27 — Autogenerated mip support is not in the public format contract

**Fact, incomplete format contract.** No entry includes `FORMATOP_AUTOGENMIPMAP` in `triton9_format.c:13-49`.

The resource path creates a hidden mip chain for `AutogenMipmap` at `triton9_resource.c:1054-1062`. `triton9GenerateMipSubLevels` generates it only with a linear filter. See `triton9_output.c:810-860`.

**Observable failure:** `CheckDeviceFormat` can reject autogenerated mips. The runtime can omit a working implementation path because the format table does not expose it.

### C28 — Depth formats advertise texture sampling that the UMD rejects

**Fact, format contradiction.** D16 and D24S8 include `FORMATOP_TEXTURE` in `triton9_format.c:25-27` and lines 44-49.

Creation requires these formats to use the `ZBuffer` flag at `triton9_resource.c:1040-1045`. Host creation gives them only a depth-stencil bind at lines 202-210. `triton9GetShaderResourceView` rejects both formats at lines 329-339.

**Observable failure:** A client can select an advertised sampleable depth texture. Creation without `ZBuffer` fails, and a depth resource cannot bind for shader sampling.

### C29 — Texgen caps exceed fixed-function coordinate support

**Fact, contradiction.** `VertexProcessingCaps` includes `D3DVTXPCAPS_TEXGEN` and `D3DVTXPCAPS_TEXGEN_SPHEREMAP`. See `triton9_ddi.c:470-476`.

The texture-stage callback accepts only the identity coordinate index. It also accepts only disabled texture transforms. See `triton9_state.cpp:1046-1057`.

The fixed pixel translator requires a declared texture coordinate for each used stage. See `triton9_shader.cpp:1755-1766`.

**Observable failure:** Camera-space texgen, reflection texgen, sphere-map texgen, or a texture transform can return unavailable. A fixed draw cannot synthesize the advertised coordinate.

### C30 — Miniport Present accepts only two exact flag values

**Fact, unverified DWM-facing assumption.** A flip must have `Flags.Value == 0x4` at `viogpu_device.cpp:863-868`. A blit must have `Flags.Value == 0x1` and nonempty rectangles at lines 929-934.

The function rejects color fill, color keys, linear-to-sRGB, rotation, and flip-with-no-wait at lines 1081-1097.

**Observable failure:** A legal runtime or DWM Present with any extra flag returns `STATUS_NOT_SUPPORTED`. The public `PresentEx` probe verifies its selected forms, not every Vista DWM flag combination.

### C31 — Baseline miniport slots contain explicit stubs

**Fact, stub surface.** `DriverEntry` registers the full baseline table at `driver.cpp:112-200`. The Callbacks section maps each private-format, palette, capture, overlay, ACPI, query-interface, swizzling, and pointer stub to its exact range.

`driver.h:112-142` states that the optional palette, swizzling, capture, and overlay capabilities are absent. These optional stubs are incomplete slots, not positive-cap contradictions.

**Observable failure:** A caller that requests one of these absent functions receives `STATUS_NOT_SUPPORTED` or `STATUS_NOT_IMPLEMENTED`. Pointer shape falls back to a software cursor because the KMD pointer caps are zero.

### C32 — Physical paging diagnostics report success without work

**Fact, fallback and unverified assumption.** `VioGpu3DBuildPagingBuffer` returns success for `DXGK_OPERATION_READ_PHYSICAL` and `DXGK_OPERATION_WRITE_PHYSICAL`. Its comment states that no virtio-gpu equivalent exists. See `driver.cpp:1050-1061`.

**Observable failure:** A scheduler diagnostic can observe success although no physical page data moved. A caller that depends on returned data can use stale bytes.

### C33 — The package orchestrator retains an unreachable host-driven tail

**Fact, workflow contradiction.** Public modes reject deployment and set `deploy_package` to zero at `scripts/build_deploy_vista_driver.sh:64-87`. The package path exits before deployment at lines 2154-2162.

The dormant code can capture and OCR a PNG at lines 201-229 and 434-449. It can stop and start QEMU at lines 818-965. It sends F8 through HMP at lines 951-958. The tail also calls a stable nonblack frame “gate 4” at lines 2333-2343.

**Observable failure:** If a later edit re-enables this tail, the host can drive the guest and treat nonblack frames as gate evidence. Such output does not meet the guest-owned public-probe contract.

### C34 — Installed UMDs do not match the current build outputs

**Fact, active-artifact split.** The Active UMD artifact provenance section gives the exact guest paths, host paths, and SHA-256 values. Both installed files match `test-artifacts/vista-unified-umd/`. Neither installed file matches its current `build-vista-*-unified` output.

The package orchestrator stages the two current build outputs at `scripts/build_deploy_vista_driver.sh:1467-1492`.

**Observable failure:** A guest probe result describes the older installed UMD pair until the guest-owned service replaces it. A current-source review alone cannot attribute that result to the current UMD binaries.

### C35 — Triangle-fan draws have no conversion path

**Fact, incomplete draw callback.** `triton9PrimitiveTopology` maps point, line, triangle-list, and triangle-strip primitives. It returns `D3DDDIERR_NOTAVAILABLE` for `D3DPT_TRIANGLEFAN`. See `triton9_shader.cpp:2098-2143`.

All four draw callbacks use this shared preparation path. See `triton9_shader.cpp:3357-3667`.

**Observable failure:** A regular D3D9 triangle-fan draw returns unavailable before host drawing. The caps table has no separate bit that tells the application to avoid triangle fans.

### C36 — Fixed-function argument modifiers and temporary results are missing

**Fact, incomplete fixed-function callback.** `triton9ValidFixedArgument` rejects every bit outside `D3DTA_SELECTMASK`. It also accepts only diffuse, specular, current, texture, and texture factor. See `triton9_state.cpp:180-195`.

The stage callback permits only `D3DTA_CURRENT` as `RESULTARG`. See `triton9_state.cpp:1039-1061`.

**Observable failure:** `D3DTA_COMPLEMENT`, `D3DTA_ALPHAREPLICATE`, `D3DTA_TEMP`, or a temporary result can fail in `SetTextureStageState`. No draw reaches the fixed pixel translator after that error.

## Contracts that are internally consistent

These items are not gaps:

- Multisample capability returns zero at `triton9_ddi.c:611-621`. Resource creation rejects multisampling at `triton9_resource.c:944-946`.
- User clip-plane capacity is zero at `triton9_ddi.c:478-483`. Enabled clip planes fail at `triton9_state.cpp:748-749`.
- Shader Model 3.0 slot counts are zero with Shader Model 2.0 versions. See `triton9_ddi.c:489-529`.
- Query caps list only event and occlusion at `triton9_ddi.c:623-636`. `triton9_query.cpp:62-240` implements exactly those types.
- D24S8 has a format entry, a host depth view, and a full-surface clear path. See `triton9_format.c:44-49`, `triton9_resource.c:305-327`, and `triton9_output.c:344-365`.
- Point, line, triangle-list, and triangle-strip topology have mappings at `triton9_shader.cpp:2098-2143`. C35 records the separate triangle-fan gap.
- Decode, video, extension, and overlay families report zero or remain null. See `triton9_ddi.c:638-644` and `triton9.h:421-426`. Their null UMD callbacks are consistent with zero public counts.
- KMD scheduling reports one node and multi-engine awareness. See `viogpu_adapter.cpp:829-845`.

## Architecture boundaries

### VirtualBox behavioral reference

VirtualBox is only a Vista-era WDDM and D3D9 behavioral reference in this source. It does not define the Triton transport or object model.

The source cites two narrow behaviors. The D3D8 caps-prefix comment cites Vista-era VBox at `triton9_ddi.c:568-572`. The standard-primary lifetime comment cites it at `triton9_resource.c:458-462`.

All mapped runtime objects remain Triton objects. They use Triton callbacks, Triton private allocation types, the Triton Neptune protocol, and the Triton host backend.

### Windows component boundary

No Windows component patch is proposed by this report. All corrective work belongs in `viogpu3d.sys`, `neptune_d3d9.dll`, the Triton transport, or their package contracts.

The legacy `aero_install.bat` milcore replacement is explicitly excluded. DWM, milcore, d3d9.dll, dxgkrnl, and other Windows binaries are not corrective targets.

### Evidence boundary

Desktop visibility, a window title, a black or gray frame, and an opaque window do not prove rendering. A DWM API success also does not prove glass.

Only the public probe can establish the required D3D9 precondition. Only the PNG pixel verifier can establish the controlled glass signal after that precondition.

## Final source sweep

### Four-pass review

1. The implementation pass mapped the build, package, service, probe, UMD, miniport, transport, host, and teardown paths.
2. The Vista WDDM and D3D9 expert pass compared public caps and formats with every selected execution path.
3. The defect hunt classified all 291 candidates. It also verified one observable effect for each of the 36 gaps.
4. The free-polish pass added the citation legend, resolved every explicit range, and applied Simplified Technical English to the prose.

### Static verification performed

- `python3 triton-kmd/viogpu/tools/check_vista_kmd_source.py` exited zero. Its decisive output was `Vista KMD source audit passed`.
- `python3 scripts/verify_aero_glass_pixels.py --self-test` exited zero. It accepted the blurred model and rejected the opaque Basic model.
- A citation-bounds pass resolved all 232 explicit file-and-line citations. No range exceeded its source file.
- `shasum -a 256` verified all four host UMD values in the artifact-provenance section.
- VM control: none. VM mutation: none. Source files modified: none by this leaf.

Files searched: 1729

Candidate files after the architecture-term filter: 291

The sweep covered these roots:

- `scripts`
- `test-artifacts`
- `triton-kmd/viogpu`
- `triton-umd/src/virtio/neptune`
- `triton-qemu/hw/display`
- `triton-qemu/include/hw/virtio`
- `triton-qemu/include/standard-headers/linux`
- `triton-virglrenderer/src`
- `triton-virglrenderer/server`
- `triton-dxmt`

The sweep included C, C++, Objective-C, Metal, headers, Python, shell, batch, PowerShell, INF, INX, project, solution, definition, export, resource, Meson, and Markdown files. It excluded build caches and binary artifacts.

The term filter was `triton9|vista|d3d9|presentex|neptune|viogpu3d|dwm|aero|deploy|d3d11createdevice|libdxmt`. The review classified all 291 candidates.

The count used `rg --files` on the listed roots. An extension filter selected the listed source types, and `sort -u` removed duplicate paths. A per-file `rg -qi` call applied the term filter.

The deterministic file list has SHA-256 `a0ea57188f333e1cfadc1c20863a0f70a1375595a3d11d226111e0cf62797c8f`. The filtered candidate list has SHA-256 `b063e773d2198ed1e2ba793207ba558f4e4b55aaca9205eac25c133c6c34f8e8`.

Generated Neptune protocol files remain part of the mapped ABI implementation. Legacy input tools, the network server, old QEMU instructions, and milcore artifacts appear in the gap inventory.

The active KMD route uses the serial-trace project, stage batch, and diagnostic INF that the build inventory names. Older KMD, INF, catalog, signing, checked-KD, and test-signing files are historical diagnostic generations. They are not parallel active package authorities.

The standard `viogpu.sln`, `viogpu3d.inx`, `viogpu3d/BUILDING.md`, and `viogpuap/GpuAdapter.cpp` files belong to the modern sibling route. `viogpu_vista.sln` and the Vista INX files remain the standard Vista route. C19 records the separate active diagnostic route.

Native compatibility tests and the shader-converter sources are build-time verification or translation support. Offline disassembly, shader-scan, corpus, blur, and theme-layout helpers do not run in the guest proof path. This group includes `scripts/aero_layout_window.py`.

Generated protocol sources are part of the ABI path. Third-party ANGLE header matches and binary, image, or log artifacts do not add a Triton driver, probe, or service.

The DXMT build graph classifies its 48 filtered candidates. Native-image, D3D11, DXGI, Metal, and export files form the mapped backend. DX11 tests, bundled DirectX headers, and NVAPI samples are build support or third-party test surfaces.

No other candidate adds a driver, probe, service, build, package, or verification surface.

Omitted relevant surfaces: none
