/* Keep Microsoft's registered res protocol as the byte loader. Chromium sees
 * a per-document, intercepted origin; no listener or arbitrary DLL access. */
#include "mshtml_resource_bind.h"
static HRESULT resource_map(ProbeDocument *document, LPCWSTR url, WCHAR *mapped, unsigned capacity)
{
    GUID id;
    WCHAR token[40];
    if(wcsnicmp(url, L"res://ieframe.dll/", 18)) return E_ACCESSDENIED;
    if(!document->resource_origin[0]) {
        HRESULT hr = CoCreateGuid(&id);
        unsigned i;
        if(FAILED(hr)) return hr;
        StringFromGUID2(&id, token, ARRAYSIZE(token));
        for(i = 0; token[i]; ++i) if(token[i] >= 'A' && token[i] <= 'F') token[i] += 'a' - 'A';
        token[37] = 0;
        _snwprintf(document->resource_origin, ARRAYSIZE(document->resource_origin),
                   L"http://triton-res-%ls.invalid/", token + 1);
    }
    if((unsigned)(lstrlenW(document->resource_origin) + lstrlenW(url + 6) + 1) > capacity) return E_INVALIDARG;
    lstrcpyW(mapped, document->resource_origin); lstrcatW(mapped, url + 6);
    return S_OK;
}

static HRESULT resource_enable(ProbeDocument *document)
{
    char origin[128], params[1024], reply[1024];
    HRESULT hr;
    if(!document->resource_origin[0]) return S_OK;
    if(!WideCharToMultiByte(CP_UTF8, 0, document->resource_origin, -1, origin, sizeof(origin), NULL, NULL))
        return E_INVALIDARG;
    if(strcmp(document->resource_script_session, document->page_session)) {
        /* Original httpErrorPagesScripts.js reads element.currentStyle.display.
         * Supply only the computed-style compatibility surface, and only in
         * this internal origin. Public pages keep their native Chromium DOM. */
        snprintf(params, sizeof(params),
            "{\"source\":\"if(location.href.indexOf('%s')===0&&!('currentStyle' in HTMLElement.prototype))"
            "Object.defineProperty(HTMLElement.prototype,'currentStyle',{configurable:true,get:function(){return getComputedStyle(this)}});\"}", origin);
        hr = document_pipe_call(document, "Page.addScriptToEvaluateOnNewDocument", params,
                                document->page_session, reply, sizeof(reply));
        if(FAILED(hr)) return hr;
        strcpy(document->resource_script_session, document->page_session);
    }
    snprintf(params, sizeof(params),
        "{\"patterns\":[{\"urlPattern\":\"%s*\",\"requestStage\":\"Request\"}%s]}", origin,
        document->request_fetch_enabled ?
        ",{\"urlPattern\":\"*\",\"resourceType\":\"Document\",\"requestStage\":\"Request\"}" : "");
    return document_pipe_call(document, "Fetch.enable", params, document->page_session, reply, sizeof(reply));
}

static BOOL resource_paused(ProbeDocument *document, const char *params)
{
    BSTR url = NULL, id = NULL;
    WCHAR native[1800], native_mime[128];
    char mime_buffer[128];
    const WCHAR *path, *p, *extension;
    const char *mime = "application/octet-stream";
    IStream *stream = NULL;
    BYTE *bytes = NULL;
    char *body = NULL, *wire = NULL, *quoted_id = NULL;
    ULONG total = 0, got = 0;
    unsigned origin_length = lstrlenW(document->resource_origin);
    CdpRequest *submitted = NULL;
    HRESULT hr = E_ACCESSDENIED;
    size_t capacity;
    if(!origin_length || FAILED(mj_bstr(mj_member(mj_member(params, "request"), "url"), &url))) return FALSE;
    if(wcsncmp(url, document->resource_origin, origin_length)) { SysFreeString(url); return FALSE; }
    if(FAILED(mj_bstr(mj_member(params, "requestId"), &id))) goto done;
    /* Request IDs are protocol data, not trusted JSON syntax. */
    for(p = id; *p; ++p) if(!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
         (*p >= '0' && *p <= '9') || *p == '-' || *p == '.' || *p == '_')) goto done;
    quoted_id = HeapAlloc(GetProcessHeap(), 0, SysStringLen(id) + 1);
    if(!quoted_id) goto done;
    WideCharToMultiByte(CP_UTF8, 0, id, -1, quoted_id, SysStringLen(id) + 1, NULL, NULL);
    path = url + origin_length;
    if(wcsnicmp(path, L"ieframe.dll/", 12) || lstrlenW(path) > 1700) goto respond;
    /* Bounded first adapter: named ieframe resources only. Reject module paths,
     * escaping, numeric type syntax and directory traversal, not normalize them. */
    for(p = path + 12; *p && *p != '?' && *p != '#'; ++p)
        if(!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
             (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' || *p == '.')) goto respond;
    if(p == path + 12 || wcsstr(path + 12, L"..")) goto respond;
    lstrcpyW(native, L"res://");
    memcpy(native + 6, path, (p - path) * sizeof(WCHAR)); native[6 + (p - path)] = 0;
    hr = native_resource_open(native, &stream, native_mime, ARRAYSIZE(native_mime));
    if(FAILED(hr)) goto respond;
    bytes = HeapAlloc(GetProcessHeap(), 0, 4u * 1024u * 1024u);
    if(!bytes) { hr = E_OUTOFMEMORY; goto respond; }
    do {
        if(total == 4u * 1024u * 1024u) { hr = E_OUTOFMEMORY; break; }
        hr = IStream_Read(stream, bytes + total, 4u * 1024u * 1024u - total, &got);
        total += got;
    } while(SUCCEEDED(hr) && got);
    if(FAILED(hr)) goto respond;
    extension = wcsrchr(native, '.');
    if(extension) {
        if(!lstrcmpiW(extension, L".htm") || !lstrcmpiW(extension, L".html")) mime = "text/html; charset=utf-8";
        else if(!lstrcmpiW(extension, L".css")) mime = "text/css";
        else if(!lstrcmpiW(extension, L".js")) mime = "application/javascript";
        else if(!lstrcmpiW(extension, L".png")) mime = "image/png";
        else if(!lstrcmpiW(extension, L".gif")) mime = "image/gif";
        else if(!lstrcmpiW(extension, L".jpg")) mime = "image/jpeg";
    }
    if(native_mime[0]) {
        for(p = native_mime; *p; ++p)
            if(!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                 (*p >= '0' && *p <= '9') || wcschr(L"/-+.;= ", *p))) break;
        if(!*p && WideCharToMultiByte(CP_UTF8, 0, native_mime, -1, mime_buffer,
                                     sizeof(mime_buffer), NULL, NULL)) mime = mime_buffer;
    }
    body = request_base64(bytes, total);
    if(!body) hr = E_OUTOFMEMORY;
respond:
    capacity = strlen(quoted_id) + (body ? strlen(body) : 0) + 1024;
    wire = HeapAlloc(GetProcessHeap(), 0, capacity);
    if(!wire) goto done;
    snprintf(wire, capacity,
        "{\"requestId\":\"%s\",\"responseCode\":%u,\"responseHeaders\":["
        "{\"name\":\"Content-Type\",\"value\":\"%s\"},"
        "{\"name\":\"Cache-Control\",\"value\":\"no-store\"},"
        "{\"name\":\"Content-Security-Policy\",\"value\":\"default-src 'self' data:; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; object-src 'none'; connect-src 'none'; frame-ancestors 'none'\"}],\"body\":\"%s\"}",
        quoted_id, SUCCEEDED(hr) ? 200 : 404, mime, SUCCEEDED(hr) && body ? body : "");
    append_text(L"MSHTML_NATIVE_RESOURCE="); append_text(url); append_text(L"\r\n");
    append_dword(L"MSHTML_NATIVE_RESOURCE_RESULT=", hr);
    hr = cdp_submit(document->runtime->cdp, "Fetch.fulfillRequest", wire, document->page_session,
                    TRUE, 0, 10000, 0, &submitted);
    if(SUCCEEDED(hr)) cdp_request_release(submitted);
done:
    if(stream) IStream_Release(stream);
    HeapFree(GetProcessHeap(), 0, bytes); HeapFree(GetProcessHeap(), 0, body);
    HeapFree(GetProcessHeap(), 0, wire); HeapFree(GetProcessHeap(), 0, quoted_id);
    SysFreeString(url); SysFreeString(id);
    return TRUE;
}
