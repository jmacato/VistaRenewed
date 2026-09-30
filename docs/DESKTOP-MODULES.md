# Vista desktop modules

The public release includes the desktop work alongside the graphics stack.
These are developer sources and optional experiments. Building a DLL does not
establish IE compatibility, and the full Supermium/CEF source build remains
unfinished. No prototype is registered automatically by the graphics installer.

| Module | Sources and entry points | Status |
| --- | --- | --- |
| IE/MSHTML | `tools/ie7_*`, `tools/mshtml_*`, `tools/vista_mshtml_*` | Experimental COM/document adapters, browser transport, activation and navigation probes; reversible per-user installation tooling |
| Sidebar | `packaging/vista-sidebar-gadgets/`, `tools/update_vista_sidebar_gadgets.py`, `tools/verify_vista_sidebar_gadgets.py` | RSS, weather, search and currency providers; original guest HTML preserved |
| Supermium/CEF | `scripts/supermium_cef_*`, `scripts/stage_supermium_runtime.py`, `packaging/supermium-runtime-pin.json` | Pinned source/toolchain workflow and runtime staging; full CEF compilation unfinished |
| Desktop control | `tools/vista_control.py`, `tools/vista_transfer.py`, UI automation and trace tools | Serial service, file transfer and diagnostics |
| Media Center | `tools/measure_wmc.py`, `tools/verify_wmc_results.py`, `tests/fixtures/wmc-720p-captions.json` | Workload and result collection utilities; historical reports are separate from current validation |
| Service readiness | `packaging/vista-licensing-ready.c` | Protected-policy readiness probe; no Windows binary redistribution |

## Build IE/MSHTML experiments

After building the toolchain image described in [BUILDING](BUILDING.md):

```sh
python3 scripts/build_vista_desktop_extras.py
```

This builds the facade, x86/x64 activation adapters and content, element,
navigation, transport and view probes into ignored `build/`. It uses ephemeral
containers via `scripts/dev-container.sh`; no named development container is
required. CI compiles these alongside the public CPU checks. Guest installation
and COM registration are explicit operations in `tools/vista_mshtml_install.py`;
read its help and rollback commands before exercising a disposable Vista guest.
The optional payloads are separate from the graphics driver ISO.

See [MSHTML experiments](MSHTML_CEF_SCOUT.md), [native MSHTML investigation](MSHTML_NATIVE_RE.md)
and [Supermium/CEF source workflow](SUPERMIUM_CEF_BUILD.md). Downloaded browser
archives, source trees and SDKs stay in ignored directories. The runtime staging
script verifies the separately obtained archive against the public pin metadata.
Third-party license terms continue to apply.

## Sidebar

`run-vm.sh` starts the host relay on container loopback port 8765, accessible
from the guest at `http://10.0.2.2:8765`. Set `VISTA_SIDEBAR=0` to disable it.
The launcher checks relay health before starting QEMU. Updating the guest is a
separate operation; existing pages and backend scripts are backed up first.

```sh
python3 tools/verify_vista_sidebar_gadgets.py --source
python3 tools/update_vista_sidebar_gadgets.py --user 'DOMAIN\username'
python3 tools/verify_vista_sidebar_gadgets.py --guest
```

For a nondefault VM, pass `--socket /path/to/control.sock` to the updater and
verifier, and `--vm-name NAME` to the guest verifier. `--no-restart` installs
providers without requiring an account name. No stock Sidebar HTML or Microsoft
DLL is shipped here. See [Sidebar provider details](VISTA_SIDEBAR_GADGETS.md)
and [desktop control](VISTA_CONTROL.md).

## Validation and investigation records

`python3 scripts/test_public.py` includes offline Sidebar provider tests and
Supermium source/toolchain and registry-policy tests. These checks do not
contact upstream services, change a guest or prove runtime compatibility.

The dated investigation reports retain their original results and references.
Their `.unlazy/`, VM and machine-specific paths describe development artifacts
that are not shipped. Use the public workflows above for a new checkout.
`patches/dxvk-deferred-resource-tracking.patch` and
`patches/dxvk-shared-clear-flush.patch` are retained investigation patches;
the bootstrap patch series is authoritative. Do not apply archival patches on
top of the curated backend automatically.
