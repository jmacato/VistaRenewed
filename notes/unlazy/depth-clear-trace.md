# D24S8 clear fault trace

## Scope and decision

This report analyzes one public D3D9 probe. The deployment ID is
`b4bf74b765f6875bdf4542a14d0257f0715b7968afc637d2844bed4dfbeb9fb4`.
The process was PID `000008a4`, and the UMD build marker was `20260816`.
The complete transcript and PID correlation are at
`notes/unlazy/offline-evidence.md:12-18,224-284`.

Gate anchor: PID 000008a4.
Create D24S8 depth/stencil PASS.
Set D24S8 depth/stencil PASS.
Clear D24S8 depth 0x80004005 FAIL.

**Fact.** The proven order is CheckFormat D24S8 PASS, Create D24S8
depth/stencil PASS, Set D24S8 depth/stencil PASS, and target-only Clear PASS.
Clear D24S8 depth then returned `0x80004005`. The exact terminal lines are at
`notes/unlazy/offline-evidence.md:228-241`. The probe source puts these calls
at `triton-umd/src/virtio/neptune/vista-d3d9/tests/triton9_runtime_probe.c:1846-1855,1986-2033`.

**Fact.** No pixel readback, triangle, or `PresentEx` ran. The first failed
public boundary stopped the probe before those stages. See
`notes/unlazy/offline-evidence.md:239-256` and
`triton9_runtime_probe.c:2031-2050`.

**Fact.** This was not a resource-creation failure. The deployed
`triton9SetDepthStencil` symbol calls `triton9EnsureResourceHost` at
`0x277fb17a5`, `triton9GetDepthStencilView` at `0x277fb17cb`, and
`triton9BindOutputs` at `0x277fb17e9`. It returns success only after those
calls succeed. These addresses are in
`test-artifacts/vista-unified-umd/neptune_d3d9.dll:triton9SetDepthStencil@0x277fb1760`.

**Decision.** The failure occurred before the deployed UMD's first depth-clear
proof record. Existing evidence does not distinguish these two boundaries:

1. The Vista runtime did not call `triton9Clear` for the depth request.
2. `triton9Clear` took a return before its first depth proof record.

The second group includes pointer validation, device loss, host-device setup,
flag validation, and rectangular D24S8 rejection. The deployed marker boundary
is proved below. No evidence proves that the depth command reached protocol
encode, host dispatch, the host proxy, or the backend operation.

## Evidence identity and version split

### Exact deployed UMD

**Fact.** The installed native UMD SHA-256 was
`2dbb366ca08f6605cf4cf4d279463930be6ca98c2d789fc82e55918b4d9a35f7`.
The installed WoW64 UMD SHA-256 was
`589337d20788960810ab3a987b99e90efcf16e2b412880fc399d7f5d80df9d36`.
They match the two files under `test-artifacts/vista-unified-umd/`. See
`notes/unlazy/architecture-audit.md:76-91`.

The exact deployed native artifact supplies the following symbols:

| Symbol | Address |
|---|---:|
| `triton9Clear` | `0x277fa7520` |
| `triton9GetDepthStencilView` | `0x277fad3c0` |
| `triton9SetDepthStencil` | `0x277fb1760` |
| `npt_id3d11device_default_GetDeviceRemovedReason` | `0x277ef83c0` |

The symbol source is
`test-artifacts/vista-unified-umd/neptune_d3d9.dll`. A read-only `nm -g`
query supplies these addresses.

**Fact.** The artifact contains these strings:
`PROOF-CLEAR-PID`, `FLAGS`, `RECTS`, `FORMAT`, `HOSTFORMAT`, `BINDS`,
`DSV-HR`, `HOST-HR`, and `RETURN-HR`. It does not contain the current
`CLEAR-ENTER` or `BADFLAGS` strings. A read-only `strings -a` query against
the same artifact supplies this result.

**Fact.** Both scoped guest logs contain five working build/PID pairs for
build `20260816` and PID `000008a4`. They contain no `PROOF-CLEAR` record.
The complete proof-tail facts are at `notes/unlazy/offline-evidence.md:258-284`.
The files were `C:\Windows\Temp\triton9-ddi.log` and
`C:\Windows\Temp\triton9-d3d9-proof.log`.

**Inference.** The missing records are meaningful because proof writes worked
in the same process. Their absence does not prove runtime pre-dispatch. The
deployed UMD has several unmarked returns before its first depth marker.

### Current source is not the deployed source identity

**Fact.** The current x64 and x86 outputs have different hashes. They are
`2047dba5...dc81` and `1f83e22e...2d2`. See
`notes/unlazy/architecture-audit.md:89-95`.

**Fact.** The current UMD repository is at commit
`7432d34c2bc10c602d72b1ad4058cde98549f98c`. Its complete
`src/virtio/neptune/vista-d3d9/` tree is untracked. The host repository is at
`65cc14eb896f121ffc5130ce04815a923a03c41d`, with a modified generated common
types file. DXMT is at `822ae637a39512ddbd8e0bbd7744af514670c9d3`.

**Inference.** Deployed UMD conclusions use artifact disassembly. Current
source supplies the reachable Triton architecture and correction ownership.
The exact deployed host and DXMT source revisions remain unknown.

## Target-only comparison

The public calls are adjacent and use the same pure device. The probe selected
the successful `HWP+PURE` device at `triton9_runtime_probe.c:1891-1905`.

| Field | Target-only call | D24S8 depth call | Evidence and limit |
|---|---|---|---|
| Flags | `D3DCLEAR_TARGET` (`0x1`) | `D3DCLEAR_ZBUFFER` (`0x2`) | Public source at `triton9_runtime_probe.c:2022-2030`. Raw DDI flags were not logged. |
| Rectangles | Count `0`, pointer `NULL` | Count `0`, pointer `NULL` | Same source lines. Raw DDI `NumRect` and pointer were not logged. |
| Color | `0xff112233` | `0x00000000` | Same source lines. Color has no effect on a depth-only clear. |
| Depth | `1.0f` | `1.0f` | Same source lines. |
| Stencil | `0` | `0` | Same source lines. The depth call does not set the stencil flag. |
| Bound resources | A8R8G8B8 target and D24S8 DSV | Same two outputs | Create and Set calls pass at `triton9_runtime_probe.c:1977-2011`. Deployed `triton9SetDepthStencil` proves DSV creation and binding. |
| Formats | Target `0x15`, depth `0x4b` | Target `0x15`, depth `0x4b` | Correlated DDI records at `notes/unlazy/offline-evidence.md:269-277`. |
| UMD operation | `ClearRenderTargetView` or `ClearView` | `ClearDepthStencilView` for zero rectangles | Deployed `triton9Clear@0x277fa77b8-0x277fa7a35` and `0x277fa7621-0x277fa766b`. |
| Status conversion | Public `S_OK` | Public `0x80004005` | Transcript at `notes/unlazy/offline-evidence.md:230-241`. Raw DDI HRESULT was not logged. |

### Vista pure-device rectangle contract

**Fact.** `D3DCLEAR_COMPUTERECTS` is bit `0x8`. When this bit is set, the local
WDK requires the driver to cull rectangles against the viewport. It also
says that only pure devices receive the bit. See
`driver/sdk/microsoft.windows.sdk.cpp/c/Include/10.0.28000.0/um/d3dhal.h:672-676`.

**Fact.** The DDI structure permits target, depth, stencil, and
`D3DCLEAR_COMPUTERECTS`. See
`driver/sdk/microsoft.windows.wdk.x64/c/Include/10.0.28000.0/um/d3dumddi.h:406-413`.
The callback receives `NumRect` and `RECT *` as separate parameters at
`d3dumddi.h:2974-2975`.

**Fact.** The Microsoft callback contract gives four cases. With
`NumRect==0` and COMPUTERECTS set, the driver clears the viewport. With a
positive count, COMPUTERECTS makes the driver clip against viewport and
scissor. See [PFND3DDDI_CLEAR remarks](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dumddi/nc-d3dumddi-pfnd3dddi_clear).

**Falsification of an overclaim.** Public Count `0` does not prove that Vista
creates one viewport rectangle. `NumRect==0` plus COMPUTERECTS is a defined DDI
shape. Therefore, the deployed rectangular rejection is not yet a proved cause.

**Inference.** The likely pure-device DDI flags are `0x9` for target and
`0xA` for depth. Both are valid in the deployed flag range `1..15`. The actual
DDI flags and `NumRect` remain unknown because the deployed entry had no generic
marker.

**Fact.** The probe viewport is `128x128`, equal to both output resources.
See `triton9_runtime_probe.c:1977-1989,2013-2020`. Thus, a viewport clear and a
full-resource clear have the same extent in this probe. This equality does not
remove the general COMPUTERECTS defect.

## Triton architecture: API to backend

This section traces the reachable design. It does not claim that the failed
depth call crossed each boundary.

| Boundary | Exact path |
|---|---|
| Vista public API | `IDirect3DDevice9Ex::Clear` is called at `triton9_runtime_probe.c:2022-2030`. |
| Runtime callback | Vista calls `PFND3DDDI_CLEAR(HANDLE,D3DDDIARG_CLEAR*,UINT,RECT*)`, declared at `d3dumddi.h:2974-2975`. |
| UMD symbol | `pfnClear = triton9Clear` at `triton-umd/src/virtio/neptune/vista-d3d9/triton9_ddi.c:1023-1025`. The deployed symbol is `triton9Clear@0x277fa7520`. |
| UMD resource/view | Current `triton9SetDepthStencil` materializes the host resource, gets a DSV, and binds outputs at `triton9_output.c:219-260`. D24S8 DSV validation and creation are at `triton9_resource.c:305-326`. |
| UMD clear | Current full D24S8 clear gets the DSV and calls `ID3D11DeviceContext1_ClearDepthStencilView` at `triton9_output.c:344-366`. |
| Protocol encode | Target encodes a view handle and four floats at `triton-umd/src/virtio/neptune/neptune-protocol/npt_protocol_guest_id3d11devicecontext.h:5332-5370`. Depth encodes the DSV handle, flags, depth, and stencil at `npt_protocol_guest_id3d11devicecontext.h:5668-5712`. |
| Guest submit | Both generated thunks use async flags `0` at `npt_protocol_guest_id3d11devicecontext.h:5416-5424,5762-5772`. The client defaults call them at `npt_protocol_client_id3d11devicecontext.c:378-400`. |
| Guest transport | Direct or indirect ring submission is selected at `triton-umd/src/virtio/neptune/npt_ring.c:665-673`. A ring fatal returns false at `npt_ring.c:314-343`. The void command path discards that result at `npt_ring.c:832-899`. |
| Runtime-to-KMD transport | The Win32 renderer sends a private command through `pfnRenderCb` at `triton-umd/src/virtio/neptune/npt_renderer_virtgpu_win32.c:258-340,491-524`. Submit failure becomes false at `npt_renderer_virtgpu_win32.c:956-986`. |
| Host dispatch | COM method `43` selects target clear, and method `46` selects depth clear. See `triton-virglrenderer/src/neptune/neptune-protocol/npt_protocol_host_dispatch.h:1667-1678`. Main header dispatch is at `npt_protocol_host_dispatch.h:5447-5470`. |
| Protocol decode | Target decode and dispatch are at `triton-virglrenderer/src/neptune/neptune-protocol/npt_protocol_host_id3d11devicecontext.h:4458-4550`. Depth decode and dispatch are at `npt_protocol_host_id3d11devicecontext.h:4745-4842`. |
| Host proxy | The host replaces guest handles, selects an override, or calls the COM vtable. Depth does this at `npt_protocol_host_id3d11devicecontext.h:4777-4825`. The current override table has no clear override at `triton-virglrenderer/src/neptune/npt_context.c:89-114`. |
| Backend operation | DXMT accepts `ClearDepthStencilView` at `triton-dxmt/src/d3d11/d3d11_context_impl.cpp:804-811,4684-4698`. It records a clear encoder at `triton-dxmt/src/dxmt/dxmt_context.cpp:489-510` and emits the Metal clear pass at `dxmt_context.cpp:1107-1141`. |

### What public target PASS does and does not prove

**Fact.** The target and depth D3D11 methods are void and asynchronous. The
generated guest sends no reply for either method. See the protocol citations
above.

**Fact.** Current `npt_ring_notify` ignores the renderer submit result at
`triton-umd/src/virtio/neptune/npt_ring.c:414-426`. Command submission also
returns void at `npt_ring.c:832-899`.

**Fact.** The deployed proxy's
`npt_id3d11device_default_GetDeviceRemovedReason@0x277ef83c0` submits an async
command and returns zero at `0x277ef844b`. Current generated source has the same
behavior at `npt_protocol_client_id3d11device.c:1105-1113`.

**Inference.** Target-only PASS proves that the public API and UMD returned
success. It does not prove that the host or backend completed the target clear.
A deferred target transport failure can surface at a later synchronized call.

## Deployed `triton9Clear` marker and return map

This map comes from read-only disassembly of the exact deployed native UMD.
The command is:

```sh
/opt/homebrew/opt/llvm/bin/llvm-objdump -d --no-show-raw-insn \
  --symbolize-operands --disassemble-symbols=triton9Clear \
  test-artifacts/vista-unified-umd/neptune_d3d9.dll
```

| Order | Deployed address and condition | Raw return or next boundary |
|---:|---|---|
| 1 | `0x277fa758a-0x277fa759b`: null device, args, or positive count with null rectangles | `E_INVALIDARG` (`0x80070057`) at `0x277fa77a0` |
| 2 | `0x277fa75a1-0x277fa75a9`: `deviceLost` | `D3DDDIERR_DEVICEREMOVED` (`0x88760870`) at `0x277fa78b0` |
| 3 | `0x277fa75af-0x277fa75b9`: `triton9EnsureHostDevice` | Its failing HRESULT returns at `0x277fa76ad` |
| 4 | `0x277fa75bf-0x277fa75c7`: flags outside integer range `1..15` | `D3DDDIERR_INVALIDCALL` (`0x8876086c`) at `0x277fa7a90` |
| 5 | `0x277fa75cd-0x277fa76e4`: positive rectangle count with stencil, missing DSV, or depth format other than D16 (`0x50`) | `D3DDDIERR_NOTAVAILABLE` (`0x8876086a`) at `0x277fa79b5` |
| 6 | `0x277fa76ea-0x277fa7796`: first depth proof block | PID, flags, rectangles, format, host format, and binds are written |
| 7 | `0x277fa75e0-0x277fa7a35`: target validation and target clear | View or rectangle error, then host check |
| 8 | `0x277fa7621-0x277fa766b`: zero-rectangle depth clear | Get DSV, then call `ClearDepthStencilView` |
| 9 | `0x277fa7820-0x277fa792c`: rectangular D16 depth clear | Validate rectangles, then call `ClearView` |
| 10 | `0x277fa7671-0x277fa7aea`: host check and device-error mapping | Success, preserved HRESULT, or device removed |

**Fact.** The first depth proof block follows all five early groups. Zero proof
records narrow the boundary to those groups or no UMD callback. They do not
identify one group.

**Fact.** The public HRESULT is `0x80004005`, but none of the constant early
returns equals that value. The exact raw DDI result and Vista HRESULT conversion
were not logged.

**Inference.** If Vista supplied a positive DDI `NumRect`, the rectangular
D24S8 branch can explain the missing markers. If the runtime converted
`0x8876086a`, the branch can explain public `E_FAIL`. A later boundary can also
supply E_FAIL. Both conditions need direct evidence.

## Reachable failure inventory

The inventory includes every rejection, stub, unsupported branch, transport
failure, backend error, and HRESULT conversion found on this path.

| Layer | Reachable failure | Evidence | Effect on this call |
|---|---|---|---|
| Vista runtime | Pre-dispatch validation or internal failure | The public call returns before any UMD marker. No raw runtime trace exists. | Can return public `E_FAIL`. This remains possible. |
| UMD entry | Null device, args, or inconsistent rectangle pointer | Deployed `triton9Clear@0x277fa758a-0x277fa759b`. | Returns `E_INVALIDARG` before markers. Runtime-owned arguments make this unlikely. |
| UMD state | `deviceLost` | Deployed `triton9Clear@0x277fa75a1-0x277fa75a9`. | Returns device removed before markers. The preceding target clear passed. |
| UMD bootstrap | `triton9EnsureHostDevice` fails | Deployed `triton9Clear@0x277fa75af-0x277fa75b9`. Current cached fast path is `triton9_ddi.c:718-725`. | Can preserve E_FAIL before markers. SetDepth and target clear already crossed this gate. |
| UMD flags | Zero or unsupported bits | Deployed integer range test at `0x277fa75bf-0x277fa75c7`. The current exact mask is at `triton9_output.c:289-295`. | Returns invalid call before markers. Expected `0x2` or `0xA` is valid. |
| UMD rectangle | Rectangular stencil or non-D16 depth | Deployed `0x277fa75cd-0x277fa76e4`. Current source is at `triton9_output.c:296-302`. | Returns unavailable before markers. This is the only depth-format-specific early branch. |
| UMD DSV | Missing host resource, wrong format, wrong bind, or CreateDSV error | `triton9_resource.c:305-326`. | Occurs after deployed depth markers for zero rectangles. SetDepth PASS already called the same DSV path. |
| UMD target | Missing RTV or invalid rectangle | `triton9_output.c:333-343` and `triton9_resource.c:281-303`. | Applies to target branch. Target public PASS falsifies it for the adjacent call. |
| Guest encode | Local allocation failure for an oversized command | `npt_protocol_guest_id3d11devicecontext.h:5395-5413,5741-5759`. | A clear command fits the local buffer, so heap allocation is not expected. |
| Guest ring | Fatal state, indirect upload error, submit error, or notify error | `triton-umd/src/virtio/neptune/npt_ring.c:314-343,414-426,665-673,832-899`. | Void async path swallows the error. It cannot directly return same-call E_FAIL. |
| Win32 transport | Invalid callbacks, bad buffers, `pfnRenderCb` failure, or incomplete replacement buffers | `npt_renderer_virtgpu_win32.c:258-340,491-524`. | Status becomes false at lines 956-971, then async code can lose it. |
| Host header dispatch | Malformed group, interface, or method | `npt_protocol_host_dispatch.h:5431-5470`. | Sets decoder fatal. |
| Host object table | Missing or wrong context/DSV handle | `triton-virglrenderer/src/neptune/npt_context.c:224-267`. Depth lookup is at `npt_protocol_host_id3d11devicecontext.h:4777-4815`. | Sets decoder fatal. |
| Host dispatch | Decode failure or null context | `npt_protocol_host_id3d11devicecontext.h:4757-4813`. | Returns without backend call and poisons the stream. |
| Host backend loader stub | Missing D3D11 library or entry point | `triton-virglrenderer/src/neptune/npt_library.c:16-70,127-158`. The creation override is at `npt_overrides_toplevel.c:74-98`. | Returns E_FAIL during device creation. Persisted `TRITON9-HOST-PROXY success` at `offline-evidence.md:269-277` falsifies it for initialization. |
| Backend clear | Invalid/null view, unsupported view, pipeline failure, or later GPU failure | Current DXMT entry is void at `d3d11_context_impl.cpp:804-811,4684-4698`. Encoder work is at `dxmt_context.cpp:489-510,1107-1141`. | No immediate HRESULT can cross the async D3D11 method. |
| Backend rectangular D24S8 | DXMT `ClearView` rejects any DSV with a stencil plane | `triton-dxmt/src/dxmt/dxmt_command.cpp:133-143`. | Current UMD rejects this shape first. A future direct ClearView would silently do no work. |
| Host health query | Generated proxy always returns S_OK | Deployed `GetDeviceRemovedReason@0x277ef83c0-0x277ef844b`. Current source is at `npt_protocol_client_id3d11device.c:1105-1113`. | Hides deferred host errors from current `triton9CheckHostDevice`. |
| HRESULT conversion | Device error mapping | `triton-umd/src/virtio/neptune/vista-d3d9/triton9.h:435-456`. | Only removed/reset/hung becomes DDI device removed. Other HRESULTs remain unchanged. |
| HRESULT conversion | Win32 render status | `npt_renderer_virtgpu_win32.c:215-225`. | All errors except OOM and invalid argument become `STATUS_UNSUCCESSFUL`. |
| HRESULT conversion | Vista DDI-to-public conversion | No scoped raw DDI return exists. | Exact conversion to public `0x80004005` is unknown. |

There is no clear stub on the selected callback. `triton9_ddi.c:1023-1025`
installs the real UMD symbol. Other unsupported D3D9 callbacks are unrelated
unless a later probe stage selects them.

## Ranked hypotheses

Ranks describe explanatory fit, not proof. Each rank has a decisive check and
separate source ownership.

### Rank 1: positive DDI rectangle count hit the deployed D24S8 rejection

**Fact.** This is the only depth-format-specific return before the first depth
marker. It exactly separates target rectangle support from D24S8 rectangle
rejection. See deployed `triton9Clear@0x277fa75cd-0x277fa76e4`.

**Contradiction.** The public count was zero. The pure-device DDI contract
defines `NumRect==0` plus COMPUTERECTS as a viewport clear. It does not require
Vista to generate a positive rectangle.

**Inference.** This rank remains first because it explains all three unique
signals: target PASS, depth-only failure, and zero depth markers. It is not a
finding until raw DDI arguments show a positive count.

**Falsification check.** In a guest-owned run, write flags, `NumRect`, and the
rectangle pointer at the first instruction of `triton9Clear`. Also write each
early raw HRESULT. `NumRect==0` falsifies this rank for the scoped call.

**Source ownership.** The DDI shape and clipping owner is
`triton9_output.c:263-376`, with viewport and scissor state in
`triton9_state.cpp:1195-1251`. A correction must implement COMPUTERECTS for all
formats and flags. It must not add a probe-specific bypass.

**Regression coverage.** Add direct DDI cases for zero count with and without
COMPUTERECTS, a full rectangle, clipped rectangles, scissor, D16, D24S8 depth,
and D24S8 stencil. Then run the unchanged public clear/readback probe.

### Rank 2: Vista runtime or KMD path returned before UMD depth dispatch

**Fact.** No deployed UMD depth marker exists. No raw entry marker existed in
that artifact. Therefore, no-callback remains observationally equivalent to an
early UMD return.

**Inference.** Runtime pre-dispatch is plausible after the rectangle-contract
contradiction. A deferred failure from the preceding async target command can
also affect a later runtime or KMD boundary.

**Falsification check.** Place one guest-owned marker at the first UMD
instruction. Correlate it with KMD `pfnRenderCb` submit status and ring-fatal
state. An entry marker proves dispatch and falsifies pure runtime pre-dispatch.

**Source ownership.** If the UMD entry is absent, do not patch Windows. First
correct any false Triton capability or status contract in `triton9_ddi.c` and
`triton9_format.c`. If a prior transport error exists, ownership moves to
`triton-umd/src/virtio/neptune/npt_ring.c` and
`npt_renderer_virtgpu_win32.c`.

**Regression coverage.** Add fault injection for `pfnRenderCb`, ring fatal,
and notify failure. The next public D3D9 boundary must report a stable driver
error instead of silent success.

### Rank 3: another deployed UMD early return

**Fact.** Pointer validation, `deviceLost`, host setup, and flags all precede
the marker. Their exact addresses appear in the deployed return map.

**Inference.** These branches are less likely. The runtime owns valid pointers,
the target clear passed immediately before depth, the host proxy was cached,
and expected flags `0x2` or `0xA` are valid.

**Falsification check.** Log a unique raw status before every deployed early
return. A single record identifies or excludes this entire rank.

**Source ownership.** Pointer and flags ownership is `triton9_output.c`.
Device-state ownership is `triton9.h:428-456`. Host setup ownership is
`triton9_ddi.c:699-805`. Change only the first failing owner.

**Regression coverage.** Add negative DDI tests for null arguments, invalid
flags, device removal, and host bootstrap failure. Assert exact raw HRESULTs.

### Rank 4: protocol, host-dispatch, or backend D24S8 failure

**Fact.** These layers are downstream of the deployed depth proof block for a
zero-rectangle clear. Both generated clear calls are asynchronous and void.

**Inference.** This rank cannot explain the same-call public E_FAIL by itself.
It can explain a deferred failure that poisons a later boundary. It also becomes
relevant after a positive UMD entry and successful pre-marker trace.

**Falsification check.** Count guest method `46`, host method `46`, DSV handle
lookup, DXMT entry, and backend completion. Insert a synchronized drain before
readback. The first missing counter identifies the boundary.

**Source ownership.** Encode and guest transport own failures before the host.
Their files are `npt_protocol_guest_id3d11devicecontext.h` and `npt_ring.c`.
Host decode and object ownership are
`npt_protocol_host_id3d11devicecontext.h`, `npt_protocol_host_dispatch.h`, and
`triton-virglrenderer/src/neptune/npt_context.c`. Backend ownership starts at
`triton-dxmt/src/d3d11/d3d11_context_impl.cpp`.

**Regression coverage.** Add wire round-trip tests for target and depth fields,
bad-handle fatal tests, backend entry counters, and D24S8 pixel readback after a
synchronized drain.

### Rank 5: proof sink or artifact mismatch hid a reached depth block

**Fact.** The installed hashes exactly match the disassembled artifact. Five
proof records succeeded in PID `000008a4`. This makes mismatch or total sink
failure unlikely.

**Falsification check.** Recompute both installed hashes through the guest-owned
verifier. Emit adjacent entry and depth records through the same proof sink.
One missing tag with the other present identifies a tag-specific sink fault.

**Source ownership.** The deployment verifier owns identity. The UMD diagnostic
helper owns writes. No renderer source change belongs to this rank.

**Regression coverage.** Make deployment acceptance compare all payload hashes.
Add a proof-sink self-test with exact PID and build correlation.

## D24S8 caps, FORMATOP, and actual behavior

**Fact.** Current Triton advertises D24S8 as
`DXGI_FORMAT_D24_UNORM_S8_UINT`. Its operations are `FORMATOP_TEXTURE`,
`FORMATOP_ZSTENCIL`, and
`FORMATOP_ZSTENCIL_WITH_ARBITRARY_COLOR_DEPTH`. See
`triton-umd/src/virtio/neptune/vista-d3d9/triton9_format.c:25-27,38-50`.
The GetCaps path copies this table at `triton9_format.c:80-94` and
`triton9_ddi.c:532-563`.

**Fact.** Current D24S8 caps also advertise all comparison and stencil
operations at `triton9_ddi.c:281-312,400-433`. The public CheckFormat D24S8
call passed before resource creation.

**Actual behavior.** Creation, host materialization, DSV creation, and output
binding passed. The next D24S8 depth clear returned public `0x80004005`.
Therefore, advertised format support and actual clear behavior are inconsistent
at the first proven clear boundary. The evidence does not make creation fail.

### VirtualBox behavioral reference only

VirtualBox advertises the same three D24S8 FORMATOP bits at
`virtualbox-wddm/src/VBox/Additions/WINNT/Graphics/Video/disp/wddm/gallium/GaWddm.cpp:975-981`.
Its clear callback forwards rectangles, flags, color, depth, and stencil to a
D3D9 backend at `GaDdi.cpp:3149-3177`. Its test separates a target clear from a
rectangular depth clear at `test/d3d9render.cpp:107-121`.

This is only a Vista-era behavioral reference. Triton architecture remains
Vista D3D9 DDI to Neptune D3D11 protocol to host proxy to DXMT. This report does
not propose the VirtualBox architecture.

## Wholesale D24S8 rectangular-clear ownership

The current UMD stores viewport and scissor state, but `triton9Clear` does not
use them. See `triton9_state.cpp:1195-1251` and
`triton9_output.c:263-376`. This is a COMPUTERECTS contract defect independent
of the scoped cause.

The correction has four disjoint execution shapes after clipping:

1. If the result covers the full DSV, use the existing
   `ClearDepthStencilView` path in `triton9_output.c:344-366`. No protocol or
   backend change is required for this shape.
2. If the result is partial D16 depth, use the existing `ClearView` path.
   Current ownership is `triton9_output.c:128-138,296-302,351-365`.
3. If the result is partial D24S8 depth only, reuse the existing Neptune
   `ClearView` wire path. It already encodes the view, value, rectangles, and
   count at `npt_protocol_guest_id3d11devicecontext.h:17561-17618`. The host
   decodes and calls DXMT at
   `npt_protocol_host_id3d11devicecontext.h:13911-14003`. Extend DXMT's clear
   draw to write depth while it preserves stencil. The present DXMT rejection
   is at `triton-dxmt/src/dxmt/dxmt_command.cpp:133-143`.
4. If the result includes a partial D24S8 stencil clear, use a private Neptune
   rectangle-clear operation. Standard D3D11 `ClearView` cannot carry a stencil
   value or stencil-selection flag.

For shape 3, the minimum architecture-consistent ownership is
`triton9_output.c`, `triton-dxmt/src/d3d11/d3d11_context_impl.cpp`, and
`triton-dxmt/src/dxmt/dxmt_command.cpp`. The generated ClearView path already
carries rectangles and needs no wire-format change.

For shape 4, the minimum architecture-consistent ownership is:

- Guest DDI semantics and clipping:
  `triton9_output.c`, `triton9_state.cpp`, and the state fields in `triton9.h`.
- Private Neptune command definition on both sides:
  `triton-umd/src/virtio/neptune/npt_transport_defs.h` and
  `triton-virglrenderer/src/neptune/npt_transport_defs.h`.
- Guest submit and host dispatch:
  `triton-umd/src/virtio/neptune/npt_dispatch.c`,
  `triton-virglrenderer/src/neptune/npt_dispatch.c`, and
  `triton-virglrenderer/src/neptune/npt_context.c`.
- Backend rectangle operation:
  `triton-dxmt/src/d3d11/d3d11_context_impl.cpp`,
  `triton-dxmt/src/dxmt/dxmt_command.cpp`, and `dxmt_command.hpp`.

This private command must carry a DSV identity, depth/stencil flags, depth,
stencil, and the clipped rectangle array. It must preserve the plane that the
flags do not select. It must report transport or device failure through a
synchronized status boundary.

A generated standard COM method is not sufficient for partial D24S8 stencil.
The current `ClearDepthStencilView` method carries no rectangles at
`npt_protocol_guest_id3d11devicecontext.h:5668-5712`. The current `ClearView`
method cannot express a stencil value. A correction that only special-cases the
probe's full viewport is not regression coverage.

## Four-pass review

### Full trace

The trace covers Vista public API, Runtime callback, UMD symbol, Protocol
encode, guest transport, Host dispatch, Protocol decode, Host proxy, and
Backend operation. It marks the last observed boundary separately from the
reachable architecture.

### Expert reread

The pure-device review found the COMPUTERECTS rule and the zero-count viewport
case. This result prevents an unsupported claim that Vista generated a
rectangle. The review also found that SetDepth PASS proves DSV creation and
binding in the exact deployed artifact.

### Defect hunt

The hunt found these contradictions:

- The current UMD stores viewport/scissor state but ignores it during Clear.
- The current UMD advertises D24S8, but rejects every rectangular D24S8 clear.
- The guest async path can discard submit failure.
- The deployed device-health proxy always returns S_OK.
- Target public PASS is not backend completion evidence.
- The current source and deployed UMD do not have the same identity.

Omitted return paths: none found in the deployed `triton9Clear` disassembly or
the reachable current protocol path.

### Free polish and safety

Facts, inferences, and falsification checks use separate labels. Every ranked
hypothesis names Source ownership and Regression coverage.

Probe shortcut: none. The unchanged public probe remains the acceptance test.

Windows patch: none. All correction ownership stays inside Triton.

VM control: none. This analysis used only persisted evidence, source, headers,
and preserved artifacts. It did not start, stop, drive, or mutate a VM, disk,
service, or framebuffer.
