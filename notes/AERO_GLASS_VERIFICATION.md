# Aero Glass verification — 2026-09-07

**PASS in the current Vista SP2 x64 VM across two guest boots.** Both captures show the controlled backdrop through the ordinary USER window, with softened stripe edges supplied by DWM. API success alone is not the proof.

Final installed publication: `ea971975b1d65c3fc7a59f52d66e552c38ac26d79746062cb82052569e17c261`.
Run: `vista-kvm/runs/neptune-22gmrwll`; QEMU PID 163048, four vCPUs.

## Evidence

| Check | First boot | Second boot |
| --- | --- | --- |
| Installed package bytes verified | 09:29:13 UTC | 09:33:50 UTC |
| Full public D3D9 rendering gate, including HWP+PURE | PASS | PASS |
| Six shader-constant isolation pixels | PASS | PASS |
| Eight distinct SM2 texture coordinates | `ff707070` exact | `ff707070` exact |
| Backdrop transmission | PASS | PASS |
| Edge contrast relative to stripe contrast | PASS | PASS |
| Normalized maximum edge sharpness | 0.199 | 0.199 |
| Actual Glass verdict | PASS | PASS |

The deployment service requested the proof reboot at 09:33:29 UTC and logged `REPROBE_LAUNCHED` at 09:33:50 UTC. The unchanged capture gate verified the two-run ordering.

Launch nonces:

- First: `5239a53b0551fe71f0bb240aab84799895a56d063c92ea1ca470c803e7f87172`
- Second: `32b2e76c34129d3fe74f86d6a87f5fd6ba3aeb073dfbe85c85ca518ccefd5f9a`

Artifacts:

- [First settled capture](../vista-kvm/runs/neptune-22gmrwll/glass-ea97-boot1-settled.png)
- [Second capture](../vista-kvm/runs/neptune-22gmrwll/glass-ea97-boot2.png)
- [First metrics, hashes and log location](../vista-kvm/runs/neptune-22gmrwll/glass-ea97-boot1-settled.json)
- [Second metrics, hashes and log location](../vista-kvm/runs/neptune-22gmrwll/glass-ea97-boot2.json)
- [Combined machine-readable report](../vista-kvm/runs/neptune-22gmrwll/aero-glass-verification.json)
- [Build/sign/package audit](../vista-kvm/runs/neptune-22gmrwll/final-package-audit.log)

An independent final audit rehashed both PNGs and saved status logs, checked the verifier and gate hashes, reran both image verifications, and validated the saved logs with `validate_evidence(..., minimum_probe_passes=2)`. Both boots' KMD, native UMD, WoW64 UMD and deployment-service hashes match the immutable published files.

## Cause and fix

DWM's captured SM2 blur shader uses eight different texture-coordinate inputs. The old translation declared only `v3.xy` and sampled `v3.xyyy` eight times: `RasterStates.TCIMapping` had remained zero, mapping every input to TEXCOORD0. The shader constructor now initializes the converter's identity mapping, consistent with the texture-coordinate state accepted by this driver.

The new public pixel regression samples eight distinct gray texels using eight coordinate inputs and checks their mean. Shader Model 2 requires independent TEXLD destinations here and a final MOV to the color output; the regression follows those restrictions. Both x86 and x64 builds passed; runtime acceptance here is specifically the x64 VM.

The two SYSTEM services now serialize exclusive COM2 opens with a shared mutex. Previously the probe held the port open and the deployment service's reboot-launch record reached its durable file but could be lost from host telemetry. The capture gate itself was not loosened.

Temporary DWM shader dumping was removed from the final package. Captured diagnostic programs remain under `vista-kvm/runs/neptune-22gmrwll/dwm-shaders-f8d`.

## Visual test validity

The probe paints raw background stripes and transparent black beneath its small label. It invokes the public DWM frame-extension and blur APIs; it does not draw a simulated blurred backdrop. Measurements use interior rows below the label.

The former plateau-contrast cutoff measured tint strength, not blur: blur can preserve the interiors of wide stripes. Verification now normalizes edge contrast by each surface's own stripe contrast. It retains the independent local sharp-edge search (maximum normalized sharpness 0.35), so tinted or displaced sharp stripes still fail.

Twelve independent synthetic blur/tint combinations pass; opaque Basic, raw transparency, tinted/displaced sharp edges, decoy reflections and undersized images fail. Crucially, the real pre-fix capture still fails (sharpness 0.517), while both final captures pass (0.199).

The first immediate automatic capture preceded presentation of the proof scene and was rejected. It is preserved as `glass-ea97-boot1.png` with its failed metrics; acceptance uses the later settled capture. The second capture allowed 30 seconds after the scene marker before measuring.

This establishes Aero Glass in this development VM. It is not a claim of complete D3D9 conformance, production driver certification, or x86 guest runtime validation.
