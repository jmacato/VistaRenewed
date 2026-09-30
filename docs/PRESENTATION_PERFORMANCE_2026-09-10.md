# Presentation performance follow-up (in progress)

> Development record retained with the public desktop sources. Local capture,
> VM and `.unlazy/` paths refer to non-shipped development artifacts. See
> [current public entry points and status](DESKTOP-MODULES.md).


The package 38 correctness report does not establish maximum throughput.
The host uses Intel Iris Xe (renderD128). The RTX 3060 is bound to vfio-pci,
with no host NVIDIA/render node available; its binding was not changed.

A new idle/fullscreen-transition hang was captured: WMC PID 2984 reported
Not Responding, its render thread waited in dwmapi from d3d9, and DWM waited
in GDI32 D3DKMTWaitForVerticalBlankEvent. A fullscreen RGB mode transition
released the wait. This is not yet a diagnosed/fixed root cause. Stack evidence:
perf39-stacks.txt and perf39-dwm-stacks.txt under vista-kvm/x64-base.

The initial host experiment enables renderer THREAD_SYNC and registers each
context's notification FD with QEMU's main loop. Watches are removed before
context destruction, reset and renderer cleanup; the polling timer remains a
fallback. The lifecycle harness and QEMU build pass, and runtime traces show
notification callbacks firing. However, one isolated native Ex comparison
measured 140.99 FPS before and 133.93 FPS after at 300 Hz, so this is NOT a
verified performance improvement. Both runs correctly synchronize at 60 Hz
(59.75 and 59.99 FPS), with successful full-frame RGB checks.

Package 39 adds opt-in TRITON9_PRESENT_PROFILE=1 timing, aggregated per device
in groups of 120 presents. Stage 0: producer GPU completion plus health query;
stage 1: post-completion transport drain and kernel context setup;
stage 2: runtime Present callback; stage 3: source-consumption completion.
Profiling is disabled by default and does not remove synchronization.
The build and both architecture PE/ABI audits passed; runtime profiling pending.

Invalid attempted baselines are explicitly listed in
vista-kvm/x64-base/perf39-invalid-measurements.txt. Do not use their timings
as WMC animation performance evidence. The four performance gates remain open.
