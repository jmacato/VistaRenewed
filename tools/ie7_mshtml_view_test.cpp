#define INITGUID
#include <windows.h>
#include <mshtml.h>
#include <docobj.h>
#include <urlmon.h>
#include <cstdio>

#define CHECK(x) do { if(!(x)) { std::printf("FAIL line=%d: %s\n", __LINE__, #x); return 1; } } while(0)

static unsigned native_key_down, native_key_up;
static WPARAM native_last_key;
static bool native_control;
static LRESULT CALLBACK host_window_proc(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    if(message == WM_KEYDOWN) {
        ++native_key_down; native_last_key = wp;
        native_control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        return 0;
    }
    if(message == WM_KEYUP) { ++native_key_up; return 0; }
    return DefWindowProcW(window, message, wp, lp);
}

// A real windowed DocObject container. Stack lifetime is bounded by all COM refs.
struct Host : IOleInPlaceSite, IOleInPlaceFrame, IOleClientSite, IOleDocumentSite {
    ULONG refs = 1;
    HWND window;
    unsigned inplace_on = 0, inplace_off = 0, ui_on = 0, ui_off = 0;
    unsigned active_on = 0, active_off = 0;
    IOleInPlaceActiveObject *active = nullptr;
    IOleDocument *activation_document = nullptr; // borrowed for the bounded test
    IOleDocumentView *activated_view = nullptr;
    IOleCommandTarget *activated_commands = nullptr;
    unsigned activate_me = 0;
    HRESULT activation_result = S_OK;
    explicit Host(HWND hwnd) : window(hwnd) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
        if(!out) return E_POINTER;
        *out = nullptr;
        if(iid == IID_IUnknown || iid == IID_IOleWindow || iid == IID_IOleInPlaceSite)
            *out = static_cast<IOleInPlaceSite *>(this);
        else if(iid == IID_IOleInPlaceUIWindow || iid == IID_IOleInPlaceFrame)
            *out = static_cast<IOleInPlaceFrame *>(this);
        else if(iid == IID_IOleClientSite)
            *out = static_cast<IOleClientSite *>(this);
        else if(iid == IID_IOleDocumentSite && activation_document)
            *out = static_cast<IOleDocumentSite *>(this);
        else return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs; }
    HRESULT STDMETHODCALLTYPE SaveObject() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetMoniker(DWORD, DWORD, IMoniker **out) override {
        if(!out) return E_POINTER;
        *out = nullptr; return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetContainer(IOleContainer **out) override {
        if(!out) return E_POINTER;
        *out = nullptr; return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE ShowObject() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnShowWindow(BOOL) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE RequestNewObjectLayout() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ActivateMe(IOleDocumentView *offered_view) override {
        ++activate_me;
        if(FAILED(activation_result)) return activation_result;
        if(!offered_view || !activation_document || activated_view) return E_UNEXPECTED;
        HRESULT hr = activation_document->CreateView(static_cast<IOleInPlaceSite *>(this),
            nullptr, 0, &activated_view);
        if(FAILED(hr)) return hr;
        if(activated_view != offered_view) return E_UNEXPECTED;
        hr = activated_view->QueryInterface(IID_IOleCommandTarget,
            reinterpret_cast<void **>(&activated_commands));
        if(FAILED(hr)) return hr;
        RECT rect;
        GetClientRect(window, &rect);
        hr = activated_view->SetRect(&rect);
        if(SUCCEEDED(hr)) hr = activated_view->UIActivate(TRUE);
        if(SUCCEEDED(hr)) hr = activated_view->Show(TRUE);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE GetWindow(HWND *out) override { if(!out) return E_POINTER; *out = window; return S_OK; }
    HRESULT STDMETHODCALLTYPE ContextSensitiveHelp(BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CanInPlaceActivate() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnInPlaceActivate() override { ++inplace_on; return S_OK; }
    HRESULT STDMETHODCALLTYPE OnUIActivate() override { ++ui_on; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetWindowContext(IOleInPlaceFrame **frame, IOleInPlaceUIWindow **doc,
        RECT *pos, RECT *clip, OLEINPLACEFRAMEINFO *info) override {
        *frame = static_cast<IOleInPlaceFrame *>(this); AddRef(); *doc = nullptr;
        GetClientRect(window, pos); *clip = *pos;
        info->cb = sizeof(*info); info->fMDIApp = FALSE; info->hwndFrame = window;
        info->haccel = nullptr; info->cAccelEntries = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Scroll(SIZE) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE OnUIDeactivate(BOOL) override { ++ui_off; return S_OK; }
    HRESULT STDMETHODCALLTYPE OnInPlaceDeactivate() override { ++inplace_off; return S_OK; }
    HRESULT STDMETHODCALLTYPE DiscardUndoState() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DeactivateAndUndo() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE OnPosRectChange(LPCRECT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetBorder(LPRECT rect) override { GetClientRect(window, rect); return S_OK; }
    HRESULT STDMETHODCALLTYPE RequestBorderSpace(LPCBORDERWIDTHS) override { return INPLACE_E_NOTOOLSPACE; }
    HRESULT STDMETHODCALLTYPE SetBorderSpace(LPCBORDERWIDTHS) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetActiveObject(IOleInPlaceActiveObject *object, LPCOLESTR) override {
        if(object) { object->AddRef(); ++active_on; } else ++active_off;
        if(active) active->Release();
        active = object;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE InsertMenus(HMENU, LPOLEMENUGROUPWIDTHS) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetMenu(HMENU, HOLEMENU, HWND) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE RemoveMenus(HMENU) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetStatusText(LPCOLESTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE EnableModeless(BOOL) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE TranslateAccelerator(LPMSG, WORD) override { return S_FALSE; }
};

struct Page {
    IHTMLDocument2 *html = nullptr;
    IOleDocument *doc = nullptr;
    IOleDocumentView *view = nullptr;
    IOleInPlaceObject *inplace = nullptr;
    IOleObject *ole = nullptr;
    HWND viewport = nullptr, renderer = nullptr;
};

static void pump(DWORD ms)
{
    DWORD start = GetTickCount();
    do {
        MSG msg;
        while(PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        Sleep(10);
    } while(GetTickCount() - start < ms);
}

static bool ready(Page &page)
{
    BSTR value = nullptr;
    HRESULT hr = page.html->get_readyState(&value);
    bool ok = SUCCEEDED(hr) && value && !lstrcmpW(value, L"complete");
    SysFreeString(value); return ok;
}

static bool send_key(WORD key, bool up)
{
    INPUT input = {};
    input.type = INPUT_KEYBOARD; input.ki.wVk = key;
    input.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    return SendInput(1, &input, sizeof(input)) == 1;
}

static BOOL CALLBACK find_input(HWND window, LPARAM value)
{
    WCHAR name[80];
    if(IsWindowVisible(window) && GetClassNameW(window, name, ARRAYSIZE(name)) &&
        !lstrcmpW(name, L"Chrome_RenderWidgetHostHWND")) {
        *reinterpret_cast<HWND *>(value) = window; return FALSE;
    }
    return TRUE;
}

int main()
{
    CHECK(SUCCEEDED(CoInitialize(nullptr)));
    HMODULE module = LoadLibraryW(L"C:\\TritonSupermiumBridge\\triton-ie7-mshtml-activation-probe.dll");
    CHECK(module);
    using GetClass = HRESULT (WINAPI *)(REFCLSID, REFIID, void **);
    using CanUnload = HRESULT (WINAPI *)();
    GetClass get_class = reinterpret_cast<GetClass>(reinterpret_cast<void *>(GetProcAddress(module, "DllGetClassObject")));
    CanUnload can_unload = reinterpret_cast<CanUnload>(reinterpret_cast<void *>(GetProcAddress(module, "DllCanUnloadNow")));
    CHECK(get_class && can_unload);
    IClassFactory *factory = nullptr;
    CHECK(get_class(CLSID_HTMLDocument, IID_IClassFactory, reinterpret_cast<void **>(&factory)) == S_OK);
    WNDCLASSW wc = {};
    wc.lpfnWndProc = host_window_proc; wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"TritonViewTestHost";
    CHECK(RegisterClassW(&wc));
    HWND window = CreateWindowExW(0, wc.lpszClassName, L"MSHTML embedded view lifecycle test", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        40, 40, 700, 480, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    CHECK(window);
    Host host(window);
    Page pages[2];
    RECT rect = {10, 20, 650, 390};
    for(unsigned i = 0; i < 2; ++i) {
        Page &p = pages[i];
        CHECK(factory->CreateInstance(nullptr, IID_IHTMLDocument2, reinterpret_cast<void **>(&p.html)) == S_OK);
        CHECK(p.html->QueryInterface(IID_IOleDocument, reinterpret_cast<void **>(&p.doc)) == S_OK);
        CHECK(p.html->QueryInterface(IID_IOleInPlaceObject, reinterpret_cast<void **>(&p.inplace)) == S_OK);
        CHECK(p.html->QueryInterface(IID_IOleObject, reinterpret_cast<void **>(&p.ole)) == S_OK);
        CHECK(p.doc->CreateView(nullptr, nullptr, 0, &p.view) == S_OK);
        CHECK(p.view->UIActivate(TRUE) == E_UNEXPECTED);
        CHECK(p.view->SetInPlaceSite(static_cast<IOleInPlaceSite *>(&host)) == S_OK);
        CHECK(p.view->SetRect(&rect) == S_OK);
        if(i) CHECK(p.view->Show(TRUE) == S_OK); // Activation before load is supported too.
        IMoniker *moniker = nullptr;
        IPersistMoniker *persist = nullptr;
        CHECK(CreateURLMoniker(nullptr, i ? L"about:Tabs" : L"about:blank#view-one", &moniker) == S_OK);
        CHECK(p.html->QueryInterface(IID_IPersistMoniker, reinterpret_cast<void **>(&persist)) == S_OK);
        CHECK(persist->Load(TRUE, moniker, nullptr, STGM_READ) == S_OK);
        persist->Release(); moniker->Release();
        unsigned ui_before = host.ui_on;
        CHECK(p.view->Show(TRUE) == S_OK);
        CHECK(host.ui_on == ui_before); // Show must not UI-activate by itself.
        CHECK(p.inplace->GetWindow(&p.viewport) == S_OK && IsWindow(p.viewport));
        CHECK(p.viewport != window && GetParent(p.viewport) == window);
        p.renderer = FindWindowExW(p.viewport, nullptr, L"Chrome_WidgetWin_1", nullptr);
        CHECK(p.renderer && IsWindowVisible(p.renderer));
        CHECK(p.view->UIActivate(TRUE) == S_OK);
        CHECK(host.active != nullptr && host.ui_on == ui_before + 1);
        CHECK(p.view->UIActivate(TRUE) == S_OK && host.ui_on == ui_before + 1);
        pump(1100);
        RECT actual;
        CHECK(GetWindowRect(p.viewport, &actual));
        MapWindowPoints(nullptr, window, reinterpret_cast<POINT *>(&actual), 2);
        CHECK(EqualRect(&actual, &rect));
        CHECK(ready(p));
        if(!i) {
            HWND renderer_input = nullptr;
            EnumChildWindows(p.renderer, find_input, reinterpret_cast<LPARAM>(&renderer_input));
            CHECK(renderer_input);
            // Foreground permission is earned with a real test click; an
            // arbitrary background SetForegroundWindow may be denied by Vista.
            CHECK(SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE));
            RECT screen;
            CHECK(GetWindowRect(window, &screen));
            INPUT mouse[3] = {};
            for(INPUT &event : mouse) event.type = INPUT_MOUSE;
            mouse[0].mi.dx = (screen.left + 180) * 65535 / (GetSystemMetrics(SM_CXSCREEN) - 1);
            mouse[0].mi.dy = (screen.top + 10) * 65535 / (GetSystemMetrics(SM_CYSCREEN) - 1);
            mouse[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
            mouse[1].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
            mouse[2].mi.dwFlags = MOUSEEVENTF_LEFTUP;
            CHECK(SendInput(3, mouse, sizeof(INPUT)) == 3);
            pump(100);
            SetFocus(renderer_input);
            pump(100);
            CHECK(GetFocus() == renderer_input);
            if(GetForegroundWindow() != window) {
                POINT cursor = {};
                WCHAR foreground_class[128] = {};
                GetCursorPos(&cursor);
                GetClassNameW(GetForegroundWindow(), foreground_class, ARRAYSIZE(foreground_class));
                std::printf("FOREGROUND_DIAGNOSTIC expected=%p actual=%p class=%ls desktop=%dx%d window=%ld,%ld cursor=%ld,%ld\n",
                    window, GetForegroundWindow(), foreground_class, GetSystemMetrics(SM_CXSCREEN),
                    GetSystemMetrics(SM_CYSCREEN), screen.left, screen.top, cursor.x, cursor.y);
            }
            CHECK(GetForegroundWindow() == window);
            unsigned before_down = native_key_down, before_up = native_key_up;
            CHECK(send_key(VK_CONTROL, false)); pump(40);
            CHECK(send_key('T', false)); pump(80);
            CHECK(send_key('T', false)); pump(40); // Repeat is swallowed, not a second tab action.
            CHECK(send_key('T', true)); pump(80);
            CHECK(send_key(VK_CONTROL, true)); pump(80);
            std::printf("ROUTED_KEYS down=%u up=%u last=%lu control=%d before=%u/%u\n",
                native_key_down, native_key_up, static_cast<unsigned long>(native_last_key),
                native_control, before_down, before_up);
            CHECK(native_key_down == before_down + 1 && native_key_up == before_up + 1);
            CHECK(native_last_key == 'T' && native_control);
            before_down = native_key_down;
            CHECK(send_key('X', false)); CHECK(send_key('X', true)); pump(100);
            CHECK(native_key_down == before_down); // Normal page keys never go to the host.
            HWND edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE,
                0, 0, 100, 20, window, nullptr, wc.hInstance, nullptr);
            CHECK(edit);
            MSG native_message = {};
            native_message.hwnd = edit; native_message.message = WM_KEYDOWN;
            native_message.wParam = 'X';
            CHECK(host.active && host.active->TranslateAccelerator(&native_message) == S_FALSE);
            SetFocus(edit);
            CHECK(send_key(VK_CONTROL, false)); pump(40);
            CHECK(send_key('T', false)); pump(80);
            CHECK(send_key('T', true)); CHECK(send_key(VK_CONTROL, true)); pump(80);
            CHECK(native_key_down == before_down); // Native controls bypass the bridge.
            DestroyWindow(edit);
            std::puts("MSHTML SCOPED KEYBOARD ROUTING PASSED");
        }
        CHECK(p.view->Show(FALSE) == S_OK);
        CHECK(!IsWindowVisible(p.viewport) && !IsWindowVisible(p.renderer));
        CHECK(host.active == nullptr);
        unsigned off = host.ui_off;
        CHECK(p.view->Show(FALSE) == S_OK && host.ui_off == off);
        pump(300); // Timer/WM_SIZE must not resurrect hidden windows.
        CHECK(!IsWindowVisible(p.viewport) && !IsWindowVisible(p.renderer));
    }
    CHECK(host.inplace_on == 2 && host.inplace_off == 0);
    CHECK(host.ui_on == 2 && host.ui_off == 2);
    for(unsigned i = 0; i < 4; ++i) {
        Page &p = pages[i % 2], &other = pages[(i + 1) % 2];
        CHECK(other.view->Show(FALSE) == S_OK);
        CHECK(p.view->UIActivate(TRUE) == S_OK);
        CHECK(p.view->Show(TRUE) == S_OK);
        pump(300);
        CHECK(IsWindowVisible(p.renderer) && !IsWindowVisible(other.renderer));
        CHECK(ready(p) && ready(other));
    }
    Page &p = pages[1];
    CHECK(p.inplace->InPlaceDeactivate() == S_OK);
    CHECK(host.inplace_off == 1 && !host.active);
    CHECK(!IsWindowVisible(p.viewport) && IsWindow(p.renderer));
    CHECK(ready(p)); // The old implementation disconnected this page here.
    CHECK(p.inplace->InPlaceDeactivate() == S_OK && host.inplace_off == 1);
    CHECK(p.view->UIActivate(TRUE) == S_OK);
    CHECK(p.view->Show(TRUE) == S_OK);
    HWND same_window = nullptr;
    CHECK(p.inplace->GetWindow(&same_window) == S_OK && same_window == p.viewport);
    CHECK(IsWindowVisible(p.renderer) && ready(p));
    CHECK(host.inplace_on == 3);
    CHECK(p.view->CloseView(0) == S_OK);
    CHECK(!IsWindowVisible(p.renderer) && ready(p));
    IOleInPlaceSite *site = nullptr;
    CHECK(FAILED(p.view->GetInPlaceSite(&site)) && !site);
    CHECK(p.view->UIActivate(TRUE) == E_UNEXPECTED);
    for(Page &page : pages) {
        CHECK(page.ole->Close(OLECLOSE_NOSAVE) == S_OK);
        CHECK(!IsWindow(page.viewport));
        page.view->SetInPlaceSite(nullptr);
        page.ole->Release(); page.inplace->Release(); page.view->Release();
        page.doc->Release(); page.html->Release();
    }
    CHECK(host.inplace_on == host.inplace_off && host.ui_on == host.ui_off);
    CHECK(host.active_on == host.active_off && host.refs == 1);
    {
        // IE's real DoVerb path asks IOleDocumentSite to establish the view;
        // directly showing the renderer omits the container's command cache.
        Host activation_host(window);
        IOleObject *object = nullptr;
        IOleDocument *document = nullptr;
        IPersistMoniker *persist = nullptr;
        IMoniker *moniker = nullptr;
        CHECK(factory->CreateInstance(nullptr, IID_IOleObject, reinterpret_cast<void **>(&object)) == S_OK);
        CHECK(object->QueryInterface(IID_IOleDocument, reinterpret_cast<void **>(&document)) == S_OK);
        activation_host.activation_document = document;
        CHECK(object->QueryInterface(IID_IPersistMoniker, reinterpret_cast<void **>(&persist)) == S_OK);
        CHECK(CreateURLMoniker(nullptr, L"about:blank#document-site", &moniker) == S_OK);
        CHECK(persist->Load(TRUE, moniker, nullptr, STGM_READ) == S_OK);
        moniker->Release(); persist->Release();
        activation_host.activation_result = E_ABORT;
        CHECK(object->DoVerb(OLEIVERB_SHOW, nullptr, &activation_host, 0, window, &rect) == E_ABORT);
        CHECK(activation_host.activate_me == 1 && activation_host.inplace_on == 0);
        activation_host.activation_result = S_OK;
        CHECK(object->DoVerb(OLEIVERB_SHOW, nullptr, &activation_host, 0, window, &rect) == S_OK);
        CHECK(activation_host.activate_me == 2 && activation_host.activated_commands && activation_host.active);
        OLECMD command = { OLECMDID_REFRESH, 0 };
        CHECK(activation_host.activated_commands->QueryStatus(nullptr, 1, &command, nullptr) == S_OK);
        CHECK(command.cmdf == (OLECMDF_SUPPORTED | OLECMDF_ENABLED));
        CHECK(object->DoVerb(12345, nullptr, &activation_host, 0, window, &rect) == OLEOBJ_E_INVALIDVERB);
        CHECK(activation_host.activate_me == 2);
        CHECK(object->Close(OLECLOSE_NOSAVE) == S_OK);
        CHECK(!activation_host.active);
        activation_host.activated_commands->Release();
        activation_host.activated_view->Release();
        CHECK(object->SetClientSite(nullptr) == S_OK);
        document->Release(); object->Release();
        CHECK(activation_host.refs == 1 && activation_host.inplace_on == activation_host.inplace_off);
        std::puts("MSHTML DOCUMENT SITE ACTIVATION HANDSHAKE PASSED");
    }
    factory->Release();
    CHECK(can_unload() == S_OK);
    DestroyWindow(window); FreeLibrary(module); CoUninitialize();
    std::puts("MSHTML VIEW LIFECYCLE PASSED");
    return 0;
}
