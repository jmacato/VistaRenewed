# Native Vista MSHTML-to-CEF feasibility experiment

> Development record retained with the public desktop sources. Local capture,
> VM and `.unlazy/` paths refer to non-shipped development artifacts. See
> [current public entry points and status](DESKTOP-MODULES.md).


## Decision

Wine's `mshtml` is useful reference and possible LGPL-licensed source material
for a compatibility facade. It is not a replaceable Vista browser engine and
cannot be copied into `System32`. A viable experiment is a new, private COM
server that presents a deliberately small MSHTML-like surface over a
CEF/Supermium-derived renderer. The first server uses its own CLSID and test
host; it does not take over the stock HTML document CLSID.

Do not replace `C:\Windows\System32\mshtml.dll`, `ieframe.dll`, `urlmon.dll`,
or `iexplore.exe` in any prototype. Explorer, Windows Sidebar, and
SearchProtocolHost use the stock IE components in this guest. Maintaining the
original registrations is also the direct rollback path.

## What Wine mshtml provides

Wine's `dlls/mshtml` exposes the Windows DLL entry points and class-factory
shape expected by MSHTML clients. Its source implements a large selection of
`IHTMLDocument*`, element, collection, window, event, persistence, OLE, and
script-host interfaces. Its `htmlwindow.c` alone services `IHTMLWindow2`
through `IHTMLWindow7`, `IDispatchEx`, `IServiceProvider`, and event-related
interfaces. That is valuable behavioral documentation for an adapter and can
reduce boilerplate in a separately maintained facade.

The code is LGPL-2.1-or-later. A product that copies or modifies it must retain
its notices and satisfy the corresponding source/relinking obligations; this
experiment does not remove that obligation.

## Why it cannot be dropped into Vista

Wine's MSHTML is not native IE source. Its source list contains more than fifty
modules. Its Wine XPCOM/Gecko boundary backs the renderer-facing code.
`nsiface.idl` explicitly says that its interfaces are XPCOM, not
Microsoft COM. The implementation uses Wine private headers, debug/runtime
helpers, Gecko cycle collection, and synchronous local DOM wrappers. A Wine
build therefore expects Wine's surrounding DLL/runtime behavior even when many
of its imported Windows API names look familiar.

CEF exposes a Chromium browser and renderer-process boundary instead. Its DOM
and V8 APIs are process- and thread-scoped; they do not provide durable,
in-process `IHTMLDocument2` or `IHTMLWindow2` objects. Replacing Wine's Gecko
calls with CEF calls means designing a message bridge, object identity table,
marshalling, navigation lifetime rules, event delivery, and JavaScript/COM
interop. That is a rewrite of the renderer boundary, not a renderer swap.

Native IE consumers also rely on interfaces beyond the document object:
`IWebBrowser2`, OLE activation (`IOleObject`, `IOleInPlaceObject`,
`IViewObject2`), persistence (`IPersistMoniker`), connection points,
security-zone services, MSHTML DISPIDs, and often undocumented IEFrame
behavior. A renderer that merely draws HTML cannot replace those contracts.

## Explicit non-goal: ActiveX and browser plug-ins

**ActiveX is out of scope for this experiment.** The private facade will not
implement or proxy ActiveX controls, `OBJECT`/`EMBED` activation, `classid`,
`IObjectSafety`, `IServiceProvider` control discovery, COM-to-JavaScript
`ActiveXObject`, or any legacy browser plug-in mechanism. There is no fallback
to stock IE for an embedded control.

The private automation object exposes a diagnostic-only `activeXPolicy`
property which returns `unsupported-stubbed`. That capability marker lets the
test host prove the limitation deliberately, rather than accidentally making a
partial compatibility claim. Unknown ActiveX-related automation names are
rejected with the normal `IDispatch` unknown-member result. Markup may be
serialized as inert DOM text by the diagnostic renderer, but is never activated
or given a COM bridge.

## Safe implementation seam

Build a new x64 in-process COM server, provisionally named `mshtml_cef.dll`,
with a private CLSID such as `CLSID_TritonCefHtmlDocument`. A native test host
creates that CLSID directly. The server owns a CEF browser instance and exposes
only the interfaces required by that host, beginning with:

1. `IUnknown` and `IDispatch` for identity and automation.
2. `IPersistMoniker` or an explicit `Navigate` test method for URL loading.
3. A minimal `IHTMLDocument2` subset: `open`, `write`, `close`, `get_URL`, and
   `get_readyState`.
4. Completion callbacks that prove navigation, script execution, and teardown.

Do not register the private server under `CLSID_HTMLDocument`, alter
`htmlfile`, or override the system `mshtml` COM registrations in the first
prototype. The only acceptable first integration is a purpose-built test
executable. A later host-specific adapter can decide whether it needs a narrow
`IWebBrowser2` subset; system IEFrame integration is a separate, much larger
decision.

Wine code may be evaluated selectively after this facade has a written COM
contract. The useful candidates are its class-factory, automation, MSHTML
interface, and OLE glue patterns. Its Gecko `moz*`/XPCOM wrappers must be
replaced, not linked into the native Vista DLL.

## Live `winvista-3` target evidence

The supplied development disk was booted through a new external overlay, with
the stock `mshtml.dll` left in place. An elevated OOBE console installed the
existing `TritonVistaControl` LocalSystem COM2 service from the attached
development media. The target probe ran through that service, not through a
host-side parser, so the results below describe the guest's actual COM
objects.

This disk intentionally targets **IE7** for the current prototype. It reports
`mshtml.dll`, `iexplore.exe`, and Internet Explorer `Version`/`W2KVersion` as
`7.0.6002.18005`. All measured interface and automation requirements in this
record therefore define the IE7/Vista compatibility contract. Do not infer IE9
coverage until the same probe is run on the separate IE9 VM.

The registered Microsoft HTML Object Library is
`C:\Windows\System32\mshtml.tlb` (type-library GUID
`{3050F1C5-98B5-11CF-BB82-00AA00BDCE0B}`, version 4.0). It contains 738 type
records: 366 dispatch interfaces, 71 vtable interfaces, 125 coclasses, 143
enums, 23 records, 7 aliases, and 3 unions. Loading `mshtml.dll` itself as a
typelib returns `0x80029C4A`; that is expected for this registration layout.

The live `CLSID_HTMLDocument` object supports `IHTMLDocument2` through
`IHTMLDocument5`, `IDispatch`, `IPersist`, `IPersistStreamInit`,
`IPersistMoniker`, `IOleObject`, `IOleInPlaceObject`, `IViewObject`,
`IViewObject2`, `IServiceProvider`, and `IConnectionPointContainer`.
`IHTMLDocument6` and `IHTMLDocument7` return `E_NOINTERFACE`.

After an in-memory `write`/`close` succeeds, the object graph exposes:

- `DispHTMLDocument`: 203 automation members;
- `IHTMLWindow2` through `IHTMLWindow5` (85 live automation members), but not
  `IHTMLWindow6` or `IHTMLWindow7`;
- `IHTMLLocation`: 27 live automation members;
- `IHTMLElement` through `IHTMLElement4`, plus `IHTMLDOMNode` and
  `IHTMLDOMNode2`, but not their later revisions; and
- a `DispHTMLBody` object with 301 automation members.

The document and window connection points include the legacy and revision-2
HTML document/window event interfaces; the body additionally exposes the
text-container event interfaces. The document's `IServiceProvider` exists but
`QueryService(SID_SHTMLWindow, IID_IHTMLWindow2)` returns `E_NOINTERFACE` on
this unhosted test object. A bridge must therefore obtain the window by the
document contract (`get_parentWindow`), not assume every service query works.

The generated evidence is retained locally as
`build/winvista-3-mshtml-interface-dump-v3.txt`; the smaller live-object view
is `build/winvista-3-mshtml-interface-dump-v2.txt`. The v3 file provides all
member names/DISPIDs from the registered typelib, while v2 proves which
interfaces and object relationships the target actually constructs.

## Concrete CEF bridge boundary

The first real bridge should be a private, test-host-only server with four
identity-stable proxy families: `DocumentProxy`, `WindowProxy`,
`ElementProxy`, and `LocationProxy`. Each proxy owns a CEF browser/frame or a
renderer-side node token; it does not try to hand a CEF DOM pointer to COM.
The browser-process COM apartment sends all DOM/V8 work across a sequenced IPC
bridge and resolves callbacks back on that apartment.

Implement in this order:

1. `IUnknown`, `IDispatch`, and the document automation map with the measured
   DISPIDs for `open`, `write`, `writeln`, `close`, `get_URL`, `get_readyState`,
   `get_body`, `get_location`, `get_parentWindow`, `getElementById`, and
   `getElementsByTagName`.
2. `WindowProxy` operations that CEF can map directly: navigation/location,
   timers, `execScript`/script evaluation, and a small event dispatch path.
3. Element identity, `innerHTML`/`outerHTML`, tag lookup, and the DOM-node
   subset needed by the test host. DOM updates must preserve COM identity
   across CEF IPC round trips and invalidate it on navigation or document
   teardown.
4. Connection-point sinks and the measured document/window/text-container
   event IIDs. `attachEvent`/`detachEvent` alone is not a sufficient event
   compatibility layer.
5. Only after host tests demand them, OLE site/activation, persistence,
   security-zone, and `IWebBrowser2` behavior.

An object must never advertise an `IHTMLDocument*`, `IHTMLWindow*`, or
`IHTMLElement*` interface unless it supplies that interface's complete binary
vtable and documented lifetime behavior. The private first host may accept
`E_NOTIMPL` for deliberately deferred members, but it must not let production
Vista components bind to that partial implementation.

## IE7 private bridge vertical slice

The first executable slice is now deployed only on the disposable IE7 overlay:
`C:\TritonCefBridge\triton-ie7-cef-bridge.dll` is registered under the private
`CLSID_TritonCefHtmlDocument` (`{A9E4D11A-3F5C-4E8A-9C77-93712BC64802}`) and
the private `Triton.CefHtmlDocument.1` ProgID. It does not alter the stock
`mshtml.dll`, any stock CLSID, `htmlfile`, or IEFrame registration.

The DLL deliberately exposes only `IUnknown`/`IDispatch`, rather than falsely
advertising a partial `IHTMLDocument2` vtable. Its late-bound map uses measured
IE7 DISPIDs for `open` (1056), `write` (1054), `writeln` (1055), `close`
(1057), `url` (1025), `readyState` (1018), `title` (1012), `body` (1004), and
`getElementById` (1088). Its private body proxy implements `innerHTML` and
`outerHTML` using the corresponding IE7 DISPIDs. This is IE7-only scope; later
interfaces remain absent by design.

Its renderer is currently `mock-cef-adapter`: a reference-counted in-memory
adapter that owns URL, title, HTML, and ready-state state. It is **not a CEF runtime** and does not draw or execute Chromium content. The seam is real,
however: the COM facade invokes no renderer-specific APIs, so a future adapter
can replace the mock operations with sequenced CEF browser/frame IPC while
preserving COM-apartment identity and lifetime rules.

The target test host creates the private CLSID through `CoCreateInstance`,
uses only late-bound IE7 names, and proved this transcript under LocalSystem:

```text
BRIDGE_BACKEND=mock-cef-adapter
BRIDGE_READY_STATE=complete
BRIDGE_URL=about:blank
BRIDGE_TITLE=IE7 CEF bridge
BRIDGE_HTML=<p>alpha</p><p>beta</p>
IE7 CEF BRIDGE HOST VERIFIED
```

The result is saved as `build/winvista-3-ie7-cef-bridge-host.txt`. Roll back
the registration with `regsvr32 /u /s
C:\TritonCefBridge\triton-ie7-cef-bridge.dll`, or simply discard the active
external overlay. Neither path modifies the backing `winvista-3.qcow2`.

## Supermium 144 renderer path

Supermium is the better engine donor for this Vista target. The pinned
portable x64 release is **Supermium 144.0.7559.256 R5** (`v144-r5`), a current
Chromium 144 fork whose project explicitly lists Vista support. Its official
`supermium_144_64_nonsetup.zip` release asset is held locally with SHA-256
`805232e5cde1bf6971748bc7fb6a2cb09fdfce9ceb91062a1814b228139956ca`.
The complete portable `Supermium/` tree is staged unchanged on a read-only ISO
with no installer and no IE binary. It has not been installed into the guest.

It must remain **out-of-process**. Supermium's `chrome.exe` and versioned
`chrome.dll` are a browser product, not a CEF embedding ABI; loading either
into `mshtml.dll` would combine incompatible process, allocator, sandbox,
thread, and lifetime ownership. The safe next implementation is a private IE7 COM facade
that launches and brokers to Supermium. The facade owns the measured
IE7 automation proxies, while a dedicated backend process owns Chromium's DOM
and renderer processes. A versioned local RPC protocol (or a tightly scoped
DevTools Protocol transport during the prototype) carries navigation,
`document.write`, DOM query, script, event, and teardown messages across that
boundary.

The immediate smoke medium launches `D:\smoke-supermium.cmd`: it records the
pinned product version and uses headless Chromium to serialize a fixed `data:` document containing
`TRITON_SUPERMIUM_144_DOM_OK`. This is intentionally a loader-and-renderer
test before changing the private bridge adapter. It uses `--no-sandbox` and
`--disable-gpu` only for the LocalSystem/session-0 diagnostic; that mode is not
a ship target. Later tests must run the normal sandboxed process tree, inspect
all child imports, exercise `chrome://gpu`, script round trips, resize/focus,
and teardown under the interactive user.

CEF branch `7559` remains a useful comparison point because it tracks the same
Chromium generation, but upstream CEF does not include Supermium's Vista port.
A true in-process CEF path would still require a reproducible source merge and
a dedicated Chromium build environment. The current MinGW toolchain is
suitable for the small COM facade and broker, not for building Chromium.

### Target evidence

The portable runtime has now executed on the disposable Vista x64 target. Its
headless smoke transcript (`build/winvista-3-supermium-smoke.txt`) contains
`TRITON_SUPERMIUM_144_DOM_OK` and zero exit codes for Chromium's DOM dump and
the marker check. A separate interactive-user launch displayed the local
renderer page in the guest; the framebuffer capture is
`build/winvista-3-supermium-visible.png`. It visibly reports Chromium
`144.0.7559.256 R5` and that the private IE7 bridge remains separate from stock
`mshtml.dll`.

The captured launch did report failed GPU shared-context creation under the
virtualized D3D path. The smoke is deliberately `--disable-gpu`, so this is an
unresolved accelerated-compositing issue rather than a claim of GPU success.
The native licensing query separately reports Vista Notification mode because
its grace period expired. No activation bypass was run; that needs a legitimate
Vista license to change.

### opt-in private adapter

The deployed private DLL's **default mock adapter** remains the in-memory IE7 facade.
It selects the diagnostic `supermium-144-headless-adapter` only when its private
test host explicitly supplies `TRITON_SUPERMIUM_EXE` pointing at the staged
`chrome.exe`. On `document.close`, that adapter serializes a temporary HTML file
through `chrome.exe --headless --dump-dom` and gives the resulting UTF-8 DOM
back to the private body proxy. This is intentionally bounded to 45 seconds and
one MiB of DOM output.

The adapter has `--no-sandbox` and `--disable-gpu` because it runs as
LocalSystem during the first bridge test; it is **not a ship target**. It does
not host Supermium inside the DLL, change the real stock `mshtml.dll`, or claim
that its narrow `IDispatch` surface can replace IE. The next production-shaped
step remains a user-session broker with a normal sandboxed Supermium process
and a versioned IPC protocol.

The target proof is `build/winvista-3-ie7-supermium-bridge-host.txt`. It was
run as LocalSystem against the private
`C:\TritonSupermiumBridge\triton-ie7-cef-bridge.dll` registration and records:

```text
BRIDGE_BACKEND=supermium-144-headless-adapter
BRIDGE_READY_STATE=complete
BRIDGE_HTML=<html><head></head><body><p>alpha</p><p>beta</p>...</body></html>
IE7 SUPERMIUM ADAPTER HOST VERIFIED
```

That is real Chromium DOM normalization behind the private IE7 late-bound
facade. It is still a process-per-`close` diagnostic, not a usable browser
embedding or a claim of IE/ActiveX compatibility. Its `activeXPolicy`
diagnostic returns `unsupported-stubbed`; no ActiveX control or plug-in
integration is planned.

## IE7 process bring-up: measured replacement seam

The actual Vista browser target is the **32-bit** executable
`C:\Program Files (x86)\Internet Explorer\iexplore.exe`. Its normal process
loads `C:\Windows\SysWOW64\mshtml.dll`; the machine-wide 32-bit
`CLSID_HTMLDocument` registration also points there. A temporary per-user
`HKCU\Software\Classes` shadow is sufficient to make `iexplore.exe` activate a
private x86 facade instead. The exercise removes that shadow before returning
and proves the merged `HKCR` registration has returned to the original
SysWOW64 path. It never changes the protected machine-wide class registration
or a system DLL.

The x86 facade is an ABI bring-up probe rather than a finished browser engine.
It is built from the Vista-compatible MinGW `IHTMLDocument2Vtbl` declaration:
all 116 entries have exact x86 calling conventions, so an unsupported call
returns `E_NOTIMPL` safely instead of invoking a partial vtable. IE7 has
already driven this facade through `IPersist`, `IPersistMoniker`,
`IPersistFile`, `IMonikerProp`, `IOleObject`, `IViewObject`, `IDispatch`,
`IServiceProvider`, `IOleCommandTarget`, and `IHTMLDocument2`. The first
implemented document behavior is `IDispatch::Invoke(DISPID_READYSTATE)`, which
returns `complete`.

On `IPersistMoniker::Load`, the facade obtains IE's URL and starts the pinned
portable Supermium process out-of-process under the interactive user. The
target transcript confirms both `SUPERMIUM_LAUNCH_SUCCEEDED` and the expected
`chrome.exe` process tree while stock `mshtml.dll` is absent from the shimmed
IE process. The launch uses a dedicated bridge profile and normal sandboxed
Supermium process tree; GPU compositing is disabled for this virtual target.

The facade now also performs the documented in-place activation handshake.
IE's `IOleObject::DoVerb(OLEIVERB_INPLACEACTIVATE)` supplies an
`IOleInPlaceSite`; the facade gets IE's window context, registers its
`IOleInPlaceActiveObject`, creates an `IOleDocumentView`, sets its rectangle,
shows it, and completes UI activation. The live target records
`IOLEOBJECT_INPLACE_ACTIVATED`, `IOLEDOCUMENT_CREATEVIEW_SUCCEEDED`, and
`IOLEDOCUMENTVIEW_SHOW=1`. IE supplies a zero-sized initial view on this
target, so the bridge selects the nearest nonzero IE ancestor instead; the
recorded 800-by-550 client area becomes the child viewport.

The same facade tracks the launched Supermium browser PID, discovers its native
top-level window, converts it to `WS_CHILD`, parents it to that IE host, and
resizes it to the negotiated viewport. It launches Supermium with `--app=<URL>`
so the embedded native window contains the renderer surface rather than the
full Supermium tab strip and omnibox. Its non-client app frame is clipped and
offset above the child viewport on every layout, leaving a renderer-only surface
inside IE. The capture
`build/winvista-3-iexplore-supermium-embedded.png` shows a live Supermium 144
page rendering inside the real IE window frame. This is a deliberately narrow
**HWND bridge**, not CEF embedding or a DOM-compatible `mshtml` replacement:
`IHTMLDocument2.get_URL` and `put_URL` now track the document URL and relaunch
the app-mode child for a requested URL, but IE navigation completion,
automation/event forwarding, broader `IHTMLDocument2` DOM methods, graceful
renderer lifecycle, and a versioned Supermium IPC broker still need real
implementations before IE can be called functional. ActiveX remains unsupported
throughout this path.

The viewport correction is captured in `build/ie-viewport.png`: IE's address
bar, tabs, and status bar remain visible around the Supermium page. A dedicated
`TritonMshtmlViewport` child follows the actual document host rectangle after
its initial zero-size layout. The earlier direct IEFrame attachment covered
the browser controls and was insufficient. Resize and keyboard behavior still
need interactive end-to-end verification; source markers alone do not prove
those behaviors or URL navigation.

The authoritative capture is `build/winvista-3-iexplore-shim.txt`; it is the
current interface-by-interface implementation order, not a claim that IE has
already been replaced.

COM lifetime now counts live factories, documents, and server locks.
`DllCanUnloadNow` refuses unloading while these remain and unregisters the
viewport class before allowing unload. `CreateInstance` also supports direct
requests for implemented document interfaces. The native Vista exercise in
`tools/ie7_mshtml_lifetime_test.c` verifies direct `IHTMLDocument2` creation,
initial URL retrieval, and unloading before/during/after object ownership.
It does not establish navigation or full DOM compatibility.
The same native test now checks name lookup and property reads through both
`IDispatch` and the dispatch portion of `IHTMLDocument2`. URL and readyState
reads share implementations, including the MSHTML document readyState DISPID
and the control readyState DISPID used by IE. Late-bound URL writes now validate
the property-put named argument, argument count, and BSTR type before launching
Supermium. The native Vista test writes `about:blank#automation` and reads it
back through `IHTMLDocument2`, and rejects missing/wrong-type arguments.
This tests dispatch and launch, not visible page-to-page navigation or
renderer-derived loading state; those remain outstanding.
Repeated navigation testing exposes Chromium's shared-profile handoff: the
second launcher exits with code 0 while the original browser owns the new
window. The shim now rejects that unresolved handoff without committing its
URL or replacing its tracked PID with the exited launcher. The guest test
reproduces this limitation. A persistent browser transport and window/target
association are required to complete repeated navigation.

The native `tools/supermium_pipe_probe.c` now demonstrates a DevTools pipe
round trip on Vista. It launches the pinned runtime with inherited anonymous
pipe handles using `--remote-debugging-pipe` and
`--remote-debugging-io-pipes=<read>,<write>`, sends a NUL-terminated
`Browser.getVersion` request, and receives `Chrome/144.0.7559.256` with protocol
version `1.3`. It then requests `Browser.close`. This prototype uses a separate
profile and has no debugging TCP listener. It is not yet integrated into the
MSHTML document; target/session discovery, navigation acknowledgments, and
DOM/event forwarding remain required.
The handle convention follows Chromium's
[DevTools agent host implementation](https://chromium.googlesource.com/chromium/src/+/refs/tags/134.0.6998.1/content/browser/devtools/devtools_agent_host_impl.cc).

The probe also creates a page target, attaches a flattened session, sends
`Page.navigate` to a data document, and reads `document.title` using
`Runtime.evaluate`. The Vista transcript `build/supermium-pipe-probe.txt`
records the navigation's matching frame/target ID and the renderer-returned
title `PIPE_NAVIGATION_OK`, then acknowledges `Browser.close`. The framing
loop consumes unsolicited NUL-terminated messages until the matching request
ID arrives. This establishes real page navigation and a DOM read in one
persistent browser; it still creates a separate probe page rather than
controlling the shim's embedded app page. The small probe string extraction
must be replaced with structured JSON parsing for general DOM traffic.

The latest probe no longer creates an extra target. It launches the same
`file:///D:/visible-supermium.html` app page used by IE, discovers that URL
through `Target.getTargets`, and requires a single unambiguous target before
attaching. The captured navigation frame ID matches the discovered app target,
and evaluation returns `PIPE_NAVIGATION_OK` in that session. An earlier
`about:blank` startup selected `chrome://newtab/`; the explicit URL check
prevents that false association. This remains a transport probe pending
integration with the embedded document object's lifetime and COM calls.

The shim now owns read/write pipe handles per document. Its extended process
startup explicitly inherits only the two Chromium pipe endpoints. Initial
loading verifies `Browser.getVersion` through those handles before committing
the URL. The real IE transcript records `MSHTML_DOCUMENT_PIPE_VERIFIED` and
successful child attachment in the same run. Document destruction closes its
endpoints. Session discovery and subsequent URL writes still need routing over
this document-owned connection; the existing relaunch path remains incomplete.

The current source now routes subsequent `put_URL` requests through
`Target.getTargets`, a flattened page session, and `Page.navigate` over the
document pipe. URLs are JSON-escaped as UTF-16 code units; ambiguous target
sets and protocol errors are rejected. The host build passes, but deployment
of this revision timed out on the serial controller. Its updated two-navigation
guest test is therefore pending; do not treat this source change as verified
embedded navigation until that test and an IE exercise have run successfully.

Deployment subsequently recovered. The native Vista COM test now passes its
second URL change over the existing pipe. Target discovery initially rejected
one page plus an iframe; it now selects the sole page object and ignores iframe
targets. The test records `MSHTML PERSISTENT SECOND NAVIGATION PASSED`, and
the shim logs session attachment and navigation acknowledgment without a second
launch. This verifies the COM-to-CDP request path, not visual navigation inside
IE or renderer-derived completion. General JSON parsing remains required.

## Disposable deployment plan

The active target now writes to `winvista-3-mshtml-cef-dev.qcow2`, backed
directly by the supplied `winvista-3.qcow2`. The backing disk is not opened for
writes. The earlier `vista-kvm/x64-base/cef-mshtml-experiment.qcow2` remains a
separate preserved overlay for the original IE9-capable VM; it is not the
current target.

For an experiment reboot, first shut Vista down normally. Restart only with:

```sh
VISTA_DISK="$PWD/winvista-3-mshtml-cef-dev.qcow2" \
VISTA_ISO="$PWD/build/mshtml-cef-media.iso" \
./run-vm.sh
```

Without `VISTA_DISK`, the launcher intentionally uses the original
`work.qcow2` and bypasses this target. Do not run a second VM against either
image while the current one is active. CEF's runtime is larger than the
guest-control service's 1 MiB file-transfer limit, so stage it on a dedicated
ISO. Rollback means powering down the experimental guest and restarting
without `VISTA_DISK`; do not merge either overlay into its backing disk.

## Latest embedded-host verification (2026-09-12)

### Durable Protected Mode launch policy

Install the executable-specific launch policy from the **host workspace**:

```sh
python3 tools/vista_supermium_policy.py install
python3 tools/vista_supermium_policy.py status
# Explicit rollback when the development installation is no longer wanted:
python3 tools/vista_supermium_policy.py uninstall
```

The script uses the existing private SYSTEM control service. It installs the
32-bit registry entry below, for the 32-bit IE7 host even though Supermium is
64-bit:

```text
HKLM\Software\Microsoft\Internet Explorer\Low Rights\ElevationPolicy\{08A1D321-9C62-4FC8-84EF-7A5F8BF3C147}
AppName           REG_SZ     chrome.exe
AppPath           REG_SZ     C:\TritonSupermium
Policy            REG_DWORD  1
TritonPolicyOwner REG_SZ     supermium-low-integrity-v1
```

Policy 1 permits silent low-integrity launch; policy 3 would instead permit
medium-integrity launch and is deliberately not used. Protected Mode, security
Zones, and certificate validation are unchanged. This narrowly scoped policy
is persistent registry configuration, not a per-run exemption. Install and
uninstall are idempotent, with exact read-back verification. Existing foreign,
unmarked, or changed entries (including subkeys) are refused, not overwritten
or recursively removed. Partial writes are rolled back only if their contents
still match this script's owned values. An interrupted install with unexpected
state requires inspection rather than automatic deletion. The private policy
lock serializes installer and exercise workflows.

Install, status, and the IE exercise also check the interactive user's
remembered launch rules. A matching per-user rule with a policy other than 1
is a conflict: the command fails and leaves that preference unchanged. A
machine entry alone is not reported as a clean installation in that case.
This mattered in the current VM: the remembered HKCU rule
`{C1545DBB-A718-4A0A-90E0-B1A9A4EFFB7E}` for the exact same chrome.exe had
Policy 3. The durable machine entry was installed, but the first fresh IE check
did not launch Chromium (`build/supermium-durable-policy-ie7.txt` and PNG).
With explicit user approval, only this rule's Policy value was changed from
3 to 1 (guest job `7192e6c9ed284112943f7f13e4d39980`). The original export is
`C:\TritonSupermiumBridge\supermium-user-policy-before-20260912.reg`, also
copied to `build/supermium-user-policy-before-20260912.reg`; SHA256
`44988674fe9467e29c15a6797e05515b6c8f23b5d09903e64e3091672cab2b88`.
The matching `after` export differs only in that DWORD. This is the
TritonBrowser user's rule, not the SYSTEM account's HKCU. The installer still
refuses future conflicting preferences rather than changing them silently.
Uninstall removes only its owned machine entry; restoring this separately
approved user preference requires importing the original export as
TritonBrowser (which deliberately restores its old medium-integrity policy).

`tools/vista_low_launch_exercise.py` now borrows a verified durable entry and
leaves it installed, reporting `LOW_POLICY_PRESERVED_AND_ZONES_UNCHANGED`.
Without an installed policy it retains the temporary install/cleanup behavior.
Conflicting entries fail closed. This corrects the old harness removing the
launch policy after 30 seconds while leaving its IE window open: later browser
launches could prompt to leave Protected Mode. After installing, cancel any
already-open elevation dialog and restart the development IE instance so a
launch does not continue using previously cached policy.

This is **only the Supermium launch-policy installer**. It does not permanently
register the experimental HTMLDocument shim or fix native address-bar routing.
The earlier five element gates covered DOM operations and initial IE rendering,
not normal repeated address-bar browsing. The reported Google navigation hit
IE's `invalidcert.htm` path before the shim received the Google URL, then an
unimplemented file-load fallback returned S_OK without content. That separate
navigation/error-handling defect remains open; suppressing the elevation
prompt must not be described as fixing it.

### Element and document verification history

Element automation now exposes real Chromium nodes through
`IHTMLDocument2::get_body`/`createElement` and
`IHTMLDocument3::get_documentElement`/`getElementById`, including their
late-bound equivalents. The new `IHTMLElement` facade implements
innerHTML/outerHTML, innerText/outerText, id, className, title, tagName,
parentElement, and document. The exact toolchain vtables are generated
(94 IHTMLElement slots, 48 IHTMLDocument3 slots); unsupported members return
E_NOTIMPL, and unsupported extended element interfaces return E_NOINTERFACE.

Each element retains its actual remote node and its owning COM document.
The document keeps a weak wrapper list, so repeated lookups preserve COM
identity without creating a reference cycle. Backend node IDs are only
lookup hints: the bridge confirms JavaScript object equality before reusing
a wrapper. Detached/replaced nodes keep their own contents, same-document
navigation preserves identity, and old handles cannot silently address a
new document after navigation or Close/reopen. Input strings are escaped
protocol arguments, not interpolated JavaScript. DOM.describeNode replies
use the bounded large-response buffer because depth-zero descriptions still
include attributes; a 20,000-character className regression exercises this.
Both Runtime and DOM reply bodies are excluded from raw diagnostic logs.

Native element test `tools/ie7_mshtml_element_test.c` passes against the
deployed DLL, including COM identity, live Unicode/text/HTML mutation,
detached-node ownership, large-attribute lookup, stale-handle isolation,
and final unload after the last element reference is released. Guest job
`a0875cff6a9842fead0528579c968daa` and
`build/mshtml-elements-large-attribute.txt` retain this run. Current DLL
SHA256: `a0e80f2617fc730ea4d58dfe00b305a8cb3de3a8414c879dc3dac6e9099bb5c1`;
native test SHA256:
`6b926cd9249a5e6e16eba23a5d5a6b86b67504df21f3548ec88fd09b227bf2b9`.

The actual guest Windows Script Host also traverses and mutates these
objects using the extended `tests/fixtures/mshtml-content.js`. This caught
and fixed an automation detail that the typed test did not: JScript requests
METHOD|PROPERTYGET for a method used as an expression. The fixed run
`688bb547de3f42c3b8ea754b4d9d3b2a` reports both element and content success,
and the harness verifies restoration of stock activation afterwards
(`build/mshtml-elements-wsh-fixed.txt`).

All four executable element-step gates were then reverified on the current
DLL: native elements, Windows Script Host, document/stream regression, and
native view/keyboard regression. Final native IE HTTPS job
`1190a5ab2d6b4c6da61770b4e9a37cb8` is retained in
`build/mshtml-elements-final-https-ie7.txt` and its matching inspected PNG.
Responsive IE PID 3608 embeds Chrome PID 5048 under TritonMshtmlViewport;
the pipe reports Chrome/144.0.7559.256. The visible frame and tab say Example
Domain, the native address bar shows https://example.com/, and Protected
Mode remains On. Stock HTMLDocument activation was restored and the
temporary launch policy removed with the security Zones unchanged. The
five element-step gates are met, with zero unmet or abandoned; the broader
Windows/MSHTML replacement remains incomplete and activation is still
exercise-scoped. The native IE demo is left visible.

An earlier native run failed once while reopening the browser after Close
(`ac300ac96e254951b5f5ce29280c389d`,
`build/mshtml-elements-first.txt` and `build/mshtml-elements-first-log.txt`).
Subsequent runs passed; the underlying intermittent startup/pipe failure
has not been diagnosed. Navigation now retains/logs the failing HRESULT
instead of always collapsing it to E_FAIL. This is diagnostic improvement,
not evidence that the intermittent failure has been fixed.

Collections, IHTMLDOMNode, attribute APIs, window/location objects, events,
and broad Windows-host compatibility remain unfinished. ActiveX controls
remain explicitly unsupported. These are selected element operations, not
a complete implementation of MSHTML's element interfaces.

Document content/Windows automation bring-up now uses the actual Chromium DOM
for `IHTMLDocument2::write`, `writeln`, `close`, and title get/put. The same
operations are available through `IDispatch`; writes handle SAFEARRAY variant
conversion and nonzero lower bounds, and late-bound variadic arguments retain
their original order. Document title changes are delivered to the native OLE
container with `OLECMDID_SETTITLE`; IE's tab and frame now show Example Domain
instead of only its URL. Polling remains bounded to once per second per view.

`IPersistStreamInit` now supports InitNew, Load, Save, GetSizeMax, and a
serialized-DOM IsDirty comparison. Load accepts UTF-8 (optional BOM) and
BOM-marked UTF-16 LE/BE; malformed UTF-8 and odd-length UTF-16 fail instead of
silently changing encoding. Save writes UTF-8 with a BOM and serializes the
current DOM/doctype, not a retained copy of the input. Legacy code-page and
HTML meta-charset sniffing remain unimplemented. Input/output DOM strings
have an explicit 1,048,576 UTF-16-unit limit; protocol buffers are bounded
and return errors on overflow rather than truncating. IsDirty currently
tracks serialized markup, not every form-control/editor state change.

The generic IPersistStreamInit docs say Load after InitNew is unexpected,
but the **actual Vista HTMLDocument** returned S_OK for InitNew, repeated
InitNew, and Load after InitNew in read-only baseline job
`a8a749fe2ce64bdb91a00d0ec2d7a0cf`
(`build/mshtml-stock-stream-contract.txt`). This adapter follows the measured
MSHTML behavior. No ActiveX control support was added.

The content bridge obtains a temporary remote Document reference for each
call and releases it afterwards. It passes BSTR data as escaped protocol
arguments, never splices that data into a JavaScript function body. The
transport now buffers pipe reads and permits larger bounded requests; DOM
return values are deliberately excluded from raw diagnostic logging. Before
initial content writes it waits for about:blank to commit, so it does not
write into the file-origin app bootstrap while navigation is pending.

Native test `tools/ie7_mshtml_content_test.c` verifies a 100,000-unit Unicode
input plus script, modern JavaScript execution, title automation including
quotes/surrogate pairs, large UTF-8 stream round-trip, UTF-16 LE loading,
invalid encoding/argument rejection, separate document contents, COM identity,
and final DLL unload. Existing view/keyboard and lifetime/command-isolation
regressions were rerun successfully. DLL SHA256 for that content-only step:
`a1afdb5ea34bc937ddc4a8fdc42e6b52c9d5cca4aaebdc5d360c2ea14423ab9b`.

An independent Windows consumer also passes: the guest's 32-bit `cscript.exe`
creates the `htmlfile` COM object, calls write/writeln/close, checks the actual
renderer user agent's Chrome/144 major version, and round-trips a Unicode
title. The WSH script uses COM activation outside the web renderer; this is
not an ActiveXObject bridge exposed to web pages. Job
`7957beda62124f1b8a6ef68b717e1487` and
`build/mshtml-windows-script-host.txt` retain the evidence. The host harness
refuses existing overrides and verifies exact restoration of stock activation.
Running the same script against restored stock MSHTML does **not** emit the
success marker and reports a renderer-script failure, retained in
`build/mshtml-windows-script-stock-negative.txt`; exit status alone is not
the oracle. The Windows Script Host check was also rerun by the content ledger.

Final native IE evidence for this DLL is guest job
`3a3c6deb757f461c9d7aedbbc8295f74`,
`build/mshtml-content-final-https-ie7.txt` and matching viewed PNG: IE PID 4208
embeds Chrome PID 4204, retains HTTPS rendering and Protected Mode On, and
shows Example Domain in its native tab and title bar. The title command returns
S_OK. Activation and launch-policy restoration are verified. The content
ledger has five met checks (four executable, one inspected), zero unmet,
and zero abandoned; the larger replacement goal remains active. The current
development setup is still exercise-scoped, not a permanent system-wide
activation installer.

That content-only step still left element/window/location proxies, broad stream encoding,
DOM events, renderer-initiated navigation/history synchronization, and other
Windows consumers unfinished. The added methods are a real compatibility
step, not a claim that the remaining generated DOM stubs are implemented.
References: [Chromium Runtime protocol](https://chromedevtools.github.io/devtools-protocol/tot/Runtime/)
and [generic stream Load contract](https://learn.microsoft.com/en-us/windows/win32/api/ocidl/nf-ocidl-ipersiststreaminit-load).

Protected Mode/network bring-up now passes both controlled HTTP and public
HTTPS navigation through the native IE7 frame. DLL SHA256 for this step:
`db26053e68d094baef43772da112cd708b25c75b3d494994ff982fc5d9e2ae5f`.
The shim chooses a writable state directory from its actual token integrity:
low-integrity hosts use `FOLDERID_LocalAppDataLow\TritonSupermiumBridge` for
the app bootstrap, log, and profile; normal hosts retain the existing dev
directory. The executable stays at `C:\TritonSupermium\chrome.exe`.
Token-query failure does not guess a path or launch at another integrity.

Without an explicit launch policy, IE intercepts CreateProcess and prompts
to open Chrome outside Protected Mode. That prompt was **not accepted**.
`tools/vista_low_launch_exercise.py` instead temporarily installs the exact
executable's 32-bit IE Low Rights policy with `Policy=1` (low-integrity
launch), runs the per-user activation exercise, then removes its own policy
in `finally`. It refuses an existing test GUID and compares the complete
per-user security Zones query before and after. Policy writes use the
existing SYSTEM control service; IE runs as the interactive user. This is
an exercise harness, not a permanent activation installer. No zone setting,
certificate-ignore flag, or Chromium sandbox-disable flag is introduced.

Controlled HTTP evidence: guest job `917aaa1de5cd409f9c31f084fa5e9178`,
`build/mshtml-low-policy-http-ie7.txt` and matching viewed PNG. IE PID 3316
loads the shim and embeds browser PID 3972; the page displays its actual
Chrome/144 user agent and IE says Protected Mode On. Both processes have
integrity RID 4096; an independent query of Chrome's token is retained in
`build/mshtml-low-browser-token.txt`. The loopback-only, expiring fixture
`tools/vista_http_fixture.py` serves a single fixed page, not workspace files.

HTTPS evidence: guest job `67184fe011054074864fc322cc8306f7`,
`build/mshtml-low-policy-https-ie7.txt` and matching viewed PNG. Native IE
PID 4676 calls the shim's IPersistMoniker with `https://example.com/`, the
pipe identifies Chrome/144.0.7559.256, and browser PID 3328 renders Example
Domain under the IE-owned viewport. Both IE and Chrome have RID 4096;
`build/mshtml-https-browser-token.txt` records the independent browser token
query. Both exercises restore stock HTMLDocument activation and end with
`LOW_POLICY_REMOVED_AND_ZONES_UNCHANGED`. These are direct render/embedding
proofs, not proof of complete certificate, origin, history, or security-UI
compatibility: IE currently labels the HTTPS document's zone Unknown.
The guest document-view/keyboard lifecycle regression was rerun successfully
against this DLL after both network exercises. The focused network ledger
has four met gates (one executable regression and three inspected integration
evidence gates), zero unmet, and zero abandoned; that is not completion of
the larger IE/Windows replacement goal. The HTTPS demo is left visible in
the VM, with its temporary activation policy already removed.

This corrects the earlier inference below: missing logs in a non-writable
directory, and the presence of stock MSHTML, did not establish that the shim
had never activated. The improved probe separately reports shim modules and
token integrity; controlled HTTP demonstrated low-integrity shim activation
before the writable-state fix. No TLS or pre-document URLMon replacement was
needed to make the tested HTTPS URL render. Most DOM interfaces and broader
Windows consumers remain incomplete.

Platform references: [LocalLow known folder](https://learn.microsoft.com/en-us/windows/win32/shell/knownfolderid),
[Microsoft's IE7 Protected Mode overview](https://techcommunity.microsoft.com/blog/askperf/application-compatibility---ie7-protected-mode/372351/),
and the elevation-policy table in
[IBM's IE sandbox research](https://blackhat.com/docs/asia-14/materials/Yason/WP-Asia-14-Yason-Diving-Into-IE10s-Enhanced-Protected-Mode-Sandbox.pdf).

Keyboard routing: `tools/mshtml_keyboard_router.h` installs one dedicated
low-level keyboard hook thread per host process while viewports exist. It
only forwards a fixed set of container shortcuts when the foreground focus
is `Chrome_RenderWidgetHostHWND` under an owned, marked, visible viewport.
It posts down/up messages to the native host frame, suppresses repeat actions,
and performs no COM, pipe calls, synchronous message sends, or key logging in
the hook. AltGr/Windows-key combinations and normal text remain untouched.
The worker pins the DLL until shutdown, is reference-counted across views,
and is included in the unload check. Native address/search controls are also
explicitly excluded from IOleInPlaceActiveObject's key-forwarding path.

Final DLL SHA256:
`e3dc136463e4049023a5189a2bb3b57ddc01d72255b0aa30274c42aff7b2163b`.
Guest view test verifies one paired Ctrl+T delivery with its modifier state,
repeat suppression, ordinary-key exclusion, native-edit exclusion (including
direct TranslateAccelerator), view lifecycle and final DLL unload. The
document lifetime/command-isolation regression also passed. Live IE7 job
`4db484dd226d48e3a474c7fcd8c5768c` verified Ctrl+T opening a tab, Ctrl+Tab from
renderer focus returning to the first tab, preserved input then further
typing (`Received: hellox`), and F6 selecting IE's address bar.
Screenshots: `build/mshtml-keyboard-final-{newtab,text,f6}.png`.
The Windows hook constraints are documented in
[LowLevelKeyboardProc](https://learn.microsoft.com/en-us/windows/win32/winmsg/lowlevelkeyboardproc).

Startup now always uses a unique blank local-file app window and then sends
the requested URL over the existing pipe. This prevents `--app=about:blank`
from selecting Chromium's normal browser window, and also keeps arbitrary
requested URLs out of CreateProcess command-line parsing. Viewed final blank
startup in `build/mshtml-blank-app-final.png`; final host test evidence in
`build/mshtml-keyboard-final-view-test.txt`.

Important next integration target: this is not yet a general Internet browser
replacement. File-to-HTTPS address navigation triggers IE's existing security
zone/new-window prompt (no protection settings were changed). A separate
fresh Internet-zone run (`2e92c03d6fbd4432acec512616791f8a`,
`build/mshtml-internet-zone-ie7.txt`) remained in native loading: stock MSHTML
was loaded, no shim activation log was created, and no Chrome process ran.
It later displayed IE's native DNS/error page. At that point the cause was
unresolved; the subsequent Protected Mode findings above supersede the
initial handoff hypothesis. Cross-process runtime sharing and the remaining
DOM/Windows interfaces still need work. Direct OLE navigation passing is not proof
that native IE HTTPS navigation works. The exercise runner now supports
`web` and an `extended` 30-second observation mode for this investigation.

Windowed view lifecycle and real tab switching now have direct guest evidence.
`Show(FALSE)` hides both viewport and renderer and UI-deactivates the view;
in-place deactivation preserves its Chromium page. UI activation notifies the
site/frame, tracks balanced callbacks, and `GetWindow` returns the actual
viewport rather than the container window. The viewport is always parented to
the document host, including its initial zero-size phase, preventing transient
overlap of IE's tab/address bands. Moniker loading and IHTMLDocument2 URL writes
now share the same persistent navigation/attachment path.

`tools/ie7_mshtml_view_test.cpp` exercises two real embedded pages, both
load-before-show and show-before-load, moniker loading, rectangle persistence,
visibility, repeated activation/deactivation, same-window reactivation,
CloseView, explicit Close, and COM reference balance. It and the existing
lifetime/command-isolation test passed against DLL SHA256
`c64e9e4109501b4f0c3134bd18df3c4c660838d4a9a8a5e2b01c57cf1623ed75`.

Real IE7 job `b5ed5dea05b64cf6815a0669d013d1e8` opened two native tabs via
mouse clicks. QMP keyboard input entered `hello`; switching to New Tab and
back preserved both the input and page JS state (`Received: hello`). IE health
reported responsive and the runner restored stock MSHTML registration.
Evidence: `build/mshtml-tabs-verified-ie7.txt`,
`build/mshtml-tabs-verified-ie7.png`, and
`build/mshtml-tabs-preserved-ie7.png`.
Vista URLMon resolves `about:Tabs` to `res://ieframe.dll/tabswelcome.htm`;
these exact URLs receive a minimal shim-owned New Tab document. This is not
general res-protocol support or a reproduction of IE's original welcome page.
Ctrl+T while Chromium owns keyboard focus still does not open a tab; native
keyboard routing and broader Windows/DOM compatibility remain incomplete.

Lifecycle contract references:
[IOleDocumentView::Show](https://learn.microsoft.com/en-us/windows/win32/api/docobj/nf-docobj-ioledocumentview-show),
[IOleDocumentView::UIActivate](https://learn.microsoft.com/en-us/windows/win32/api/docobj/nf-docobj-ioledocumentview-uiactivate),
[IOleInPlaceObject::InPlaceDeactivate](https://learn.microsoft.com/en-us/windows/win32/api/oleidl/nf-oleidl-ioleinplaceobject-inplacedeactivate).

Closed-document command isolation: Refresh/Stop now require the document's own
target and session, not merely the shared browser transport. Command execution
no longer allocates a page for an unopened or closed document. The native guest
lifetime test closes the second document, verifies both commands are disabled,
and successfully refreshes the surviving document. The full test passed with
DLL SHA256 `6ddf3d61045a4358a8b86d63544130d8df6ef71980ad0706caab4164cf0c0918`.
Real IE7 exercise job `3ba161a69a17457c9103c3c1d82a9137` reported
`responsive=1`, persistent-navigation acknowledgement, and restored the stock
MSHTML registration. Evidence: `build/mshtml-command-isolation-ie7.txt`.
This does not establish native toolbar/shortcut routing or multi-tab correctness.

UIA update work was stopped at user request. KB971513 did not install; its
interactive installer exited with code 1. Offline rollback snapshot
`pre-uia-kb971513-20260912` remains on the development overlay.

Shared app-target allocation is now integrated into the shim. A fresh document
on a live runtime launches app mode with a uniquely created temporary bootstrap
file, discovers that exact URL's target through the existing pipe, and attaches
its own session. The bootstrap file is deleted afterward. Creation/session
attachment is serialized under the runtime lock. Window association uses the
tested CDP staging-bounds/PID match and requires exactly one HWND.

Vista job `2df8824476c343afbacb8d98b5030cac` passed the previously failing
two-live-document test, independent release, and the full existing native test
sequence. `build/shared-runtime-integrated.log` records HWND association and
target-specific closure. This is DLL-local sharing, not cross-process brokerage.
Actual multiple IE tabs, concurrent UI threads, failed-allocation rollback,
staging-bounds collisions and shared input-attachment lifetime remain unverified
or incomplete; do not infer full multi-tab usability from this COM test.

Runtime acquisition/release is now reference-counted through a DLL-local
registry protected by an SRW lock. Documents in that DLL share the runtime
allocation. Job `dd3bfb8c98ff4fbfb790b0a552354818` passed a new assertion that
releasing an unused second document preserves renderer access from the first.
The full test still fails at second-document navigation (line 153): a guard
explicitly refuses to attach a fresh document to another document's page until
the app-target allocator is integrated. This avoids a false pass caused by two
COM documents controlling the same target. Cross-process sharing remains absent;
the DLL-local registry is an integration stage, not the final broker.

Document teardown now retains/uses its target ID and sends Target.closeTarget,
not Browser.close. BrowserRuntime counts live pages; cleanup preserves its pipe
while that count is nonzero. Runtime objects are still per-document, so this
branch is preparatory rather than verified shared lifetime. Job
`e3211ba241ee4e99a62bc3aae0d9ac38` passed existing tests through close/reopen,
then hit the unchanged second-document failure. `build/target-close-shim.log`
records successful target-close and browser-exit results. Runtime reference
counting and acquisition must precede actual sharing; do not share the current
allocation without changing final Release ownership.

Integration refactor started: pipe handles, browser-process handle and request
counter now live in an allocated BrowserRuntime, with serialized pipe exchanges
under its transport lock. Each document still owns a distinct runtime at this
stage; shared ownership/target routing is not implemented. Vista job
`4f560fed629a4d57a649449fb505177d` preserved moniker, URL, readyState,
Close and reopen assertions, then failed at the existing second-document
navigation assertion (line 148). Later test assertions did not execute; do not
describe this run as a complete lifetime regression pass.

Target-to-HWND association has a tested prototype: the pipe probe obtains the
second target's Browser window ID, sets diagnostic bounds through that ID,
then requires exactly one visible Chrome_WidgetWin_1 with those bounds in the
owned browser PID. Job `4deac88accc4421c96ffe2d5b49f94df` passed association,
independent target close, and first-page survival (`build/target-hwnd-probe.txt`).
Page-controlled window titles are not used as identity. A broker must serialize
creation/association, choose collision-free staging bounds or reject ambiguity,
and handle DPI/bounds changes; these are not yet implemented. The standalone
probe remains separate from the still-failing two-document MSHTML regression.

Window-mode prerequisite resolved in the standalone probe. Default CDP
Target.createTarget(newWindow=true) visibly created a full browser toolbar
(`build/shared-target-window.png`, job `0842d0b504044d31bcb3dfc748271860`).
The probe now launches a second --app URL against its existing profile, discovers
that target through the original pipe, and attaches its session there. Job
`1009b00c47ed4ba1bcb2a3cc11b3cdf5` passed independent navigation/DOM reads,
target close and first-page survival. `build/shared-app-window.png` confirms
the second window has only an app title bar, no tabs or omnibox; the existing
embedding trim can address that title bar. Distinct Browser.getWindowForTarget
IDs were recorded in `build/shared-app-window.txt`. This path is still a probe,
not integrated into the shim; reliable target-to-HWND association and shared
runtime ownership remain required.

Shared-transport prerequisite verified by the extended standalone pipe probe:
job `58bc04ea8fad4425ab5fa6c957f83842` created a second target through the
existing browser connection, attached a distinct session, navigated/read its
unique title, closed only that target, and read the original page's unchanged
title afterward. Evidence: `build/shared-target-probe.txt`; source:
`tools/supermium_pipe_probe.c`. This supports a shared-runtime broker design.
It is not integrated into the MSHTML facade: its two-document regression remains
failing. Target.createTarget with newWindow=true has not been verified to make
a renderer-only app window; mapping targets to HWNDs and suppressing browser UI
remain required before this path can be used by embedded IE documents.

Two simultaneous COM documents are now an explicit failing regression in
`tools/ie7_mshtml_lifetime_test.c`. Job `223b8bca7132485a8dd9eb3656580a9a`
passed all earlier steps but failed the second object's first URL navigation.
`build/two-document-failure.log` records launcher exit code 0 and
SUPERMIUM_LAUNCH_HANDOFF_UNRESOLVED: Supermium handed the second --app launch to
the existing profile owner rather than providing a second usable pipe.
The current test suite is therefore failing, not fully green.

Required next architecture work: separate browser/profile ownership from
document ownership. A runtime connection must multiplex document sessions and
allocate/select each document's target and HWND; releasing one document must
close its target, not Browser.close the shared runtime. The current sole-page
target assumption, per-document request IDs, per-document process ownership,
and Browser.close cleanup all need coordinated changes. Independent throwaway
profiles would bypass this failure but would not establish shared-session IE
tab behavior. Cross-IE-process sharing needs an out-of-process broker or an
equivalent explicitly owned connection, not only a DLL-local singleton.

Same-document navigation after Close initially failed in job
`86820fd01e294a35b1acc43022ef7573`. The shim now retains its browser process
handle and waits up to five seconds for shutdown during cleanup; it refuses a
new launch while that owned process remains alive rather than racing the shared
profile. With this change, job `1618b7142cc04b548f1d762fa359fdd2` passed
navigation after Close plus prior tests. `build/reopen-success.log` records
successful process-exit waits. This bounded synchronous wait is a prototype
tradeoff, not a solution to UI responsiveness or multi-document ownership.

IOleObject::Close now requests Browser.close and releases the renderer window,
viewport, explicit input attachment and pipe handles. Teardown also clears the
session ID so subsequent commands cannot target a dead session; final Release
uses the same cleanup helper. Native job `59b3cc37c6884314b023b47344b6d9ef`
passed invalid-close-argument rejection, repeated Close, disabled Refresh after
Close, and existing moniker/navigation/COM lifetime checks. This is not full
OLE deactivation: host-site callbacks, saved edits, browser-process exit timing,
and multiple documents still require verification/implementation.

IPersistMoniker now retains the successful Load moniker and GetCurMoniker
returns that same COM object with an added reference. Null output is rejected;
URL navigation clears the retained moniker to avoid stale identity. Native
job `b54c84ad7ca141a6b15a0320eef4e581` passed load/get identity and existing
navigation, command and lifetime assertions. The test now links `-lurlmon`
to create its URL moniker. No claim is made that toolbar Refresh is fixed;
IOleObject moniker methods and monikers for later URL navigations remain
incomplete. Contract reference:
https://learn.microsoft.com/en-us/previous-versions/windows/internet-explorer/ie-developer/platform-apis/ms775041(v=vs.85)

Post-keyboard-fix Refresh UI regression: job
`d61c2fd133c048109b521a36029ab427` successfully typed `hello`, but clicking
native Refresh left both input and event output unchanged
(`build/refresh-after.png`) and emitted no Refresh Exec (`build/refresh-ui.log`).
The health probe now records WindowFromPoint and its ancestor chain at the
800x600 test display's Refresh coordinate. Job
`4f080823cbf646e99394e9cefe531c08` hit ToolbarWindow32 under Address Band
Root, not the embedded viewport (`build/refresh-hit-test.txt`). IE was responsive
and stock registration restored. This excludes viewport hit-test occlusion in
that layout; the remaining Refresh issue is upstream host command/lifecycle
integration, separate from the now-working page keyboard path.

Actual embedded text entry now passes. The focus helper retains its explicit
input-queue attachment for the document instead of immediately detaching it;
close/destruction releases that attachment. After removing a leftover stock IE
error-page process that obscured the experiment, job
`ce8a3706e73e4d8baeb9475ac20233d5` received QMP mouse and keyboard events in
the embedded HTML input. `build/ie-input-attached3.png` shows both `hello` in
the input and `Received: hello` from the page's input event handler. This is
physical-input-path evidence, not DOM value injection. The change still needs
multi-document attachment accounting and crash/deactivation responsiveness
testing; shared input queues can couple UI-thread responsiveness.

Keyboard positive control now exists in `tools/vista_keyboard_probe.c`: a
temporary native EDIT window logs foreground/focus and final text. Initial
SetForegroundWindow was denied, so the probe alone uses HWND_TOPMOST followed
by a physical-QMP mouse click into its field. Job
`93d3026db6af4574a11784f61e29eecc` visibly received `hello` from the same
QMP send-key sequence used for the embedded input test; screenshot
`build/native-keyboard-focused.png`. This validates the keyboard injection
path independently of Chromium and narrows the embedded failure to activation
and input routing. Topmost is confined to the diagnostic probe, not the shim.

Added `tests/fixtures/ie-input.html`; pass `input` to the guest IE exercise to
load it. Physical-QMP-key experiments captured `build/ie-input-keys.png` and
`build/ie-input-click.png`: no text appeared, but clicking the field produced
a visible caret. Job `5bee8ed3d0a54510a5c5b39115b729b5` restored stock
registration. A Notepad keyboard control was inconclusive because another IE
window remained foreground in the captures. Therefore these results establish
mouse interaction with the embedded field, not a conclusive keyboard diagnosis.
The next input test must positively identify/activate the intended foreground
window and first prove the keyboard injection path with a stock text control.

Focus handoff now selects the visible Chrome_RenderWidgetHostHWND child,
temporarily attaches the calling and target input queues, calls SetFocus,
detaches, and measures target-thread focus. Vista job
`cbaa2bb0355848e3bdd1978b84df947d` reported focus verification=1, and the
independent window-health probe later found Chromium's focus still on that
exact child HWND. IE remained responsive. IE's queue reported null focus and
Chromium's queue reported null active window after detachment, so this does
not yet prove physical keyboard events reach the page. A text-entry fixture
and actual input test remain required. Prior IEFrame-only focus measurements
omitted Chromium's separate input queue and were incomplete evidence.
API contract: https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setfocus

Retest `290da58da80e41d092c82084d590147b` confirms accepted download-state
notifications alone do not restore toolbar Refresh routing
(`build/ie-notified-refresh.log`). Added `tools/ie_window_health.c` and an
optional invocation in the IE exercise to measure WM_NULL responsiveness and
GUI-thread input ownership. Live job `4d720806e18f4a3a80338bb6360b3e8d`
reported responsive=1, no capture, and focus on IEFrame rather than the embedded
renderer. Thus no general IE-thread hang was observed; the focus request log
must not be treated as successful keyboard focus. Further input integration
needs to verify actual focus ownership and text entry.

The viewport now polls renderer readyState while a host load cycle is pending
and sends OLECMDID_SETDOWNLOADSTATE (1, then 0) to the client site's command
target. Host callbacks are guarded against recursive notification and hold a
document reference. IE exercise `2d30a52748694c549afc7b4bff87e3b2` returned
S_OK for both notifications and restored stock registration. This establishes
accepted download-state reporting, not DocumentComplete event compatibility or
a demonstrated toolbar fix. Subsequent URL navigation and Refresh reset the
cycle. The polling implementation still performs synchronous pipe reads on the
UI thread; an asynchronous transport remains necessary for responsiveness.
Reference: https://raw.githubusercontent.com/wine-mirror/wine/master/dlls/mshtml/persist.c

UI regression finding: live IE exercises `37dbad5a6d1b4cc6a1dabe14837467ba`
and `3729948ffa9545959a211a234dc979af` received QMP mouse input at the native
Refresh button; the second also explicitly activated IE, repeated held clicks,
and sent F5. `build/ie-refresh-click.png` shows the pointer on the native button.
Neither `build/ie-refresh-click.log` nor `build/ie-refresh-repeat.log` records
Refresh Exec or its renderer acknowledgement. Thus direct COM command tests do
not establish usable IE toolbar routing. Investigate host navigation lifecycle
and command routing next; missing host completion notifications are a lead,
not a verified cause. The current IPersistMoniker::Load only launches/navigates
the renderer and returns; no bind-status or download-completion notification
path exists in this source.

Standard IOleCommandTarget Refresh and Stop now forward to Page.reload and
Page.stopLoading on the existing renderer session. QueryStatus advertises them
as enabled only with a connected pipe; unsupported commands and command groups
return explicit errors. Vista test job `34a5844d897441e9befbeb105ecfa7c9`
passed command-status and error-path assertions and received both CDP
acknowledgements (`build/command-routing.log`). Toolbar-click routing and actual
cancellation of an in-flight network response remain unverified. Refresh cache
policy flags are not yet interpreted.
Command definitions: https://learn.microsoft.com/en-us/windows/win32/api/docobj/ne-docobj-olecmdid

`build/ie-current.png` captures the real IE7 process with Supermium's page
inside IE's native address/tab/status UI. Exercise job
`de0acc0df8f445059b51c0ee090a5350` exited zero and restored the stock x86
HTMLDocument registration. Its log confirms renderer-derived readyState.

Initial launch now attaches a DevTools page session and sends Page.navigate,
just like subsequent navigation. This corrects Supermium app mode substituting
New Tab for about:blank. The session identifier is reset when launching a new
browser. Native test job `09259531e6954fd3ac36820b9ed21188` passed direct
activation, initial readyState, second navigation, dispatch validation and
COM lifetime checks. `build/initial-navigation.log` records both navigation
acknowledgements against the same frame/session. This is not proof of full
DOM support or full Windows compatibility: most HTML interfaces remain stubs,
and input, browser commands, multiple documents and teardown need further work.

Bulk guest uploads now use `tools/vista_transfer.py`; see `docs/VISTA_CONTROL.md`.
The earlier ISO-only advice applies to the old serial-only workflow.

## Evidence and source pins

- Wine MSHTML module and build inventory:
  https://github.com/wine-mirror/wine/tree/master/dlls/mshtml
- Wine's XPCOM bridge declaration:
  https://github.com/wine-mirror/wine/blob/master/dlls/mshtml/nsiface.idl
- CEF branch mapped to Chromium 144:
  https://github.com/chromiumembedded/cef/blob/7559/CHROMIUM_BUILD_COMPATIBILITY.txt
- Supermium's Vista-compatible Chromium source:
  https://github.com/win32ss/supermium

The snapshot gate verifies the live QMP disk node and the QCOW backing chain.
The implementation gate begins only after a CEF 7559 plus Supermium-port source
tree and a build toolchain are pinned with hashes.
