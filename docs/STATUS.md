# Current limits

This is a developer preview.

## Triton

Direct3D compatibility and display pacing are still being fixed. Test the
installed drivers with their matching QEMU, renderer and backend builds.
For display performance, measure advancing frames in the QEMU window along
with guest frame production. Guest FPS counters alone miss host display stalls.

## IE and MSHTML

The adapters are prototypes. The full Supermium/CEF source build is unfinished.
Install the adapters separately; the graphics installer leaves IE unchanged.

## Sidebar

The host needs internet access for the data providers. The relay can serve
cached data during a service outage. A request fails if no cached data is available.

The screenshots show development builds. See [capture dates](images/README.md).
