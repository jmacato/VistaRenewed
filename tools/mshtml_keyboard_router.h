/* Private keyboard bridge for the out-of-process renderer. No key logging.
 * The hook runs on a dedicated message thread: never perform COM, CDP, file
 * I/O, or synchronous cross-thread messages inside its callback. */
#define MSHTML_KEYBOARD_ROUTE_PROPERTY L"TritonMshtmlKeyboardRoute"
#define MSHTML_FOCUS_ADDRESS_MESSAGE (WM_APP + 72)

static SRWLOCK keyboard_router_lock = SRWLOCK_INIT;
static HANDLE keyboard_router_thread, keyboard_router_stop, keyboard_router_ready;
static unsigned keyboard_router_users;
static DWORD keyboard_router_error;
static HWND keyboard_router_forwarded[256];

static BOOL is_container_shortcut(UINT key, BOOL control, BOOL alt, BOOL windows)
{
    if(windows || (control && alt)) return FALSE; /* Preserve Win keys and AltGr. */
    if(control) return key == 'T' || key == 'L' || key == 'N' || key == 'W' ||
                       key == VK_TAB || key == 'R';
    if(alt) return key == VK_LEFT || key == VK_RIGHT || key == VK_HOME || key == VK_F4;
    return key == VK_F5 || key == VK_F6 || key == VK_F11;
}

static LRESULT CALLBACK keyboard_router_hook(int code, WPARAM message, LPARAM parameter)
{
    const KBDLLHOOKSTRUCT *key = (const KBDLLHOOKSTRUCT *)parameter;
    GUITHREADINFO info;
    HWND window, root;
    WCHAR name[80];
    DWORD pid;
    LPARAM bits;
    BOOL up;
    if(code != HC_ACTION || key->vkCode >= ARRAYSIZE(keyboard_router_forwarded))
        return CallNextHookEx(NULL, code, message, parameter);
    up = message == WM_KEYUP || message == WM_SYSKEYUP;
    bits = 1 | (key->scanCode << 16) | ((key->flags & LLKHF_EXTENDED) ? (1L << 24) : 0) |
           ((key->flags & LLKHF_ALTDOWN) ? (1L << 29) : 0);
    if(keyboard_router_forwarded[key->vkCode]) {
        root = keyboard_router_forwarded[key->vkCode];
        if(up) {
            keyboard_router_forwarded[key->vkCode] = NULL;
            PostMessageW(root, (UINT)message, key->vkCode, bits | (3UL << 30));
        }
        return 1; /* Suppress autorepeat and the matching key-up. */
    }
    if(up || !is_container_shortcut(key->vkCode, !!(GetAsyncKeyState(VK_CONTROL) & 0x8000),
        !!(key->flags & LLKHF_ALTDOWN),
        !!((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000)))
        return CallNextHookEx(NULL, code, message, parameter);
    ZeroMemory(&info, sizeof(info)); info.cbSize = sizeof(info);
    if(!GetGUIThreadInfo(0, &info) || !info.hwndFocus)
        return CallNextHookEx(NULL, code, message, parameter);
    // Only the renderer's input HWND qualifies, not native IE edit controls.
    if(!GetClassNameW(info.hwndFocus, name, ARRAYSIZE(name)) ||
       lstrcmpW(name, L"Chrome_RenderWidgetHostHWND"))
        return CallNextHookEx(NULL, code, message, parameter);
    for(window = GetParent(info.hwndFocus); window; window = GetParent(window)) {
        GetWindowThreadProcessId(window, &pid);
        if(pid == GetCurrentProcessId() && GetPropW(window, MSHTML_KEYBOARD_ROUTE_PROPERTY) &&
           IsWindowVisible(window) && GetClassNameW(window, name, ARRAYSIZE(name)) &&
           !lstrcmpW(name, L"TritonMshtmlViewport")) {
            root = GetAncestor(window, GA_ROOT);
            GetWindowThreadProcessId(root, &pid);
            if(pid == GetCurrentProcessId() && key->vkCode == 'L' &&
               (GetAsyncKeyState(VK_CONTROL) & 0x8000) &&
               PostMessageW(window, MSHTML_FOCUS_ADDRESS_MESSAGE, 0, 0)) {
                keyboard_router_forwarded[key->vkCode] = root;
                return 1;
            }
            if(pid == GetCurrentProcessId() &&
               PostMessageW(root, (UINT)message, key->vkCode, bits)) {
                keyboard_router_forwarded[key->vkCode] = root;
                return 1;
            }
            break;
        }
    }
    return CallNextHookEx(NULL, code, message, parameter);
}

static DWORD WINAPI keyboard_router_main(void *parameter)
{
    HMODULE pin = parameter;
    HHOOK hook;
    ZeroMemory(keyboard_router_forwarded, sizeof(keyboard_router_forwarded));
    hook = SetWindowsHookExW(WH_KEYBOARD_LL, keyboard_router_hook, pin, 0);
    keyboard_router_error = hook ? ERROR_SUCCESS : GetLastError();
    SetEvent(keyboard_router_ready);
    if(hook) {
        for(;;) {
            DWORD wait = MsgWaitForMultipleObjects(1, &keyboard_router_stop, FALSE, INFINITE, QS_ALLINPUT);
            MSG message;
            if(wait == WAIT_OBJECT_0 || wait == WAIT_FAILED) break;
            while(PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        UnhookWindowsHookEx(hook);
    }
    FreeLibraryAndExitThread(pin, 0);
    return 0;
}

static void keyboard_router_reap(void)
{
    if(keyboard_router_thread && WaitForSingleObject(keyboard_router_thread, 0) == WAIT_OBJECT_0) {
        CloseHandle(keyboard_router_thread); keyboard_router_thread = NULL;
        CloseHandle(keyboard_router_stop); keyboard_router_stop = NULL;
        CloseHandle(keyboard_router_ready); keyboard_router_ready = NULL;
    }
}

static BOOL keyboard_router_acquire(void)
{
    HMODULE pin;
    BOOL ok = FALSE;
    AcquireSRWLockExclusive(&keyboard_router_lock);
    keyboard_router_reap();
    if(keyboard_router_users) { ++keyboard_router_users; ok = TRUE; goto done; }
    if(keyboard_router_thread) goto done; // A timed-out shutdown still owns its resources.
    keyboard_router_stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    keyboard_router_ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    if(!keyboard_router_stop || !keyboard_router_ready) goto failed;
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        (LPCWSTR)(ULONG_PTR)keyboard_router_main, &pin)) goto failed;
    keyboard_router_thread = CreateThread(NULL, 0, keyboard_router_main, pin, 0, NULL);
    if(!keyboard_router_thread) { FreeLibrary(pin); goto failed; }
    if(WaitForSingleObject(keyboard_router_ready, 2000) != WAIT_OBJECT_0 || keyboard_router_error) {
        SetEvent(keyboard_router_stop);
        WaitForSingleObject(keyboard_router_thread, 2000);
        keyboard_router_reap();
        goto done;
    }
    keyboard_router_users = 1;
    ok = TRUE;
    goto done;
failed:
    if(keyboard_router_stop) CloseHandle(keyboard_router_stop);
    if(keyboard_router_ready) CloseHandle(keyboard_router_ready);
    keyboard_router_stop = keyboard_router_ready = NULL;
done:
    ReleaseSRWLockExclusive(&keyboard_router_lock);
    return ok;
}

static void keyboard_router_release(void)
{
    AcquireSRWLockExclusive(&keyboard_router_lock);
    if(keyboard_router_users && !--keyboard_router_users) {
        SetEvent(keyboard_router_stop);
        WaitForSingleObject(keyboard_router_thread, 2000);
        keyboard_router_reap();
    }
    ReleaseSRWLockExclusive(&keyboard_router_lock);
}

static BOOL keyboard_router_idle(void)
{
    BOOL idle;
    AcquireSRWLockExclusive(&keyboard_router_lock);
    keyboard_router_reap();
    idle = !keyboard_router_users && !keyboard_router_thread;
    ReleaseSRWLockExclusive(&keyboard_router_lock);
    return idle;
}
