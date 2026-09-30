/* Vista RTM capture broker. Run the controller elevated in the console
 * session; it starts a worker with the logged-on user's ordinary token. No cross-session inherited
 * handles, linked administrator token, network listener, or per-frame file I/O.
 * SPDX-License-Identifier: MIT */
#define _WIN32_WINNT 0x0600
#define WINVER 0x0600
#ifndef UNICODE
#define UNICODE
#endif
#define _UNICODE
#include <windows.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <sddl.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include "../triton-kmd/viogpu/shared/triton_trace_wire.h"

typedef struct { HDC dc; UINT adapter; LUID luid; UINT source; } OpenAdapter;
typedef struct { UINT adapter, device, type, flags; void *data; UINT size, context; } TraceEscape;
typedef LONG (WINAPI *OpenFn)(OpenAdapter *);
typedef LONG (WINAPI *EscapeFn)(const TraceEscape *);
typedef LONG (WINAPI *CloseFn)(const UINT *);
static UINT adapter;
static EscapeFn escapeFn;
static CloseFn closeFn;
static ULONGLONG run;
static WCHAR prefix[MAX_PATH];
static WCHAR loaded_path[MAX_PATH];

static BOOL file_sha256(const WCHAR *path, char out[65])
{
    HCRYPTPROV provider = 0; HCRYPTHASH hash = 0; BYTE digest[32], buffer[65536];
    DWORD n, size = sizeof(digest); BOOL ok = FALSE;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    if (!CryptAcquireContextW(&provider, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) ||
        !CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash)) goto done;
    for (;;) {
        if (!ReadFile(file, buffer, sizeof(buffer), &n, NULL)) goto done;
        if (!n) break;
        if (!CryptHashData(hash, buffer, n, 0)) goto done;
    }
    if (!CryptGetHashParam(hash, HP_HASHVAL, digest, &size, 0) || size != 32) goto done;
    for (UINT i = 0; i < 32; ++i) sprintf(out + i * 2, "%02x", digest[i]);
    ok = TRUE;
done:
    if (hash) CryptDestroyHash(hash);
    if (provider) CryptReleaseContext(provider, 0);
    CloseHandle(file); return ok;
}

static void json_path(FILE *f, const WCHAR *path)
{
    char text[MAX_PATH*3];
    if (!WideCharToMultiByte(CP_UTF8, 0, path, -1, text, sizeof(text), NULL, NULL)) text[0] = 0;
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (*p == '\\' || *p == '"') fputc('\\', f);
        if (*p < 32) fprintf(f, "\\u%04x", *p); else fputc(*p, f);
    }
    fputc('"', f);
}

static ULONGLONG ticks(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
static FILE *output(const WCHAR *suffix, const WCHAR *mode)
{
    WCHAR path[MAX_PATH];
    if (wcslen(prefix) + wcslen(suffix) >= MAX_PATH) return NULL;
    swprintf(path, MAX_PATH, L"%ls%ls", prefix, suffix);
    return _wfopen(path, mode);
}
static void active_state(BOOL active, DWORD pid)
{
    WCHAR temporary[MAX_PATH], final[MAX_PATH];
    swprintf(temporary, MAX_PATH, L"%ls.active.tmp", prefix);
    swprintf(final, MAX_PATH, L"%ls.active.json", prefix);
    FILE *f = _wfopen(temporary, L"wb");
    if (f) {
        fprintf(f, "{\"active\":%s,\"pid\":%lu}\n", active ? "true":"false", pid);
        fclose(f); MoveFileExW(temporary, final, MOVEFILE_REPLACE_EXISTING);
    }
}
static BOOL privilege(const WCHAR *name)
{
    HANDLE token; TOKEN_PRIVILEGES p = {0};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) return FALSE;
    p.PrivilegeCount = 1; p.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    BOOL ok = LookupPrivilegeValueW(NULL, name, &p.Privileges[0].Luid);
    if (ok) { SetLastError(0); ok = AdjustTokenPrivileges(token, FALSE, &p, 0, NULL, NULL) && GetLastError() == 0; }
    CloseHandle(token); return ok;
}

/* GDI adapter handles are session-specific on Vista. The host invokes the
 * controller through the service's elevated console-session operation. */
static int enter_console_session(UINT seconds)
{
    (void)seconds;
    DWORD current = 0, console = WTSGetActiveConsoleSessionId();
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &current) || current != console) {
        fprintf(stderr, "controller requires the interactive console session (vista_control --user)\n");
        return 1;
    }
    WCHAR path[MAX_PATH]; swprintf(path, MAX_PATH, L"%ls.controller-details.txt", prefix);
    if (!_wfreopen(path, L"wb", stderr)) return 1;
    return -1;
}

static LONG control(TRITON_TRACE_REQUEST *r, UINT operation, UINT size)
{
    r->type = TRITON_TRACE_ESCAPE_TYPE; r->length = size - 4;
    r->operation = operation; r->run = run;
    TraceEscape e = {0}; e.adapter = adapter; e.data = r; e.size = size;
    LONG status = escapeFn(&e);
    if (status < 0) fprintf(stderr, "trace escape %u status=%08lx\n", operation, (ULONG)status);
    return status;
}
static BOOL open_adapter(void)
{
    HMODULE gdi = LoadLibraryW(L"gdi32.dll");
    OpenFn openFn = (OpenFn)GetProcAddress(gdi, "D3DKMTOpenAdapterFromHdc");
    escapeFn = (EscapeFn)GetProcAddress(gdi, "D3DKMTEscape");
    closeFn = (CloseFn)GetProcAddress(gdi, "D3DKMTCloseAdapter");
    if (!openFn || !escapeFn || !closeFn) return FALSE;
    OpenAdapter o = {0}; o.dc = CreateDCW(L"DISPLAY", NULL, NULL, NULL);
    LONG status = o.dc ? openFn(&o) : -1;
    if (o.dc) DeleteDC(o.dc);
    if (status < 0) { fprintf(stderr, "open adapter=%08lx\n", (ULONG)status); return FALSE; }
    adapter = o.adapter; return TRUE;
}

static HANDLE normal_user_token(DWORD session)
{
    HANDLE token = NULL;
    if (WTSQueryUserToken(session, &token)) return token; // SYSTEM console broker.
    /* A linked token returned to an administrator on Vista can be an
     * identification-level handle, unusable for process creation. Duplicate
     * the primary token of this session's shell and verify it is unelevated. */
    DWORD pid = 0, shell_session = 0, needed;
    GetWindowThreadProcessId(GetShellWindow(), &pid);
    if (!pid || !ProcessIdToSessionId(pid, &shell_session) || shell_session != session) return NULL;
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid), primary = NULL;
    if (!process) return NULL;
    if (OpenProcessToken(process, TOKEN_DUPLICATE | TOKEN_QUERY, &primary)) {
        TOKEN_ELEVATION elevation;
        if (GetTokenInformation(primary, TokenElevation, &elevation, sizeof(elevation), &needed) && !elevation.TokenIsElevated)
            DuplicateTokenEx(primary, TOKEN_ALL_ACCESS, NULL, SecurityImpersonation, TokenPrimary, &token);
        CloseHandle(primary);
    }
    CloseHandle(process);
    return token;
}

static BOOL grant_read(const WCHAR *path)
{
    PSECURITY_DESCRIPTOR sd = NULL;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;GRGX;;;BU)", SDDL_REVISION_1, &sd, NULL)) return FALSE;
    BOOL ok = SetFileSecurityW(path, DACL_SECURITY_INFORMATION, sd);
    LocalFree(sd); return ok;
}
static BOOL dump(const WCHAR *suffix, TRITON_TRACE_HEADER header, TRITON_TRACE_RECORD *records)
{
    UINT count = header.count < 0 ? 0 : (UINT)header.count;
    if (count > header.capacity) count = header.capacity;
    header.reserved[0] = 0;
    UINT crc = triton_trace_crc32(0, &header, sizeof(header));
    header.reserved[0] = triton_trace_crc32(crc, records, count * sizeof(*records));
    FILE *f = output(suffix, L"wb"); if (!f) return FALSE;
    BOOL ok = fwrite(&header, sizeof(header), 1, f) == 1 &&
              fwrite(records, sizeof(*records), count, f) == count;
    return fclose(f) == 0 && ok;
}

typedef struct { ULONGLONG ticks; DWORD foreground; UINT visible, responsive, loaded; } Sample;
typedef struct { DWORD pid; HWND window; } WindowSearch;
static BOOL CALLBACK find_window(HWND window, LPARAM arg)
{
    WindowSearch *s = (WindowSearch *)arg; DWORD pid;
    GetWindowThreadProcessId(window, &pid);
    if (pid == s->pid && IsWindowVisible(window) && !GetWindow(window, GW_OWNER)) {
        s->window = window; return FALSE;
    }
    return TRUE;
}
static BOOL loaded(DWORD pid)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return FALSE;
    MODULEENTRY32W m = {0}; m.dwSize = sizeof(m); BOOL found = FALSE;
    if (Module32FirstW(snap, &m)) do {
        if (!_wcsicmp(m.szModule, L"neptune_d3d9.dll") || !_wcsicmp(m.szModule, L"neptune_d3d9_wow.dll")) {
            wcscpy(loaded_path, m.szExePath); found = TRUE; break;
        }
    } while (Module32NextW(snap, &m));
    CloseHandle(snap); return found;
}

/* Worker arguments: --worker PREFIX SECONDS COMMAND_FILE. */
static int worker(int argc, WCHAR **argv)
{
    if (argc != 5 || wcslen(argv[2]) > MAX_PATH - 32) return 2;
    wcscpy(prefix, argv[2]); UINT seconds = wcstoul(argv[3], NULL, 10);
    if (!seconds || seconds > 60) return 2;
    FILE *f = _wfopen(argv[4], L"rb"); if (!f) return 3;
    char utf8[16384]; size_t n = fread(utf8, 1, sizeof(utf8)-1, f); fclose(f); utf8[n] = 0;
    WCHAR command[16384];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, command, 16384)) return 3;
    WCHAR path[MAX_PATH]; swprintf(path, MAX_PATH, L"%ls.stdout.txt", prefix);
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
    HANDLE log = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, 0, NULL);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    if (log == INVALID_HANDLE_VALUE || nul == INVALID_HANDLE_VALUE) return 4;
    STARTUPINFOW si = {0}; PROCESS_INFORMATION pi = {0}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES; si.hStdOutput = si.hStdError = log; si.hStdInput = nul;
    ULONGLONG begin = ticks();
    BOOL started = CreateProcessW(NULL, command, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    DWORD launch_error = started ? 0 : GetLastError(); CloseHandle(log); CloseHandle(nul);
    Sample samples[601]; UINT count = 0; BOOL timeout = FALSE, was_loaded = FALSE;
    DWORD code = launch_error; LARGE_INTEGER frequency; QueryPerformanceFrequency(&frequency);
    if (started) {
        active_state(TRUE, pi.dwProcessId);
        while (WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT && count < 601) {
            WindowSearch ws = {pi.dwProcessId, NULL}; EnumWindows(find_window, (LPARAM)&ws);
            DWORD fg = 0; GetWindowThreadProcessId(GetForegroundWindow(), &fg);
            DWORD_PTR response = 0;
            BOOL responsive = ws.window && SendMessageTimeoutW(ws.window, WM_NULL, 0, 0,
                SMTO_ABORTIFHUNG | SMTO_BLOCK, 50, &response);
            if (!was_loaded) was_loaded = loaded(pi.dwProcessId);
            samples[count++] = (Sample){ticks(), fg, ws.window && !IsIconic(ws.window), responsive, was_loaded};
            if (ticks() - begin >= seconds * (ULONGLONG)frequency.QuadPart) { timeout = TRUE; break; }
            WaitForSingleObject(pi.hProcess, 100);
        }
        GetExitCodeProcess(pi.hProcess, &code);
    }
    ULONGLONG end = ticks(); DEVMODEW mode = {0}; mode.dmSize = sizeof(mode);
    active_state(FALSE, pi.dwProcessId);
    BOOL got_mode = EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &mode);
    f = output(L".workload.json", L"wb");
    if (f) {
        char sha[65] = {0};
        BOOL hashed = loaded_path[0] && file_sha256(loaded_path, sha);
        fprintf(f, "{\"schema\":1,\"pid\":%lu,\"launch_error\":%lu,\"exit_code\":%lu,\"timed_out\":%s,\"begin\":%llu,\"end\":%llu,\"frequency\":%llu,\"mode\":[%lu,%lu,%lu],\"samples\":[",
            pi.dwProcessId, launch_error, code, timeout ? "true":"false", begin, end, frequency.QuadPart,
            got_mode ? mode.dmPelsWidth:0, got_mode ? mode.dmPelsHeight:0, got_mode ? mode.dmDisplayFrequency:0);
        for (UINT i = 0; i < count; ++i) {
            Sample *s = &samples[i];
            fprintf(f, "%s[%llu,%lu,%u,%u,%u]", i ? ",":"", s->ticks, s->foreground, s->visible, s->responsive, s->loaded);
        }
        fprintf(f, "],\"loaded_module_path\":"); json_path(f, loaded_path);
        fprintf(f, ",\"loaded_module_file_sha256\":\"%s\"}\n", hashed ? sha : ""); fclose(f);
    }
    /* The explicit duration bounds interactive workloads too. Closing their
     * window requests ordinary cleanup; the service job is the final bound. */
    if (started) {
        if (timeout) {
            WindowSearch ws = {pi.dwProcessId, NULL}; EnumWindows(find_window, (LPARAM)&ws);
            if (ws.window) PostMessageW(ws.window, WM_CLOSE, 0, 0);
            if (WaitForSingleObject(pi.hProcess, 2000) == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 124);
        }
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    }
    return f ? 0 : 5;
}

/* Controller arguments: RUN_HEX PREFIX SECONDS COMMAND_FILE TRACE(0|1) [TTL_MS [GPU_EVERY]]. */
int wmain(int argc, WCHAR **argv)
{
    if (argc > 1 && !wcscmp(argv[1], L"--worker")) return worker(argc, argv);
    if (argc < 6 || argc > 8 || wcslen(argv[2]) > MAX_PATH - 32) return 2;
    run = _wcstoui64(argv[1], NULL, 16); wcscpy(prefix, argv[2]);
    UINT seconds = wcstoul(argv[3], NULL, 10); BOOL tracing = !wcscmp(argv[5], L"1");
    if (!run || !seconds || seconds > 60) return 2;
    UINT ttl = argc >= 7 ? wcstoul(argv[6], NULL, 10) : (seconds + 15) * 1000;
    if (!ttl || ttl > 90000) return 2;
    UINT gpu_every = argc == 8 ? wcstoul(argv[7], NULL, 10) : 16;
    if (!gpu_every || gpu_every > 256) return 2;
    int session_result = enter_console_session(seconds);
    if (session_result >= 0) return session_result;
    int result = 1; BOOL kernel_started = FALSE, mutex_owned = FALSE, header_owned = FALSE;
    const char *stage = "controller mutex";
    HANDLE mapping = NULL, token = NULL, mutex = NULL, worker_job = NULL; LPVOID environment = NULL;
    TRITON_TRACE_HEADER *header = NULL; PROCESS_INFORMATION pi = {0};
    TRITON_TRACE_REQUEST request = {0}; PSECURITY_DESCRIPTOR sd = NULL;
    mutex = CreateMutexW(NULL, FALSE, L"Global\\TritonVistaTraceControllerV1");
    if (!mutex) goto cleanup;
    DWORD waited = WaitForSingleObject(mutex, 0);
    if (waited != WAIT_OBJECT_0 && waited != WAIT_ABANDONED) goto cleanup;
    mutex_owned = TRUE;
    stage = "debug privilege / display adapter";
    if (!privilege(SE_DEBUG_NAME) || !open_adapter()) goto cleanup;
    if (control(&request, TT_STATUS, sizeof(request)) < 0 || request.header.enabled) goto cleanup;
    stage = "global mapping privilege";
    if (!privilege(SE_CREATE_GLOBAL_NAME)) goto cleanup;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;AU)", SDDL_REVISION_1, &sd, NULL)) goto cleanup;
    SECURITY_ATTRIBUTES sa = {sizeof(sa), sd, FALSE};
    stage = "create global mapping";
    UINT bytes = sizeof(*header) + TRITON_TRACE_CAPACITY * sizeof(TRITON_TRACE_RECORD);
    mapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, TRITON_TRACE_MAPPING);
    if (!mapping && GetLastError() == ERROR_FILE_NOT_FOUND)
        mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, bytes, TRITON_TRACE_MAPPING);
    if (!mapping) goto cleanup;
    header = MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, bytes); if (!header) goto cleanup;
    if (header->enabled || header->writers) goto cleanup;
    header_owned = TRUE;
    /* Keep the atomic reference gate untouched while old mapped producers
     * can still read it. It is already closed and drained here. */
    ZeroMemory(header, offsetof(TRITON_TRACE_HEADER, enabled));
    UINT after_gate = offsetof(TRITON_TRACE_HEADER, enabled) + sizeof(header->enabled);
    ZeroMemory((BYTE *)header + after_gate, bytes - after_gate);
    LARGE_INTEGER frequency; QueryPerformanceFrequency(&frequency);
    header->magic = TRITON_TRACE_MAGIC; header->version = TRITON_TRACE_VERSION;
    header->record_size = sizeof(TRITON_TRACE_RECORD); header->capacity = TRITON_TRACE_CAPACITY;
    header->run = run; header->frequency = frequency.QuadPart; header->start_ticks = ticks();
    header->reserved[1] = GetTickCount(); header->reserved[2] = ttl;
    header->reserved[4] = gpu_every;
    if (tracing) {
        stage = "start kernel recorder";
        request.arg0 = ttl;
        if (control(&request, TT_START, sizeof(request)) < 0) goto cleanup;
        kernel_started = TRUE; InterlockedExchange(&header->enabled, 1);
    }
    DWORD session = WTSGetActiveConsoleSessionId();
    stage = "ordinary user token";
    token = normal_user_token(session);
    if (session == 0xffffffff || !token ||
        !CreateEnvironmentBlock(&environment, token, FALSE)) goto cleanup;
    WCHAR exe[MAX_PATH], line[2048]; GetModuleFileNameW(NULL, exe, MAX_PATH);
    stage = "share worker inputs for reading";
    if (!grant_read(exe) || !grant_read(argv[4])) goto cleanup;
    swprintf(line, 2048, L"\"%ls\" --worker \"%ls\" %u \"%ls\"", exe, prefix, seconds, argv[4]);
    STARTUPINFOW si = {0}; si.cb = sizeof(si); si.lpDesktop = L"winsta0\\default";
    stage = "start ordinary user worker";
    privilege(SE_INCREASE_QUOTA_NAME);
    BOOL created = CreateProcessAsUserW(token, exe, line, NULL, NULL, FALSE,
        CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW, environment, NULL, &si, &pi);
    if (!created && GetLastError() == ERROR_PRIVILEGE_NOT_HELD && privilege(SE_IMPERSONATE_NAME)) {
        swprintf(line, 2048, L"\"%ls\" --worker \"%ls\" %u \"%ls\"", exe, prefix, seconds, argv[4]);
        created = CreateProcessWithTokenW(token, 0, exe, line,
            CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW, environment, NULL, &si, &pi);
    }
    if (!created) goto cleanup;
    /* Secondary Logon may create outside the service's job. Give that process
     * its own bounded job before it can launch the workload. Vista cannot nest
     * jobs, so retain the existing service job when it already owns the child. */
    BOOL in_job = FALSE;
    stage = "bound worker lifetime";
    if (!IsProcessInJob(pi.hProcess, NULL, &in_job)) goto cleanup;
    if (!in_job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        worker_job = CreateJobObjectW(NULL, NULL);
        if (!worker_job || !SetInformationJobObject(worker_job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
            !AssignProcessToJobObject(worker_job, pi.hProcess)) goto cleanup;
    }
    if (ResumeThread(pi.hThread) == (DWORD)-1) goto cleanup;
    waited = WaitForSingleObject(pi.hProcess, (seconds + 10) * 1000);
    stage = "wait for worker";
    if (waited != WAIT_OBJECT_0) { TerminateProcess(pi.hProcess, 124); goto cleanup; }
    DWORD code; if (!GetExitCodeProcess(pi.hProcess, &code) || code) goto cleanup;
    result = 0;
cleanup:
    if (result) fprintf(stderr, "controller failed stage=%s win32=%lu\n", stage, GetLastError());
    if (result && pi.hProcess) { TerminateProcess(pi.hProcess, 124); WaitForSingleObject(pi.hProcess, 1000); }
    if (worker_job) { CloseHandle(worker_job); worker_job = NULL; }
    if (header_owned) {
        BOOL expired = tracing && !(header->enabled & 1);
        InterlockedAnd(&header->enabled, ~1L);
        DWORD start = GetTickCount();
        while (header->enabled && GetTickCount() - start < 2000) Sleep(1);
        header->writers = header->enabled >> 1;
        header->stop_ticks = ticks(); header->complete = !header->enabled && !expired;
        if (!dump(L".umd.bin", *header, (TRITON_TRACE_RECORD *)(header + 1))) result = 1;
    }
    if (kernel_started) {
        if (control(&request, TT_STOP, sizeof(request)) < 0) result = 1;
        else {
            TRITON_TRACE_HEADER kh = request.header;
            TRITON_TRACE_RECORD *records = calloc(kh.count ? kh.count : 1, sizeof(*records));
            UINT chunk_size = sizeof(request) + 256 * sizeof(TRITON_TRACE_RECORD);
            TRITON_TRACE_REQUEST *chunk = calloc(1, chunk_size);
            BOOL ok = records && chunk;
            for (UINT offset = 0; ok && offset < (UINT)kh.count;) {
                chunk->offset = offset; chunk->count = 256;
                ok = control(chunk, TT_READ, chunk_size) >= 0 && chunk->count > 0;
                if (ok) { CopyMemory(records + offset, chunk + 1, chunk->count * sizeof(*records)); offset += chunk->count; }
            }
            if (!ok || !dump(L".kmd.bin", kh, records)) result = 1;
            free(chunk); free(records);
        }
    }
    if (header_owned) {
        FILE *f = output(L".environment.json", L"wb");
        if (f) {
            const WCHAR *paths[] = {L"C:\\Windows\\System32\\drivers\\viogpu3d.sys",
                L"C:\\Windows\\System32\\neptune_d3d9.dll", L"C:\\Windows\\SysWOW64\\neptune_d3d9_wow.dll"};
            OSVERSIONINFOW os = {0}; os.dwOSVersionInfoSize = sizeof(os); GetVersionExW(&os);
            fprintf(f, "{\"os_build\":%lu,\"files\":{", os.dwBuildNumber);
            for (UINT i = 0; i < 3; ++i) {
                char sha[65] = {0}; BOOL ok = file_sha256(paths[i], sha);
                if (i) fputc(',', f);
                json_path(f, paths[i]); fprintf(f, ":\"%s\"", ok ? sha : "");
                if (!ok) result = 1;
            }
            fprintf(f, "}}\n"); if (fclose(f)) result = 1;
        } else result = 1;
    }
    if (pi.hThread) CloseHandle(pi.hThread);
    if (pi.hProcess) CloseHandle(pi.hProcess);
    if (environment) DestroyEnvironmentBlock(environment);
    if (token) CloseHandle(token);
    if (header) UnmapViewOfFile(header);
    if (mapping) CloseHandle(mapping);
    if (sd) LocalFree(sd);
    if (adapter) closeFn(&adapter);
    if (mutex_owned) ReleaseMutex(mutex);
    if (mutex) CloseHandle(mutex);
    return result;
}
