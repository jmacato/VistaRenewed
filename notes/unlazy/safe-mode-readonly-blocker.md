# Safe Mode deployment failure

Run `r-b3ed276f4b6d` used immutable deployment `bff1650bebc90e9cfdb810de68ff9cd413264a5e8ca5c1353c68ed86aa29a85b`. The guest authenticated the package and requested Safe Mode without host input.

The COM2 log stopped after `REBOOT_TO_SAFE_MODE`. Read-only inspection of the stopped qcow2 found the complete guest log. Safe Mode started at `2026-08-26T09:46:02Z`.

The service staged both UMDs and its replacement executable. It then retried `neptune_d3d9.dll` replacement 123 times with Win32 error 5.

The NTFS metadata identifies the cause. Both installed UMDs have `RA` attributes. Their staged replacements have only the `A` attribute.

`CopyFileW` preserved the read-only attribute from earlier optical media. The old service cleared this attribute only on each staged file. Vista rejected replacement of the read-only destination.

The source now clears only `FILE_ATTRIBUTE_READONLY` on an existing destination before `MOVEFILE_REPLACE_EXISTING`. The source audit and service PE build pass.

Immutable deployment `7d478df5b5f55d5e06002060b53d168e31e92e65990218904063d977a0f18947` contains the corrected, signed service. Its package, catalog, source manifest, and ISO checks pass.

The stopped guest still owns deployment `bff1650...a85b`. Its registry state is `SafeModeOwned=1`, `Result=INSTALLING_SAFE`, and `InstallOutcome=PENDING`.

The installed old service accepts only the exact `bff1650...a85b` manifest. It cannot consume the corrected deployment while it owns this Safe Mode transaction.

No raw disk derivative or framebuffer was created. The host did not inject input, select a boot entry, or write to the Vista disk.
