# Verification and limits

On September 7, 2026 the x64 checked Vista SP2 guest passed the public rendering
gate and actual Glass image checks across two guest-owned boots. Installed file
hashes and distinct launch nonces tied the results to the tested package.
See the [full report](../notes/AERO_GLASS_VERIFICATION.md).

The accepted package was `ea971975b1d65c3fc7a59f52d66e552c38ac26d79746062cb82052569e17c261`.
The source later gained quieter kernel tracing, native GTK display and USB
tablet input. Those changes must not be confused with the package used for the
two-boot evidence. A later quiet package passed the first public rendering gate;
its second-boot result is not asserted here.

## Known limits

- Runtime acceptance covers this x64 guest and Intel Vulkan host. x86 builds
  pass compile/PE checks but have no equivalent guest runtime proof.
- The driver is incomplete; this is not full D3D9 conformance or WHQL certification.
- TCG boot hit STOP 0x7E. KVM was restored for interactive development.
- A later shutdown hit STOP C000021A. Its cause was not established. Avoid
  issuing manual shutdown while the guest deployment service owns a reboot.
- Native GTK/OpenGL can leave QMP `screendump` stale. Inspect the actual window
  for that configuration; the strict image verifier expects an undecorated
  guest capture, not a scaled host-window screenshot.
- Reduced logging eliminated observed debug-port traffic. The CPU samples were
  not controlled FPS benchmarks and do not establish a graphics speedup.
- Setup and test signing still require development-machine configuration.
  The packaging route currently contains a deployment trust identity that
  must be replaced consistently for another developer's signing keys.

## Evidence included here

The two accepted PNGs are copied byte-for-byte into `docs/evidence/`, with a
SHA-256 manifest. The detailed report retains the local run identifiers for
traceability. Full deployment logs, Windows shader captures and VM disks remain
local and are not distributed. The PNGs can be remeasured, but they alone do
not independently reproduce the installed-file and reboot-order checks.
