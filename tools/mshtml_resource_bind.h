/* Observe the original protocol's MIME notification while URLMon binds bytes. */
typedef struct NativeResourceBind {
    IBindStatusCallback iface;
    LONG refs;
    WCHAR mime[128];
} NativeResourceBind;
static ULONG STDMETHODCALLTYPE nr_addref(IBindStatusCallback *iface)
{ return InterlockedIncrement(&((NativeResourceBind *)iface)->refs); }
static ULONG STDMETHODCALLTYPE nr_release(IBindStatusCallback *iface)
{
    LONG refs = InterlockedDecrement(&((NativeResourceBind *)iface)->refs);
    if(!refs) HeapFree(GetProcessHeap(), 0, iface);
    return refs;
}
static HRESULT STDMETHODCALLTYPE nr_query(IBindStatusCallback *iface, REFIID iid, void **out)
{
    if(!out) return E_POINTER;
    *out = NULL;
    if(!IsEqualIID(iid, &IID_IUnknown) && !IsEqualIID(iid, &IID_IBindStatusCallback)) return E_NOINTERFACE;
    *out = iface; nr_addref(iface); return S_OK;
}
static HRESULT STDMETHODCALLTYPE nr_start(IBindStatusCallback *iface, DWORD reserved, IBinding *binding)
{ (void)iface; (void)reserved; (void)binding; return S_OK; }
static HRESULT STDMETHODCALLTYPE nr_priority(IBindStatusCallback *iface, LONG *priority)
{ (void)iface; if(priority) *priority = 0; return S_OK; }
static HRESULT STDMETHODCALLTYPE nr_low(IBindStatusCallback *iface, DWORD reserved)
{ (void)iface; (void)reserved; return S_OK; }
static HRESULT STDMETHODCALLTYPE nr_progress(IBindStatusCallback *iface, ULONG progress, ULONG maximum,
                                             ULONG status, LPCWSTR text)
{
    NativeResourceBind *bind = (NativeResourceBind *)iface;
    (void)progress; (void)maximum;
    if(text && (status == BINDSTATUS_MIMETYPEAVAILABLE || status == BINDSTATUS_VERIFIEDMIMETYPEAVAILABLE))
        lstrcpynW(bind->mime, text, ARRAYSIZE(bind->mime));
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE nr_stop(IBindStatusCallback *iface, HRESULT hr, LPCWSTR text)
{ (void)iface; (void)hr; (void)text; return S_OK; }
static HRESULT STDMETHODCALLTYPE nr_info(IBindStatusCallback *iface, DWORD *flags, BINDINFO *info)
{ (void)iface; (void)info; if(!flags) return E_POINTER; *flags = 0; return S_OK; }
static HRESULT STDMETHODCALLTYPE nr_data(IBindStatusCallback *iface, DWORD flags, DWORD size,
                                         FORMATETC *format, STGMEDIUM *medium)
{ (void)iface; (void)flags; (void)size; (void)format; (void)medium; return S_OK; }
static HRESULT STDMETHODCALLTYPE nr_object(IBindStatusCallback *iface, REFIID iid, IUnknown *object)
{ (void)iface; (void)iid; (void)object; return E_NOTIMPL; }
static IBindStatusCallbackVtbl nr_vtbl = {nr_query, nr_addref, nr_release, nr_start,
    nr_priority, nr_low, nr_progress, nr_stop, nr_info, nr_data, nr_object};
static HRESULT native_resource_open(LPCWSTR url, IStream **stream, WCHAR *mime, unsigned capacity)
{
    NativeResourceBind *bind = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*bind));
    HRESULT hr;
    *stream = NULL; if(capacity) mime[0] = 0;
    if(!bind) return E_OUTOFMEMORY;
    bind->iface.lpVtbl = &nr_vtbl; bind->refs = 1;
    hr = URLOpenBlockingStreamW(NULL, url, stream, 0, &bind->iface);
    if(capacity) lstrcpynW(mime, bind->mime, capacity);
    nr_release(&bind->iface);
    return hr;
}
