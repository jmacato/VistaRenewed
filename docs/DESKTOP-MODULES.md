# Vista desktop tools

## IE and MSHTML

Build the optional adapters and probes after creating the toolchain image:

```sh
bash scripts/dev-container.sh build
python3 scripts/build_vista_desktop_extras.py
```

Outputs go in `build/`. They are separate from the graphics driver ISO.
The source includes document adapters, browser transport and tests for
activation, navigation, content and window handling.

`tools/vista_mshtml_install.py` provides per-user installation, status and
rollback commands. It requires a running VM, the control service and a
separately installed Supermium runtime. Read its help before installing:

```sh
python3 tools/vista_mshtml_install.py --help
```

The adapters are prototypes. See [current limits](STATUS.md).

## Sidebar

Follow the [Sidebar setup guide](VISTA_SIDEBAR_GADGETS.md). It covers the
host relay, guest updates and checks.

## Supermium and CEF

The full CEF build is unfinished. The source and toolchain helpers are
`scripts/supermium_cef_source.py`, `scripts/supermium_cef_toolchain.py` and
`scripts/supermium_cef_build.py`; each has `--help`.

`packaging/supermium-runtime-pin.json` records the runtime version, download
URL and checksum. Put the matching archive at the path listed in that file,
then run `python3 scripts/stage_supermium_runtime.py` to create a runtime ISO.
Downloaded browser files, SDKs and source trees are separate inputs; their
license terms still apply.

## Other tools

- [Desktop control and file transfer](VISTA_CONTROL.md)
- [Graphics tracing](TRITON_TRACE.md)
- [Test commands](../tests/README.md)
- Media Center helpers: `tools/measure_wmc.py` and `tools/verify_wmc_results.py`
