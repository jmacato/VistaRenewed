/* Renderer-backed document content and stream persistence. Included after
 * the document/session helpers; this is not an independent HTML model. */
#define CONTENT_MAX_CHARS (1024u * 1024u)
#define CONTENT_REPLY_BYTES (8u * 1024u * 1024u)

static HRESULT content_remote_result(const char *reply, const char **result)
{
    const char *outer = mj_member(reply, "result");
    if(!outer || mj_member(outer, "exceptionDetails")) return E_FAIL;
    *result = mj_member(outer, "result");
    return *result ? S_OK : E_FAIL;
}

static HRESULT content_ensure_page(ProbeDocument *document)
{
    HRESULT hr;
    unsigned attempt;
    if(document->page_target[0] && document->page_session[0]) return S_OK;
    hr = navigate_document_url(document, L"about:blank");
    if(FAILED(hr)) return hr;
    /* Page.navigate acknowledges before commit. Never write privileged HTML
     * into the file-origin bootstrap document while about:blank is pending. */
    for(attempt = 0; attempt < 50; ++attempt) {
        char reply[2048];
        const char *result;
        BSTR url = NULL;
        hr = document_pipe_call(document, "Runtime.evaluate",
            "{\"expression\":\"location.href\",\"returnByValue\":true,\"timeout\":1000}",
            document->page_session, reply, sizeof(reply));
        if(SUCCEEDED(hr) && SUCCEEDED(content_remote_result(reply, &result)) &&
           SUCCEEDED(mj_bstr(mj_member(result, "value"), &url))) {
            BOOL ready = !lstrcmpW(url, L"about:blank");
            SysFreeString(url);
            if(ready) { document_process_events(document); return S_OK; }
        }
        Sleep(100);
    }
    return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
}

static void content_release_object(ProbeDocument *document, BSTR object_id)
{
    char *quoted = mj_quote(object_id), params[2048], reply[2048];
    int n;
    if(!quoted) return;
    n = snprintf(params, sizeof(params), "{\"objectId\":%s}", quoted);
    if(n > 0 && (size_t)n < sizeof(params) && document->page_session[0])
        document_pipe_call(document, "Runtime.releaseObject", params,
                            document->page_session, reply, sizeof(reply));
    HeapFree(GetProcessHeap(), 0, quoted);
}

static HRESULT content_acquire_document(ProbeDocument *document, BSTR *object_id)
{
    char reply[4096];
    const char *result;
    HRESULT hr = content_ensure_page(document);
    *object_id = NULL;
    if(FAILED(hr)) return hr;
    document_process_events(document);
    hr = document_pipe_call(document, "Runtime.evaluate",
        "{\"expression\":\"document\",\"timeout\":1000}", document->page_session, reply, sizeof(reply));
    /* Replies are ordered after prior commit events on this CDP connection.
     * Settle the DOM generation before publishing wrappers for the result. */
    document_process_events(document);
    if(SUCCEEDED(hr)) hr = content_remote_result(reply, &result);
    if(SUCCEEDED(hr)) hr = mj_bstr(mj_member(result, "objectId"), object_id);
    return hr;
}

static char *content_argument(BSTR argument)
{
    char *quoted = mj_quote(argument), *out;
    size_t size;
    if(!quoted) return NULL;
    size = strlen(quoted) + 16;
    out = HeapAlloc(GetProcessHeap(), 0, size);
    if(out) snprintf(out, size, "[{\"value\":%s}]", quoted);
    HeapFree(GetProcessHeap(), 0, quoted);
    return out;
}

/* arguments is assembled only by internal, JSON-escaping callers. A caller
 * owns its remote reference; this helper never rebinds a stale element. */
static HRESULT content_call_object(ProbeDocument *document, BSTR object_id, LPCWSTR function,
                                   const char *arguments, BOOL by_value, char **reply)
{
    char *params = NULL, *quoted_id = NULL, *quoted_function = NULL;
    BSTR declaration = NULL;
    HRESULT hr;
    size_t length;
    *reply = NULL;
    if(!object_id || !document->page_target[0] || !document->page_session[0])
        return CO_E_OBJNOTCONNECTED;
    EnterCriticalSection(&document->runtime->transport_lock);
    quoted_id = mj_quote(object_id);
    declaration = SysAllocString(function);
    if(declaration) quoted_function = mj_quote(declaration);
    if(!quoted_id || !quoted_function) {
        hr = E_OUTOFMEMORY; goto done;
    }
    length = strlen(quoted_id) + strlen(quoted_function) + strlen(arguments) + 256;
    params = HeapAlloc(GetProcessHeap(), 0, length);
    *reply = HeapAlloc(GetProcessHeap(), 0, CONTENT_REPLY_BYTES);
    if(!params || !*reply) { hr = E_OUTOFMEMORY; goto done; }
    snprintf(params, length,
        "{\"objectId\":%s,\"functionDeclaration\":%s,\"returnByValue\":%s,"
        "\"arguments\":%s}", quoted_id, quoted_function, by_value ? "true" : "false", arguments);
    hr = document_pipe_call(document, "Runtime.callFunctionOn", params,
                            document->page_session, *reply, CONTENT_REPLY_BYTES);
done:
    HeapFree(GetProcessHeap(), 0, params);
    HeapFree(GetProcessHeap(), 0, quoted_id);
    HeapFree(GetProcessHeap(), 0, quoted_function);
    SysFreeString(declaration);
    LeaveCriticalSection(&document->runtime->transport_lock);
    if(FAILED(hr)) { HeapFree(GetProcessHeap(), 0, *reply); *reply = NULL; }
    return hr;
}

static HRESULT content_call(ProbeDocument *document, LPCWSTR function, BSTR argument,
                            BOOL has_argument, BSTR *value)
{
    BSTR object_id = NULL;
    char *args = NULL, *reply = NULL;
    const char *result;
    HRESULT hr;
    if(value) *value = NULL;
    if(has_argument && SysStringLen(argument) > CONTENT_MAX_CHARS) return E_INVALIDARG;
    hr = content_acquire_document(document, &object_id);
    if(FAILED(hr)) return hr;
    if(has_argument && !(args = content_argument(argument))) { hr = E_OUTOFMEMORY; goto done; }
    hr = content_call_object(document, object_id, function, has_argument ? args : "[]", TRUE, &reply);
    if(SUCCEEDED(hr)) hr = content_remote_result(reply, &result);
    if(SUCCEEDED(hr) && value) hr = mj_bstr(mj_member(result, "value"), value);
    if(SUCCEEDED(hr) && value && SysStringLen(*value) > CONTENT_MAX_CHARS) {
        SysFreeString(*value); *value = NULL;
        hr = HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
    }
done:
    content_release_object(document, object_id);
    HeapFree(GetProcessHeap(), 0, reply);
    HeapFree(GetProcessHeap(), 0, args);
    SysFreeString(object_id);
    return hr;
}

static HRESULT content_get_title(ProbeDocument *document, BSTR *value)
{
    if(!value) return E_POINTER;
    *value = NULL;
    if(!document->page_target[0]) {
        *value = SysAllocString(L"");
        return *value ? S_OK : E_OUTOFMEMORY;
    }
    return content_call(document, L"function(){return this.title;}", NULL, FALSE, value);
}

static HRESULT content_put_title(ProbeDocument *document, BSTR value)
{
    return content_call(document, L"function(s){this.title=s;}", value, TRUE, NULL);
}

static HRESULT content_serialize(ProbeDocument *document, BSTR *value)
{
    return content_call(document,
        L"function(){return (this.doctype?new XMLSerializer().serializeToString(this.doctype):'')"
        L"+(this.documentElement?this.documentElement.outerHTML:'');}", NULL, FALSE, value);
}

static HRESULT content_write(ProbeDocument *document, SAFEARRAY *array, BOOL newline)
{
    LONG lower, upper, index;
    VARTYPE type;
    VARIANT *values = NULL, converted;
    BSTR combined = NULL;
    UINT used = 0;
    HRESULT hr;
    if(!array) return E_INVALIDARG;
    if(SafeArrayGetDim(array) != 1 || FAILED(SafeArrayGetVartype(array, &type)) || type != VT_VARIANT)
        return E_INVALIDARG;
    if(FAILED(SafeArrayGetLBound(array, 1, &lower)) || FAILED(SafeArrayGetUBound(array, 1, &upper)))
        return E_INVALIDARG;
    if((LONGLONG)upper - lower + 1 > 65536) return E_INVALIDARG;
    hr = SafeArrayAccessData(array, (void **)&values);
    if(FAILED(hr)) return hr;
    combined = SysAllocStringLen(NULL, CONTENT_MAX_CHARS);
    if(!combined) { SafeArrayUnaccessData(array); return E_OUTOFMEMORY; }
    hr = S_OK;
    for(index = 0; (LONGLONG)index <= (LONGLONG)upper - lower; ++index) {
        UINT length;
        VariantInit(&converted);
        hr = VariantChangeType(&converted, &values[index], 0, VT_BSTR);
        if(FAILED(hr)) { VariantClear(&converted); break; }
        length = SysStringLen(converted.bstrVal);
        if(length > CONTENT_MAX_CHARS - used) hr = E_INVALIDARG;
        else {
            memcpy(combined + used, converted.bstrVal, length * sizeof(WCHAR));
            used += length;
        }
        VariantClear(&converted);
        if(FAILED(hr)) break;
    }
    SafeArrayUnaccessData(array);
    if(SUCCEEDED(hr)) {
        BSTR sized = SysAllocStringLen(combined, used);
        if(!sized) hr = E_OUTOFMEMORY;
        else {
            hr = content_call(document, newline ? L"function(s){this.writeln(s);}"
                                               : L"function(s){this.write(s);}", sized, TRUE, NULL);
            SysFreeString(sized);
        }
    }
    SysFreeString(combined);
    return hr;
}

static HRESULT content_close(ProbeDocument *document)
{
    return content_call(document, L"function(){this.close();}", NULL, FALSE, NULL);
}

static ProbeDocument *content_from_stream(IPersistStreamInit *iface)
{
    return (ProbeDocument *)((char *)iface - offsetof(ProbeDocument, IPersistStreamInit_iface));
}
static HRESULT STDMETHODCALLTYPE content_stream_query(IPersistStreamInit *iface, REFIID iid, void **out)
{ return document_query_interface(content_from_stream(iface), iid, out); }
static ULONG STDMETHODCALLTYPE content_stream_addref(IPersistStreamInit *iface)
{ return document_add_ref(content_from_stream(iface)); }
static ULONG STDMETHODCALLTYPE content_stream_release(IPersistStreamInit *iface)
{ return document_release(content_from_stream(iface)); }
static HRESULT STDMETHODCALLTYPE content_stream_class(IPersistStreamInit *iface, CLSID *id)
{ (void)iface; return document_get_class_id(id); }

static HRESULT content_remember_persisted(ProbeDocument *document)
{
    BSTR html = NULL;
    HRESULT hr = content_serialize(document, &html);
    if(SUCCEEDED(hr)) {
        SysFreeString(document->persisted_html);
        document->persisted_html = html;
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE content_stream_dirty(IPersistStreamInit *iface)
{
    ProbeDocument *document = content_from_stream(iface);
    BSTR html = NULL;
    HRESULT hr;
    if(!document->page_target[0]) return S_FALSE;
    hr = content_serialize(document, &html);
    if(FAILED(hr)) return hr;
    hr = document->persisted_html && SysStringLen(html) == SysStringLen(document->persisted_html) &&
         !memcmp(html, document->persisted_html, SysStringByteLen(html)) ? S_FALSE : S_OK;
    SysFreeString(html);
    return hr;
}

static HRESULT content_decode(const BYTE *bytes, ULONG size, BSTR *out)
{
    UINT i;
    int count;
    UINT page = CP_UTF8;
    *out = NULL;
    if(size >= 2 && ((bytes[0] == 0xff && bytes[1] == 0xfe) ||
                     (bytes[0] == 0xfe && bytes[1] == 0xff))) {
        BOOL little = bytes[0] == 0xff;
        if(size & 1) return HRESULT_FROM_WIN32(ERROR_NO_UNICODE_TRANSLATION);
        *out = SysAllocStringLen(NULL, size / 2 - 1);
        if(!*out) return E_OUTOFMEMORY;
        for(i = 2; i < size; i += 2)
            (*out)[i / 2 - 1] = little ? bytes[i] | ((WORD)bytes[i + 1] << 8)
                                       : ((WORD)bytes[i] << 8) | bytes[i + 1];
        return S_OK;
    }
    if(size >= 3 && !memcmp(bytes, "\xef\xbb\xbf", 3)) { bytes += 3; size -= 3; }
    if(!size) { *out = SysAllocString(L""); return *out ? S_OK : E_OUTOFMEMORY; }
    count = MultiByteToWideChar(page, MB_ERR_INVALID_CHARS, (const char *)bytes, size, NULL, 0);
    if(!count) return HRESULT_FROM_WIN32(ERROR_NO_UNICODE_TRANSLATION);
    *out = SysAllocStringLen(NULL, count);
    if(!*out) return E_OUTOFMEMORY;
    if(!MultiByteToWideChar(page, MB_ERR_INVALID_CHARS, (const char *)bytes, size, *out, count)) {
        SysFreeString(*out); *out = NULL;
        return HRESULT_FROM_WIN32(GetLastError());
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE content_stream_load(IPersistStreamInit *iface, IStream *stream)
{
    ProbeDocument *document = content_from_stream(iface);
    BYTE *bytes;
    ULONG used = 0, got;
    BSTR html = NULL;
    HRESULT hr;
    if(!stream) return E_POINTER;
    bytes = HeapAlloc(GetProcessHeap(), 0, CONTENT_MAX_CHARS * 4 + 1);
    if(!bytes) return E_OUTOFMEMORY;
    for(;;) {
        hr = IStream_Read(stream, bytes + used, min(16384, CONTENT_MAX_CHARS * 4 + 1 - used), &got);
        if(FAILED(hr)) goto done;
        used += got;
        if(used > CONTENT_MAX_CHARS * 4) { hr = E_INVALIDARG; goto done; }
        if(!got) break; /* Short successful reads do not necessarily mean EOF. */
    }
    hr = content_decode(bytes, used, &html);
    if(FAILED(hr)) goto done;
    hr = content_call(document, L"function(s){this.open();this.write(s);this.close();}", html, TRUE, NULL);
    if(FAILED(hr)) append_dword(L"MSHTML_STREAM_LOAD_WRITE_FAILURE=", hr);
    if(SUCCEEDED(hr)) {
        hr = content_remember_persisted(document);
        if(FAILED(hr)) append_dword(L"MSHTML_STREAM_LOAD_SERIALIZE_FAILURE=", hr);
    }
done:
    SysFreeString(html);
    HeapFree(GetProcessHeap(), 0, bytes);
    return hr;
}

static HRESULT content_utf8(ProbeDocument *document, BSTR *html, char **bytes, ULONG *size)
{
    HRESULT hr = content_serialize(document, html);
    int length;
    *bytes = NULL;
    if(FAILED(hr)) return hr;
    length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, *html, SysStringLen(*html), NULL, 0, NULL, NULL);
    if(!length && SysStringLen(*html)) return HRESULT_FROM_WIN32(GetLastError());
    *bytes = HeapAlloc(GetProcessHeap(), 0, (size_t)length + 3);
    if(!*bytes) return E_OUTOFMEMORY;
    memcpy(*bytes, "\xef\xbb\xbf", 3);
    if(length && !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, *html, SysStringLen(*html),
                                     *bytes + 3, length, NULL, NULL)) return HRESULT_FROM_WIN32(GetLastError());
    *size = (ULONG)length + 3;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE content_stream_save(IPersistStreamInit *iface, IStream *stream, BOOL clear)
{
    ProbeDocument *document = content_from_stream(iface);
    BSTR html = NULL;
    char *bytes = NULL;
    ULONG size, written = 0;
    HRESULT hr;
    if(!stream) return E_POINTER;
    hr = content_utf8(document, &html, &bytes, &size);
    if(SUCCEEDED(hr)) {
        hr = IStream_Write(stream, bytes, size, &written);
        if(SUCCEEDED(hr) && written != size) hr = STG_E_WRITEFAULT;
        if(SUCCEEDED(hr) && clear) {
            SysFreeString(document->persisted_html);
            document->persisted_html = html;
            html = NULL;
        }
    }
    SysFreeString(html); HeapFree(GetProcessHeap(), 0, bytes);
    return hr;
}

static HRESULT STDMETHODCALLTYPE content_stream_size(IPersistStreamInit *iface, ULARGE_INTEGER *size)
{
    BSTR html = NULL;
    char *bytes = NULL;
    ULONG length;
    HRESULT hr;
    if(!size) return E_POINTER;
    size->QuadPart = 0;
    hr = content_utf8(content_from_stream(iface), &html, &bytes, &length);
    if(SUCCEEDED(hr)) size->QuadPart = length;
    SysFreeString(html); HeapFree(GetProcessHeap(), 0, bytes);
    return hr;
}

static HRESULT STDMETHODCALLTYPE content_stream_init(IPersistStreamInit *iface)
{
    /* Vista's real HTMLDocument accepts repeated InitNew and Load after
     * InitNew (stock probe a8a749fe2ce64bdb91a00d0ec2d7a0cf). Its concrete
     * contract is more permissive than the generic IPersistStreamInit docs. */
    ProbeDocument *document = content_from_stream(iface);
    HRESULT hr = content_call(document, L"function(){this.open();this.close();}", NULL, FALSE, NULL);
    if(SUCCEEDED(hr)) hr = content_remember_persisted(document);
    return hr;
}

static IPersistStreamInitVtbl content_stream_vtbl = {
    content_stream_query, content_stream_addref, content_stream_release, content_stream_class,
    content_stream_dirty, content_stream_load, content_stream_save, content_stream_size, content_stream_init,
};
