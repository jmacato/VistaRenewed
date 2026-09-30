/* Bounded x86 checked-IE trace. Hardware breakpoints only: no code patches.
 * Restore each thread's debug registers before detaching; never kill debuggee. */
#define _WIN32_WINNT 0x0600
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

typedef struct SavedThread { DWORD id; CONTEXT original; } SavedThread;
static SavedThread saved[256];
static unsigned saved_count;
static DWORD points[4], base;
static HANDLE process;
static BOOL read_words(DWORD address, void *buffer, SIZE_T size)
{
    SIZE_T read = 0;
    return ReadProcessMemory(process, (void *)(ULONG_PTR)address, buffer, size, &read) && read == size;
}
static BOOL install_thread(DWORD id)
{
    HANDLE thread;
    CONTEXT context = {0};
    unsigned i;
    for(i = 0; i < saved_count; ++i) if(saved[i].id == id) return TRUE;
    if(saved_count == ARRAYSIZE(saved)) return FALSE;
    thread = OpenThread(THREAD_QUERY_INFORMATION | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, id);
    if(!thread) { printf("OPEN_THREAD_ERROR tid=%lu error=%lu\n", (unsigned long)id, (unsigned long)GetLastError()); return FALSE; }
    context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if(!GetThreadContext(thread, &context)) { CloseHandle(thread); return FALSE; }
    saved[saved_count].id = id; saved[saved_count++].original = context;
    context.Dr0 = points[0]; context.Dr1 = points[1]; context.Dr2 = points[2]; context.Dr3 = points[3];
    context.Dr6 = 0; context.Dr7 = 0x55; /* four local execution breakpoints */
    if(!SetThreadContext(thread, &context)) { CloseHandle(thread); return FALSE; }
    CloseHandle(thread);
    return TRUE;
}
static BOOL restore_threads(void)
{
    unsigned i;
    BOOL ok = TRUE;
    for(i = 0; i < saved_count; ++i) if(saved[i].id) {
        HANDLE thread = OpenThread(THREAD_QUERY_INFORMATION | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, saved[i].id);
        if(!thread) {
            if(GetLastError() != ERROR_INVALID_PARAMETER) ok = FALSE;
            continue;
        }
        if(SuspendThread(thread) != (DWORD)-1) {
            if(!SetThreadContext(thread, &saved[i].original)) {
                printf("RESTORE_ERROR tid=%lu error=%lu\n", (unsigned long)saved[i].id, (unsigned long)GetLastError());
                ok = FALSE;
            }
            if(ResumeThread(thread) == (DWORD)-1) ok = FALSE;
        } else ok = FALSE;
        CloseHandle(thread);
    }
    return ok;
}
static LONG WINAPI trace_exception(EXCEPTION_POINTERS *exception)
{
    printf("TRACER_EXCEPTION code=%08lx address=%p\n",
           (unsigned long)exception->ExceptionRecord->ExceptionCode,
           exception->ExceptionRecord->ExceptionAddress);
    fflush(stdout);
    return EXCEPTION_CONTINUE_SEARCH;
}
static BOOL inspect_threads(DWORD pid, BOOL clear_owned)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 entry = {0};
    BOOL ok = TRUE;
    unsigned count = 0;
    if(snapshot == INVALID_HANDLE_VALUE) return FALSE;
    entry.dwSize = sizeof(entry);
    if(Thread32First(snapshot, &entry)) do {
        HANDLE thread;
        CONTEXT context = {0};
        if(entry.th32OwnerProcessID != pid) continue;
        thread = OpenThread(THREAD_QUERY_INFORMATION | THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME |
                            (clear_owned ? THREAD_SET_CONTEXT : 0), FALSE, entry.th32ThreadID);
        if(!thread) { ok = FALSE; continue; }
        if(SuspendThread(thread) == (DWORD)-1) { CloseHandle(thread); ok = FALSE; continue; }
        context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if(GetThreadContext(thread, &context)) {
            if(clear_owned && context.Dr7) {
                if(context.Dr0 != points[0] || context.Dr1 != points[1] ||
                   context.Dr2 != points[2] || context.Dr3 != points[3] || context.Dr7 != 0x55) {
                    puts("REFUSING_FOREIGN_DEBUG_REGISTERS"); ok = FALSE;
                } else {
                    context.Dr0 = context.Dr1 = context.Dr2 = context.Dr3 = context.Dr6 = context.Dr7 = 0;
                    if(!SetThreadContext(thread, &context) || !GetThreadContext(thread, &context)) ok = FALSE;
                }
            }
            printf("THREAD_DEBUG tid=%lu dr0=%08lx dr1=%08lx dr2=%08lx dr3=%08lx dr7=%08lx\n",
                   (unsigned long)entry.th32ThreadID, (unsigned long)context.Dr0,
                   (unsigned long)context.Dr1, (unsigned long)context.Dr2,
                   (unsigned long)context.Dr3, (unsigned long)context.Dr7);
            ++count;
        } else { printf("GET_CONTEXT_ERROR=%lu\n", (unsigned long)GetLastError()); ok = FALSE; }
        if(ResumeThread(thread) == (DWORD)-1) ok = FALSE;
        CloseHandle(thread);
    } while(Thread32Next(snapshot, &entry));
    CloseHandle(snapshot);
    printf("THREAD_INSPECTION count=%u ok=%d\n", count, ok);
    return count && ok;
}
static BOOL trace_hit(DWORD tid)
{
    HANDLE thread = OpenThread(THREAD_QUERY_INFORMATION | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, tid);
    CONTEXT context = {0};
    DWORD stack[10] = {0};
    unsigned i;
    BOOL ours = FALSE;
    if(!thread) return FALSE;
    context.ContextFlags = CONTEXT_FULL | CONTEXT_DEBUG_REGISTERS;
    if(!GetThreadContext(thread, &context)) { CloseHandle(thread); return FALSE; }
    for(i = 0; i < 4; ++i) if(context.Eip == points[i] && (context.Dr6 & (1u << i))) ours = TRUE;
    if(ours) {
        read_words(context.Esp, stack, sizeof(stack));
        printf("NATIVE_HIT tid=%lu rva=%08lx ecx=%08lx stack=", (unsigned long)tid,
               (unsigned long)(context.Eip - base), (unsigned long)context.Ecx);
        for(i = 0; i < ARRAYSIZE(stack); ++i) printf("%08lx%s", (unsigned long)stack[i], i == 9 ? "\n" : ",");
        printf("NATIVE_REGS eax=%08lx ebx=%08lx esi=%08lx edi=%08lx ebp=%08lx\n",
               (unsigned long)context.Eax, (unsigned long)context.Ebx,
               (unsigned long)context.Esi, (unsigned long)context.Edi, (unsigned long)context.Ebp);
        /* Known Refresh2 receiver's guards, not guessed C++ local variables. */
        if(context.Eip - base == 0x155980) {
            DWORD fields[2] = {0};
            read_words(stack[1] + 0xb4, &fields[0], 4);
            read_words(stack[1] + 0xe4, &fields[1], 4);
            printf("REFRESH2_RECEIVERS b4=%08lx e4=%08lx\n", (unsigned long)fields[0], (unsigned long)fields[1]);
        }
        if(context.Eip - base == 0x1982d8) {
            DWORD receiver = 0, vtable = 0, exec = 0;
            read_words(stack[1] + 0x1b0, &receiver, 4);
            if(receiver) read_words(receiver, &vtable, 4);
            if(vtable) read_words(vtable + 0x10, &exec, 4);
            printf("EXECDOWN_TARGET this=%08lx receiver=%08lx vtable=%08lx exec=%08lx\n",
                   (unsigned long)stack[1], (unsigned long)receiver, (unsigned long)vtable, (unsigned long)exec);
        }
        context.Dr6 = 0;
        context.EFlags |= 0x10000; /* RF: execute this instruction once */
        if(!SetThreadContext(thread, &context)) ours = FALSE;
        fflush(stdout);
    }
    CloseHandle(thread); return ours;
}
int main(int argc, char **argv)
{
    HANDLE snapshot, token;
    MODULEENTRY32W module = {0};
    TOKEN_PRIVILEGES privilege = {0};
    DWORD pid, seconds, started;
    DEBUG_EVENT event;
    unsigned i;
    BOOL attached = FALSE, alive = TRUE, ok = TRUE;
    BOOL initial_breakpoint = FALSE;
    BOOL clear_owned;
    setvbuf(stdout, NULL, _IONBF, 0);
    SetUnhandledExceptionFilter(trace_exception);
    if(argc != 7 && (argc != 3 || strcmp(argv[2], "--inspect"))) {
        puts("usage: ie-native-trace PID seconds RVA0 RVA1 RVA2 RVA3 | PID --inspect"); return 2;
    }
    pid = strtoul(argv[1], NULL, 10);
    clear_owned = argc == 7 && !strcmp(argv[2], "--clear-owned");
    seconds = argc == 7 && !clear_owned ? strtoul(argv[2], NULL, 10) : 0;
    if(!pid || (argc == 7 && !clear_owned && (!seconds || seconds > 120))) return 2;
    if(OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        privilege.PrivilegeCount = 1;
        LookupPrivilegeValueW(NULL, SE_DEBUG_NAME, &privilege.Privileges[0].Luid);
        privilege.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(token, FALSE, &privilege, 0, NULL, NULL); CloseHandle(token);
    }
    if(argc == 3) {
        BOOL debugging = FALSE;
        process = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
        if(!process || !CheckRemoteDebuggerPresent(process, &debugging)) return 1;
        CloseHandle(process);
        printf("TARGET_DEBUGGER_PRESENT=%d\n", debugging);
        return inspect_threads(pid, FALSE) ? 0 : 1;
    }
    snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    module.dwSize = sizeof(module);
    if(snapshot == INVALID_HANDLE_VALUE) return 1;
    if(Module32FirstW(snapshot, &module)) do {
        if(!lstrcmpiW(module.szModule, L"ieframe.dll")) { base = (DWORD)(ULONG_PTR)module.modBaseAddr; break; }
    } while(Module32NextW(snapshot, &module));
    CloseHandle(snapshot);
    if(!base) { puts("IEFRAME_NOT_FOUND"); return 1; }
    {
        DWORD ignored = 0, size = GetFileVersionInfoSizeW(module.szExePath, &ignored);
        void *data = size ? HeapAlloc(GetProcessHeap(), 0, size) : NULL;
        VS_FIXEDFILEINFO *version = NULL;
        UINT bytes = 0;
        BOOL matching = data && GetFileVersionInfoW(module.szExePath, 0, size, data) &&
            VerQueryValueW(data, L"\\", (void **)&version, &bytes) && bytes >= sizeof(*version) &&
            version->dwFileVersionMS == 0x00070000 && version->dwFileVersionLS == ((6002u << 16) | 18005u) &&
            (version->dwFileFlags & version->dwFileFlagsMask & VS_FF_DEBUG);
        if(data) HeapFree(GetProcessHeap(), 0, data);
        if(!matching) { puts("NOT_EXPECTED_CHECKED_IEFRAME"); return 1; }
    }
    for(i = 0; i < 4; ++i) {
        DWORD rva = strtoul(argv[i + 3], NULL, 16);
        if(!rva || rva >= module.modBaseSize) return 2;
        points[i] = base + rva;
    }
    if(clear_owned) {
        BOOL debugging = FALSE;
        process = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
        if(!process || !CheckRemoteDebuggerPresent(process, &debugging) || debugging) return 1;
        CloseHandle(process);
        return inspect_threads(pid, TRUE) ? 0 : 1;
    }
    process = OpenProcess(PROCESS_VM_READ, FALSE, pid);
    if(!process || !DebugActiveProcess(pid)) {
        printf("ATTACH_FAILED error=%lu\n", (unsigned long)GetLastError());
        if(process) CloseHandle(process);
        return 1;
    }
    attached = TRUE;
    if(!DebugSetProcessKillOnExit(FALSE)) { ok = FALSE; goto done; }
    started = GetTickCount();
    printf("NATIVE_TRACE_ATTACHED pid=%lu base=%08lx seconds=%lu\n", (unsigned long)pid, (unsigned long)base, (unsigned long)seconds);
    fflush(stdout);
    while(alive && ok && GetTickCount() - started < seconds * 1000) {
        DWORD status = DBG_CONTINUE;
        if(!WaitForDebugEvent(&event, 100)) {
            if(GetLastError() != ERROR_SEM_TIMEOUT) {
                printf("WAIT_ERROR=%lu\n", (unsigned long)GetLastError()); ok = FALSE; break;
            }
            continue;
        }
        if(event.dwDebugEventCode != LOAD_DLL_DEBUG_EVENT && event.dwDebugEventCode != OUTPUT_DEBUG_STRING_EVENT)
            printf("DEBUG_EVENT code=%lu tid=%lu\n", (unsigned long)event.dwDebugEventCode,
                   (unsigned long)event.dwThreadId);
        switch(event.dwDebugEventCode) {
        case CREATE_PROCESS_DEBUG_EVENT:
            ok = install_thread(event.dwThreadId);
            if(event.u.CreateProcessInfo.hFile) CloseHandle(event.u.CreateProcessInfo.hFile);
            break;
        case CREATE_THREAD_DEBUG_EVENT:
            ok = install_thread(event.dwThreadId); break;
        case EXIT_THREAD_DEBUG_EVENT:
            for(i = 0; i < saved_count; ++i) if(saved[i].id == event.dwThreadId) saved[i].id = 0;
            break;
        case LOAD_DLL_DEBUG_EVENT:
            if(event.u.LoadDll.hFile) CloseHandle(event.u.LoadDll.hFile);
            break;
        case EXIT_PROCESS_DEBUG_EVENT: alive = FALSE; break;
        case EXCEPTION_DEBUG_EVENT:
            printf("DEBUG_EXCEPTION code=%08lx first=%lu address=%p\n",
                   (unsigned long)event.u.Exception.ExceptionRecord.ExceptionCode,
                   (unsigned long)event.u.Exception.dwFirstChance,
                   event.u.Exception.ExceptionRecord.ExceptionAddress);
            if(event.u.Exception.ExceptionRecord.ExceptionCode == EXCEPTION_SINGLE_STEP && trace_hit(event.dwThreadId)) break;
            if(event.u.Exception.ExceptionRecord.ExceptionCode == EXCEPTION_BREAKPOINT && !initial_breakpoint)
                initial_breakpoint = TRUE;
            else status = DBG_EXCEPTION_NOT_HANDLED;
            break;
        default: break;
        }
        if(!ok) printf("INSTALL_ERROR=%lu\n", (unsigned long)GetLastError());
        if(!ContinueDebugEvent(event.dwProcessId, event.dwThreadId, status)) {
            printf("CONTINUE_ERROR=%lu\n", (unsigned long)GetLastError()); ok = FALSE;
        }
    }
done:
    if(alive && !restore_threads()) ok = FALSE;
    if(attached && alive && !DebugActiveProcessStop(pid)) ok = FALSE;
    CloseHandle(process);
    puts(ok ? "NATIVE_TRACE_DETACHED" : "NATIVE_TRACE_FAILED");
    return ok ? 0 : 1;
}
