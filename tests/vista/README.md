# Vista graphics validation

The public preview has separate CPU, native GPU, Windows build/ABI, and installed
Vista acceptance checks. A pass in one tier does not substitute for another.
Do not run every `test-*.py` file in a shell loop: several runners execute GPU
work, and native fixtures cannot establish game performance in QEMU.

## CPU regression suite

From a bootstrapped checkout:

```sh
python3 scripts/test_public.py --list
python3 scripts/test_public.py
```

The fixed manifest runs production helper code with bounded API models, fault
controls, shader/cache logic, transport and resource contracts, and mocked
launcher/control tests. It starts no container, GPU workload, or VM. It requires
Python 3, Clang/Clang++, a C/C++ compiler (`cc`, `c++`, `g++`), `pkg-config`,
`qemu-img`, and GLib, libepoxy and zlib development packages. The EGL teardown
fixture links libepoxy but replaces the EGL/GL calls with CPU mocks. Repository
DXVK native headers are sufficient; Windows SDK downloads and an installed
native backend are not prerequisites for this tier.

Every selected case gets a retained log and a result in
`test-artifacts/cpu-TIMESTAMP-PID/results.json`. Missing dependencies, timeouts,
and failed negative controls fail the suite. `--output DIR` must name a fresh
directory. `--only NAME` runs an explicitly labelled subset; it cannot establish
a full-suite pass. Python assertions must remain enabled.

The CPU checks include KMD flip ordering and timing, cursor queues, host scanout
bounds/refresh and GPU failure propagation, EGL context/reset lifetime, shared
resource/map/query transport, DX9 queries, DX10 descriptor/ownership/upload
contracts, external-memory map initialization, and shader-cache failure handling.
The generic `test-shared-map-initialization.py` regression protects external
payloads from debug zeroing; it does not enable direct-primary storage.

## Native GPU checks

Build the matching public DXVK/native host libraries first; see
[BUILDING.md](../../docs/BUILDING.md). Run these sequentially, outside any VM
performance capture. Select the intended device explicitly with
`DXVK_FILTER_DEVICE_NAME` or the runner's device option. These tests use
readback only as an observation oracle and do not prove CPU-free guest display.

```sh
DXVK_FILTER_DEVICE_NAME='DEVICE NAME' python3 tests/vista/run-d3d9-shaders.py --host
DXVK_FILTER_DEVICE_NAME='DEVICE NAME' python3 tests/vista/run-d3d9-resources.py --host
DXVK_FILTER_DEVICE_NAME='DEVICE NAME' python3 tests/vista/run-d3d9-pipeline.py --host
python3 tests/vista/test-color-copy-backend.py --device 'DEVICE NAME'
python3 tests/vista/test-color-copy-runtime.py --device-filter 'DEVICE NAME'
python3 tests/vista/test-predication-backend.py --device 'DEVICE NAME' --software-device llvmpipe
python3 tests/vista/test-dither-backend.py --device 'DEVICE NAME' --software-device llvmpipe
python3 tests/vista/test-occlusion-readiness.py --device 'DEVICE NAME' --software-device llvmpipe
DXVK_FILTER_DEVICE_NAME='DEVICE NAME' python3 tests/vista/test-stream-output.py
```

The predication runner's Vulkan proxy is strictly a test fault injector. Its
fallback and dropped-command controls run in isolated child environments; do
not install it or put it in a production loader path. Dithering and predication
software-device cases are required controls, so an absent requested adapter
fails rather than silently reducing coverage.

The QEMU external-copy fixtures compile only by default. They require a
configured matching QEMU build and Vulkan/GL/EGL development headers/libraries.
They retain source and binary identities; `--run` requires a device filter:

```sh
python3 tests/vista/test-primary-external-copy.py --output test-artifacts/external-copy-1
python3 tests/vista/test-primary-external-copy.py --output test-artifacts/external-copy-1 --no-build --run --device 'DEVICE NAME'
python3 tests/vista/test-primary-external-integration.py --output test-artifacts/external-integration-1
python3 tests/vista/test-primary-external-integration.py --output test-artifacts/external-integration-1 --no-build --run --device 'DEVICE NAME'
```

`external_copy_fixture.h` contains only test allocation/pattern/identity support;
`evidence.py` prevents overwritten or reused run evidence. Neither contains a
standalone direct-primary/KVM experiment. GPU oracle completion does not certify
the installed QEMU display path.

## Windows build and ABI checks

After preparing the licensed SDK headers and building both Vista UMD targets:

```sh
python3 tests/vista/run-d3d9-shaders.py --build-public
python3 tests/vista/run-d3d9-pipeline.py --build-public
python3 tests/vista/run-kart-game.py build-probes
python3 triton-umd/src/virtio/neptune/triton/tests/test_d3d10_extended_versions.py --abi
```

Public PE probe builds and ABI syntax checks use `scripts/dev-container.sh run`;
no persistent named container is required. The wrapper mounts the checkout at
`/workspace` and forwards compiler stdin. Windows ABI success is distinct from
executing Microsoft's runtime in Vista.

`run-d3d10-compat.py --host` is an aggregate tier: it includes CPU tests,
native GPU presentation/sharing, and Windows ABI/package checks. Run it only
when all those dependencies are prepared and no VM performance capture is active.
The CPU-only subset is `run-d3d10-compat.py --cpu`.

## Configurable paths

| Setting | Purpose/default |
| --- | --- |
| `TRITON_TEST_ARTIFACTS` | Native/generated fixture output root; `test-artifacts` |
| `VISTA_HOST_PREFIX` | Native host installation; `host-linux` |
| `TRITON_TEST_DXVK_LIBDIR` | Colon-separated native DXVK library directories |
| `VISTA_QEMU_BUILD_DIR` | Configured QEMU build; `triton-qemu/build-linux` |
| `VISTA_SDK_ROOT` | Downloaded/extracted SDK header root; `driver/sdk` |
| `VISTA_SDK_VERSION` | Select a header version if several are present |
| `VISTA_UMD_BUILD_X64`, `VISTA_UMD_BUILD_X86` | UMD directories containing `compile_commands.json` |
| `CONTAINER_ENGINE`, `VISTA_BUILD_IMAGE` | Public build-container engine/image |

Set a fresh artifact root for each native campaign and retain failed runs.
Build steps executed in the container require their output directory to be
inside the checkout. See individual `--help` for explicit output/build options.

## Installed Vista acceptance

Install the exact matching curated host/guest build, record its hashes and Vista
version, and run the public D3D9 and D3D10 probes on the interactive desktop.
D3D10 requires both native/WOW64, 10.0/10.1, windowed/fullscreen and 1x/4x sample
cases; inspect [D3D10-RESULTS.md](D3D10-RESULTS.md) for the remaining gate.

Run the real SuperTuxKart workload described in [KART-RESULTS.md](KART-RESULTS.md)
and the desktop 3DMark06. Keep the display awake and other GPU tests stopped.
Measure guest frame production and **visibly advancing frames in the actual
QEMU window over the same interval**. Retain frame pacing, stalls, visual
correctness and completion. Present calls, vblank counters, enabled Aero, or a
static screenshot cannot establish acceptable performance.

The recorded September 27 regression of approximately **10–12 visible QEMU
window updates/s remains open**. CPU/native tests, historical packages, and
agent review do not close it. The public preview makes no full DX9/10
compatibility or acceptable game-performance claim.
