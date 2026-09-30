# Pinned sources and patches

Dependencies for [Vista Renewed](../README.md).

Run `python3 scripts/bootstrap_sources.py` before building. It needs Python 3,
Git and HTTPS access to the dependency repositories.

The script checks hashes in `sources.json`, restores DXVK from the bundle,
fetches its dependencies and applies the patches below. You can run it again
after a successful bootstrap. Unexpected revisions or local edits stop it;
the script leaves those changes in place.

| Artifact | Base | Purpose |
| --- | --- | --- |
| `dxvk-neptune.bundle` | DXVK `404240fdacf47470b02c76d6e684639a95dc7387` | Existing Neptune native-source commit `c6bb6d57fac2b6cae7f6adbbc521eb949849815e` |
| `dxvk-vista.patch` | DXVK `c6bb6d57fac2b6cae7f6adbbc521eb949849815e` | Vista downstream backend semantics, shared export layout/ownership and allocation cleanup |
| `dxbc-spirv-vista.patch` | dxbc-spirv `0e79a703db8b23004c77dbabacf25ed2d41f0bd9` | Stream-output topology and IO lowering paired with the DXVK patch |


The DXVK patch includes the shared-clear flush and deferred-resource tracking
fixes. It also handles single-plane sharing, imported resources and allocation
cleanup. The direct-primary storage experiment is outside this patch series.

Images that lack the required usage flags use the regular copy path.

To run the source restoration tests:

```sh
python3 tests/bootstrap/test_sources.py
```

Update `sources.json` when changing a bundle or patch. Its hashes check file
contents. Component licenses are in their source trees.
