/* Native IE7 host contracts. Included after the document/CDP helpers.
 * Window and document have different canonical IUnknowns, but share storage
 * and lifetime; navigation replaces a DOM generation, not this window.
 * The host is explicitly disconnected on SetClientSite(NULL)/Close. */
typedef struct PropertySubscription {
    struct PropertySubscription *next;
    IPropertyNotifySink *sink;
    DWORD cookie;
} PropertySubscription;

static LRESULT CALLBACK state_observer_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    ProbeDocument *document = (ProbeDocument *)GetWindowLongPtrW(window, GWLP_USERDATA);
    if(document && message == WM_TIMER && wparam == 1) {
        update_host_load_state(document);
        return 0;
    }
    return document ? CallWindowProcW(document->state_observer_original, window, message, wparam, lparam) :
                      DefWindowProcW(window, message, wparam, lparam);
}
static HRESULT document_observe_state(ProbeDocument *document, BOOL enable)
{
    if(!enable) {
        if(document->state_observer) {
            KillTimer(document->state_observer, 1);
            SetWindowLongPtrW(document->state_observer, GWLP_WNDPROC, (LONG_PTR)document->state_observer_original);
            DestroyWindow(document->state_observer);
            document->state_observer = NULL;
        }
        return S_OK;
    }
    if(document->state_observer) return S_OK;
    /* A message-only observer must exist before in-place activation: IE waits
     * for ready-state notifications before it creates/shows the document view.
     * Subclass a system class so no registered class points into an unloaded DLL. */
    document->state_observer = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0,
        HWND_MESSAGE, NULL, module_instance, NULL);
    if(!document->state_observer) return HRESULT_FROM_WIN32(GetLastError());
    SetWindowLongPtrW(document->state_observer, GWLP_USERDATA, (LONG_PTR)document);
    document->state_observer_original = (WNDPROC)SetWindowLongPtrW(document->state_observer,
        GWLP_WNDPROC, (LONG_PTR)state_observer_proc);
    if(!SetTimer(document->state_observer, 1, 100, NULL)) {
        HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
        document_observe_state(document, FALSE);
        return FAILED(hr) ? hr : E_FAIL;
    }
    return S_OK;
}

static ProbeDocument *from_window(IHTMLWindow2 *iface)
{
    return CONTAINING_RECORD(iface, ProbeDocument, IHTMLWindow2_iface);
}
static ProbeDocument *from_private_window(TritonPrivateWindow4 *iface)
{
    return CONTAINING_RECORD(iface, ProbeDocument, private_window_iface);
}
static HRESULT window_query_interface(ProbeDocument *document, REFIID iid, void **out)
{
    if(!out) return E_POINTER;
    *out = NULL;
    append_guid(L"WINDOW_QUERY_IID=", iid);
    if(IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IDispatch) ||
       IsEqualIID(iid, &IID_IHTMLWindow2) || IsEqualIID(iid, &IID_IHTMLFramesCollection2))
        *out = &document->IHTMLWindow2_iface;
    else if(IsEqualIID(iid, &triton_private_window4_iid) || IsEqualIID(iid, &triton_private_window_iid))
        *out = &document->private_window_iface;
    else return E_NOINTERFACE;
    document_add_ref(document);
    return S_OK;
}
static HRESULT document_parent_window(ProbeDocument *document, IHTMLWindow2 **window)
{
    return window_query_interface(document, &IID_IHTMLWindow2, (void **)window);
}
static HRESULT window_set_name(ProbeDocument *document, BSTR name)
{
    BSTR copy = SysAllocString(name ? name : L"");
    if(!copy) return E_OUTOFMEMORY;
    SysFreeString(document->window_name);
    document->window_name = copy;
    return S_OK;
}
static HRESULT window_ids(IHTMLWindow2 *iface, REFIID iid, LPOLESTR *names, UINT count, DISPID *ids)
{
    UINT i;
    HRESULT hr = S_OK;
    (void)iface;
    if(!IsEqualIID(iid, &IID_NULL)) return DISP_E_UNKNOWNINTERFACE;
    if(!names || !ids) return E_POINTER;
    for(i = 0; i < count; ++i) {
        ids[i] = DISPID_UNKNOWN;
        if(names[i] && !lstrcmpiW(names[i], L"document")) ids[i] = DISPID_IHTMLWINDOW2_DOCUMENT;
        else if(names[i] && !lstrcmpiW(names[i], L"navigate")) ids[i] = DISPID_IHTMLWINDOW2_NAVIGATE;
        else if(names[i] && !lstrcmpiW(names[i], L"self")) ids[i] = DISPID_IHTMLWINDOW2_SELF;
        else if(names[i] && !lstrcmpiW(names[i], L"window")) ids[i] = DISPID_IHTMLWINDOW2_WINDOW;
        else if(names[i] && !lstrcmpiW(names[i], L"top")) ids[i] = DISPID_IHTMLWINDOW2_TOP;
        else if(names[i] && !lstrcmpiW(names[i], L"parent")) ids[i] = DISPID_IHTMLWINDOW2_PARENT;
        else if(names[i] && !lstrcmpiW(names[i], L"closed")) ids[i] = DISPID_IHTMLWINDOW2_CLOSED;
        else hr = DISP_E_UNKNOWNNAME;
    }
    return hr;
}
static HRESULT window_invoke(IHTMLWindow2 *iface, DISPID id, REFIID iid, WORD flags,
                             DISPPARAMS *params, VARIANT *result)
{
    ProbeDocument *document = from_window(iface);
    UINT count = params ? params->cArgs : 0;
    if(result) VariantInit(result);
    if(!IsEqualIID(iid, &IID_NULL)) return DISP_E_UNKNOWNINTERFACE;
    if(params && params->cNamedArgs) return DISP_E_NONAMEDARGS;
    if(id == DISPID_IHTMLWINDOW2_NAVIGATE && flags == DISPATCH_METHOD) {
        if(count != 1 || !params->rgvarg) return DISP_E_BADPARAMCOUNT;
        if(params->rgvarg[0].vt != VT_BSTR) return DISP_E_TYPEMISMATCH;
        return navigate_document_url(document, params->rgvarg[0].bstrVal);
    }
    if(flags != DISPATCH_PROPERTYGET) return DISP_E_MEMBERNOTFOUND;
    if(count) return DISP_E_BADPARAMCOUNT;
    if(!result) return E_POINTER;
    if(id == DISPID_IHTMLWINDOW2_DOCUMENT) {
        result->vt = VT_DISPATCH;
        result->pdispVal = &document->IDispatch_iface;
    } else if(id == DISPID_IHTMLWINDOW2_SELF || id == DISPID_IHTMLWINDOW2_WINDOW ||
              id == DISPID_IHTMLWINDOW2_TOP || id == DISPID_IHTMLWINDOW2_PARENT) {
        result->vt = VT_DISPATCH;
        result->pdispVal = (IDispatch *)iface;
    } else if(id == DISPID_IHTMLWINDOW2_CLOSED) {
        result->vt = VT_BOOL;
        result->boolVal = document->window_closed ? VARIANT_TRUE : VARIANT_FALSE;
        return S_OK;
    } else return DISP_E_MEMBERNOTFOUND;
    document_add_ref(document);
    return S_OK;
}
static HRESULT window_exec_script(ProbeDocument *document, BSTR code, BSTR language, VARIANT *result)
{
    char *quoted, *params, *reply;
    size_t size;
    HRESULT hr;
    if(result) VariantInit(result);
    if(language && *language && lstrcmpiW(language, L"javascript") && lstrcmpiW(language, L"jscript"))
        return E_NOTIMPL;
    if(!code) return E_INVALIDARG;
    hr = content_ensure_page(document);
    if(FAILED(hr)) return hr;
    quoted = mj_quote(code);
    if(!quoted) return E_OUTOFMEMORY;
    size = strlen(quoted) + 100;
    params = HeapAlloc(GetProcessHeap(), 0, size);
    reply = HeapAlloc(GetProcessHeap(), 0, 65536);
    if(!params || !reply) hr = E_OUTOFMEMORY;
    else {
        snprintf(params, size, "{\"expression\":%s,\"returnByValue\":true,\"timeout\":1000}", quoted);
        hr = document_pipe_call(document, "Runtime.evaluate", params, document->page_session, reply, 65536);
        if(SUCCEEDED(hr) && strstr(reply, "\"exceptionDetails\"")) hr = DISP_E_EXCEPTION;
    }
    HeapFree(GetProcessHeap(), 0, params);
    HeapFree(GetProcessHeap(), 0, reply);
    HeapFree(GetProcessHeap(), 0, quoted);
    /* IE's execScript returns VT_EMPTY, not the script's final expression. */
    return hr;
}

static HRESULT STDMETHODCALLTYPE private_query(TritonPrivateWindow4 *iface, REFIID iid, void **out)
{ return window_query_interface(from_private_window(iface), iid, out); }
static ULONG STDMETHODCALLTYPE private_addref(TritonPrivateWindow4 *iface)
{ return document_add_ref(from_private_window(iface)); }
static ULONG STDMETHODCALLTYPE private_release(TritonPrivateWindow4 *iface)
{ return document_release(from_private_window(iface)); }

static void document_request_clear(ProbeDocument *document)
{
    CdpRequest *request = NULL;
    if(document->request_fetch_enabled && document->runtime && document->runtime->cdp &&
       document->page_session[0] && SUCCEEDED(cdp_submit(document->runtime->cdp,
          "Fetch.disable", "{}", document->page_session, TRUE, 0, 10000, 0, &request)))
        cdp_request_release(request);
    document->request_fetch_enabled = FALSE;
    HeapFree(GetProcessHeap(), 0, document->request_post_data);
    HeapFree(GetProcessHeap(), 0, document->request_headers);
    document->request_post_data = document->request_headers = NULL;
    document->request_origin[0] = 0;
    document->request_override_pending = FALSE;
    if(document->resource_origin[0] && document->page_session[0] && document->runtime->cdp)
        resource_enable(document);
}

static char *request_base64(const BYTE *bytes, ULONG length)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t size = ((size_t)length + 2) / 3 * 4;
    char *out = HeapAlloc(GetProcessHeap(), 0, size + 1);
    ULONG input = 0;
    size_t output = 0;
    if(!out) return NULL;
    while(input < length) {
        unsigned available = length - input;
        unsigned a = bytes[input++];
        unsigned b = input < length ? bytes[input++] : 0;
        unsigned c = input < length ? bytes[input++] : 0;
        out[output++] = alphabet[a >> 2];
        out[output++] = alphabet[((a & 3) << 4) | (b >> 4)];
        out[output++] = available > 1 ? alphabet[((b & 15) << 2) | (c >> 6)] : '=';
        out[output++] = available > 2 ? alphabet[c & 63] : '=';
    }
    out[output] = 0;
    return out;
}

static BOOL request_origin_from_wide(LPCWSTR url, char *origin, size_t capacity)
{
    char utf8[2048], *authority, *end, *p;
    int bytes;
    if(!url || !origin || capacity < 8) return FALSE;
    bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, url, -1,
                                utf8, sizeof(utf8), NULL, NULL);
    if(!bytes) return FALSE;
    authority = strstr(utf8, "://");
    if(!authority) return FALSE;
    end = strpbrk(authority + 3, "/?#");
    if(!end) end = utf8 + strlen(utf8);
    if((size_t)(end - utf8) >= capacity) return FALSE;
    memcpy(origin, utf8, (size_t)(end - utf8));
    origin[end - utf8] = 0;
    for(p = origin; *p; ++p) if(*p >= 'A' && *p <= 'Z') *p += 'a' - 'A';
    return TRUE;
}

static HRESULT request_headers_json(BSTR headers, char **out)
{
    const WCHAR *line, *end;
    size_t capacity = headers ? (size_t)SysStringLen(headers) * 8 + 32 : 32;
    char *json = HeapAlloc(GetProcessHeap(), 0, capacity);
    size_t used = 0;
    *out = NULL;
    if(!json) return E_OUTOFMEMORY;
    json[used++] = '[';
    for(line = headers; line && *line;) {
        const WCHAR *colon, *next;
        BSTR name = NULL, value = NULL;
        char *quoted_name = NULL, *quoted_value = NULL;
        int written;
        next = wcschr(line, L'\n');
        if(!next) next = line + lstrlenW(line);
        end = next;
        while(end > line && end[-1] == L'\r') --end;
        if(end == line) { line = *next ? next + 1 : next; continue; }
        colon = line;
        while(colon < end && *colon != L':') ++colon;
        if(colon == line || colon == end) { HeapFree(GetProcessHeap(), 0, json); return E_INVALIDARG; }
        {
            const WCHAR *value_start = colon + 1, *value_end = end;
            while(value_start < value_end && (*value_start == L' ' || *value_start == L'\t')) ++value_start;
            while(value_end > value_start && (value_end[-1] == L' ' || value_end[-1] == L'\t')) --value_end;
            name = SysAllocStringLen(line, (UINT)(colon - line));
            value = SysAllocStringLen(value_start, (UINT)(value_end - value_start));
        }
        if(name) quoted_name = mj_quote(name);
        if(value) quoted_value = mj_quote(value);
        if(!name || !value || !quoted_name || !quoted_value) {
            SysFreeString(name); SysFreeString(value);
            HeapFree(GetProcessHeap(), 0, quoted_name); HeapFree(GetProcessHeap(), 0, quoted_value);
            HeapFree(GetProcessHeap(), 0, json); return E_OUTOFMEMORY;
        }
        written = snprintf(json + used, capacity - used, "%s{\"name\":%s,\"value\":%s}",
                           used > 1 ? "," : "", quoted_name, quoted_value);
        SysFreeString(name); SysFreeString(value);
        HeapFree(GetProcessHeap(), 0, quoted_name); HeapFree(GetProcessHeap(), 0, quoted_value);
        if(written < 0 || (size_t)written >= capacity - used) {
            HeapFree(GetProcessHeap(), 0, json); return HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW);
        }
        used += (size_t)written;
        line = *next ? next + 1 : next;
    }
    json[used++] = ']'; json[used] = 0;
    *out = json;
    return S_OK;
}

static HRESULT document_request_prepare(ProbeDocument *document, LPCWSTR url,
                                        VARIANT *post, VARIANT *headers)
{
    SAFEARRAY *array = NULL;
    BYTE *data = NULL;
    LONG lower = 0, upper = -1;
    HRESULT hr = S_OK;
    char reply[2048];
    document_request_clear(document);
    if(post && post->vt != VT_EMPTY && post->vt != VT_NULL) {
        if(post->vt != (VT_ARRAY | VT_UI1) || !(array = post->parray) ||
           SafeArrayGetDim(array) != 1 || FAILED(SafeArrayGetLBound(array, 1, &lower)) ||
           FAILED(SafeArrayGetUBound(array, 1, &upper)) || upper < lower ||
           (unsigned long)(upper - lower + 1) > 8u * 1024u * 1024u)
            return E_NOTIMPL;
    }
    if(headers && headers->vt != VT_EMPTY && headers->vt != VT_NULL && headers->vt != VT_BSTR)
        return E_NOTIMPL;
    if(!request_origin_from_wide(url, document->request_origin,
                                 sizeof(document->request_origin))) return E_INVALIDARG;
    hr = document_pipe_call(document, "Fetch.enable",
        "{\"patterns\":[{\"urlPattern\":\"*\",\"resourceType\":\"Document\",\"requestStage\":\"Request\"}]}",
        document->page_session, reply, sizeof(reply));
    if(FAILED(hr)) { document_request_clear(document); return hr; }
    document->request_fetch_enabled = TRUE;
    if(document->resource_origin[0] && FAILED(hr = resource_enable(document))) {
        document_request_clear(document); return hr;
    }
    if(array) {
        hr = SafeArrayAccessData(array, (void **)&data);
        if(SUCCEEDED(hr)) {
            document->request_post_data = request_base64(data, (ULONG)(upper - lower + 1));
            SafeArrayUnaccessData(array);
            if(!document->request_post_data) hr = E_OUTOFMEMORY;
        }
    }
    if(SUCCEEDED(hr) && headers && headers->vt != VT_EMPTY && headers->vt != VT_NULL) {
        hr = request_headers_json(headers->bstrVal, &document->request_headers);
    }
    if(FAILED(hr)) { document_request_clear(document); return hr; }
    if(!document->request_headers) {
        document->request_headers = HeapAlloc(GetProcessHeap(), 0, 3);
        if(!document->request_headers) { document_request_clear(document); return E_OUTOFMEMORY; }
        strcpy(document->request_headers, "[]");
    }
    document->request_override_pending = document->request_post_data != NULL;
    return S_OK;
}

static void document_continue_paused_request(ProbeDocument *document, const char *params)
{
    const char *request = mj_member(params, "request");
    char request_id[80], frame_id[80], origin[512];
    BSTR url = NULL, wide_request_id = NULL;
    char *wire = NULL;
    BOOL top, same_origin = FALSE, override;
    size_t capacity;
    CdpRequest *submitted = NULL;
    HRESULT hr;
    if(resource_paused(document, params)) return;
    if(FAILED(mj_bstr(mj_member(params, "requestId"), &wide_request_id)) ||
       !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_request_id, -1,
                            request_id, sizeof(request_id), NULL, NULL)) {
        SysFreeString(wide_request_id);
        append_text(L"MSHTML_FETCH_INVALID_REQUEST_ID\r\n");
        return;
    }
    SysFreeString(wide_request_id);
    top = nav_id(params, "frameId", frame_id) && !strcmp(frame_id, document->main_frame);
    if(top && SUCCEEDED(mj_bstr(mj_member(request, "url"), &url)))
        same_origin = request_origin_from_wide(url, origin, sizeof(origin)) &&
                      !strcmp(origin, document->request_origin);
    SysFreeString(url);
    override = top && same_origin && document->request_headers;
    capacity = strlen(request_id) + (override ? strlen(document->request_headers) : 0) +
               (document->request_post_data ? strlen(document->request_post_data) : 0) + 160;
    wire = HeapAlloc(GetProcessHeap(), 0, capacity);
    if(!wire) return;
    if(override && document->request_override_pending) {
        snprintf(wire, capacity,
            "{\"requestId\":\"%s\",\"method\":\"POST\",\"postData\":\"%s\",\"headers\":%s}",
            request_id, document->request_post_data, document->request_headers);
        document->request_override_pending = FALSE;
        append_text(L"MSHTML_FETCH_POST_OVERRIDE\r\n");
    } else if(override) {
        snprintf(wire, capacity, "{\"requestId\":\"%s\",\"headers\":%s}",
                 request_id, document->request_headers);
        append_text(L"MSHTML_FETCH_SAME_ORIGIN_HEADERS\r\n");
    } else if(top && document->request_headers) {
        /* A redirected request may inherit the headers installed on the first
         * request. An explicit empty array is required to strip them. */
        snprintf(wire, capacity, "{\"requestId\":\"%s\",\"headers\":[]}", request_id);
        append_text(L"MSHTML_FETCH_CROSS_ORIGIN_HEADERS_STRIPPED\r\n");
    } else {
        snprintf(wire, capacity, "{\"requestId\":\"%s\"}", request_id);
    }
    hr = cdp_submit(document->runtime->cdp, "Fetch.continueRequest", wire,
                    document->page_session, TRUE, 0, 10000, 0, &submitted);
    if(SUCCEEDED(hr)) cdp_request_release(submitted);
    else append_dword(L"MSHTML_FETCH_CONTINUE_FAILURE=", hr);
    HeapFree(GetProcessHeap(), 0, wire);
}

static HRESULT document_open_new_window(ProbeDocument *document, LPCWSTR url,
                                        LPCWSTR name, VARIANT *post,
                                        VARIANT *headers, DWORD flags)
{
    TritonDocObjectService *events = NULL;
    IWebBrowser2 *browser = NULL;
    VARIANT destination, options, target, body, header_text;
    BYTE *post_bytes = NULL;
    ULONG post_length = 0;
    BOOL cancelled = FALSE;
    HRESULT hr;
    if(!url || !*url) return E_INVALIDARG;
    VariantInit(&destination); VariantInit(&options); VariantInit(&target);
    VariantInit(&body); VariantInit(&header_text);
    if(post && post->vt != VT_EMPTY && post->vt != VT_NULL) {
        if(post->vt != (VT_ARRAY | VT_UI1) || !post->parray ||
           SafeArrayGetDim(post->parray) != 1) return E_NOTIMPL;
        post_length = SafeArrayGetElemsize(post->parray) * post->parray->rgsabound[0].cElements;
        if(FAILED(SafeArrayAccessData(post->parray, (void **)&post_bytes))) return E_INVALIDARG;
    }
    if(document->client_site && SUCCEEDED(document_get_browser_events(document->client_site, &events)) &&
       events->lpVtbl->FireBeforeNavigate2) {
        hr = events->lpVtbl->FireBeforeNavigate2(events,
            (IDispatch *)&document->IHTMLWindow2_iface, url, flags | 8,
            name && *name ? name : L"_blank", post_bytes, post_length,
            headers && headers->vt == VT_BSTR ? headers->bstrVal : NULL, FALSE, &cancelled);
        events->lpVtbl->Release(events);
        if(FAILED(hr)) {
            if(post_bytes) SafeArrayUnaccessData(post->parray);
        }
        if(FAILED(hr)) return hr;
    }
    if(post_bytes) SafeArrayUnaccessData(post->parray);
    if(cancelled) {
        append_text(L"MSHTML_NEW_WINDOW_HOST_OWNED\r\n");
        return S_OK;
    }
    hr = CoCreateInstance(&CLSID_InternetExplorer, NULL, CLSCTX_LOCAL_SERVER,
                          &IID_IWebBrowser2, (void **)&browser);
    if(SUCCEEDED(hr)) {
        destination.vt = VT_BSTR; destination.bstrVal = SysAllocString(url);
        options.vt = VT_I4; options.lVal = 1; /* navOpenInNewWindow */
        target.vt = VT_BSTR; target.bstrVal = SysAllocString(name && *name ? name : L"_blank");
        if(post && post->vt != VT_EMPTY && post->vt != VT_NULL) hr = VariantCopy(&body, post);
        if(SUCCEEDED(hr) && headers && headers->vt != VT_EMPTY && headers->vt != VT_NULL)
            hr = VariantCopy(&header_text, headers);
        if(SUCCEEDED(hr) && (!destination.bstrVal || !target.bstrVal)) hr = E_OUTOFMEMORY;
        if(SUCCEEDED(hr)) hr = IWebBrowser2_Navigate2(browser, &destination, &options,
                                                      &target, &body, &header_text);
        if(SUCCEEDED(hr)) hr = IWebBrowser2_put_Visible(browser, VARIANT_TRUE);
        IWebBrowser2_Release(browser);
    }
    VariantClear(&destination); VariantClear(&target); VariantClear(&body); VariantClear(&header_text);
    append_dword(L"MSHTML_NEW_WINDOW_IE_HOST_RESULT=", hr);
    return hr;
}

static void document_open_new_window_event(ProbeDocument *document, const char *params)
{
    BSTR payload = NULL, url = NULL, name = NULL, disposition = NULL;
    char *utf8 = NULL;
    int bytes;
    HRESULT hr = mj_bstr(mj_member(params, "payload"), &payload);
    if(SUCCEEDED(hr)) {
        bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, payload, -1, NULL, 0, NULL, NULL);
        utf8 = bytes > 0 ? HeapAlloc(GetProcessHeap(), 0, bytes) : NULL;
        if(!utf8 || !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, payload, -1,
                                         utf8, bytes, NULL, NULL)) hr = E_FAIL;
    }
    if(SUCCEEDED(hr)) hr = mj_bstr(mj_member(utf8, "url"), &url);
    if(SUCCEEDED(hr)) hr = mj_bstr(mj_member(utf8, "name"), &name);
    if(SUCCEEDED(hr)) hr = mj_bstr(mj_member(utf8, "disposition"), &disposition);
    if(SUCCEEDED(hr) && lstrcmpW(disposition, L"script") && lstrcmpW(disposition, L"link"))
        hr = E_INVALIDARG;
    if(SUCCEEDED(hr)) hr = document_open_new_window(document, url, name, NULL, NULL, 8);
    append_dword(L"MSHTML_RENDERER_NEW_WINDOW_RESULT=", hr);
    SysFreeString(payload); SysFreeString(url); SysFreeString(name); SysFreeString(disposition);
    HeapFree(GetProcessHeap(), 0, utf8);
}

static HRESULT private_navigate(ProbeDocument *document, LPCWSTR url, BSTR location,
                                BSTR frame, VARIANT *post, VARIANT *headers, DWORD flags)
{
    HRESULT hr;
    BOOL transaction = (post && post->vt != VT_EMPTY && post->vt != VT_NULL) ||
                       (headers && headers->vt != VT_EMPTY && headers->vt != VT_NULL &&
                        !(headers->vt == VT_BSTR && !SysStringLen(headers->bstrVal)));
    append_dword(L"MSHTML_PRIVATE_NAVIGATION_FLAGS=", flags);
    if(flags & 8)
        return document_open_new_window(document, url, frame, post, headers, flags);
    if(frame && *frame && lstrcmpiW(frame, L"_self") && lstrcmpiW(frame, L"_top") &&
       lstrcmpiW(frame, L"_parent") && (!document->window_name || lstrcmpW(frame, document->window_name)))
        return E_NOTIMPL;
    if(location && *location) {
        /* Fragment location is not an alternate destination. */
        BSTR combined;
        UINT prefix, suffix;
        LPCWSTR fragment;
        if(!url || transaction) return E_INVALIDARG;
        fragment = wcschr(url, L'#');
        prefix = fragment ? (UINT)(fragment - url) : (UINT)lstrlenW(url);
        if(location[0] == L'#') ++location;
        suffix = lstrlenW(location);
        if(prefix > 1600 || suffix > 1600 || prefix + suffix + 1 > 1600) return E_INVALIDARG;
        combined = SysAllocStringLen(NULL, prefix + 1 + suffix);
        if(!combined) return E_OUTOFMEMORY;
        memcpy(combined, url, prefix * sizeof(WCHAR));
        combined[prefix] = L'#';
        memcpy(combined + prefix + 1, location, (suffix + 1) * sizeof(WCHAR));
        hr = navigate_document_url(document, combined);
        SysFreeString(combined);
        return hr;
    }
    if(transaction) {
        hr = content_ensure_page(document);
        if(SUCCEEDED(hr)) hr = document_request_prepare(document, url, post, headers);
        if(SUCCEEDED(hr)) hr = navigate_document_url(document, url);
        if(FAILED(hr)) document_request_clear(document);
        return hr;
    }
    document_request_clear(document);
    return navigate_document_url(document, url);
}
static HRESULT STDMETHODCALLTYPE private_super_navigate(TritonPrivateWindow4 *iface, BSTR url,
    BSTR location, BSTR shortcut, BSTR frame, VARIANT *post, VARIANT *headers, DWORD flags)
{
    (void)shortcut;
    append_text(L"IHTMLPRIVATEWINDOW4_SUPERNAVIGATE\r\n");
    return private_navigate(from_private_window(iface), url, location, frame, post, headers, flags);
}
static HRESULT STDMETHODCALLTYPE private_super_navigate2(TritonPrivateWindow4 *iface, IUri *uri,
    BSTR location, BSTR shortcut, BSTR frame, VARIANT *post, VARIANT *headers, DWORD flags)
{
    BSTR url = NULL;
    HRESULT hr;
    (void)shortcut;
    append_text(L"IHTMLPRIVATEWINDOW4_SUPERNAVIGATE2\r\n");
    if(!uri) return E_INVALIDARG;
    hr = IUri_GetAbsoluteUri(uri, &url);
    if(SUCCEEDED(hr)) hr = private_navigate(from_private_window(iface), url, location, frame, post, headers, flags);
    SysFreeString(url);
    return hr;
}
static HRESULT STDMETHODCALLTYPE private_super_navigate3(TritonPrivateWindow4 *iface, IUri *uri,
    BSTR location, BSTR shortcut, BSTR frame, IStream *post, DWORD flags)
{
    if(post) return E_NOTIMPL;
    return private_super_navigate2(iface, uri, location, shortcut, frame, NULL, NULL, flags);
}
static HRESULT STDMETHODCALLTYPE private_pending_url(TritonPrivateWindow4 *iface, BSTR *url)
{
    ProbeDocument *document = from_private_window(iface);
    if(!url) return E_POINTER;
    *url = NULL;
    if(document->ready_state == READYSTATE_COMPLETE || !document->current_url) return S_FALSE;
    *url = SysAllocString(document->current_url);
    return *url ? S_OK : E_OUTOFMEMORY;
}
static HRESULT STDMETHODCALLTYPE private_address_url(TritonPrivateWindow4 *iface, BSTR *url)
{ return html2_get_url(&from_private_window(iface)->IHTMLDocument2_iface, url); }
static HRESULT STDMETHODCALLTYPE private_pics_target(TritonPrivateWindow4 *iface, IOleCommandTarget *target)
{ (void)iface; (void)target; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE private_pics_complete(TritonPrivateWindow4 *iface, BOOL complete)
{ (void)iface; (void)complete; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE private_find_window(TritonPrivateWindow4 *iface, LPCWSTR name, IHTMLWindow2 **window)
{
    ProbeDocument *document = from_private_window(iface);
    if(!window) return E_POINTER;
    *window = NULL;
    if(!name) return E_INVALIDARG;
    if(!lstrcmpiW(name, L"_self") || !lstrcmpiW(name, L"_top") ||
       !lstrcmpiW(name, L"_parent") || (document->window_name && !lstrcmpW(name, document->window_name)))
        return document_parent_window(document, window);
    return S_FALSE;
}
static const TritonPrivateWindow4Vtbl private_window_vtbl = {
    private_query, private_addref, private_release, private_super_navigate,
    private_pending_url, private_pics_target, private_pics_complete, private_find_window,
    private_address_url, private_super_navigate2, private_super_navigate3
};

static HRESULT document_host_navigation(ProbeDocument *document, IOleClientSite *site, BOOL enable)
{
    IOleCommandTarget *commands = NULL;
    VARIANT window;
    HRESULT hr;
    if(!site) return S_OK;
    hr = IOleClientSite_QueryInterface(site, &IID_IOleCommandTarget, (void **)&commands);
    if(FAILED(hr)) return hr;
    document_add_ref(document);
    VariantInit(&window);
    window.vt = VT_UNKNOWN;
    window.punkVal = (IUnknown *)&document->IHTMLWindow2_iface;
    hr = IOleCommandTarget_Exec(commands, &triton_doc_host_commands, 0, 0, enable ? &window : NULL, NULL);
    append_dword(enable ? L"MSHTML_HOST_DOCCANNAVIGATE=": L"MSHTML_HOST_NAVIGATION_DISCONNECT=", hr);
    if(enable && SUCCEEDED(hr)) {
        HRESULT notify_hr = IOleCommandTarget_Exec(commands, &triton_doc_host_commands, 13, 0, NULL, NULL);
        append_dword(L"MSHTML_HOST_DOCNEEDSNAVNOTIFICATIONS=", notify_hr);
    }
    IOleCommandTarget_Release(commands);
    document_release(document);
    return hr;
}

static HRESULT document_get_browser_events(IOleClientSite *site, TritonDocObjectService **events)
{
    IServiceProvider *provider = NULL;
    IUnknown *browser = NULL;
    HRESULT hr;
    *events = NULL;
    if(!site) return E_NOINTERFACE;
    hr = IOleClientSite_QueryInterface(site, &IID_IServiceProvider, (void **)&provider);
    if(SUCCEEDED(hr)) {
        /* CDoc::InitDocHost at 771ccc9f: SID_SShellBrowser has the same
         * value as IID_IShellBrowser; the requested IID is IBrowserService. */
        hr = IServiceProvider_QueryService(provider, &IID_IShellBrowser,
            &triton_browser_service_iid, (void **)&browser);
        IServiceProvider_Release(provider);
    }
    if(SUCCEEDED(hr)) {
        hr = IUnknown_QueryInterface(browser, &triton_doc_object_service_iid, (void **)events);
        IUnknown_Release(browser);
    }
    return hr;
}

static ProbeDocument *from_connections(IConnectionPointContainer *iface)
{ return CONTAINING_RECORD(iface, ProbeDocument, connections_iface); }
static ProbeDocument *from_property_point(IConnectionPoint *iface)
{ return CONTAINING_RECORD(iface, ProbeDocument, property_point_iface); }
static HRESULT STDMETHODCALLTYPE connections_query(IConnectionPointContainer *iface, REFIID iid, void **out)
{ return document_query_interface(from_connections(iface), iid, out); }
static ULONG STDMETHODCALLTYPE connections_addref(IConnectionPointContainer *iface)
{ return document_add_ref(from_connections(iface)); }
static ULONG STDMETHODCALLTYPE connections_release(IConnectionPointContainer *iface)
{ return document_release(from_connections(iface)); }
static HRESULT STDMETHODCALLTYPE connections_enum(IConnectionPointContainer *iface, IEnumConnectionPoints **out)
{ (void)iface; if(!out) return E_POINTER; *out = NULL; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE connections_find(IConnectionPointContainer *iface, REFIID iid, IConnectionPoint **out)
{
    ProbeDocument *document = from_connections(iface);
    if(!out) return E_POINTER;
    *out = NULL;
    if(!IsEqualIID(iid, &IID_IPropertyNotifySink)) return CONNECT_E_NOCONNECTION;
    *out = &document->property_point_iface;
    document_add_ref(document);
    return S_OK;
}
static IConnectionPointContainerVtbl connections_vtbl = {
    connections_query, connections_addref, connections_release, connections_enum, connections_find
};
static HRESULT STDMETHODCALLTYPE property_query(IConnectionPoint *iface, REFIID iid, void **out)
{
    if(!out) return E_POINTER;
    *out = NULL;
    if(!IsEqualIID(iid, &IID_IUnknown) && !IsEqualIID(iid, &IID_IConnectionPoint)) return E_NOINTERFACE;
    *out = iface;
    document_add_ref(from_property_point(iface));
    return S_OK;
}
static ULONG STDMETHODCALLTYPE property_addref(IConnectionPoint *iface)
{ return document_add_ref(from_property_point(iface)); }
static ULONG STDMETHODCALLTYPE property_release(IConnectionPoint *iface)
{ return document_release(from_property_point(iface)); }
static HRESULT STDMETHODCALLTYPE property_iid(IConnectionPoint *iface, IID *iid)
{ (void)iface; if(!iid) return E_POINTER; *iid = IID_IPropertyNotifySink; return S_OK; }
static HRESULT STDMETHODCALLTYPE property_container(IConnectionPoint *iface, IConnectionPointContainer **out)
{ return document_query_interface(from_property_point(iface), &IID_IConnectionPointContainer, (void **)out); }
static HRESULT STDMETHODCALLTYPE property_advise(IConnectionPoint *iface, IUnknown *unknown, DWORD *cookie)
{
    ProbeDocument *document = from_property_point(iface);
    PropertySubscription *subscription;
    IPropertyNotifySink *sink = NULL;
    if(!cookie) return E_POINTER;
    *cookie = 0;
    if(!unknown) return E_POINTER;
    if(FAILED(IUnknown_QueryInterface(unknown, &IID_IPropertyNotifySink, (void **)&sink)))
        return CONNECT_E_CANNOTCONNECT;
    if(document->next_property_cookie == MAXDWORD) {
        IPropertyNotifySink_Release(sink);
        return CONNECT_E_ADVISELIMIT;
    }
    subscription = HeapAlloc(GetProcessHeap(), 0, sizeof(*subscription));
    if(!subscription) { IPropertyNotifySink_Release(sink); return E_OUTOFMEMORY; }
    subscription->sink = sink;
    subscription->cookie = ++document->next_property_cookie;
    subscription->next = document->property_subscriptions;
    document->property_subscriptions = subscription;
    *cookie = subscription->cookie;
    append_text(L"MSHTML_PROPERTY_SINK_ADVISED\r\n");
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE property_unadvise(IConnectionPoint *iface, DWORD cookie)
{
    ProbeDocument *document = from_property_point(iface);
    PropertySubscription **link = &document->property_subscriptions, *subscription;
    while(*link && (*link)->cookie != cookie) link = &(*link)->next;
    if(!*link) return CONNECT_E_NOCONNECTION;
    subscription = *link;
    *link = subscription->next;
    IPropertyNotifySink_Release(subscription->sink);
    HeapFree(GetProcessHeap(), 0, subscription);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE property_enum(IConnectionPoint *iface, IEnumConnections **out)
{ (void)iface; if(!out) return E_POINTER; *out = NULL; return E_NOTIMPL; }
static IConnectionPointVtbl property_point_vtbl = {
    property_query, property_addref, property_release, property_iid, property_container,
    property_advise, property_unadvise, property_enum
};

static ProbeDocument *from_security_manager(IInternetSecurityManager *iface)
{ return CONTAINING_RECORD(iface, ProbeDocument, IInternetSecurityManager_iface); }
static HRESULT security_default(IInternetSecurityManager **manager)
{ return CoInternetCreateSecurityManager(NULL, manager, 0); }
static HRESULT STDMETHODCALLTYPE security_qi(IInternetSecurityManager *iface, REFIID iid, void **out)
{ return document_query_interface(from_security_manager(iface), iid, out); }
static ULONG STDMETHODCALLTYPE security_addref(IInternetSecurityManager *iface)
{ return document_add_ref(from_security_manager(iface)); }
static ULONG STDMETHODCALLTYPE security_release(IInternetSecurityManager *iface)
{ return document_release(from_security_manager(iface)); }
static HRESULT STDMETHODCALLTYPE security_set_site(IInternetSecurityManager *iface,
                                                    IInternetSecurityMgrSite *site)
{ (void)iface; (void)site; return INET_E_DEFAULT_ACTION; }
static HRESULT STDMETHODCALLTYPE security_get_site(IInternetSecurityManager *iface,
                                                    IInternetSecurityMgrSite **site)
{ (void)iface; if(!site) return E_POINTER; *site = NULL; return INET_E_DEFAULT_ACTION; }
static HRESULT STDMETHODCALLTYPE security_map_zone(IInternetSecurityManager *iface,
                                                    LPCWSTR url, DWORD *zone, DWORD flags)
{
    IInternetSecurityManager *manager = NULL;
    HRESULT hr;
    (void)iface;
    if(!zone) return E_POINTER;
    hr = security_default(&manager);
    if(SUCCEEDED(hr)) {
        hr = IInternetSecurityManager_MapUrlToZone(manager, url, zone, flags);
        IInternetSecurityManager_Release(manager);
    }
    return hr;
}
static HRESULT STDMETHODCALLTYPE security_id(IInternetSecurityManager *iface, LPCWSTR url,
    BYTE *id, DWORD *count, DWORD_PTR reserved)
{
    IInternetSecurityManager *manager = NULL; HRESULT hr;
    (void)iface; hr = security_default(&manager);
    if(SUCCEEDED(hr)) { hr = IInternetSecurityManager_GetSecurityId(manager, url, id, count, reserved);
        IInternetSecurityManager_Release(manager); }
    return hr;
}
static HRESULT STDMETHODCALLTYPE security_action(IInternetSecurityManager *iface, LPCWSTR url,
    DWORD action, BYTE *policy, DWORD policy_size, BYTE *context, DWORD context_size,
    DWORD flags, DWORD reserved)
{
    IInternetSecurityManager *manager = NULL; HRESULT hr;
    (void)iface; hr = security_default(&manager);
    if(SUCCEEDED(hr)) { hr = IInternetSecurityManager_ProcessUrlAction(manager, url, action,
        policy, policy_size, context, context_size, flags, reserved); IInternetSecurityManager_Release(manager); }
    return hr;
}
static HRESULT STDMETHODCALLTYPE security_custom(IInternetSecurityManager *iface, LPCWSTR url,
    REFGUID key, BYTE **policy, DWORD *policy_size, BYTE *context, DWORD context_size, DWORD reserved)
{
    IInternetSecurityManager *manager = NULL; HRESULT hr;
    (void)iface; hr = security_default(&manager);
    if(SUCCEEDED(hr)) { hr = IInternetSecurityManager_QueryCustomPolicy(manager, url, key,
        policy, policy_size, context, context_size, reserved); IInternetSecurityManager_Release(manager); }
    return hr;
}
static HRESULT STDMETHODCALLTYPE security_set_mapping(IInternetSecurityManager *iface,
    DWORD zone, LPCWSTR pattern, DWORD flags)
{
    IInternetSecurityManager *manager = NULL; HRESULT hr;
    (void)iface; hr = security_default(&manager);
    if(SUCCEEDED(hr)) { hr = IInternetSecurityManager_SetZoneMapping(manager, zone, pattern, flags);
        IInternetSecurityManager_Release(manager); }
    return hr;
}
static HRESULT STDMETHODCALLTYPE security_get_mappings(IInternetSecurityManager *iface,
    DWORD zone, IEnumString **strings, DWORD flags)
{
    IInternetSecurityManager *manager = NULL; HRESULT hr;
    (void)iface; hr = security_default(&manager);
    if(SUCCEEDED(hr)) { hr = IInternetSecurityManager_GetZoneMappings(manager, zone, strings, flags);
        IInternetSecurityManager_Release(manager); }
    return hr;
}
static IInternetSecurityManagerVtbl security_manager_vtbl = {
    security_qi,security_addref,security_release,security_set_site,security_get_site,
    security_map_zone,security_id,security_action,security_custom,security_set_mapping,
    security_get_mappings
};

static void document_report_security(ProbeDocument *document)
{
    IOleCommandTarget *target = NULL;
    DWORD zone = URLZONE_INVALID;
    VARIANT lock;
    HRESULT hr;
    if(document->security_reported_serial == document->navigation_serial ||
       !document->current_url) return;
    document->security_reported_serial = document->navigation_serial;
    hr = security_map_zone(&document->IInternetSecurityManager_iface,
                           document->current_url, &zone, 0);
    if(SUCCEEDED(hr)) append_dword(L"MSHTML_SECURITY_ZONE=", zone);
    if(!document->client_site || FAILED(IOleClientSite_QueryInterface(document->client_site,
        &IID_IOleCommandTarget, (void **)&target))) return;
    VariantInit(&lock); lock.vt = VT_I4;
    lock.lVal = !document->navigation_security_error &&
        !wcsncmp(document->current_url, L"https://", 8) ? secureLockIconSecure128Bit :
                                                          secureLockIconUnsecure;
    hr = IOleCommandTarget_Exec(target, &history_explorer_commands, 25,
                                OLECMDEXECOPT_DONTPROMPTUSER, &lock, NULL);
    append_dword(L"MSHTML_HOST_SECURE_LOCK_RESULT=", hr);
    IOleCommandTarget_Release(target);
}
static void document_set_ready_state(ProbeDocument *document, LONG state)
{
    PropertySubscription *subscription;
    IPropertyNotifySink **snapshot;
    size_t count = 0, index = 0;
    if(document->ready_state == state) return;
    document->ready_state = state;
    if(state == READYSTATE_COMPLETE) document_report_security(document);
    for(subscription = document->property_subscriptions; subscription; subscription = subscription->next) ++count;
    if(!count) return;
    snapshot = HeapAlloc(GetProcessHeap(), 0, count * sizeof(*snapshot));
    if(!snapshot) return;
    document_add_ref(document);
    for(subscription = document->property_subscriptions; subscription; subscription = subscription->next) {
        snapshot[index++] = subscription->sink;
        IPropertyNotifySink_AddRef(subscription->sink);
    }
    for(index = 0; index < count; ++index) {
        IPropertyNotifySink_OnChanged(snapshot[index], DISPID_READYSTATE);
        IPropertyNotifySink_Release(snapshot[index]);
    }
    HeapFree(GetProcessHeap(), 0, snapshot);
    document_release(document);
}
static void document_clear_subscriptions(ProbeDocument *document)
{
    PropertySubscription *subscription;
    while((subscription = document->property_subscriptions)) {
        document->property_subscriptions = subscription->next;
        IPropertyNotifySink_Release(subscription->sink);
        HeapFree(GetProcessHeap(), 0, subscription);
    }
}
#include "../build/ie7_window_stubs.inc"
