#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdio.h>

static HWND edit;
static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if(message == WM_CREATE) {
        edit = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                             20, 40, 500, 50, window, NULL, GetModuleHandleW(NULL), NULL);
        SetTimer(window, 1, 12000, NULL);
    } else if(message == WM_TIMER) {
        WCHAR text[256];
        GetWindowTextW(edit, text, 256);
        wprintf(L"KEYBOARD_TEXT=[%ls] foreground=%d focus=%d\n", text,
                GetForegroundWindow() == window, GetFocus() == edit);
        fflush(stdout);
        DestroyWindow(window);
        return 0;
    } else if(message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

int main(void)
{
    WNDCLASSW wc = {0};
    MSG message;
    HWND window;
    wc.lpfnWndProc = procedure;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"TritonKeyboardProbe";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    if(!RegisterClassW(&wc)) return 1;
    window = CreateWindowW(wc.lpszClassName, L"Native keyboard positive control",
                           WS_OVERLAPPEDWINDOW | WS_VISIBLE, 80, 120, 580, 200,
                           NULL, NULL, wc.hInstance, NULL);
    if(!window || !edit) return 1;
    SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    SetForegroundWindow(window);
    SetFocus(edit);
    printf("KEYBOARD_READY foreground=%d focus=%d\n",
           GetForegroundWindow() == window, GetFocus() == edit);
    fflush(stdout);
    while(GetMessageW(&message, NULL, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}
