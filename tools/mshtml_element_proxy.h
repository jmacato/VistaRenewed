/* Stable COM wrappers around Chromium nodes. The document's list is weak;
 * live wrappers retain both their remote node and their owning COM document. */
#include <errno.h>
#include <stdlib.h>

struct ElementProxy {
    IHTMLElement IHTMLElement_iface;
    LONG references;
    ProbeDocument *document;
    ElementProxy *next;
    BSTR object_id;
    unsigned backend_id, generation;
    char session[80];
};

static IHTMLElementVtbl element_vtbl;
static ElementProxy *element_from_iface(IHTMLElement *iface) { return (ElementProxy *)iface; }
static ProbeDocument *document_from_html3(IHTMLDocument3 *iface)
{ return (ProbeDocument *)((char *)iface - offsetof(ProbeDocument, IHTMLDocument3_iface)); }

static BOOL element_connected(ElementProxy *element)
{
    return element->generation == element->document->page_generation &&
           element->document->page_target[0] &&
           !strcmp(element->session, element->document->page_session);
}

static ULONG element_add_ref(ElementProxy *element)
{ return (ULONG)InterlockedIncrement(&element->references); }

static ULONG element_release(ElementProxy *element)
{
    LONG refs = InterlockedDecrement(&element->references);
    if(!refs) {
        ProbeDocument *document = element->document;
        ElementProxy **link;
        EnterCriticalSection(&document->runtime->transport_lock);
        for(link = &document->elements; *link && *link != element; link = &(*link)->next) {}
        if(*link) *link = element->next;
        if(element_connected(element)) content_release_object(document, element->object_id);
        LeaveCriticalSection(&document->runtime->transport_lock);
        SysFreeString(element->object_id);
        HeapFree(GetProcessHeap(), 0, element);
        document_release(document);
    }
    return (ULONG)refs;
}

static HRESULT element_query(ElementProxy *element, REFIID iid, void **out)
{
    if(!out) return E_POINTER;
    *out = NULL;
    if(!IsEqualIID(iid, &IID_IUnknown) && !IsEqualIID(iid, &IID_IDispatch) &&
       !IsEqualIID(iid, &IID_IHTMLElement)) return E_NOINTERFACE;
    *out = &element->IHTMLElement_iface;
    element_add_ref(element);
    return S_OK;
}

static BOOL element_uint(const char *json, unsigned *value)
{
    char *end;
    long n;
    if(!json || *json < '0' || *json > '9') return FALSE;
    errno = 0;
    n = strtol(json, &end, 10);
    if(errno || end != mj_skip(json, 0) || n <= 0) return FALSE;
    *value = (unsigned)n;
    return TRUE;
}

static HRESULT element_call_raw(ElementProxy *element, LPCWSTR function,
                                const char *arguments, BOOL by_value, char **reply)
{
    HRESULT hr;
    const char *result;
    *reply = NULL;
    document_process_events(element->document);
    if(!element_connected(element)) return CO_E_OBJNOTCONNECTED;
    hr = content_call_object(element->document, element->object_id, function, arguments, by_value, reply);
    if(SUCCEEDED(hr)) hr = content_remote_result(*reply, &result);
    if(FAILED(hr)) { HeapFree(GetProcessHeap(), 0, *reply); *reply = NULL; }
    return hr;
}

/* A backend id is only a lookup hint, not permanent identity. Confirm actual
 * JavaScript object equality before reusing a wrapper: renderer restarts and
 * new contexts may recycle numeric backend ids. */
static HRESULT element_from_remote(ProbeDocument *document, const char *reply, IHTMLElement **out)
{
    const char *result, *node, *type;
    char *description = NULL, params[2048], *quoted = NULL;
    BSTR object_id = NULL;
    ElementProxy *element;
    unsigned backend_id, node_type;
    HRESULT hr = content_remote_result(reply, &result);
    *out = NULL;
    if(FAILED(hr)) return hr;
    type = mj_member(result, "subtype");
    if(type && !strncmp(type, "\"null\"", 6)) return S_OK;
    hr = mj_bstr(mj_member(result, "objectId"), &object_id);
    if(FAILED(hr)) return hr;
    quoted = mj_quote(object_id);
    if(!quoted) { hr = E_OUTOFMEMORY; goto done; }
    if(strlen(quoted) > 1024) { hr = E_FAIL; goto done; }
    /* describeNode includes attributes even at depth zero; a valid element
     * with a large class/style/data attribute must not hit a 4 KiB ceiling. */
    description = HeapAlloc(GetProcessHeap(), 0, CONTENT_REPLY_BYTES);
    if(!description) { hr = E_OUTOFMEMORY; goto done; }
    snprintf(params, sizeof(params), "{\"objectId\":%s,\"depth\":0}", quoted);
    hr = document_pipe_call(document, "DOM.describeNode", params, document->page_session,
                            description, CONTENT_REPLY_BYTES);
    if(FAILED(hr)) goto done;
    node = mj_member(mj_member(description, "result"), "node");
    if(!element_uint(mj_member(node, "backendNodeId"), &backend_id) ||
       !element_uint(mj_member(node, "nodeType"), &node_type) || node_type != 1) {
        hr = E_NOINTERFACE; goto done;
    }
    snprintf(params, sizeof(params), "[{\"objectId\":%s}]", quoted);
    for(element = document->elements; element; element = element->next) {
        char *comparison = NULL;
        const char *value;
        if(element->backend_id != backend_id || !element_connected(element)) continue;
        hr = element_call_raw(element, L"function(other){return this===other;}", params, TRUE, &comparison);
        value = SUCCEEDED(hr) ? mj_member(mj_member(mj_member(comparison, "result"), "result"), "value") : NULL;
        if(value && !strncmp(value, "true", 4)) {
            HeapFree(GetProcessHeap(), 0, comparison);
            element_add_ref(element);
            *out = &element->IHTMLElement_iface;
            hr = S_OK; goto done;
        }
        HeapFree(GetProcessHeap(), 0, comparison);
    }
    element = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*element));
    if(!element) { hr = E_OUTOFMEMORY; goto done; }
    element->IHTMLElement_iface.lpVtbl = &element_vtbl;
    element->references = 1;
    element->document = document;
    element->object_id = object_id; object_id = NULL;
    element->backend_id = backend_id;
    element->generation = document->page_generation;
    strcpy(element->session, document->page_session);
    element->next = document->elements;
    document->elements = element;
    document_add_ref(document);
    *out = &element->IHTMLElement_iface;
    hr = S_OK;
done:
    if(object_id) content_release_object(document, object_id);
    SysFreeString(object_id);
    HeapFree(GetProcessHeap(), 0, quoted);
    HeapFree(GetProcessHeap(), 0, description);
    return hr;
}

static HRESULT document_element(ProbeDocument *document, LPCWSTR function, BSTR argument,
                                BOOL has_argument, IHTMLElement **out)
{
    BSTR object_id = NULL;
    char *args = NULL, *reply = NULL;
    HRESULT hr;
    if(!out) return E_POINTER;
    *out = NULL;
    if(has_argument && SysStringLen(argument) > CONTENT_MAX_CHARS) return E_INVALIDARG;
    EnterCriticalSection(&document->runtime->transport_lock);
    hr = content_acquire_document(document, &object_id);
    if(FAILED(hr)) goto done;
    if(has_argument && !(args = content_argument(argument))) { hr = E_OUTOFMEMORY; goto done; }
    hr = content_call_object(document, object_id, function, has_argument ? args : "[]", FALSE, &reply);
    if(SUCCEEDED(hr)) hr = element_from_remote(document, reply, out);
done:
    if(object_id) content_release_object(document, object_id);
    SysFreeString(object_id);
    HeapFree(GetProcessHeap(), 0, args); HeapFree(GetProcessHeap(), 0, reply);
    LeaveCriticalSection(&document->runtime->transport_lock);
    return hr;
}

static HRESULT element_get_property(ElementProxy *element, LPCWSTR property, BSTR *out)
{
    WCHAR function[128];
    char *reply = NULL;
    const char *result;
    HRESULT hr;
    if(!out) return E_POINTER;
    *out = NULL;
    wsprintfW(function, L"function(){return this.%ls;}", property);
    hr = element_call_raw(element, function, "[]", TRUE, &reply);
    if(SUCCEEDED(hr)) hr = content_remote_result(reply, &result);
    if(SUCCEEDED(hr)) hr = mj_bstr(mj_member(result, "value"), out);
    if(SUCCEEDED(hr) && SysStringLen(*out) > CONTENT_MAX_CHARS) {
        SysFreeString(*out); *out = NULL; hr = HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
    }
    HeapFree(GetProcessHeap(), 0, reply);
    return hr;
}

static HRESULT element_put_property(ElementProxy *element, LPCWSTR property, BSTR value)
{
    WCHAR function[128];
    char *reply = NULL, *args;
    HRESULT hr;
    if(SysStringLen(value) > CONTENT_MAX_CHARS) return E_INVALIDARG;
    args = content_argument(value);
    if(!args) return E_OUTOFMEMORY;
    wsprintfW(function, L"function(s){this.%ls=s;}", property);
    hr = element_call_raw(element, function, args, TRUE, &reply);
    HeapFree(GetProcessHeap(), 0, args); HeapFree(GetProcessHeap(), 0, reply);
    return hr;
}

static HRESULT element_parent(ElementProxy *element, IHTMLElement **out)
{
    char *reply = NULL;
    HRESULT hr;
    if(!out) return E_POINTER;
    *out = NULL;
    EnterCriticalSection(&element->document->runtime->transport_lock);
    hr = element_call_raw(element, L"function(){return this.parentElement;}", "[]", FALSE, &reply);
    if(SUCCEEDED(hr)) hr = element_from_remote(element->document, reply, out);
    HeapFree(GetProcessHeap(), 0, reply);
    LeaveCriticalSection(&element->document->runtime->transport_lock);
    return hr;
}

static HRESULT element_document(ElementProxy *element, IDispatch **out)
{
    char *reply = NULL;
    HRESULT hr;
    if(!out) return E_POINTER;
    *out = NULL;
    hr = element_call_raw(element, L"function(){return true;}", "[]", TRUE, &reply);
    HeapFree(GetProcessHeap(), 0, reply);
    if(SUCCEEDED(hr)) hr = document_query_interface(element->document, &IID_IDispatch, (void **)out);
    return hr;
}

typedef struct ElementProperty { LPCWSTR name; DISPID id; BOOL writable; } ElementProperty;
static const ElementProperty element_properties[] = {
    {L"innerHTML", DISPID_IHTMLELEMENT_INNERHTML, TRUE},
    {L"outerHTML", DISPID_IHTMLELEMENT_OUTERHTML, TRUE},
    {L"innerText", DISPID_IHTMLELEMENT_INNERTEXT, TRUE},
    {L"outerText", DISPID_IHTMLELEMENT_OUTERTEXT, TRUE},
    {L"id", DISPID_IHTMLELEMENT_ID, TRUE},
    {L"className", DISPID_IHTMLELEMENT_CLASSNAME, TRUE},
    {L"tagName", DISPID_IHTMLELEMENT_TAGNAME, FALSE},
    {L"title", DISPID_IHTMLELEMENT_TITLE, TRUE},
    {L"parentElement", DISPID_IHTMLELEMENT_PARENTELEMENT, FALSE},
    {L"document", DISPID_IHTMLELEMENT_DOCUMENT, FALSE},
};

static HRESULT element_names(REFIID iid, LPOLESTR *names, UINT count, DISPID *ids)
{
    UINT i, j;
    HRESULT hr = S_OK;
    if(!IsEqualIID(iid, &IID_NULL)) return DISP_E_UNKNOWNINTERFACE;
    if(!names || !ids) return E_POINTER;
    for(i = 0; i < count; ++i) {
        ids[i] = DISPID_UNKNOWN;
        for(j = 0; j < ARRAYSIZE(element_properties); ++j)
            if(names[i] && !lstrcmpiW(names[i], element_properties[j].name)) {
                ids[i] = element_properties[j].id; break;
            }
        if(ids[i] == DISPID_UNKNOWN) hr = DISP_E_UNKNOWNNAME;
    }
    return hr;
}

static HRESULT element_invoke(ElementProxy *element, DISPID id, REFIID iid, WORD flags,
                               DISPPARAMS *params, VARIANT *result, UINT *bad)
{
    UINT i;
    HRESULT hr;
    const ElementProperty *property = NULL;
    if(!IsEqualIID(iid, &IID_NULL)) return DISP_E_UNKNOWNINTERFACE;
    for(i = 0; i < ARRAYSIZE(element_properties); ++i)
        if(element_properties[i].id == id) { property = &element_properties[i]; break; }
    if(!property) return DISP_E_MEMBERNOTFOUND;
    if(flags == DISPATCH_PROPERTYGET) {
        if(params && params->cArgs) return DISP_E_BADPARAMCOUNT;
        if(!result) return E_POINTER;
        VariantInit(result);
        if(id == (DISPID)DISPID_IHTMLELEMENT_DOCUMENT) {
            hr = element_document(element, &result->pdispVal);
            if(SUCCEEDED(hr)) result->vt = VT_DISPATCH;
        } else if(id == (DISPID)DISPID_IHTMLELEMENT_PARENTELEMENT) {
            hr = element_parent(element, (IHTMLElement **)&result->pdispVal);
            if(SUCCEEDED(hr)) result->vt = VT_DISPATCH;
        } else {
            hr = element_get_property(element, property->name, &result->bstrVal);
            if(SUCCEEDED(hr)) result->vt = VT_BSTR;
        }
        return hr;
    }
    if(flags != DISPATCH_PROPERTYPUT || !property->writable) return DISP_E_MEMBERNOTFOUND;
    if(!params || params->cArgs != 1) return DISP_E_BADPARAMCOUNT;
    if(!params->rgvarg || params->cNamedArgs != 1 || !params->rgdispidNamedArgs ||
       params->rgdispidNamedArgs[0] != DISPID_PROPERTYPUT) return DISP_E_PARAMNOTFOUND;
    if(params->rgvarg[0].vt != VT_BSTR) {
        if(bad) *bad = 0;
        return DISP_E_TYPEMISMATCH;
    }
    return element_put_property(element, property->name, params->rgvarg[0].bstrVal);
}

#include "../build/ie7_element_stubs.inc"
