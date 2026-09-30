# Developer-preview validation

This document defines release evidence, not a claim that the candidate passes.
A newly curated source tree must be built and tested itself. Historical tests
apply only to their original sources, installed binaries, hardware and workload.

## Required evidence

| Gate | Evidence to retain |
| --- | --- |
| Source restoration | Clean-clone bootstrap, source/patch hashes and repeat-bootstrap result |
| Builds | Container image ID, commands, x64/x86 KMD/UMD results and Linux host results |
| Packaging | PE/INF checks, signatures, catalog membership, extracted ISO manifest and hashes |
| CPU regressions | Exact commands, terminal statuses and retained failure output |
| Native GPU tests | GPU/driver, binary/source identities, pixel/lifetime results and limitations |
| Guest runtime | Exact installed package/host identities, Aero/probe results, boot/reset behavior |
| SuperTuxKart and desktop 3DMark06 | Matched guest production and advancing actual QEMU-window frames, frame pacing, stalls, visual correctness and completion |
| Review | Fresh adversarial findings, corrections and clean review of the final candidate |

Keep failed, timed-out and interrupted runs explicitly unsuccessful. A passing
subset does not convert a failed full run to success. Do not use host-native
tests instead of the real Vista applications.

## Visible-window performance is a blocking gate

The recorded 2026-09-27 SuperTuxKart regression is approximately **10–12 actual
QEMU-window updates per second**. It remains open until valid measurements of
the corrected installed build establish acceptable presentation.

Measure guest frame production and visibly advancing game frames in the host
QEMU window over the same workload and interval. Include pacing/stalls, not
just averages. Successful `Present` calls, repeated draws, vblank counters,
static screenshots and the Aero-enabled flag are insufficient. A mismatch
between produced and displayed frames blocks a performance success claim.

Record that Vista's display stayed awake and that no other GPU tests ran during
the capture. Retain interference evidence and rerun affected captures. Earlier
failures cannot be dismissed by guessing at power-saving state.

## Scope of a public preview

Passing compilation and packaging permits describing those checks precisely.
It does not establish full DX9/DX10 conformance, x86 guest compatibility, smooth
Aero, successful benchmark completion or acceptable gaming performance. Any
remaining runtime defect must stay visible in release notes. Broad compatibility
and performance remain unproven until their corresponding gates pass.

Agent review supplements testing locally. It does not constitute human testing
or satisfy an upstream project's contribution eligibility rules. See
[UPSTREAM](UPSTREAM.md) for those constraints, the scoped MIT grant and remaining attribution questions.
