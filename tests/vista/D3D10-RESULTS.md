# D3D10/10.1 validation status

The curated public preview implements a Vista D3D10/10.1 frontend and paired
Neptune host transport. Final installed-build acceptance remains open. Earlier
private development reports certify only their recorded snapshots and are not
release results for this branch.

| Tier | Scope | Evidence required for this candidate |
| --- | --- | --- |
| CPU | Legacy descriptor/function-table conversion; concurrent runtime ownership; shared metadata; map abort/retry; upload bounds; query/capability behavior; shader tokens | `python3 scripts/test_public.py`, retained per-case logs/results |
| Native GPU | Real resource creation, views, resolves, copies, shared import/export, predication, queries and stream output | Matching public backend; explicit device identity; positive pixel and negative fault controls |
| Windows ABI/build | Real x86/x64 WDK callback types, modern D3D11 shared-source compatibility, PE and INF/package selection | Both configured Vista UMD builds and public package validation |
| Installed Vista | Microsoft's D3D10/10.1 runtime, native/WOW64, sharing, presentation and device lifetime | Exact installed binary hashes and live public probe results |
| Application/display | SuperTuxKart, desktop 3DMark06 and Aero behavior | Visibly advancing QEMU-window frames matched to guest production, pacing/stalls/visuals/completion |

Run `tests/vista/run-d3d10-compat.py --cpu` for the CPU subset. `--host` also
executes native GPU and Windows build/ABI checks; it is not a CPU-only command.
See [README.md](README.md) for prerequisites and configurable paths. No private
reviewer snapshots or local investigation artifacts are required inputs.

The public runtime probe uses `--10`, `--10.1`, or both by default. It checks
hardware-device creation, Triton module/adapter identity, two concurrent devices,
destroy-first survivor behavior, data, rendering and recreation. It does not
request WARP or reference fallback. Missing APIs, a missing driver, wrong pixels,
and timeouts are failures.

The presentation probe accepts `--fullscreen`, `--msaa4`, and `--10.1`
independently. Run all API, sample and window/fullscreen combinations with both
architectures on the interactive Vista desktop. Check its pixels and backbuffer
descriptor together with an independent host-window observation. Occlusion or
completion without advancing visible frames is not a successful display test.

Final coverage must include shared-handle lifetime, reset/removal, query timing
under pressure, geometry/stream output, DrawAuto, MRT/sample behavior, stencil,
and modern D3D11 regressions. Supported formats and Vulkan/device capabilities
remain constraints. None of these fixtures establishes universal game
compatibility. Agent review substitutes for local manual review only; it does
not satisfy separate upstream human-testing requirements.
