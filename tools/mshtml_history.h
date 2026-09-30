/* IE's document IPersistHistory path, with engine-owned live-session state.
 * Native command 38: bit 0 = local anchor, bit 1 = force, bit 2 = TL client.
 * The ordinary top-level MSHTML sender does not select the TL-client path.
 * Records refer to this Chromium context; they never turn foreign/stale state
 * into a fresh URL load that silently loses form state or changes its origin.
 * SaveHistoryEx's explicit Triton Cross Session mode carries bounded,
 * checksummed same-origin restart state for a newly created engine context. */
#define HISTORY_MAX_URL 16384u
#define HISTORY_REPLY_BYTES (1024u * 1024u)
#define HISTORY_MAX_RESTART_STATE (256u * 1024u)
static const GUID history_explorer_commands = {0x000214d0,0,0,{0xc0,0,0,0,0,0,0,0x46}};

struct HistorySnapshot {
    unsigned entry, index;
    unsigned count;
    unsigned *entries;
    BOOL is_current;
    DWORD position;
    BSTR url, title;
};
typedef struct HistoryHeader {
    char magic[8];
    DWORD version, characters;
    GUID context;
    DWORD entry, position;
} HistoryHeader;
_Static_assert(sizeof(HistoryHeader) == 40, "history wire header must remain architecture-independent");
typedef struct RestartHistoryHeader {
    char magic[8];
    DWORD version, url_characters, state_characters, entry, position, checksum;
    GUID context;
} RestartHistoryHeader;
_Static_assert(sizeof(RestartHistoryHeader) == 48,
    "restart history wire header must remain architecture-independent");

static DWORD history_checksum_update(DWORD hash, const void *data, size_t bytes)
{
    const BYTE *p = data;
    while(bytes--) { hash ^= *p++; hash *= 16777619u; }
    return hash;
}

static DWORD history_restart_checksum(const RestartHistoryHeader *header,
                                      LPCWSTR url, LPCWSTR state)
{
    DWORD hash = 2166136261u;
    hash = history_checksum_update(hash, &header->version,
        offsetof(RestartHistoryHeader, checksum) - offsetof(RestartHistoryHeader, version));
    hash = history_checksum_update(hash, &header->context, sizeof(header->context));
    hash = history_checksum_update(hash, url, header->url_characters * sizeof(WCHAR));
    hash = history_checksum_update(hash, state, header->state_characters * sizeof(WCHAR));
    return hash;
}

static BOOL history_same_origin(LPCWSTR url, LPCWSTR state)
{
    const WCHAR *scheme = wcsstr(url, L"://"), *authority_end;
    const WCHAR *key = wcsstr(state, L"\"origin\":\"");
    const WCHAR *origin, *origin_end;
    size_t url_length, origin_length, i;
    if(!scheme || !key) return FALSE;
    authority_end = wcspbrk(scheme + 3, L"/?#");
    if(!authority_end) authority_end = url + lstrlenW(url);
    origin = key + 10;
    origin_end = wcschr(origin, L'\"');
    if(!origin_end) return FALSE;
    url_length = (size_t)(authority_end - url);
    origin_length = (size_t)(origin_end - origin);
    if(url_length != origin_length) return FALSE;
    for(i = 0; i < url_length; ++i)
        if(towlower(url[i]) != towlower(origin[i])) return FALSE;
    return TRUE;
}

static void history_free(HistorySnapshot *snapshot)
{
    if(!snapshot) return;
    HeapFree(GetProcessHeap(), 0, snapshot->entries);
    SysFreeString(snapshot->url); SysFreeString(snapshot->title);
    HeapFree(GetProcessHeap(), 0, snapshot);
}
static void history_clear(ProbeDocument *document)
{
    history_free(document->history_current);
    document->history_current = NULL;
    history_free(document->history_departure);
    document->history_departure = NULL;
    document->history_request = 0;
    document->history_snapshot_pending = FALSE;
    ZeroMemory(&document->history_context, sizeof(document->history_context));
}
static BOOL history_unsigned(const char *value, unsigned *out)
{
    const char *end;
    unsigned result = 0;
    if(!value || *value < '0' || *value > '9') return FALSE;
    end = mj_skip(value, 0);
    if(!end) return FALSE;
    while(value != end) {
        if(*value < '0' || *value > '9' || result > (INT_MAX - (unsigned)(*value - '0')) / 10)
            return FALSE;
        result = result * 10 + (unsigned)(*value++ - '0');
    }
    *out = result;
    return TRUE;
}
static HRESULT history_parse(const char *reply, unsigned wanted, HistorySnapshot **out)
{
    const char *result = mj_member(reply, "result"), *entry;
    HistorySnapshot *snapshot = NULL;
    unsigned *entries;
    unsigned current, index = 0;
    *out = NULL;
    if(!history_unsigned(mj_member(result, "currentIndex"), &current)) return E_FAIL;
    entry = mj_member(result, "entries");
    if(!entry || *entry != '[') return E_FAIL;
    entries = HeapAlloc(GetProcessHeap(), 0, 4097 * sizeof(*entries));
    if(!entries) return E_OUTOFMEMORY;
    entry = mj_space(entry + 1);
    while(*entry != ']') {
        unsigned id;
        const char *end = mj_skip(entry, 0);
        if(index > 4096 || !end || !history_unsigned(mj_member(entry, "id"), &id) || !id) {
            history_free(snapshot); HeapFree(GetProcessHeap(), 0, entries); return E_FAIL;
        }
        entries[index] = id;
        if(wanted ? id == wanted : index == current) {
            HRESULT hr;
            if(snapshot) { history_free(snapshot); HeapFree(GetProcessHeap(), 0, entries); return E_FAIL; }
            snapshot = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*snapshot));
            if(!snapshot) { HeapFree(GetProcessHeap(), 0, entries); return E_OUTOFMEMORY; }
            hr = mj_bstr(mj_member(entry, "url"), &snapshot->url);
            if(SUCCEEDED(hr)) hr = mj_bstr(mj_member(entry, "title"), &snapshot->title);
            if(SUCCEEDED(hr) && (!SysStringLen(snapshot->url) || SysStringLen(snapshot->url) > HISTORY_MAX_URL ||
               (UINT)lstrlenW(snapshot->url) != SysStringLen(snapshot->url))) hr = E_INVALIDARG;
            if(FAILED(hr)) { history_free(snapshot); HeapFree(GetProcessHeap(), 0, entries); return hr; }
            snapshot->entry = id; snapshot->index = index;
            snapshot->is_current = index == current;
        }
        entry = mj_space(end);
        if(*entry == ',') entry = mj_space(entry + 1);
        else if(*entry != ']') { history_free(snapshot); HeapFree(GetProcessHeap(), 0, entries); return E_FAIL; }
        ++index;
    }
    if(!snapshot) { HeapFree(GetProcessHeap(), 0, entries); return HRESULT_FROM_WIN32(ERROR_NOT_FOUND); }
    snapshot->entries = HeapAlloc(GetProcessHeap(), 0, index * sizeof(*snapshot->entries));
    if(!snapshot->entries) { history_free(snapshot); HeapFree(GetProcessHeap(), 0, entries); return E_OUTOFMEMORY; }
    memcpy(snapshot->entries, entries, index * sizeof(*entries));
    snapshot->count = index;
    HeapFree(GetProcessHeap(), 0, entries);
    *out = snapshot;
    return S_OK;
}

static BOOL history_contains(const HistorySnapshot *snapshot, unsigned entry)
{
    unsigned index;
    for(index = 0; index < snapshot->count; ++index)
        if(snapshot->entries[index] == entry) return TRUE;
    return FALSE;
}
static HRESULT history_query_snapshot(ProbeDocument *document, unsigned wanted, HistorySnapshot **out);

static HRESULT history_synchronize_renderer_travel(ProbeDocument *document,
    const HistorySnapshot *departure, int offset)
{
    IOleClientSite *site = document->client_site;
    IServiceProvider *provider = NULL;
    TritonBrowserService *browser = NULL;
    TritonTravelLog *travel = NULL;
    IUnknown *relative = NULL;
    HRESULT hr, relative_hr;
    if(!site || !offset || document->history_export) return E_UNEXPECTED;
    IOleClientSite_AddRef(site);
    hr = IOleClientSite_QueryInterface(site, &IID_IServiceProvider, (void **)&provider);
    if(SUCCEEDED(hr)) {
        hr = IServiceProvider_QueryService(provider, &IID_IShellBrowser,
            &triton_browser_service_iid, (void **)&browser);
        IServiceProvider_Release(provider);
    }
    if(SUCCEEDED(hr)) hr = browser->lpVtbl->GetTravelLog(browser, &travel);
    if(SUCCEEDED(hr)) {
        /* The binding path reaches here before Chromium moves. IEFRAME owns the
         * cursor transition and calls our LoadHistory to move Chromium once.
         * The retained fallback can also reach here after an unhooked renderer
         * move; that path is diagnostic and may legitimately fail because the
         * opposite IE entry was never created. */
        document->history_export = departure;
        hr = travel->lpVtbl->Travel(travel, (IUnknown *)browser, offset);
        document->history_export = NULL;
        append_dword(L"MSHTML_RENDERER_HISTORY_TRAVEL_RESULT=", hr);
        if(SUCCEEDED(hr)) {
            relative_hr = travel->lpVtbl->GetTravelEntry(travel, (IUnknown *)browser, -1, &relative);
            append_dword(L"MSHTML_RENDERER_HISTORY_BACK_ENTRY_RESULT=", relative_hr);
            if(relative) { IUnknown_Release(relative); relative = NULL; }
            relative_hr = travel->lpVtbl->GetTravelEntry(travel, (IUnknown *)browser, 1, &relative);
            append_dword(L"MSHTML_RENDERER_HISTORY_FORWARD_ENTRY_RESULT=", relative_hr);
            if(relative) IUnknown_Release(relative);
            hr = browser->lpVtbl->UpdateBackForwardState(browser);
            append_dword(L"MSHTML_RENDERER_HISTORY_STATE_RESULT=", hr);
        }
    }
    if(travel) travel->lpVtbl->Release(travel);
    if(browser) browser->lpVtbl->Release(browser);
    IOleClientSite_Release(site);
    return hr;
}

static void history_renderer_travel_request(ProbeDocument *document, const char *params)
{
    const char *payload = mj_member(params, "payload");
    char *end = NULL;
    long parsed;
    HistorySnapshot *departure = NULL;
    HRESULT hr = E_INVALIDARG;
    if(payload && *payload == '"') {
        parsed = strtol(payload + 1, &end, 10);
        if(end != payload + 1 && *end == '"' && parsed >= -100 && parsed <= 100 && parsed)
            hr = history_query_snapshot(document, 0, &departure);
        if(SUCCEEDED(hr) && (!departure->is_current || !document->history_current ||
           departure->entry != document->history_current->entry)) hr = E_PENDING;
        if(SUCCEEDED(hr)) hr = history_synchronize_renderer_travel(document, departure, (int)parsed);
    }
    history_free(departure);
    append_dword(L"MSHTML_RENDERER_HISTORY_REQUEST_RESULT=", hr);
}
static HRESULT history_query_snapshot(ProbeDocument *document, unsigned wanted, HistorySnapshot **out)
{
    char *reply;
    HRESULT hr;
    *out = NULL;
    if(!document->page_session[0]) return CO_E_OBJNOTCONNECTED;
    reply = HeapAlloc(GetProcessHeap(), 0, HISTORY_REPLY_BYTES);
    if(!reply) return E_OUTOFMEMORY;
    hr = document_pipe_call(document, "Page.getNavigationHistory", "{}", document->page_session,
                            reply, HISTORY_REPLY_BYTES);
    if(SUCCEEDED(hr)) hr = history_parse(reply, wanted, out);
    HeapFree(GetProcessHeap(), 0, reply);
    return hr;
}
/* The native cookie is Y scroll, not an arbitrary host-assigned identifier:
 * CMarkup::GetPositionCookie 77274fa0 and SetPositionCookie 77275020. */
static HRESULT history_scroll_position(ProbeDocument *document, BOOL setting, DWORD *position)
{
    char params[256], reply[2048];
    const char *result;
    unsigned actual;
    HRESULT hr;
    if(!document->page_session[0]) return CO_E_OBJNOTCONNECTED;
    if(setting) snprintf(params, sizeof(params),
        "{\"expression\":\"scrollTo(scrollX,%ld);Math.max(0,Math.round(scrollY))\",\"returnByValue\":true,\"timeout\":1000}",
        (long)(LONG)*position);
    else strcpy(params,
        "{\"expression\":\"Math.max(0,Math.round(scrollY))\",\"returnByValue\":true,\"timeout\":1000}");
    hr = document_pipe_call(document, "Runtime.evaluate", params, document->page_session, reply, sizeof(reply));
    if(SUCCEEDED(hr)) hr = content_remote_result(reply, &result);
    if(SUCCEEDED(hr) && !history_unsigned(mj_member(result, "value"), &actual)) hr = E_FAIL;
    if(SUCCEEDED(hr)) { *position = actual; document->history_position = actual; }
    return hr;
}
static HRESULT history_capture_departure(ProbeDocument *document)
{
    HistorySnapshot *departure = NULL;
    HRESULT hr = history_query_snapshot(document, 0, &departure);
    if(SUCCEEDED(hr)) hr = history_scroll_position(document, FALSE, &departure->position);
    if(FAILED(hr)) { history_free(departure); return hr; }
    history_free(document->history_departure);
    document->history_departure = departure;
    return S_OK;
}
static void history_request_snapshot(ProbeDocument *document)
{
    CdpRequest *request;
    HRESULT hr = cdp_submit(document->runtime->cdp, "Page.getNavigationHistory", "{}",
        document->page_session, TRUE, document->navigation_serial, 2000, 0, &request);
    if(SUCCEEDED(hr)) {
        document->history_snapshot_pending = FALSE;
        document->history_request = request->id;
        cdp_request_release(request);
    } else {
        document->history_snapshot_pending = TRUE;
        append_dword(L"MSHTML_HISTORY_SNAPSHOT_QUEUE_FAILURE=", hr);
    }
}
static void history_receive_snapshot(ProbeDocument *document, const CdpMessage *message)
{
    HistorySnapshot *next = NULL, *previous;
    HRESULT hr = message->status;
    unsigned serial = document->navigation_serial;
    document->history_request = 0;
    if(message->tag != serial) return;
    if(SUCCEEDED(hr)) hr = history_parse(message->json, 0, &next);
    if(SUCCEEDED(hr) && (!document->observed_url || lstrcmpW(next->url, document->observed_url)))
        hr = E_PENDING; /* A newer renderer commit already overtook this query. */
    if(FAILED(hr)) {
        history_free(next);
        append_dword(L"MSHTML_HISTORY_SNAPSHOT_FAILURE=", hr);
        return;
    }
    next->position = document->history_position;
    previous = document->history_current;
    /* Detach ownership before calling arbitrary host COM. Close/SetClientSite
     * can reenter, so the exporting snapshot must outlive that callback. */
    document->history_current = NULL;
    if(previous && document->navigation_kind == 3 && next->entry != previous->entry &&
       history_contains(previous, next->entry)) {
        int offset = (int)next->index - (int)previous->index;
        hr = history_synchronize_renderer_travel(document, previous, offset);
        if(FAILED(hr)) append_dword(L"MSHTML_RENDERER_HISTORY_TRAVERSAL_UNSYNCED=", hr);
    } else if(previous && document->navigation_kind == 3 && next->entry != previous->entry &&
       next->index > previous->index && document->client_site) {
        IOleClientSite *site = document->client_site;
        IOleCommandTarget *target = NULL;
        IOleClientSite_AddRef(site);
        hr = IOleClientSite_QueryInterface(site, &IID_IOleCommandTarget, (void **)&target);
        if(SUCCEEDED(hr)) {
            VARIANT flags;
            VariantInit(&flags); flags.vt = VT_I4;
            flags.lVal = document->observed_same_document ? 1 : 0;
            document->history_export = previous;
            hr = IOleCommandTarget_Exec(target, &history_explorer_commands, 38, 0, &flags, NULL);
            document->history_export = NULL;
            IOleCommandTarget_Release(target);
            append_dword(L"MSHTML_NATIVE_HISTORY_UPDATE_RESULT=", hr);
        }
        if(document->navigation_serial != serial || document->client_site != site) {
            history_free(previous); history_free(next);
            IOleClientSite_Release(site);
            return;
        }
        IOleClientSite_Release(site);
    } else if(previous && document->navigation_kind == 3 && next->entry != previous->entry) {
        append_dword(L"MSHTML_RENDERER_HISTORY_TRAVERSAL_UNSYNCED=", E_UNEXPECTED);
    }
    history_free(previous);
    document->history_current = next;
    append_dword(L"MSHTML_HISTORY_CURRENT_ENTRY=", next->entry);
}
static ProbeDocument *history_document(IPersistHistory *iface)
{ return CONTAINING_RECORD(iface, ProbeDocument, IPersistHistory_iface); }
static HRESULT STDMETHODCALLTYPE history_qi(IPersistHistory *iface, REFIID iid, void **out)
{ return document_query_interface(history_document(iface), iid, out); }
static ULONG STDMETHODCALLTYPE history_addref(IPersistHistory *iface)
{ return document_add_ref(history_document(iface)); }
static ULONG STDMETHODCALLTYPE history_release(IPersistHistory *iface)
{ return document_release(history_document(iface)); }
static HRESULT STDMETHODCALLTYPE history_class(IPersistHistory *iface, CLSID *id)
{ (void)iface; return document_get_class_id(id); }

static HRESULT history_transfer(IStream *stream, void *buffer, ULONG count, BOOL writing)
{
    ULONG offset = 0;
    while(offset < count) {
        ULONG transferred = 0;
        HRESULT hr = writing ? IStream_Write(stream, (BYTE *)buffer + offset, count - offset, &transferred)
                             : IStream_Read(stream, (BYTE *)buffer + offset, count - offset, &transferred);
        if(FAILED(hr)) return hr;
        if(!transferred || transferred > count - offset) return writing ? STG_E_WRITEFAULT : STG_E_READFAULT;
        offset += transferred;
    }
    return S_OK;
}

static HRESULT history_save_restart(ProbeDocument *document, IStream *stream)
{
    HistorySnapshot *snapshot = NULL;
    RestartHistoryHeader header;
    char *reply = NULL;
    const char *value;
    BSTR state = NULL;
    HRESULT hr;
    reply = HeapAlloc(GetProcessHeap(), 0, HISTORY_REPLY_BYTES);
    if(!reply) return E_OUTOFMEMORY;
    hr = history_query_snapshot(document, 0, &snapshot);
    if(SUCCEEDED(hr)) hr = history_scroll_position(document, FALSE, &snapshot->position);
    if(SUCCEEDED(hr)) hr = document_pipe_call(document, "Runtime.evaluate",
        "{\"expression\":\"(()=>JSON.stringify({origin:location.origin,controls:Array.from(document.querySelectorAll('input,textarea,select')).map(e=>[e.value,!!e.checked,e.selectedIndex]),x:Math.max(0,Math.round(scrollX)),y:Math.max(0,Math.round(scrollY))}))()\",\"returnByValue\":true,\"timeout\":1000}",
        document->page_session, reply, HISTORY_REPLY_BYTES);
    value = SUCCEEDED(hr) ? mj_member(mj_member(mj_member(reply, "result"), "result"), "value") : NULL;
    if(SUCCEEDED(hr)) hr = mj_bstr(value, &state);
    if(SUCCEEDED(hr) && (!SysStringLen(state) || SysStringLen(state) > HISTORY_MAX_RESTART_STATE ||
       !history_same_origin(snapshot->url, state))) hr = STG_E_INVALIDHEADER;
    if(SUCCEEDED(hr)) {
        ZeroMemory(&header, sizeof(header));
        memcpy(header.magic, "TRI7HST2", 8); header.version = 2;
        header.url_characters = SysStringLen(snapshot->url);
        header.state_characters = SysStringLen(state);
        header.entry = snapshot->entry; header.position = snapshot->position;
        header.context = document->history_context;
        header.checksum = history_restart_checksum(&header, snapshot->url, state);
        hr = history_transfer(stream, &header, sizeof(header), TRUE);
        if(SUCCEEDED(hr)) hr = history_transfer(stream, snapshot->url,
            header.url_characters * sizeof(WCHAR), TRUE);
        if(SUCCEEDED(hr)) hr = history_transfer(stream, state,
            header.state_characters * sizeof(WCHAR), TRUE);
    }
    append_dword(L"MSHTML_SAVE_RESTART_HISTORY_RESULT=", hr);
    SysFreeString(state); history_free(snapshot); HeapFree(GetProcessHeap(), 0, reply);
    return hr;
}

static void history_apply_restart_state(ProbeDocument *document)
{
    char *quoted = NULL, *code = NULL, *quoted_code = NULL, *params = NULL, *reply = NULL;
    BSTR wide_code = NULL;
    size_t capacity, params_capacity;
    int wide_length;
    HRESULT hr;
    if(!document->history_restart_state || !document->page_session[0]) return;
    quoted = mj_quote(document->history_restart_state);
    capacity = quoted ? strlen(quoted) + 700 : 0;
    code = capacity ? HeapAlloc(GetProcessHeap(), 0, capacity) : NULL;
    reply = HeapAlloc(GetProcessHeap(), 0, 4096);
    if(!quoted || !code || !reply) hr = E_OUTOFMEMORY;
    else {
        snprintf(code, capacity,
            "(()=>{const s=JSON.parse(%s);if(s.origin!==location.origin)throw Error('origin');"
            "const e=document.querySelectorAll('input,textarea,select');s.controls.forEach((v,i)=>{if(!e[i])return;"
            "e[i].value=v[0];if('checked'in e[i])e[i].checked=v[1];if(e[i].tagName==='SELECT')e[i].selectedIndex=v[2]});"
            "scrollTo(s.x,s.y);return true})()", quoted);
        wide_length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, code, -1, NULL, 0);
        wide_code = wide_length > 0 ? SysAllocStringLen(NULL, (UINT)wide_length - 1) : NULL;
        if(!wide_code || !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, code, -1,
                                               wide_code, wide_length)) hr = E_FAIL;
        else {
            quoted_code = mj_quote(wide_code);
            params_capacity = quoted_code ? strlen(quoted_code) + 80 : 0;
            params = params_capacity ? HeapAlloc(GetProcessHeap(), 0, params_capacity) : NULL;
            if(!quoted_code || !params) hr = E_OUTOFMEMORY;
            else {
                snprintf(params, params_capacity,
                    "{\"expression\":%s,\"returnByValue\":true,\"timeout\":1000}", quoted_code);
                hr = document_pipe_call(document, "Runtime.evaluate", params,
                                        document->page_session, reply, 4096);
            }
        }
    }
    if(SUCCEEDED(hr)) {
        SysFreeString(document->history_restart_state);
        document->history_restart_state = NULL;
        document->history_position = document->history_restart_position;
        append_text(L"MSHTML_RESTART_HISTORY_STATE_APPLIED\r\n");
    } else append_dword(L"MSHTML_RESTART_HISTORY_APPLY_FAILURE=", hr);
    HeapFree(GetProcessHeap(), 0, quoted);
    HeapFree(GetProcessHeap(), 0, code);
    HeapFree(GetProcessHeap(), 0, quoted_code);
    HeapFree(GetProcessHeap(), 0, params);
    HeapFree(GetProcessHeap(), 0, reply);
    SysFreeString(wide_code);
}
static HRESULT STDMETHODCALLTYPE history_save(IPersistHistory *iface, IStream *stream)
{
    ProbeDocument *document = history_document(iface);
    HistorySnapshot *queried = NULL;
    const HistorySnapshot *snapshot = document->history_export;
    HistoryHeader header;
    HRESULT hr = S_OK;
    if(!stream) return E_POINTER;
    if(!document->page_session[0]) return CO_E_OBJNOTCONNECTED;
    /* IE saves the outgoing document during activation of the committed
     * destination. This shim reuses one document, unlike native MSHTML. */
    if(!snapshot && document->navigation_kind == 1 && document->host_load_phase != 2)
        snapshot = document->history_departure;
    if(!snapshot) {
        /* The COM persistence call is synchronous. Ordinary observation and
         * native completion use the asynchronous worker request above. */
        hr = history_query_snapshot(document, 0, &queried);
        if(FAILED(hr)) return hr;
        hr = history_scroll_position(document, FALSE, &queried->position);
        if(FAILED(hr)) { history_free(queried); return hr; }
        snapshot = queried;
    }
    ZeroMemory(&header, sizeof(header));
    memcpy(header.magic, "TRI7HST1", 8); header.version = 1;
    header.characters = SysStringLen(snapshot->url);
    header.context = document->history_context;
    header.entry = snapshot->entry; header.position = snapshot->position;
    hr = history_transfer(stream, &header, sizeof(header), TRUE);
    if(SUCCEEDED(hr)) hr = history_transfer(stream, snapshot->url, header.characters * sizeof(WCHAR), TRUE);
    history_free(queried);
    append_dword(L"MSHTML_SAVE_HISTORY_RESULT=", hr);
    return hr;
}
static HRESULT history_prepare_travel(ProbeDocument *document, IBindCtx *context)
{
    IOleClientSite *site = document->client_site;
    IOleCommandTarget *target = NULL;
    IUnknown *marker = NULL;
    VARIANT flags;
    unsigned serial = document->navigation_serial;
    HRESULT hr;
    if(!site) return S_OK;
    if(context && SUCCEEDED(IBindCtx_GetObjectParam(context, L"Internal Navigation", &marker))) {
        if(marker) IUnknown_Release(marker);
        /* _NavigateWithinView already calls ITravelLog::UpdateEntry at
         * 75e5488c before document LoadHistory at 75e548a0. A second command
         * 38 here would append an entry and truncate Forward history. */
        append_text(L"MSHTML_INTERNAL_HISTORY_DEPARTURE_HOST_OWNED\r\n");
        return S_OK;
    }
    if(marker) IUnknown_Release(marker);
    document_add_ref(document);
    IOleClientSite_AddRef(site);
    VariantInit(&flags); flags.vt = VT_I4; flags.lVal = 2;
    hr = IOleClientSite_QueryInterface(site, &IID_IOleCommandTarget, (void **)&target);
    if(SUCCEEDED(hr)) {
        /* CTravelLog::_TravelToEntryInternal preserves the departure entry
         * before Invoke. The browser's forced update materializes that entry
         * and sees its history-travel bit, so it does not append a new one.
         * Save before queuing the renderer restore: otherwise Forward loses
         * the page just left, or serializes the destination as its state. */
        hr = IOleCommandTarget_Exec(target, &history_explorer_commands, 38, 0, &flags, NULL);
        IOleCommandTarget_Release(target);
        append_dword(L"MSHTML_NATIVE_HISTORY_DEPARTURE_RESULT=", hr);
    }
    if(document->navigation_serial != serial || document->client_site != site || !document->page_session[0]) hr = E_ABORT;
    else if(hr == E_NOINTERFACE || hr == OLECMDERR_E_UNKNOWNGROUP || hr == OLECMDERR_E_NOTSUPPORTED) hr = S_OK;
    IOleClientSite_Release(site);
    document_release(document);
    return hr;
}
static HRESULT STDMETHODCALLTYPE history_load(IPersistHistory *iface, IStream *stream, IBindCtx *context)
{
    ProbeDocument *document = history_document(iface);
    union { HistoryHeader live; RestartHistoryHeader restart; } record;
    HistorySnapshot *snapshot = NULL;
    BSTR url = NULL, state = NULL;
    char params[80];
    HRESULT hr;
    if(!stream) return E_POINTER;
    ZeroMemory(&record, sizeof(record));
    hr = history_transfer(stream, &record, 12, FALSE);
    if(FAILED(hr)) return hr;
    if(!memcmp(record.restart.magic, "TRI7HST2", 8) && record.restart.version == 2) {
        hr = history_transfer(stream, (BYTE *)&record.restart + 12,
                              sizeof(record.restart) - 12, FALSE);
        if(FAILED(hr)) return hr;
        if(!record.restart.url_characters || record.restart.url_characters > HISTORY_MAX_URL ||
           !record.restart.state_characters ||
           record.restart.state_characters > HISTORY_MAX_RESTART_STATE ||
           !record.restart.entry || record.restart.entry > INT_MAX)
            return STG_E_INVALIDHEADER;
        url = SysAllocStringLen(NULL, record.restart.url_characters);
        state = SysAllocStringLen(NULL, record.restart.state_characters);
        if(!url || !state) hr = E_OUTOFMEMORY;
        if(SUCCEEDED(hr)) hr = history_transfer(stream, url,
            record.restart.url_characters * sizeof(WCHAR), FALSE);
        if(SUCCEEDED(hr)) hr = history_transfer(stream, state,
            record.restart.state_characters * sizeof(WCHAR), FALSE);
        if(SUCCEEDED(hr) && ((UINT)lstrlenW(url) != record.restart.url_characters ||
           (UINT)lstrlenW(state) != record.restart.state_characters ||
           record.restart.checksum != history_restart_checksum(&record.restart, url, state) ||
           !history_same_origin(url, state))) hr = STG_E_INVALIDHEADER;
        if(SUCCEEDED(hr)) hr = content_ensure_page(document);
        if(SUCCEEDED(hr)) {
            SysFreeString(document->history_restart_state);
            document->history_restart_state = state;
            state = NULL;
            document->history_restart_position = record.restart.position;
            hr = navigate_document_url(document, url);
            if(FAILED(hr)) {
                SysFreeString(document->history_restart_state);
                document->history_restart_state = NULL;
            }
        }
        SysFreeString(state); SysFreeString(url);
        append_dword(L"MSHTML_LOAD_RESTART_HISTORY_RESULT=", hr);
        return hr;
    }
    if(memcmp(record.live.magic, "TRI7HST1", 8) || record.live.version != 1)
        return STG_E_INVALIDHEADER;
    hr = history_transfer(stream, (BYTE *)&record.live + 12, sizeof(record.live) - 12, FALSE);
    if(FAILED(hr)) return hr;
    if(!record.live.characters || record.live.characters > HISTORY_MAX_URL ||
       !record.live.entry || record.live.entry > INT_MAX) return STG_E_INVALIDHEADER;
    if(!document->page_session[0] || !IsEqualGUID(&record.live.context, &document->history_context))
        return CO_E_OBJNOTCONNECTED;
    url = SysAllocStringLen(NULL, record.live.characters);
    if(!url) return E_OUTOFMEMORY;
    hr = history_transfer(stream, url, record.live.characters * sizeof(WCHAR), FALSE);
    if(SUCCEEDED(hr) && (UINT)lstrlenW(url) != record.live.characters) hr = STG_E_INVALIDHEADER;
    if(SUCCEEDED(hr)) hr = history_query_snapshot(document, record.live.entry, &snapshot);
    if(SUCCEEDED(hr) && lstrcmpW(url, snapshot->url)) hr = STG_E_INVALIDHEADER;
    if(SUCCEEDED(hr) && !snapshot->is_current) hr = history_prepare_travel(document, context);
    if(SUCCEEDED(hr) && snapshot->is_current && document->observed_url &&
       !lstrcmpW(url, document->observed_url)) {
        /* Renderer-originated travel reaches IE after Chromium has committed.
         * Complete LoadHistory synchronously while IEFRAME's history-travel
         * bit is still set. Posting another navigateToHistoryEntry needlessly
         * crosses that synchronous ABI boundary and loses the native Forward
         * entry before the later completion callbacks. */
        ++document->navigation_serial;
        document->navigation_kind = 5;
        document->history_request = 0;
        document->history_snapshot_pending = FALSE;
        history_free(document->history_current);
        document->history_current = snapshot;
        snapshot = NULL;
        document->navigation_wait_ack = FALSE;
        document->navigation_committed = TRUE;
        document->history_waiting_commit = FALSE;
        document->observed_same_document = TRUE;
        document->host_navigate_notified = FALSE;
        document->host_load_phase = 0;
        document_set_ready_state(document, READYSTATE_COMPLETE);
        append_text(L"MSHTML_HISTORY_ALREADY_CURRENT\r\n");
    } else if(SUCCEEDED(hr)) {
        snprintf(params, sizeof(params), "{\"entryId\":%lu}", (unsigned long)record.live.entry);
        hr = document_post_navigation(document, "Page.navigateToHistoryEntry", params, 5, url, FALSE);
    }
    history_free(snapshot); SysFreeString(url);
    append_dword(L"MSHTML_LOAD_HISTORY_RESULT=", hr);
    return hr;
}
static HRESULT STDMETHODCALLTYPE history_set_position(IPersistHistory *iface, DWORD position)
{ return history_scroll_position(history_document(iface), TRUE, &position); }
static HRESULT STDMETHODCALLTYPE history_get_position(IPersistHistory *iface, DWORD *position)
{
    if(!position) return E_POINTER;
    *position = 0;
    return history_scroll_position(history_document(iface), FALSE, position);
}
static HRESULT STDMETHODCALLTYPE history_save_ex(IPersistHistory *iface, IStream *stream, IBindCtx *context)
{
    ProbeDocument *document = history_document(iface);
    IUnknown *marker = NULL;
    HRESULT hr;
    if(!stream) return E_POINTER;
    if(context) {
        hr = IBindCtx_GetObjectParam(context, L"Triton Cross Session", &marker);
        if(SUCCEEDED(hr)) {
            if(marker) IUnknown_Release(marker);
            append_text(L"MSHTML_SAVE_RESTART_HISTORY_EX\r\n");
            return history_save_restart(document, stream);
        }
        marker = NULL;
        /* Match the native browser extension's recognized context; never
         * claim support for unrelated bind-context persistence modes. */
        hr = IBindCtx_GetObjectParam(context, L"Internal Navigation", &marker);
        if(marker) IUnknown_Release(marker);
        if(FAILED(hr)) return E_NOTIMPL;
    }
    append_text(L"MSHTML_SAVE_HISTORY_EX\r\n");
    return history_save(iface, stream);
}
static TritonPersistHistory2Vtbl history_vtbl = {{history_qi,history_addref,history_release,history_class,
    history_load,history_save,history_set_position,history_get_position},history_save_ex};
