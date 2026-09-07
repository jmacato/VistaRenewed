#define _WIN32_WINNT 0x0600
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <string.h>

/* Bounded startup observer: never kills DWM or consumes its exceptions. */
int main(int argc, char **argv)
{
    HANDLE token, process = NULL;
    TOKEN_PRIVILEGES privileges = {0};
    DWORD pid = 0, deadline = GetTickCount() + 120000;
    BOOL initialBreakpoint = TRUE, attached = FALSE;
    BOOL launchMode = argc > 2 && strcmp(argv[1], "--launch") == 0;
    FILE *log = fopen(launchMode ? "runtime-observer.log" :
                      "C:\\Windows\\Temp\\dwm-observer.log", "w");
    if (!log) return 1;
    setvbuf(log, NULL, _IONBF, 0);
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        privileges.PrivilegeCount = 1;
        LookupPrivilegeValueA(NULL, SE_DEBUG_NAME, &privileges.Privileges[0].Luid);
        privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(token, FALSE, &privileges, 0, NULL, NULL);
        CloseHandle(token);
    }
    /* Optional diagnostic launch captures startup before a short-lived probe
     * can exit. The same exception policy and bounded detach apply. */
    if (launchMode) {
        char command[4096] = {0};
        STARTUPINFOA startup = {0};
        PROCESS_INFORMATION child = {0};
        size_t used = 0;
        startup.cb = sizeof(startup);
        for (int i = 2; i < argc; ++i) {
            int count = snprintf(command + used, sizeof(command) - used,
                                 "\"%s\" ", argv[i]);
            if (count < 0 || (size_t)count >= sizeof(command) - used) {
                fclose(log);
                return 2;
            }
            used += count;
        }
        if (!CreateProcessA(NULL, command, NULL, NULL, FALSE,
                            DEBUG_ONLY_THIS_PROCESS, NULL, NULL, &startup, &child)) {
            fprintf(log, "LAUNCH error=%lu\n", GetLastError());
            fclose(log);
            return 2;
        }
        DebugSetProcessKillOnExit(FALSE);
        pid = child.dwProcessId;
        process = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
        CloseHandle(child.hThread);
        CloseHandle(child.hProcess);
        attached = TRUE;
        fprintf(log, "LAUNCHED pid=%lu\n", (unsigned long)pid);
    }
    fprintf(log, "WAIT dwm.exe\n");
    while ((LONG)(GetTickCount() - deadline) < 0 && !attached) {
        PROCESSENTRY32 entry = {0};
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        entry.dwSize = sizeof(entry);
        if (snapshot != INVALID_HANDLE_VALUE && Process32First(snapshot, &entry)) {
            do {
                if (_stricmp(entry.szExeFile, "dwm.exe") == 0) {
                    pid = entry.th32ProcessID;
                    if (DebugActiveProcess(pid)) {
                        DebugSetProcessKillOnExit(FALSE);
                        attached = TRUE;
                        process = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
                        fprintf(log, "ATTACHED pid=%lu\n", (unsigned long)pid);
                        break;
                    }
                }
            } while (Process32Next(snapshot, &entry));
        }
        if (snapshot != INVALID_HANDLE_VALUE) CloseHandle(snapshot);
        if (!attached) Sleep(10);
    }
    while (attached && (LONG)(GetTickCount() - deadline) < 0) {
        DEBUG_EVENT event;
        DWORD disposition = DBG_CONTINUE;
        if (!WaitForDebugEvent(&event, 100)) {
            if (GetLastError() != ERROR_SEM_TIMEOUT) break;
            continue;
        }
        switch (event.dwDebugEventCode) {
        case OUTPUT_DEBUG_STRING_EVENT: {
            char bytes[8192] = {0};
            SIZE_T received = 0;
            SIZE_T count = event.u.DebugString.nDebugStringLength;
            if (event.u.DebugString.fUnicode) count *= sizeof(WCHAR);
            if (count > sizeof(bytes) - 2) count = sizeof(bytes) - 2;
            if (process && ReadProcessMemory(process, event.u.DebugString.lpDebugStringData,
                                               bytes, count, &received)) {
                if (event.u.DebugString.fUnicode) {
                    char text[16384];
                    int size = WideCharToMultiByte(CP_UTF8, 0, (WCHAR *)bytes,
                                                  (int)(received / 2), text,
                                                  sizeof(text) - 1, NULL, NULL);
                    text[size] = 0;
                    fprintf(log, "DBG tid=%lu %s\n", (unsigned long)event.dwThreadId, text);
                } else {
                    fprintf(log, "DBG tid=%lu %s\n", (unsigned long)event.dwThreadId, bytes);
                }
            }
            break;
        }
        case EXCEPTION_DEBUG_EVENT:
            fprintf(log, "EXCEPTION tid=%lu code=%08lx address=%p first=%lu\n",
                    (unsigned long)event.dwThreadId,
                    (unsigned long)event.u.Exception.ExceptionRecord.ExceptionCode,
                    event.u.Exception.ExceptionRecord.ExceptionAddress,
                    (unsigned long)event.u.Exception.dwFirstChance);
            disposition = DBG_EXCEPTION_NOT_HANDLED;
            if (initialBreakpoint && event.u.Exception.dwFirstChance &&
                event.u.Exception.ExceptionRecord.ExceptionCode == EXCEPTION_BREAKPOINT) {
                initialBreakpoint = FALSE;
                disposition = DBG_CONTINUE;
            }
            break;
        case CREATE_PROCESS_DEBUG_EVENT:
            fprintf(log, "IMAGE base=%p\n", event.u.CreateProcessInfo.lpBaseOfImage);
            if (event.u.CreateProcessInfo.hFile) CloseHandle(event.u.CreateProcessInfo.hFile);
            CloseHandle(event.u.CreateProcessInfo.hProcess);
            CloseHandle(event.u.CreateProcessInfo.hThread);
            break;
        case CREATE_THREAD_DEBUG_EVENT:
            CloseHandle(event.u.CreateThread.hThread);
            break;
        case LOAD_DLL_DEBUG_EVENT: {
            char path[1024] = {0};
            if (event.u.LoadDll.hFile)
                GetFinalPathNameByHandleA(event.u.LoadDll.hFile, path, sizeof(path), 0);
            fprintf(log, "DLL base=%p %s\n", event.u.LoadDll.lpBaseOfDll, path);
            if (event.u.LoadDll.hFile) CloseHandle(event.u.LoadDll.hFile);
            break;
        }
        case EXIT_PROCESS_DEBUG_EVENT:
            fprintf(log, "EXIT code=%08lx\n", (unsigned long)event.u.ExitProcess.dwExitCode);
            attached = FALSE;
            break;
        }
        ContinueDebugEvent(event.dwProcessId, event.dwThreadId, disposition);
    }
    if (attached) fprintf(log, "DETACH result=%d\n", DebugActiveProcessStop(pid));
    if (process) CloseHandle(process);
    fprintf(log, "DONE\n");
    fclose(log);
    return 0;
}
