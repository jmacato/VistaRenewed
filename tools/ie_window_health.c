#define UNICODE
#define _UNICODE
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <string.h>

static BOOL inspect_toolbars;
static void inspect_toolbar(HWND window, DWORD pid)
{
    HANDLE process;
    void *remote;
    DWORD_PTR count, result;
    unsigned index;
    if(!SendMessageTimeoutW(window, TB_BUTTONCOUNT, 0, 0, SMTO_ABORTIFHUNG, 1000, &count) || count > 100) return;
    printf("TOOLBAR hwnd=%p pid=%lu enabled=%d visible=%d count=%lu\n", window,
           (unsigned long)pid, IsWindowEnabled(window), IsWindowVisible(window), (unsigned long)count);
    /* This diagnostic is x86, like the IE process. Allocate only a scratch
     * output buffer for documented read-only toolbar messages; never patch code. */
    process = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_READ, FALSE, pid);
    if(!process) { printf("TOOLBAR_OPEN_ERROR=%lu\n", (unsigned long)GetLastError()); return; }
    remote = VirtualAllocEx(process, NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if(!remote) { CloseHandle(process); return; }
    for(index = 0; index < count; ++index) {
        TBBUTTON button;
        RECT rect;
        SIZE_T read;
        if(!SendMessageTimeoutW(window, TB_GETBUTTON, index, (LPARAM)remote,
            SMTO_ABORTIFHUNG, 1000, &result) || !result ||
           !ReadProcessMemory(process, remote, &button, sizeof(button), &read) || read != sizeof(button)) break;
        ZeroMemory(&rect, sizeof(rect));
        if(SendMessageTimeoutW(window, TB_GETITEMRECT, index, (LPARAM)remote,
            SMTO_ABORTIFHUNG, 1000, &result) && result &&
           ReadProcessMemory(process, remote, &rect, sizeof(rect), &read) && read == sizeof(rect))
            MapWindowPoints(window, NULL, (POINT *)&rect, 2);
        printf("TOOLBAR_BUTTON index=%u command=%d state=%02x style=%02x rect=%ld,%ld,%ld,%ld\n",
               index, button.idCommand, button.fsState, button.fsStyle,
               (long)rect.left, (long)rect.top, (long)rect.right, (long)rect.bottom);
    }
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    CloseHandle(process);
}

static BOOL CALLBACK inspect_child(HWND window, LPARAM unused)
{
    WCHAR name[128];
    DWORD pid, thread;
    GUITHREADINFO info = {0};
    (void)unused;
    if(!GetClassNameW(window, name, 128)) return TRUE;
    thread = GetWindowThreadProcessId(window, &pid);
    if(inspect_toolbars && !lstrcmpW(name, L"ToolbarWindow32")) inspect_toolbar(window, pid);
    info.cbSize = sizeof(info);
    if(wcsstr(name, L"Chrome") || wcsstr(name, L"Triton")) {
        RECT rect;
        wprintf(L"EMBED_CHILD hwnd=%p class=%ls pid=%lu thread=%lu visible=%d\n",
                window, name, (unsigned long)pid, (unsigned long)thread, IsWindowVisible(window));
        if(!lstrcmpW(name, L"TritonMshtmlViewport") && GetWindowRect(window, &rect))
            printf("IE_VIEWPORT_RECT=%ld,%ld,%ld,%ld\n", rect.left, rect.top, rect.right, rect.bottom);
        if(GetGUIThreadInfo(thread, &info))
            printf("EMBED_INPUT focus=%p active=%p capture=%p\n", info.hwndFocus,
                   info.hwndActive, info.hwndCapture);
    }
    return TRUE;
}

static BOOL CALLBACK inspect(HWND window, LPARAM unused)
{
    WCHAR name[128];
    DWORD pid, thread;
    DWORD_PTR result;
    GUITHREADINFO info = {0};
    (void)unused;
    if(!GetClassNameW(window, name, 128) || lstrcmpW(name, L"IEFrame")) return TRUE;
    thread = GetWindowThreadProcessId(window, &pid);
    info.cbSize = sizeof(info);
    printf("IE_HEALTH pid=%lu hwnd=%p responsive=%d\n", (unsigned long)pid, window,
           !!SendMessageTimeoutW(window, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 1000, &result));
    {
        WCHAR title[1024];
        RECT rect;
        if(GetWindowTextW(window, title, ARRAYSIZE(title))) wprintf(L"IE_TITLE=%ls\n", title);
        if(GetWindowRect(window, &rect))
            printf("IE_RECT=%ld,%ld,%ld,%ld\n", rect.left, rect.top, rect.right, rect.bottom);
    }
    if(GetGUIThreadInfo(thread, &info)) {
        printf("IE_INPUT active=%p focus=%p capture=%p flags=%lu\n",
               info.hwndActive, info.hwndFocus, info.hwndCapture, (unsigned long)info.flags);
        if(info.hwndFocus && GetClassNameW(info.hwndFocus, name, 128))
            wprintf(L"IE_FOCUS_CLASS=%ls\n", name);
    }
    EnumChildWindows(window, inspect_child, 0);
    return TRUE;
}

int main(int argc, char **argv)
{
    POINT point = {508, 44};
    HWND hit = WindowFromPoint(point);
    WCHAR name[128];
    if(argc != 1 && (argc != 2 || strcmp(argv[1], "--toolbars"))) return 2;
    inspect_toolbars = argc == 2;
    while(hit) {
        if(GetClassNameW(hit, name, 128))
            wprintf(L"REFRESH_HIT hwnd=%p class=%ls\n", hit, name);
        hit = GetParent(hit);
    }
    EnumWindows(inspect, 0);
    return 0;
}
