# Vista guest-owned driver deployment

`TritonVistaDeploy` owns the complete deployment cycle inside Vista. The host does not send input, select a boot mode, or restart the VM.

The service uses this sequence:

1. It finds `triton-deploy.ini` on the read-only deployment ISO.
2. It checks the pinned certificate and package hashes. It also verifies the catalog, its members, and each signed executable.
3. It records Safe Mode ownership, sets the Safe Mode BCD option, and restarts Vista.
4. In Safe Mode, it stages each file beside its destination. It flushes each staged file and compares its bytes with the package.
5. It atomically replaces the unloaded KMD and both UMDs. It schedules one restart-time replacement for the running service executable.
6. It verifies the three graphics files before it permits a normal boot. An interrupted deployment resumes in Safe Mode.
7. It removes its Safe Mode BCD option and restarts Vista. An older service generation can require one additional activation restart.
8. After restart, it compares the installed KMD, both UMDs, and the running service with the package.
9. It records success only after all byte comparisons pass. It then starts the public D3D9 probe with a unique result nonce.
10. It waits for the complete public probe and controlled Aero scene. It then requests a second guest restart.
11. After the second restart, it verifies all installed bytes again and repeats the public probe.

The service stores its state in `HKLM\SOFTWARE\Triton\VistaDeploy`. It writes a durable log to `%SystemRoot%\Temp\triton-deploy.log`.

If QEMU provides COM2, the service also writes `TRITONDEPLOY` status records to COM2. COM2 is an output-only evidence channel.

Each probe uses a nonce-specific result file in `%SystemRoot%\Temp`. Inspect guest files only after the exact Vista QEMU process stops.

## Existing guest requirement

The current Vista image must already contain the automatic `TritonVistaDeploy` service. The display INF installs this service and registers its Safe Mode entries.

An old image without this service needs a one-time guest-side bootstrap. Do not use host input or a host-selected boot mode for that bootstrap.

## Build and stage the media

Use `--build-only` after a KMD source change. Use `--restage-only` only when the signed KMD still matches the source manifest.

```sh
scripts/build_deploy_vista_driver.sh --build-only
scripts/stage_vista_deploy_media.sh
```

The stage script verifies `package-manifest.sha256`. It creates an immutable bundle under `vista-deploy-publications/<deployment-id>` and atomically updates `vista-deploy-current`.

The bundle contains its verified tree, ISO, ISO hash, and deployment identifier. The launcher resolves this bundle once, so pointer rotation cannot change its media.

The stage script creates a read-only ISO9660/Joliet image with a 512 MiB limit. It checks free space first.

The ISO contains the signed package, public certificate, deployment service, public probe, and `triton-deploy.ini`. The configuration pins certificate and probe hashes.

## Start the guest-owned deployment

Check that no Vista QEMU process uses the qcow2. Then start the hardware-only launcher:

```sh
scripts/run_vista_guest_deploy.sh
```

The launcher creates a private `0700` run directory below `vista-deploy-runs`. It stores the run token, deployment identifier, QEMU identity, `status.log`, and QMP socket there.

The launcher attaches the verified ISO as read-only optical media. It does not send QMP input, HMP input, keyboard input, or mouse input.

Vista can restart while QEMU stays active. The guest service owns every restart, Safe Mode transition, and recovery action.

## Accept the result

Do not accept a staging message as success. Require these ordered records for one deployment identifier:

- `PACKAGE_SIGNATURES_OK`
- `POST_REBOOT_FILE_OK` for the KMD, native UMD, WoW64 UMD, and deployment service
- `POST_REBOOT_PAYLOAD_OK`
- two `POST_REBOOT_COMMIT` records
- `REPROBE_REBOOT`
- `REPROBE_LAUNCHED`
- two nonce-matched `PROBE_RUN_ARMED` and `TRITON9-RUN` pairs

Each public probe must pass clear/readback, triangle/readback, and PresentEx. Collect a passive PNG only while its controlled scene is active.

Use the exact run directory and deployment identifier that the launcher recorded:

```sh
run_dir=/absolute/path/to/vista-deploy-runs/<run>
deployment_id=$(<"$run_dir/deployment-id")
png=/absolute/path/to/new-proof.png

python3 scripts/capture_vista_passive_png.py \
  --run-directory "$run_dir" \
  --status-log "$run_dir/status.log" \
  --deployment-id "$deployment_id" \
  --qmp-socket "$run_dir/qmp.sock" \
  --output "$png" \
  --minimum-probe-passes 2
python3 scripts/verify_aero_glass_pixels.py "$png"
shasum -a 256 "$png"
```

Use `--minimum-probe-passes 1` only for an optional first-run capture. The final proof requires `--minimum-probe-passes 2` after the guest restart.

Record the PNG hash and verifier measurements. Then remove that exact temporary PNG.

Stop the exact Vista QEMU process before offline inspection. Read the existing qcow2 in place and in read-only mode.
