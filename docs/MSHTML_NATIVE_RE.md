# Vista IE7 native MSHTML reverse-engineering audit

> Development record retained with the public desktop sources. Local capture,
> VM and `.unlazy/` paths refer to non-shipped development artifacts. See
> [current public entry points and status](DESKTOP-MODULES.md).


Date: 2026-09-12. Target: the running `winvista-3-mshtml-cef-dev.qcow2` development VM derived from `winvista-3.qcow2`.

The original audit below describes the pre-repair shim. The 2026-09-13 follow-ups record additional native findings and subsequent implementation/tests; the original defect descriptions are not a claim that every defect remains unchanged. The latest [DocObject activation follow-up](#2026-09-13-follow-up-native-docobject-activation-and-toolbar-routing) traces and repairs native Refresh/Stop routing. The full IE/Windows integration goal remains open.

## Initial audit result

The actual guest DLLs and matching Microsoft PDBs are available locally, with symbol-addressed radare2 disassembly. This is a native-binary audit, not an inference from Wine's implementation. The shim is missing the document-owned navigation handshake and window interfaces, has the wrong automation ready-state type, omits host notifications, and claims successful file loads without loading anything. Its new-document path explicitly launches another Chromium app window.

No replacement-shim code, DLL registration, launch-security policy, activation setting, or original guest DLL was changed in this audit. A small diagnostic EXE and export script were staged in the existing guest bridge directory. The native COM probe opens no browser and navigates nowhere. This audit does **not** claim the broken navigation is fixed. Web-page ActiveX controls remain out of scope; the browser's own OLE/COM hosting contracts are still required.

## Binary and symbol identity

All four copied DLLs report version **7.0.6002.18005** and have `VS_FF_DEBUG` set under the version-resource flag mask. They are checked-build binaries, not substituted retail downloads.

| Local DLL | Actual guest source | Bytes | CodeView GUID; image age |
| --- | --- | ---: | --- |
| `mshtml-x86.dll` | `C:\Windows\SysWOW64\mshtml.dll` | 9043456 | `60393B1F-7C55-4E63-B5DD-0E0F69DCD3B5`; 1 |
| `ieframe-x86.dll` | `C:\Windows\SysWOW64\ieframe.dll` | 7787008 | `C4C6C3D0-973B-4E67-BDC6-8792404AC32B`; 1 |
| `urlmon-x86.dll` | `C:\Windows\SysWOW64\urlmon.dll` | 1452544 | `C613F714-1ADF-4F59-9452-051438E57DDE`; 1 |
| `mshtml-x64.dll` | `C:\Windows\System32\mshtml.dll` | 12056576 | `96FA256D-099F-4C0B-BAA9-8842E0F633DC`; 1 |

Extraction used the private QEMU TCP gateway, not multi-megabyte serial transfers. [Extraction provenance](../build/mshtml-native-re/provenance.json) records guest paths, sizes, versions, and SHA-256 hashes; [PE inventory](../build/mshtml-native-re/binaries.json) independently records architecture, hashes, sections, image bases, debug flags, and CodeView records.

Matching files were fetched from Microsoft's symbol store:

- [x86 MSHTML PDB](https://msdl.microsoft.com/download/symbols/mshtml.pdb/60393B1F7C554E63B5DD0E0F69DCD3B51/mshtml.pdb)
- [x86 IEFRAME PDB](https://msdl.microsoft.com/download/symbols/ieframe.pdb/C4C6C3D0973B4E67BDC68792404AC32B1/ieframe.pdb)
- [x86 URLMON PDB](https://msdl.microsoft.com/download/symbols/urlmon.pdb/C613F7141ADF4F599452051438E57DDE1/urlmon.pdb)
- [x64 MSHTML PDB](https://msdl.microsoft.com/download/symbols/mshtml.pdb/96FA256D099F4C0BBAA98842E0F633DC1/mshtml.pdb)

These are stripped public PDBs, not original source or complete private type information. All have PDB information-stream age 2 and DBI age 1. This is a valid match: Microsoft's `PDB1::OpenValidate4` requires the GUID to match, information-stream age to be at least the image age, and nonzero DBI age to equal image age. The verifier follows those rules; it does not force-load mismatched symbols. [Microsoft PDB validation implementation](https://github.com/microsoft/microsoft-pdb/blob/master/PDB/dbi/pdb.cpp).

The exact-match positive control passed; substituted GUID and DBI-age expectations were both rejected. Each PDB has a saved summary and URL/size/hash provenance JSON beside it.

## Method and limits

LLVM inspects PE/PDB metadata; radare2 5.9.8 loads the verified PDBs and disassembles selected native functions. Ghidra was not installed; radare2 was actually used, not merely proposed. Function listings, raw LLVM public symbols, recovered function tables, and annotated instruction dumps are in [the artifact directory](../build/mshtml-native-re/).

All addresses below are **preferred-image VAs**, not current guest ASLR addresses. Bases are MSHTML `0x77000000`, IEFRAME `0x75c80000`, URLMON `0x72820000`; use `live_module_base + RVA` in a debugger. Analysis focuses on the 32-bit IE7 path. The x64 MSHTML/PDB pair was acquired and indexed, but its navigation implementation has not been audited to the same depth.

The disassembler keeps raw operands (`asm.sub.var=false`, `asm.sub.names=false`). Its automatic local-variable guesses are not treated as recovered C++ layouts. Focused `af`/`pdf` analysis can omit exception-handler or indirect-call branches. Call tables, GUID bytes, checked-build assertion strings, and runtime probes are used to corroborate important interpretations. Embedded build paths and assertion text are not recovered source files.

## 1. The missing navigation handshake, from producer to consumer

This is the most important architectural finding.

Native MSHTML's [CDoc::InitDocHost, VA 771ccbd0](../build/mshtml-native-re/mshtml-x86.dll.disassembly/771ccbd0.txt) calls `SetHostNavigation(FALSE)` at `771ccc20`. It also sends `DOCHOST_DOCNEEDSNAVNOTIFICATIONS`, command 13, through `CGID_DocHostCmdPriv`.

[CDoc::SetHostNavigation, VA 771ccf40](../build/mshtml-native-re/mshtml-x86.dll.disassembly/771ccf40.txt) prepares a `VT_UNKNOWN` variant and, when document-owned navigation is appropriate, obtains the primary window's `IUnknown`. At `771cd0d1`, it calls the client's command target with:

```text
group = CGID_DocHostCmdPriv = {000214D4-0000-0000-C000-000000000046}
command = DOCHOST_DOCCANNAVIGATE = 0
input = VT_UNKNOWN(primary window), or NULL when the host owns navigation
```

The checked binary retains a diagnostic string naming this exact command and conditional input. The function also has guards for document/host state; implementing an unconditional notification is not a faithful substitute.

IEFRAME receives it through [CDocObjectHost::Exec, VA 75e18063](../build/mshtml-native-re/ieframe-x86.dll.disassembly/75e18063.txt) and [_HandleDocHostCmdPriv, VA 75e14fc9](../build/mshtml-native-re/ieframe-x86.dll.disassembly/75e14fc9.txt). In command 0's branch, it checks top-browser/browser-band eligibility, releases its previous HTML window, validates `VT_UNKNOWN` with a nonnull object, sets `_fDocCanNavigate`, and queries `IHTMLWindow2` into `_pHTMLWindow` (`75e150d5` through `75e1510a`). Null/invalid input clears the capability (`75e15113`).

The eligibility test at `75e150bc` is `IBrowserService::GetFlags`, slot 20 (`+0x50`), followed by bit `0x800`, `BSF_TOPBROWSER`. [InitHostWindow, VA 75e0c5e2](../build/mshtml-native-re/ieframe-x86.dll.disassembly/75e0c5e2.txt) acquires this pointer with `QI(IBrowserService)` at `75e0c7da`; the method layout and flag definition agree with the local MinGW `shdeprecated.h`. The GUID is `{02BA3B52-0547-11D1-B833-00C04FC9B31F}`, not `IBrowserService2` (the earlier report misnamed it). **It is not `IOleObject::GetMiscStatus` or `OLEMISC_ALWAYSRUN`.** A speculative identification of those flags was rejected by the native probe and corrected during this audit. In ordinary top-browser hosting, the missing command means IE never receives the replacement's navigable window through this path.

Then [CDocObjectHost::SetTarget, VA 75e162d0](../build/mshtml-native-re/ieframe-x86.dll.disassembly/75e162d0.txt) can call `_NavigateDocument` at `75e166eb`. Otherwise its binding path includes `_StartAsyncBinding` at `75e16c3c`.

```text
MSHTML InitDocHost -> SetHostNavigation
  -> client IOleCommandTarget: DOCHOST_DOCCANNAVIGATE(window)
  -> IEFRAME stores IHTMLWindow2 and navigation capability

IE SetTarget
  -> eligible document-owned path -> _NavigateDocument
     -> window QI(IHTMLPrivateWindow4) -> SuperNavigate2
  -> binding path -> _StartAsyncBinding -> URLMON BindToObject
     -> object persistence, or navigation-error handling
```

[CDocObjectHost::_NavigateDocument, VA 75e0aa6c](../build/mshtml-native-re/ieframe-x86.dll.disassembly/75e0aa6c.txt) asserts `_fDocCanNavigate`, queries `{3050F594-98B5-11CF-BB82-00AA00BDCE0B}` at `75e0ab65`, retrieves POST data and headers, and calls interface vtable offset `+0x24` at `75e0ac8c`.

The native function table proves that this is `IHTMLPrivateWindow4::SuperNavigate2`, not a guessed method name. [Recovered CWindow::s_apfnIHTMLPrivateWindow4 table](../build/mshtml-native-re/mshtml-x86.dll.table-77048d08.json), at `77048d08`:

| Slot | x86 offset | Method | Native target |
| ---: | ---: | --- | --- |
| 0–2 | 00–08 | QueryInterface / AddRef / Release | tear-off thunks |
| 3 | 0c | SuperNavigate | 772fe470 |
| 4 | 10 | GetPendingUrl | 772fff00 |
| 5 | 14 | SetPICSTarget | 772ffff0 |
| 6 | 18 | PICSComplete | 77300090 |
| 7 | 1c | FindWindowByName | 773001a0 |
| 8 | 20 | GetAddressBarUrl | 772ffe30 |
| 9 | 24 | SuperNavigate2 | 772fda90 |
| 10 | 28 | SuperNavigate3 | 772fdad0 |

[SuperNavigate2](../build/mshtml-native-re/mshtml-x86.dll.disassembly/772fda90.txt) passes an `IUri`, string parameters, POST/header variants, and flags to [SuperNavigateInternal](../build/mshtml-native-re/mshtml-x86.dll.disassembly/772fdc40.txt), which reaches `CDoc::FollowHyperlink2` at `772fe299`.

**Shim comparison:** `IHTMLDocument2::get_parentWindow` is a generated `E_NOTIMPL` stub. There is no corresponding window object/private-window implementation or `DOCHOST_DOCCANNAVIGATE` notification. Adding only the private-window IID, without the correct window identity and host handshake, would leave the integration incomplete. Checked-build assertions and the native probe establish that these are real contracts; a live breakpoint capture of the entire failing address-bar transition is still outstanding.

## 2. Ready-state automation type and notifications are wrong

[IEFRAME::_OnChangedReadyState, VA 75e1607a](../build/mshtml-native-re/ieframe-x86.dll.disassembly/75e1607a.txt) invokes `DISPID_READYSTATE = -525` at `75e16143`, then explicitly compares the returned variant type with **3 (`VT_I4`)** at `75e1614a`. The numeric result is passed to `_OnReadyState` only on that branch.

The typed DOM property is different: native [CDocument::get_readyState, VA 772ad3e0](../build/mshtml-native-re/mshtml-x86.dll.disassembly/772ad3e0.txt) converts the state enum into a BSTR. The shim's `html2_invoke` returns `VT_BSTR` for `-525`; `dispatch_invoke` routes both this legacy numeric DISPID and the typed HTML ready-state DISPID through that handler. `html2_ready_state` also defaults to `complete` before a renderer exists.

This is exercised by IE, not merely hypothetical: [the broken-navigation trace](../build/ie-address-unexpected-launch.log) contains `IDISPATCH_INVOKE_DISPID=4294966771` (unsigned `-525`) at lines 40, 94, 141, and 219.

[IEFRAME::_SetUpTransitionCapability, VA 75e161b6](../build/mshtml-native-re/ieframe-x86.dll.disassembly/75e161b6.txt) connects `IPropertyNotifySink`. Native [CMarkup::SetReadyState, VA 771d0740](../build/mshtml-native-re/mshtml-x86.dll.disassembly/771d0740.txt) calls `CBase::FirePropertyNotify(-525, ...)` at `771d0afe` and `CDocument::Fire_onreadystatechange` at `771d0b35`. The shim does not expose the connection-point container. Sending `OLECMDID_SETDOWNLOADSTATE` is not equivalent to delivering these notifications.

There are native fallbacks that assume completion when notification capabilities are absent, so the type mismatch alone is not proof that every navigation must hang. It is a confirmed break in the host's state-transition contract.

## 3. Persistence: false success and discarded binding context

Native [CDoc::Load(LPCWSTR,DWORD), VA 772d8950](../build/mshtml-native-re/mshtml-x86.dll.disassembly/772d8950.txt) puts the filename in a load-info structure and calls `LoadFromInfo` at `772d89ad`. The shim's `file_load` ignores the filename and returns `S_OK` (`IPERSISTFILE_LOAD_STUBBED`).

Native [URLMON CBinding::ObjectPersistFileLoad, VA 7286bc4d](../build/mshtml-native-re/urlmon-x86.dll.disassembly/7286bc4d.txt) obtains a filename, queries `IPersistFile`, performs its safety checks, then calls `Load(filename, 0)` at `7286bcec`. The recorded failure reaches the shim's file-load stub at trace lines 128–129. Successful return cannot honestly mean loaded content here.

Native [CDoc::Load(IPersistMoniker), VA 772d9270](../build/mshtml-native-re/mshtml-x86.dll.disassembly/772d9270.txt) preserves the supplied moniker and bind context, obtains the display name and URI, recognizes error URLs, and enters `LoadFromInfo` at `772d93b7`.

[CDoc::LoadFromInfo, VA 772dcea0](../build/mshtml-native-re/mshtml-x86.dll.disassembly/772dcea0.txt) consumes bind-context parameters, including the client-site object parameter `{d4db6850-5385-11d0-89e9-00a0c90a90ac}` and `__PrecreatedObject`. It can obtain/set `IOleClientSite` (`772dd857`–`772dd8e8`), replace the markup's moniker (`772ddcfa`), and invoke `CMarkup::LoadFromInfo` (`772dde91`). This is not a contract to extract one URL string and discard the existing navigation transaction.

**Shim comparison:** `moniker_load` primarily converts the display name into `Page.navigate`; it does not preserve the native binding's body, headers, response/error state, or context-provided site semantics. It also overwrites `current_moniker` after AddRef without releasing the previous retained moniker. A real binding-aware implementation, or an explicitly designed engine-owned navigation boundary, is required.

The native wrapper does not appear to use every `fullyAvailable`/`dwMode` argument either. Those omissions alone are not ranked as defects.

## 4. Why the recorded URL becomes an IE certificate-error page

The existing trace records a new document loaded with `res://ieframe.dll/invalidcert.htm?SSLError=67108864` at line 123. The intended Google URL is never supplied to this shim's moniker-load entry in that capture.

Native [IEFRAME::_StartAsyncBinding, VA 75e10b8a](../build/mshtml-native-re/ieframe-x86.dll.disassembly/75e10b8a.txt) calls `IMoniker::BindToObject` at `75e10ebf`: IE/URLMON can make network and security decisions before the replacement renderer receives an ordinary document load.

[CDOHBindStatusCallback::OnStopBinding, VA 75dc7a24](../build/mshtml-native-re/ieframe-x86.dll.disassembly/75dc7a24.txt) has a navigation-error route to `_NavigateToErrorPage` at `75dc8d23`. [_NavigateToErrorPage, VA 75dc5d20](../build/mshtml-native-re/ieframe-x86.dll.disassembly/75dc5d20.txt) maps errors to error-page URLs and appends `?SSLError=%s` at `75dc6134`. Its certificate-handling path is upstream of simply rendering the resulting URL.

**Conclusion:** launching a newer renderer inside IE does not by itself transfer navigation/network ownership out of IE's legacy binding stack. The trace is consistent with that native error path. This audit does not identify the exact certificate validation failure, claim the CA store is the cause, or justify disabling certificate validation. It also does not prove the precise persistence fallback branch without a live trace.

## 5. Host view notifications and lifetime

Native [IEFRAME::_InitOleObject, VA 75e0c423](../build/mshtml-native-re/ieframe-x86.dll.disassembly/75e0c423.txt) calls `IViewObject::SetAdvise(DVASPECT_CONTENT, ADVF_PRIMEFIRST, sink)` at `75e0c56a`.

Native [CServer::SetAdvise, VA 77391d10](../build/mshtml-native-re/mshtml-x86.dll.disassembly/77391d10.txt) releases its previous sink, retains an advise sink, and performs the prime-first `OnViewChange` callback at `77391dc8`. The shim's `view_set_advise` ignores all inputs and returns `S_OK`. Host notification support is missing even with web-page ActiveX disabled.

Native [CServer::Close, VA 7737dbe0](../build/mshtml-native-re/mshtml-x86.dll.disassembly/7737dbe0.txt) handles close/save state, including dirty-document/site-save handling, and transitions the object to the loaded state. [CloseView, VA 77397970](../build/mshtml-native-re/mshtml-x86.dll.disassembly/77397970.txt) detaches the in-place site; it is not an unconditional browser-process destruction operation.

The shim's `ole_close` immediately closes its Chromium target and can wait for the browser. COM document, DOM generation, view, browser window, and Chromium runtime lifetimes must not be treated as one lifetime.

## 6. The extra app launch and blocking teardown are explicit shim behavior

In [the shim source](../tools/ie7_mshtml_activation_probe_shim.c), `ensure_document_session_unlocked` calls `create_app_target` when a new document lacks a session but another page is live. `create_app_target` explicitly calls `CreateProcessW` with `chrome.exe --app=...` and then locates the resulting window. Reusing a browser profile does not make this a same-page navigation.

`document_pipe_exchange` performs synchronous polling and sleeps on the caller, and does not retain unmatched CDP event messages. Target close and browser-exit waits are also synchronous. The existing trace records a target-close timeout (`0x800705b4`) at line 179. These are concrete sources of blocking behavior, although no full live IE hang stack was captured in this audit.

The integration needs a stable browsing-context/window identity with document generations underneath it. Native IE may legitimately create a new COM document; prohibiting all new documents is not a valid fix. Conversely, creating a new document must not automatically request another app window. A `Page.navigate` response is only command acknowledgement, not proof that the requested page committed and finished loading.

## Additional contract hazards

The shim's `service_provider_query_service` returns its command target for **any service GUID** if the requested IID is `IOleCommandTarget`; the service identity is only logged. Native [CDoc::QueryService, VA 77317fe0](../build/mshtml-native-re/mshtml-x86.dll.disassembly/77317fe0.txt) dispatches by service identity. The shim must distinguish supported service/IID pairs instead of claiming every command-target service. The effect of specific rejected or incorrectly accepted services on the recorded failure still needs tracing.

The shim's `IViewObject::Draw` also returns success without producing a drawing, and `IOleObject::Advise` does not register sinks. These matter for completeness of the advertised OLE object, but this audit does not establish them as the cause of the address-bar failure. Printing, snapshots, save/dirty behavior, and less-used DOM contracts need separate acceptance tests rather than blanket `S_OK` stubs.

## Native COM probe: actually run in Vista

[Probe source](../tools/mshtml_native_contract_probe.c) loads `C:\Windows\SysWOW64\mshtml.dll` directly and calls its `DllGetClassObject`; it does not rely on the user's HTMLDocument registration. After creating a native document and `InitNew`, it checks the real contracts in the guest.

[Guest output](../build/mshtml-native-re/native-contract-probe.txt):

```text
DISPID_READYSTATE=-525 HR=0x00000000 VT=3 VALUE=1
TYPED_READYSTATE HR=0x00000000 VALUE=loading
PARENT_WINDOW=0x00000000
IHTMLPrivateWindow4=0x00000000
PROPERTY_NOTIFY_CONNECTION_POINT=0x00000000
```

The native OLE miscellaneous flags were `0x00220191`, without `OLEMISC_ALWAYSRUN`. That rejected an early mistaken association between unrelated flag namespaces; [the rejected-assumption run is retained](../build/mshtml-native-re/native-misc-assumption-rejected.txt). The corrected probe records those flags without asserting the mistaken bit.

## Suspicions not supported as navigation fixes

- Native [SaveViewState, VA 77397a00](../build/mshtml-native-re/mshtml-x86.dll.disassembly/77397a00.txt) and [ApplyViewState, VA 77397a70](../build/mshtml-native-re/mshtml-x86.dll.disassembly/77397a70.txt) are intentionally successful no-ops in this build. Their shim no-ops are not evidence of the current fault.
- Native [CreateView, VA 77396cb0](../build/mshtml-native-re/mshtml-x86.dll.disassembly/77396cb0.txt) enforces one created view. A single-view design is not inherently wrong; repeated-create HRESULT/state handling still needs fidelity.
- Native `CGID_Explorer` command mapping in [CBase::IDMFromCmdID, VA 77383f70](../build/mshtml-native-re/mshtml-x86.dll.disassembly/77383f70.txt) does not establish support for command 43 seen in the trace. An unsupported command log by itself is not proof that implementing that command will fix navigation.
- Browser-service flags are not OLE miscellaneous flags. Do not add `OLEMISC_ALWAYSRUN` based on the `BSF_TOPBROWSER` check.

## Repair order and acceptance criteria

This is the implementation sequence justified by the evidence, not changes completed by this audit:

1. Implement a real stable window/browsing-context object, `get_parentWindow`, the required private-window contract, and the correctly timed `DOCHOST_DOCCANNAVIGATE` handshake. Preserve COM identity/refcounts and separate document generations from renderer targets.
2. Implement numeric legacy ready state separately from the BSTR DOM property. Deliver property and document events from correlated Chromium navigation state, with host callbacks on the owning apartment.
3. Stop false-success persistence stubs; define ownership of initial URL binding, subsequent navigation, POST/header data, redirects, failures, and resource/error URLs. Do not merely strip `res://` errors or suppress security validation.
4. Move CDP I/O and waits off the IE UI thread, retain and route asynchronous events, and make cancellation/target teardown bounded and generation-aware. Implement host view-advise retention and callbacks.
5. Run native-vs-shim contract probes and a visible IE address-bar sequence: URL A, URL B, redirect, Back, Forward, Refresh, Stop, and certificate-error handling. Assert committed URL/content, responsive UI, correct history/load events, and no additional top-level Chromium app window for same-tab navigation. Opening a new tab/window must still have distinct identity.

Useful next debugger locations are IEFRAME RVAs `194fc9` (handshake receiver), `1962d0` (SetTarget), `18aa6c` (in-document navigation), `190b8a` (legacy binding), and `19607a` (ready-state consumer); MSHTML RVAs `1ccf40` (handshake sender), `2fda90` (SuperNavigate2), `2d9270` (moniker load), and `2d8950` (file load).

Remaining unverified work includes the complete live failing-navigation call sequence, all private-window method semantics, travel-log integration, cross-process/protected-mode marshaling, document/window identity over history restoration, and full x64 parity. These limits prevent treating this audit as a finished browser replacement.

## Reproduction

Run from the repository root. The extractor refuses to overwrite existing extracted DLLs/provenance; preserve the existing audit before making a new extraction workspace. LLVM, radare2, and Python `pefile` are required on the host.

```sh
python3 tools/extract_vista_mshtml.py
python3 tools/mshtml_native_symbols.py inventory
python3 tools/mshtml_native_symbols.py fetch
python3 tools/mshtml_native_symbols.py verify-binaries
python3 tools/mshtml_native_symbols.py verify-symbols
python3 tools/mshtml_native_symbols.py index
python3 tools/mshtml_native_symbols.py disassemble --module ieframe-x86.dll --match 'CDocObjectHost::_NavigateDocument_'
python3 tools/mshtml_native_symbols.py disassemble --match 'CDoc::SetHostNavigation_'
python3 tools/mshtml_native_symbols.py table --match 'CWindow::s_apfnIHTMLPrivateWindow4$' --slots 11
```

Build and run the probe using the existing development container and guest control channel:

```sh
podman exec -w /workspace vista-driver-builder i686-w64-mingw32-gcc -std=c11 -O2 -Wall -Wextra -Werror -static -static-libgcc -Wl,--subsystem,console:6.0 tools/mshtml_native_contract_probe.c -o build/mshtml-native-re/native-contract-probe.exe -lole32 -loleaut32 -luuid
python3 tools/vista_control.py put build/mshtml-native-re/native-contract-probe.exe 'C:\TritonSupermiumBridge\native-contract-probe.exe'
python3 tools/vista_control.py run --user --timeout 30 'C:\TritonSupermiumBridge\native-contract-probe.exe'
```

The audit's unlazy acceptance ledger is [.unlazy/mshtml-native-re/GATES.md](../.unlazy/mshtml-native-re/GATES.md). Binary/PDB and native guest-probe checks are executable; the native-vs-shim interpretation and report-boundary review are evidence-backed manual gates. They certify this audit, not end-to-end navigation.

## 2026-09-13 follow-up: completion is a separate native contract

The first native-contract repair provided the stable window/private-window-4 object, command-0 handshake, numeric ready state and property connection point. A message-only observer was needed because IE waits for ready-state changes before creating/showing the view; a timer attached only to the not-yet-created viewport cannot satisfy that dependency.

Visible native address-bar entry then reached `SuperNavigate2` for both fixture URLs, with the same IE process (3968), Chromium browser process (4856), embedded app HWND (`f01c0102`), CDP target (`A43F1B170EA32301871F96EFD8E386D6`) and session. [Page A](../build/mshtml-native-navigation-typed-a.png), [Page B](../build/mshtml-native-navigation-typed-b.png), and [Page B trace](../build/mshtml-native-navigation-typed-b.log) retain the evidence. Chromium rendered different page contents, but IE still displayed **Connecting…** and did not enable history. That is partial navigation, not a completed integration.

Further disassembly identifies another missing path:

1. [CDoc::InitDocHost](../build/mshtml-native-re/mshtml-x86.dll.disassembly/771ccbd0.txt), `771ccc9f`–`771cccb5`, obtains `IBrowserService` through `QueryService(SID_SShellBrowser, IID_IBrowserService)`. At `771ccd24`–`771ccd40`, it queries that browser for `IDocObjectService`, IID `{3050F801-98B5-11CF-BB82-00AA00BDCE0B}`.
2. The [actual CBaseBrowser2 table](../build/mshtml-native-re/ieframe-x86.dll.table-75cdac74.json), VA `75cdac74`, has 13 slots. Important offsets are `+10 FireNavigateComplete2(IHTMLWindow2*,DWORD)`, `+14 FireDownloadBegin()`, `+18 FireDownloadComplete()`, and `+1c FireDocumentComplete(IHTMLWindow2*,DWORD)`. Offsets are hexadecimal. The next pointer is another interface's QI thunk, not slot 13 of this interface.
3. Native [CWebOCEvents::NavigateComplete2](../build/mshtml-native-re/mshtml-x86.dll.disassembly/7741cb40.txt) calls service slot `+10` at `7741cdef`; [DocumentComplete](../build/mshtml-native-re/mshtml-x86.dll.disassembly/7741d1c0.txt) calls `+1c` at `7741d494`. Primary/top-level document-complete flags are zero; the subframe bit is 2. [FireDownloadEvents](../build/mshtml-native-re/mshtml-x86.dll.disassembly/7741e020.txt) calls `+14` and `+18` at `7741e1cb` and `7741e214` respectively.
4. IE's [FireNavigateComplete2](../build/mshtml-native-re/ieframe-x86.dll.disassembly/75e53959.txt) and [FireDocumentComplete](../build/mshtml-native-re/ieframe-x86.dll.disassembly/75e51b55.txt) both obtain the URL through [GetHTMLWindowUrl](../build/mshtml-native-re/ieframe-x86.dll.disassembly/75e4bf1b.txt). At `75e4bf2e` it queries `{3050F6DC-98B5-11CF-BB82-00AA00BDCE0B}`, then calls slot `+20` at `75e4bf44`.
5. That IID is **IHTMLPrivateWindow, without a numeric suffix**. It is not `IHTMLPrivateWindow3`, whose GUID bytes are `{30510411-98B5-11CF-BB82-00AA00BDCE0B}`. [CWindow::PrivateQueryInterface](../build/mshtml-native-re/mshtml-x86.dll.disassembly/77299950.txt), `7729a017`–`7729a03a`, constructs the old interface from the same `s_apfnIHTMLPrivateWindow4` table used for version 4 at `77299fc2`–`77299fe5`. Its `+20` slot is [GetAddressBarUrl](../build/mshtml-native-re/mshtml-x86.dll.disassembly/772ffe30.txt), which returns a BSTR from the markup URI.
6. With a URL, IE's [_FireNavigateComplete2](../build/mshtml-native-re/ieframe-x86.dll.disassembly/75e53761.txt) enters `_ActivateView` for top-level navigation (`75e5378e`). [_FireDocumentComplete](../build/mshtml-native-re/ieframe-x86.dll.disassembly/75e50fbb.txt) emits the browser event and posts the top-level completion message (`75e5106e`). Sending only `OLECMDID_SETDOWNLOADSTATE` does not execute these methods.

The [extended native probe](../build/mshtml-native-re/native-completion-contract-probe.txt) independently confirms QI for the old private-window IID and a successful `GetAddressBarUrl` returning `about:blank` on the real checked DLL. This rejects a GUID/name guess before installing an ABI alias.

The shim now exposes that verified prefix alias, sends command 13, and queries the native browser-event service. It gates navigation-complete on matching the acknowledged top-level Chromium loader, updates redirected committed URLs from that frame, then reports document-complete when the renderer is complete. Host callbacks remain on the owning apartment. Callback state is marked before calling out, with navigation-generation/site checks afterward; temporary service references are released after the notification pass.

The [guest shim contract test](../build/mshtml-completion-contract-test.txt) passes against DLL SHA-256 `ab11a01fe3982fb6c7276a7bf715d666a6022b76ef2eb97d88b5197107aefc77`. It tests the old IID/URL callback, two completion sequences without duplicate events, and reentrant site detachment during NavigateComplete without delivering stale subsequent completion callbacks. This is a mock-host test using real Chromium, not proof of full IE history or Windows hosting. Those remain tracked separately in [.unlazy/mshtml-native-navigation/GATES.md](../.unlazy/mshtml-native-navigation/GATES.md).

### Visible IE result after completion integration

The same DLL was then exercised in actual IE7, not just the mock container:

- [Page A](../build/mshtml-native-navigation-completion-a-live.png) and [Page B](../build/mshtml-native-navigation-completion-b-live.png) render different content and update both the IE tab and main window titles. The tab no longer remains on Connecting.
- [Redirect](../build/mshtml-native-navigation-completion-redirect.png) changes the displayed address from `/redirect` to the committed `/b`. The [fixture log](../build/mshtml-completion-fixture.log) records Chromium requests for the redirect and destination.
- [Back to Page A](../build/mshtml-native-navigation-completion-back-2.png) and [Forward to Page B](../build/mshtml-native-navigation-completion-forward.png) visibly work. The first Back traversed the duplicate B entry created by the redirect; the second Back reached A. The [Back trace](../build/mshtml-native-navigation-completion-back-2.log) shows IE calling `SuperNavigate2` on the existing window again. This demonstrates URL history traversal, not preservation of form values, scroll position, or script heap state.
- The corresponding `.health.txt` snapshots all identify IE PID 3772, Chromium browser PID 5024, embedded app HWND `f0170138`, viewport `f0210294`, and `responsive=1`. CDP frame `B2F931B56542F4D790B5C140458A923C` and session `431C6BFFBE971C4F4AB8F464C5A72611` remain the same. New renderer input HWNDs across different pages are normal; the containing app window did not change.
- [The post-repair trace](../build/mshtml-native-navigation-completion-b-live.log) records `MSHTML_BROWSER_NAVIGATE_COMPLETE=0` and `MSHTML_BROWSER_DOCUMENT_COMPLETE=0`, and actual host QI for the legacy private-window IID.

An earlier A attempt failed with `net::ERR_CONNECTION_REFUSED` because the bounded local fixture had expired; [that trace is retained](../build/mshtml-native-navigation-completion-a.log). A was retried after restarting the fixture. It is not counted as a successful navigation. An F5 input attempt did not yield a `Page.reload` acknowledgement in [its capture](../build/mshtml-native-navigation-completion-refresh.log), so refresh is **not** certified by that screenshot.

Further saved history listings are [IE `_ActivateView`](../build/mshtml-native-re/ieframe-x86.dll.disassembly/75e518d4.txt), [IE `_UpdateTravelLog`](../build/mshtml-native-re/ieframe-x86.dll.disassembly/75e4a2a4.txt), [IE `_SaveHistory`](../build/mshtml-native-re/ieframe-x86.dll.disassembly/75e4c9ea.txt), [CTravelEntry::Update](../build/mshtml-native-re/ieframe-x86.dll.disassembly/75f55fc5.txt), and native MSHTML [SaveHistory](../build/mshtml-native-re/mshtml-x86.dll.disassembly/771f2ec0.txt)/[LoadHistory](../build/mshtml-native-re/mshtml-x86.dll.disassembly/771f5470.txt). These are investigation artifacts, not a claim that native history-stream serialization has been implemented.

Remaining confirmed gaps include ignored `IPersistFile::Load`, missing view-advise behavior and history-state persistence, synchronous CDP I/O, unsupported POST/headers and new-window private-navigation flags, and broad `QueryService` acceptance. Slow-load Refresh/Stop, renderer-originated navigation events, network-error completion, full Windows hosting and x64 parity still require implementation or end-to-end proof. The integration goal is not complete.

Final re-verification rebuilt and TCP-deployed DLL SHA-256 `dee60f24e5ffcbec7b79d39d79f9bcb9a2c0051eeaae94372435358cd6eee504`. The navigation contract test and existing content, element, view and lifetime tests passed again. PE rebuild timestamps change the whole-file digest; the earlier visible sequence names its own tested build above.

## 2026-09-13 follow-up: retained events and asynchronous navigation

This section supersedes the earlier synchronous navigation implementation, not
the remaining full IE/Windows acceptance requirements. The deployed DLL is now
SHA-256 `460cf55e405f1f6f9ede9b8317c1610692c5447e3de2499f4dd1052e9d35f486`.
The previous working DLL was preserved in the guest as
`C:\TritonSupermiumBridge\mshtml-pre-event-transport.dll`. Native system DLLs,
the base QCOW2, activation settings, and launch-security policy were not changed.

[The pipe transport](../tools/mshtml_cdp_transport.h) now owns reads/writes on a
worker thread, routes replies by request/session, and retains watched Page
events separately. Request buffers outlive caller timeouts. Shutdown cancels a
blocked pipe writer and keeps a module reference until the worker exits. Closed
sessions unsubscribe and drain their pending messages. There are bounded request,
reply and event queues; overflow is an error, not silently discarded navigation.

[Apartment-owned navigation state](../tools/mshtml_navigation_events.h) consumes
those messages. URL navigation, reload and Stop queue commands without waiting
for network completion. Commit and ready state follow top-level frame/loader
and lifecycle events; title polling is asynchronous. This does **not** make all
COM methods nonblocking: initial renderer/session setup, synchronous DOM calls,
and target/browser teardown still contain waits.

Two regressions were found and repaired before accepting the deployed build:

- A no-loader acknowledgement for same-document navigation and its subsequent
  `Page.navigatedWithinDocument` event started duplicate download cycles. Commit
  now waits for the actual event. Frame URLs also retain CDP's separate
  `urlFragment` field.
- Delayed commit processing invalidated element wrappers created before the
  initial DOM generation had been settled. Document acquisition now processes
  prior events before publishing wrappers; stale element calls also process
  pending navigation events. The unchanged element identity/isolation test passes.

The [transport test](../tools/mshtml_transport_test.c) passed twice in Vista:
out-of-order replies, session-isolated unsolicited events, sync/async timeouts,
response overflow, protocol errors, disconnect, and blocked-writer cancellation.
The [expanded navigation test](../tools/ie7_mshtml_navigation_test.c), run with
`--events`, passed twice using real Chromium and the private HTTP fixture. It
supersedes a request whose headers are delayed twelve seconds, stops another
before commit, and waits beyond each delayed response to reject stale document
completion. Both command-return checks were below the 500 ms assertion threshold
(observed GetTickCount delta 0 ms, not a sub-millisecond measurement). It also
checks renderer link/fragment/redirect completion and reload. The cancelled,
uncommitted request produces no fabricated DocumentComplete.

To repeat `--events`, first run `podman exec -w /var/home/strix/winvistachecked triton-vista-x64-normal python3 tools/vista_mshtml_navigation_exercise.py fixture`
in a tracked terminal session and wait for its READY marker. The fixture binds
container loopback and expires after 900 seconds. The test fixture was stopped
after this verification; the final HTTPS page does not depend on it.

All existing navigation, content, element, view, and lifetime consumer tests
passed on the deployed DLL. These are mock COM hosts using the actual renderer,
not substitutes for IE toolbar/history tests. Current runnable evidence is in
[the event-transport ledger](../.unlazy/mshtml-event-transport/GATES.md); the
[guest integration trace](../build/mshtml-event-integration.log) includes the
regression runs. The navigation test EXE hash is
`59846203d0f07e98928d59ee539051d5751da93a881ebd69fca7f04d9d3ac609`.

### Visible IE: working behavior and failed checks

[The settled links page](../build/mshtml-native-navigation-event-links-settled.png),
[clicked fragment](../build/mshtml-native-navigation-event-fragment.png), and
[clicked redirect to B](../build/mshtml-native-navigation-event-link-redirect.png)
show native address/tab/title updates without another app window. The
[corresponding trace](../build/mshtml-native-navigation-event-link-history.log)
records renderer-originated navigation and successful native completion
callbacks. Health snapshots retain IE PID 3792, Chromium PID 4660, viewport
`f01f0168`, embedded app HWND `f0160120`, and `responsive=1`. Renderer input
children can change across commits without changing the embedded app.

**The following checks did not pass and remain open:**

- Back/Forward after this renderer-originated link sequence did not traverse
  history. [Back](../build/mshtml-native-navigation-event-link-back.png) and
  [Forward](../build/mshtml-native-navigation-event-link-forward.png) remained on
  B, with no new navigation command in the trace. Earlier typed-URL history
  success does not certify renderer-originated history.
- Native Refresh/Stop button attempts did not reach the shim's corresponding
  command entry. F5 from the address control reached the active-object
  accelerator path and returned S_FALSE, but did not produce a reload command.
  A repeated mouse attempt with a 150 ms button hold also produced no reload.
  [F5 trace](../build/mshtml-native-navigation-event-native-f5.log),
  [held-click trace](../build/mshtml-native-navigation-event-refresh-held.log),
  [slow-page/Stop trace](../build/mshtml-native-navigation-event-native-stop.log).
  The responsive slow-page screenshot does not prove Stop or Refresh worked.
- The first address-entry attempt raced initial renderer focus attachment;
  [its capture](../build/mshtml-native-navigation-event-links.png) is not counted
  as successful navigation. Entry was repeated after initial attachment settled.

IE was left visible on [HTTPS Example Domain](../build/mshtml-native-navigation-event-final-https.png)
using the same embedded app; [its trace](../build/mshtml-native-navigation-event-final-https.log)
records the committed HTTPS URL and native completion callbacks. This verifies
one successful TLS navigation, not all certificate/error-page semantics. No
certificate-validation or Chromium sandbox bypass was added.

### Additional native disassembly, not guessed command mappings

Binary provenance and all four PDB matches were reverified before extending
the symbol-addressed analysis:

- [CBaseBrowser2::_StopCurrentView, 75e465dd](../build/mshtml-native-re/ieframe-x86.dll.disassembly/75e465dd.txt)
  calls the command target at `+10`, with null group, command 23 and option 2.
- [CIEFrameAuto::Refresh2, 75dd5980](../build/mshtml-native-re/ieframe-x86.dll.disassembly/75dd5980.txt)
  calls `IUnknown_Exec` with null group and command 22 at `75dd59d3`.
  [CDocObjectView::Refresh](../build/mshtml-native-re/ieframe-x86.dll.disassembly/75e616c6.txt)
  likewise uses command 22, passing a VT_I4 input with value 4. The shim still
  needs complete refresh-input/cache-mode fidelity; inventing a different
  command group would not explain the missing native toolbar dispatch.
- [CDoc::UpdateTravelLog, 771dd3a0](../build/mshtml-native-re/mshtml-x86.dll.disassembly/771dd3a0.txt)
  has a top-level delegation path sending `CGID_Explorer` command **38**, with
  a VT_I4 flag value, through the client site at `771dd639`.
  [CBaseBrowser2::Exec](../build/mshtml-native-re/ieframe-x86.dll.disassembly/75e54970.txt)
  selects command 38 at `75e55001`, decodes the input at `75e550c8`, and calls
  `_UpdateTravelLog` at `75e55103`. The shim has no equivalent delegation yet.
- [CDoc::UpdateBackForwardState](../build/mshtml-native-re/mshtml-x86.dll.disassembly/771ddaa0.txt)
  separately calls its stored browser interface at vtable offset `+48`.
  [CWebOCEvents::BeforeNavigate2](../build/mshtml-native-re/mshtml-x86.dll.disassembly/7741d4d0.txt)
  and [CBaseBrowser2::FireBeforeNavigate2](../build/mshtml-native-re/ieframe-x86.dll.disassembly/75e48eff.txt)
  are now saved for the next contract work. These listings do not establish
  that firing a late BeforeNavigate event after a Chromium commit is valid;
  cancellation needs a pre-commit interception design.

The active full goal remains **make iexplore and Windows work with the
Supermium-based MSHTML shim**. Next required work includes renderer-originated
travel-log ownership, actual toolbar/accelerator dispatch, before-navigation
cancellation, network-error handling, POST/headers/new-window semantics,
persistence and history/form-state restoration, view advice/service identity,
nonblocking setup/teardown and setup-failure recovery, Windows hosts/x64 parity,
and durable installation/rollback. ActiveX page controls remain out of scope.
The passing worker and mock-host tests do not close those requirements.

The final build was relaunched interactively and left visibly displaying [HTTPS Example Domain](../build/mshtml-native-navigation-completion-https-final.png). The [final trace](../build/mshtml-native-navigation-completion-https-final.log) shows `SuperNavigate2` receiving `https://example.org/`, loader commit, and successful browser completion callbacks. Its [health snapshot](../build/mshtml-native-navigation-completion-https-final.health.txt) reports IE PID 788, Chromium PID 4184, app HWND `f00e0362`, viewport `f014029e`, and `responsive=1`. This verifies one public HTTPS page, not certificate-error/security-indicator parity: IE still displays Unknown Zone.

The per-user 32-bit HTMLDocument exercise override is intentionally still installed while that IE process is alive. The owned receipt is `build/mshtml-navigation-exercise.json`; close IE before running `python3 tools/vista_mshtml_navigation_exercise.py stop` to restore it. This is the reversible development registration, not a claim that the durable production installer or all Windows consumers are complete. Original system DLLs and the base disk were not replaced.

Unlazy re-verification: native audit 5 met; navigation implementation 4 met and 1 still open; no abandoned gates. The open integration gate retains the requirements listed above.

## 2026-09-13 follow-up: native DocObject activation and toolbar routing

### Native failure captured in the running process

The [bounded hardware-breakpoint trace](../tools/ie_native_trace.c) observed the
actual checked IE process, not a mock COM host. The first
[toolbar trace](../build/mshtml-native-refresh-routing-trace.log) follows
`CAddressBand::_OnCommand` into `CIEFrameAuto::Refresh2` and
`CDocObjectHost::ExecDown`. The [focused receiver trace](../build/mshtml-native-execdown-trace.log)
shows that host's cached view command target at `ExecDown this + 0x1b0` is NULL.
Refresh returns S_OK without a renderer call; Stop returns `0x80040100`
(`OLECMDERR_E_NOTSUPPORTED`). Coordinates and enabled button states were also
checked with the [toolbar inspection diagnostic](../tools/ie_window_health.c).

The trace used live IEFRAME base `0x72af0000`; its logged RVAs map to the verified
PDB-addressed listings. Important native facts:

- [MSHTML CServer::ActivateView, 7738f750](../build/mshtml-native-re/mshtml-x86.dll.disassembly/7738f750.txt)
  queries the client site for IOleDocumentSite and passes its IOleDocumentView to
  `ActivateMe` at `7738f841`.
- [IEFRAME CDocObjectHost::ActivateMe, 75e0d213](../build/mshtml-native-re/ieframe-x86.dll.disassembly/75e0d213.txt)
  calls `_CreateMsoView` when its view is absent.
- [_CreateMsoView, 75e0d09b](../build/mshtml-native-re/ieframe-x86.dll.disassembly/75e0d09b.txt)
  calls IOleDocument::CreateView, sets the in-place site, then queries the view
  for IOleCommandTarget at `75e0d1c9`. It saves that pointer at host base + `0x1cc`,
  the same field as `ExecDown this + 0x1b0` after the interface offset.
- [OnExec, 75e16c95](../build/mshtml-native-re/ieframe-x86.dll.disassembly/75e16c95.txt)
  checks that cache for Refresh at `75e16e66`. Its null case explains the observed
  false-success return. [ExecDown](../build/mshtml-native-re/ieframe-x86.dll.disassembly/75e182d8.txt)
  uses the same cache for downward command forwarding.

The shim's DoVerb had activated its HWND directly without calling ActivateMe.
That produced a visible page while bypassing IE's view ownership/command setup.
This is also contrary to the documented
[IOleDocumentSite::ActivateMe contract](https://learn.microsoft.com/en-us/windows/win32/api/docobj/nf-docobj-ioledocumentsite-activateme).
No guessed private command or UI click interception was needed to fix it.

ShowUI/HideUI were disassembled too, but their cache is a different field:
[ShowUI](../build/mshtml-native-re/ieframe-x86.dll.disassembly/75e01a18.txt),
[HideUI](../build/mshtml-native-re/ieframe-x86.dll.disassembly/75e01b5b.txt).
Their omission is still a host-UI contract gap; it was not substituted for the
proven ActivateMe failure.

### Repair and actual IE checks

The [shim](../tools/ie7_mshtml_activation_probe_shim.c) now delegates DocObject
activation to IOleDocumentSite, offers its view, retains itself across the
callback, rejects recursive activation, propagates container failure, and uses
ordinary in-place activation only when the site lacks IOleDocumentSite.
Unsupported DoVerb values no longer falsely activate a page.

DLL used for the native toolbar checks below, SHA-256:
`55024d6db301c5352dc34cd3777baab1728307399ec26d81639501f9e2f171eb`.
The prior DLL is retained as `C:\TritonSupermiumBridge\mshtml-pre-document-site.dll`.
Transfer used the existing hash-verified private TCP channel. IE was closed
before replacing the development DLL; no native system DLL or base disk changed.

The [expanded view test](../tools/ie7_mshtml_view_test.cpp) checks the ActivateMe
handoff, container failure propagation, cached command availability, unsupported
verbs, and balanced cleanup. It passed alone. The first regression-suite run
with interactive IE open failed a foreground-window assertion, not a COM
assertion; [failure](../build/mshtml-document-site-regression-failure.log) and
[retry](../build/mshtml-document-site-regression-retry.log) are preserved.
The desktop had resized from 800x600 to 1280x768, making the old fixed QMP
coordinate scale invalid; the exercise now reads actual display geometry before
each click. The foreground assertion was retained, with additional diagnostics.

Actual native toolbar evidence, all on IE PID 3884, Chromium PID 4144,
viewport `f02c01d2`, app HWND `f0200146`, responsive=1:

- Refresh advanced a no-cache server-generated counter from
  [1](../build/mshtml-native-navigation-counter-before-refresh.png) to
  [2](../build/mshtml-native-navigation-counter-after-refresh.png).
  [The trace](../build/mshtml-native-counter-after.log) records command 22,
  renderer acknowledgement, commit and native completion.
- An uncancelled slow resource produced
  [Slow resource finished](../build/mshtml-native-navigation-document-site-slow-positive.png),
  establishing the positive control. A second native reload was
  [still loading](../build/mshtml-native-navigation-document-site-slow-loading.png)
  when Stop was clicked after 1.5 seconds. After another 14 seconds the page
  [still had its original title](../build/mshtml-native-navigation-document-site-stop-verified.png):
  the delayed script did not run. [The trace](../build/mshtml-native-stop-verified.log)
  records command 23, Stop acknowledgement and native download completion.

The trace helper initially exposed two Vista/WOW64 handle-rights bugs; review
also found premature debug-event handle closes. The initial exit-255 cause was
not conclusively established. Failed trials are not presented as
successful clean detaches. The owned leftover breakpoints were explicitly
cleared; subsequent bounded tracing detached normally, and an independent
[inspection](../build/mshtml-native-trace-cleanup.log) verified no attached
debugger and zero debug registers on all 12 surviving target threads. Debug-event
process/thread handles are now left to the operating system's documented
[ContinueDebugEvent lifetime](https://learn.microsoft.com/en-us/windows/win32/api/debugapi/nf-debugapi-continuedebugevent).

### Still required

Renderer-originated native history remains unimplemented, not waived by toolbar
success. The native [ITravelLogClient table](../build/mshtml-native-re/mshtml-x86.dll.table-77048c3c.json)
contains FindWindowByIndex, GetWindowData and LoadHistoryPosition, whose native
implementations are now saved:
[FindWindowByIndex](../build/mshtml-native-re/mshtml-x86.dll.disassembly/7729a8b0.txt),
[GetWindowData](../build/mshtml-native-re/mshtml-x86.dll.disassembly/7729aa60.txt),
[LoadHistoryPosition](../build/mshtml-native-re/mshtml-x86.dll.disassembly/7729acc0.txt).
The receiver's [ITravelLog table](../build/mshtml-native-re/ieframe-x86.dll.table-75d3bed8.json)
distinguishes AddEntry, UpdateEntry and Travel. Command 38 flag interpretation,
pre-commit ordering and history-state serialization still need implementation
and actual native Back/Forward tests.

The next disassembly also establishes why returning the current URL alone is
insufficient: [CWindow::UpdateWindowData](../build/mshtml-native-re/mshtml-x86.dll.disassembly/7729ae80.txt)
creates a cached stream at `7729afe0` and calls CMarkup::SaveHistoryInternal at
`7729b0b3`. GetWindowData requires cached URL/title/stream, duplicates the strings
with task allocation, rewinds the cached stream and copies it into the supplied
stream. This is snapshot ownership, not a live DOM serialization callback after
the old document has already gone away. LoadHistoryPosition's saved listing
handles a non-primary window; its primary-window path returns E_FAIL, so it
must not be treated as the entire top-level Back/Forward contract.

### Regression re-verification and persistence caveat

The quiet-desktop full regression suite passed after the coordinate correction,
as did the worker and asynchronous-navigation integration tests. However, an
earlier quiet-desktop run failed loading a saved HTML stream into its second
document: [failure](../build/mshtml-document-site-regression-quiet.log),
[renderer trace](../build/mshtml-content-regression-detail.log). That trace did
not retain the failure HRESULT/CDP reply, so its root cause is not established.
The shim now records bounded failure replies and stream-load stages; the test
prints the second-document Load HRESULT. These are diagnostic changes, not a
claimed load fix. Eleven subsequent standalone content tests and the complete
regression suite passed with unchanged assertions. Intermittent stream-load
reliability remains open under the full goal.

Current diagnostic DLL SHA-256:
`804e7328c60ff098e1a69b7511c229038a4209b79b07cf909861d14348729887`.
Its ActivateMe repair is unchanged from the toolbar-tested build. The original
pre-ActivateMe development DLL backup is still retained.

The current diagnostic build also passed a fresh actual-IE toolbar exercise:
[counter 1](../build/mshtml-native-navigation-diagnostic-counter-one.png),
[counter 2](../build/mshtml-native-navigation-diagnostic-counter-two.png),
[slow positive control](../build/mshtml-native-navigation-diagnostic-slow-positive.png),
[reload still loading](../build/mshtml-native-navigation-diagnostic-slow-loading.png),
[Stop verified after 14 seconds](../build/mshtml-native-navigation-diagnostic-stop-verified.png),
and [command trace](../build/mshtml-native-diagnostic-toolbar.log).
All retain IE 4436, Chromium 4156, app HWND `f010030a`, viewport `f01e0328`,
with responsive=1.

History was retested on that build, not inferred from the earlier failure:
[fragment](../build/mshtml-native-navigation-diagnostic-fragment.png) and
[link redirect](../build/mshtml-native-navigation-diagnostic-link-redirect.png)
update the native URL/title. Native Back then
[skips to the earlier typed slow-page](../build/mshtml-native-navigation-diagnostic-link-back-settled.png)
instead of returning to links#section; the
[trace](../build/mshtml-native-diagnostic-history.log) confirms the requested
slow-page URL. The native button now routes, but renderer-originated entries
are still missing. The page and app remain embedded in the same IE context.

The broader goal remains unchanged: IE and Windows consumers using the
Supermium-based MSHTML replacement, with page ActiveX controls out of scope.
Unresolved items include host UI/advice contracts, history/form restoration,
before-navigation cancellation, POST/headers/new-window behavior, network and
certificate errors, refresh cache modes, persistence, nonblocking setup/teardown,
Windows hosts/x64 and durable installation/rollback. Native toolbar success is
not full browser compatibility.

The latest session is left visibly displaying
[HTTPS Example Domain](../build/mshtml-native-navigation-diagnostic-https-final.png).
Its [health snapshot](../build/mshtml-native-navigation-diagnostic-https-final.health.txt)
retains IE 4436, Chromium 4156, app HWND `f010030a` and viewport `f01e0328`,
responsive=1; the [final trace](../build/mshtml-native-diagnostic-final.log)
records the HTTPS commit and browser completion. This does not certify IE zone
or certificate-indicator parity. The controlled loopback HTTP fixture was
stopped after the tests. The owned per-user exercise registration remains
installed for the live IE session; the original native DLLs and base disk remain
untouched.

Current unlazy verification: native audit **5 met, 0 unmet**;
event/navigation component **5 met, 1 unmet** (renderer-created native history);
**0 abandoned**. The intermittent content-load caveat and broader full-goal
requirements above remain tracked rather than being described as solved.

## Follow-up: native travel-log streams and position cookies (2026-09-13)

This section supersedes the earlier statement that renderer-originated history
delegation is entirely missing. It does not supersede the remaining full-goal
requirements or turn partial tests into full browser compatibility.

### Further instruction-level findings

- Command 38's flags are now traced through actual instructions, not inferred
  from the name of a nearby interface. In
  [CBaseBrowser2::Exec](../build/mshtml-native-re/ieframe-x86.dll.disassembly/75e54970.txt),
  `75e54d1f` puts **4**, not 1, into EDX. At `75e550c8`, bit 0 is the
  local-anchor flag, bit 1 forces update, and bit 2 selects window
  ITravelLogClient. The ordinary top-level
  [CDoc sender](../build/mshtml-native-re/mshtml-x86.dll.disassembly/771dd3a0.txt)
  sets only bits 0/1 at `771dd5cf..771dd5ed`. This path uses document
  IPersistHistory through the native browser; implementing only window
  ITravelLogClient would not satisfy it.
- Native [browser SaveHistory](../build/mshtml-native-re/ieframe-x86.dll.disassembly/75e4c9ea.txt)
  queries the document's IPersistHistory at `75e4cba7`, obtains its class ID,
  writes IE's persisted-frame header/PIDL and calls document SaveHistory at
  `75e4cc59`. Native
  [LoadHistory](../build/mshtml-native-re/ieframe-x86.dll.disassembly/75e4c5dd.txt)
  can reuse the existing HTMLDocument: QI at `75e4c7d2`, LoadHistory at
  `75e4c7fd`. Its separate object-creation fallback is **not** supported by
  the shim's live-context-only history format yet.
- Local-anchor entries have another branch:
  [CTravelEntry::_PersistHistoryToStream](../build/mshtml-native-re/ieframe-x86.dll.disassembly/75f53263.txt)
  queries `{4F77BADD-A032-41C7-B32F-0B44570DF139}` and calls slot `+20`
  with a stream and an `Internal Navigation` bind context. The native
  [IPersistHistory2 table](../build/mshtml-native-re/ieframe-x86.dll.table-75cdad10.json)
  identifies that slot as SaveHistoryEx. The
  [browser implementation](../build/mshtml-native-re/ieframe-x86.dll.disassembly/75e4cd63.txt)
  delegates recognized internal-navigation contexts to the document's same
  extension. When this interface is unavailable,
  [entry Update](../build/mshtml-native-re/ieframe-x86.dll.disassembly/75f55fc5.txt)
  falls back to GetPositionCookie, and
  [entry Invoke](../build/mshtml-native-re/ieframe-x86.dll.disassembly/75f52e44.txt)
  can restore through SetPositionCookie instead of LoadHistory.
- That cookie is not an arbitrary opaque field in this checked engine:
  [CMarkup::GetPositionCookie](../build/mshtml-native-re/mshtml-x86.dll.disassembly/77274fa0.txt)
  returns the layout's Y scroll position. Its
  [setter](../build/mshtml-native-re/mshtml-x86.dll.disassembly/77275020.txt)
  calls NavigateHere with that position. The document delegates through
  [CWindow::GetPositionCookie](../build/mshtml-native-re/mshtml-x86.dll.disassembly/7729bda0.txt)
  and [SetPositionCookie](../build/mshtml-native-re/mshtml-x86.dll.disassembly/7729be70.txt).
  The first shim implementation incorrectly treated it as a stored number;
  that assumption and the associated test were corrected, not retained as a
  compatibility requirement.

### Implementation and the initial native failure

[History implementation](../tools/mshtml_history.h) now exposes document
IPersistHistory with canonical document IUnknown identity, captures Chromium
history-entry IDs, and restores through Page.navigateToHistoryEntry. The
versioned, bounded stream contains a per-page-context GUID, entry ID and UTF-16
URL. Foreign contexts, invalid headers, oversized lengths, truncated payloads
and entry/URL mismatches fail instead of silently loading an unrelated fresh
URL. The engine retains actual form/scroll state; this is not cross-session
state serialization. Ordinary commit snapshots run on the asynchronous CDP
worker. Explicit synchronous COM persistence calls still perform bounded CDP
queries.

For renderer-created entries, command 38 exports the previous entry's retained
snapshot before publishing native navigation completion. The exporting snapshot
is detached from document ownership across reentrant host COM callbacks.
JavaScript-driven back/forward cursor synchronization, fast overlapping commits,
subframe travel logs and cross-session restoration remain unfinished.

The first native run on DLL
`2e7ff09c04c6a663f840a4f41077cd9304236b86a5bd4ae803e87cc230d2547f`
did improve the original symptom: after a
[fragment](../build/mshtml-native-navigation-history-native-fragment.png) and
[redirect](../build/mshtml-native-navigation-history-native-redirect.png),
[Back returned to links#section](../build/mshtml-native-navigation-history-native-back.png).
But [a second Back](../build/mshtml-native-navigation-history-native-back-two.png)
changed IE's address through the cookie fallback without changing Chromium's
entry. [Forward then stalled](../build/mshtml-native-navigation-history-native-forward.png):
the [trace](../build/mshtml-history-native-second.log) shows LoadHistory restoring
an already-current Chromium entry, whose acknowledgement has no frame event.
These captures are a failed end-to-end test, not a passing history gate.

The subsequent patch supplies the receiver's
[extended stream ABI](../tools/mshtml_history_contract.h), supports the recognized
Internal Navigation SaveHistoryEx context, implements the cookie as actual
renderer scroll position, and settles already-current history restores on their
acknowledgement. This extends the document using an interface IE explicitly
queries; it is not a claim that native MSHTML's public symbols contain a
CDoc::SaveHistoryEx implementation.

The expanded [guest test](../tools/ie7_mshtml_navigation_test.c) checks extended
interface identity, unsupported bind contexts, actual scroll-cookie behavior,
live form/900-pixel scroll restoration, old-entry export through SaveHistoryEx,
already-current restoration, and malformed/foreign-stream negative controls.
Direct guest job `3c959edc2d0549048a088c2f78b5e8f1` passed on patched DLL
`547d18b769275aab5af6143d039fa0194c5d7a5411cf83d56a6ae68f990595d6`.

A separate regression failure remains explicit:
[element test](../build/mshtml-history-regression-failure.log),
[CDP trace](../build/mshtml-history-regression-detail.log).
On the first history DLL, IHTMLElement::get_innerText timed out with
`800705b4`, followed by Runtime.releaseObject timeouts; Target.closeTarget
still succeeded. No root cause or reliability fix is claimed. This is retained
alongside the previous intermittent content-stream failure under the full goal.

### Departure-entry ownership during Back/Forward

The extended-stream patch alone did not preserve the final Forward entry.
[Back to the fragment](../build/mshtml-native-navigation-history-confirmed-back-one.png)
restored the typed `native-history-state` text, but the
[last Forward](../build/mshtml-native-navigation-history-confirmed-forward-two.png)
restored that fragment again instead of B. In the
[trace](../build/mshtml-history-native-extended.log), IE had never saved B's
departure state; a later lazy save captured the page already being displayed.
An earlier `history-fixed-*` capture set stayed on about:blank because input
was sent during initial browser activation; it is not used as navigation
evidence. The later `history-confirmed-*` run first verified the fixture page.

[CTravelLog::_TravelToEntryInternal](../build/mshtml-native-re/ieframe-x86.dll.disassembly/75f51e85.txt)
preserves a departure pointer before switching its current entry and invoking
the destination. The extended
[UpdateEntry](../build/mshtml-native-re/ieframe-x86.dll.disassembly/75f5623e.txt)
selects that departure pointer when present. The ordinary browser
[LoadHistory path](../build/mshtml-native-re/ieframe-x86.dll.disassembly/75e4c5dd.txt)
sets its history-travel bit before document LoadHistory. A forced command 38
update can therefore save the departing Chromium entry without appending a
new native entry. The shim now does this only after validating the input stream
and **before** queuing Chromium's restore.

There is a distinct internal-navigation path that must not repeat this update:
[CBaseBrowser2::LoadHistory](../build/mshtml-native-re/ieframe-x86.dll.disassembly/75e5688a.txt)
recognizes the `Internal Navigation` bind context and calls
[_NavigateWithinView](../build/mshtml-native-re/ieframe-x86.dll.disassembly/75e5477e.txt).
That routine already calls ITravelLog::UpdateEntry at `75e5488c`, then document
LoadHistory at `75e548a0`. Repeating command 38 there appends an entry and
truncates Forward. The intermediate
[failed trial](../build/mshtml-native-navigation-history-departure-forward-one.png)
and [trace](../build/mshtml-history-native-departure.log) are retained. The
current code leaves this internal departure update to IE and tests that no
duplicate host update occurs. The same routine fires native navigation events
after its call returns; complete internal-navigation event timing with the
asynchronous renderer remains a broader contract to audit, not certified merely
by a settled screenshot.

### Final checked-IE history and toolbar verification

The corrected build
`1c3519cd438089babfdb8c25f1a0ac43f0e6a1887975f34a649ef523373787bd`
was deployed as the owned 32-bit HTMLDocument override and exercised through
the actual IE7 chrome. The sequence used a renderer link to create
`/links#section`, another renderer link through `/redirect` to `/b`, two native
Back clicks and two native Forward clicks. The resulting captures show:

- [Back one](../build/mshtml-native-navigation-history-final-back-one.png) at
  `/links#section` and [Back two](../build/mshtml-native-navigation-history-final-back-two.png)
  at `/links`, both retaining the typed `native-history-state` value.
- [Forward one](../build/mshtml-native-navigation-history-final-forward-one.png)
  returns to `/links#section`; [Forward two](../build/mshtml-native-navigation-history-final-forward-two.png)
  returns to `/b` rather than truncating or duplicating the forward list.
- Every associated health capture reports IE PID 4248, Chromium PID 2912,
  IE HWND `f03e0368`, viewport `f0490140`, app `f01f031a` and `responsive=1`.

The [final trace](../build/mshtml-history-native-final.log), SHA-256
`cf1013e540fcb6cfe7eb29d8215c36de20d70aa69ff8f0eab96c2ec9ca528963`,
shows Chromium entries 6/8/9 for links/fragment/B, successful SaveHistoryEx and
LoadHistory calls, renderer commits for every restored URL, and successful IE
NavigateComplete/DocumentComplete callbacks. Ordinary travel performs the one
forced departure save needed by `_LoadHistory`; the two local-anchor traversals
log `MSHTML_INTERNAL_HISTORY_DEPARTURE_HOST_OWNED`, confirming that the shim no
longer duplicates `_NavigateWithinView`'s host update.

Native toolbar behavior was repeated without changing the DLL. Refresh advanced
the controlled endpoint from
[count 1](../build/mshtml-native-navigation-history-final-counter-one.png) to
[count 2](../build/mshtml-native-navigation-history-final-counter-two.png).
The slow-script [positive control](../build/mshtml-native-navigation-history-final-slow-positive.png)
reached `Slow resource finished`; after a second Refresh, Stop left
[Navigation SLOW-PAGE](../build/mshtml-native-navigation-history-final-stop-verified.png)
after the 12-second resource delay. The trace contains command 22, command 23,
`MSHTML_STOP_ACKNOWLEDGED` and balanced host download completion.

The final deployed navigation test
`798c9021cc5f3982caab48bea790dafc9cd7f6f7aec6ca6d7a1f7a5ca6297697`
also passes the event, ordinary document, content, element, view, lifetime and
expanded history suites. This closes the retained-events/history component's
seven acceptance gates. It does **not** close the full IE/Windows integration
goal: renderer-initiated history traversal, fast overlapping commits, subframe
travel logs, cross-session persistence, POST/header semantics, new-window
behavior, x64 and other Windows hosts, security UI, and durable deployment
remain in scope. The intermittent persistence failures documented above also
remain reliability defects rather than being erased by a passing rerun.

## Follow-up: renderer History API traversal (2026-09-13)

The remaining renderer-history symptom was not fixed by copying Chromium's
already-moved cursor back into IE. Checked instruction traces show why.

[CDoc::Travel](../build/mshtml-native-re/mshtml-x86.dll.disassembly/771dd850.txt)
calls ITravelLog slot `+18` as `Travel(travelLog, browserIdentity, offset)`.
The zero visible earlier in the function belongs to checked `DbgExTraceHR`; it
is not a third COM argument. The first shim declaration included that zero and
therefore unbalanced the x86 stdcall stack. That crashing trial remains in
[its trace](../build/mshtml-renderer-history-travel-crash.log). After correcting
the ABI, [CTravelLog::Travel](../build/mshtml-native-re/ieframe-x86.dll.disassembly/75f55a2a.txt)
ran, but querying the checked native log immediately afterward returned
`E_FAIL` for the newly required opposite direction. Updating toolbar state did
not recreate the missing entry. The relevant state callback is browser-service
slot `+0x48`, established by
[CDoc::UpdateBackForwardState](../build/mshtml-native-re/mshtml-x86.dll.disassembly/771ddaa0.txt)
and
[CShellBrowser2::UpdateBackForwardState](../build/mshtml-native-re/ieframe-x86.dll.disassembly/75ec7dc6.txt).

A bounded checked-IE hardware-breakpoint run provides independent control-flow
evidence. [The breakpoint log](../build/mshtml-renderer-native-travel-breakpoints.log),
SHA-256 `6e8c703838351e7f9f490406fd63b02c554243fb791f6130bd4dec6332abaa14`,
hits `CTravelLog::Travel` with the expected browser identity and `-1`, while
the entry-add/update/prune paths do not run during that retroactive travel.
[GetTravelEntry](../build/mshtml-native-re/ieframe-x86.dll.disassembly/75f53bff.txt),
[_FindEntryByOffset](../build/mshtml-native-re/ieframe-x86.dll.disassembly/75f53b68.txt),
[CanInvoke](../build/mshtml-native-re/ieframe-x86.dll.disassembly/75f52acf.txt),
and [Invoke](../build/mshtml-native-re/ieframe-x86.dll.disassembly/75f52e44.txt)
were saved from the checked PDB-backed binary. Together with the live `E_FAIL`
probe, these disprove the retroactive design for this host.

The shim now interposes ordinary `history.back()`, `history.forward()` and
nonzero `history.go()` before Chromium changes entries. A per-document
`Runtime.addBinding` named `__tritonHistoryTravel` carries only the signed
offset over the existing TCP/CDP channel. The retained-event worker dispatches
that binding to native ITravelLog::Travel. IEFRAME advances its own cursor,
calls the document's IPersistHistory::LoadHistory, and that existing path
issues exactly one Page.navigateToHistoryEntry. Zero, nonnumeric, and
out-of-range offsets remain no-ops or use Chromium's original zero reload
behavior as appropriate.

Actual checked IE7 on DLL
`d111ca79d833614a4a6835229fad7a67c251e6a74f31db1515ca10f1a46af2e5`
then completed the following sequence in the same browsing context:

- renderer link `/links` to [/b](../build/mshtml-native-navigation-renderer-hook-b.png);
- page-script [Back to /links](../build/mshtml-native-navigation-renderer-hook-script-back.png),
  with native Forward enabled;
- [native Forward to /b](../build/mshtml-native-navigation-renderer-hook-native-forward.png)
  and native Back to `/links`;
- page-script [Forward to /b](../build/mshtml-native-navigation-renderer-hook-script-forward.png),
  followed by [native Back to /links](../build/mshtml-native-navigation-renderer-hook-native-back-after-forward.png).

All six associated health captures retain IE PID 4260, Chromium PID 2364,
IE HWND `f04601d2`, viewport `f0470336`, app `f039018c`, and `responsive=1`.
The [final trace](../build/mshtml-renderer-history-hook-final.log), SHA-256
`859b71ecd06c754fd7083531c7b0aaf7c2ef563db38f77691d66e3062f0ccbb0`,
records native SaveHistory/LoadHistory, each renderer travel request, one CDP
history acknowledgement, and the matching commit. Navigation-test build
`d14137086a9844306dc42074c1be05fc3292671cd40790d7ac9030409a1c4540`
also covers both directions and passed guest job
`6bfa5e580fb94f7f82ce43f147658baa`.

This establishes the ordinary page History API path; it is not a blanket
claim for every JavaScript escape hatch.

The next hardening build moved the interposition from own properties on the
`history` object to the top-level window's `History` prototype. It therefore
covers direct calls such as `History.prototype.forward.call(history)`, while
preserving the original method for a foreign receiver. New-document injection
explicitly ignores subframes so a frame-local call cannot accidentally advance
the top-level IE travel log. DLL
`5ebe6268467d5d38a9dfd543c35cada2ce46917e10ae93f1ea9a97338ecd5d3c`
and navigation test
`b819a8cbea2dee05590dea8acfccbe6a4568669a675e422489e2f2f43241ff80`
passed direct prototype Forward and `history.go(-1)` in guest job
`c4a3cdd16949406c848871224bf62057`, then passed all transport, async,
document/content/element/view/lifetime, and persistence regressions.

The hardened DLL was also repeated visibly: [/links](../build/mshtml-native-navigation-renderer-prototype-final-links.png)
to [/b](../build/mshtml-native-navigation-renderer-prototype-final-b.png),
[script Back](../build/mshtml-native-navigation-renderer-prototype-final-script-back.png),
[native Forward](../build/mshtml-native-navigation-renderer-prototype-final-native-forward.png),
and [native Back](../build/mshtml-native-navigation-renderer-prototype-final-visible.png).
Every health capture retains IE PID 2076, Chromium PID 3944, IE HWND
`f06d02fa`, viewport `f034034c`, app `f0590368`, and `responsive=1`. The
[hardened trace](../build/mshtml-renderer-history-prototype-final.log), SHA-256
`379a36ec1ae4b6c151091c0a9518e65dc03762aa6652e4760b02715cd6c531d5`,
contains the corresponding native Travel, SaveHistory/LoadHistory, CDP
acknowledgement and renderer commit.

The original repeated-address-bar failure was separately rerun before the
prototype hardening: native `/links` ->
[/a](../build/mshtml-native-navigation-renderer-hook-final-typed-a.png) ->
[/b](../build/mshtml-native-navigation-renderer-hook-final-typed-b.png) stayed
in IE PID 4144, Chromium PID 2476, IE HWND `f06402fa`, viewport `f061012c`,
and app `f04702ec`, with `responsive=1`. It did not launch a second independent
Chrome UI or stall on the second address.

A page that deliberately replaces the installed prototype methods or invokes
the exposed test helper, genuine subframe history synchronization, and rapid
overlapping travel still need separate compatibility work. The unchanged full
goal also retains cross-session persistence, POST/header semantics, new-window
behavior, x64 and other Windows hosts, security UI, and durable deployment.

## Follow-up: fast overtakes and missing-event reconciliation (2026-09-13)

The event consumer now has an explicit fast-overtake case. It queues a stalled
`/slow-headers` navigation and `/a` immediately, requires the two calls together
to return in under 500 ms, accepts exactly one completion for `/a`, waits beyond
the stale request's 12-second response delay, and then rechecks IE's address,
document title, completion count and event ordering. The final test passes this
case as `MSHTML FAST NAVIGATION OVERTAKE PASSED`; request cookies and loader IDs
prevent the superseded acknowledgement or frame from publishing native state.

During the all-suite rerun, the first history attempt failed because the
900-second fixture had expired and correctly surfaced Chromium's error page.
After a fresh fixture, a different retained reliability failure recurred:
`Page.navigate` acknowledged `/history-a` and the fixture received it, but no
main-frame event reached the shim before the consumer deadline. The
[preserved user-context trace](../build/mshtml-history-prototype-regression-failure-user.log),
SHA-256 `d72541102a8bd90d3158d28da2e5e2a575aefd75e560e795c585c68a6c5a0005`,
shows the acknowledgement and missing commit rather than a fabricated root
cause. A standalone retry passed, matching the earlier intermittent symptom.

The final implementation retains Page events as the primary truth and adds a
nonblocking reconciliation path. After a successful top-level Page.navigate
acknowledgement, the apartment timer may asynchronously request
`Page.getFrameTree`. It accepts a result only when the returned main-frame
loader exactly matches the current acknowledgement's loader, then separately
polls `document.readyState` through an asynchronous Runtime request. A new
navigation invalidates both probe IDs through the existing serial/cookie
generation, so a late probe cannot complete an overtaking request. No control
path waits synchronously for either probe.

The negative control sets a one-shot test environment switch immediately before
`/history-a` and deliberately drops that main-frame event. Guest job
`6d19e75b29b041c9ac8933a415e4ff9a` still completed and printed
`MSHTML LOST FRAME EVENT ASYNC RECONCILIATION PASSED`. The
[recovery trace](../build/mshtml-navigation-frame-recovery-final.log), SHA-256
`3af9bbb62f23d75143946f1c7ba36648b80e182ad17699c3d0315855916993c8`,
records `MSHTML_TEST_MAIN_FRAME_EVENT_DROPPED`, the matching `/history-a`
commit, and `MSHTML_NAVIGATION_FRAME_PROBE_RECOVERED` in order. Five consecutive
full history runs with that drop injected passed, followed by a full ten-gate
reverification on final DLL
`4c3e7273f01d25976e45a5cafaa26f3d8095f761171416ccb32dc8f687f34225`
and navigation test
`8fcea6f4f14f028581ab9a2d454d1755025dff244b26eab1ac5835eb85226441`.

The final DLL was then repeated in visible checked IE7:
[/links](../build/mshtml-native-navigation-frame-recovery-final-links.png) ->
[/b](../build/mshtml-native-navigation-frame-recovery-final-b.png) ->
[script Back](../build/mshtml-native-navigation-frame-recovery-final-script-back.png) ->
[native Forward](../build/mshtml-native-navigation-frame-recovery-final-native-forward.png) ->
[native Back](../build/mshtml-native-navigation-frame-recovery-final-visible.png).
All five health captures retain IE PID 4412, Chromium PID 4148, IE HWND
`f099033e`, viewport `f0540380`, app `f07901a0`, and `responsive=1`. The VM is
left visibly on `/links` with native Forward enabled.

This closes the known fast top-level overtake and missing-main-frame-event
cases, not the full integration goal. Subframe travel logs, cross-session
persistence, POST/header semantics, new-window behavior, x64/other Windows
hosts, security UI, and durable general deployment remain in scope. The older
intermittent IPersistStreamInit content-load failure remains separately tracked.

## Follow-up: remaining IE7 host contracts and durable deployment (2026-09-13)

The remaining bounded IE7 integration contracts are now implemented and have
runnable guest checks. Subframe lifecycle events retain Chromium's frame ID,
parent ID, loader ID and URL, but only the main-frame path may update IEFRAME's
top-level travel log. Both same-origin and cross-origin frame navigations leave
the native top-level cursor and entry count unchanged. The guest consumer prints
`MSHTML SUBFRAME TRAVEL LOG ISOLATION PASSED` after exercising both cases.

`IPersistHistory::SaveHistoryEx` now emits a versioned `TRI7HST2` restart stream
when IE supplies the `Triton Cross Session` bind context. The stream contains a
bounded URL plus form values, checked states, selected indices and scroll state,
and includes an integrity checksum and source origin. `LoadHistory` can create a
fresh Chromium process and document from that stream after the original object
and renderer have been destroyed. Truncated, modified, and origin-mismatched
streams fail without navigation. This is separate from the existing live
Chromium-history-entry format and is covered by
`MSHTML CROSS-SESSION HISTORY RESTORE PASSED`.

The navigation adapter preserves POST bytes, content type, and caller-supplied
headers when building Chromium's request. Redirect handling follows the method
transition reported by Chromium and strips caller headers before a cross-origin
request. The fixture verifies exact body/header receipt, same-origin redirect
behavior, and non-leakage at its second origin; the guest reports
`MSHTML POST HEADER AND REDIRECT CONTRACT PASSED`.

New-window disposition is owned by the Windows host. A top-level CDP binding
interposes `window.open` and `target=_blank`, forwards URL, target name, opener
source and the native new-window flag through IE's navigation callback, and
uses `IWebBrowser2::Navigate2` as the standalone fallback. Chromium is not
allowed to leave an independent top-level Chrome window. The native consumer
checks script, anchor and direct-call cases and reports
`MSHTML HOST OWNED NEW WINDOW CONTRACT PASSED`. A script-visible proxy for the
returned child `window` object is not implemented yet; the interposed
`window.open` currently returns null after handing ownership to the host.

The same shim source now builds as reproducible Vista x86 and x64 DLLs. The x64
contract host directly loads the x64 DLL and exercises COM identity, document,
window, navigation event, reentrant detach and unload behavior outside IE. The
result is `MSHTML X64 AND ALTERNATE HOST CONTRACT PASSED`. ActiveX remains
explicitly out of scope and unsupported rather than being silently forwarded.

The document also exposes an `IInternetSecurityManager` service that delegates
zone mapping and URL actions to Windows' default manager. Navigation state
reports the IE-owned secure-lock command for HTTP, valid HTTPS, certificate
failure, navigation error and attachment/download transitions. Together with
the narrowly owned Supermium Low Rights elevation policy, this avoids the
unsigned external-program prompt without disabling Protected Mode, zone policy,
or certificate checks. The test covers the local HTTP fixture, valid public
HTTPS, an expired-certificate endpoint, and a local attachment response, and
prints `MSHTML IE SECURITY UI CONTRACT PASSED`. This is the tested IE7 surface,
not a claim that every later MSHTML security-service extension exists.

`tools/vista_mshtml_install.py` is the durable deployment entry point. It builds
reproducible x86/x64 payloads, stores each at a SHA-256-versioned immutable path,
writes exact per-user 32-bit and 64-bit COM registrations, and records an owned
receipt before mutation. Repeated install is a verified no-op. A foreign or
drifted registration is preserved and rejected. A Vista-native helper verifies
guest hashes and enumerates loaded modules in both architectures, including
non-browser processes such as Sidebar; a changed payload is never overwritten
while mapped. Rollback first restores the exact recorded registry state, then
deletes unused owned files or schedules locked ones for deletion. Reboots use
Windows shutdown rather than QMP reset so the user hive and pending rename list
are flushed durably.

The installer verification performs foreign-state refusal, install twice,
real x86 and x64 `CoCreateInstance(HTMLDocument)` live-module refusal, reboot
status, exact rollback, final reinstall, and a second reboot/status check. It
finishes with `MSHTML DURABLE INSTALL ROLLBACK AND REBOOT PASSED`. The complete
retained-navigation ledger is now 17/17. A fresh aggregate guest run also passed
subframes, cross-session restore, POST/headers, new windows, x64 alternate-host,
and security UI in one job (`a703b39f2140458db0d87cc1338fc748`).

This completes the requested bounded bring-up, not a wholesale binary-compatible
replacement for every MSHTML feature. Generated interface methods still marked
`E_NOTIMPL`, ActiveX, HTA-specific hosting, script access to a returned popup
proxy, and untested third-party host assumptions remain compatibility work.

## Follow-up: real WindowProxy and native address input (2026-09-13)

This section supersedes the null-popup-proxy limitation and the earlier native
navigation confidence above; the older 17/17 claim did not cover the failures
found by the actual installed-IE exercise below.

The previous popup callback test did not establish usable child-window
semantics: its mock host accepted a notification, while the injected hook
returned null. The navigation tests also loaded a fixed development DLL instead
of the versioned DLL selected by the installed COM registration. The navigation
consumer now reads its architecture's registered InprocServer32 path and prints
the exact DLL being tested.

The popup hook now retains the original Chromium `window.open` return value.
Chromium owns WindowProxy identity, synchronous same-origin DOM access, origin
checks, location changes, named-window reuse and closed state. The shim discovers
popup targets and moves their renderer HWNDs into `TritonMshtmlPopup` windows in
the IE process. Those hosts handle resizing, focus and closing and retain the
runtime while the child lives. Popup blocking and noopener/noreferrer behavior
remain Chromium decisions. This replaces the former fabricated null result;
it does not emulate cross-origin DOM access in native code.

The fixture `tests/fixtures/mshtml-popup-proxy.html` checks a returned reference,
opener identity, synchronous document.write, child navigation, named reuse,
cross-origin SecurityError, return to same-origin access and close/closed. The
automated consumer sends a renderer click and separately requires one visible
native popup host. A script-only invocation was rejected by native popup
blocking, which is why a real input event is required for the positive control.

Native navigation inspection reproduced swallowed Ctrl+L when the embedded
renderer held focus. The hook now posts an explicit address-focus request to the
document's apartment, which focuses and selects the native address edit. View
reactivation and renderer commits also preserve focus when the user is typing
in a native edit control, rather than redirecting subsequent input to the page.
The viewport has a background brush and popup teardown invalidates it to prevent
stale pixels at the edge of the embedded renderer.

`tools/vista_mshtml_native_navigation_check.py` operates the actual IE window:
Ctrl+L from renderer focus, typed local addresses, a clicked link, public HTTPS,
native Back and Forward. It observes native titles and checks that both the IE
window and Chromium app window identities survive the sequence. Its fixture is
checked before input; if no IE is open, a uniquely named interactive scheduled
task launches one outside the control-service job. The test deletes its task
and closes only its own IE process. Current acceptance and
fresh evidence are recorded in `.unlazy/mshtml-popup-navigation/GATES.md`.

Native Back exposed another reuse-contract defect: SaveHistory during incoming
view activation queried Chromium after it had already committed the destination,
serializing that destination into the outgoing IE travel entry. Host-requested
navigation now captures the departure before submitting Page.navigate and
exports it during that activation interval. An already-current LoadHistory also
refreshes the current snapshot synchronously and invalidates old pending replies;
otherwise a subsequent history.forward binding could see a stale entry and
return E_PENDING without performing travel.

Installed follow-up payloads: x86 SHA-256
`5e62b673892add6e860adb86a83485c3024115a7d0e78d38c7fde77a5de7145d`,
x64 `ce93a059f56acfe6f51767889dbc10bd6452c5a3add4fb887d5e200f782de2d7`.
All three follow-up gates passed on these versioned registrations: native input
and travel; real Chromium WindowProxy semantics plus native popup adoption;
and installed x86/x64 history, restart state, POST/headers, subframes and security
regressions. This is bounded bring-up evidence, not complete MSHTML compatibility.
The additional installed `--events` run also passed slow-response supersession,
Stop, fast overtaking, renderer links/fragments/redirects and reload. Its observed
slow-navigation submission was 47 ms and Stop returned in 0 ms (guest timer
resolution); neither waited for the deliberately delayed HTTP response.

## Original resource-protocol adapter

Inspection of the matched original x86 MSHTML PDB locates `CResProtocol::Start`
at 77788810, `CrackResUrl` at 77787c90, `DoParseAndBind` at 77788b60 and
`GetResource` at 7742f0e0. The checked source-path diagnostics identify
`mshtml/src/other/moniker/resprot.cxx`. This is Microsoft's resource protocol,
not a Wine implementation. It includes local-machine-zone feature checks,
resource/MUI lookup, MIME notifications, and an IStream-backed result.

`mshtml-res-probe.exe` successfully binds dnserror.htm and tabswelcome.htm with
the shim's HTMLDocument registration still installed; it observes native
`text/html` MIME notifications and rejects a missing resource. The adapter
therefore calls URLMon's original resource loader rather than duplicating PE
resource extraction or generating replacement error-page HTML.

The initial adapter intentionally accepts named `ieframe.dll` resources only.
Each document uses an unguessable intercepted HTTP origin for Chromium's
relative asset resolution. Fetch responses contain the original bytes and
native MIME metadata. There is no network listener. The OLE address remains the
original resource URL, but renderer-side location/origin uses the adapter URL;
this is not complete equivalence to MSHTML's res-origin model. A restrictive CSP
blocks plugins, network connections and framing of these internal documents.
Arbitrary DLL paths and resource traversal are rejected. Native shell actions
exposed through window.external remain separate compatibility work; this does
not enable ActiveX or certificate overrides.

The original httpErrorPagesScripts.js uses currentStyle.display in both its
error-information expander and tab-information controls. An origin-restricted
new-document script supplies HTMLElement.currentStyle via getComputedStyle;
the original resource bytes remain unchanged. Tests execute the native expander
and verify that a subsequent public page has no currentStyle shim.

The fresh-start checks launch IE with its unchanged configured home page, not
about:blank. The default Vista URLMon failure can therefore render its real IE
error resource and subsequently accept a web address instead of leaving a
detached bootstrap window. The internal-page test also checks CSS, script and
image loading, tab-welcome rendering, foreign-DLL refusal and HTTPS navigation.

Resource-adapter deployment hashes: x86
`159adfc0fe6f5e5d938897e7f64aedd1fccb49b5c14c1e6cde8fa071ba835fb2`,
x64 `f45c6697e7f04b4a5823c24bd9c160657b6a2cfe7b5fdf327582b899f91dde38`.
Both are installed through the existing hash-versioned installation script;
the operating system DLLs and configured home page are unchanged. Fresh-start
acceptance evidence is in `.unlazy/mshtml-fresh-start/GATES.md`.
