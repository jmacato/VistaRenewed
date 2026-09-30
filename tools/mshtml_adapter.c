/*
 * x86 activation probe for the disposable IE7 integration exercise.
 *
 * The guest runner temporarily shadows the 32-bit HTMLDocument class in the
 * current user's HKCU\Software\Classes hive, starts iexplore.exe, captures the
 * actual activation sequence, and removes the shadow before its job ends.
 * This incremental shim implements COM/persistence plumbing, a shared
 * Supermium runtime, and windowed DocObject activation. Most DOM interfaces
 * remain incomplete. IE owns the native chrome; individual Chromium app
 * windows are embedded below each document host.
 * It never advertises ActiveX support and never registers itself.
 */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <cguid.h>
#include <mshtml.h>
#include <mshtmdid.h>
#include <objbase.h>
#include <oaidl.h>
#include <objidl.h>
#include <oleidl.h>
#include <docobj.h>
#include <servprov.h>
#include <idispids.h>
#include <urlmon.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <wctype.h>
#include <unknwn.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <ocidl.h>
#include <perhist.h>
#include <exdisp.h>
DEFINE_GUID(IID_IPersistHistory, 0x91a565c1, 0xe38f, 0x11d0, 0x94, 0xbf, 0x00, 0xa0, 0xc9, 0x05, 0x5c, 0xbf);

#include "mshtml_json.h"
#include "mshtml_private_window.h"
#include "mshtml_history_contract.h"
#include "mshtml_cdp_transport.h"

static INIT_ONCE state_paths_once = INIT_ONCE_STATIC_INIT;
static WCHAR state_directory[MAX_PATH], profile_directory[MAX_PATH], log_path[MAX_PATH];
static LONG live_objects;
static LONG server_locks;
static HINSTANCE module_instance;

#include "mshtml_keyboard_router.h"

static BOOL CALLBACK initialize_state_paths(PINIT_ONCE once, PVOID parameter, PVOID *context)
{
    HANDLE token;
    TOKEN_MANDATORY_LABEL *label;
    DWORD size = 0, rid;
    PWSTR local_low = NULL;
    BOOL ok;
    (void)once; (void)parameter; (void)context;
    if(!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return FALSE;
    GetTokenInformation(token, TokenIntegrityLevel, NULL, 0, &size);
    label = size ? HeapAlloc(GetProcessHeap(), 0, size) : NULL;
    ok = label && GetTokenInformation(token, TokenIntegrityLevel, label, size, &size);
    if(ok) rid = *GetSidSubAuthority(label->Label.Sid, *GetSidSubAuthorityCount(label->Label.Sid) - 1);
    else rid = 0;
    if(label) HeapFree(GetProcessHeap(), 0, label);
    CloseHandle(token);
    if(!ok) return FALSE; // Never guess an integrity boundary after a failed query.
    if(rid < SECURITY_MANDATORY_MEDIUM_RID) {
        if(FAILED(SHGetKnownFolderPath(&FOLDERID_LocalAppDataLow, 0, NULL, &local_low))) return FALSE;
        if(lstrlenW(local_low) > MAX_PATH - 100) { CoTaskMemFree(local_low); return FALSE; }
        wsprintfW(state_directory, L"%ls\\TritonSupermiumBridge", local_low);
        CoTaskMemFree(local_low);
    } else {
        lstrcpyW(state_directory, L"C:\\TritonSupermiumBridge");
    }
    if(!CreateDirectoryW(state_directory, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return FALSE;
    wsprintfW(profile_directory, L"%ls\\IEProfile", state_directory);
    wsprintfW(log_path, L"%ls\\iexplore-mshtml-activation-probe.log", state_directory);
    return TRUE;
}

static BOOL ensure_state_paths(void)
{
    return InitOnceExecuteOnce(&state_paths_once, initialize_state_paths, NULL, NULL);
}

typedef struct ProbeFactory {
    IClassFactory IClassFactory_iface;
    LONG references;
} ProbeFactory;

typedef struct BrowserRuntime {
    HANDLE pipe_read;
    HANDLE pipe_write;
    HANDLE browser_process;
    CdpTransport *cdp;
    unsigned live_pages;
    unsigned references;
    CRITICAL_SECTION transport_lock;
} BrowserRuntime;

static SRWLOCK runtime_registry_lock = SRWLOCK_INIT;
static BrowserRuntime *shared_runtime;

static BrowserRuntime *runtime_acquire(void)
{
    BrowserRuntime *runtime;
    if(!ensure_state_paths()) return NULL;
    AcquireSRWLockExclusive(&runtime_registry_lock);
    runtime = shared_runtime;
    if(!runtime) {
        runtime = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*runtime));
        if(runtime && !InitializeCriticalSectionAndSpinCount(&runtime->transport_lock, 0)) {
            HeapFree(GetProcessHeap(), 0, runtime);
            runtime = NULL;
        }
        shared_runtime = runtime;
    }
    if(runtime) ++runtime->references;
    ReleaseSRWLockExclusive(&runtime_registry_lock);
    return runtime;
}

static void runtime_release(BrowserRuntime *runtime)
{
    AcquireSRWLockExclusive(&runtime_registry_lock);
    if(--runtime->references == 0) {
        shared_runtime = NULL;
        cdp_stop(runtime->cdp);
        if(runtime->browser_process) CloseHandle(runtime->browser_process);
        DeleteCriticalSection(&runtime->transport_lock);
        HeapFree(GetProcessHeap(), 0, runtime);
    }
    ReleaseSRWLockExclusive(&runtime_registry_lock);
}

typedef struct ElementProxy ElementProxy;
typedef struct PropertySubscription PropertySubscription;
typedef struct HistorySnapshot HistorySnapshot;

typedef struct ProbeDocument {
    IPersist IPersist_iface;
    IPersistMoniker IPersistMoniker_iface;
    IPersistFile IPersistFile_iface;
    IPersistStreamInit IPersistStreamInit_iface;
    IPersistHistory IPersistHistory_iface;
    IMonikerProp IMonikerProp_iface;
    IOleObject IOleObject_iface;
    IViewObject IViewObject_iface;
    IOleDocument IOleDocument_iface;
    IServiceProvider IServiceProvider_iface;
    IInternetSecurityManager IInternetSecurityManager_iface;
    IOleCommandTarget IOleCommandTarget_iface;
    IOleDocumentView IOleDocumentView_iface;
    IOleInPlaceObject IOleInPlaceObject_iface;
    IOleInPlaceActiveObject IOleInPlaceActiveObject_iface;
    IHTMLDocument2 IHTMLDocument2_iface;
    IHTMLDocument3 IHTMLDocument3_iface;
    IDispatch IDispatch_iface;
    IHTMLWindow2 IHTMLWindow2_iface;
    TritonPrivateWindow4 private_window_iface;
    IConnectionPointContainer connections_iface;
    IConnectionPoint property_point_iface;
    PropertySubscription *property_subscriptions;
    DWORD next_property_cookie;
    LONG ready_state;
    BSTR window_name;
    BOOL window_closed;
    HWND state_observer;
    WNDPROC state_observer_original;
    IOleClientSite *client_site;
    IOleInPlaceSite *view_site;
    IOleInPlaceFrame *in_place_frame;
    IOleInPlaceUIWindow *in_place_ui_window;
    RECT view_rect;
    BOOL view_visible;
    BOOL in_place_active;
    BOOL ui_active;
    BOOL activating_document_site;
    BOOL keyboard_router_acquired;
    HWND host_window;
    DWORD supermium_process_id;
    HWND renderer_window;
    HWND renderer_parent;
    HWND viewport_window;
    BSTR current_url;
    BSTR notified_title;
    BSTR persisted_html;
    IMoniker *current_moniker;
    BrowserRuntime *runtime;
    char page_session[80];
    char page_target[80];
    unsigned popup_scan_request;
    DWORD next_popup_scan;
    char navigation_loader[80];
    char main_frame[80], observed_loader[80], committed_loader[80], lifecycle_loader[80];
    BSTR observed_url;
    BSTR renderer_title;
    unsigned title_request, navigation_probe_request, ready_probe_request;
    DWORD next_title_poll, next_navigation_probe;
    LONG lifecycle_state;
    unsigned navigation_request, navigation_kind, navigation_cookie;
    BOOL navigation_wait_ack, navigation_committed, processing_events;
    BOOL navigation_expects_same_document;
    BOOL observed_same_document;
    unsigned navigation_serial;
    BOOL navigation_virtual_url;
    BOOL host_navigate_notified;
    DWORD input_owner_thread;
    DWORD input_target_thread;
    unsigned host_load_phase;
    BOOL notifying_host;
    DWORD next_state_poll;
    LONG references;
    ElementProxy *elements; /* Weak list; each live element owns a document reference. */
    unsigned page_generation;
    GUID history_context;
    HistorySnapshot *history_current;
    WCHAR resource_origin[100];
    char resource_script_session[80];
    HistorySnapshot *history_departure;
    const HistorySnapshot *history_export; /* Borrowed during a host callback. */
    DWORD history_position;
    unsigned history_request;
    BOOL history_waiting_commit;
    BOOL history_snapshot_pending;
    BSTR history_restart_state;
    DWORD history_restart_position;
    char *request_post_data;       /* Base64 body for the next top-level request. */
    char *request_headers;         /* CDP HeaderEntry array, without surrounding key. */
    char request_origin[512];      /* ASCII serialization of scheme://authority. */
    BOOL request_override_pending;
    BOOL request_fetch_enabled;
    unsigned security_reported_serial;
    BOOL navigation_security_error;
    BOOL navigation_download;
} ProbeDocument;

static HRESULT html2_ready_state(IHTMLDocument2 *iface, BSTR *state);
static void document_set_ready_state(ProbeDocument *document, LONG state);
static void document_poll_popups(ProbeDocument *document);
static void document_receive_popups(ProbeDocument *document, CdpMessage *message);
static void document_clear_subscriptions(ProbeDocument *document);
static HRESULT document_host_navigation(ProbeDocument *document, IOleClientSite *site, BOOL enable);
static HRESULT document_parent_window(ProbeDocument *document, IHTMLWindow2 **window);
static HRESULT document_observe_state(ProbeDocument *document, BOOL enable);
static HRESULT document_get_browser_events(IOleClientSite *site, TritonDocObjectService **events);
static void document_process_events(ProbeDocument *document);
static void document_poll_title(ProbeDocument *document);
static void document_poll_navigation_state(ProbeDocument *document);
static void history_clear(ProbeDocument *document);
static HRESULT history_capture_departure(ProbeDocument *document);
static void history_request_snapshot(ProbeDocument *document);
static void history_renderer_travel_request(ProbeDocument *document, const char *params);
static void history_receive_snapshot(ProbeDocument *document, const CdpMessage *message);
static void history_apply_restart_state(ProbeDocument *document);
static void document_request_clear(ProbeDocument *document);
static void document_continue_paused_request(ProbeDocument *document, const char *params);
static HRESULT resource_enable(ProbeDocument *document);
static BOOL resource_paused(ProbeDocument *document, const char *params);
static HRESULT resource_map(ProbeDocument *document, LPCWSTR url, WCHAR *mapped, unsigned capacity);
static void document_open_new_window_event(ProbeDocument *document, const char *params);
static HRESULT document_post_navigation(ProbeDocument *document, const char *method,
    const char *params, unsigned kind, LPCWSTR url, BOOL virtual_url);
static HRESULT content_get_title(ProbeDocument *document, BSTR *value);
static void close_supermium_window(ProbeDocument *document);
static HRESULT document_pipe_call(ProbeDocument *document, const char *method,
                                  const char *params, const char *session,
                                  char *reply, size_t capacity);


static HRESULT STDMETHODCALLTYPE factory_query_interface(IClassFactory *iface, REFIID iid,
                                                          void **out);
static ULONG STDMETHODCALLTYPE factory_add_ref(IClassFactory *iface);
static ULONG STDMETHODCALLTYPE factory_release(IClassFactory *iface);
static HRESULT STDMETHODCALLTYPE factory_create_instance(IClassFactory *iface, IUnknown *outer,
                                                         REFIID iid, void **out);
static HRESULT STDMETHODCALLTYPE factory_lock_server(IClassFactory *iface, BOOL lock);

static HRESULT STDMETHODCALLTYPE persist_query_interface(IPersist *iface, REFIID iid, void **out);
static ULONG STDMETHODCALLTYPE persist_add_ref(IPersist *iface);
static ULONG STDMETHODCALLTYPE persist_release(IPersist *iface);
static HRESULT STDMETHODCALLTYPE persist_get_class_id(IPersist *iface, CLSID *class_id);
static HRESULT STDMETHODCALLTYPE moniker_query_interface(IPersistMoniker *iface, REFIID iid,
                                                          void **out);
static ULONG STDMETHODCALLTYPE moniker_add_ref(IPersistMoniker *iface);
static ULONG STDMETHODCALLTYPE moniker_release(IPersistMoniker *iface);
static HRESULT STDMETHODCALLTYPE moniker_get_class_id(IPersistMoniker *iface, CLSID *class_id);
static HRESULT STDMETHODCALLTYPE moniker_is_dirty(IPersistMoniker *iface);
static HRESULT STDMETHODCALLTYPE moniker_load(IPersistMoniker *iface, BOOL fully_available,
                                              IMoniker *moniker, LPBC bind_context, DWORD mode);
static HRESULT STDMETHODCALLTYPE moniker_save(IPersistMoniker *iface, IMoniker *moniker,
                                              LPBC bind_context, BOOL remember);
static HRESULT STDMETHODCALLTYPE moniker_save_completed(IPersistMoniker *iface, IMoniker *moniker,
                                                        LPBC bind_context);
static HRESULT STDMETHODCALLTYPE moniker_get_cur_moniker(IPersistMoniker *iface, IMoniker **moniker);
static HRESULT STDMETHODCALLTYPE file_query_interface(IPersistFile *iface, REFIID iid, void **out);
static ULONG STDMETHODCALLTYPE file_add_ref(IPersistFile *iface);
static ULONG STDMETHODCALLTYPE file_release(IPersistFile *iface);
static HRESULT STDMETHODCALLTYPE file_get_class_id(IPersistFile *iface, CLSID *class_id);
static HRESULT STDMETHODCALLTYPE file_is_dirty(IPersistFile *iface);
static HRESULT STDMETHODCALLTYPE file_load(IPersistFile *iface, LPCOLESTR file_name, DWORD mode);
static HRESULT STDMETHODCALLTYPE file_save(IPersistFile *iface, LPCOLESTR file_name, BOOL remember);
static HRESULT STDMETHODCALLTYPE file_save_completed(IPersistFile *iface, LPCOLESTR file_name);
static HRESULT STDMETHODCALLTYPE file_get_cur_file(IPersistFile *iface, LPOLESTR *file_name);
static HRESULT STDMETHODCALLTYPE moniker_prop_query_interface(IMonikerProp *iface, REFIID iid,
                                                               void **out);
static ULONG STDMETHODCALLTYPE moniker_prop_add_ref(IMonikerProp *iface);
static ULONG STDMETHODCALLTYPE moniker_prop_release(IMonikerProp *iface);
static HRESULT STDMETHODCALLTYPE moniker_prop_put_property(IMonikerProp *iface,
                                                            MONIKERPROPERTY property,
                                                            LPCWSTR value);
static HRESULT STDMETHODCALLTYPE ole_query_interface(IOleObject *iface, REFIID iid, void **out);
static ULONG STDMETHODCALLTYPE ole_add_ref(IOleObject *iface);
static ULONG STDMETHODCALLTYPE ole_release(IOleObject *iface);
static HRESULT STDMETHODCALLTYPE ole_set_client_site(IOleObject *iface, IOleClientSite *client_site);
static HRESULT STDMETHODCALLTYPE ole_get_client_site(IOleObject *iface, IOleClientSite **client_site);
static HRESULT STDMETHODCALLTYPE ole_set_host_names(IOleObject *iface, LPCOLESTR application,
                                                    LPCOLESTR object);
static HRESULT STDMETHODCALLTYPE ole_close(IOleObject *iface, DWORD save_option);
static HRESULT STDMETHODCALLTYPE ole_set_moniker(IOleObject *iface, DWORD which_moniker,
                                                 IMoniker *moniker);
static HRESULT STDMETHODCALLTYPE ole_get_moniker(IOleObject *iface, DWORD assign,
                                                 DWORD which_moniker, IMoniker **moniker);
static HRESULT STDMETHODCALLTYPE ole_init_from_data(IOleObject *iface, IDataObject *data,
                                                    BOOL creation, DWORD reserved);
static HRESULT STDMETHODCALLTYPE ole_get_clipboard_data(IOleObject *iface, DWORD reserved,
                                                        IDataObject **data);
static HRESULT STDMETHODCALLTYPE ole_do_verb(IOleObject *iface, LONG verb, LPMSG message,
                                             IOleClientSite *client_site, LONG index, HWND parent,
                                             LPCRECT rectangle);
static HRESULT STDMETHODCALLTYPE ole_enum_verbs(IOleObject *iface, IEnumOLEVERB **verbs);
static HRESULT STDMETHODCALLTYPE ole_update(IOleObject *iface);
static HRESULT STDMETHODCALLTYPE ole_is_up_to_date(IOleObject *iface);
static HRESULT STDMETHODCALLTYPE ole_get_user_class_id(IOleObject *iface, CLSID *class_id);
static HRESULT STDMETHODCALLTYPE ole_get_user_type(IOleObject *iface, DWORD form,
                                                   LPOLESTR *user_type);
static HRESULT STDMETHODCALLTYPE ole_set_extent(IOleObject *iface, DWORD aspect, SIZEL *size);
static HRESULT STDMETHODCALLTYPE ole_get_extent(IOleObject *iface, DWORD aspect, SIZEL *size);
static HRESULT STDMETHODCALLTYPE ole_advise(IOleObject *iface, IAdviseSink *sink, DWORD *connection);
static HRESULT STDMETHODCALLTYPE ole_unadvise(IOleObject *iface, DWORD connection);
static HRESULT STDMETHODCALLTYPE ole_enum_advise(IOleObject *iface, IEnumSTATDATA **enumerator);
static HRESULT STDMETHODCALLTYPE ole_get_misc_status(IOleObject *iface, DWORD aspect, DWORD *status);
static HRESULT STDMETHODCALLTYPE ole_set_color_scheme(IOleObject *iface, LOGPALETTE *palette);
static HRESULT STDMETHODCALLTYPE view_query_interface(IViewObject *iface, REFIID iid, void **out);
static ULONG STDMETHODCALLTYPE view_add_ref(IViewObject *iface);
static ULONG STDMETHODCALLTYPE view_release(IViewObject *iface);
static HRESULT STDMETHODCALLTYPE view_draw(IViewObject *iface, DWORD aspect, LONG index,
                                           void *aspect_info, DVTARGETDEVICE *target,
                                           HDC target_hdc, HDC draw_hdc, LPCRECTL bounds,
                                           LPCRECTL w_bounds,
                                           BOOL (STDMETHODCALLTYPE *continue_fn)(ULONG_PTR),
                                           ULONG_PTR continue_data);
static HRESULT STDMETHODCALLTYPE view_get_color_set(IViewObject *iface, DWORD aspect, LONG index,
                                                    void *aspect_info, DVTARGETDEVICE *target,
                                                    HDC target_hdc, LOGPALETTE **palette);
static HRESULT STDMETHODCALLTYPE view_freeze(IViewObject *iface, DWORD aspect, LONG index,
                                             void *aspect_info, DWORD *freeze);
static HRESULT STDMETHODCALLTYPE view_unfreeze(IViewObject *iface, DWORD freeze);
static HRESULT STDMETHODCALLTYPE view_set_advise(IViewObject *iface, DWORD aspects, DWORD flags,
                                                 IAdviseSink *sink);
static HRESULT STDMETHODCALLTYPE view_get_advise(IViewObject *iface, DWORD *aspects, DWORD *flags,
                                                 IAdviseSink **sink);
static HRESULT STDMETHODCALLTYPE ole_document_query_interface(IOleDocument *iface, REFIID iid,
                                                               void **out);
static ULONG STDMETHODCALLTYPE ole_document_add_ref(IOleDocument *iface);
static ULONG STDMETHODCALLTYPE ole_document_release(IOleDocument *iface);
static HRESULT STDMETHODCALLTYPE ole_document_create_view(IOleDocument *iface,
                                                          IOleInPlaceSite *site, IStream *stream,
                                                          DWORD reserved, IOleDocumentView **view);
static HRESULT STDMETHODCALLTYPE ole_document_get_misc_status(IOleDocument *iface, DWORD *status);
static HRESULT STDMETHODCALLTYPE ole_document_enum_views(IOleDocument *iface,
                                                         IEnumOleDocumentViews **enumerator,
                                                         IOleDocumentView **view);
static HRESULT STDMETHODCALLTYPE service_provider_query_interface(IServiceProvider *iface,
                                                                   REFIID iid, void **out);
static ULONG STDMETHODCALLTYPE service_provider_add_ref(IServiceProvider *iface);
static ULONG STDMETHODCALLTYPE service_provider_release(IServiceProvider *iface);
static HRESULT STDMETHODCALLTYPE service_provider_query_service(IServiceProvider *iface,
                                                                 REFGUID service, REFIID iid,
                                                                 void **out);
static HRESULT STDMETHODCALLTYPE command_target_query_interface(IOleCommandTarget *iface,
                                                                 REFIID iid, void **out);
static ULONG STDMETHODCALLTYPE command_target_add_ref(IOleCommandTarget *iface);
static ULONG STDMETHODCALLTYPE command_target_release(IOleCommandTarget *iface);
static HRESULT STDMETHODCALLTYPE command_target_query_status(IOleCommandTarget *iface,
                                                              const GUID *group, ULONG count,
                                                              OLECMD commands[],
                                                              OLECMDTEXT *command_text);
static HRESULT STDMETHODCALLTYPE command_target_exec(IOleCommandTarget *iface,
                                                      const GUID *group, DWORD command_id,
                                                      DWORD command_exec_opt, VARIANT *input,
                                                      VARIANT *output);
static HRESULT STDMETHODCALLTYPE document_view_query_interface(IOleDocumentView *iface,
                                                                REFIID iid, void **out);
static ULONG STDMETHODCALLTYPE document_view_add_ref(IOleDocumentView *iface);
static ULONG STDMETHODCALLTYPE document_view_release(IOleDocumentView *iface);
static HRESULT STDMETHODCALLTYPE document_view_set_in_place_site(IOleDocumentView *iface,
                                                                  IOleInPlaceSite *site);
static HRESULT STDMETHODCALLTYPE document_view_get_in_place_site(IOleDocumentView *iface,
                                                                  IOleInPlaceSite **site);
static HRESULT STDMETHODCALLTYPE document_view_get_document(IOleDocumentView *iface,
                                                             IUnknown **document);
static HRESULT STDMETHODCALLTYPE document_view_set_rect(IOleDocumentView *iface, LPRECT rect);
static HRESULT STDMETHODCALLTYPE document_view_get_rect(IOleDocumentView *iface, LPRECT rect);
static HRESULT STDMETHODCALLTYPE document_view_set_rect_complex(IOleDocumentView *iface,
                                                                 LPRECT view_rect,
                                                                 LPRECT horizontal_scroll,
                                                                 LPRECT vertical_scroll,
                                                                 LPRECT size_box);
static HRESULT STDMETHODCALLTYPE document_view_show(IOleDocumentView *iface, WINBOOL show);
static HRESULT STDMETHODCALLTYPE document_view_ui_activate(IOleDocumentView *iface,
                                                           WINBOOL activate);
static HRESULT STDMETHODCALLTYPE document_view_open(IOleDocumentView *iface);
static HRESULT STDMETHODCALLTYPE document_view_close_view(IOleDocumentView *iface,
                                                          DWORD reserved);
static HRESULT STDMETHODCALLTYPE document_view_save_view_state(IOleDocumentView *iface,
                                                               LPSTREAM stream);
static HRESULT STDMETHODCALLTYPE document_view_apply_view_state(IOleDocumentView *iface,
                                                                LPSTREAM stream);
static HRESULT STDMETHODCALLTYPE document_view_clone(IOleDocumentView *iface,
                                                      IOleInPlaceSite *site,
                                                      IOleDocumentView **view);
static HRESULT STDMETHODCALLTYPE in_place_object_query_interface(IOleInPlaceObject *iface,
                                                                  REFIID iid, void **out);
static ULONG STDMETHODCALLTYPE in_place_object_add_ref(IOleInPlaceObject *iface);
static ULONG STDMETHODCALLTYPE in_place_object_release(IOleInPlaceObject *iface);
static HRESULT STDMETHODCALLTYPE in_place_object_get_window(IOleInPlaceObject *iface,
                                                            HWND *window);
static HRESULT STDMETHODCALLTYPE in_place_object_context_sensitive_help(IOleInPlaceObject *iface,
                                                                         WINBOOL enter_mode);
static HRESULT STDMETHODCALLTYPE in_place_object_deactivate(IOleInPlaceObject *iface);
static HRESULT STDMETHODCALLTYPE in_place_object_ui_deactivate(IOleInPlaceObject *iface);
static HRESULT STDMETHODCALLTYPE in_place_object_set_object_rects(IOleInPlaceObject *iface,
                                                                   LPCRECT position,
                                                                   LPCRECT clip);
static HRESULT STDMETHODCALLTYPE in_place_object_reactivate_and_undo(IOleInPlaceObject *iface);
static HRESULT STDMETHODCALLTYPE active_object_query_interface(IOleInPlaceActiveObject *iface,
                                                                REFIID iid, void **out);
static ULONG STDMETHODCALLTYPE active_object_add_ref(IOleInPlaceActiveObject *iface);
static ULONG STDMETHODCALLTYPE active_object_release(IOleInPlaceActiveObject *iface);
static HRESULT STDMETHODCALLTYPE active_object_get_window(IOleInPlaceActiveObject *iface,
                                                          HWND *window);
static HRESULT STDMETHODCALLTYPE active_object_context_sensitive_help(IOleInPlaceActiveObject *iface,
                                                                       WINBOOL enter_mode);
static HRESULT STDMETHODCALLTYPE active_object_translate_accelerator(IOleInPlaceActiveObject *iface,
                                                                      LPMSG message);
static HRESULT STDMETHODCALLTYPE active_object_on_frame_window_activate(
    IOleInPlaceActiveObject *iface, WINBOOL activate);
static HRESULT STDMETHODCALLTYPE active_object_on_doc_window_activate(
    IOleInPlaceActiveObject *iface, WINBOOL activate);
static HRESULT STDMETHODCALLTYPE active_object_resize_border(IOleInPlaceActiveObject *iface,
                                                              LPCRECT border,
                                                              IOleInPlaceUIWindow *window,
                                                              WINBOOL frame_window);
static HRESULT STDMETHODCALLTYPE active_object_enable_modeless(IOleInPlaceActiveObject *iface,
                                                               WINBOOL enable);
static HRESULT STDMETHODCALLTYPE dispatch_query_interface(IDispatch *iface, REFIID iid, void **out);
static ULONG STDMETHODCALLTYPE dispatch_add_ref(IDispatch *iface);
static ULONG STDMETHODCALLTYPE dispatch_release(IDispatch *iface);
static HRESULT STDMETHODCALLTYPE dispatch_get_type_info_count(IDispatch *iface, UINT *count);
static HRESULT STDMETHODCALLTYPE dispatch_get_type_info(IDispatch *iface, UINT index, LCID locale,
                                                         ITypeInfo **info);
static HRESULT STDMETHODCALLTYPE dispatch_get_ids_of_names(IDispatch *iface, REFIID iid,
                                                            LPOLESTR *names, UINT count,
                                                            LCID locale, DISPID *ids);
static HRESULT STDMETHODCALLTYPE dispatch_invoke(IDispatch *iface, DISPID member, REFIID iid,
                                                 LCID locale, WORD flags, DISPPARAMS *params,
                                                 VARIANT *result, EXCEPINFO *exception,
                                                 UINT *argument_error);

static IClassFactoryVtbl factory_vtbl = {
    factory_query_interface, factory_add_ref, factory_release,
    factory_create_instance, factory_lock_server,
};
static IPersistVtbl persist_vtbl = {
    persist_query_interface, persist_add_ref, persist_release, persist_get_class_id,
};
static IPersistMonikerVtbl moniker_vtbl = {
    moniker_query_interface, moniker_add_ref, moniker_release, moniker_get_class_id,
    moniker_is_dirty, moniker_load, moniker_save, moniker_save_completed, moniker_get_cur_moniker,
};
static IPersistFileVtbl file_vtbl = {
    file_query_interface, file_add_ref, file_release, file_get_class_id, file_is_dirty,
    file_load, file_save, file_save_completed, file_get_cur_file,
};
static IMonikerPropVtbl moniker_prop_vtbl = {
    moniker_prop_query_interface, moniker_prop_add_ref, moniker_prop_release,
    moniker_prop_put_property,
};
static IOleObjectVtbl ole_vtbl = {
    ole_query_interface, ole_add_ref, ole_release, ole_set_client_site, ole_get_client_site,
    ole_set_host_names, ole_close, ole_set_moniker, ole_get_moniker, ole_init_from_data,
    ole_get_clipboard_data, ole_do_verb, ole_enum_verbs, ole_update, ole_is_up_to_date,
    ole_get_user_class_id, ole_get_user_type, ole_set_extent, ole_get_extent, ole_advise,
    ole_unadvise, ole_enum_advise, ole_get_misc_status, ole_set_color_scheme,
};
static IViewObjectVtbl view_vtbl = {
    view_query_interface, view_add_ref, view_release, view_draw, view_get_color_set,
    view_freeze, view_unfreeze, view_set_advise, view_get_advise,
};
static IOleDocumentVtbl ole_document_vtbl = {
    ole_document_query_interface, ole_document_add_ref, ole_document_release,
    ole_document_create_view, ole_document_get_misc_status, ole_document_enum_views,
};
static IServiceProviderVtbl service_provider_vtbl = {
    service_provider_query_interface, service_provider_add_ref, service_provider_release,
    service_provider_query_service,
};
static IOleCommandTargetVtbl command_target_vtbl = {
    command_target_query_interface, command_target_add_ref, command_target_release,
    command_target_query_status, command_target_exec,
};
static IOleDocumentViewVtbl document_view_vtbl = {
    document_view_query_interface, document_view_add_ref, document_view_release,
    document_view_set_in_place_site, document_view_get_in_place_site,
    document_view_get_document, document_view_set_rect, document_view_get_rect,
    document_view_set_rect_complex, document_view_show, document_view_ui_activate,
    document_view_open, document_view_close_view, document_view_save_view_state,
    document_view_apply_view_state, document_view_clone,
};
static IOleInPlaceObjectVtbl in_place_object_vtbl = {
    in_place_object_query_interface, in_place_object_add_ref, in_place_object_release,
    in_place_object_get_window, in_place_object_context_sensitive_help,
    in_place_object_deactivate, in_place_object_ui_deactivate,
    in_place_object_set_object_rects, in_place_object_reactivate_and_undo,
};
static IOleInPlaceActiveObjectVtbl active_object_vtbl = {
    active_object_query_interface, active_object_add_ref, active_object_release,
    active_object_get_window, active_object_context_sensitive_help,
    active_object_translate_accelerator, active_object_on_frame_window_activate,
    active_object_on_doc_window_activate, active_object_resize_border,
    active_object_enable_modeless,
};
static IDispatchVtbl dispatch_vtbl = {
    dispatch_query_interface, dispatch_add_ref, dispatch_release, dispatch_get_type_info_count,
    dispatch_get_type_info, dispatch_get_ids_of_names, dispatch_invoke,
};

static void append_text(const WCHAR *text)
{
    HANDLE file;
    DWORD bytes, written;
    UINT length = lstrlenW(text);
    char buffer[256];

    if(!ensure_state_paths()) return;
    file = CreateFileW(log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                       OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if(file == INVALID_HANDLE_VALUE) return;
    bytes = (DWORD)WideCharToMultiByte(CP_UTF8, 0, text, length, NULL, 0, NULL, NULL);
    if(bytes && bytes < ARRAYSIZE(buffer) &&
       WideCharToMultiByte(CP_UTF8, 0, text, length, buffer, bytes, NULL, NULL))
        WriteFile(file, buffer, bytes, &written, NULL);
    CloseHandle(file);
}

static void append_guid(const WCHAR *prefix, REFIID iid)
{
    WCHAR guid[40], line[128];
    if(!StringFromGUID2(iid, guid, ARRAYSIZE(guid))) lstrcpyW(guid, L"{invalid-guid}");
    wsprintfW(line, L"%ls%ls\r\n", prefix, guid);
    append_text(line);
}

static void append_dword(const WCHAR *prefix, DWORD value)
{
    WCHAR line[128];
    wsprintfW(line, L"%ls%lu\r\n", prefix, (unsigned long)value);
    append_text(line);
}

static void append_rect(const WCHAR *prefix, const RECT *rect)
{
    WCHAR line[192];
    if(!rect) return;
    wsprintfW(line, L"%ls%ld,%ld,%ld,%ld\r\n", prefix, (long)rect->left, (long)rect->top,
              (long)rect->right, (long)rect->bottom);
    append_text(line);
}

static ProbeDocument *from_persist(IPersist *iface)
{
    return (ProbeDocument *)iface;
}

static ProbeDocument *from_moniker(IPersistMoniker *iface)
{
    return (ProbeDocument *)((char *)iface - offsetof(ProbeDocument, IPersistMoniker_iface));
}

static ProbeDocument *from_file(IPersistFile *iface)
{
    return (ProbeDocument *)((char *)iface - offsetof(ProbeDocument, IPersistFile_iface));
}

static ProbeDocument *from_moniker_prop(IMonikerProp *iface)
{
    return (ProbeDocument *)((char *)iface - offsetof(ProbeDocument, IMonikerProp_iface));
}

static ProbeDocument *from_ole(IOleObject *iface)
{
    return (ProbeDocument *)((char *)iface - offsetof(ProbeDocument, IOleObject_iface));
}

static ProbeDocument *from_view(IViewObject *iface)
{
    return (ProbeDocument *)((char *)iface - offsetof(ProbeDocument, IViewObject_iface));
}

static ProbeDocument *from_ole_document(IOleDocument *iface)
{
    return (ProbeDocument *)((char *)iface - offsetof(ProbeDocument, IOleDocument_iface));
}

static ProbeDocument *from_service_provider(IServiceProvider *iface)
{
    return (ProbeDocument *)((char *)iface - offsetof(ProbeDocument, IServiceProvider_iface));
}

static ProbeDocument *from_command_target(IOleCommandTarget *iface)
{
    return (ProbeDocument *)((char *)iface - offsetof(ProbeDocument, IOleCommandTarget_iface));
}

static ProbeDocument *from_document_view(IOleDocumentView *iface)
{
    return (ProbeDocument *)((char *)iface - offsetof(ProbeDocument, IOleDocumentView_iface));
}

static ProbeDocument *from_in_place_object(IOleInPlaceObject *iface)
{
    return (ProbeDocument *)((char *)iface - offsetof(ProbeDocument, IOleInPlaceObject_iface));
}

static ProbeDocument *from_active_object(IOleInPlaceActiveObject *iface)
{
    return (ProbeDocument *)((char *)iface - offsetof(ProbeDocument,
                                                       IOleInPlaceActiveObject_iface));
}

static ProbeDocument *from_html2(IHTMLDocument2 *iface)
{
    return (ProbeDocument *)((char *)iface - offsetof(ProbeDocument, IHTMLDocument2_iface));
}

static ProbeDocument *from_dispatch(IDispatch *iface)
{
    return (ProbeDocument *)((char *)iface - offsetof(ProbeDocument, IDispatch_iface));
}

static ULONG document_add_ref(ProbeDocument *document)
{
    return (ULONG)InterlockedIncrement(&document->references);
}

static ULONG document_release(ProbeDocument *document)
{
    LONG references = InterlockedDecrement(&document->references);
    if(references == 0) {
        document_observe_state(document, FALSE);
        close_supermium_window(document);
        history_clear(document);
        if(document->current_url) SysFreeString(document->current_url);
        SysFreeString(document->notified_title);
        SysFreeString(document->persisted_html);
        SysFreeString(document->window_name);
        SysFreeString(document->observed_url);
        SysFreeString(document->renderer_title);
        SysFreeString(document->history_restart_state);
        document_clear_subscriptions(document);
        if(document->current_moniker) IMoniker_Release(document->current_moniker);
        if(document->client_site) IOleClientSite_Release(document->client_site);
        if(document->view_site) IOleInPlaceSite_Release(document->view_site);
        if(document->in_place_frame) IOleInPlaceFrame_Release(document->in_place_frame);
        if(document->in_place_ui_window) IOleInPlaceUIWindow_Release(document->in_place_ui_window);
        runtime_release(document->runtime);
        HeapFree(GetProcessHeap(), 0, document);
        InterlockedDecrement(&live_objects);
    }
    return (ULONG)references;
}

static HRESULT document_query_interface(ProbeDocument *document, REFIID iid, void **out)
{
    if(!out) return E_POINTER;
    *out = NULL;
    append_guid(L"DOCUMENT_QUERY_IID=", iid);
    if(IsEqualIID(iid, &IID_IHTMLDocument2)) append_text(L"IHTMLDOCUMENT2_REQUESTED\r\n");
    if(IsEqualIID(iid, &IID_IServiceProvider)) append_text(L"ISERVICEPROVIDER_REQUESTED\r\n");
    if(IsEqualIID(iid, &IID_IOleCommandTarget)) append_text(L"IOLECOMMANDTARGET_REQUESTED\r\n");
    if(IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IPersist))
        *out = &document->IPersist_iface;
    else if(IsEqualIID(iid, &IID_IPersistMoniker))
        *out = &document->IPersistMoniker_iface;
    else if(IsEqualIID(iid, &IID_IPersistFile))
        *out = &document->IPersistFile_iface;
    else if(IsEqualIID(iid, &IID_IPersistStreamInit))
        *out = &document->IPersistStreamInit_iface;
    else if(IsEqualIID(iid, &IID_IPersistHistory) || IsEqualIID(iid, &triton_persist_history2_iid))
        *out = &document->IPersistHistory_iface;
    else if(IsEqualIID(iid, &IID_IMonikerProp))
        *out = &document->IMonikerProp_iface;
    else if(IsEqualIID(iid, &IID_IOleObject))
        *out = &document->IOleObject_iface;
    else if(IsEqualIID(iid, &IID_IViewObject))
        *out = &document->IViewObject_iface;
    else if(IsEqualIID(iid, &IID_IOleDocument))
        *out = &document->IOleDocument_iface;
    else if(IsEqualIID(iid, &IID_IServiceProvider))
        *out = &document->IServiceProvider_iface;
    else if(IsEqualIID(iid, &IID_IInternetSecurityManager))
        *out = &document->IInternetSecurityManager_iface;
    else if(IsEqualIID(iid, &IID_IOleCommandTarget))
        *out = &document->IOleCommandTarget_iface;
    else if(IsEqualIID(iid, &IID_IOleInPlaceObject))
        *out = &document->IOleInPlaceObject_iface;
    else if(IsEqualIID(iid, &IID_IOleInPlaceActiveObject))
        *out = &document->IOleInPlaceActiveObject_iface;
    else if(IsEqualIID(iid, &IID_IHTMLDocument) || IsEqualIID(iid, &IID_IHTMLDocument2))
        *out = &document->IHTMLDocument2_iface;
    else if(IsEqualIID(iid, &IID_IHTMLDocument3))
        *out = &document->IHTMLDocument3_iface;
    else if(IsEqualIID(iid, &IID_IDispatch))
        *out = &document->IDispatch_iface;
    else if(IsEqualIID(iid, &IID_IConnectionPointContainer))
        *out = &document->connections_iface;
    else
        return E_NOINTERFACE;
    document_add_ref(document);
    return S_OK;
}

static HRESULT document_get_class_id(CLSID *class_id)
{
    if(!class_id) return E_POINTER;
    *class_id = CLSID_HTMLDocument;
    append_text(L"IPERSIST_GETCLASSID\r\n");
    return S_OK;
}

static BOOL CALLBACK find_supermium_window(HWND window, LPARAM parameter)
{
    ProbeDocument *document = (ProbeDocument *)parameter;
    DWORD process_id = 0;
    GetWindowThreadProcessId(window, &process_id);
    if(process_id != document->supermium_process_id || !IsWindowVisible(window)) return TRUE;
    document->renderer_window = window;
    return FALSE;
}

static BOOL CALLBACK log_host_window(HWND window, LPARAM parameter)
{
    WCHAR class_name[80], line[160];
    RECT rect;
    (void)parameter;
    if(!GetClassNameW(window, class_name, ARRAYSIZE(class_name))) return TRUE;
    wsprintfW(line, L"IE_HOST_CLASS=%ls\r\n", class_name);
    append_text(line);
    if(GetWindowRect(window, &rect)) append_rect(L"IE_HOST_SCREEN_RECT=", &rect);
    return TRUE;
}

static HRESULT layout_supermium_window(ProbeDocument *document, const RECT *rect)
{
    HRGN content_region;
    int width;
    int height;
    int frame_height;

    if(!document->renderer_window || !rect) return E_UNEXPECTED;
    width = rect->right - rect->left;
    height = rect->bottom - rect->top;
    if(width <= 0 || height <= 0) return E_INVALIDARG;
    frame_height = GetSystemMetrics(SM_CYCAPTION) + 2 * GetSystemMetrics(SM_CYFRAME);
    if(frame_height < 1) frame_height = 28;
    if(!SetWindowPos(document->renderer_window, HWND_TOP, rect->left, rect->top - frame_height,
                     width, height + frame_height, SWP_FRAMECHANGED | SWP_NOACTIVATE |
                     (document->view_visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW)))
        return HRESULT_FROM_WIN32(GetLastError());
    content_region = CreateRectRgn(0, frame_height, width, height + frame_height);
    if(!content_region) return HRESULT_FROM_WIN32(GetLastError());
    if(!SetWindowRgn(document->renderer_window, content_region, TRUE)) {
        DeleteObject(content_region);
        return HRESULT_FROM_WIN32(GetLastError());
    }
    append_dword(L"SUPERMIUM_WINDOW_FRAME_TRIM=", (DWORD)frame_height);
    append_text(L"SUPERMIUM_WINDOW_CONTENT_ONLY\r\n");
    return S_OK;
}

static BOOL CALLBACK find_renderer_input(HWND window, LPARAM parameter)
{
    WCHAR name[80];
    if(IsWindowVisible(window) && GetClassNameW(window, name, ARRAYSIZE(name)) &&
       !lstrcmpW(name, L"Chrome_RenderWidgetHostHWND")) {
        *(HWND *)parameter = window;
        return FALSE;
    }
    return TRUE;
}

static void focus_supermium_window(ProbeDocument *document)
{
    if(document->view_visible && document->ui_active && document->renderer_window &&
       IsWindowVisible(document->renderer_window)) {
        HWND input = NULL;
        DWORD current = GetCurrentThreadId(), target;
        GUITHREADINFO info;
        WCHAR focused_class[80];
        ZeroMemory(&info, sizeof(info)); info.cbSize = sizeof(info);
        if(GetGUIThreadInfo(0, &info) && info.hwndFocus &&
           GetClassNameW(info.hwndFocus, focused_class, ARRAYSIZE(focused_class)) &&
           (!lstrcmpW(focused_class, L"Edit") || !lstrcmpW(focused_class, L"RichEdit20W")) &&
           !IsChild(document->renderer_window, info.hwndFocus)) {
            /* Renderer commits and view reactivation are asynchronous. They
             * must not redirect keystrokes out of IE's address/search field. */
            return;
        }
        EnumChildWindows(document->renderer_window, find_renderer_input, (LPARAM)&input);
        if(!input) input = document->renderer_window;
        target = GetWindowThreadProcessId(input, NULL);
        if(document->input_target_thread && document->input_target_thread != target) {
            AttachThreadInput(document->input_owner_thread, document->input_target_thread, FALSE);
            document->input_target_thread = 0;
        }
        if(target != current && !document->input_target_thread) {
            if(!AttachThreadInput(current, target, TRUE)) {
                append_dword(L"SUPERMIUM_FOCUS_ATTACH_ERROR=", GetLastError());
                return;
            }
            document->input_owner_thread = current;
            document->input_target_thread = target;
        }
        SetFocus(input);
        ZeroMemory(&info, sizeof(info));
        info.cbSize = sizeof(info);
        append_dword(L"SUPERMIUM_RENDERER_FOCUS_VERIFIED=",
            GetGUIThreadInfo(target, &info) && info.hwndFocus == input);
    }
}

static void update_host_load_state(ProbeDocument *document)
{
    IOleCommandTarget *target = NULL;
    IOleClientSite *site = NULL;
    TritonDocObjectService *events = NULL;
    unsigned serial;
    BSTR state = NULL;
    VARIANT value;
    HRESULT hr;
    if(document->notifying_host || !document->client_site || !document->runtime->pipe_read) return;
    if(document->next_state_poll &&
       (LONG)(GetTickCount() - document->next_state_poll) < 0) return;
    document->next_state_poll = GetTickCount() + 100;
    document_add_ref(document);
    document->notifying_host = TRUE;
    document_process_events(document);
    document_poll_navigation_state(document);
    document_poll_popups(document);
    if(!document->client_site || !document->runtime->pipe_read) goto done;
    /* The history query classifies a renderer transition before any host
     * download/navigation callbacks are emitted. A renderer traversal is then
     * handed to ITravelLog, whose browser-side path owns those callbacks. */
    if(document->history_request && document->navigation_kind == 3) goto done;
    site = document->client_site;
    IOleClientSite_AddRef(site);
    serial = document->navigation_serial;
    hr = IOleClientSite_QueryInterface(site, &IID_IOleCommandTarget,
                                       (void **)&target);
    if(FAILED(hr)) goto done;
    document_get_browser_events(site, &events);
    VariantInit(&value);
    if(document->host_load_phase != 2) {
        value.vt = VT_I4;
        if(document->host_load_phase == 0) {
            document->host_load_phase = 1;
            value.lVal = 1;
            hr = IOleCommandTarget_Exec(target, NULL, OLECMDID_SETDOWNLOADSTATE,
                                        OLECMDEXECOPT_DONTPROMPTUSER, &value, NULL);
            append_dword(L"MSHTML_HOST_DOWNLOAD_START_RESULT=", (DWORD)hr);
            if(document->navigation_serial != serial || document->client_site != site) goto done;
            if(events) {
                hr = events->lpVtbl->FireDownloadBegin(events);
                append_dword(L"MSHTML_BROWSER_DOWNLOAD_BEGIN=", hr);
            }
        }
        if(document->navigation_serial != serial || document->client_site != site) goto done;
        hr = html2_ready_state(&document->IHTMLDocument2_iface, &state);
        if(document->navigation_serial != serial || document->client_site != site) goto done;
        if(SUCCEEDED(hr) && document->navigation_committed &&
           (!document->history_request || document->navigation_kind != 3) &&
           !document->host_navigate_notified) {
            /* Page.navigate is only an acknowledgement. The loader must
             * commit before IE activates the view and records navigation. */
            document->host_navigate_notified = TRUE;
            if(events) {
                hr = events->lpVtbl->FireNavigateComplete2(events, &document->IHTMLWindow2_iface, 0);
                append_dword(L"MSHTML_BROWSER_NAVIGATE_COMPLETE=", hr);
            }
        }
        if(document->navigation_serial != serial || document->client_site != site) goto done;
        if(state && !lstrcmpW(state, L"complete") &&
           (!document->history_request || document->navigation_kind != 3)) {
            document->host_load_phase = 2;
            value.vt = VT_I4;
            value.lVal = 0;
            hr = IOleCommandTarget_Exec(target, NULL, OLECMDID_SETDOWNLOADSTATE,
                                        OLECMDEXECOPT_DONTPROMPTUSER, &value, NULL);
            append_dword(L"MSHTML_HOST_DOWNLOAD_COMPLETE_RESULT=", (DWORD)hr);
            if(document->navigation_serial != serial || document->client_site != site) goto done;
            if(events) {
                hr = events->lpVtbl->FireDownloadComplete(events);
                append_dword(L"MSHTML_BROWSER_DOWNLOAD_COMPLETE=", hr);
                if(document->navigation_serial != serial || document->client_site != site) goto done;
                if(document->navigation_committed) {
                    hr = events->lpVtbl->FireDocumentComplete(events, &document->IHTMLWindow2_iface, 0);
                    append_dword(L"MSHTML_BROWSER_DOCUMENT_COMPLETE=", hr);
                }
            }
        }
    }
    if(document->navigation_serial != serial || document->client_site != site) goto done;
    document_poll_title(document);
    {
        BSTR title = document->renderer_title ? SysAllocString(document->renderer_title) : NULL;
        if(title &&
           (!document->notified_title || lstrcmpW(title, document->notified_title))) {
            value.vt = VT_BSTR;
            value.bstrVal = title;
            hr = IOleCommandTarget_Exec(target, NULL, OLECMDID_SETTITLE,
                                        OLECMDEXECOPT_DONTPROMPTUSER, &value, NULL);
            if(SUCCEEDED(hr)) {
                SysFreeString(document->notified_title);
                document->notified_title = title;
                title = NULL;
            }
            append_dword(L"MSHTML_HOST_TITLE_RESULT=", (DWORD)hr);
        }
        SysFreeString(title);
    }
done:
    SysFreeString(state);
    if(events) events->lpVtbl->Release(events);
    if(target) IOleCommandTarget_Release(target);
    if(site) IOleClientSite_Release(site);
    document->notifying_host = FALSE;
    document_release(document);
}

static BOOL CALLBACK find_address_edit(HWND window, LPARAM parameter)
{
    WCHAR name[80], parent_name[80];
    HWND parent = GetParent(window);
    if(IsWindowVisible(window) && GetClassNameW(window, name, ARRAYSIZE(name)) &&
       !lstrcmpW(name, L"Edit") && parent &&
       GetClassNameW(parent, parent_name, ARRAYSIZE(parent_name)) &&
       (!lstrcmpW(parent_name, L"ComboBox") || !lstrcmpW(parent_name, L"ComboBoxEx32"))) {
        *(HWND *)parameter = window;
        return FALSE;
    }
    return TRUE;
}

static LRESULT CALLBACK viewport_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    ProbeDocument *document = (ProbeDocument *)GetWindowLongPtrW(window, GWLP_USERDATA);
    RECT rect;
    if(message == WM_NCCREATE) {
        document = ((CREATESTRUCTW *)lparam)->lpCreateParams;
        SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)document);
    }
    if(document && message == MSHTML_FOCUS_ADDRESS_MESSAGE) {
        HWND edit = NULL;
        EnumChildWindows(GetAncestor(window, GA_ROOT), find_address_edit, (LPARAM)&edit);
        if(edit) {
            DWORD thread = GetWindowThreadProcessId(edit, NULL), current = GetCurrentThreadId();
            BOOL attached = thread != current && AttachThreadInput(current, thread, TRUE);
            SetFocus(edit);
            SendMessageW(edit, EM_SETSEL, 0, -1);
            if(attached) AttachThreadInput(current, thread, FALSE);
        }
        return 0;
    }
    if(document && message == WM_TIMER && IsWindow(document->host_window)) {
        rect = document->view_rect;
        if(IsRectEmpty(&rect)) GetClientRect(document->host_window, &rect);
        if(document->view_visible && !IsRectEmpty(&rect)) {
            MapWindowPoints(document->host_window, GetParent(window), (POINT *)&rect, 2);
            SetWindowPos(window, HWND_TOP, rect.left, rect.top, rect.right - rect.left,
                         rect.bottom - rect.top, SWP_NOACTIVATE);
        }
        update_host_load_state(document);
        return 0;
    }
    if(document && message == WM_SIZE && document->renderer_window && GetClientRect(window, &rect)) {
        layout_supermium_window(document, &rect);
        return 0;
    }
    if(message == WM_NCDESTROY) {
        KillTimer(window, 1);
        RemovePropW(window, MSHTML_KEYBOARD_ROUTE_PROPERTY);
        if(document && document->keyboard_router_acquired) {
            document->keyboard_router_acquired = FALSE;
            keyboard_router_release();
        }
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

static HRESULT attach_supermium_window(ProbeDocument *document)
{
    RECT rect;
    RECT host_client_rect;
    HWND candidate;
    HWND renderer_parent = NULL;
    LONG style;
    HWND previous_parent;
    DWORD error;
    HRESULT layout_hr;
    unsigned attempt;

    if(!document->supermium_process_id || !document->host_window) return S_FALSE;
    if(!document->renderer_window) {
        for(attempt = 0; attempt < 20 && !document->renderer_window; ++attempt) {
            EnumWindows(find_supermium_window, (LPARAM)document);
            if(!document->renderer_window) Sleep(100);
        }
    }
    if(!document->renderer_window) {
        append_text(L"SUPERMIUM_WINDOW_NOT_FOUND\r\n");
        return S_FALSE;
    }
    rect = document->view_rect;
    append_rect(L"SUPERMIUM_VIEW_RECT=", &rect);
    SetRectEmpty(&host_client_rect);
    if(GetClientRect(document->host_window, &host_client_rect))
        append_rect(L"SUPERMIUM_HOST_CLIENT_RECT=", &host_client_rect);
    for(candidate = document->host_window; candidate; candidate = GetParent(candidate)) {
        if(GetClientRect(candidate, &host_client_rect) &&
           host_client_rect.right > host_client_rect.left &&
           host_client_rect.bottom > host_client_rect.top) {
            renderer_parent = candidate;
            break;
        }
    }
    if(!renderer_parent) {
        append_text(L"SUPERMIUM_WINDOW_NO_SIZED_PARENT\r\n");
        return S_FALSE;
    }
    append_rect(L"SUPERMIUM_ATTACH_PARENT_RECT=", &host_client_rect);
    log_host_window(renderer_parent, 0);
    EnumChildWindows(renderer_parent, log_host_window, 0);
    if(rect.right <= rect.left || rect.bottom <= rect.top) {
        rect = host_client_rect;
    }
    /* The document host may start at zero size. Use an ancestor only to seed
     * dimensions, never as the parent: otherwise background tabs escape their
     * hidden host and can cover IE's command bands during initial layout. */
    renderer_parent = document->host_window;
    if(!document->viewport_window) {
        WNDCLASSW wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.lpfnWndProc = viewport_proc;
        wc.hInstance = module_instance;
        wc.lpszClassName = L"TritonMshtmlViewport";
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        if(!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return HRESULT_FROM_WIN32(GetLastError());
        document->viewport_window = CreateWindowExW(0, wc.lpszClassName, L"",
            WS_CHILD | (document->view_visible ? WS_VISIBLE : 0) | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
            rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
            renderer_parent, NULL, wc.hInstance, document);
        if(!document->viewport_window) return HRESULT_FROM_WIN32(GetLastError());
        document->keyboard_router_acquired = keyboard_router_acquire();
        if(document->keyboard_router_acquired &&
           !SetPropW(document->viewport_window, MSHTML_KEYBOARD_ROUTE_PROPERTY, (HANDLE)1)) {
            keyboard_router_release();
            document->keyboard_router_acquired = FALSE;
        }
        append_dword(L"MSHTML_KEYBOARD_ROUTER_AVAILABLE=", document->keyboard_router_acquired);
        SetTimer(document->viewport_window, 1, 100, NULL);
    } else if(GetParent(document->viewport_window) != renderer_parent) {
        SetLastError(ERROR_SUCCESS);
        if(!SetParent(document->viewport_window, renderer_parent) && GetLastError())
            return HRESULT_FROM_WIN32(GetLastError());
    }
    renderer_parent = document->viewport_window;
    GetClientRect(renderer_parent, &rect);
    SetLastError(ERROR_SUCCESS);
    previous_parent = SetParent(document->renderer_window, renderer_parent);
    error = GetLastError();
    if(!previous_parent && error != ERROR_SUCCESS) return HRESULT_FROM_WIN32(error);
    Sleep(200);
    style = GetWindowLongW(document->renderer_window, GWL_STYLE);
    SetWindowLongW(document->renderer_window, GWL_STYLE,
                   (style & ~(WS_POPUP | WS_CAPTION | WS_THICKFRAME | WS_SYSMENU |
                              WS_MINIMIZEBOX | WS_MAXIMIZEBOX)) | WS_CHILD);
    layout_hr = layout_supermium_window(document, &rect);
    if(FAILED(layout_hr)) return layout_hr;
    focus_supermium_window(document);
    document->renderer_parent = renderer_parent;
    append_text(L"SUPERMIUM_WINDOW_ATTACHED_TO_IE\r\n");
    return S_OK;
}

static void close_supermium_window(ProbeDocument *document)
{
    document_request_clear(document);
    SysFreeString(document->history_restart_state);
    document->history_restart_state = NULL;
    history_clear(document);
    if(document->page_target[0]) {
        ++document->page_generation;
        char params[128], reply[8192];
        HRESULT hr;
        snprintf(params, sizeof(params), "{\"targetId\":\"%s\"}", document->page_target);
        hr = document_pipe_call(document, "Target.closeTarget", params, NULL, reply, sizeof(reply));
        append_dword(L"MSHTML_TARGET_CLOSE_RESULT=", (DWORD)hr);
        document->page_target[0] = 0;
        if(document->runtime->live_pages) --document->runtime->live_pages;
    }
    cdp_unwatch(document->runtime->cdp, document->page_session);
    document->navigation_request = document->title_request = 0;
    document->popup_scan_request = 0;
    document->next_popup_scan = 0;
    document->navigation_probe_request = document->ready_probe_request = 0;
    document->navigation_wait_ack = document->navigation_committed = FALSE;
    document->navigation_kind = 0;
    document->main_frame[0] = document->observed_loader[0] = document->committed_loader[0] = 0;
    document->lifecycle_loader[0] = 0;
    SysFreeString(document->observed_url); document->observed_url = NULL;
    SysFreeString(document->renderer_title); document->renderer_title = NULL;
    if(document->input_target_thread) {
        AttachThreadInput(document->input_owner_thread, document->input_target_thread, FALSE);
        document->input_target_thread = 0;
    }
    if(document->renderer_window && IsWindow(document->renderer_window)) {
        PostMessageW(document->renderer_window, WM_CLOSE, 0, 0);
        append_text(L"SUPERMIUM_WINDOW_CLOSE_REQUESTED\r\n");
    }
    document->renderer_window = NULL;
    if(document->viewport_window) DestroyWindow(document->viewport_window);
    document->viewport_window = NULL;
    document->renderer_parent = NULL;
    document->supermium_process_id = 0;
    document->page_session[0] = 0;
    if(document->runtime->live_pages) return;
    cdp_stop(document->runtime->cdp);
    document->runtime->cdp = NULL;
    document->runtime->pipe_write = document->runtime->pipe_read = NULL;
    document->page_session[0] = 0;
    if(document->runtime->browser_process) {
        DWORD wait = WaitForSingleObject(document->runtime->browser_process, 5000);
        append_dword(L"MSHTML_BROWSER_EXIT_WAIT=", wait);
        if(wait == WAIT_OBJECT_0) {
            CloseHandle(document->runtime->browser_process);
            document->runtime->browser_process = NULL;
        }
    }
}

static HRESULT remember_url(ProbeDocument *document, LPCWSTR url)
{
    BSTR copy;
    if(!url) return E_INVALIDARG;
    copy = SysAllocString(url);
    if(!copy) return E_OUTOFMEMORY;
    if(document->current_url) SysFreeString(document->current_url);
    document->current_url = copy;
    SysFreeString(document->notified_title);
    document->notified_title = NULL;
    if(document->current_moniker) {
        IMoniker_Release(document->current_moniker);
        document->current_moniker = NULL;
    }
    return S_OK;
}

#include "mshtml_navigation_events.h"

static HRESULT document_pipe_call(ProbeDocument *document, const char *method,
                                  const char *params, const char *session,
                                  char *reply, size_t capacity)
{
    HRESULT hr;
    BOOL failed_reply;
    EnterCriticalSection(&document->runtime->transport_lock);
    hr = cdp_call(document->runtime->cdp, method, params, session, reply, capacity, 5000);
    failed_reply = FAILED(hr) || (reply[0] && mj_member(mj_member(reply, "result"), "exceptionDetails"));
    if(failed_reply) {
        WCHAR failure[256];
        _snwprintf(failure, 256, L"MSHTML_CDP_FAILURE method=%hs hr=%08lx\r\n", method, (unsigned long)hr);
        failure[255] = 0;
        append_text(failure);
    }
    if(failed_reply || (SUCCEEDED(hr) && strncmp(method, "Runtime.", 8) && strncmp(method, "DOM.", 4))) {
        HANDLE trace = CreateFileW(log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if(trace != INVALID_HANDLE_VALUE) {
            DWORD written;
            WriteFile(trace, reply, (DWORD)(failed_reply ? min(strlen(reply), 4096) : strlen(reply)), &written, NULL);
            WriteFile(trace, "\r\n", 2, &written, NULL);
            CloseHandle(trace);
        }
    }
    LeaveCriticalSection(&document->runtime->transport_lock);
    return hr;
}

static HRESULT verify_document_pipe(ProbeDocument *document)
{
    char reply[8192];
    HRESULT hr = document_pipe_call(document, "Browser.getVersion", "{}", NULL, reply, sizeof(reply));
    if(FAILED(hr)) return hr;
    if(!strstr(reply, "\"product\":\"Chrome/144.")) return E_FAIL;
    append_text(L"MSHTML_DOCUMENT_PIPE_VERIFIED\r\n");
    return S_OK;
}

/* Restrict identifiers to Chromium's hexadecimal IDs; ambiguous target sets
 * are rejected until the broker has structured multi-target selection. */
static BOOL pipe_identifier(const char *reply, const char *key, char *out, size_t size)
{
    const char *p = strstr(reply, key);
    size_t used = 0;
    if(!p) return FALSE;
    p += strlen(key);
    while(*p && *p != '"') {
        if(!((*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'F') ||
             (*p >= 'a' && *p <= 'f')) || used + 1 >= size) return FALSE;
        out[used++] = *p++;
    }
    out[used] = 0;
    return used && *p == '"';
}

typedef struct TargetWindowMatch {
    DWORD pid;
    HWND window;
    unsigned count;
} TargetWindowMatch;

static BOOL CALLBACK match_staging_window(HWND window, LPARAM parameter)
{
    TargetWindowMatch *match = (TargetWindowMatch *)parameter;
    DWORD pid;
    RECT rect;
    WCHAR name[80];
    GetWindowThreadProcessId(window, &pid);
    if(pid == match->pid && IsWindowVisible(window) &&
       GetClassNameW(window, name, ARRAYSIZE(name)) && !lstrcmpW(name, L"Chrome_WidgetWin_1") &&
       GetWindowRect(window, &rect) && rect.left == 37 && rect.top == 53 &&
       rect.right == 654 && rect.bottom == 484) {
        match->window = window;
        ++match->count;
    }
    return TRUE;
}

static HRESULT create_app_bootstrap(WCHAR *path, WCHAR *url)
{
    unsigned i;
    /* GetTempFileName creates an empty file. A file URL reliably selects app
     * mode; --app=about:blank can instead create ordinary browser chrome. */
    if(!ensure_state_paths()) return E_ACCESSDENIED;
    if(!GetTempFileNameW(state_directory, L"doc", 0, path))
        return HRESULT_FROM_WIN32(GetLastError());
    wsprintfW(url, L"file:///%ls", path);
    for(i = 0; url[i]; ++i) if(url[i] == L'\\') url[i] = L'/';
    return S_OK;
}

static HRESULT create_app_target(ProbeDocument *document, char *target, size_t capacity)
{
    WCHAR path[MAX_PATH], url[MAX_PATH + 16], command[1024];
    char key[MAX_PATH + 40], narrow[MAX_PATH + 16], reply[8192];
    STARTUPINFOW startup = {0};
    PROCESS_INFORMATION process = {0};
    unsigned attempt;
    HRESULT hr = create_app_bootstrap(path, url);
    if(FAILED(hr)) return hr;
    hr = E_FAIL;
    if(!WideCharToMultiByte(CP_UTF8, 0, url, -1, narrow, sizeof(narrow), NULL, NULL)) goto done;
    snprintf(key, sizeof(key), "\"url\":\"%s\"", narrow);
    wsprintfW(command, L"C:\\TritonSupermium\\chrome.exe --app=\"%ls\" --no-first-run --disable-gpu --user-data-dir=\"%ls\"", url, profile_directory);
    startup.cb = sizeof(startup);
    if(!CreateProcessW(L"C:\\TritonSupermium\\chrome.exe", command, NULL, NULL, FALSE,
                       0, NULL, NULL, &startup, &process)) goto done;
    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, 3000);
    CloseHandle(process.hProcess);
    for(attempt = 0; attempt < 30; ++attempt) {
        const char *object;
        hr = document_pipe_call(document, "Target.getTargets", "{}", NULL, reply, sizeof(reply));
        if(FAILED(hr)) goto done;
        object = strstr(reply, key);
        if(object) {
            while(object > reply && *object != '{') --object;
            if(pipe_identifier(object, "\"targetId\":\"", target, capacity)) {
                char params[256];
                const char *id;
                unsigned window_id, retry;
                TargetWindowMatch match;
                document->supermium_process_id = GetProcessId(document->runtime->browser_process);
                snprintf(params, sizeof(params), "{\"targetId\":\"%s\"}", target);
                hr = document_pipe_call(document, "Browser.getWindowForTarget", params, NULL, reply, sizeof(reply));
                if(FAILED(hr)) goto done;
                id = strstr(reply, "\"windowId\":");
                hr = E_FAIL;
                if(!id || sscanf(id, "\"windowId\":%u", &window_id) != 1) goto done;
                snprintf(params, sizeof(params), "{\"windowId\":%u,\"bounds\":{\"left\":37,\"top\":53,\"width\":617,\"height\":431}}", window_id);
                hr = document_pipe_call(document, "Browser.setWindowBounds", params, NULL, reply, sizeof(reply));
                if(FAILED(hr)) goto done;
                ZeroMemory(&match, sizeof(match));
                match.pid = document->supermium_process_id;
                for(retry = 0; retry < 30; ++retry) {
                    match.count = 0;
                    EnumWindows(match_staging_window, (LPARAM)&match);
                    if(match.count == 1) break;
                    Sleep(100);
                }
                hr = E_FAIL;
                if(match.count == 1) {
                    document->renderer_window = match.window;
                    append_text(L"MSHTML_APP_TARGET_HWND_ASSOCIATED\r\n");
                    hr = S_OK;
                }
                goto done;
            }
        }
        Sleep(100);
    }
    hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
done:
    DeleteFileW(path);
    return hr;
}

#include "mshtml_popup_host.h"

static HRESULT ensure_document_session_unlocked(ProbeDocument *document)
{
    static const char history_hook[] =
        "(()=>{if(window!==top||window.__tritonHistoryHook)return;"
        "const p=Object.getPrototypeOf(history),back=p.back,forward=p.forward,go=p.go;"
        "const nativeGo=v=>Reflect.apply(go,history,[v]);"
        "Object.defineProperty(window,'__tritonHistoryOriginalGo',{value:nativeGo});"
        "Object.defineProperty(p,'back',{configurable:true,writable:true,value:function(){"
        "if(this!==history)return Reflect.apply(back,this,arguments);__tritonHistoryTravel('-1')}});"
        "Object.defineProperty(p,'forward',{configurable:true,writable:true,value:function(){"
        "if(this!==history)return Reflect.apply(forward,this,arguments);__tritonHistoryTravel('1')}});"
        "Object.defineProperty(p,'go',{configurable:true,writable:true,value:function(v){"
        "if(this!==history)return Reflect.apply(go,this,arguments);const n=Number(v);"
        "if(!Number.isFinite(n)||n===0)return nativeGo(v);__tritonHistoryTravel(String(Math.trunc(n)))}});"
        "Object.defineProperty(window,'__tritonHistoryHook',{value:1})})()";
    static const char new_window_hook[] =
        "(()=>{if(window!==top||window.__tritonNewWindowHook)return;"
        "const open=window.open;"
        "Object.defineProperty(window,'open',{configurable:true,writable:true,value:function(u,n,f){"
        "return Reflect.apply(open,this,[u,n,/^_(self|top|parent)$/i.test(n)?f:'popup=yes,'+(f||'')])}});"
        "addEventListener('click',e=>{const a=e.target&&e.target.closest&&e.target.closest('a[target]');"
        "if(e.defaultPrevented||e.button!==0||e.ctrlKey||e.shiftKey||e.altKey||e.metaKey||!a||!a.target||/^_(self|top|parent)$/i.test(a.target))return;"
        "e.preventDefault();Reflect.apply(open,window,[a.href,a.target,'popup=yes,'+(a.relList.contains('noreferrer')?'noreferrer':(a.relList.contains('noopener')||(a.target==='_blank'&&!a.relList.contains('opener'))?'noopener':''))])},false);"
        "Object.defineProperty(window,'__tritonNewWindowHook',{value:1})})()";
    char reply[8192], target[80], params[12000];
    HRESULT hr;
    unsigned attempt;
    if(!document->page_session[0]) {
        hr = CoCreateGuid(&document->history_context);
        if(FAILED(hr)) return hr;
        if(document->runtime->live_pages) {
            hr = create_app_target(document, target, sizeof(target));
            if(FAILED(hr)) return hr;
        } else {
          for(attempt = 0; attempt < 30; ++attempt) {
            const char *first;
            hr = document_pipe_call(document, "Target.getTargets", "{}", NULL, reply, sizeof(reply));
            if(FAILED(hr)) return hr;
            first = strstr(reply, "\"type\":\"page\"");
            if(first && !strstr(first + 1, "\"type\":\"page\"")) {
                /* Chromium emits targetId before type in each target object.
                 * Select that object's ID, not an iframe's adjacent ID. */
                const char *object = first;
                while(object > reply && *object != '{') --object;
                if(*object == '{' && pipe_identifier(object, "\"targetId\":\"",
                                                      target, sizeof(target))) break;
            }
            Sleep(100);
        }
        if(attempt == 30) return E_FAIL;
        }
        snprintf(params, sizeof(params), "{\"targetId\":\"%s\",\"flatten\":true}", target);
        hr = document_pipe_call(document, "Target.attachToTarget", params, NULL, reply, sizeof(reply));
        if(FAILED(hr) || !pipe_identifier(reply, "\"sessionId\":\"", document->page_session,
                                         sizeof(document->page_session))) return E_FAIL;
        append_text(L"MSHTML_PAGE_SESSION_ATTACHED\r\n");
        strcpy(document->page_target, target);
        ++document->runtime->live_pages;
        hr = cdp_watch(document->runtime->cdp, document->page_session);
        if(FAILED(hr)) return hr;
        hr = document_pipe_call(document, "Page.getFrameTree", "{}", document->page_session, reply, sizeof(reply));
        if(FAILED(hr) || !pipe_identifier(reply, "\"id\":\"", document->main_frame,
                                         sizeof(document->main_frame))) return E_FAIL;
        hr = document_pipe_call(document, "Page.enable", "{}", document->page_session, reply, sizeof(reply));
        if(FAILED(hr)) return hr;
        hr = document_pipe_call(document, "Runtime.enable", "{}", document->page_session, reply, sizeof(reply));
        if(FAILED(hr)) return hr;
        hr = document_pipe_call(document, "Runtime.addBinding", "{\"name\":\"__tritonHistoryTravel\"}",
                                document->page_session, reply, sizeof(reply));
        if(FAILED(hr)) return hr;
        hr = document_pipe_call(document, "Runtime.addBinding", "{\"name\":\"__tritonOpenWindow\"}",
                                document->page_session, reply, sizeof(reply));
        if(FAILED(hr)) return hr;
        snprintf(params, sizeof(params), "{\"source\":\"%s\"}", history_hook);
        hr = document_pipe_call(document, "Page.addScriptToEvaluateOnNewDocument", params,
                                document->page_session, reply, sizeof(reply));
        if(FAILED(hr)) return hr;
        snprintf(params, sizeof(params), "{\"source\":\"%s\"}", new_window_hook);
        hr = document_pipe_call(document, "Page.addScriptToEvaluateOnNewDocument", params,
                                document->page_session, reply, sizeof(reply));
        if(FAILED(hr)) return hr;
        /* The first temporary page already has a world; later documents use
         * the registered new-document script. */
        snprintf(params, sizeof(params), "{\"expression\":\"%s\"}", history_hook);
        hr = document_pipe_call(document, "Runtime.evaluate", params,
                                document->page_session, reply, sizeof(reply));
        if(FAILED(hr)) return hr;
        snprintf(params, sizeof(params), "{\"expression\":\"%s\"}", new_window_hook);
        hr = document_pipe_call(document, "Runtime.evaluate", params,
                                document->page_session, reply, sizeof(reply));
        if(FAILED(hr)) return hr;
        hr = document_pipe_call(document, "Page.setLifecycleEventsEnabled", "{\"enabled\":true}",
                                document->page_session, reply, sizeof(reply));
        if(FAILED(hr)) return hr;
    }
    return S_OK;
}

static HRESULT ensure_document_session(ProbeDocument *document)
{
    HRESULT hr;
    EnterCriticalSection(&document->runtime->transport_lock);
    hr = ensure_document_session_unlocked(document);
    LeaveCriticalSection(&document->runtime->transport_lock);
    return hr;
}

static HRESULT navigate_document_pipe(ProbeDocument *document, LPCWSTR url)
{
    char params[12000];
    WCHAR resource_url[1800];
    HRESULT hr;
    LPCWSTR engine_url = url;
    size_t i, length, offset;
    /* IE owns the tab/address chrome; provide only document content for its
     * built-in new-tab URL, which Chromium does not understand. Keep the OLE
     * URL/moniker unchanged rather than exposing this internal data URL.
     * Vista URLMon expands about:Tabs to the exact resource URL below. */
    if(!lstrcmpiW(url, L"about:Tabs") || !wcsnicmp(url, L"res://", 6)) {
        hr = resource_map(document, !lstrcmpiW(url, L"about:Tabs") ?
                          L"res://ieframe.dll/tabswelcome.htm" : url,
                          resource_url, ARRAYSIZE(resource_url));
        if(FAILED(hr)) return hr;
        engine_url = resource_url;
    }
    length = lstrlenW(engine_url);
    if(length > 1600) return E_INVALIDARG;
    hr = ensure_document_session(document);
    if(FAILED(hr)) return hr;
    if(document->resource_origin[0]) {
        hr = resource_enable(document);
        if(FAILED(hr)) return hr;
    }
    strcpy(params, "{\"url\":\"");
    offset = strlen(params);
    for(i = 0; i < length; ++i) {
        snprintf(params + offset, sizeof(params) - offset, "\\u%04x", (unsigned)engine_url[i]);
        offset += 6;
    }
    strcpy(params + offset, "\"}");
    return document_post_navigation(document, "Page.navigate", params, 1, url, engine_url != url);
}

static HRESULT launch_supermium_url(ProbeDocument *document, LPCWSTR url)
{
    WCHAR command[2048];
    WCHAR bootstrap_path[MAX_PATH], bootstrap_url[MAX_PATH + 16];
    STARTUPINFOEXW startup;
    PROCESS_INFORMATION process;
    DWORD exit_code;
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
    HANDLE inherited[2] = {NULL, NULL}, read_pipe = NULL, write_pipe = NULL;
    SIZE_T attribute_size = 0;
    HRESULT launch_error;

    if(document->runtime->browser_process) {
        if(WaitForSingleObject(document->runtime->browser_process, 0) != WAIT_OBJECT_0)
            return HRESULT_FROM_WIN32(ERROR_BUSY);
        CloseHandle(document->runtime->browser_process);
        document->runtime->browser_process = NULL;
    }
    if(lstrlenW(url) > 1600) {
        return HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW);
    }
    launch_error = create_app_bootstrap(bootstrap_path, bootstrap_url);
    if(FAILED(launch_error)) return launch_error;
    ZeroMemory(&startup, sizeof(startup));
    if(!CreatePipe(&inherited[0], &write_pipe, &sa, 0) ||
       !CreatePipe(&read_pipe, &inherited[1], &sa, 0) ||
       !SetHandleInformation(write_pipe, HANDLE_FLAG_INHERIT, 0) ||
       !SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0)) goto pipe_failed;
    InitializeProcThreadAttributeList(NULL, 1, 0, &attribute_size);
    startup.lpAttributeList = HeapAlloc(GetProcessHeap(), 0, attribute_size);
    if(!startup.lpAttributeList) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); goto pipe_failed; }
    if(!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attribute_size)) {
        HeapFree(GetProcessHeap(), 0, startup.lpAttributeList);
        startup.lpAttributeList = NULL;
        goto pipe_failed;
    }
    if(!UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                  inherited, sizeof(inherited), NULL, NULL)) goto pipe_failed;
    wsprintfW(command,
              L"C:\\TritonSupermium\\chrome.exe --app=\"%ls\" --no-first-run --disable-gpu "
              L"--disable-background-networking --disable-component-update --disable-default-apps "
              L"--hide-crash-restore-bubble "
              L"--remote-debugging-pipe --remote-debugging-io-pipes=%lu,%lu "
              L"--user-data-dir=\"%ls\"", bootstrap_url,
              (unsigned long)(ULONG_PTR)inherited[0], (unsigned long)(ULONG_PTR)inherited[1], profile_directory);
    ZeroMemory(&process, sizeof(process));
    startup.StartupInfo.cb = sizeof(startup);
    append_text(L"SUPERMIUM_LAUNCH_REQUESTED\r\n");
    if(!CreateProcessW(L"C:\\TritonSupermium\\chrome.exe", command, NULL, NULL, TRUE,
                       EXTENDED_STARTUPINFO_PRESENT, NULL, NULL, &startup.StartupInfo, &process))
        goto pipe_failed;
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    HeapFree(GetProcessHeap(), 0, startup.lpAttributeList);
    startup.lpAttributeList = NULL;
    CloseHandle(inherited[0]); inherited[0] = NULL;
    CloseHandle(inherited[1]); inherited[1] = NULL;
    append_dword(L"SUPERMIUM_LAUNCH_PID=", process.dwProcessId);
    WaitForInputIdle(process.hProcess, 1000);
    if(GetExitCodeProcess(process.hProcess, &exit_code)) {
        append_dword(L"SUPERMIUM_LAUNCH_PROCESS_STATE=", exit_code);
        if(exit_code != STILL_ACTIVE) {
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            append_text(L"SUPERMIUM_LAUNCH_HANDOFF_UNRESOLVED\r\n");
            SetLastError(ERROR_PROCESS_ABORTED);
            goto pipe_failed;
        }
    }
    document->supermium_process_id = process.dwProcessId;
    cdp_stop(document->runtime->cdp);
    document->runtime->cdp = cdp_start(read_pipe, write_pipe);
    document->runtime->pipe_read = read_pipe;
    document->runtime->pipe_write = write_pipe;
    document->page_session[0] = 0;
    document->renderer_window = NULL;
    CloseHandle(process.hThread);
    document->runtime->browser_process = process.hProcess;
    launch_error = document->runtime->cdp ? verify_document_pipe(document) : E_OUTOFMEMORY;
    if(FAILED(launch_error)) {
        if(document->runtime->cdp) cdp_stop(document->runtime->cdp);
        else { CloseHandle(read_pipe); CloseHandle(write_pipe); }
        document->runtime->cdp = NULL;
        document->runtime->pipe_read = document->runtime->pipe_write = NULL;
        DeleteFileW(bootstrap_path);
        return launch_error;
    }
    /* --app may substitute New Tab for about:blank. The document's first
     * navigation must use the same acknowledged transport as later ones. */
    launch_error = navigate_document_pipe(document, url);
    DeleteFileW(bootstrap_path);
    if(FAILED(launch_error)) return launch_error;
    append_text(L"SUPERMIUM_LAUNCH_SUCCEEDED\r\n");
    return S_OK;
pipe_failed:
    launch_error = HRESULT_FROM_WIN32(GetLastError());
    DeleteFileW(bootstrap_path);
    if(startup.lpAttributeList) {
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        HeapFree(GetProcessHeap(), 0, startup.lpAttributeList);
    }
    if(inherited[0]) CloseHandle(inherited[0]);
    if(inherited[1]) CloseHandle(inherited[1]);
    if(read_pipe) CloseHandle(read_pipe);
    if(write_pipe) CloseHandle(write_pipe);
    return launch_error;
}

static HRESULT navigate_document_url(ProbeDocument *document, LPCWSTR url)
{
    HRESULT hr;
    if(!url || !*url) return E_INVALIDARG;
    if(lstrlenW(url) > 1600) return E_INVALIDARG;
    append_text(L"MSHTML_NAVIGATE_URL=");
    append_text(url);
    append_text(L"\r\n");
    if(document->runtime->pipe_read && document->runtime->pipe_write) {
        hr = navigate_document_pipe(document, url);
    } else {
        if(document->renderer_window) close_supermium_window(document);
        hr = launch_supermium_url(document, url);
    }
    if(SUCCEEDED(hr) && document->in_place_active) {
        hr = attach_supermium_window(document);
        append_dword(L"MSHTML_NAVIGATION_ATTACH_RESULT=", (DWORD)hr);
    }
    return hr;
}

static HRESULT launch_supermium(ProbeDocument *document, IMoniker *moniker, LPBC bind_context)
{
    WCHAR *url = NULL;
    HRESULT hr;
    if(!moniker) return E_INVALIDARG;
    hr = IMoniker_GetDisplayName(moniker, bind_context, NULL, &url);
    if(FAILED(hr) || !url) return FAILED(hr) ? hr : E_FAIL;
    hr = navigate_document_url(document, url);
    CoTaskMemFree(url);
    return hr;
}

static HRESULT html2_unimplemented(IHTMLDocument2 *iface, const WCHAR *method)
{
    WCHAR line[160];
    (void)iface;
    wsprintfW(line, L"IHTMLDOCUMENT2_STUB_METHOD=%ls\r\n", method);
    append_text(line);
    return E_NOTIMPL;
}

static HRESULT html2_ready_state(IHTMLDocument2 *iface, BSTR *state)
{
    ProbeDocument *document = from_html2(iface);
    static const WCHAR *const values[] = {L"uninitialized", L"loading", L"loaded", L"interactive", L"complete"};
    LONG value;
    if(!state) return E_POINTER;
    *state = NULL;
    document_process_events(document);
    document_poll_navigation_state(document);
    document_poll_popups(document);
    value = document->ready_state;
    if(value < 0 || value > READYSTATE_COMPLETE) return E_UNEXPECTED;
    *state = SysAllocString(values[value]);
    return *state ? S_OK : E_OUTOFMEMORY;
}

static HRESULT html2_invoke(IHTMLDocument2 *iface, DISPID member, WORD flags, VARIANT *result)
{
    HRESULT hr;
    if(result) VariantInit(result);
    if(member != DISPID_READYSTATE || !(flags & DISPATCH_PROPERTYGET) || !result)
        return E_NOTIMPL;
    hr = html2_ready_state(iface, &result->bstrVal);
    if(FAILED(hr)) return hr;
    result->vt = VT_BSTR;
    if(!lstrcmpW(result->bstrVal, L"complete"))
        append_text(L"IHTMLDOCUMENT2_INVOKE_READYSTATE=complete\r\n");
    return S_OK;
}

static HRESULT html2_put_url(IHTMLDocument2 *iface, BSTR url)
{
    append_text(L"IHTMLDOCUMENT2_PUT_URL\r\n");
    return navigate_document_url(from_html2(iface), url);
}

static HRESULT html2_get_url(IHTMLDocument2 *iface, BSTR *url)
{
    ProbeDocument *document = from_html2(iface);
    if(!url) return E_POINTER;
    *url = SysAllocString(document->current_url ? document->current_url : L"about:blank");
    if(!*url) return E_OUTOFMEMORY;
    append_text(L"IHTMLDOCUMENT2_GET_URL\r\n");
    return S_OK;
}

#include "mshtml_document_content.h"
#include "mshtml_history.h"
#include "mshtml_element_proxy.h"
#include "mshtml_host_contract.h"
#include "mshtml_resource_adapter.h"
#include "../build/ie7_ihtmldocument2_stubs.inc"

static HRESULT STDMETHODCALLTYPE factory_query_interface(IClassFactory *iface, REFIID iid,
                                                          void **out)
{
    if(!out) return E_POINTER;
    *out = NULL;
    append_guid(L"FACTORY_QUERY_IID=", iid);
    if(IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IClassFactory)) {
        *out = iface;
        factory_add_ref(iface);
        return S_OK;
    }
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE factory_add_ref(IClassFactory *iface)
{
    return (ULONG)InterlockedIncrement(&((ProbeFactory *)iface)->references);
}

static ULONG STDMETHODCALLTYPE factory_release(IClassFactory *iface)
{
    ProbeFactory *factory = (ProbeFactory *)iface;
    LONG references = InterlockedDecrement(&factory->references);
    if(references == 0) {
        HeapFree(GetProcessHeap(), 0, factory);
        InterlockedDecrement(&live_objects);
    }
    return (ULONG)references;
}

static HRESULT STDMETHODCALLTYPE factory_create_instance(IClassFactory *iface, IUnknown *outer,
                                                         REFIID iid, void **out)
{
    ProbeDocument *document;
    HRESULT hr;
    (void)iface;
    if(!out) return E_POINTER;
    *out = NULL;
    append_guid(L"CREATE_INSTANCE_IID=", iid);
    append_text(L"ACTIVEX_POLICY=unsupported-stubbed\r\n");
    if(outer) return CLASS_E_NOAGGREGATION;
    document = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*document));
    if(!document) return E_OUTOFMEMORY;
    document->runtime = runtime_acquire();
    if(!document->runtime) {
        HeapFree(GetProcessHeap(), 0, document);
        return E_OUTOFMEMORY;
    }
    InterlockedIncrement(&live_objects);
    document->IPersist_iface.lpVtbl = &persist_vtbl;
    document->IPersistMoniker_iface.lpVtbl = &moniker_vtbl;
    document->IPersistFile_iface.lpVtbl = &file_vtbl;
    document->IPersistStreamInit_iface.lpVtbl = &content_stream_vtbl;
    document->IPersistHistory_iface.lpVtbl = &history_vtbl.base;
    document->IMonikerProp_iface.lpVtbl = &moniker_prop_vtbl;
    document->IOleObject_iface.lpVtbl = &ole_vtbl;
    document->IViewObject_iface.lpVtbl = &view_vtbl;
    document->IOleDocument_iface.lpVtbl = &ole_document_vtbl;
    document->IServiceProvider_iface.lpVtbl = &service_provider_vtbl;
    document->IInternetSecurityManager_iface.lpVtbl = &security_manager_vtbl;
    document->IOleCommandTarget_iface.lpVtbl = &command_target_vtbl;
    document->IOleDocumentView_iface.lpVtbl = &document_view_vtbl;
    document->IOleInPlaceObject_iface.lpVtbl = &in_place_object_vtbl;
    document->IOleInPlaceActiveObject_iface.lpVtbl = &active_object_vtbl;
    document->IHTMLDocument2_iface.lpVtbl = &document2_vtbl;
    document->IHTMLDocument3_iface.lpVtbl = &document3_vtbl;
    document->IDispatch_iface.lpVtbl = &dispatch_vtbl;
    document->IHTMLWindow2_iface.lpVtbl = &window2_vtbl;
    document->private_window_iface.lpVtbl = &private_window_vtbl;
    document->connections_iface.lpVtbl = &connections_vtbl;
    document->property_point_iface.lpVtbl = &property_point_vtbl;
    document->references = 1;
    append_text(L"PROBE_INTERFACE_MODE=persist-documentview-command-stubs\r\n");
    hr = document_query_interface(document, iid, out);
    document_release(document);
    return hr;
}

static HRESULT STDMETHODCALLTYPE factory_lock_server(IClassFactory *iface, BOOL lock)
{
    (void)iface;
    if(lock) InterlockedIncrement(&server_locks);
    else InterlockedDecrement(&server_locks);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE persist_query_interface(IPersist *iface, REFIID iid, void **out)
{
    return document_query_interface(from_persist(iface), iid, out);
}
static ULONG STDMETHODCALLTYPE persist_add_ref(IPersist *iface) { return document_add_ref(from_persist(iface)); }
static ULONG STDMETHODCALLTYPE persist_release(IPersist *iface) { return document_release(from_persist(iface)); }
static HRESULT STDMETHODCALLTYPE persist_get_class_id(IPersist *iface, CLSID *class_id)
{
    (void)iface;
    return document_get_class_id(class_id);
}

static HRESULT STDMETHODCALLTYPE moniker_query_interface(IPersistMoniker *iface, REFIID iid, void **out)
{
    return document_query_interface(from_moniker(iface), iid, out);
}
static ULONG STDMETHODCALLTYPE moniker_add_ref(IPersistMoniker *iface) { return document_add_ref(from_moniker(iface)); }
static ULONG STDMETHODCALLTYPE moniker_release(IPersistMoniker *iface) { return document_release(from_moniker(iface)); }
static HRESULT STDMETHODCALLTYPE moniker_get_class_id(IPersistMoniker *iface, CLSID *class_id)
{
    (void)iface;
    return document_get_class_id(class_id);
}
static HRESULT STDMETHODCALLTYPE moniker_is_dirty(IPersistMoniker *iface)
{
    (void)iface;
    append_text(L"IPERSISTMONIKER_ISDIRTY\r\n");
    return S_FALSE;
}
static HRESULT STDMETHODCALLTYPE moniker_load(IPersistMoniker *iface, BOOL fully_available,
                                              IMoniker *moniker, LPBC bind_context, DWORD mode)
{
    HRESULT hr;
    (void)fully_available;
    append_dword(L"IPERSISTMONIKER_LOAD_MODE=", mode);
    hr = launch_supermium(from_moniker(iface), moniker, bind_context);
    if(SUCCEEDED(hr)) {
        IMoniker_AddRef(moniker);
        if(from_moniker(iface)->current_moniker) IMoniker_Release(from_moniker(iface)->current_moniker);
        from_moniker(iface)->current_moniker = moniker;
        append_text(L"IPERSISTMONIKER_LOAD_SUPERMIUM=complete\r\n");
    }
    return hr;
}
static HRESULT STDMETHODCALLTYPE moniker_save(IPersistMoniker *iface, IMoniker *moniker,
                                              LPBC bind_context, BOOL remember)
{
    (void)iface;
    (void)moniker;
    (void)bind_context;
    (void)remember;
    append_text(L"IPERSISTMONIKER_SAVE\r\n");
    return E_NOTIMPL;
}
static HRESULT STDMETHODCALLTYPE moniker_save_completed(IPersistMoniker *iface, IMoniker *moniker,
                                                        LPBC bind_context)
{
    (void)iface;
    (void)moniker;
    (void)bind_context;
    append_text(L"IPERSISTMONIKER_SAVECOMPLETED\r\n");
    return E_NOTIMPL;
}
static HRESULT STDMETHODCALLTYPE moniker_get_cur_moniker(IPersistMoniker *iface, IMoniker **moniker)
{
    ProbeDocument *document = from_moniker(iface);
    if(!moniker) return E_INVALIDARG;
    *moniker = NULL;
    append_text(L"IPERSISTMONIKER_GETCURMONIKER\r\n");
    if(!document->current_moniker) return E_UNEXPECTED;
    IMoniker_AddRef(document->current_moniker);
    *moniker = document->current_moniker;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE file_query_interface(IPersistFile *iface, REFIID iid, void **out)
{
    return document_query_interface(from_file(iface), iid, out);
}
static ULONG STDMETHODCALLTYPE file_add_ref(IPersistFile *iface) { return document_add_ref(from_file(iface)); }
static ULONG STDMETHODCALLTYPE file_release(IPersistFile *iface) { return document_release(from_file(iface)); }
static HRESULT STDMETHODCALLTYPE file_get_class_id(IPersistFile *iface, CLSID *class_id)
{
    (void)iface;
    return document_get_class_id(class_id);
}
static HRESULT STDMETHODCALLTYPE file_is_dirty(IPersistFile *iface)
{
    (void)iface;
    append_text(L"IPERSISTFILE_ISDIRTY\r\n");
    return S_FALSE;
}
static HRESULT STDMETHODCALLTYPE file_load(IPersistFile *iface, LPCOLESTR file_name, DWORD mode)
{
    (void)iface;
    (void)file_name;
    append_dword(L"IPERSISTFILE_LOAD_MODE=", mode);
    append_text(L"IPERSISTFILE_LOAD_STUBBED\r\n");
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE file_save(IPersistFile *iface, LPCOLESTR file_name, BOOL remember)
{
    (void)iface;
    (void)file_name;
    (void)remember;
    append_text(L"IPERSISTFILE_SAVE\r\n");
    return E_NOTIMPL;
}
static HRESULT STDMETHODCALLTYPE file_save_completed(IPersistFile *iface, LPCOLESTR file_name)
{
    (void)iface;
    (void)file_name;
    append_text(L"IPERSISTFILE_SAVECOMPLETED\r\n");
    return E_NOTIMPL;
}
static HRESULT STDMETHODCALLTYPE file_get_cur_file(IPersistFile *iface, LPOLESTR *file_name)
{
    (void)iface;
    if(!file_name) return E_POINTER;
    *file_name = NULL;
    append_text(L"IPERSISTFILE_GETCURFILE\r\n");
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE moniker_prop_query_interface(IMonikerProp *iface, REFIID iid,
                                                               void **out)
{
    return document_query_interface(from_moniker_prop(iface), iid, out);
}
static ULONG STDMETHODCALLTYPE moniker_prop_add_ref(IMonikerProp *iface)
{
    return document_add_ref(from_moniker_prop(iface));
}
static ULONG STDMETHODCALLTYPE moniker_prop_release(IMonikerProp *iface)
{
    return document_release(from_moniker_prop(iface));
}
static HRESULT STDMETHODCALLTYPE moniker_prop_put_property(IMonikerProp *iface,
                                                            MONIKERPROPERTY property,
                                                            LPCWSTR value)
{
    (void)iface;
    (void)value;
    append_dword(L"IMONIKERPROP_PROPERTY=", (DWORD)property);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ole_query_interface(IOleObject *iface, REFIID iid, void **out)
{
    return document_query_interface(from_ole(iface), iid, out);
}
static ULONG STDMETHODCALLTYPE ole_add_ref(IOleObject *iface) { return document_add_ref(from_ole(iface)); }
static ULONG STDMETHODCALLTYPE ole_release(IOleObject *iface) { return document_release(from_ole(iface)); }
static HRESULT STDMETHODCALLTYPE ole_set_client_site(IOleObject *iface, IOleClientSite *client_site)
{
    ProbeDocument *document = from_ole(iface);
    if(document->client_site != client_site) {
        ++document->navigation_serial;
        document_host_navigation(document, document->client_site, FALSE);
        SysFreeString(document->notified_title);
        document->notified_title = NULL;
        document->host_load_phase = 0;
    }
    if(client_site) IOleClientSite_AddRef(client_site);
    if(document->client_site) IOleClientSite_Release(document->client_site);
    document->client_site = client_site;
    append_text(L"IOLEOBJECT_SETCLIENTSITE\r\n");
    if(FAILED(document_observe_state(document, client_site != NULL))) return E_FAIL;
    document_host_navigation(document, client_site, TRUE);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE ole_get_client_site(IOleObject *iface, IOleClientSite **client_site)
{
    ProbeDocument *document = from_ole(iface);
    if(!client_site) return E_POINTER;
    *client_site = document->client_site;
    if(*client_site) IOleClientSite_AddRef(*client_site);
    append_text(L"IOLEOBJECT_GETCLIENTSITE\r\n");
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE ole_set_host_names(IOleObject *iface, LPCOLESTR application,
                                                    LPCOLESTR object)
{
    (void)iface;
    (void)application;
    (void)object;
    append_text(L"IOLEOBJECT_SETHOSTNAMES\r\n");
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE ole_close(IOleObject *iface, DWORD save_option)
{
    ProbeDocument *document = from_ole(iface);
    if(save_option > OLECLOSE_PROMPTSAVE) return E_INVALIDARG;
    append_dword(L"IOLEOBJECT_CLOSE=", save_option);
    ++document->navigation_serial;
    ++document->navigation_cookie;
    document_observe_state(document, FALSE);
    document_host_navigation(document, document->client_site, FALSE);
    in_place_object_deactivate(&document->IOleInPlaceObject_iface);
    close_supermium_window(document);
    document->in_place_active = document->ui_active = document->view_visible = FALSE;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE ole_set_moniker(IOleObject *iface, DWORD which_moniker,
                                                 IMoniker *moniker)
{
    (void)iface;
    (void)moniker;
    append_dword(L"IOLEOBJECT_SETMONIKER=", which_moniker);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE ole_get_moniker(IOleObject *iface, DWORD assign,
                                                 DWORD which_moniker, IMoniker **moniker)
{
    (void)iface;
    (void)assign;
    (void)which_moniker;
    if(!moniker) return E_POINTER;
    *moniker = NULL;
    append_text(L"IOLEOBJECT_GETMONIKER\r\n");
    return E_NOTIMPL;
}
static HRESULT STDMETHODCALLTYPE ole_init_from_data(IOleObject *iface, IDataObject *data,
                                                    BOOL creation, DWORD reserved)
{
    (void)iface;
    (void)data;
    (void)creation;
    (void)reserved;
    append_text(L"IOLEOBJECT_INITFROMDATA\r\n");
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE ole_get_clipboard_data(IOleObject *iface, DWORD reserved,
                                                        IDataObject **data)
{
    (void)iface;
    (void)reserved;
    if(!data) return E_POINTER;
    *data = NULL;
    append_text(L"IOLEOBJECT_GETCLIPBOARDDATA\r\n");
    return E_NOTIMPL;
}

static HRESULT ensure_in_place(ProbeDocument *document)
{
    IOleInPlaceSite *site = document->view_site;
    IOleInPlaceFrame *frame = NULL;
    IOleInPlaceUIWindow *document_window = NULL;
    OLEINPLACEFRAMEINFO frame_info;
    RECT position;
    RECT clip;
    HRESULT hr;
    if(document->in_place_active) return S_OK;
    if(!site) return E_UNEXPECTED;
    hr = IOleInPlaceSite_CanInPlaceActivate(site);
    append_dword(L"IOLEOBJECT_CANINPLACEACTIVATE=", (DWORD)hr);
    if(hr != S_OK) return FAILED(hr) ? hr : E_FAIL;
    hr = IOleInPlaceSite_OnInPlaceActivate(site);
    append_dword(L"IOLEOBJECT_ONINPLACEACTIVATE=", (DWORD)hr);
    if(FAILED(hr)) return hr;
    document->in_place_active = TRUE;
    hr = IOleInPlaceSite_GetWindow(site, &document->host_window);
    if(FAILED(hr) || !IsWindow(document->host_window)) {
        hr = E_FAIL;
        goto deactivate;
    }
    ZeroMemory(&frame_info, sizeof(frame_info));
    frame_info.cb = sizeof(frame_info);
    SetRectEmpty(&position);
    SetRectEmpty(&clip);
    hr = IOleInPlaceSite_GetWindowContext(site, &frame, &document_window, &position, &clip,
                                          &frame_info);
    append_dword(L"IOLEOBJECT_GETWINDOWCONTEXT=", (DWORD)hr);
    if(FAILED(hr)) goto deactivate;
    document->in_place_frame = frame;
    document->in_place_ui_window = document_window;
    if(IsRectEmpty(&document->view_rect)) document->view_rect = position;
    return S_OK;

deactivate:
    document->in_place_active = FALSE;
    IOleInPlaceSite_OnInPlaceDeactivate(site);
    if(document_window) IOleInPlaceUIWindow_Release(document_window);
    if(frame) IOleInPlaceFrame_Release(frame);
    return hr;
}

static HRESULT activate_in_place(ProbeDocument *document, IOleClientSite *client_site,
                                 HWND parent, LPCRECT rectangle)
{
    IOleInPlaceSite *site = NULL;
    IOleDocumentSite *document_site = NULL;
    HRESULT hr;
    (void)parent;
    if(client_site) {
        hr = ole_set_client_site(&document->IOleObject_iface, client_site);
        if(FAILED(hr)) return hr;
    }
    /* A DocObject container owns view creation/activation. Native IE's
     * ActivateMe -> _CreateMsoView queries the view's IOleCommandTarget and
     * retains it for ExecDown. Activating our HWND directly bypassed that
     * handshake, leaving native Refresh/Stop with a null receiver. */
    if(document->client_site) {
        hr = IOleClientSite_QueryInterface(document->client_site, &IID_IOleDocumentSite,
                                           (void **)&document_site);
        if(SUCCEEDED(hr)) {
            if(document->activating_document_site) {
                IOleDocumentSite_Release(document_site);
                return E_UNEXPECTED;
            }
            document->activating_document_site = TRUE;
            hr = IOleDocumentSite_ActivateMe(document_site, &document->IOleDocumentView_iface);
            document->activating_document_site = FALSE;
            IOleDocumentSite_Release(document_site);
            append_dword(L"MSHTML_DOCUMENT_SITE_ACTIVATE_RESULT=", hr);
            return hr;
        }
        if(hr != E_NOINTERFACE) return hr;
    }
    if(!document->view_site) {
        if(!document->client_site) return E_NOINTERFACE;
        hr = IOleClientSite_QueryInterface(document->client_site, &IID_IOleInPlaceSite,
                                           (void **)&site);
        if(FAILED(hr)) return hr;
        document_view_set_in_place_site(&document->IOleDocumentView_iface, site);
        IOleInPlaceSite_Release(site);
    }
    hr = document_view_ui_activate(&document->IOleDocumentView_iface, TRUE);
    if(FAILED(hr)) return hr;
    if(rectangle) document_view_set_rect(&document->IOleDocumentView_iface, (LPRECT)rectangle);
    hr = document_view_show(&document->IOleDocumentView_iface, TRUE);
    if(FAILED(hr)) in_place_object_deactivate(&document->IOleInPlaceObject_iface);
    else {
        focus_supermium_window(document);
        append_text(L"IOLEOBJECT_INPLACE_ACTIVATED\r\n");
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE ole_do_verb(IOleObject *iface, LONG verb, LPMSG message,
                                             IOleClientSite *client_site, LONG index, HWND parent,
                                             LPCRECT rectangle)
{
    ProbeDocument *document = from_ole(iface);
    HRESULT hr;
    (void)message;
    (void)index;
    append_dword(L"IOLEOBJECT_DOVERB=", (DWORD)verb);
    if(verb == OLEIVERB_HIDE)
        return in_place_object_deactivate(&document->IOleInPlaceObject_iface);
    if(verb != OLEIVERB_PRIMARY && verb != OLEIVERB_SHOW &&
       verb != OLEIVERB_UIACTIVATE && verb != OLEIVERB_INPLACEACTIVATE)
        return OLEOBJ_E_INVALIDVERB;
    document_add_ref(document);
    hr = activate_in_place(document, client_site, parent, rectangle);
    append_dword(L"IOLEOBJECT_DOVERB_RESULT=", (DWORD)hr);
    document_release(document);
    return hr;
}
static HRESULT STDMETHODCALLTYPE ole_enum_verbs(IOleObject *iface, IEnumOLEVERB **verbs)
{
    (void)iface;
    if(!verbs) return E_POINTER;
    *verbs = NULL;
    append_text(L"IOLEOBJECT_ENUMVERBS\r\n");
    return E_NOTIMPL;
}
static HRESULT STDMETHODCALLTYPE ole_update(IOleObject *iface)
{
    (void)iface;
    append_text(L"IOLEOBJECT_UPDATE\r\n");
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE ole_is_up_to_date(IOleObject *iface)
{
    (void)iface;
    append_text(L"IOLEOBJECT_ISUPTODATE\r\n");
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE ole_get_user_class_id(IOleObject *iface, CLSID *class_id)
{
    (void)iface;
    return document_get_class_id(class_id);
}
static HRESULT STDMETHODCALLTYPE ole_get_user_type(IOleObject *iface, DWORD form,
                                                   LPOLESTR *user_type)
{
    (void)iface;
    (void)form;
    if(!user_type) return E_POINTER;
    *user_type = NULL;
    append_text(L"IOLEOBJECT_GETUSERTYPE\r\n");
    return E_NOTIMPL;
}
static HRESULT STDMETHODCALLTYPE ole_set_extent(IOleObject *iface, DWORD aspect, SIZEL *size)
{
    (void)iface;
    (void)aspect;
    (void)size;
    append_text(L"IOLEOBJECT_SETEXTENT\r\n");
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE ole_get_extent(IOleObject *iface, DWORD aspect, SIZEL *size)
{
    (void)iface;
    (void)aspect;
    if(!size) return E_POINTER;
    size->cx = 8000;
    size->cy = 6000;
    append_text(L"IOLEOBJECT_GETEXTENT\r\n");
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE ole_advise(IOleObject *iface, IAdviseSink *sink, DWORD *connection)
{
    (void)iface;
    (void)sink;
    if(connection) *connection = 0;
    append_text(L"IOLEOBJECT_ADVISE\r\n");
    return OLE_E_ADVISENOTSUPPORTED;
}
static HRESULT STDMETHODCALLTYPE ole_unadvise(IOleObject *iface, DWORD connection)
{
    (void)iface;
    (void)connection;
    append_text(L"IOLEOBJECT_UNADVISE\r\n");
    return OLE_E_NOCONNECTION;
}
static HRESULT STDMETHODCALLTYPE ole_enum_advise(IOleObject *iface, IEnumSTATDATA **enumerator)
{
    (void)iface;
    if(!enumerator) return E_POINTER;
    *enumerator = NULL;
    append_text(L"IOLEOBJECT_ENUMADVISE\r\n");
    return OLE_E_ADVISENOTSUPPORTED;
}
static HRESULT STDMETHODCALLTYPE ole_get_misc_status(IOleObject *iface, DWORD aspect, DWORD *status)
{
    (void)iface;
    (void)aspect;
    if(!status) return E_POINTER;
    *status = 0;
    append_text(L"IOLEOBJECT_GETMISCSTATUS\r\n");
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE ole_set_color_scheme(IOleObject *iface, LOGPALETTE *palette)
{
    (void)iface;
    (void)palette;
    append_text(L"IOLEOBJECT_SETCOLORSCHEME\r\n");
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE view_query_interface(IViewObject *iface, REFIID iid, void **out)
{
    return document_query_interface(from_view(iface), iid, out);
}
static ULONG STDMETHODCALLTYPE view_add_ref(IViewObject *iface) { return document_add_ref(from_view(iface)); }
static ULONG STDMETHODCALLTYPE view_release(IViewObject *iface) { return document_release(from_view(iface)); }
static HRESULT STDMETHODCALLTYPE view_draw(IViewObject *iface, DWORD aspect, LONG index,
                                           void *aspect_info, DVTARGETDEVICE *target,
                                           HDC target_hdc, HDC draw_hdc, LPCRECTL bounds,
                                           LPCRECTL w_bounds,
                                           BOOL (STDMETHODCALLTYPE *continue_fn)(ULONG_PTR),
                                           ULONG_PTR continue_data)
{
    (void)iface;
    (void)aspect;
    (void)index;
    (void)aspect_info;
    (void)target;
    (void)target_hdc;
    (void)draw_hdc;
    (void)bounds;
    (void)w_bounds;
    (void)continue_fn;
    (void)continue_data;
    append_text(L"IVIEWOBJECT_DRAW_STUBBED\r\n");
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE view_get_color_set(IViewObject *iface, DWORD aspect, LONG index,
                                                    void *aspect_info, DVTARGETDEVICE *target,
                                                    HDC target_hdc, LOGPALETTE **palette)
{
    (void)iface;
    (void)aspect;
    (void)index;
    (void)aspect_info;
    (void)target;
    (void)target_hdc;
    if(!palette) return E_POINTER;
    *palette = NULL;
    append_text(L"IVIEWOBJECT_GETCOLORSET\r\n");
    return E_NOTIMPL;
}
static HRESULT STDMETHODCALLTYPE view_freeze(IViewObject *iface, DWORD aspect, LONG index,
                                             void *aspect_info, DWORD *freeze)
{
    (void)iface;
    (void)aspect;
    (void)index;
    (void)aspect_info;
    if(!freeze) return E_POINTER;
    *freeze = 0;
    append_text(L"IVIEWOBJECT_FREEZE\r\n");
    return E_NOTIMPL;
}
static HRESULT STDMETHODCALLTYPE view_unfreeze(IViewObject *iface, DWORD freeze)
{
    (void)iface;
    (void)freeze;
    append_text(L"IVIEWOBJECT_UNFREEZE\r\n");
    return E_NOTIMPL;
}
static HRESULT STDMETHODCALLTYPE view_set_advise(IViewObject *iface, DWORD aspects, DWORD flags,
                                                 IAdviseSink *sink)
{
    (void)iface;
    (void)aspects;
    (void)flags;
    (void)sink;
    append_text(L"IVIEWOBJECT_SETADVISE\r\n");
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE view_get_advise(IViewObject *iface, DWORD *aspects, DWORD *flags,
                                                 IAdviseSink **sink)
{
    (void)iface;
    if(aspects) *aspects = 0;
    if(flags) *flags = 0;
    if(sink) *sink = NULL;
    append_text(L"IVIEWOBJECT_GETADVISE\r\n");
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE ole_document_query_interface(IOleDocument *iface, REFIID iid,
                                                               void **out)
{
    return document_query_interface(from_ole_document(iface), iid, out);
}
static ULONG STDMETHODCALLTYPE ole_document_add_ref(IOleDocument *iface)
{
    return document_add_ref(from_ole_document(iface));
}
static ULONG STDMETHODCALLTYPE ole_document_release(IOleDocument *iface)
{
    return document_release(from_ole_document(iface));
}
static HRESULT STDMETHODCALLTYPE ole_document_create_view(IOleDocument *iface,
                                                          IOleInPlaceSite *site, IStream *stream,
                                                          DWORD reserved, IOleDocumentView **view)
{
    ProbeDocument *document = from_ole_document(iface);
    HRESULT hr;
    (void)stream;
    (void)reserved;
    if(!view) return E_POINTER;
    *view = NULL;
    append_text(L"IOLEDOCUMENT_CREATEVIEW\r\n");
    hr = document_view_set_in_place_site(&document->IOleDocumentView_iface, site);
    if(FAILED(hr)) return hr;
    document_add_ref(document);
    *view = &document->IOleDocumentView_iface;
    append_text(L"IOLEDOCUMENT_CREATEVIEW_SUCCEEDED\r\n");
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE ole_document_get_misc_status(IOleDocument *iface, DWORD *status)
{
    (void)iface;
    if(!status) return E_POINTER;
    *status = 0;
    append_text(L"IOLEDOCUMENT_GETMISCSTATUS\r\n");
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE ole_document_enum_views(IOleDocument *iface,
                                                         IEnumOleDocumentViews **enumerator,
                                                         IOleDocumentView **view)
{
    (void)iface;
    if(!enumerator || !view) return E_POINTER;
    *enumerator = NULL;
    *view = NULL;
    append_text(L"IOLEDOCUMENT_ENUMVIEWS\r\n");
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE service_provider_query_interface(IServiceProvider *iface,
                                                                   REFIID iid, void **out)
{
    return document_query_interface(from_service_provider(iface), iid, out);
}
static ULONG STDMETHODCALLTYPE service_provider_add_ref(IServiceProvider *iface)
{
    return document_add_ref(from_service_provider(iface));
}
static ULONG STDMETHODCALLTYPE service_provider_release(IServiceProvider *iface)
{
    return document_release(from_service_provider(iface));
}
static HRESULT STDMETHODCALLTYPE service_provider_query_service(IServiceProvider *iface,
                                                                 REFGUID service, REFIID iid,
                                                                 void **out)
{
    ProbeDocument *document = from_service_provider(iface);
    if(!out) return E_POINTER;
    *out = NULL;
    append_guid(L"ISERVICEPROVIDER_SERVICE=", service);
    append_guid(L"ISERVICEPROVIDER_IID=", iid);
    if(IsEqualGUID(service, &SID_SInternetSecurityManager) &&
       IsEqualIID(iid, &IID_IInternetSecurityManager)) {
        *out = &document->IInternetSecurityManager_iface;
        document_add_ref(document);
        append_text(L"ISERVICEPROVIDER_SECURITY_MANAGER=provided\r\n");
        return S_OK;
    }
    if(IsEqualIID(iid, &IID_IOleCommandTarget)) {
        *out = &document->IOleCommandTarget_iface;
        document_add_ref(document);
        append_text(L"ISERVICEPROVIDER_COMMANDTARGET=provided\r\n");
        return S_OK;
    }
    return E_NOINTERFACE;
}

static HRESULT STDMETHODCALLTYPE command_target_query_interface(IOleCommandTarget *iface,
                                                                 REFIID iid, void **out)
{
    return document_query_interface(from_command_target(iface), iid, out);
}
static ULONG STDMETHODCALLTYPE command_target_add_ref(IOleCommandTarget *iface)
{
    return document_add_ref(from_command_target(iface));
}
static ULONG STDMETHODCALLTYPE command_target_release(IOleCommandTarget *iface)
{
    return document_release(from_command_target(iface));
}
static HRESULT STDMETHODCALLTYPE command_target_query_status(IOleCommandTarget *iface,
                                                              const GUID *group, ULONG count,
                                                              OLECMD commands[],
                                                              OLECMDTEXT *command_text)
{
    ULONG index;
    ProbeDocument *document = from_command_target(iface);
    (void)command_text;
    if(count && !commands) return E_POINTER;
    if(group) append_guid(L"IOLECOMMANDTARGET_GROUP=", group);
    append_dword(L"IOLECOMMANDTARGET_QUERYSTATUS_COUNT=", count);
    for(index = 0; index < count; ++index) {
        commands[index].cmdf = 0;
        if(!group && (commands[index].cmdID == OLECMDID_REFRESH ||
                      commands[index].cmdID == OLECMDID_STOP)) {
            commands[index].cmdf = OLECMDF_SUPPORTED;
            if(document->runtime->pipe_read && document->runtime->pipe_write &&
               document->page_session[0] && document->page_target[0])
                commands[index].cmdf |= OLECMDF_ENABLED;
        }
        append_dword(L"IOLECOMMANDTARGET_QUERYSTATUS_ID=", commands[index].cmdID);
    }
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE command_target_exec(IOleCommandTarget *iface,
                                                      const GUID *group, DWORD command_id,
                                                      DWORD command_exec_opt, VARIANT *input,
                                                      VARIANT *output)
{
    ProbeDocument *document = from_command_target(iface);
    HRESULT hr;
    (void)input;
    (void)output;
    if(group) append_guid(L"IOLECOMMANDTARGET_EXEC_GROUP=", group);
    append_dword(L"IOLECOMMANDTARGET_EXEC_ID=", command_id);
    if(group) return OLECMDERR_E_UNKNOWNGROUP;
    if(command_id != OLECMDID_REFRESH && command_id != OLECMDID_STOP)
        return OLECMDERR_E_NOTSUPPORTED;
    if(command_exec_opt == OLECMDEXECOPT_SHOWHELP) return OLECMDERR_E_NOHELP;
    if(!document->runtime->pipe_read || !document->runtime->pipe_write ||
       !document->page_session[0] || !document->page_target[0]) return OLECMDERR_E_DISABLED;
    hr = document_post_navigation(document,
        command_id == OLECMDID_REFRESH ? "Page.reload" : "Page.stopLoading", "{}",
        command_id == OLECMDID_REFRESH ? 2 : 4, NULL, document->navigation_virtual_url);
    if(SUCCEEDED(hr)) append_text(command_id == OLECMDID_REFRESH ?
        L"MSHTML_REFRESH_QUEUED\r\n" : L"MSHTML_STOP_QUEUED\r\n");
    return hr;
}

static HRESULT STDMETHODCALLTYPE document_view_query_interface(IOleDocumentView *iface,
                                                                REFIID iid, void **out)
{
    ProbeDocument *document = from_document_view(iface);
    if(!out) return E_POINTER;
    *out = NULL;
    if(IsEqualIID(iid, &IID_IOleDocumentView)) {
        *out = iface;
        document_add_ref(document);
        return S_OK;
    }
    return document_query_interface(document, iid, out);
}
static ULONG STDMETHODCALLTYPE document_view_add_ref(IOleDocumentView *iface)
{
    return document_add_ref(from_document_view(iface));
}
static ULONG STDMETHODCALLTYPE document_view_release(IOleDocumentView *iface)
{
    return document_release(from_document_view(iface));
}
static HRESULT STDMETHODCALLTYPE document_view_set_in_place_site(IOleDocumentView *iface,
                                                                  IOleInPlaceSite *site)
{
    ProbeDocument *document = from_document_view(iface);
    if(site == document->view_site) return S_OK;
    if(site) IOleInPlaceSite_AddRef(site);
    in_place_object_deactivate(&document->IOleInPlaceObject_iface);
    if(document->view_site) IOleInPlaceSite_Release(document->view_site);
    document->view_site = site;
    document->host_window = NULL;
    append_text(L"IOLEDOCUMENTVIEW_SETINPLACESITE\r\n");
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE document_view_get_in_place_site(IOleDocumentView *iface,
                                                                  IOleInPlaceSite **site)
{
    ProbeDocument *document = from_document_view(iface);
    if(!site) return E_POINTER;
    *site = document->view_site;
    if(!*site) return E_FAIL;
    IOleInPlaceSite_AddRef(*site);
    append_text(L"IOLEDOCUMENTVIEW_GETINPLACESITE\r\n");
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE document_view_get_document(IOleDocumentView *iface,
                                                             IUnknown **document_out)
{
    ProbeDocument *document = from_document_view(iface);
    if(!document_out) return E_POINTER;
    *document_out = (IUnknown *)&document->IPersist_iface;
    document_add_ref(document);
    append_text(L"IOLEDOCUMENTVIEW_GETDOCUMENT\r\n");
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE document_view_set_rect(IOleDocumentView *iface, LPRECT rect)
{
    ProbeDocument *document = from_document_view(iface);
    if(!rect) return E_POINTER;
    document->view_rect = *rect;
    if(document->viewport_window && IsWindow(document->host_window) && !IsRectEmpty(rect)) {
        RECT mapped = *rect;
        MapWindowPoints(document->host_window, GetParent(document->viewport_window), (POINT *)&mapped, 2);
        if(!SetWindowPos(document->viewport_window, NULL, mapped.left, mapped.top,
            mapped.right - mapped.left, mapped.bottom - mapped.top, SWP_NOZORDER | SWP_NOACTIVATE))
            return HRESULT_FROM_WIN32(GetLastError());
    }
    append_text(L"IOLEDOCUMENTVIEW_SETRECT\r\n");
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE document_view_get_rect(IOleDocumentView *iface, LPRECT rect)
{
    ProbeDocument *document = from_document_view(iface);
    if(!rect) return E_POINTER;
    *rect = document->view_rect;
    append_text(L"IOLEDOCUMENTVIEW_GETRECT\r\n");
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE document_view_set_rect_complex(IOleDocumentView *iface,
                                                                 LPRECT view_rect,
                                                                 LPRECT horizontal_scroll,
                                                                 LPRECT vertical_scroll,
                                                                 LPRECT size_box)
{
    (void)horizontal_scroll;
    (void)vertical_scroll;
    (void)size_box;
    append_text(L"IOLEDOCUMENTVIEW_SETRECTCOMPLEX\r\n");
    if(view_rect) return document_view_set_rect(iface, view_rect);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE document_view_show(IOleDocumentView *iface, WINBOOL show)
{
    ProbeDocument *document = from_document_view(iface);
    HRESULT hr;
    if(show) {
        hr = ensure_in_place(document);
        if(FAILED(hr)) return hr;
    } else {
        hr = document_view_ui_activate(iface, FALSE);
        if(FAILED(hr)) return hr;
    }
    document->view_visible = show ? TRUE : FALSE;
    if(show && document->supermium_process_id) {
        hr = attach_supermium_window(document);
        if(hr != S_OK) {
            document->view_visible = FALSE;
            return FAILED(hr) ? hr : E_FAIL;
        }
    }
    if(document->viewport_window) ShowWindow(document->viewport_window, show ? SW_SHOWNOACTIVATE : SW_HIDE);
    if(document->renderer_window) ShowWindow(document->renderer_window, show ? SW_SHOWNOACTIVATE : SW_HIDE);
    append_dword(L"IOLEDOCUMENTVIEW_SHOW=", document->view_visible);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE document_view_ui_activate(IOleDocumentView *iface,
                                                           WINBOOL activate)
{
    ProbeDocument *document = from_document_view(iface);
    HRESULT hr;
    append_dword(L"IOLEDOCUMENTVIEW_UIACTIVATE=", activate ? 1 : 0);
    if(!activate) return in_place_object_ui_deactivate(&document->IOleInPlaceObject_iface);
    hr = ensure_in_place(document);
    if(FAILED(hr)) return hr;
    if(!document->ui_active) {
        hr = IOleInPlaceSite_OnUIActivate(document->view_site);
        if(FAILED(hr)) return hr;
        document->ui_active = TRUE;
        if(document->in_place_frame) {
            hr = IOleInPlaceFrame_SetActiveObject(document->in_place_frame,
                &document->IOleInPlaceActiveObject_iface, L"Supermium");
            if(FAILED(hr)) goto failed;
        }
        if(document->in_place_ui_window) {
            hr = IOleInPlaceUIWindow_SetActiveObject(document->in_place_ui_window,
                &document->IOleInPlaceActiveObject_iface, L"Supermium");
            if(FAILED(hr)) goto failed;
        }
    }
    focus_supermium_window(document);
    return S_OK;
failed:
    in_place_object_ui_deactivate(&document->IOleInPlaceObject_iface);
    return hr;
}
static HRESULT STDMETHODCALLTYPE document_view_open(IOleDocumentView *iface)
{
    (void)iface;
    append_text(L"IOLEDOCUMENTVIEW_OPEN\r\n");
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE document_view_close_view(IOleDocumentView *iface,
                                                          DWORD reserved)
{
    (void)reserved;
    append_text(L"IOLEDOCUMENTVIEW_CLOSEVIEW\r\n");
    in_place_object_deactivate(&from_document_view(iface)->IOleInPlaceObject_iface);
    return document_view_set_in_place_site(iface, NULL);
}
static HRESULT STDMETHODCALLTYPE document_view_save_view_state(IOleDocumentView *iface,
                                                               LPSTREAM stream)
{
    (void)iface;
    (void)stream;
    append_text(L"IOLEDOCUMENTVIEW_SAVEVIEWSTATE\r\n");
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE document_view_apply_view_state(IOleDocumentView *iface,
                                                                LPSTREAM stream)
{
    (void)iface;
    (void)stream;
    append_text(L"IOLEDOCUMENTVIEW_APPLYVIEWSTATE\r\n");
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE document_view_clone(IOleDocumentView *iface,
                                                      IOleInPlaceSite *site,
                                                      IOleDocumentView **view)
{
    (void)iface;
    (void)site;
    if(!view) return E_POINTER;
    *view = NULL;
    append_text(L"IOLEDOCUMENTVIEW_CLONE\r\n");
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE in_place_object_query_interface(IOleInPlaceObject *iface,
                                                                  REFIID iid, void **out)
{
    return document_query_interface(from_in_place_object(iface), iid, out);
}
static ULONG STDMETHODCALLTYPE in_place_object_add_ref(IOleInPlaceObject *iface)
{
    return document_add_ref(from_in_place_object(iface));
}
static ULONG STDMETHODCALLTYPE in_place_object_release(IOleInPlaceObject *iface)
{
    return document_release(from_in_place_object(iface));
}
static HRESULT STDMETHODCALLTYPE in_place_object_get_window(IOleInPlaceObject *iface,
                                                            HWND *window)
{
    ProbeDocument *document = from_in_place_object(iface);
    if(!window) return E_POINTER;
    *window = document->viewport_window;
    append_text(L"IOLEINPLACEOBJECT_GETWINDOW\r\n");
    return *window ? S_OK : E_FAIL;
}
static HRESULT STDMETHODCALLTYPE in_place_object_context_sensitive_help(IOleInPlaceObject *iface,
                                                                         WINBOOL enter_mode)
{
    (void)iface;
    (void)enter_mode;
    append_text(L"IOLEINPLACEOBJECT_CONTEXTHELP\r\n");
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE in_place_object_ui_deactivate(IOleInPlaceObject *iface)
{
    ProbeDocument *document = from_in_place_object(iface);
    if(document->ui_active) {
        document->ui_active = FALSE;
        if(document->in_place_frame) IOleInPlaceFrame_SetActiveObject(document->in_place_frame, NULL, NULL);
        if(document->in_place_ui_window) IOleInPlaceUIWindow_SetActiveObject(document->in_place_ui_window, NULL, NULL);
        if(document->view_site) IOleInPlaceSite_OnUIDeactivate(document->view_site, FALSE);
    }
    if(document->input_target_thread) {
        HWND focus = GetFocus();
        if(focus && (focus == document->renderer_window || IsChild(document->renderer_window, focus)))
            SetFocus(document->host_window);
        AttachThreadInput(document->input_owner_thread, document->input_target_thread, FALSE);
        document->input_target_thread = 0;
    }
    append_text(L"IOLEINPLACEOBJECT_UIDEACTIVATE\r\n");
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE in_place_object_deactivate(IOleInPlaceObject *iface)
{
    ProbeDocument *document = from_in_place_object(iface);
    document_view_show(&document->IOleDocumentView_iface, FALSE);
    if(document->in_place_active) {
        document->in_place_active = FALSE;
        if(document->view_site) IOleInPlaceSite_OnInPlaceDeactivate(document->view_site);
    }
    if(document->in_place_frame) IOleInPlaceFrame_Release(document->in_place_frame);
    if(document->in_place_ui_window) IOleInPlaceUIWindow_Release(document->in_place_ui_window);
    document->in_place_frame = NULL;
    document->in_place_ui_window = NULL;
    append_text(L"IOLEINPLACEOBJECT_INPLACEDEACTIVATE\r\n");
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE in_place_object_set_object_rects(IOleInPlaceObject *iface,
                                                                   LPCRECT position,
                                                                   LPCRECT clip)
{
    ProbeDocument *document = from_in_place_object(iface);
    (void)clip;
    if(!position) return E_POINTER;
    document_view_set_rect(&document->IOleDocumentView_iface, (LPRECT)position);
    if(document->renderer_window && document->viewport_window) {
        RECT client;
        HRESULT hr;
        if(!GetClientRect(document->viewport_window, &client))
            return HRESULT_FROM_WIN32(GetLastError());
        hr = layout_supermium_window(document, &client);
        append_dword(L"SUPERMIUM_WINDOW_RESIZE_RESULT=", (DWORD)hr);
    }
    /* This is the container's resize notification; calling OnPosRectChange
     * back into it here can recursively re-enter SetObjectRects. */
    append_text(L"IOLEINPLACEOBJECT_SETOBJECTRECTS\r\n");
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE in_place_object_reactivate_and_undo(IOleInPlaceObject *iface)
{
    (void)iface;
    append_text(L"IOLEINPLACEOBJECT_REACTIVATEANDUNDO\r\n");
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE active_object_query_interface(IOleInPlaceActiveObject *iface,
                                                                REFIID iid, void **out)
{
    return document_query_interface(from_active_object(iface), iid, out);
}
static ULONG STDMETHODCALLTYPE active_object_add_ref(IOleInPlaceActiveObject *iface)
{
    return document_add_ref(from_active_object(iface));
}
static ULONG STDMETHODCALLTYPE active_object_release(IOleInPlaceActiveObject *iface)
{
    return document_release(from_active_object(iface));
}
static HRESULT STDMETHODCALLTYPE active_object_get_window(IOleInPlaceActiveObject *iface,
                                                          HWND *window)
{
    ProbeDocument *document = from_active_object(iface);
    if(!window) return E_POINTER;
    *window = document->viewport_window;
    append_text(L"IOLEINPLACEACTIVEOBJECT_GETWINDOW\r\n");
    return *window ? S_OK : E_FAIL;
}
static HRESULT STDMETHODCALLTYPE active_object_context_sensitive_help(IOleInPlaceActiveObject *iface,
                                                                       WINBOOL enter_mode)
{
    (void)iface;
    (void)enter_mode;
    append_text(L"IOLEINPLACEACTIVEOBJECT_CONTEXTHELP\r\n");
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE active_object_translate_accelerator(IOleInPlaceActiveObject *iface,
                                                                      LPMSG message)
{
    ProbeDocument *document = from_active_object(iface);
    HWND input = NULL;
    if(!message) return E_POINTER;
    if(!document->ui_active || !document->view_visible ||
       message->message < WM_KEYFIRST || message->message > WM_KEYLAST)
        return S_FALSE;
    if(message->wParam == VK_F5 || message->wParam == VK_F6 ||
       ((GetKeyState(VK_CONTROL) & 0x8000) &&
        (message->wParam == 'T' || message->wParam == 'L' || message->wParam == 'N' ||
         message->wParam == 'W' || message->wParam == VK_TAB)) ||
       ((GetKeyState(VK_MENU) & 0x8000) &&
        (message->wParam == VK_LEFT || message->wParam == VK_RIGHT ||
         message->wParam == VK_HOME || message->wParam == VK_F4))) {
        append_text(L"MSHTML_HOST_ACCELERATOR_NOT_CONSUMED\r\n");
        return S_FALSE;
    }
    /* The container may offer keys from its native address/search controls.
     * Those are not renderer input and must remain with the container. */
    if(!document->viewport_window ||
       (message->hwnd != document->viewport_window &&
        !IsChild(document->viewport_window, message->hwnd))) return S_FALSE;
    if(document->renderer_window)
        EnumChildWindows(document->renderer_window, find_renderer_input, (LPARAM)&input);
    if(message && document->renderer_window && IsWindow(document->renderer_window) &&
       message->message >= WM_KEYFIRST && message->message <= WM_KEYLAST &&
       input && PostMessageW(input, message->message, message->wParam, message->lParam)) {
        append_text(L"SUPERMIUM_INPUT_KEY_FORWARDED\r\n");
        return S_OK;
    }
    append_text(L"IOLEINPLACEACTIVEOBJECT_TRANSLATEACCELERATOR\r\n");
    return S_FALSE;
}
static HRESULT STDMETHODCALLTYPE active_object_on_frame_window_activate(
    IOleInPlaceActiveObject *iface, WINBOOL activate)
{
    if(activate) focus_supermium_window(from_active_object(iface));
    append_dword(L"IOLEINPLACEACTIVEOBJECT_FRAMEACTIVATE=", activate ? 1 : 0);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE active_object_on_doc_window_activate(
    IOleInPlaceActiveObject *iface, WINBOOL activate)
{
    (void)iface;
    append_dword(L"IOLEINPLACEACTIVEOBJECT_DOCACTIVATE=", activate ? 1 : 0);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE active_object_resize_border(IOleInPlaceActiveObject *iface,
                                                              LPCRECT border,
                                                              IOleInPlaceUIWindow *window,
                                                              WINBOOL frame_window)
{
    (void)iface;
    (void)border;
    (void)window;
    (void)frame_window;
    append_text(L"IOLEINPLACEACTIVEOBJECT_RESIZEBORDER\r\n");
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE active_object_enable_modeless(IOleInPlaceActiveObject *iface,
                                                               WINBOOL enable)
{
    (void)iface;
    append_dword(L"IOLEINPLACEACTIVEOBJECT_ENABLEMODELESS=", enable ? 1 : 0);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dispatch_query_interface(IDispatch *iface, REFIID iid, void **out)
{
    return document_query_interface(from_dispatch(iface), iid, out);
}
static ULONG STDMETHODCALLTYPE dispatch_add_ref(IDispatch *iface) { return document_add_ref(from_dispatch(iface)); }
static ULONG STDMETHODCALLTYPE dispatch_release(IDispatch *iface) { return document_release(from_dispatch(iface)); }
static HRESULT STDMETHODCALLTYPE dispatch_get_type_info_count(IDispatch *iface, UINT *count)
{
    (void)iface;
    if(!count) return E_POINTER;
    *count = 0;
    append_text(L"IDISPATCH_GETTYPEINFOCOUNT\r\n");
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE dispatch_get_type_info(IDispatch *iface, UINT index, LCID locale,
                                                         ITypeInfo **info)
{
    (void)iface;
    (void)index;
    (void)locale;
    if(info) *info = NULL;
    append_text(L"IDISPATCH_GETTYPEINFO\r\n");
    return DISP_E_BADINDEX;
}
static HRESULT STDMETHODCALLTYPE dispatch_get_ids_of_names(IDispatch *iface, REFIID iid,
                                                            LPOLESTR *names, UINT count,
                                                            LCID locale, DISPID *ids)
{
    (void)iface;
    (void)iid;
    (void)locale;
    UINT index;
    HRESULT hr = S_OK;
    if(!IsEqualIID(iid, &IID_NULL)) return DISP_E_UNKNOWNINTERFACE;
    if(!names || !ids) return E_POINTER;
    for(index = 0; index < count; ++index) {
        ids[index] = DISPID_UNKNOWN;
        if(names[index] && !lstrcmpiW(names[index], L"URL"))
            ids[index] = DISPID_IHTMLDOCUMENT2_URL;
        else if(names[index] && !lstrcmpiW(names[index], L"readyState"))
            ids[index] = DISPID_IHTMLDOCUMENT2_READYSTATE;
        else if(names[index] && !lstrcmpiW(names[index], L"title"))
            ids[index] = DISPID_IHTMLDOCUMENT2_TITLE;
        else if(names[index] && !lstrcmpiW(names[index], L"write"))
            ids[index] = DISPID_IHTMLDOCUMENT2_WRITE;
        else if(names[index] && !lstrcmpiW(names[index], L"writeln"))
            ids[index] = DISPID_IHTMLDOCUMENT2_WRITELN;
        else if(names[index] && !lstrcmpiW(names[index], L"close"))
            ids[index] = DISPID_IHTMLDOCUMENT2_CLOSE;
        else if(names[index] && !lstrcmpiW(names[index], L"body"))
            ids[index] = DISPID_IHTMLDOCUMENT2_BODY;
        else if(names[index] && !lstrcmpiW(names[index], L"documentElement"))
            ids[index] = DISPID_IHTMLDOCUMENT3_DOCUMENTELEMENT;
        else if(names[index] && !lstrcmpiW(names[index], L"getElementById"))
            ids[index] = DISPID_IHTMLDOCUMENT3_GETELEMENTBYID;
        else if(names[index] && !lstrcmpiW(names[index], L"createElement"))
            ids[index] = DISPID_IHTMLDOCUMENT2_CREATEELEMENT;
        else hr = DISP_E_UNKNOWNNAME;
    }
    append_text(L"IDISPATCH_GETIDSOFNAMES\r\n");
    return hr;
}
static HRESULT STDMETHODCALLTYPE dispatch_invoke(IDispatch *iface, DISPID member, REFIID iid,
                                                 LCID locale, WORD flags, DISPPARAMS *params,
                                                 VARIANT *result, EXCEPINFO *exception,
                                                 UINT *argument_error)
{
    ProbeDocument *document = from_dispatch(iface);
    (void)locale;
    (void)flags;
    (void)params;
    (void)result;
    (void)exception;
    (void)argument_error;
    append_dword(L"IDISPATCH_INVOKE_DISPID=", (DWORD)member);
    if(!IsEqualIID(iid, &IID_NULL)) return DISP_E_UNKNOWNINTERFACE;
    if(member == DISPID_IHTMLDOCUMENT2_BODY || member == DISPID_IHTMLDOCUMENT3_DOCUMENTELEMENT ||
       member == DISPID_IHTMLDOCUMENT3_GETELEMENTBYID || member == DISPID_IHTMLDOCUMENT2_CREATEELEMENT) {
        BOOL method = member == DISPID_IHTMLDOCUMENT3_GETELEMENTBYID || member == DISPID_IHTMLDOCUMENT2_CREATEELEMENT;
        LPCWSTR function;
        HRESULT hr;
        if(method ? (flags != DISPATCH_METHOD && flags != (DISPATCH_METHOD | DISPATCH_PROPERTYGET))
                  : flags != DISPATCH_PROPERTYGET) return DISP_E_MEMBERNOTFOUND;
        if((params ? params->cArgs : 0) != (method ? 1u : 0u)) return DISP_E_BADPARAMCOUNT;
        if(params && params->cNamedArgs) return DISP_E_NONAMEDARGS;
        if(method && (!params->rgvarg || params->rgvarg[0].vt != VT_BSTR)) {
            if(argument_error) *argument_error = 0;
            return DISP_E_TYPEMISMATCH;
        }
        if(!result) return E_POINTER;
        VariantInit(result);
        function = member == DISPID_IHTMLDOCUMENT2_BODY ? L"function(){return this.body;}" :
                   member == DISPID_IHTMLDOCUMENT3_DOCUMENTELEMENT ? L"function(){return this.documentElement;}" :
                   member == DISPID_IHTMLDOCUMENT3_GETELEMENTBYID ? L"function(s){return this.getElementById(s);}" :
                   L"function(s){return this.createElement(s);}";
        hr = document_element(document, function, method ? params->rgvarg[0].bstrVal : NULL, method,
                              (IHTMLElement **)&result->pdispVal);
        if(SUCCEEDED(hr)) result->vt = VT_DISPATCH;
        return hr;
    }
    if(member == DISPID_IHTMLDOCUMENT2_TITLE) {
        if(flags == DISPATCH_PROPERTYGET) {
            HRESULT hr;
            if(params && params->cArgs) return DISP_E_BADPARAMCOUNT;
            if(!result) return E_POINTER;
            VariantInit(result);
            hr = content_get_title(document, &result->bstrVal);
            if(SUCCEEDED(hr)) result->vt = VT_BSTR;
            return hr;
        }
        if(flags == DISPATCH_PROPERTYPUT) {
            if(!params || params->cArgs != 1) return DISP_E_BADPARAMCOUNT;
            if(!params->rgvarg || params->cNamedArgs != 1 || !params->rgdispidNamedArgs ||
               params->rgdispidNamedArgs[0] != DISPID_PROPERTYPUT) return DISP_E_PARAMNOTFOUND;
            if(params->rgvarg[0].vt != VT_BSTR) {
                if(argument_error) *argument_error = 0;
                return DISP_E_TYPEMISMATCH;
            }
            return content_put_title(document, params->rgvarg[0].bstrVal);
        }
        return DISP_E_MEMBERNOTFOUND;
    }
    if(member == DISPID_IHTMLDOCUMENT2_WRITE || member == DISPID_IHTMLDOCUMENT2_WRITELN) {
        SAFEARRAY *array;
        HRESULT hr;
        LONG i;
        UINT count = params ? params->cArgs : 0;
        if(flags != DISPATCH_METHOD) return DISP_E_MEMBERNOTFOUND;
        if(params && params->cNamedArgs) return DISP_E_NONAMEDARGS;
        if(count > 65536 || (count && !params->rgvarg)) return DISP_E_BADPARAMCOUNT;
        if(count == 1 && params->rgvarg[0].vt == (VT_ARRAY | VT_VARIANT))
            return content_write(document, params->rgvarg[0].parray, member == DISPID_IHTMLDOCUMENT2_WRITELN);
        array = SafeArrayCreateVector(VT_VARIANT, 0, count);
        if(!array) return E_OUTOFMEMORY;
        hr = S_OK;
        for(i = 0; (UINT)i < count; ++i) {
            hr = SafeArrayPutElement(array, &i, &params->rgvarg[count - 1 - i]);
            if(FAILED(hr)) break;
        }
        if(SUCCEEDED(hr)) hr = content_write(document, array, member == DISPID_IHTMLDOCUMENT2_WRITELN);
        SafeArrayDestroy(array);
        return hr;
    }
    if(member == DISPID_IHTMLDOCUMENT2_CLOSE) {
        if(flags != DISPATCH_METHOD) return DISP_E_MEMBERNOTFOUND;
        if(params && params->cArgs) return DISP_E_BADPARAMCOUNT;
        return content_close(document);
    }
    if(member == DISPID_IHTMLDOCUMENT2_URL && (flags & DISPATCH_PROPERTYPUT)) {
        if(!params || params->cArgs != 1) return DISP_E_BADPARAMCOUNT;
        if(!params->rgvarg || params->cNamedArgs != 1 || !params->rgdispidNamedArgs ||
           params->rgdispidNamedArgs[0] != DISPID_PROPERTYPUT) return DISP_E_PARAMNOTFOUND;
        if(params->rgvarg[0].vt != VT_BSTR) {
            if(argument_error) *argument_error = 0;
            return DISP_E_TYPEMISMATCH;
        }
        return html2_put_url(&document->IHTMLDocument2_iface, params->rgvarg[0].bstrVal);
    }
    if(member == DISPID_READYSTATE) {
        if(flags != DISPATCH_PROPERTYGET) return DISP_E_MEMBERNOTFOUND;
        if(params && params->cArgs) return DISP_E_BADPARAMCOUNT;
        if(!result) return E_POINTER;
        VariantInit(result);
        result->vt = VT_I4;
        result->lVal = document->ready_state;
        return S_OK;
    }
    if(member == DISPID_IHTMLDOCUMENT2_READYSTATE)
        return html2_invoke(&document->IHTMLDocument2_iface, DISPID_READYSTATE, flags, result);
    if(member == DISPID_IHTMLDOCUMENT2_URL && (flags & DISPATCH_PROPERTYGET)) {
        HRESULT hr;
        if(!result) return E_POINTER;
        VariantInit(result);
        hr = html2_get_url(&document->IHTMLDocument2_iface, &result->bstrVal);
        if(SUCCEEDED(hr)) result->vt = VT_BSTR;
        return hr;
    }
    return DISP_E_MEMBERNOTFOUND;
}

__declspec(dllexport) HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void **out)
{
    ProbeFactory *factory;
    HRESULT hr;
    append_guid(L"DLLGETCLASSOBJECT_CLSID=", clsid);
    if(!IsEqualCLSID(clsid, &CLSID_HTMLDocument)) return CLASS_E_CLASSNOTAVAILABLE;
    factory = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*factory));
    if(!factory) return E_OUTOFMEMORY;
    InterlockedIncrement(&live_objects);
    factory->IClassFactory_iface.lpVtbl = &factory_vtbl;
    factory->references = 1;
    hr = factory_query_interface(&factory->IClassFactory_iface, iid, out);
    factory_release(&factory->IClassFactory_iface);
    return hr;
}

__declspec(dllexport) HRESULT WINAPI DllCanUnloadNow(void)
{
    if(InterlockedCompareExchange(&cdp_live_workers, 0, 0)) return S_FALSE;
    if(InterlockedCompareExchange(&live_objects, 0, 0) ||
       InterlockedCompareExchange(&server_locks, 0, 0)) return S_FALSE;
    if(!keyboard_router_idle()) return S_FALSE;
    if(!UnregisterClassW(L"TritonMshtmlViewport", module_instance) &&
       GetLastError() != ERROR_CLASS_DOES_NOT_EXIST) return S_FALSE;
    if(!UnregisterClassW(L"TritonMshtmlPopup", module_instance) &&
       GetLastError() != ERROR_CLASS_DOES_NOT_EXIST) return S_FALSE;
    return S_OK;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)instance;
    (void)reserved;
    if(reason == DLL_PROCESS_ATTACH) {
        module_instance = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
