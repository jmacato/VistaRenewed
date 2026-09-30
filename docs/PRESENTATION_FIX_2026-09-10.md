# Vista x64 presentation fixes: final package 38

> Development record retained with the public desktop sources. Local capture,
> VM and `.unlazy/` paths refer to non-shipped development artifacts. See
> [current public entry points and status](DESKTOP-MODULES.md).


Package **7.14.1.38** is installed and validated in normal-mode Vista Ultimate
x64 RTM. Native and WOW D3D9/D3D9Ex applications, DWM and Windows Media Center
load Triton. The VM is left running with WMC open. Test signing remains enabled;
this is a development-signed driver, not a production WHQL release.

## Defects corrected

1. A framebuffer address of zero was mistaken for an absent address. DMA flips
   now publish zero correctly; blt/creation paths explicitly preserve the prior
   address. Generation checks and allocation lifetime protections remain.
2. Interval-one presentation ignored the requested refresh interval. The kernel
   now coordinates synchronized flips with vblank and orders DMA completion
   before the CRTC notification. Immediate presentation bypasses this wait.
   Successful waits must not call `RecordFailure`: that helper converts success
   into a failure status. A regression test covers this subtle failure path.
3. Ordinary windowed D3D9 presentation failed with `D3DERR_NOTAVAILABLE` in both
   architectures. The UMD rejected `Blt` flags `0x500`, the normal
   BeginPresentToDwm/EndPresentToDwm combination. It now accepts DWM presentation
   markers while continuing to reject unsupported pixel operations.
   [Microsoft documents these presentation marker flags](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dumddi/ns-d3dumddi-_d3dddi_bltflags).

The earlier WMC capability, shared-resource initialization, raster, cursor and
host presentation fixes remain included. The unsuccessful queued-flip capability
experiment is not enabled. The investigation and intermediate packages are
recorded in [the preceding report](WMC_RERUN_2026-09-09.md).

## Final runtime measurements

| Test | Package 38 result |
| --- | --- |
| Interval ONE at 60 Hz: native/WOW D3D9, Ex, Ex video | All six pass; **59.87–60.40 FPS** |
| Same six variants at 300 Hz | All pass; 122.05–148.71 FPS, below the requested ceiling |
| Immediate presentation: native/WOW D3D9 and Ex, both modes | All pass; **146.21–191.88 FPS**, no universal 60 FPS cap |
| Ordinary windowed native/WOW D3D9 and Ex | All four run over five seconds, load Triton, return success with no occlusion |
| Normal-user windowed native/WOW D3D9 | Both pass; captured ordinary Aero windows show changing, correct solid colors |
| Shared textures, native and WOW | Create/open/readback pass; 65536 pixels each, zero mismatches |
| WinSAT ALU shader, 15 seconds | Valid=1; FPS **95.83**, EffectiveFPS **88.88** |
| WinSAT fullscreen texture, 1280×720, 5 seconds | Valid=1; FPS **200.48**, EffectiveFPS **192.32** |
| Fullscreen RGB while moving the cursor | 12 captures, each 921600 correct pixels; all three colors covered |
| Raster and RGB checks in all fullscreen variants | Valid active/blank raster status; six full-frame readbacks per variant, zero mismatches |

WMC received 32 up/down inputs over approximately 24 seconds. Ten screenshots
captured within that same input loop confirm navigation across five menu rows,
without a modal dialog, black output or stale selection. DWM PID 1672 and WMC
PID 2984 both loaded `neptune_d3d9.dll`.

The WMC trace contains **3166 host scanout completions over 23.98996 seconds**:
median interval **6.884 ms**, p95 **10.166 ms**, maximum **13.595 ms**. The current
desktop is **1280×720, 32-bit, 300 Hz**, queried with `EnumDisplaySettings` after
the run without changing modes. Its nominal period is **3.333 ms**. These
completion intervals are therefore below 300 Hz throughput, although every
recorded gap is shorter than the **16.667 ms** period of 60 Hz. They are host
completion timings, not physical monitor FPS or counts of unique animation
frames. This establishes removal of the reproduced long stalls; it does not
prove a locked physical 60/300 FPS WMC animation or maximum possible performance.

## Verification and evidence

The final setup uses 12 vCPUs, 16 GiB RAM, the 32 GB disk, Intel Iris Xe rendering,
host QEMU build 28, KMD 37 functionality and both package 38 UMDs. QEMU uses GTK
GL with zoom-to-fit disabled. Installed SYS and both DLLs match the signed CD
byte-for-byte; the installed INF reports 7.14.1.38. The private signing key was
removed from the build container after signing.

Both kernel architectures and both UMD architectures built successfully; UMD
Vista PE/ABI audits passed. Eleven targeted local regressions passed: DWM blt
flags, wait status, synchronized flip, scanout address, flip flags, vsync worker,
vsync timing, primary allocation, flip-copy completion, cursor queue and scanout
surface. Negative controls reproduce the old blt-mask, zero-address and
successful-wait failure bugs. `git diff --check` passes.

Evidence is under `vista-kvm/x64-base/`:

- `audit38-manifest.json`, `audit38-verify.txt`, `audit38-modules.txt`,
  `audit38-current-mode.txt`: package identity, loaded modules and display mode.
- `audit38-raster*-ex*.txt`, `audit38-video*.txt`, `audit38-immediate*-ex*.txt`:
  synchronized/immediate presentation, raster and pixel results.
- `audit38-windowed*-ex*.txt`, `audit38-windowed{64,32}.txt`,
  `audit38-windowed-visual/`: windowed calls and independently sampled colors.
- `audit38-shared{64,32}.txt`, `audit38-{fullscreen-,}winsat.xml`,
  `audit38-rgb-cursor/`: shared textures, actual WinSAT and framebuffer captures.
- `wmc-audit38-controlled-{host.log,input.json}`, `audit38-wmc-visual/`:
  WMC trace, inputs, capture times and reviewed menu navigation.

Shared-texture probes cover two texture objects within one process/device per
architecture; they do not establish arbitrary cross-process sharing. QMP
screenshots exclude the hardware cursor, so the RGB captures establish
framebuffer integrity during movement, not independent proof of cursor
visibility. Fast windowed submission counts are not visible frame rates.
The tested application paths pass; untested programs and workloads are not
implicitly certified. Changes remain in the working tree and have not been
pushed.
