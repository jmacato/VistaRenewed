# Source checkpoint before public cleanup

This local repository records the transferred source exports and the subsequent
Linux/Vista work. The former root and component Git object databases were not
included in the transfer; their recorded upstream revisions and historical
changes are in `handoff/repositories.json` and the adjacent patches.

DXVK retains its own Git history. Its local Neptune change is committed as
`c6bb6d57fac2b6cae7f6adbbc521eb949849815e`, based on
`404240fdacf47470b02c76d6e684639a95dc7387` from `osy/dxvk`.
The new commit has not been pushed to that upstream. The parent repository
records it as a submodule and also includes `handoff/triton-dxvk-linux.patch`
so the change can be reconstructed from the upstream base with `git am`.
Do not assume a recursive clone can fetch the local commit from upstream.
`scripts/bootstrap_sources.py` now restores this exact commit from the bundled
delta in `patches/dxvk-neptune.bundle`; see the root README for clone instructions.

VM disks and runtime records, downloaded SDK/WDK/Windows files, generated
packages and host binaries, and local signing identities are excluded.
They remain on disk. `triton-kmd/build` is source and is included.

This is a development checkpoint, not a public release. Historical handoff
notes describe earlier states; `notes/AERO_GLASS_VERIFICATION.md` documents
the accepted Aero evidence. Later development added native GTK display,
USB tablet input, and quieter Vista kernel logging.
