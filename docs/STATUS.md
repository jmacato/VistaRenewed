# Verification and limits

On September 7, 2026, the checked Vista SP2 x64 guest passed the rendering and Glass image checks across two boots.
A checked build includes additional diagnostic checks.
Installed file hashes and distinct launch identifiers connected these results to the tested package.
See the [full report](../notes/AERO_GLASS_VERIFICATION.md).

The accepted package identifier is `ea971975b1d65c3fc7a59f52d66e552c38ac26d79746062cb82052569e17c261`.
Later source changes added quieter kernel logging, a native GTK window and USB tablet input.
GTK is the window toolkit that QEMU uses for its native display.
The USB tablet supplies absolute pointer coordinates.
These changes are separate from the package with the accepted two-boot evidence.
A later package passed its first rendering check.
This report does not establish its second-boot result.

## Known limits

The runtime tests cover one x64 virtual machine (VM) and one Intel Vulkan host.
Vulkan is the host graphics programming interface.
x86 builds pass compilation and Portable Executable (PE) file checks.
The tests do not establish x86 guest compatibility.
The driver does not implement every Direct3D 9 operation.
The package has no Windows Hardware Quality Labs (WHQL) certification.

A Tiny Code Generator (TCG) boot produced STOP 0x7E.
TCG emulates guest processor instructions.
Development resumed with Kernel-based Virtual Machine (KVM) hardware acceleration.
A later shutdown produced STOP C000021A.
The investigation did not establish its cause.
While the guest service controls a restart, do not issue another shutdown request.

With native GTK/OpenGL display, QEMU Machine Protocol (QMP) `screendump` can return a stale image.
QMP supplies the VM control interface.
For this display mode, inspect the actual window.
The strict image verifier requires an unscaled guest capture without host window decorations.

Reduced logging eliminated observed debug-port traffic.
The processor samples do not measure frames per second (FPS) under controlled conditions.
They do not establish a graphics speedup.

The local package tools use a persistent development signing identity.
The [continuous integration (CI) workflow](CI.md) generates a temporary identity and matching certificate pins for each build.
A certificate pin identifies the certificate that the service accepts.
CI does not provide production signing or guest runtime tests.

## Included evidence

The `docs/evidence/` directory contains two unchanged Portable Network Graphics (PNG) captures.
A SHA-256 manifest records their cryptographic file hashes.
The detailed report retains the local run identifiers.
Deployment logs, captured Windows shaders and VM disks remain local.
The image verifier can repeat the pixel measurements.
The images alone cannot repeat the installed-file or reboot-order checks.
