/* Vista 7.0.6002.18005: recovered from CWindow::s_apfnIHTMLPrivateWindow4.
 * See docs/MSHTML_NATIVE_RE.md. This is IUnknown-based, not IDispatch.
 * Keep every slot/signature, including unsupported legacy PICS operations. */
#ifndef TRITON_MSHTML_PRIVATE_WINDOW_H
#define TRITON_MSHTML_PRIVATE_WINDOW_H
static const IID triton_private_window4_iid =
    {0x3050f594,0x98b5,0x11cf,{0xbb,0x82,0x00,0xaa,0x00,0xbd,0xce,0x0b}};
/* CWindow::PrivateQueryInterface, 7729a017..7729a03a, uses the same
 * s_apfnIHTMLPrivateWindow4 table for this older nine-slot interface. */
static const IID triton_private_window_iid =
    {0x3050f6dc,0x98b5,0x11cf,{0xbb,0x82,0x00,0xaa,0x00,0xbd,0xce,0x0b}};
static const GUID triton_doc_host_commands =
    {0x000214d4,0,0,{0xc0,0,0,0,0,0,0,0x46}};
static const IID triton_browser_service_iid =
    {0x02ba3b52,0x0547,0x11d1,{0xb8,0x33,0x00,0xc0,0x4f,0xc9,0xb3,0x1f}};
static const IID triton_doc_object_service_iid =
    {0x3050f801,0x98b5,0x11cf,{0xbb,0x82,0x00,0xaa,0x00,0xbd,0xce,0x0b}};
typedef struct TritonPrivateWindow4 TritonPrivateWindow4;
typedef struct TritonPrivateWindow4Vtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(TritonPrivateWindow4 *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(TritonPrivateWindow4 *);
    ULONG (STDMETHODCALLTYPE *Release)(TritonPrivateWindow4 *);
    HRESULT (STDMETHODCALLTYPE *SuperNavigate)(TritonPrivateWindow4 *, BSTR, BSTR, BSTR, BSTR, VARIANT *, VARIANT *, DWORD);
    HRESULT (STDMETHODCALLTYPE *GetPendingUrl)(TritonPrivateWindow4 *, BSTR *);
    HRESULT (STDMETHODCALLTYPE *SetPICSTarget)(TritonPrivateWindow4 *, IOleCommandTarget *);
    HRESULT (STDMETHODCALLTYPE *PICSComplete)(TritonPrivateWindow4 *, BOOL);
    HRESULT (STDMETHODCALLTYPE *FindWindowByName)(TritonPrivateWindow4 *, LPCWSTR, IHTMLWindow2 **);
    HRESULT (STDMETHODCALLTYPE *GetAddressBarUrl)(TritonPrivateWindow4 *, BSTR *);
    HRESULT (STDMETHODCALLTYPE *SuperNavigate2)(TritonPrivateWindow4 *, IUri *, BSTR, BSTR, BSTR, VARIANT *, VARIANT *, DWORD);
    HRESULT (STDMETHODCALLTYPE *SuperNavigate3)(TritonPrivateWindow4 *, IUri *, BSTR, BSTR, BSTR, IStream *, DWORD);
} TritonPrivateWindow4Vtbl;
struct TritonPrivateWindow4 { const TritonPrivateWindow4Vtbl *lpVtbl; };

/* Actual IEFRAME CBaseBrowser2 IDocObjectService table, VA 75cdac74.
 * These are host callbacks, not methods implemented by the document. */
typedef struct TritonDocObjectService TritonDocObjectService;
typedef struct TritonDocObjectServiceVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(TritonDocObjectService *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(TritonDocObjectService *);
    ULONG (STDMETHODCALLTYPE *Release)(TritonDocObjectService *);
    HRESULT (STDMETHODCALLTYPE *FireBeforeNavigate2)(TritonDocObjectService *, IDispatch *, LPCWSTR, DWORD, LPCWSTR, BYTE *, DWORD, LPCWSTR, BOOL, BOOL *);
    HRESULT (STDMETHODCALLTYPE *FireNavigateComplete2)(TritonDocObjectService *, IHTMLWindow2 *, DWORD);
    HRESULT (STDMETHODCALLTYPE *FireDownloadBegin)(TritonDocObjectService *);
    HRESULT (STDMETHODCALLTYPE *FireDownloadComplete)(TritonDocObjectService *);
    HRESULT (STDMETHODCALLTYPE *FireDocumentComplete)(TritonDocObjectService *, IHTMLWindow2 *, DWORD);
    HRESULT (STDMETHODCALLTYPE *UpdateDesktopComponent)(TritonDocObjectService *, IHTMLWindow2 *);
    HRESULT (STDMETHODCALLTYPE *GetPendingUrl)(TritonDocObjectService *, BSTR *);
    HRESULT (STDMETHODCALLTYPE *ActiveElementChanged)(TritonDocObjectService *, IHTMLElement *);
    HRESULT (STDMETHODCALLTYPE *GetUrlSearchComponent)(TritonDocObjectService *, BSTR *);
    HRESULT (STDMETHODCALLTYPE *IsErrorUrl)(TritonDocObjectService *, LPCWSTR, BOOL *);
} TritonDocObjectServiceVtbl;
struct TritonDocObjectService { const TritonDocObjectServiceVtbl *lpVtbl; };
#endif
