# Project instructions

## Verify the frame rate actually displayed by QEMU

- Guest FPS counters and successful `Present` calls do not prove that the
  QEMU window displays frames at that rate. Never report acceptable game
  performance from those measurements alone.
- Measure guest frame production and QEMU window updates over the same
  workload and time interval. Verify visibly advancing game frames in the
  actual host window; repeated draws, vblank counters, a static screenshot,
  or an enabled Aero flag are insufficient evidence of smooth presentation.
- Treat a mismatch between guest FPS and visible host presentation as a
  blocking performance defect. Investigate and fix it before declaring the
  game, benchmark, or display path successful.
- Validate the final installed build with the actual Vista applications:
  SuperTuxKart and the desktop 3DMark06. Retain frame pacing, stalls, visual
  correctness, and completion results. Keep failed or interrupted runs
  explicitly failed; do not substitute native host tests for these checks.
- Keep other GPU tests out of performance captures. Record any measurement
  interference and rerun affected captures before making performance claims.
- Keep Vista's display awake during agent-run performance captures and record
  that condition. Power-saving state can change DWM and vblank behavior;
  do not dismiss an earlier failed run without evidence from that run.
- Preserve this requirement throughout implementation and fresh adversarial
  review. The user's priority is the frame rate they actually see in QEMU.

Recorded regression, 2026-09-27: QEMU reported only about 10–12 window updates
per second during a SuperTuxKart run. The earlier guest-side FPS result did
not establish acceptable visible host performance and must not be used to
close that requirement.
