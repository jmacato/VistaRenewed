# Pinned downstream sources

Run `python3 scripts/bootstrap_sources.py` from a checkout before building.
Python 3, Git, and HTTPS access to the public dependency repositories are required.
The script verifies the source artifacts against `sources.json`, restores the
recorded DXVK commit from the bundle, initializes its pinned dependencies, and
applies the two complete downstream patches. Repeating the command accepts the
same resulting source tree. It refuses unexpected revisions, staged changes,
or divergent tracked/nonignored untracked files; it never resets or cleans your
work. Build outputs should stay in ignored directories.

| Artifact | Base | Purpose |
| --- | --- | --- |
| `dxvk-neptune.bundle` | DXVK `404240fdacf47470b02c76d6e684639a95dc7387` | Existing Neptune native-source commit `c6bb6d57fac2b6cae7f6adbbc521eb949849815e` |
| `dxvk-vista.patch` | DXVK `c6bb6d57fac2b6cae7f6adbbc521eb949849815e` | Vista downstream backend semantics, shared export layout/ownership and allocation cleanup |
| `dxbc-spirv-vista.patch` | dxbc-spirv `0e79a703db8b23004c77dbabacf25ed2d41f0bd9` | Stream-output topology and IO lowering paired with the DXVK patch |

The DXVK patch includes the prior shared-clear flush and deferred-resource
tracking changes. Do not apply those older patches separately. The unfinished
DIRECT_PRIMARY coherent-memory experiment is excluded. SINGLE_PLANE negotiation,
foreign ownership, imported descriptor lifetime, and protection against clearing
shared memory through the debug zeroing option are retained.

The inline-copy optimization rejects non-relocatable images when their existing
usage lacks the required input-attachment or storage bit. This keeps shared
images on the regular copy path without first attempting an impossible usage
change. Compilation and runtime validation of this correction are separate
from source restoration checks.

These are downstream source changes, not claims of upstream acceptance or full
DX9/DX10 compatibility. Component licenses remain in their source trees. No new
ownership attribution or developer sign-off is implied by this packaging.

CPU-only bootstrap regression tests:

```sh
python3 tests/bootstrap/test_sources.py
```

The patch manifest is a reproducibility checksum ledger, not an authenticity
signature. Review changes to both artifacts and the ledger together.
