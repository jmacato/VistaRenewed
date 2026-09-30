/* Keep Chromium's actual WindowProxy and security boundary. Only HWND hosting
 * changes: a popup renderer belongs to a native window in the IE process. */
typedef struct MshtmlPopup {
    struct MshtmlPopup *next;
    ProbeDocument *opener;
    HWND host, renderer;
    DWORD input_thread;
    char target[80];
} MshtmlPopup;
static MshtmlPopup *mshtml_popups;

static BOOL popup_identifier(const char *entry, const char *key, char *out)
{
    const char *value = mj_member(entry, key);
    unsigned count = 0;
    if(!value || *value++ != '"') return FALSE;
    while(mj_hex(*value) >= 0 && count < 79) out[count++] = *value++;
    out[count] = 0;
    return count && *value == '"';
}

static void popup_layout(MshtmlPopup *popup)
{
    RECT rect;
    int trim = GetSystemMetrics(SM_CYCAPTION) + 2 * GetSystemMetrics(SM_CYFRAME);
    HRGN region;
    if(!IsWindow(popup->renderer) || !GetClientRect(popup->host, &rect)) return;
    SetWindowPos(popup->renderer, HWND_TOP, 0, -trim, rect.right, rect.bottom + trim,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    region = CreateRectRgn(0, trim, rect.right, rect.bottom + trim);
    if(region && !SetWindowRgn(popup->renderer, region, TRUE)) DeleteObject(region);
}

static LRESULT CALLBACK popup_window_proc(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    MshtmlPopup *popup = (MshtmlPopup *)GetWindowLongPtrW(window, GWLP_USERDATA);
    if(message == WM_NCCREATE) {
        popup = ((CREATESTRUCTW *)lp)->lpCreateParams;
        popup->host = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)popup);
    }
    if(!popup) return DefWindowProcW(window, message, wp, lp);
    if(message == WM_SIZE) { popup_layout(popup); return 0; }
    if(message == WM_SETFOCUS) {
        HWND input = NULL;
        EnumChildWindows(popup->renderer, find_renderer_input, (LPARAM)&input);
        if(input) {
            DWORD thread = GetWindowThreadProcessId(input, NULL);
            if(!popup->input_thread && thread != GetCurrentThreadId() &&
               AttachThreadInput(GetCurrentThreadId(), thread, TRUE)) popup->input_thread = thread;
            SetFocus(input);
        }
        return 0;
    }
    if(message == WM_TIMER) {
        if(!IsWindow(popup->renderer)) DestroyWindow(window);
        else popup_layout(popup);
        return 0;
    }
    if(message == WM_CLOSE) {
        /* Chromium processes close(), unload and WindowProxy.closed itself. */
        if(IsWindow(popup->renderer)) PostMessageW(popup->renderer, WM_CLOSE, 0, 0);
        else DestroyWindow(window);
        return 0;
    }
    if(message == WM_NCDESTROY) {
        MshtmlPopup **link = &mshtml_popups;
        ProbeDocument *opener = popup->opener;
        KillTimer(window, 1);
        if(popup->input_thread) AttachThreadInput(GetCurrentThreadId(), popup->input_thread, FALSE);
        while(*link && *link != popup) link = &(*link)->next;
        if(*link) *link = popup->next;
        if(opener->runtime->live_pages) --opener->runtime->live_pages;
        if(IsWindow(opener->viewport_window))
            RedrawWindow(opener->viewport_window, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        HeapFree(GetProcessHeap(), 0, popup);
        document_release(opener);
    }
    return DefWindowProcW(window, message, wp, lp);
}

static HRESULT popup_attach(ProbeDocument *document, const char *target, BSTR title)
{
    char params[256], reply[8192];
    const char *id;
    unsigned window_id;
    TargetWindowMatch match;
    MshtmlPopup *popup;
    WNDCLASSW wc = {0};
    HRESULT hr;
    LONG style;
    snprintf(params, sizeof(params), "{\"targetId\":\"%s\"}", target);
    hr = document_pipe_call(document, "Browser.getWindowForTarget", params, NULL, reply, sizeof(reply));
    if(FAILED(hr)) return hr;
    id = mj_member(mj_member(reply, "result"), "windowId");
    if(!id || sscanf(id, "%u", &window_id) != 1) return E_FAIL;
    snprintf(params, sizeof(params), "{\"windowId\":%u,\"bounds\":{\"left\":37,\"top\":53,\"width\":617,\"height\":431}}", window_id);
    hr = document_pipe_call(document, "Browser.setWindowBounds", params, NULL, reply, sizeof(reply));
    if(FAILED(hr)) return hr;
    ZeroMemory(&match, sizeof(match)); match.pid = document->supermium_process_id;
    EnumWindows(match_staging_window, (LPARAM)&match);
    if(match.count != 1 || match.window == document->renderer_window) return S_FALSE;
    popup = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*popup));
    if(!popup) return E_OUTOFMEMORY;
    popup->opener = document; popup->renderer = match.window;
    strcpy(popup->target, target);
    wc.lpfnWndProc = popup_window_proc; wc.hInstance = module_instance;
    wc.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512)); wc.lpszClassName = L"TritonMshtmlPopup";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    if(!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        HeapFree(GetProcessHeap(), 0, popup); return E_FAIL;
    }
    document_add_ref(document); ++document->runtime->live_pages;
    popup->next = mshtml_popups; mshtml_popups = popup;
    popup->host = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName,
        title && *title ? title : L"Internet Explorer", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 640, 480, NULL, NULL, module_instance, popup);
    if(!popup->host) {
        mshtml_popups = popup->next; --document->runtime->live_pages;
        HeapFree(GetProcessHeap(), 0, popup); document_release(document); return E_FAIL;
    }
    ShowWindow(match.window, SW_HIDE);
    SetLastError(0);
    if(!SetParent(match.window, popup->host) && GetLastError()) {
        DestroyWindow(popup->host); return E_FAIL;
    }
    style = GetWindowLongW(match.window, GWL_STYLE);
    SetWindowLongW(match.window, GWL_STYLE,
        (style & ~(WS_POPUP | WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)) | WS_CHILD);
    popup_layout(popup);
    SetTimer(popup->host, 1, 100, NULL);
    ShowWindow(popup->host, SW_SHOW); SetForegroundWindow(popup->host);
    append_text(L"MSHTML_NATIVE_POPUP_ATTACHED\r\n");
    return S_OK;
}

static void document_receive_popups(ProbeDocument *document, CdpMessage *message)
{
    const char *array, *entry;
    document->popup_scan_request = 0;
    if(FAILED(message->status)) return;
    array = mj_member(mj_member(message->json, "result"), "targetInfos");
    if(!array || *array != '[') return;
    entry = mj_space(array + 1);
    while(entry && *entry == '{') {
        char opener[80], target[80];
        MshtmlPopup *popup;
        BSTR title = NULL;
        BOOL ours = popup_identifier(entry, "openerId", opener) &&
                    !strcmp(opener, document->page_target);
        if(!ours && popup_identifier(entry, "openerId", opener)) {
            for(popup = mshtml_popups; popup; popup = popup->next)
                if(popup->opener == document && !strcmp(popup->target, opener)) { ours = TRUE; break; }
        }
        if(ours && popup_identifier(entry, "targetId", target)) {
            for(popup = mshtml_popups; popup; popup = popup->next)
                if(popup->opener->runtime == document->runtime && !strcmp(popup->target, target)) break;
            mj_bstr(mj_member(entry, "title"), &title);
            if(popup) {
                if(title) SetWindowTextW(popup->host, title);
            } else popup_attach(document, target, title);
            SysFreeString(title);
        }
        entry = mj_skip(entry, 0);
        if(entry && *(entry = mj_space(entry)) == ',') entry = mj_space(entry + 1);
        else break;
    }
}

static void document_poll_popups(ProbeDocument *document)
{
    CdpRequest *request = NULL;
    HRESULT hr;
    if(!document->page_session[0] || !document->runtime->cdp || document->popup_scan_request ||
       (document->next_popup_scan && (LONG)(GetTickCount() - document->next_popup_scan) < 0)) return;
    document->next_popup_scan = GetTickCount() + 250;
    hr = cdp_submit(document->runtime->cdp, "Target.getTargets", "{}", document->page_session,
                     TRUE, 0, 5000, 0, &request);
    if(SUCCEEDED(hr)) {
        document->popup_scan_request = request->id;
        cdp_request_release(request);
    }
}
