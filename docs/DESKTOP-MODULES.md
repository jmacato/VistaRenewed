# Internet Explorer and MSHTML

The adapter connects IE to a Supermium browser process. It is a prototype;
the full Supermium/CEF source build is unfinished.

## Build

```sh
bash scripts/dev-container.sh build
python3 scripts/build_mshtml.py --both
```

Outputs go in `build/`: the x86 and x64 adapter DLLs and navigation test hosts.
Install them separately from the Triton graphics driver ISO.

## Install

You need a running Vista VM, Supermium and the [control service](VISTA_CONTROL.md).
The installer supports per-user installation, status checks and rollback:

```sh
python3 tools/vista_mshtml_install.py --help
```

## Browser engine

The CEF source and toolchain helpers are `scripts/supermium_cef_source.py`,
`scripts/supermium_cef_toolchain.py` and `scripts/supermium_cef_build.py`.
Each has `--help`. Build patches are in `packaging/supermium-cef/`.

`packaging/supermium-runtime-pin.json` records the runtime version, download
URL and checksum. Put the matching archive at the path listed in that file,
then run `python3 scripts/stage_supermium_runtime.py` to create a runtime ISO.
Browser files, SDKs and source trees have their own license terms.
