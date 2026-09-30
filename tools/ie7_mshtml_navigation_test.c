#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <cguid.h>
#include <mshtml.h>
#include <mshtmdid.h>
#include <docobj.h>
#include <ocidl.h>
#include <urlmon.h>
#include <servprov.h>
#include <shlobj.h>
#include <stdio.h>
#include <perhist.h>
#include <exdisp.h>
#include "mshtml_history_contract.h"
DEFINE_GUID(IID_IPersistHistory, 0x91a565c1, 0xe38f, 0x11d0, 0x94, 0xbf, 0x00, 0xa0, 0xc9, 0x05, 0x5c, 0xbf);
#include "mshtml_private_window.h"

#ifndef TRITON_MSHTML_DLL_PATH
#define TRITON_MSHTML_DLL_PATH L"C:\\TritonSupermiumBridge\\triton-ie7-mshtml-activation-probe.dll"
#endif

typedef HRESULT (WINAPI *GetClass)(REFCLSID, REFIID, void **);
typedef HRESULT (WINAPI *CanUnload)(void);
#define CHECK(test) do { if(!(test)) { printf("FAIL line=%d: %s\n", __LINE__, #test); return 1; } } while(0)
static LONG site_refs = 1, sink_refs = 1;
static IHTMLWindow2 *host_window;
static IDispatch *observed_document;
static IConnectionPoint *observed_point;
static DWORD observed_cookie;
static BOOL remove_in_callback;
static unsigned ready_events, bad_events, enables, disables;
static unsigned needs_notifications, complete_events;
static char browser_events[256];
static WCHAR completed_url[1024];
static unsigned browser_event_count;
static BOOL detach_on_navigate;
static IOleObject *observed_ole;
static IOleCommandTarget command_iface;
static IServiceProvider provider_iface;
static TritonDocObjectService browser_iface;
static TritonBrowserService browser_service_iface;
static TritonTravelLog travel_iface;
static BOOL record_history;
static unsigned history_updates;
static unsigned history_forced_updates;
static DWORD history_flags;
static IStream *history_exported;
static unsigned history_travel_calls;
static int history_travel_offsets[4];
static unsigned history_state_updates;
static int history_mock_pending;
static unsigned new_window_calls;
static DWORD new_window_flags;
static BOOL new_window_opener_matches;
static WCHAR new_window_url[1024], new_window_name[128];
static unsigned secure_lock_calls;
static LONG secure_lock_value = -1;
static const GUID explorer_commands = {0x000214d0,0,0,{0xc0,0,0,0,0,0,0,0x46}};
static HRESULT test_script(IHTMLWindow2 *window, LPCWSTR source);

static HRESULT STDMETHODCALLTYPE site_query(IOleClientSite *iface, REFIID iid, void **out)
{
    if(!out) return E_POINTER;
    *out = NULL;
    if(IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IOleClientSite)) *out = iface;
    else if(IsEqualIID(iid, &IID_IOleCommandTarget)) *out = &command_iface;
    else if(IsEqualIID(iid, &IID_IServiceProvider)) *out = &provider_iface;
    else if(IsEqualIID(iid, &triton_doc_object_service_iid)) *out = &browser_iface;
    else return E_NOINTERFACE;
    ++site_refs;
    return S_OK;
}
static ULONG STDMETHODCALLTYPE site_addref(IOleClientSite *iface) { (void)iface; return ++site_refs; }
static ULONG STDMETHODCALLTYPE site_release(IOleClientSite *iface) { (void)iface; return --site_refs; }
static HRESULT STDMETHODCALLTYPE site_save(IOleClientSite *iface) { (void)iface; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE site_moniker(IOleClientSite *iface, DWORD a, DWORD w, IMoniker **out)
{ (void)iface; (void)a; (void)w; *out = NULL; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE site_container(IOleClientSite *iface, IOleContainer **out)
{ (void)iface; *out = NULL; return E_NOINTERFACE; }
static HRESULT STDMETHODCALLTYPE site_show(IOleClientSite *iface) { (void)iface; return S_OK; }
static HRESULT STDMETHODCALLTYPE site_window(IOleClientSite *iface, BOOL show) { (void)iface; (void)show; return S_OK; }
static HRESULT STDMETHODCALLTYPE site_layout(IOleClientSite *iface) { (void)iface; return E_NOTIMPL; }
static IOleClientSiteVtbl site_vtbl = {site_query,site_addref,site_release,site_save,site_moniker,
    site_container,site_show,site_window,site_layout};
static IOleClientSite site_iface = {&site_vtbl};
static HRESULT STDMETHODCALLTYPE command_query(IOleCommandTarget *iface, REFIID iid, void **out)
{ (void)iface; return site_query(&site_iface, iid, out); }
static ULONG STDMETHODCALLTYPE command_addref(IOleCommandTarget *iface) { (void)iface; return ++site_refs; }
static ULONG STDMETHODCALLTYPE command_release(IOleCommandTarget *iface) { (void)iface; return --site_refs; }
static HRESULT STDMETHODCALLTYPE command_status(IOleCommandTarget *iface, const GUID *group,
    ULONG count, OLECMD *commands, OLECMDTEXT *text)
{ (void)iface; (void)group; (void)count; (void)commands; (void)text; return OLECMDERR_E_NOTSUPPORTED; }
static HRESULT STDMETHODCALLTYPE command_exec(IOleCommandTarget *iface, const GUID *group,
    DWORD command, DWORD options, VARIANT *input, VARIANT *output)
{
    (void)iface; (void)options; (void)output;
    if(group && IsEqualGUID(group, &explorer_commands) && command == 25) {
        if(!input || input->vt != VT_I4) return E_INVALIDARG;
        secure_lock_value = input->lVal;
        ++secure_lock_calls;
        return S_OK;
    }
    if(group && IsEqualGUID(group, &explorer_commands) && command == 25) {
        if(!input || input->vt != VT_I4) return E_INVALIDARG;
        ++secure_lock_calls; secure_lock_value = input->lVal;
        return S_OK;
    }
    if(record_history && group && IsEqualGUID(group, &explorer_commands) && command == 38) {
        IPersistHistory *history = NULL;
        IStream *stream = NULL;
        IBindCtx *context = NULL;
        HRESULT hr;
        if(!input || input->vt != VT_I4 || input->lVal < 0 || input->lVal > 3) return E_INVALIDARG;
        hr = IDispatch_QueryInterface(observed_document,
            (input->lVal & 1) ? &triton_persist_history2_iid : &IID_IPersistHistory, (void **)&history);
        if(SUCCEEDED(hr)) hr = CreateStreamOnHGlobal(NULL, TRUE, &stream);
        if(SUCCEEDED(hr) && (input->lVal & 1)) {
            hr = CreateBindCtx(0, &context);
            if(SUCCEEDED(hr)) hr = IBindCtx_RegisterObjectParam(context, L"Internal Navigation", (IUnknown *)observed_document);
            if(SUCCEEDED(hr)) hr = ((const TritonPersistHistory2Vtbl *)history->lpVtbl)->SaveHistoryEx(history, stream, context);
        } else if(SUCCEEDED(hr)) hr = IPersistHistory_SaveHistory(history, stream);
        if(context) IBindCtx_Release(context);
        if(history) IPersistHistory_Release(history);
        if(SUCCEEDED(hr) && (input->lVal & 2)) {
            ++history_forced_updates;
            IStream_Release(stream);
        } else if(SUCCEEDED(hr)) {
            if(history_exported) IStream_Release(history_exported);
            history_exported = stream;
            ++history_updates; history_flags = input->lVal;
        } else if(stream) IStream_Release(stream);
        return hr;
    }
    if(group && IsEqualGUID(group, &triton_doc_host_commands) && command == 13) {
        if(input || output) return E_INVALIDARG;
        ++needs_notifications;
        return S_OK;
    }
    if(group && IsEqualGUID(group, &triton_doc_host_commands) && command == 0) {
        IHTMLWindow2 *window = NULL;
        if(input) {
            if(input->vt != VT_UNKNOWN || !input->punkVal) return E_INVALIDARG;
            if(FAILED(IUnknown_QueryInterface(input->punkVal, &IID_IHTMLWindow2, (void **)&window)))
                return E_NOINTERFACE;
            ++enables;
        } else ++disables;
        if(host_window) IHTMLWindow2_Release(host_window);
        host_window = window;
        return S_OK;
    }
    return OLECMDERR_E_NOTSUPPORTED;
}
static IOleCommandTargetVtbl command_vtbl = {command_query,command_addref,command_release,command_status,command_exec};
static IOleCommandTarget command_iface = {&command_vtbl};

static HRESULT STDMETHODCALLTYPE provider_query(IServiceProvider *iface, REFIID iid, void **out)
{ (void)iface; return site_query(&site_iface, iid, out); }
static ULONG STDMETHODCALLTYPE provider_addref(IServiceProvider *iface) { (void)iface; return ++site_refs; }
static ULONG STDMETHODCALLTYPE provider_release(IServiceProvider *iface) { (void)iface; return --site_refs; }
static HRESULT STDMETHODCALLTYPE provider_service(IServiceProvider *iface, REFGUID service, REFIID iid, void **out)
{
    (void)iface;
    if(!out) return E_POINTER;
    *out = NULL;
    if(!IsEqualGUID(service, &IID_IShellBrowser) || !IsEqualIID(iid, &triton_browser_service_iid))
        return E_NOINTERFACE;
    *out = &browser_service_iface;
    ++site_refs;
    return S_OK;
}
static IServiceProviderVtbl provider_vtbl = {provider_query,provider_addref,provider_release,provider_service};
static IServiceProvider provider_iface = {&provider_vtbl};
static HRESULT STDMETHODCALLTYPE travel_query(TritonTravelLog *iface, REFIID iid, void **out)
{ (void)iface; return site_query(&site_iface, iid, out); }
static ULONG STDMETHODCALLTYPE travel_addref(TritonTravelLog *iface) { (void)iface; return ++site_refs; }
static ULONG STDMETHODCALLTYPE travel_release(TritonTravelLog *iface) { (void)iface; return --site_refs; }
static HRESULT STDMETHODCALLTYPE travel_unused(TritonTravelLog *iface, IUnknown *object, BOOL value)
{ (void)iface; (void)object; (void)value; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE travel_external(TritonTravelLog *iface, IUnknown *a, IUnknown *b)
{ (void)iface; (void)a; (void)b; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE travel_move(TritonTravelLog *iface, IUnknown *owner, int offset)
{
    WCHAR script[96];
    HRESULT hr;
    (void)iface;
    if(owner != (IUnknown *)&browser_service_iface || !offset || history_travel_calls >= ARRAYSIZE(history_travel_offsets))
        return E_INVALIDARG;
    history_travel_offsets[history_travel_calls++] = offset;
    if(history_mock_pending == offset) {
        history_mock_pending = 0;
        return S_OK;
    }
    history_mock_pending = offset;
    _snwprintf(script, ARRAYSIZE(script), L"window.__tritonHistoryOriginalGo(%d)", offset);
    script[ARRAYSIZE(script) - 1] = 0;
    hr = test_script(host_window, script);
    if(FAILED(hr)) history_mock_pending = 0;
    return hr;
}
static HRESULT STDMETHODCALLTYPE travel_get(TritonTravelLog *iface, IUnknown *owner, int offset, IUnknown **out)
{
    (void)iface; (void)owner; (void)offset;
    if(!out) return E_POINTER;
    *out = NULL;
    return E_FAIL;
}
static const TritonTravelLogVtbl travel_vtbl = {travel_query,travel_addref,travel_release,
    travel_unused,travel_unused,travel_external,travel_move,travel_get};
static TritonTravelLog travel_iface = {&travel_vtbl};
static HRESULT STDMETHODCALLTYPE browser_service_query(TritonBrowserService *iface, REFIID iid, void **out)
{
    if(!out) return E_POINTER;
    *out = NULL;
    if(IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &triton_browser_service_iid)) *out = iface;
    else if(IsEqualIID(iid, &triton_doc_object_service_iid)) *out = &browser_iface;
    else return E_NOINTERFACE;
    ++site_refs;
    return S_OK;
}
static ULONG STDMETHODCALLTYPE browser_service_addref(TritonBrowserService *iface) { (void)iface; return ++site_refs; }
static ULONG STDMETHODCALLTYPE browser_service_release(TritonBrowserService *iface) { (void)iface; return --site_refs; }
static HRESULT STDMETHODCALLTYPE browser_service_parent(TritonBrowserService *iface, void **out)
{ (void)iface; if(out) *out = NULL; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE browser_service_set_title(TritonBrowserService *iface, void *view, LPCWSTR title)
{ (void)iface; (void)view; (void)title; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE browser_service_get_title(TritonBrowserService *iface, void *view, LPWSTR title, DWORD count)
{ (void)iface; (void)view; (void)title; (void)count; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE browser_service_ole(TritonBrowserService *iface, IOleObject **out)
{ (void)iface; if(out) *out = NULL; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE browser_service_travel(TritonBrowserService *iface, TritonTravelLog **out)
{
    (void)iface;
    if(!out) return E_POINTER;
    *out = &travel_iface;
    ++site_refs;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE browser_service_update_history(TritonBrowserService *iface)
{
    (void)iface;
    ++history_state_updates;
    return S_OK;
}
static const TritonBrowserServiceVtbl browser_service_vtbl = {browser_service_query,
    browser_service_addref,browser_service_release,browser_service_parent,browser_service_set_title,
    browser_service_get_title,browser_service_ole,browser_service_travel,
    NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,browser_service_update_history};
static TritonBrowserService browser_service_iface = {&browser_service_vtbl};
static HRESULT STDMETHODCALLTYPE browser_query(TritonDocObjectService *iface, REFIID iid, void **out)
{ (void)iface; return site_query(&site_iface, iid, out); }
static ULONG STDMETHODCALLTYPE browser_addref(TritonDocObjectService *iface) { (void)iface; return ++site_refs; }
static ULONG STDMETHODCALLTYPE browser_release(TritonDocObjectService *iface) { (void)iface; return --site_refs; }
static void browser_record(char event)
{
    if(browser_event_count + 1 >= sizeof(browser_events)) { ++bad_events; return; }
    browser_events[browser_event_count++] = event;
    browser_events[browser_event_count] = 0;
}
static HRESULT browser_window_url(IHTMLWindow2 *window, DWORD flags)
{
    TritonPrivateWindow4 *legacy = NULL;
    BSTR url = NULL;
    HRESULT hr;
    if(window != host_window || flags) { ++bad_events; return E_INVALIDARG; }
    hr = IHTMLWindow2_QueryInterface(window, &triton_private_window_iid, (void **)&legacy);
    if(SUCCEEDED(hr)) {
        hr = legacy->lpVtbl->GetAddressBarUrl(legacy, &url);
        if(!url || !*url) hr = E_FAIL;
        SysFreeString(url);
        legacy->lpVtbl->Release(legacy);
    }
    if(FAILED(hr)) ++bad_events;
    return hr;
}
static HRESULT STDMETHODCALLTYPE browser_navigate(TritonDocObjectService *iface, IHTMLWindow2 *window, DWORD flags)
{
    HRESULT hr;
    (void)iface;
    browser_record('N');
    hr = browser_window_url(window, flags);
    if(detach_on_navigate) {
        detach_on_navigate = FALSE;
        if(IOleObject_SetClientSite(observed_ole, NULL) != S_OK) ++bad_events;
    }
    return hr;
}
static HRESULT STDMETHODCALLTYPE browser_begin(TritonDocObjectService *iface)
{ (void)iface; browser_record('B'); return S_OK; }
static HRESULT STDMETHODCALLTYPE browser_end(TritonDocObjectService *iface)
{ (void)iface; browser_record('C'); return S_OK; }
static HRESULT STDMETHODCALLTYPE browser_complete(TritonDocObjectService *iface, IHTMLWindow2 *window, DWORD flags)
{
    TritonPrivateWindow4 *legacy = NULL;
    BSTR url = NULL;
    (void)iface; browser_record('D'); ++complete_events;
    if(SUCCEEDED(IHTMLWindow2_QueryInterface(window, &triton_private_window_iid, (void **)&legacy))) {
        if(SUCCEEDED(legacy->lpVtbl->GetAddressBarUrl(legacy, &url)) && url)
            lstrcpynW(completed_url, url, ARRAYSIZE(completed_url));
        SysFreeString(url); legacy->lpVtbl->Release(legacy);
    }
    return browser_window_url(window, flags);
}
static HRESULT STDMETHODCALLTYPE browser_before(TritonDocObjectService *iface, IDispatch *source,
    LPCWSTR url, DWORD flags, LPCWSTR frame, BYTE *post, DWORD post_length,
    LPCWSTR headers, BOOL play_sound, BOOL *cancel)
{
    (void)iface; (void)post; (void)post_length; (void)headers; (void)play_sound;
    if(!url || !frame || !cancel) return E_INVALIDARG;
    ++new_window_calls;
    new_window_flags = flags;
    new_window_opener_matches = source == (IDispatch *)host_window;
    lstrcpynW(new_window_url, url, ARRAYSIZE(new_window_url));
    lstrcpynW(new_window_name, frame, ARRAYSIZE(new_window_name));
    *cancel = TRUE; /* The harness represents an IE host that accepted ownership. */
    return S_OK;
}
/* Unused slots stay NULL so accidentally calling one fails the test. */
static const TritonDocObjectServiceVtbl browser_vtbl = {
    .QueryInterface=browser_query, .AddRef=browser_addref, .Release=browser_release,
    .FireBeforeNavigate2=browser_before,
    .FireNavigateComplete2=browser_navigate, .FireDownloadBegin=browser_begin,
    .FireDownloadComplete=browser_end, .FireDocumentComplete=browser_complete
};
static TritonDocObjectService browser_iface = {&browser_vtbl};
static void pump_messages(void)
{
    MSG message;
    DISPPARAMS params = {0};
    VARIANT state;
    while(PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    /* A non-windowed document host has no viewport timer. Real IE queries
     * readyState while pumping; mirror that so retained CDP replies cannot
     * sit behind a quiet message queue in this alternate-host harness. */
    if(observed_document) {
        VariantInit(&state);
        IDispatch_Invoke(observed_document, DISPID_IHTMLDOCUMENT2_READYSTATE, &IID_NULL, 0,
                         DISPATCH_PROPERTYGET, &params, &state, NULL, NULL);
        VariantClear(&state);
    }
    Sleep(100);
}

static HRESULT STDMETHODCALLTYPE sink_query(IPropertyNotifySink *iface, REFIID iid, void **out)
{
    if(!out) return E_POINTER;
    *out = NULL;
    if(!IsEqualIID(iid, &IID_IUnknown) && !IsEqualIID(iid, &IID_IPropertyNotifySink)) return E_NOINTERFACE;
    *out = iface;
    ++sink_refs;
    return S_OK;
}
static ULONG STDMETHODCALLTYPE sink_addref(IPropertyNotifySink *iface) { (void)iface; return ++sink_refs; }
static ULONG STDMETHODCALLTYPE sink_release(IPropertyNotifySink *iface) { (void)iface; return --sink_refs; }
static HRESULT STDMETHODCALLTYPE sink_changed(IPropertyNotifySink *iface, DISPID id)
{
    DISPPARAMS params = {0};
    VARIANT value;
    HRESULT hr;
    (void)iface;
    VariantInit(&value);
    hr = IDispatch_Invoke(observed_document, id, &IID_NULL, 0, DISPATCH_PROPERTYGET,
                          &params, &value, NULL, NULL);
    if(id != DISPID_READYSTATE || FAILED(hr) || value.vt != VT_I4) ++bad_events;
    else ++ready_events;
    VariantClear(&value);
    if(remove_in_callback) {
        remove_in_callback = FALSE;
        if(IConnectionPoint_Unadvise(observed_point, observed_cookie) != S_OK) ++bad_events;
    }
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE sink_edit(IPropertyNotifySink *iface, DISPID id)
{ (void)iface; (void)id; return S_OK; }
static IPropertyNotifySinkVtbl sink_vtbl = {sink_query,sink_addref,sink_release,sink_changed,sink_edit};
static IPropertyNotifySink sink_iface = {&sink_vtbl};

static HRESULT test_navigate(TritonPrivateWindow4 *window, LPCWSTR address)
{
    IUri *uri = NULL;
    HRESULT hr = CreateUri(address, 0, 0, &uri);
    if(SUCCEEDED(hr)) {
        hr = window->lpVtbl->SuperNavigate2(window, uri, NULL, NULL, NULL, NULL, NULL, 0);
        IUri_Release(uri);
    }
    return hr;
}
static HRESULT test_navigate_post(TritonPrivateWindow4 *window, LPCWSTR address,
                                  const BYTE *body, ULONG length, LPCWSTR header_text)
{
    IUri *uri = NULL;
    SAFEARRAY *array = NULL;
    VARIANT post, headers;
    BYTE *destination = NULL;
    HRESULT hr;
    VariantInit(&post); VariantInit(&headers);
    array = SafeArrayCreateVector(VT_UI1, 0, length);
    if(!array) return E_OUTOFMEMORY;
    hr = SafeArrayAccessData(array, (void **)&destination);
    if(SUCCEEDED(hr)) {
        memcpy(destination, body, length);
        SafeArrayUnaccessData(array);
    }
    if(SUCCEEDED(hr)) hr = CreateUri(address, 0, 0, &uri);
    post.vt = VT_ARRAY | VT_UI1; post.parray = array;
    headers.vt = VT_BSTR; headers.bstrVal = SysAllocString(header_text);
    if(SUCCEEDED(hr) && !headers.bstrVal) hr = E_OUTOFMEMORY;
    if(SUCCEEDED(hr))
        hr = window->lpVtbl->SuperNavigate2(window, uri, NULL, NULL, NULL, &post, &headers, 0);
    SysFreeString(headers.bstrVal);
    if(uri) IUri_Release(uri);
    SafeArrayDestroy(array);
    return hr;
}
static void pump_for(DWORD milliseconds)
{
    DWORD started = GetTickCount();
    while(GetTickCount() - started < milliseconds) pump_messages();
}
static BOOL await_completion(unsigned count, LPCWSTR address)
{
    DWORD started = GetTickCount();
    while(complete_events < count && GetTickCount() - started < 15000) pump_messages();
    if(complete_events != count || lstrcmpW(completed_url, address) || bad_events) {
        printf("EVENTS expected=%u actual=%u sequence=%s url=%ls bad=%u\n",
               count, complete_events, browser_events, completed_url, bad_events);
        return FALSE;
    }
    return TRUE;
}
static BOOL await_title_timeout(IHTMLDocument2 *document, LPCWSTR expected, DWORD timeout)
{
    DWORD started = GetTickCount();
    BSTR title = NULL;
    for(;;) {
        if(SUCCEEDED(IHTMLDocument2_get_title(document, &title)) && title && !lstrcmpW(title, expected)) {
            SysFreeString(title);
            return TRUE;
        }
        if(GetTickCount() - started >= timeout) break;
        SysFreeString(title); title = NULL;
        pump_messages();
    }
    if(title)
        printf("MSHTML AWAIT TITLE ACTUAL=%ls\n", title);
    SysFreeString(title);
    return FALSE;
}
static BOOL await_title(IHTMLDocument2 *document, LPCWSTR expected)
{ return await_title_timeout(document, expected, 10000); }
static HRESULT test_script(IHTMLWindow2 *window, LPCWSTR source)
{
    BSTR code = SysAllocString(source), language = SysAllocString(L"JavaScript");
    VARIANT result;
    HRESULT hr;
    VariantInit(&result);
    hr = code && language ? IHTMLWindow2_execScript(window, code, language, &result) : E_OUTOFMEMORY;
    VariantClear(&result); SysFreeString(code); SysFreeString(language);
    return hr;
}
static int internal_page_exercise(IHTMLDocument2 *document, TritonPrivateWindow4 *window)
{
    CHECK(test_navigate(window, L"res://ieframe.dll/dnserror.htm") == S_OK);
    CHECK(await_completion(1, L"res://ieframe.dll/dnserror.htm"));
    CHECK(await_title(document, L"Internet Explorer cannot display the webpage"));
    CHECK(test_script(host_window, L"document.title=document.styleSheets.length&&Array.from(document.images).every(i=>i.complete&&i.naturalWidth>0)&&typeof initMoreInfo==='function'?'NATIVE RESOURCE ASSETS PASSED':'RESOURCE ASSET FAILURE'") == S_OK);
    CHECK(await_title(document, L"NATIVE RESOURCE ASSETS PASSED"));
    CHECK(test_navigate(window, L"res://kernel32.dll/not-an-approved-resource.htm") == E_ACCESSDENIED);
    CHECK(await_title(document, L"NATIVE RESOURCE ASSETS PASSED"));
    CHECK(test_script(host_window, L"expandCollapse('infoBlockID',true);document.title=document.getElementById('infoBlockID').style.display!=='none'?'NATIVE RESOURCE SCRIPT PASSED':'RESOURCE SCRIPT FAILURE'") == S_OK);
    CHECK(await_title(document, L"NATIVE RESOURCE SCRIPT PASSED"));
    CHECK(test_navigate(window, L"res://ieframe.dll/tabswelcome.htm") == S_OK);
    CHECK(await_completion(2, L"res://ieframe.dll/tabswelcome.htm"));
    CHECK(await_title(document, L"Welcome to Tabbed Browsing"));
    CHECK(test_navigate(window, L"https://example.org/") == S_OK);
    CHECK(await_completion(3, L"https://example.org/"));
    CHECK(await_title(document, L"Example Domain"));
    CHECK(test_script(host_window, L"document.title=!('currentStyle' in HTMLElement.prototype)?'PUBLIC DOM UNMODIFIED':'PUBLIC DOM MODIFIED'") == S_OK);
    CHECK(await_title(document, L"PUBLIC DOM UNMODIFIED"));
    puts("MSHTML INTERNAL PAGE STARTUP PASSED");
    return 0;
}
static int event_exercise(IHTMLDocument2 *document, TritonPrivateWindow4 *window)
{
    IOleCommandTarget *commands;
    DWORD started;
    BSTR state = NULL, address = NULL;
    unsigned completions;
    CHECK(IHTMLDocument2_QueryInterface(document, &IID_IOleCommandTarget, (void **)&commands) == S_OK);
    CHECK(test_navigate(window, L"http://10.0.2.2:18767/a") == S_OK);
    CHECK(await_completion(1, L"http://10.0.2.2:18767/a"));
    started = GetTickCount();
    CHECK(test_navigate(window, L"http://10.0.2.2:18767/slow-headers") == S_OK);
    printf("SLOW_NAVIGATION_RETURN_MS=%lu\n", (unsigned long)(GetTickCount() - started));
    CHECK(GetTickCount() - started < 500);
    pump_for(1000);
    CHECK(IHTMLDocument2_get_readyState(document, &state) == S_OK && !lstrcmpW(state, L"loading"));
    SysFreeString(state); state = NULL;
    CHECK(complete_events == 1);
    started = GetTickCount();
    CHECK(test_navigate(window, L"http://10.0.2.2:18767/b") == S_OK);
    CHECK(GetTickCount() - started < 500);
    CHECK(await_completion(2, L"http://10.0.2.2:18767/b"));
    pump_for(12500); /* beyond the abandoned server's response delay */
    CHECK(complete_events == 2);
    puts("MSHTML SLOW NAVIGATION SUPERSESSION PASSED");

    CHECK(test_navigate(window, L"http://10.0.2.2:18767/slow-headers") == S_OK);
    pump_for(1000);
    started = GetTickCount();
    CHECK(IOleCommandTarget_Exec(commands, NULL, OLECMDID_STOP, 0, NULL, NULL) == S_OK);
    printf("STOP_RETURN_MS=%lu\n", (unsigned long)(GetTickCount() - started));
    CHECK(GetTickCount() - started < 500);
    pump_for(1000);
    CHECK(IHTMLDocument2_get_readyState(document, &state) == S_OK && !lstrcmpW(state, L"complete"));
    SysFreeString(state); state = NULL;
    CHECK(window->lpVtbl->GetAddressBarUrl(window, &address) == S_OK);
    CHECK(!lstrcmpW(address, L"http://10.0.2.2:18767/b"));
    SysFreeString(address); address = NULL;
    pump_for(12500);
    CHECK(complete_events == 2);
    puts("MSHTML UNCOMMITTED STOP WITHOUT STALE DOCUMENT COMPLETE PASSED");

    started = GetTickCount();
    CHECK(test_navigate(window, L"http://10.0.2.2:18767/slow-headers") == S_OK);
    CHECK(test_navigate(window, L"http://10.0.2.2:18767/a") == S_OK);
    CHECK(GetTickCount() - started < 500);
    CHECK(await_completion(3, L"http://10.0.2.2:18767/a"));
    pump_for(12500); /* late response/event from the immediately superseded request */
    CHECK(complete_events == 3 && bad_events == 0);
    CHECK(window->lpVtbl->GetAddressBarUrl(window, &address) == S_OK);
    CHECK(!lstrcmpW(address, L"http://10.0.2.2:18767/a"));
    SysFreeString(address); address = NULL;
    CHECK(IHTMLDocument2_get_title(document, &state) == S_OK && !lstrcmpW(state, L"Navigation A"));
    SysFreeString(state); state = NULL;
    puts("MSHTML FAST NAVIGATION OVERTAKE PASSED");

    started = GetTickCount();
    CHECK(IOleCommandTarget_Exec(commands, NULL, OLECMDID_REFRESH, 0, NULL, NULL) == S_OK);
    CHECK(GetTickCount() - started < 500);
    CHECK(await_completion(4, L"http://10.0.2.2:18767/a"));
    CHECK(test_navigate(window, L"http://10.0.2.2:18767/links") == S_OK);
    CHECK(await_completion(5, L"http://10.0.2.2:18767/links"));
    CHECK(test_script(host_window, L"document.getElementById('fragment').click()") == S_OK);
    CHECK(await_completion(6, L"http://10.0.2.2:18767/links#section"));
    CHECK(test_script(host_window, L"document.getElementById('redirect').click()") == S_OK);
    CHECK(await_completion(7, L"http://10.0.2.2:18767/b"));
    completions = complete_events;
    pump_for(1500);
    CHECK(complete_events == completions && bad_events == 0);
    IOleCommandTarget_Release(commands);
    puts("MSHTML RENDERER LINK FRAGMENT REDIRECT AND RELOAD PASSED");
    puts("MSHTML ASYNC NAVIGATION INTEGRATION PASSED");
    return 0;
}

static int subframe_exercise(IHTMLDocument2 *document, TritonPrivateWindow4 *window)
{
    BSTR title = NULL, address = NULL;
    DWORD started;
    CHECK(test_navigate(window, L"http://10.0.2.2:18767/frames") == S_OK);
    CHECK(await_completion(1, L"http://10.0.2.2:18767/frames"));
    started = GetTickCount();
    for(;;) {
        CHECK(IHTMLDocument2_get_title(document, &title) == S_OK);
        if(!lstrcmpW(title, L"Frames 1 1")) { SysFreeString(title); title = NULL; break; }
        SysFreeString(title); title = NULL;
        CHECK(GetTickCount() - started < 10000);
        pump_messages();
    }
    CHECK(test_script(host_window,
        L"document.getElementById('same').src='/frame-b';"
        L"document.getElementById('cross').src='http://10.0.2.2:18768/frame-y'") == S_OK);
    started = GetTickCount();
    for(;;) {
        CHECK(IHTMLDocument2_get_title(document, &title) == S_OK);
        if(!lstrcmpW(title, L"Frames 2 2")) { SysFreeString(title); title = NULL; break; }
        SysFreeString(title); title = NULL;
        CHECK(GetTickCount() - started < 10000);
        pump_messages();
    }
    pump_for(1000);
    CHECK(complete_events == 1 && bad_events == 0);
    CHECK(window->lpVtbl->GetAddressBarUrl(window, &address) == S_OK);
    CHECK(!lstrcmpW(address, L"http://10.0.2.2:18767/frames"));
    SysFreeString(address);
    CHECK(history_updates == 0 && history_forced_updates == 0 && history_travel_calls == 0);
    puts("MSHTML SUBFRAME TRAVEL LOG ISOLATION PASSED");
    return 0;
}

static int post_header_exercise(IHTMLDocument2 *document, TritonPrivateWindow4 *window)
{
    static const BYTE body[] = "alpha=one+two";
    static const WCHAR headers[] = L"Content-Type: application/x-www-form-urlencoded\r\n"
                                    L"X-Triton-Request: exact-value\r\n";
    /* The baseline deliberately detaches during NavigateComplete. Drain any
     * completion from that lifetime test before counting this transaction. */
    pump_for(1000);
    browser_event_count = complete_events = 0; browser_events[0] = 0;
    CHECK(test_navigate_post(window, L"http://10.0.2.2:18767/post-echo",
                             body, sizeof(body) - 1, headers) == S_OK);
    CHECK(await_completion(1, L"http://10.0.2.2:18767/post-echo"));
    CHECK(await_title(document,
        L"POST alpha=one+two exact-value application/x-www-form-urlencoded"));

    CHECK(test_navigate_post(window, L"http://10.0.2.2:18767/post-redirect-same",
                             body, sizeof(body) - 1, headers) == S_OK);
    CHECK(await_completion(2, L"http://10.0.2.2:18767/post-echo"));
    CHECK(await_title(document,
        L"POST alpha=one+two exact-value application/x-www-form-urlencoded"));

    CHECK(test_navigate_post(window, L"http://10.0.2.2:18767/post-redirect-cross",
                             body, sizeof(body) - 1, headers) == S_OK);
    CHECK(await_completion(3, L"http://10.0.2.2:18768/cross-target"));
    CHECK(await_title(document, L"CROSS GET MISSING"));
    puts("MSHTML POST HEADER AND REDIRECT CONTRACT PASSED");
    return 0;
}

static BOOL CALLBACK visible_chrome_window(HWND window, LPARAM parameter)
{
    WCHAR class_name[80];
    unsigned *count = (unsigned *)parameter;
    if(IsWindowVisible(window) && !GetParent(window) &&
       GetClassNameW(window, class_name, ARRAYSIZE(class_name)) &&
       !lstrcmpW(class_name, L"Chrome_WidgetWin_1")) ++*count;
    return TRUE;
}

static BOOL CALLBACK popup_test_input(HWND window, LPARAM parameter)
{
    WCHAR name[80];
    if(IsWindowVisible(window) && GetClassNameW(window, name, ARRAYSIZE(name)) &&
       !lstrcmpW(name, L"Chrome_RenderWidgetHostHWND")) {
        *(HWND *)parameter = window;
        return FALSE;
    }
    return TRUE;
}
static BOOL CALLBACK popup_test_window(HWND window, LPARAM parameter)
{
    WCHAR name[80];
    if(IsWindowVisible(window) && GetClassNameW(window, name, ARRAYSIZE(name)) &&
       !lstrcmpW(name, L"Chrome_WidgetWin_1"))
        EnumChildWindows(window, popup_test_input, parameter);
    return !*(HWND *)parameter;
}
static BOOL CALLBACK popup_native_host(HWND window, LPARAM parameter)
{
    WCHAR name[80];
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if(pid == GetCurrentProcessId() && IsWindowVisible(window) &&
       GetClassNameW(window, name, ARRAYSIZE(name)) && !lstrcmpW(name, L"TritonMshtmlPopup"))
        ++*(unsigned *)parameter;
    return TRUE;
}

static int new_window_exercise(IHTMLDocument2 *document, TritonPrivateWindow4 *window)
{
    IUri *uri = NULL;
    BSTR address = NULL, frame = NULL;
    HWND input = NULL;
    unsigned native_hosts = 0;
    DWORD started;
    unsigned visible_chrome = 0, baseline_chrome = 0;
    CHECK(test_navigate(window, L"http://10.0.2.2:18767/popup-proxy") == S_OK);
    CHECK(await_completion(1, L"http://10.0.2.2:18767/popup-proxy"));
    EnumWindows(visible_chrome_window, (LPARAM)&baseline_chrome);

    new_window_calls = 0;
    EnumWindows(popup_test_window, (LPARAM)&input);
    CHECK(input);
    PostMessageW(input, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(120, 65));
    PostMessageW(input, WM_LBUTTONUP, 0, MAKELPARAM(120, 65));
    started = GetTickCount();
    while(!native_hosts && GetTickCount() - started < 5000) {
        pump_messages();
        EnumWindows(popup_native_host, (LPARAM)&native_hosts);
    }
    CHECK(native_hosts == 1);
    /* The fixture performs four bounded asynchronous waits, not one load. */
    CHECK(await_title_timeout(document, L"MSHTML POPUP PROXY PASSED", 45000));

    CHECK(CreateUri(L"http://10.0.2.2:18767/b", 0, 0, &uri) == S_OK);
    frame = SysAllocString(L"direct-name"); CHECK(frame);
    CHECK(window->lpVtbl->SuperNavigate2(window, uri, NULL, NULL, frame,
                                          NULL, NULL, 8) == S_OK);
    IUri_Release(uri); SysFreeString(frame);
    CHECK(new_window_calls == 1 && new_window_opener_matches && (new_window_flags & 8));
    CHECK(!lstrcmpW(new_window_name, L"direct-name"));
    CHECK(window->lpVtbl->GetAddressBarUrl(window, &address) == S_OK);
    CHECK(!lstrcmpW(address, L"http://10.0.2.2:18767/popup-proxy"));
    SysFreeString(address);
    pump_for(500);
    EnumWindows(visible_chrome_window, (LPARAM)&visible_chrome);
    CHECK(visible_chrome == baseline_chrome);
    puts("MSHTML SCRIPT VISIBLE CHILD WINDOW PROXY PASSED");
    puts("MSHTML HOST OWNED NEW WINDOW CONTRACT PASSED");
    return 0;
}

static BOOL wait_secure_lock(unsigned previous, LONG expected)
{
    DWORD started = GetTickCount();
    while(secure_lock_calls == previous && GetTickCount() - started < 20000) pump_messages();
    return secure_lock_calls > previous && secure_lock_value == expected;
}

static BOOL durable_launch_policy_present(void)
{
    static const WCHAR path[] = L"Software\\Microsoft\\Internet Explorer\\Low Rights\\ElevationPolicy\\{08A1D321-9C62-4FC8-84EF-7A5F8BF3C147}";
    HKEY key = NULL;
    WCHAR app[64], directory[MAX_PATH], owner[80];
    DWORD size, kind, policy = 0;
    BOOL valid = FALSE;
    if(RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_QUERY_VALUE | KEY_WOW64_32KEY, &key)) return FALSE;
    size = sizeof(app);
    if(RegQueryValueExW(key, L"AppName", NULL, &kind, (BYTE *)app, &size) || kind != REG_SZ ||
       lstrcmpiW(app, L"chrome.exe")) goto done;
    size = sizeof(directory);
    if(RegQueryValueExW(key, L"AppPath", NULL, &kind, (BYTE *)directory, &size) || kind != REG_SZ ||
       lstrcmpiW(directory, L"C:\\TritonSupermium")) goto done;
    size = sizeof(policy);
    if(RegQueryValueExW(key, L"Policy", NULL, &kind, (BYTE *)&policy, &size) ||
       kind != REG_DWORD || policy != 1) goto done;
    size = sizeof(owner);
    if(RegQueryValueExW(key, L"TritonPolicyOwner", NULL, &kind, (BYTE *)owner, &size) ||
       kind != REG_SZ || lstrcmpW(owner, L"supermium-low-integrity-v1")) goto done;
    valid = TRUE;
done:
    RegCloseKey(key);
    return valid;
}

static int security_ui_exercise(IHTMLDocument2 *document, TritonPrivateWindow4 *window)
{
    IServiceProvider *provider = NULL;
    IInternetSecurityManager *shim = NULL, *system = NULL;
    DWORD shim_zone = URLZONE_INVALID, system_zone = URLZONE_INVALID;
    unsigned previous;
    CHECK(durable_launch_policy_present());
    CHECK(IHTMLDocument2_QueryInterface(document, &IID_IServiceProvider, (void **)&provider) == S_OK);
    CHECK(IServiceProvider_QueryService(provider, &SID_SInternetSecurityManager,
          &IID_IInternetSecurityManager, (void **)&shim) == S_OK);
    CHECK(CoInternetCreateSecurityManager(NULL, &system, 0) == S_OK);
    CHECK(IInternetSecurityManager_MapUrlToZone(shim, L"http://10.0.2.2:18767/a", &shim_zone, 0) == S_OK);
    CHECK(IInternetSecurityManager_MapUrlToZone(system, L"http://10.0.2.2:18767/a", &system_zone, 0) == S_OK);
    CHECK(shim_zone == system_zone && shim_zone != (DWORD)URLZONE_INVALID);
    IInternetSecurityManager_Release(system); IInternetSecurityManager_Release(shim);
    IServiceProvider_Release(provider);
    previous = secure_lock_calls;
    CHECK(test_navigate(window, L"http://10.0.2.2:18767/a") == S_OK);
    CHECK(await_completion(1, L"http://10.0.2.2:18767/a"));
    CHECK(wait_secure_lock(previous, secureLockIconUnsecure));

    previous = secure_lock_calls;
    CHECK(test_navigate(window, L"https://example.org/") == S_OK);
    CHECK(await_completion(2, L"https://example.org/"));
    CHECK(wait_secure_lock(previous, secureLockIconSecure128Bit));

    previous = secure_lock_calls;
    CHECK(test_navigate(window, L"https://expired.badssl.com/") == S_OK);
    CHECK(wait_secure_lock(previous, secureLockIconUnsecure));

    previous = secure_lock_calls;
    CHECK(test_navigate(window, L"http://10.0.2.2:18767/download") == S_OK);
    CHECK(wait_secure_lock(previous, secureLockIconUnsecure));
    puts("MSHTML IE SECURITY UI CONTRACT PASSED");
    return 0;
}

static HRESULT history_rewind(IStream *stream)
{
    LARGE_INTEGER zero;
    zero.QuadPart = 0;
    return IStream_Seek(stream, zero, STREAM_SEEK_SET, NULL);
}

typedef struct RestartTestHeader {
    char magic[8];
    DWORD version, url_characters, state_characters, entry, position, checksum;
    GUID context;
} RestartTestHeader;
_Static_assert(sizeof(RestartTestHeader) == 48, "restart test header wire size");

static DWORD restart_hash(DWORD hash, const void *bytes, SIZE_T count)
{
    const BYTE *cursor = bytes;
    while(count--) { hash ^= *cursor++; hash *= 16777619u; }
    return hash;
}

static DWORD restart_checksum(const RestartTestHeader *header, const WCHAR *url, const WCHAR *state)
{
    DWORD hash = restart_hash(2166136261u, &header->version,
        offsetof(RestartTestHeader, checksum) - offsetof(RestartTestHeader, version));
    hash = restart_hash(hash, &header->context, sizeof(header->context));
    hash = restart_hash(hash, url, header->url_characters * sizeof(WCHAR));
    return restart_hash(hash, state, header->state_characters * sizeof(WCHAR));
}

static HRESULT restart_create_document(IClassFactory *factory, IHTMLDocument2 **document,
                                       IHTMLWindow2 **window, TritonPrivateWindow4 **private_window,
                                       IOleObject **ole)
{
    HRESULT hr;
    *document = NULL; *window = NULL; *private_window = NULL; *ole = NULL;
    hr = IClassFactory_CreateInstance(factory, NULL, &IID_IHTMLDocument2, (void **)document);
    if(SUCCEEDED(hr)) hr = IHTMLDocument2_QueryInterface(*document, &IID_IDispatch,
                                                         (void **)&observed_document);
    if(SUCCEEDED(hr)) hr = IHTMLDocument2_get_parentWindow(*document, window);
    if(SUCCEEDED(hr)) hr = IHTMLWindow2_QueryInterface(*window, &triton_private_window4_iid,
                                                       (void **)private_window);
    if(SUCCEEDED(hr)) hr = IHTMLDocument2_QueryInterface(*document, &IID_IOleObject, (void **)ole);
    if(SUCCEEDED(hr)) {
        observed_ole = *ole;
        hr = IOleObject_SetClientSite(*ole, &site_iface);
    }
    return hr;
}

static void restart_release_document(IHTMLDocument2 *document, IHTMLWindow2 *window,
                                     TritonPrivateWindow4 *private_window, IOleObject *ole)
{
    if(ole) {
        IOleObject_SetClientSite(ole, NULL);
        IOleObject_Close(ole, OLECLOSE_NOSAVE);
    }
    observed_ole = NULL;
    if(private_window) private_window->lpVtbl->Release(private_window);
    if(window) IHTMLWindow2_Release(window);
    if(observed_document) { IDispatch_Release(observed_document); observed_document = NULL; }
    if(ole) IOleObject_Release(ole);
    if(document) IHTMLDocument2_Release(document);
}

static BOOL restart_await_address(TritonPrivateWindow4 *window, LPCWSTR expected)
{
    DWORD started = GetTickCount();
    BSTR address = NULL;
    while(GetTickCount() - started < 15000) {
        if(SUCCEEDED(window->lpVtbl->GetAddressBarUrl(window, &address)) && address &&
           !lstrcmpW(address, expected)) {
            SysFreeString(address);
            return TRUE;
        }
        SysFreeString(address); address = NULL;
        pump_messages();
    }
    SysFreeString(address);
    return FALSE;
}

static int restart_history_exercise(IClassFactory *factory)
{
    IHTMLDocument2 *first = NULL, *second = NULL;
    IHTMLWindow2 *first_window = NULL, *second_window = NULL;
    TritonPrivateWindow4 *first_private = NULL, *second_private = NULL;
    IOleObject *first_ole = NULL, *second_ole = NULL;
    IPersistHistory *history = NULL, *history2 = NULL;
    IBindCtx *context = NULL;
    IStream *saved = NULL, *bad = NULL;
    STATSTG stat;
    BYTE *wire = NULL, *copy;
    RestartTestHeader *header;
    WCHAR *saved_url, *saved_state, *origin;
    ULONG transferred;
    BSTR address = NULL;

    browser_event_count = complete_events = bad_events = 0; browser_events[0] = 0;
    CHECK(restart_create_document(factory, &first, &first_window, &first_private, &first_ole) == S_OK);
    CHECK(test_navigate(first_private, L"http://10.0.2.2:18767/history-a") == S_OK);
    CHECK(await_completion(1, L"http://10.0.2.2:18767/history-a"));
    CHECK(await_title(first, L"Navigation HISTORY-A"));
    CHECK(test_script(first_window,
        L"document.querySelector('input').value='restart-state-\\u4e16\\u754c';scrollTo(0,900)") == S_OK);
    CHECK(IHTMLDocument2_QueryInterface(first, &triton_persist_history2_iid, (void **)&history2) == S_OK);
    CHECK(CreateStreamOnHGlobal(NULL, TRUE, &saved) == S_OK);
    CHECK(CreateBindCtx(0, &context) == S_OK);
    CHECK(IBindCtx_RegisterObjectParam(context, L"Triton Cross Session", (IUnknown *)first) == S_OK);
    CHECK(((const TritonPersistHistory2Vtbl *)history2->lpVtbl)->SaveHistoryEx(history2, saved, context) == S_OK);
    IBindCtx_Release(context); context = NULL;
    IPersistHistory_Release(history2); history2 = NULL;

    CHECK(IStream_Stat(saved, &stat, STATFLAG_NONAME) == S_OK);
    CHECK(stat.cbSize.QuadPart > sizeof(RestartTestHeader) && stat.cbSize.QuadPart < 1024 * 1024);
    wire = HeapAlloc(GetProcessHeap(), 0, stat.cbSize.LowPart);
    CHECK(wire);
    CHECK(history_rewind(saved) == S_OK);
    CHECK(IStream_Read(saved, wire, stat.cbSize.LowPart, &transferred) == S_OK &&
          transferred == stat.cbSize.LowPart);
    header = (RestartTestHeader *)wire;
    CHECK(!memcmp(header->magic, "TRI7HST2", 8) && header->version == 2);

    /* Release every interface on the source document. This closes its target
     * and process before the persisted bytes enter a fresh COM instance. */
    restart_release_document(first, first_window, first_private, first_ole);
    first = NULL; first_window = NULL; first_private = NULL; first_ole = NULL;
    pump_for(1000);

    CHECK(restart_create_document(factory, &second, &second_window, &second_private, &second_ole) == S_OK);
    CHECK(IHTMLDocument2_QueryInterface(second, &IID_IPersistHistory, (void **)&history) == S_OK);
    CHECK(history_rewind(saved) == S_OK);
    CHECK(IPersistHistory_LoadHistory(history, saved, NULL) == S_OK);
    CHECK(restart_await_address(second_private, L"http://10.0.2.2:18767/history-a"));
    CHECK(await_title(second, L"Navigation HISTORY-A"));
    CHECK(test_script(second_window,
        L"document.title=document.querySelector('input').value+':'+Math.round(scrollY)") == S_OK);
    CHECK(await_title(second, L"restart-state-\x4e16\x754c:900"));

    CHECK(second_private->lpVtbl->GetAddressBarUrl(second_private, &address) == S_OK && address);
    copy = HeapAlloc(GetProcessHeap(), 0, stat.cbSize.LowPart); CHECK(copy);
    memcpy(copy, wire, stat.cbSize.LowPart); copy[sizeof(RestartTestHeader)] ^= 1;
    CHECK(CreateStreamOnHGlobal(NULL, TRUE, &bad) == S_OK);
    CHECK(IStream_Write(bad, copy, stat.cbSize.LowPart, &transferred) == S_OK &&
          transferred == stat.cbSize.LowPart && history_rewind(bad) == S_OK);
    CHECK(IPersistHistory_LoadHistory(history, bad, NULL) == STG_E_INVALIDHEADER);
    IStream_Release(bad); bad = NULL; HeapFree(GetProcessHeap(), 0, copy);
    CHECK(restart_await_address(second_private, address));

    copy = HeapAlloc(GetProcessHeap(), 0, stat.cbSize.LowPart); CHECK(copy);
    memcpy(copy, wire, stat.cbSize.LowPart); header = (RestartTestHeader *)copy;
    saved_url = (WCHAR *)(copy + sizeof(*header));
    saved_state = saved_url + header->url_characters;
    origin = wcsstr(saved_state, L"10.0.2.2"); CHECK(origin); origin[7] = L'3';
    header->checksum = restart_checksum(header, saved_url, saved_state);
    CHECK(CreateStreamOnHGlobal(NULL, TRUE, &bad) == S_OK);
    CHECK(IStream_Write(bad, copy, stat.cbSize.LowPart, &transferred) == S_OK &&
          transferred == stat.cbSize.LowPart && history_rewind(bad) == S_OK);
    CHECK(IPersistHistory_LoadHistory(history, bad, NULL) == STG_E_INVALIDHEADER);
    CHECK(restart_await_address(second_private, address));

    puts("MSHTML CROSS-SESSION HISTORY RESTORE PASSED");
    SysFreeString(address);
    IStream_Release(bad); HeapFree(GetProcessHeap(), 0, copy); HeapFree(GetProcessHeap(), 0, wire);
    IStream_Release(saved); IPersistHistory_Release(history);
    restart_release_document(second, second_window, second_private, second_ole);
    return 0;
}

static int history_exercise(IHTMLDocument2 *document, TritonPrivateWindow4 *window, IClassFactory *factory)
{
    IPersistHistory *history, *foreign_history, *history2;
    IHTMLDocument2 *foreign;
    IHTMLWindow2 *foreign_window;
    IUnknown *identity, *expected_identity;
    IStream *saved, *malformed;
    IBindCtx *context;
    CLSID id;
    DWORD position;
    ULONG written;
    BYTE header[40];
    LARGE_INTEGER offset;
    STATSTG stat;
    ULARGE_INTEGER copied;
    BSTR title = NULL, blank;
    CHECK(IHTMLDocument2_QueryInterface(document, &IID_IPersistHistory, (void **)&history) == S_OK);
    CHECK(IPersistHistory_QueryInterface(history, &IID_IUnknown, (void **)&identity) == S_OK);
    CHECK(IHTMLDocument2_QueryInterface(document, &IID_IUnknown, (void **)&expected_identity) == S_OK);
    CHECK(identity == expected_identity);
    IUnknown_Release(identity); IUnknown_Release(expected_identity);
    CHECK(IPersistHistory_GetClassID(history, &id) == S_OK && IsEqualCLSID(&id, &CLSID_HTMLDocument));
    CHECK(IPersistHistory_SaveHistory(history, NULL) == E_POINTER);
    CHECK(IPersistHistory_LoadHistory(history, NULL, NULL) == E_POINTER);
    CHECK(IPersistHistory_GetPositionCookie(history, NULL) == E_POINTER);
    CHECK(IPersistHistory_QueryInterface(history, &triton_persist_history2_iid, (void **)&history2) == S_OK);
    CHECK(history2 == history);
    CHECK(((const TritonPersistHistory2Vtbl *)history2->lpVtbl)->SaveHistoryEx(history2, NULL, NULL) == E_POINTER);
    CHECK(CreateBindCtx(0, &context) == S_OK);
    CHECK(CreateStreamOnHGlobal(NULL, TRUE, &malformed) == S_OK);
    CHECK(((const TritonPersistHistory2Vtbl *)history2->lpVtbl)->SaveHistoryEx(history2, malformed, context) == E_NOTIMPL);
    IBindCtx_Release(context); IStream_Release(malformed); IPersistHistory_Release(history2);
    CHECK(SetEnvironmentVariableW(L"TRITON_MSHTML_DROP_NEXT_MAIN_FRAME_EVENT", L"1"));
    CHECK(test_navigate(window, L"http://10.0.2.2:18767/history-a") == S_OK);
    CHECK(await_completion(1, L"http://10.0.2.2:18767/history-a"));
    puts("MSHTML LOST FRAME EVENT ASYNC RECONCILIATION PASSED");
    CHECK(test_script(host_window, L"document.querySelector('input').value='history-state-\\u4e16\\u754c';scrollTo(0,900)") == S_OK);
    CHECK(IPersistHistory_GetPositionCookie(history, &position) == S_OK && position == 900);
    CHECK(IPersistHistory_SetPositionCookie(history, 42) == S_OK);
    CHECK(IPersistHistory_GetPositionCookie(history, &position) == S_OK && position == 42);
    CHECK(test_script(host_window, L"document.title=String(Math.round(scrollY))") == S_OK);
    CHECK(IHTMLDocument2_get_title(document, &title) == S_OK && !lstrcmpW(title, L"42"));
    SysFreeString(title); title = NULL;
    CHECK(IPersistHistory_SetPositionCookie(history, 900) == S_OK);
    CHECK(CreateStreamOnHGlobal(NULL, TRUE, &saved) == S_OK);
    CHECK(IPersistHistory_SaveHistory(history, saved) == S_OK);
    CHECK(test_navigate(window, L"http://10.0.2.2:18767/b") == S_OK);
    CHECK(await_completion(2, L"http://10.0.2.2:18767/b"));
    CHECK(IPersistHistory_SetPositionCookie(history, 55) == S_OK);
    CHECK(IPersistHistory_GetPositionCookie(history, &position) == S_OK && position == 0); /* Page B cannot scroll. */
    CHECK(history_rewind(saved) == S_OK);
    record_history = TRUE;
    CHECK(IPersistHistory_LoadHistory(history, saved, NULL) == S_OK);
    CHECK(await_completion(3, L"http://10.0.2.2:18767/history-a"));
    CHECK(history_forced_updates == 1 && history_updates == 0);
    record_history = FALSE;
    CHECK(IPersistHistory_GetPositionCookie(history, &position) == S_OK && position == 900);
    CHECK(test_script(host_window, L"document.title=document.querySelector('input').value+':'+Math.round(scrollY)") == S_OK);
    CHECK(IHTMLDocument2_get_title(document, &title) == S_OK);
    printf("HISTORY_RESTORED_TITLE_LENGTH=%u\n", SysStringLen(title));
    CHECK(!lstrcmpW(title, L"history-state-\x4e16\x754c:900"));
    SysFreeString(title); title = NULL;
    puts("MSHTML ENGINE HISTORY FORM AND SCROLL RESTORED");

    record_history = TRUE;
    CHECK(test_script(host_window, L"document.getElementById('fragment').click()") == S_OK);
    CHECK(await_completion(4, L"http://10.0.2.2:18767/history-a#section"));
    CHECK(history_updates == 1 && history_flags == 1 && history_exported);
    CHECK(history_rewind(history_exported) == S_OK);
    CHECK(CreateBindCtx(0, &context) == S_OK);
    CHECK(IBindCtx_RegisterObjectParam(context, L"Internal Navigation", (IUnknown *)document) == S_OK);
    CHECK(IPersistHistory_LoadHistory(history, history_exported, context) == S_OK);
    IBindCtx_Release(context);
    CHECK(await_completion(5, L"http://10.0.2.2:18767/history-a"));
    CHECK(history_updates == 1); /* A restore is not another new travel entry. */
    CHECK(history_forced_updates == 1); /* Native internal navigation already saves the departure. */
    CHECK(history_rewind(history_exported) == S_OK);
    CHECK(IPersistHistory_LoadHistory(history, history_exported, NULL) == S_OK);
    CHECK(await_completion(6, L"http://10.0.2.2:18767/history-a")); /* Already-current entry emits no CDP frame event. */
    CHECK(history_updates == 1);
    record_history = FALSE;
    IStream_Release(history_exported); history_exported = NULL;

    CHECK(test_script(host_window, L"History.prototype.forward.call(history)") == S_OK);
    CHECK(await_completion(7, L"http://10.0.2.2:18767/history-a#section"));
    CHECK(history_travel_calls == 2 && history_travel_offsets[0] == 1 &&
          history_travel_offsets[1] == 1 && history_state_updates == 2 && !history_mock_pending);
    CHECK(test_script(host_window, L"history.go(-1)") == S_OK);
    CHECK(await_completion(8, L"http://10.0.2.2:18767/history-a"));
    CHECK(history_travel_calls == 4 && history_travel_offsets[2] == -1 &&
          history_travel_offsets[3] == -1 && history_state_updates == 4 && !history_mock_pending);
    puts("MSHTML RENDERER HISTORY INSTANCE PROTOTYPE AND GO CURSOR SYNCHRONIZED");

    CHECK(CreateStreamOnHGlobal(NULL, TRUE, &malformed) == S_OK);
    CHECK(IStream_Write(malformed, "bad", 3, &written) == S_OK && written == 3);
    CHECK(history_rewind(malformed) == S_OK);
    CHECK(IPersistHistory_LoadHistory(history, malformed, NULL) == STG_E_READFAULT);
    IStream_Release(malformed);
    CHECK(history_rewind(saved) == S_OK);
    CHECK(IStream_Read(saved, header, sizeof(header), &written) == S_OK && written == sizeof(header));
    header[0] ^= 1;
    CHECK(CreateStreamOnHGlobal(NULL, TRUE, &malformed) == S_OK);
    CHECK(IStream_Write(malformed, header, sizeof(header), &written) == S_OK && written == sizeof(header));
    CHECK(history_rewind(malformed) == S_OK);
    CHECK(IPersistHistory_LoadHistory(history, malformed, NULL) == STG_E_INVALIDHEADER);
    header[0] ^= 1;
    memset(header + 12, 0xff, 4); /* Oversized UTF-16 length must fail before allocation. */
    CHECK(history_rewind(malformed) == S_OK);
    CHECK(IStream_Write(malformed, header, sizeof(header), &written) == S_OK && written == sizeof(header));
    CHECK(history_rewind(malformed) == S_OK);
    CHECK(IPersistHistory_LoadHistory(history, malformed, NULL) == STG_E_INVALIDHEADER);
    IStream_Release(malformed);
    CHECK(CreateStreamOnHGlobal(NULL, TRUE, &malformed) == S_OK);
    CHECK(history_rewind(saved) == S_OK && IStream_Stat(saved, &stat, STATFLAG_NONAME) == S_OK);
    CHECK(stat.cbSize.QuadPart > sizeof(header) && stat.cbSize.QuadPart < 65536);
    CHECK(IStream_CopyTo(saved, malformed, stat.cbSize, NULL, &copied) == S_OK && copied.QuadPart == stat.cbSize.QuadPart);
    offset.QuadPart = sizeof(header);
    CHECK(IStream_Seek(malformed, offset, STREAM_SEEK_SET, NULL) == S_OK);
    CHECK(IStream_Write(malformed, L"x", sizeof(WCHAR), &written) == S_OK && written == sizeof(WCHAR));
    CHECK(history_rewind(malformed) == S_OK);
    CHECK(IPersistHistory_LoadHistory(history, malformed, NULL) == STG_E_INVALIDHEADER); /* Entry/URL mismatch. */
    IStream_Release(malformed);
    CHECK(IClassFactory_CreateInstance(factory, NULL, &IID_IHTMLDocument2, (void **)&foreign) == S_OK);
    CHECK(IHTMLDocument2_get_parentWindow(foreign, &foreign_window) == S_OK);
    blank = SysAllocString(L"about:blank"); CHECK(blank);
    CHECK(IHTMLWindow2_navigate(foreign_window, blank) == S_OK);
    SysFreeString(blank);
    CHECK(IHTMLDocument2_QueryInterface(foreign, &IID_IPersistHistory, (void **)&foreign_history) == S_OK);
    CHECK(history_rewind(saved) == S_OK);
    CHECK(IPersistHistory_LoadHistory(foreign_history, saved, NULL) == CO_E_OBJNOTCONNECTED);
    IPersistHistory_Release(foreign_history); IHTMLWindow2_Release(foreign_window); IHTMLDocument2_Release(foreign);
    pump_for(500);
    CHECK(complete_events == 8 && history_updates == 1 && history_forced_updates == 1);
    IStream_Release(saved); IPersistHistory_Release(history);
    puts("MSHTML LIVE HISTORY PERSISTENCE AND NEGATIVE CONTROLS PASSED");
    return 0;
}

int main(int argc, char **argv)
{
    HMODULE module;
    WCHAR installed_path[MAX_PATH];
    DWORD installed_bytes = sizeof(installed_path), installed_type = 0;
    HKEY installed_key;
    GetClass get_class;
    CanUnload can_unload;
    IClassFactory *factory;
    IHTMLDocument2 *document, *returned_document;
    IOleObject *ole;
    IHTMLWindow2 *window, *same_window;
    IUnknown *doc_identity, *win_identity, *private_identity;
    TritonPrivateWindow4 *private_window;
    IConnectionPointContainer *connections;
    IConnectionPoint *wrong = NULL;
    IUri *uri;
    VARIANT value, post;
    DISPPARAMS params = {0};
    BSTR state = NULL, url = NULL;
    unsigned attempt, previous_events;
    CHECK(argc == 1 || (argc == 2 && (!strcmp(argv[1], "--events") ||
          !strcmp(argv[1], "--history") || !strcmp(argv[1], "--subframes") ||
          !strcmp(argv[1], "--post-headers") || !strcmp(argv[1], "--restart-history") ||
          !strcmp(argv[1], "--new-window") || !strcmp(argv[1], "--security-ui") ||
          !strcmp(argv[1], "--internal-pages"))));
    CHECK(CoInitialize(NULL) == S_OK);
    if(RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Classes\\CLSID\\{25336920-03F9-11CF-8FD0-00AA00686F13}\\InprocServer32",
        0, KEY_READ, &installed_key) == ERROR_SUCCESS) {
        LONG result = RegQueryValueExW(installed_key, NULL, NULL, &installed_type,
                                       (BYTE *)installed_path, &installed_bytes);
        RegCloseKey(installed_key);
        CHECK(result == ERROR_SUCCESS && installed_type == REG_SZ &&
              installed_bytes >= sizeof(WCHAR) && installed_bytes <= sizeof(installed_path) &&
              installed_path[installed_bytes / sizeof(WCHAR) - 1] == 0);
        printf("MSHTML TEST INSTALLED DLL=%ls\n", installed_path);
        module = LoadLibraryW(installed_path);
    } else module = LoadLibraryW(TRITON_MSHTML_DLL_PATH);
    CHECK(module);
    get_class = (GetClass)(void *)GetProcAddress(module, "DllGetClassObject");
    can_unload = (CanUnload)(void *)GetProcAddress(module, "DllCanUnloadNow");
    CHECK(get_class && can_unload && can_unload() == S_OK);
    CHECK(get_class(&CLSID_HTMLDocument, &IID_IClassFactory, (void **)&factory) == S_OK);
    if(argc == 2 && !strcmp(argv[1], "--restart-history")) {
        CHECK(restart_history_exercise(factory) == 0);
        IClassFactory_Release(factory);
        CHECK(can_unload() == S_OK);
        CoUninitialize(); FreeLibrary(module);
        puts("MSHTML NAVIGATION CONTRACT TEST PASSED");
        return 0;
    }
    CHECK(IClassFactory_CreateInstance(factory, NULL, &IID_IHTMLDocument2, (void **)&document) == S_OK);
    CHECK(IHTMLDocument2_QueryInterface(document, &IID_IDispatch, (void **)&observed_document) == S_OK);
    VariantInit(&value);
    CHECK(IDispatch_Invoke(observed_document, DISPID_READYSTATE, &IID_NULL, 0,
        DISPATCH_PROPERTYGET, &params, &value, NULL, NULL) == S_OK);
    CHECK(value.vt == VT_I4 && value.lVal == READYSTATE_UNINITIALIZED);
    VariantClear(&value);
    CHECK(IDispatch_Invoke(observed_document, DISPID_IHTMLDOCUMENT2_READYSTATE, &IID_NULL, 0,
        DISPATCH_PROPERTYGET, &params, &value, NULL, NULL) == S_OK);
    CHECK(value.vt == VT_BSTR && !lstrcmpW(value.bstrVal, L"uninitialized"));
    VariantClear(&value);
    CHECK(IHTMLDocument2_get_parentWindow(document, &window) == S_OK);
    CHECK(IHTMLDocument2_get_parentWindow(document, &same_window) == S_OK && same_window == window);
    IHTMLWindow2_Release(same_window);
    CHECK(IHTMLWindow2_get_document(window, &returned_document) == S_OK && returned_document == document);
    IHTMLDocument2_Release(returned_document);
    CHECK(IHTMLDocument2_QueryInterface(document, &IID_IUnknown, (void **)&doc_identity) == S_OK);
    CHECK(IHTMLWindow2_QueryInterface(window, &IID_IUnknown, (void **)&win_identity) == S_OK);
    CHECK(doc_identity != win_identity);
    CHECK(IHTMLWindow2_QueryInterface(window, &triton_private_window4_iid, (void **)&private_window) == S_OK);
    CHECK(private_window->lpVtbl->QueryInterface(private_window, &IID_IUnknown, (void **)&private_identity) == S_OK);
    CHECK(private_identity == win_identity);
    IUnknown_Release(private_identity); IUnknown_Release(win_identity); IUnknown_Release(doc_identity);
    CHECK(IHTMLDocument2_QueryInterface(document, &IID_IOleObject, (void **)&ole) == S_OK);
    observed_ole = ole;
    CHECK(IOleObject_SetClientSite(ole, &site_iface) == S_OK);
    CHECK(enables == 1 && needs_notifications == 1 && host_window == window);
    CHECK(IHTMLDocument2_QueryInterface(document, &IID_IConnectionPointContainer, (void **)&connections) == S_OK);
    CHECK(IConnectionPointContainer_FindConnectionPoint(connections, &IID_IUnknown, &wrong) == CONNECT_E_NOCONNECTION);
    CHECK(!wrong);
    CHECK(IConnectionPointContainer_FindConnectionPoint(connections, &IID_IPropertyNotifySink, &observed_point) == S_OK);
    CHECK(IConnectionPoint_Advise(observed_point, (IUnknown *)&site_iface, &observed_cookie) == CONNECT_E_CANNOTCONNECT);
    CHECK(observed_cookie == 0);
    CHECK(IConnectionPoint_Advise(observed_point, (IUnknown *)&sink_iface, &observed_cookie) == S_OK);
    CHECK(sink_refs == 2 && observed_cookie != 0);
    remove_in_callback = TRUE;
    CHECK(CreateUri(L"about:blank#first-native-contract", 0, 0, &uri) == S_OK);
    CHECK(private_window->lpVtbl->SuperNavigate2(private_window, uri, NULL, NULL, NULL, NULL, NULL, 0) == S_OK);
    IUri_Release(uri);
    CHECK(!remove_in_callback && ready_events == 1 && bad_events == 0 && sink_refs == 1);
    CHECK(IConnectionPoint_Unadvise(observed_point, observed_cookie) == CONNECT_E_NOCONNECTION);
    CHECK(IConnectionPoint_Advise(observed_point, (IUnknown *)&sink_iface, &observed_cookie) == S_OK);
    for(attempt = 0; attempt < 50; ++attempt) {
        CHECK(IHTMLDocument2_get_readyState(document, &state) == S_OK);
        if(!lstrcmpW(state, L"complete")) { SysFreeString(state); break; }
        SysFreeString(state);
        Sleep(100);
    }
    CHECK(attempt < 50 && ready_events >= 2 && bad_events == 0);
    for(attempt = 0; attempt < 150 && complete_events < 1; ++attempt) pump_messages();
    if(complete_events != 1 || strcmp(browser_events, "BNCD") || bad_events)
        printf("BASELINE_EVENTS actual=%u sequence=%s bad=%u\n", complete_events, browser_events, bad_events);
    CHECK(complete_events == 1 && !strcmp(browser_events, "BNCD") && bad_events == 0);
    CHECK(CreateUri(L"about:blank#second-native-contract", 0, 0, &uri) == S_OK);
    previous_events = ready_events;
    CHECK(private_window->lpVtbl->SuperNavigate2(private_window, uri, NULL, NULL, NULL, NULL, NULL, 0) == S_OK);
    CHECK(ready_events > previous_events && bad_events == 0);
    CHECK(IHTMLDocument2_get_parentWindow(document, &same_window) == S_OK && same_window == window);
    IHTMLWindow2_Release(same_window);
    CHECK(private_window->lpVtbl->GetAddressBarUrl(private_window, &url) == S_OK);
    CHECK(!lstrcmpW(url, L"about:blank#second-native-contract")); SysFreeString(url);
    VariantInit(&post); post.vt = VT_I4; post.lVal = 1;
    CHECK(private_window->lpVtbl->SuperNavigate2(private_window, uri, NULL, NULL, NULL, &post, NULL, 0) == E_NOTIMPL);
    CHECK(private_window->lpVtbl->SuperNavigate2(private_window, uri, NULL, NULL, NULL, NULL, NULL, 8) == S_OK);
    IUri_Release(uri);
    for(attempt = 0; attempt < 50 && complete_events < 2; ++attempt) pump_messages();
    CHECK(complete_events == 2 && !strcmp(browser_events, "BNCDBNCD") && bad_events == 0);
    for(attempt = 0; attempt < 15; ++attempt) pump_messages();
    CHECK(!strcmp(browser_events, "BNCDBNCD")); /* no duplicate completions */
    detach_on_navigate = TRUE;
    CHECK(CreateUri(L"about:blank#reentrant-detach", 0, 0, &uri) == S_OK);
    CHECK(private_window->lpVtbl->SuperNavigate2(private_window, uri, NULL, NULL, NULL, NULL, NULL, 0) == S_OK);
    IUri_Release(uri);
    for(attempt = 0; attempt < 50 && detach_on_navigate; ++attempt) pump_messages();
    CHECK(!detach_on_navigate && !host_window && disables == 1 && bad_events == 0);
    CHECK(!strcmp(browser_events, "BNCDBNCDBN") && complete_events == 2);
    CHECK(IConnectionPoint_Unadvise(observed_point, observed_cookie) == S_OK && sink_refs == 1);
    IConnectionPoint_Release(observed_point);
    IConnectionPointContainer_Release(connections);
    CHECK(IOleObject_SetClientSite(ole, NULL) == S_OK && !host_window && disables == 1);
    CHECK(site_refs == 1);
    if(argc == 2) {
        browser_event_count = complete_events = 0; browser_events[0] = 0;
        CHECK(IOleObject_SetClientSite(ole, &site_iface) == S_OK);
        if(!strcmp(argv[1], "--history")) CHECK(history_exercise(document, private_window, factory) == 0);
        else if(!strcmp(argv[1], "--subframes")) CHECK(subframe_exercise(document, private_window) == 0);
        else if(!strcmp(argv[1], "--post-headers")) CHECK(post_header_exercise(document, private_window) == 0);
        else if(!strcmp(argv[1], "--new-window")) CHECK(new_window_exercise(document, private_window) == 0);
        else if(!strcmp(argv[1], "--security-ui")) CHECK(security_ui_exercise(document, private_window) == 0);
        else if(!strcmp(argv[1], "--internal-pages")) CHECK(internal_page_exercise(document, private_window) == 0);
        else CHECK(event_exercise(document, private_window) == 0);
        CHECK(IOleObject_SetClientSite(ole, NULL) == S_OK && site_refs == 1);
    }
    CHECK(IOleObject_Close(ole, OLECLOSE_NOSAVE) == S_OK);
    IOleObject_Release(ole);
    private_window->lpVtbl->Release(private_window);
    IHTMLWindow2_Release(window);
    IDispatch_Release(observed_document);
    IHTMLDocument2_Release(document);
    IClassFactory_Release(factory);
    CHECK(can_unload() == S_OK);
    CoUninitialize(); FreeLibrary(module);
#ifdef TRITON_X64_HOST_CONTRACT
    puts("MSHTML X64 AND ALTERNATE HOST CONTRACT PASSED");
#endif
    puts("MSHTML NAVIGATION CONTRACT TEST PASSED");
    return 0;
}
