/* Exercise Chromium's inherited-handle DevTools transport on Vista. */
#define _WIN32_WINNT 0x0600
#include <windows.h>
#include <stdio.h>
#include <string.h>

/* Probe-only framing: consume whole NUL frames, including unsolicited events. */
static int exchange(HANDLE write_pipe, HANDLE read_pipe, const char *request,
                    unsigned id, char *reply, size_t capacity)
{
    DWORD written, available, count, start = GetTickCount();
    size_t used = 0;
    char prefix[40], byte;
    sprintf(prefix, "\"id\":%u,", id);
    if(!WriteFile(write_pipe, request, (DWORD)strlen(request) + 1, &written, NULL)) return 0;
    while(GetTickCount() - start < 10000) {
        if(!PeekNamedPipe(read_pipe, NULL, 0, NULL, &available, NULL)) return 0;
        if(!available) { Sleep(10); continue; }
        if(!ReadFile(read_pipe, &byte, 1, &count, NULL) || count != 1) return 0;
        if(used + 1 >= capacity) return 0;
        reply[used++] = byte;
        if(!byte) {
            if(strstr(reply, prefix)) { puts(reply); return !strstr(reply, "\"error\":"); }
            used = 0;
        }
    }
    return 0;
}

static int identifier(const char *reply, const char *key, char *out, size_t capacity)
{
    const char *value = strstr(reply, key), *end;
    size_t length;
    if(!value) return 0;
    value += strlen(key);
    end = strchr(value, '"');
    if(!end) return 0;
    length = (size_t)(end - value);
    if(!length || length >= capacity) return 0;
    memcpy(out, value, length); out[length] = 0;
    return 1;
}

typedef struct WindowMatch {
    DWORD pid;
    RECT bounds;
    HWND window;
    unsigned matches;
} WindowMatch;

static BOOL CALLBACK match_window(HWND window, LPARAM parameter)
{
    WindowMatch *match = (WindowMatch *)parameter;
    DWORD pid;
    RECT rect;
    WCHAR name[80];
    GetWindowThreadProcessId(window, &pid);
    if(pid != match->pid || !IsWindowVisible(window) ||
       !GetClassNameW(window, name, 80) || lstrcmpW(name, L"Chrome_WidgetWin_1")) return TRUE;
    if(GetWindowRect(window, &rect) && EqualRect(&rect, &match->bounds)) {
        match->window = window;
        ++match->matches;
    }
    return TRUE;
}

int main(void)
{
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
    HANDLE child_read = NULL, host_write = NULL, host_read = NULL, child_write = NULL;
    STARTUPINFOW startup = {0};
    PROCESS_INFORMATION process = {0};
    WCHAR command[1024];
    char reply[16384];
    char target[128], session[128], message[2048];
    char second_target[128], second_session[128];
    const char request[] = "{\"id\":1,\"method\":\"Browser.getVersion\"}";
    const char close_request[] = "{\"id\":99,\"method\":\"Browser.close\"}";
    int result = 1;
    unsigned attempt;
    const char *page;
    if(!CreatePipe(&child_read, &host_write, &sa, 0) ||
       !CreatePipe(&host_read, &child_write, &sa, 0)) goto done;
    if(!SetHandleInformation(host_write, HANDLE_FLAG_INHERIT, 0) ||
       !SetHandleInformation(host_read, HANDLE_FLAG_INHERIT, 0)) goto done;
    startup.cb = sizeof(startup);
    wsprintfW(command, L"C:\\TritonSupermium\\chrome.exe --app=file:///D:/visible-supermium.html "
        L"--no-first-run --disable-gpu --disable-background-networking "
        L"--remote-debugging-pipe --remote-debugging-io-pipes=%lu,%lu "
        L"--user-data-dir=C:\\TritonSupermiumBridge\\PipeProbeProfile",
        (unsigned long)(ULONG_PTR)child_read, (unsigned long)(ULONG_PTR)child_write);
    if(!CreateProcessW(L"C:\\TritonSupermium\\chrome.exe", command, NULL, NULL, TRUE,
                       0, NULL, NULL, &startup, &process)) goto done;
    CloseHandle(child_read); child_read = NULL;
    CloseHandle(child_write); child_write = NULL;
    if(!exchange(host_write, host_read, request, 1, reply, sizeof(reply))) goto done;
    target[0] = 0;
    for(attempt = 0; attempt < 50 && !target[0]; ++attempt) {
        if(!exchange(host_write, host_read, "{\"id\":2,\"method\":\"Target.getTargets\"}",
                     2, reply, sizeof(reply))) goto done;
        page = strstr(reply, "\"type\":\"page\"");
        if(page && strstr(reply, "\"url\":\"file:///D:/visible-supermium.html\"")) {
            /* Dedicated probe profile: require exactly one page, so target
             * selection cannot silently choose an unrelated tab. */
            if(strstr(page + 1, "\"type\":\"page\"")) goto done;
            if(strstr(strstr(reply, "\"targetId\":") + 1, "\"targetId\":")) goto done;
            if(!identifier(reply, "\"targetId\":\"", target, sizeof(target))) goto done;
        } else Sleep(100);
    }
    if(!target[0]) goto done;
    puts("SUPERMIUM EXISTING APP PAGE DISCOVERED");
    sprintf(message, "{\"id\":3,\"method\":\"Target.attachToTarget\",\"params\":{\"targetId\":\"%s\",\"flatten\":true}}", target);
    if(!exchange(host_write, host_read, message, 3, reply, sizeof(reply)) ||
       !identifier(reply, "\"sessionId\":\"", session, sizeof(session))) goto done;
    sprintf(message, "{\"id\":4,\"sessionId\":\"%s\",\"method\":\"Page.navigate\",\"params\":{\"url\":\"data:text/html,<title>PIPE_NAVIGATION_OK</title><p>Vista pipe navigation</p>\"}}", session);
    if(!exchange(host_write, host_read, message, 4, reply, sizeof(reply))) goto done;
    Sleep(500);
    sprintf(message, "{\"id\":5,\"sessionId\":\"%s\",\"method\":\"Runtime.evaluate\",\"params\":{\"expression\":\"document.title\",\"returnByValue\":true}}", session);
    if(!exchange(host_write, host_read, message, 5, reply, sizeof(reply)) ||
       !strstr(reply, "\"value\":\"PIPE_NAVIGATION_OK\"")) goto done;
    puts("SUPERMIUM PERSISTENT PAGE NAVIGATION AND DOM READ PASSED");
    {
        STARTUPINFOW app_start = {0};
        PROCESS_INFORMATION app_process = {0};
        WCHAR app_command[] = L"C:\\TritonSupermium\\chrome.exe --app=file:///C:/TritonSupermiumBridge/ie-input.html --user-data-dir=C:\\TritonSupermiumBridge\\PipeProbeProfile --no-first-run --disable-gpu";
        app_start.cb = sizeof(app_start);
        if(!CreateProcessW(L"C:\\TritonSupermium\\chrome.exe", app_command, NULL, NULL,
                           FALSE, 0, NULL, NULL, &app_start, &app_process)) goto done;
        CloseHandle(app_process.hThread);
        WaitForSingleObject(app_process.hProcess, 3000);
        CloseHandle(app_process.hProcess);
    }
    second_target[0] = 0;
    for(attempt = 0; attempt < 50 && !second_target[0]; ++attempt) {
        const char *object;
        if(!exchange(host_write, host_read, "{\"id\":6,\"method\":\"Target.getTargets\"}",
                     6, reply, sizeof(reply))) goto done;
        object = strstr(reply, "\"url\":\"file:///C:/TritonSupermiumBridge/ie-input.html\"");
        if(object) {
            while(object > reply && *object != '{') --object;
            if(!identifier(object, "\"targetId\":\"", second_target, sizeof(second_target))) goto done;
        } else Sleep(100);
    }
    if(!second_target[0] || !strcmp(target, second_target)) goto done;
    sprintf(message, "{\"id\":7,\"method\":\"Target.attachToTarget\",\"params\":{\"targetId\":\"%s\",\"flatten\":true}}", second_target);
    if(!exchange(host_write, host_read, message, 7, reply, sizeof(reply)) ||
       !identifier(reply, "\"sessionId\":\"", second_session, sizeof(second_session)) ||
       !strcmp(session, second_session)) goto done;
    sprintf(message, "{\"id\":8,\"sessionId\":\"%s\",\"method\":\"Page.navigate\",\"params\":{\"url\":\"data:text/html,<title>SECOND_TARGET_OK</title>\"}}", second_session);
    if(!exchange(host_write, host_read, message, 8, reply, sizeof(reply))) goto done;
    Sleep(500);
    sprintf(message, "{\"id\":9,\"sessionId\":\"%s\",\"method\":\"Runtime.evaluate\",\"params\":{\"expression\":\"document.title\",\"returnByValue\":true}}", second_session);
    if(!exchange(host_write, host_read, message, 9, reply, sizeof(reply)) ||
       !strstr(reply, "\"value\":\"SECOND_TARGET_OK\"")) goto done;
    sprintf(message, "{\"id\":12,\"method\":\"Browser.getWindowForTarget\",\"params\":{\"targetId\":\"%s\"}}", target);
    if(!exchange(host_write, host_read, message, 12, reply, sizeof(reply))) goto done;
    sprintf(message, "{\"id\":13,\"method\":\"Browser.getWindowForTarget\",\"params\":{\"targetId\":\"%s\"}}", second_target);
    if(!exchange(host_write, host_read, message, 13, reply, sizeof(reply))) goto done;
    {
        const char *value = strstr(reply, "\"windowId\":");
        unsigned window_id;
        WindowMatch match;
        if(!value || sscanf(value, "\"windowId\":%u", &window_id) != 1) goto done;
        /* Broker owns this new, not-yet-embedded app window. Assign unique
         * bounds through its target's CDP window ID, then match only its PID.
         * Never use a page-controlled title as window identity. */
        sprintf(message, "{\"id\":14,\"method\":\"Browser.setWindowBounds\",\"params\":{\"windowId\":%u,\"bounds\":{\"left\":37,\"top\":53,\"width\":617,\"height\":431}}}", window_id);
        if(!exchange(host_write, host_read, message, 14, reply, sizeof(reply))) goto done;
        memset(&match, 0, sizeof(match));
        match.pid = process.dwProcessId;
        SetRect(&match.bounds, 37, 53, 654, 484);
        for(attempt = 0; attempt < 30; ++attempt) {
            match.matches = 0;
            EnumWindows(match_window, (LPARAM)&match);
            if(match.matches == 1) break;
            Sleep(100);
        }
        if(match.matches != 1) goto done;
        printf("SUPERMIUM TARGET HWND MATCH PASSED hwnd=%p pid=%lu\n",
               match.window, (unsigned long)match.pid);
    }
    puts("SUPERMIUM SECOND WINDOW READY FOR VISUAL INSPECTION");
    fflush(stdout);
    Sleep(8000);
    sprintf(message, "{\"id\":10,\"method\":\"Target.closeTarget\",\"params\":{\"targetId\":\"%s\"}}", second_target);
    if(!exchange(host_write, host_read, message, 10, reply, sizeof(reply)) ||
       !strstr(reply, "\"success\":true")) goto done;
    sprintf(message, "{\"id\":11,\"sessionId\":\"%s\",\"method\":\"Runtime.evaluate\",\"params\":{\"expression\":\"document.title\",\"returnByValue\":true}}", session);
    if(!exchange(host_write, host_read, message, 11, reply, sizeof(reply)) ||
       !strstr(reply, "\"value\":\"PIPE_NAVIGATION_OK\"")) goto done;
    puts("SUPERMIUM TWO TARGETS AND INDEPENDENT CLOSE PASSED");
    result = 0;
    exchange(host_write, host_read, close_request, 99, reply, sizeof(reply));
done:
    if(result) printf("SUPERMIUM PIPE PROBE FAILED win32=%lu\n", GetLastError());
    else puts("SUPERMIUM PIPE ROUNDTRIP PASSED");
    if(host_write) CloseHandle(host_write);
    if(host_read) CloseHandle(host_read);
    if(child_read) CloseHandle(child_read);
    if(child_write) CloseHandle(child_write);
    if(process.hThread) CloseHandle(process.hThread);
    if(process.hProcess) { WaitForSingleObject(process.hProcess, 3000); CloseHandle(process.hProcess); }
    return result;
}
