/* Checked IEFRAME CBaseBrowser2 IPersistHistory2 table and
 * CTravelEntry::_PersistHistoryToStream, VA 75f53263. The eight inherited
 * IPersistHistory slots precede SaveHistoryEx(IStream *, IBindCtx *). */
#ifndef TRITON_MSHTML_HISTORY_CONTRACT_H
#define TRITON_MSHTML_HISTORY_CONTRACT_H
#include <stddef.h>
#include <perhist.h>
static const IID triton_persist_history2_iid =
    {0x4f77badd,0xa032,0x41c7,{0xb3,0x2f,0x0b,0x44,0x57,0x0d,0xf1,0x39}};

/* Checked Vista CDoc::InitDocHost (771ccd54..771ccd87) obtains the browser's
 * travel log from IBrowserService slot +1c. CDoc::Travel (771dd925..771dd94c)
 * then calls ITravelLog slot +18 with browser identity and signed offset. The
 * zero pushed before those arguments belongs to checked DbgExTraceHR, not the
 * COM call; declaring a third argument corrupts the stdcall stack.
 * CDoc::UpdateBackForwardState (771ddaa0..771ddad6) then calls browser-service
 * slot +48; checked CShellBrowser2::UpdateBackForwardState (75ec7dc6) posts
 * the deferred shell-state refresh. Only consumed slots are named here; this
 * is not a claim to implement either complete private host interface. */
typedef struct TritonTravelLog TritonTravelLog;
typedef struct TritonTravelLogVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(TritonTravelLog *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(TritonTravelLog *);
    ULONG (STDMETHODCALLTYPE *Release)(TritonTravelLog *);
    HRESULT (STDMETHODCALLTYPE *AddEntry)(TritonTravelLog *, IUnknown *, BOOL);
    HRESULT (STDMETHODCALLTYPE *UpdateEntry)(TritonTravelLog *, IUnknown *, BOOL);
    HRESULT (STDMETHODCALLTYPE *UpdateExternal)(TritonTravelLog *, IUnknown *, IUnknown *);
    HRESULT (STDMETHODCALLTYPE *Travel)(TritonTravelLog *, IUnknown *, int);
    HRESULT (STDMETHODCALLTYPE *GetTravelEntry)(TritonTravelLog *, IUnknown *, int, IUnknown **);
} TritonTravelLogVtbl;
struct TritonTravelLog { const TritonTravelLogVtbl *lpVtbl; };

typedef struct TritonBrowserService TritonBrowserService;
typedef struct TritonBrowserServiceVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(TritonBrowserService *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(TritonBrowserService *);
    ULONG (STDMETHODCALLTYPE *Release)(TritonBrowserService *);
    HRESULT (STDMETHODCALLTYPE *GetParentSite)(TritonBrowserService *, void **);
    HRESULT (STDMETHODCALLTYPE *SetTitle)(TritonBrowserService *, void *, LPCWSTR);
    HRESULT (STDMETHODCALLTYPE *GetTitle)(TritonBrowserService *, void *, LPWSTR, DWORD);
    HRESULT (STDMETHODCALLTYPE *GetOleObject)(TritonBrowserService *, IOleObject **);
    HRESULT (STDMETHODCALLTYPE *GetTravelLog)(TritonBrowserService *, TritonTravelLog **);
    void *Reserved8;
    void *Reserved9;
    void *Reserved10;
    void *Reserved11;
    void *Reserved12;
    void *Reserved13;
    void *Reserved14;
    void *Reserved15;
    void *Reserved16;
    void *Reserved17;
    HRESULT (STDMETHODCALLTYPE *UpdateBackForwardState)(TritonBrowserService *);
} TritonBrowserServiceVtbl;
_Static_assert(offsetof(TritonBrowserServiceVtbl, UpdateBackForwardState) == 18 * sizeof(void *),
    "checked Vista IBrowserService history-state slot must stay at vtable index 18");
struct TritonBrowserService { const TritonBrowserServiceVtbl *lpVtbl; };
typedef struct TritonPersistHistory2Vtbl {
    IPersistHistoryVtbl base;
    HRESULT (STDMETHODCALLTYPE *SaveHistoryEx)(IPersistHistory *, IStream *, IBindCtx *);
} TritonPersistHistory2Vtbl;
#endif
