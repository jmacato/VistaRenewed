# Current limits

This is a developer preview.

- **Triton:** Direct3D compatibility and display pacing are still being fixed.
  Build and CPU checks do not prove that applications run correctly in Vista.
  Guest FPS counters do not prove smooth output in the QEMU window.
- **IE/MSHTML:** the adapters are prototypes, not a complete IE replacement.
  The full Supermium/CEF source build is unfinished. The graphics installer
  does not register these adapters.
- **Sidebar:** the providers need network access from the host. If a service
  is unavailable, the relay uses cached data when it has any.

Use matching guest drivers, QEMU, renderer and backend builds. Screenshots
show development builds; they do not validate a new installer.
