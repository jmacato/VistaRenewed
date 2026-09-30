# Vista-to-GPU tracing contract

> Development record retained with the public desktop sources. Local capture,
> VM and `.unlazy/` paths refer to non-shipped development artifacts. See
> [current public entry points and status](DESKTOP-MODULES.md).


The tracing work supersedes speculative performance changes. Existing package
39 and host 39 are the starting configuration; neither the notification change
nor the current CPU profile establishes a speedup. Preserve their identity in
capture metadata and measure tracing against the same binaries with tracing off.

A trustworthy capture must include:

- A unique run identity, binary hashes, GPU/renderer identity, guest architecture,
  selected refresh and resolution, workload arguments and the actual exit/result.
- Per-frame CPU Present phases with process/thread/device identity, kernel
  command admission/submission/retirement, host command handling and display
  publication. Correlation must follow explicit identities and ordering, not
  timestamp proximity alone. Missing links remain missing.
- GPU timestamp spans or an explicit unavailable result. A CPU fence wait and
  a host observation of a completed fence are not GPU execution duration.
- Monotonic timestamps in each clock domain. Cross-domain calibration records
  uncertainty and drift; cross-domain durations must not pretend to have more
  precision than the calibration provides.
- Bounded, opt-in recording with drop/truncation counters, a complete footer,
  and disabled-path overhead kept small. Hot paths must not open/write/close
  a file for every event or log synchronously over serial.
- A single capture command with bounded jobs, serialized guest-control access,
  concurrent input/capture in one scheduler, cleanup in finally blocks and no
  RDP prompts. WMC screenshots must prove navigation happened during the trace.
- Immutable raw evidence, a machine-readable summary, Perfetto-compatible
  timeline JSON and a readable report. Invalid or partial captures must never
  produce a successful performance verdict.
- Negative controls for malformed/truncated data, lost events, missing/duplicate
  IDs, clock errors, wrong modules, occlusion, stale screenshots and failed
  workloads; alternating off/on runs to quantify observer overhead.

Configured 300 Hz, application submission FPS, GPU work, host scanout completion
and physical monitor delivery are different measurements. Do not merge them
into one FPS claim. Preserve both the reproduced DWM vblank wait hang and any
failed tool runs as diagnostic evidence, not successful benchmark baselines.
