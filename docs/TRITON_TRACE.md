# Vista → GPU performance tracing

> Development record retained with the public desktop sources. Local capture,
> VM and `.unlazy/` paths refer to non-shipped development artifacts. See
> [current public entry points and status](DESKTOP-MODULES.md).


`tools/triton_trace.py` records the existing Vista x64 VM through its local
control service and QMP socket. It supports native and WOW D3D9/Ex raster tests
and the current English, fullscreen 1280×720 Media Center navigation workload.
It requires the tracing KMD, both UMDs, and QEMU; ordinary builds without these
hooks cannot provide equivalent evidence.

```bash
python3 tools/triton_trace.py capture \
  --output vista-kvm/x64-base/my-trace \
  --kind raster --command 'E:\raster64.exe --ex' \
  --package-manifest vista-kvm/x64-base/audit43-manifest.json

python3 tools/triton_trace.py capture \
  --output vista-kvm/x64-base/my-wow-trace \
  --kind raster --command 'E:\raster32.exe --ex'

python3 tools/triton_trace.py capture \
  --output vista-kvm/x64-base/my-wmc-trace \
  --kind wmc --seconds 16 --command 'C:\Windows\ehome\ehshell.exe'

python3 tools/triton_trace.py overhead \
  --output vista-kvm/x64-base/my-overhead \
  --kind raster --command 'E:\raster64.exe --ex' --pairs 4

python3 tools/triton_trace.py analyze vista-kvm/x64-base/my-trace
```

Close an existing WMC instance before a controlled launch. The controller starts
the workload with the console shell's unelevated token, observes its actual PID,
and bounds its lifetime. Its own elevation permits kernel trace control. No RDP,
remote network service, or desktop portal is involved. `--seconds` bounds the
workload, including startup; tracing and file transfer finish afterward.

`--gpu-every 16` is the default. It samples one consecutive Present interval out
of each sixteen while retaining every instrumented CPU/transport event.
`--gpu-every 1` captures every GPU interval at higher cost. `--off` runs the same
workload and observer without enabling the recorders. The overhead command
alternates off/on and on/off pairs and reports every run; it stops on an invalid
workload. Sampling leaves individual unsampled GPU intervals unknown.

Each fresh output directory contains:

- `manifest.json`: run ID, command, collector/source hashes, actual running QEMU
  executable hash, guest binary hashes and optional expected-package checks.
- `umd.bin`, `kmd.bin`, `host.bin`: original fixed-width records with CRC32,
  sequence numbers, capacities, loss counts and completion state. Guest CAB files
  retain the compressed transfer payloads; the manifest hashes the saved files.
- `workload.json`, `environment.json`, `stdout.txt`, controller diagnostics and
  WMC navigation screenshots with input/capture timestamps.
- `report.json` and `report.txt`: validity reasons, observed frame coverage,
  phase distributions, clock uncertainty, GPU sampling and probe misses.
- `timeline.json.gz`: named process/thread tracks, CPU spans, all raw events and
  virtqueue correlation flows for the Perfetto viewer.

Open the compressed timeline with Perfetto's **Open trace file** command. Perfetto
supports gzip-compressed Chrome JSON, including complete spans and flow arrows.
See [Perfetto's format documentation](https://perfetto.dev/docs/getting-started/other-formats).

Guest QPC and host monotonic time are aligned using matched virtqueue cookies:
the host command must begin between guest SEND and RECV. The intersection of
those brackets gives an offset interval, not an assumed exact timestamp. An
inconsistent clock model invalidates cross-clock claims. Same-clock phase
durations do not depend on that alignment. The report separates the application's
60 Hz and 300 Hz intervals from aggregated background pipeline work.

GPU spans come from D3D11 timestamp/disjoint queries on the actual host device.
They measure the GPU-clock interval between consecutive Presents, including
idle time; they are not GPU busy time. Absolute GPU timestamps are not placed on
the CPU timeline without calibration. Results are read after the existing
completion fence using `DONOTFLUSH`, without adding a GPU wait. The diagnostic
read bypasses the optional query-feedback cache: its host generation currently
stays zero while the guest advances generations. Ordinary query-feedback behavior
is outside this recorder change and remains a driver defect to fix separately.

Configured refresh, application Present rate, kernel retirement, scanout
publication and QEMU display handling are separate observations. QEMU's display
event ends after its EGL swap/flush; the recorder does not measure physical
monitor delivery. WMC fullscreen Presents can be correlated directly. A windowed
WMC Blt and a later DWM composition do not get an invented causal link across
shared surfaces.

Invalid or incomplete captures retain their evidence and exit nonzero. Checks
include file checksums, dropped records, unclean stops, stale run IDs, binary
mismatches, missing joins, failed/occluded Presents, foreground loss and absent
workload success. Brief 50 ms responsiveness-probe misses are reported as
observations; a process that never responds is rejected. A valid trace can record
slow frames—it is not a declaration that the application was smooth.

WMC's selected captions are compared with reviewed reference masks and must
follow the injected up/down keys. The current oracle recognizes TV + Movies,
Online Media and Tasks at the supported layout. Unknown captions/layouts are
rejected. Pixel changes alone do not count as navigation. Screenshots are taken
only while the workload's active marker is set; their observer intervals are
retained and can perturb WMC. Custom commands can be captured with `--kind custom`,
but receive no automatic performance-validity verdict.

Recorders are bounded and never wrap. Per-event disk/serial logging is absent.
STOP closes the UMD writer gate before draining it, so a late producer cannot
write into the next run. A kernel timer and UMD deadline bound recording if the
controller disappears. `--deadline-ms 1000` deliberately exercises expiry: a
longer workload should continue normally while the capture is rejected. Do not
use a watchdog-expired capture for performance conclusions.

Build the helper in the existing builder container:

```bash
podman exec -w /workspace vista-driver-builder \
  bash scripts/build_triton_trace_guest_linux.sh
python3 tools/vista_control.py put build/triton-trace-guest.exe \
  'C:\Windows\Temp\triton-trace-guest.exe'
```

Host dependencies are Python 3, Pillow, libarchive (CAB decoding), Podman and the
existing `vista_control.py` client. The guest uses Vista inbox libraries. The
KMD and UMD build scripts compile the fixed native/WOW ABI; QEMU consumes the same
wire header from this monorepo. The normal signing/install/reboot procedure still
applies to changed driver binaries. Runtime hashes are recorded separately from
the working tree: a source snapshot alone does not prove a binary's provenance.
