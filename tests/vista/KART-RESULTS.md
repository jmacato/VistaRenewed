# SuperTuxKart Vista acceptance workload

Status: final installed-build game and display acceptance remains open. A
September 27 development run showed only about **10–12 visibly advancing QEMU
window updates per second**. Earlier guest FPS and Present distributions do not
close that defect and are not performance results for this public branch.

The acceptance workload is **SuperTuxKart at 1280×720 with stable 30 FPS** using
its DirectX 9 renderer. Also validate the desktop 3DMark06. Measure guest
production and visibly advancing actual QEMU-window frames over the same race
and interval, with Vista's display awake and no other GPU tests running. Retain
pacing, stalls, visual correctness, completion and instrumentation interference.
Failed or interrupted runs remain failed. A guest-only assessor pass is only
one part of this gate.

## Pinned, authentic workload

- SuperTuxKart **1.5**, the current stable release, official Windows portable ZIP:
  <https://github.com/supertuxkart/stk-code/releases/tag/1.5>.
- ZIP SHA-256:
  `9df7e2d67e8562127a3bb633030f1bd4ee77fa9e7f0ae110897473025af8acdc`.
  This matches the digest published by the GitHub release API.
- Source revision: `1fb491f507216c5d181ccd85f29ff08eca003827`.
- Official x86 and x64 binaries and matching assets are retained unchanged.
  The upstream [Windows build workflow](https://github.com/supertuxkart/stk-code/blob/1fb491f507216c5d181ccd85f29ff08eca003827/.github/workflows/windows.yml)
  enables `USE_DIRECTX=ON`. The
  [Irrlicht driver](https://github.com/supertuxkart/stk-code/blob/1fb491f507216c5d181ccd85f29ff08eca003827/lib/irrlicht/source/Irrlicht/CD3D9Driver.cpp)
  loads `Direct3DCreate9` and renders through D3D9.
- All selected x86/x64 PE images have subsystem versions no newer than 6.0 and
  import MSVCRT, Vista inbox DLLs, or bundled dependencies. This static check
  is not a substitute for running the loader and game in Vista.

The renderer also needs Microsoft's [June 2010 DirectX runtime](https://www.microsoft.com/en-us/download/details.aspx?id=8109).
Extract `Jun2010_d3dx9_43_x64.cab`, `Jun2010_d3dx9_43_x86.cab`,
`Jun2010_D3DCompiler_43_x64.cab`, and `Jun2010_D3DCompiler_43_x86.cab`
from that redist. Place the extracted DLLs in `directx-runtime/x64` and
`directx-runtime/x86` under the work directory, or pass `--directx-dir DIR`.
Preparation verifies pinned DLL hashes, architectures, assembly exports and
imports, then copies both DLLs beside each game executable. D3DX loads
D3DCompiler dynamically, so the original static import check missed it.
The original game files remain unchanged. A later uninstrumented guest replay completed after the dependency correction;
that completion alone did not satisfy the pacing criteria.

`python3 tests/vista/run-kart-game.py prepare --download` authenticates the ZIP,
checks every selected extracted file against it, compiles both measurement
DLLs with the Vista toolchain, and builds
`test-artifacts/kart/triton-kart-1.5.iso`. Its staged manifest records
all 5,668 official file hashes, both probe hashes, and four DirectX DLL hashes. The image is about 1 GiB.
The preparation command prints the current image SHA-256.

## Real races and measurement

The ISO contains `install.cmd`, which copies into a dedicated `C:\triton-kart`
and refuses to overwrite an existing manifest. Run the batch files on the
**active user desktop**, not a service session:

```text
C:\triton-kart\run-benchmark-x64.cmd
C:\triton-kart\run-race-x64.cmd
C:\triton-kart\run-benchmark-x64-control.cmd
```

The launcher captures the actual executable's version before the workload:

```bat
start /b /wait "" supertuxkart.exe --version > "%KART_CONFIG%\version.log" 2>&1
set "KART_VERSION_EXIT=%ERRORLEVEL%"
> "%KART_CONFIG%\version-exit-code.txt" echo %KART_VERSION_EXIT%
```

The pinned [version handler](https://github.com/supertuxkart/stk-code/blob/1fb491f507216c5d181ccd85f29ff08eca003827/src/main.cpp#L810)
prints through the console logger and exits before either `--no-console-log`
or `--stdout` is processed. The [startup call order](https://github.com/supertuxkart/stk-code/blob/1fb491f507216c5d181ccd85f29ff08eca003827/src/main.cpp#L2215)
puts `--stdout` handling and log-file initialization after that early-exit
handler. Consequently the previous `--version --stdout=version.log` command did
not create the expected file. The corrected command redirects the inherited
stdout/stderr handles, waits for the process, retains its exit code and requires
real version output. An old result missing this evidence remains unassessed;
query the same deployed executable and record if that observation is post-run.
Do not synthesize a version file from the expected version or package name.

Equivalent `x86` files exercise the WOW64 driver. Each run creates a fresh
`C:\triton-kart-results\<mode>-<architecture>-<random>\config-0.10` and prints
`KART_RESULT_DIR`. Copy that entire directory back to the host.

- The official `--benchmark` plays the included Black Forest replay, records
  main-loop frame durations, saves CSVs, and exits itself. Its profiler starts
  at the race's GO phase and stops at the end of the race.
- `--profile-laps=3 --numkarts=8 --track=black_forest --difficulty=2` runs an
  actual eight-kart AI race. The upstream
  [ProfileWorld implementation](https://github.com/supertuxkart/stk-code/blob/1fb491f507216c5d181ccd85f29ff08eca003827/src/modes/profile_world.cpp)
  waits for all karts to finish three laps, prints race statistics, and exits.
- The random seed is `20260926`. Both modes use 720p at 100% render resolution,
  geometry level 2, animated karts and particles, normal 512-pixel textures,
  the DirectX 9 lighting path, no anisotropy, and no VSync/frame-rate cap.
  Audio remains enabled. The isolated profile disables Internet access.
- Upstream ProfileWorld's “Average FPS” counts **physics updates**, so it is
  never accepted as rendering performance.
- A game-local D3D9 measurement DLL forwards to the real Windows System32
  runtime (SysWOW64 when the process is x86). It observes the game's actual
  `IDirect3DDevice9::Present` and `IDirect3DSwapChain9::Present` calls, retaining
  the original arguments, HRESULT and thread last-error state. It counts a
  nested device-to-swapchain call once. It does not implement a renderer or
  alter render state. Existing public vtable slots are patched atomically in
  place; the original objects, table addresses, private runtime entries,
  QueryInterface, AddRef and Release remain intact. The probe and its small
  per-table forwarding records stay pinned for process lifetime. Overlapping
  Present calls on different threads invalidate the capture while the calls
  themselves retain their original concurrency.
- The capture records Vista version, actual 1280×720 backbuffer, HAL device,
  hardware vertex processing, process architecture and WOW64 state, and the
  actual loaded UMD path. Native x64 and native x86 use the system directory's
  `neptune_d3d9.dll`; WOW64 uses `neptune_d3d9_wow.dll` in the WOW64 system
  directory. The probe queries the process and Windows directories, looks up
  only the matching loaded module, and rejects absent modules or foreign paths.
  The v2 parser repeats those checks and binds benchmark process identity to
  the collector's architecture/WOW64 observations. Missing
  identity, failed Present, Reset, malformed or incomplete captures fail.
  The CSV requires a normal-shutdown footer and contiguous frame numbering.
- The API capture spans process rendering. Assessment explicitly excludes its
  first 15 seconds and last 2 seconds. The AI run must contain at least 120
  seconds of remaining rendered cadence, eight-kart startup and normal
  three-lap completion. It must also have agent-reviewed screenshots/video
  showing the actual race with correct geometry, textures and HUD. API calls
  alone do not prove that the host displayed correct pixels.

Assess each three-lap race independently:

```sh
python3 tests/vista/run-kart-game.py assess --mode race --architecture x64 --results /path/to/race-x64
python3 tests/vista/run-kart-game.py assess --mode race --architecture x86 --results /path/to/race-x86
```

The numerical criterion remains average FPS ≥30, 95th-percentile frame time
≤33.334 ms, 99th percentile ≤50 ms, and at most 0.1% of frames over 100 ms.
The AI-race requirement remains **at least 120 seconds** of Present intervals
retained after its 15-second warmup and 2-second tail exclusions.

The official replay is shorter than the old per-run benchmark duration gate.
Its authenticated `benchmark_black_forest.replay` has 1,149 samples ending at
**37.137501 seconds**, with SHA-256
`0049a18428049c02dcfdb6e2944c7ba7196f2a0017f239d0305ed12fb15614df`.
The header `min_time: 73.286743` is metadata, not playback duration. In the pinned
source, [StandardRace::isRaceOver](https://github.com/supertuxkart/stk-code/blob/1fb491f507216c5d181ccd85f29ff08eca003827/src/modes/standard_race.cpp#L36)
uses [GhostController::isReplayEnd](https://github.com/supertuxkart/stk-code/blob/1fb491f507216c5d181ccd85f29ff08eca003827/src/karts/controller/ghost_controller.hpp#L71),
whose sample index advances from the
[recorded timestamps](https://github.com/supertuxkart/stk-code/blob/1fb491f507216c5d181ccd85f29ff08eca003827/src/karts/controller/ghost_controller.cpp#L44).

Benchmark acceptance therefore uses **three complete instrumented replays and
three matching uninstrumented controls**. Run all six under the same driver,
architecture, replay, seed and graphics settings, retaining failures. Repeat
this separately for x64 and x86. Every run must independently pass the unchanged
pacing criterion above. Each of these three sets of retained intervals must sum
to at least 60 seconds: instrumented Present, instrumented main loop, and control
main loop. The assessor never counts time between processes and never averages a
failed run into passing runs.

Both timing traces exclude their first 15 seconds and last 2 seconds, retaining
only whole intervals within those boundaries. The Present trace begins at the
process's first presentation; the upstream main-loop trace begins when its race
profiler starts. These clocks do not share a start point. Their distributions
are reported separately; they are not required to be exactly equal. The source's
[main-loop marker](https://github.com/supertuxkart/stk-code/blob/1fb491f507216c5d181ccd85f29ff08eca003827/src/main_loop.cpp#L457)
measures elapsed time through rendering, with Windows QPC underneath it, rather
than physics-update counts or thread CPU usage.

Each full raw trace must contain at least 37 seconds before trimming, a short-
capture guard based on the final recorded replay timestamp with a 137.501 ms
allowance for marker boundaries. Completion is independently checked: the
main-loop CSV's positive frame count and summed microseconds, floored to
milliseconds, must exactly match the single upstream `Profiler: Frame count`
completion line. The logged replay path must name the pinned benchmark. Each
instrumented Present trace must have contiguous frames, successful HRESULTs and
its normal-shutdown footer. A zero exit code, saved 720p graphics configuration,
version 1.5 and the D3D9 renderer log are required for every run.

Before assessment, the trusted VM collector supplies `guest-facts.json` for each
run, based on the actual guest directory and deployed files:

```json
{
  "run_id": "C:\\triton-kart-results\\benchmark-x64-123-456",
  "architecture": "x64",
  "wow64": false,
  "instrumented": true,
  "replay_sha256": "0049a18428049c02dcfdb6e2944c7ba7196f2a0017f239d0305ed12fb15614df",
  "executable_sha256": "cbbc2e03a79c669c02878a7345e46094657a7a8c4f4fd97496f909a0bcd5a650",
  "driver_sha256": "<actual deployed driver package SHA-256>",
  "probe_sha256": "<actual deployed probe DLL SHA-256>",
  "seed": 20260926
}
```

Replace the angle-bracket values with observed lowercase hashes. To obtain
those hashes, have the VM collector export the actual installed game executable,
replay, deployed driver package and (for instrumented runs) loaded probe to a
separate collection directory. Then hash those collected bytes, for example:

```sh
sha256sum /collected/deployment/supertuxkart.exe \
  /collected/deployment/benchmark_black_forest.replay \
  /collected/deployment/driver-package.zip \
  /collected/deployment/d3d9.dll
```

The paths above are examples of collector outputs, not input expectations.
Record which actual installed package the collection represents; do not hash
an unrelated candidate archive or fill fields from this document. Derive the
architecture and WOW64 state from the actual process/guest observation. For an
instrumented run, its v2 Present header records `process_arch`, `wow64` and
`system_directory`; control identity must come from the collector because no
probe runs there. Set `architecture` to `x86` and `wow64` to `true` for an x86
process under 64-bit Vista; native 32-bit Vista uses `x86` and `false`. Native
x64 uses `x64` and `false`. The assessor rejects inconsistent collected and
captured architecture or WOW64 state.

Use the unique
actual guest run directory as `run_id`. For x86, the pinned executable hash is
`f5d164dd52242c8aa356dc8b141579680290e67dc1a1dea0ba02667ffd5a37ca`.
For a control, set `instrumented` to `false` and `probe_sha256` to `null`, and
verify the proxy was absent. The driver package identity must be the same across
all six repetitions. Probe versions must match across the three instrumented
runs. These collector facts must not be fabricated from expected values; the
host receipt does not independently attest which guest files were loaded.

Seal each collected directory once, then assess all six together:

```sh
python3 tests/vista/run-kart-game.py seal-benchmark --results /results/measured-1 --identity /results/measured-1/guest-facts.json
# Repeat sealing for measured-2, measured-3, control-1, control-2 and control-3.
python3 tests/vista/run-kart-game.py assess-benchmarks \
  --instrumented /results/measured-1 /results/measured-2 /results/measured-3 \
  --controls /results/control-1 /results/control-2 /results/control-3 \
  --output /results/benchmark-group.json
```

`run-integrity.json` binds each run's stdout, version output and version exit code,
configuration, game exit code,
main-loop CSV and, when instrumented, Present CSV to their collected SHA-256s.
The assessor rejects missing/edited files, repeated guest identities, duplicate
CSV timing captures (even if renamed/resealed or given changed whitespace,
unrelated columns or shifted QPC origins), wrong replay/executable identities,
nonzero exits, inconsistent completion counts and mismatched groups. It retains
all six diagnoses in its JSON report, including failed repetitions. Integrity
receipts detect collection errors; screenshots/video and trusted guest collection
remain necessary evidence of actual rendering.

The group report compares the three-run median FPS and frame-time percentiles
between matching instrumented and control main-loop traces. It reports absolute
and relative differences, plus every individual run, so probe overhead and
run-to-run variation remain visible. A comparison is suppressed for mismatched
or incomplete groups. A passing pacing result does not by itself prove negligible
measurement overhead.

The control batch temporarily removes the measurement DLL and restores it.
A failed or interrupted control may leave `d3d9.dll.disabled`; the next control
refuses to overwrite it. Restore it before another instrumented run. Remove the
probe permanently for normal gameplay.

## Results required from the curated installed build

| Required evidence | Status |
| --- | --- |
| Vista version, architecture, matching host/guest hashes | Pending |
| Authentic x64 and WOW64 game races finish with correct pixels/input | Pending |
| Three instrumented replays and three controls per architecture | Pending |
| Matched guest production and actual QEMU-window advancing frames | Pending; earlier 10–12 updates/s regression remains open |
| Frame pacing, stalls, display-awake state and instrumentation sensitivity | Pending |
| Desktop 3DMark06 visuals, pacing and completion | Pending |
| Fresh review of curated implementation, harness and exact results | Pending |

`self-test` exercises the production measurement proxy and parsers using CPU
fixtures, including private runtime dispatch, COM lifetime, reentrancy,
concurrency, architecture/path identity and intentionally invalid captures.
`build-probes` compiles the x64/x86 Windows DLLs through the public build
container; `verify` checks the official workload files and PE dependency closure.
None executes the actual game in Vista or measures its displayed performance.

The current assessor requires `triton-kart-present-v2` captures. Older v1
captures remain historical evidence for their own package. The probe preserves
the runtime's full private vtable and validates the loaded native/WOW64 UMD path.
Rebuild and deploy both probe architectures and launchers as one matched set;
record their hashes. Do not relabel old media or reports as this candidate.
