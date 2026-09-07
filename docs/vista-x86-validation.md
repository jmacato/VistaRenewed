# Vista x86 validation — 2026-09-08

The x86 package booted twice with `testsigning No` and `nointegritychecks No`. The first boot supplied the BCD output. That output contained no `loadoptions` or `safeboot` entry. Both boots passed the installed-payload checks, the D3D9 probe, and the Aero API probe. The user confirmed that Aero works after the second boot.

The tests used an independent full disk clone. The original VM remained available with the previously accepted graphics package. The clone used four emulated CPUs, 4 GiB RAM, and the Neptune-enabled QEMU build on macOS arm64. Reboots were slow under TCG.

Window movement and the animated Acer GameZone menu passed visual review with the timeline fence change. The user accepted the flicker correction. This evidence does not establish compatibility with every game, Vista SP1, or x64.

The UMD signals a new 64-bit fence value for each Present and waits for that value before the host copy. It then waits for a consumption marker on the same kernel context before source reuse. The KMD waits for an active scanout copy before DMA completion. QEMU copies shared-memory pixels into its own display surface and preserves retained primary pixels for rectangle updates.

The CPU waits for the GPU fence with bounded `Sleep(1)` polling. The generic END-only query feedback path still needs separate investigation. The x64 boot policy remains unchanged. The existing service bypass for its custom signature check also remains unchanged. The package still uses manifest hashes, a pinned certificate, and the existing installation checks.

Run the regression tests from the repository root. Python 3, Clang, and Clang++ are required.

```sh
for test in tests/vista/test-*.py; do
    python3 "$test" || exit 1
done
python3 triton-kmd/viogpu/tools/check_vista_clang_varargs.py
python3 triton-kmd/viogpu/tools/check_vista_inf.py --arch x86 packaging/viogpu3d-diagnostic-x86.inf
```

The varargs check also requires the extracted WDK inputs. The extraction harnesses exercise the actual source helpers with controlled completion, failure, and buffer conditions. They supplement the VM results.

The tested package identifiers and native fence call path are in [vista-x86-validation.json](vista-x86-validation.json).
