# Vista Control Panel / Personalization recurrence

> Development record retained with the public desktop sources. Local capture,
> VM and `.unlazy/` paths refer to non-shipped development artifacts. See
> [current public entry points and status](DESKTOP-MODULES.md).


The Control Panel repair retains the stock licensing runtime and adds an automatic readiness check. The requested full licensing bypass was attempted, failed integration testing, and was rolled back. Both original SLC DLLs and their TrustedInstaller ownership and permissions are restored. No kernel patch was installed.

Target: the local Vista Ultimate x64 checked VM, Windows 6.0.6000 RTM, on 2026-09-11.

## Proven failure

Initially, Software Licensing (`slsvc`) was configured for manual startup and was stopped after boot, with exit code 1077 (never started). `SLOpen` returned `0x80070426` (`ERROR_SERVICE_NOT_ACTIVE`). Control Panel and Personalization produced no requested window. Themes and Desktop Window Manager were running, and the graphics device reported `ConfigManagerErrorCode=0`.

Starting `slsvc` restored the licensing API and both populated pages in the same boot. The existing activation was already permanent; it had not removed these runtime dependencies.

A second failure was reproduced with automatic startup: `slsvc` reached RUNNING and activation queries succeeded, but `SLGetWindowsInformationDWORD` returned `0xC004F047` (`SL_E_PROXY_POLICY_NOT_UPDATED`) for both `shell32-EnableProxyFeature` and `WindowsSearchEngine-Licensing-SearchEnabled`. Restarting the service changed both queries to `S_OK` with value 1 and restored the pages. The internal reason the service leaves this cache uninitialized on its first startup remains unproven.

Evidence is under `.unlazy/control-panel-root-cause/`: `baseline.json`, `started.json`, `rollback-stock.json`, `rollback-stock-restarted.json`, and the repaired cold-boot captures. Each capture includes the boot timestamp, service/API state, graphics state and screenshots. The check uses window identity plus OCR for populated contents; Vista's MSAA interface returned no names even for working windows.

## Installed repair

- `slsvc` is configured for automatic startup.
- The SYSTEM task `TritonLicensingReady` runs `C:\ProgramData\TritonLicensing\licensing-ready.exe` at boot.
- The helper waits for service startup and checks the two actual protected policies. If they remain unavailable after six attempts, it restarts the service once, checks recovery, logs the result, and exits. It does not repeatedly restart a healthy service or alter activation data.
- Its log is `C:\ProgramData\TritonLicensing\readiness.log`. A successful run ends with `LICENSING_READY` and task result 0.
- `SLUINotify` and ReadyBoost remain disabled as previously requested.

Source: [`packaging/vista-licensing-ready.c`](../packaging/vista-licensing-ready.c). Build with the existing Linux builder:

```sh
podman exec -w /workspace vista-driver-builder x86_64-w64-mingw32-gcc \
  -O2 -s -Wall -Wextra -Werror -Wno-misleading-indentation \
  packaging/vista-licensing-ready.c -o build/vista-licensing-ready.exe -ladvapi32
```

The task only queries two existing policy values and performs a bounded service restart when necessary. Its data and executable directory grants SYSTEM and Administrators full access and Users read/execute access.

## Why the bypass was rolled back

The experimental 32-bit and 64-bit replacements retained all 41 export names and ordinals and returned cached edition policies and the existing activated SKU. Private tests passed, but cold-boot integration failed.

The returned policy values are insufficient to provide Vista's protected-code execution. Explorer and Control Panel trapped at `SHELL32+0x167515`, followed by further `INT3` instructions in the proxy path; SearchIndexer also failed in its protected path. Microsoft's [kernel-mode redirection design](https://patents.google.com/patent/US20060191014A1/en) describes licensing-bound proxy execution triggered by such instructions. This behavior explains why simply returning success does not provide the code these components need.

With the replacement installed, the stock service exited with `0xC004D401`, a security-processor system-file mismatch. Starting SPSys independently did not restore the missing execution support. SHA-1 page-hash signing with the VM's existing test certificate also failed this separate licensing integrity check. Native kernel policy queries returned `STATUS_ACCESS_DENIED` for protected policies while ordinary policies remained readable.

The BCD `nointegritychecks` flag is left at `Yes`, as requested; `testsigning` was already `Yes`. This boot setting does not disable the licensing subsystem's own integrity checks. Those checks were not bypassed. The replacement DLLs are retained only as failed experimental artifacts under `.unlazy/control-panel-root-cause/` and are not installed.

## Verification and recovery

The final checks compare both installed SLC DLLs byte-for-byte with the originals, verify two distinct cold boots with populated Control Panel and Personalization pages, check the task's successful result and native policy responses, and run `slmgr.vbs /xpr`. Gate results and transcripts are in `.unlazy/control-panel-root-cause/GATES.md`. The two bypass gates remain abandoned rather than being represented as successful repairs.

The cold disk backup is `vista-kvm/x64-base/pre-licensing-shim-20260911.qcow2`; `qemu-img check` reports no errors. Keep its `base.qcow2` backing chain intact.

For complete rollback, shut Windows down and confirm `podman inspect --format '{{.State.Running}}' triton-vista-x64-normal` returns `false`. Preserve the current `work.qcow2`, copy the backup to a new file beside it with `cp --reflink=auto --sparse=always`, then rename that copy to `work.qcow2`. Run `./run-vm.sh`. Never replace a disk that QEMU has open. This also restores the earlier manual licensing-service configuration that caused the initial recurrence.

The DLL rollback helper was executed and the original runtime booted successfully; its DACLs were restored from `system32-acl.txt` and `syswow64-acl.txt`, followed by resetting ownership to `NT SERVICE\TrustedInstaller`. To remove only the new startup check, run `schtasks /delete /tn TritonLicensingReady /f` from an elevated prompt. To restore the original boot integrity flag, run `bcdedit /set {current} nointegritychecks off` and reboot.
