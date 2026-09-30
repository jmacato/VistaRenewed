/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Standalone Vista D3D9Ex bring-up probe.  This intentionally tests the
 * public D3D9 runtime rather than calling private UMD entry points.  Each
 * successful line proves one more runtime -> UMD -> KMD -> host boundary.
 */

#define COBJMACROS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <wincrypt.h>
#include <d3d9.h>
#include <dwmapi.h>

#include "../triton9_shader_token_contract.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifndef DWM_E_COMPOSITIONDISABLED
#define DWM_E_COMPOSITIONDISABLED ((HRESULT)0x80263001L)
#endif

#define PROBE_WIDTH  128u
#define PROBE_HEIGHT 128u
#define PROBE_TRANSCRIPT_SIZE (128u * 1024u)
#define PROBE_DDI_LOG_TAIL_SIZE (64u * 1024u)
#define PROBE_SERVICE_NAME "TritonD3D9ProbeV2"
#define PROBE_SERVICE_NAME_MAX 96u
#define PROBE_RESULT_NONCE_LENGTH 64u
#define PROBE_AERO_WINDOW_CLASS "TritonVistaAeroBlurProbe"
#define PROBE_AERO_BACKDROP_CLASS "TritonVistaAeroBackdrop"
#define PROBE_AERO_WINDOW_HOLD_MS 240000u
#define PROBE_AERO_WINSAT_TIMEOUT_MS 900000u
#define PROBE_AERO_TRANSITION_TIMEOUT_MS 120000u
#define PROBE_PUBLIC_SERVICE_WAIT_MS 300000u
#define PROBE_AERO_SERVICE_WAIT_MS \
    (PROBE_AERO_WINSAT_TIMEOUT_MS + \
     2u * PROBE_AERO_TRANSITION_TIMEOUT_MS + \
     PROBE_AERO_WINDOW_HOLD_MS + 60000u)
/* Fixed geometry permits comparison of guest and host-window captures.
 * The black-painted glass plate must soften the backdrop's stripe edges;
 * transparency or successful DWM calls alone do not establish blur. */
#define PROBE_AERO_BACKDROP_X 80
#define PROBE_AERO_BACKDROP_Y 100
#define PROBE_AERO_BACKDROP_WIDTH 640
#define PROBE_AERO_BACKDROP_HEIGHT 360
#define PROBE_AERO_STRIPE_WIDTH 40
#define PROBE_AERO_GLASS_X 200
#define PROBE_AERO_GLASS_Y 190
#define PROBE_AERO_GLASS_WIDTH 320
#define PROBE_AERO_GLASS_HEIGHT 180

typedef HRESULT (WINAPI *PFN_DIRECT3DCREATE9EX)(UINT, IDirect3D9Ex **);
typedef HRESULT (WINAPI *PFN_DWMISCOMPOSITIONENABLED)(BOOL *enabled);
typedef HRESULT (WINAPI *PFN_DWMENABLECOMPOSITION)(UINT action);
typedef HRESULT (WINAPI *PFN_DWMEXTENDFRAMEINTOCLIENTAREA)(
    HWND window, const MARGINS *margins);
typedef HRESULT (WINAPI *PFN_DWMENABLEBLURBEHINDWINDOW)(
    HWND window, const DWM_BLURBEHIND *blurBehind);
typedef HRESULT (WINAPI *PFN_DWMGETCOLORIZATIONCOLOR)(DWORD *color, BOOL *opaque);
typedef HRESULT (WINAPI *PFN_DWMGETWINDOWATTRIBUTE)(
    HWND window, DWORD attribute, void *value, DWORD size);

typedef struct ProbeAeroWindowState {
    BOOL reapplyPending;
    BOOL compositionChanged;
    BOOL attributesChanged;
} ProbeAeroWindowState;

/* Keep these Vista KMT declarations local to the probe.  New SDK copies of
 * d3dkmthk.h select structures from the build machine's WDDM level, while
 * this diagnostic must issue the exact Vista ABI used by d3d9.dll. */
typedef UINT PROBE_D3DKMT_HANDLE;

typedef struct PROBE_D3DKMT_OPENADAPTERFROMHDC {
    HDC hDc;
    PROBE_D3DKMT_HANDLE hAdapter;
    LUID AdapterLuid;
    UINT VidPnSourceId;
} PROBE_D3DKMT_OPENADAPTERFROMHDC;

typedef struct PROBE_D3DKMT_QUERYADAPTERINFO {
    PROBE_D3DKMT_HANDLE hAdapter;
    UINT Type;
    void *pPrivateDriverData;
    UINT PrivateDriverDataSize;
} PROBE_D3DKMT_QUERYADAPTERINFO;

typedef struct PROBE_D3DKMT_UMDFILENAMEINFO {
    UINT Version;
    WCHAR UmdFileName[MAX_PATH];
} PROBE_D3DKMT_UMDFILENAMEINFO;

typedef struct PROBE_D3DKMT_CLOSEADAPTER {
    PROBE_D3DKMT_HANDLE hAdapter;
} PROBE_D3DKMT_CLOSEADAPTER;

typedef LONG (WINAPI *PFN_D3DKMTOPENADAPTERFROMHDC)(
    PROBE_D3DKMT_OPENADAPTERFROMHDC *);
typedef LONG (WINAPI *PFN_D3DKMTQUERYADAPTERINFO)(
    PROBE_D3DKMT_QUERYADAPTERINFO *);
typedef LONG (WINAPI *PFN_D3DKMTCLOSEADAPTER)(
    const PROBE_D3DKMT_CLOSEADAPTER *);

#define PROBE_KMTQAITYPE_UMDRIVERNAME 1u
#define PROBE_KMTUMDVERSION_DX9       0u

typedef struct PROBE_VERTEX {
    float x;
    float y;
    float z;
    DWORD color;
} PROBE_VERTEX;

/* COLOR0/COLOR1 is the FVF form of D3DTA_DIFFUSE/D3DTA_SPECULAR.  MIL's
 * Level-1 device test programs COLOR1 before it creates any resources; this
 * vertex gives the later public readback proof that Triton preserves it all
 * the way through the fixed-function shader path. */
typedef struct PROBE_SPECULAR_VERTEX {
    float x;
    float y;
    float z;
    DWORD diffuse;
    DWORD specular;
} PROBE_SPECULAR_VERTEX;

/* Matches MIL's dominant 2D shader-vertex shape: position, diffuse color,
 * and two texture-coordinate sets.  The texture probe below only samples the
 * first set, but retaining the second makes declaration translation cover the
 * normal compositor input layout rather than a test-only layout. */
typedef struct PROBE_TEXTURE_VERTEX {
    float x;
    float y;
    float z;
    DWORD color;
    float u0;
    float v0;
    float u1;
    float v1;
} PROBE_TEXTURE_VERTEX;

/* Vista's MIL uses this private 9Ex managed-equivalent pool for lockable
 * textures.  It is deliberately not D3DPOOL_MANAGED (1): milcore's
 * CD3DDeviceLevel1 selects pool 6 after it obtains IDirect3DDevice9Ex.
 * Keep the value local and named so the public probe exercises the same
 * resource contract without pretending that the old XPDM pool is involved. */
#define TRITON9_VISTA_MANAGED_POOL ((D3DPOOL)6)

static HANDLE g_log = INVALID_HANDLE_VALUE;
/* COM2 is an output-only QEMU serial channel.  It is deliberately separate
 * from checked-KD on COM1 and lets the guest-owned service prove each public
 * D3D9 boundary without a screen scrape or a host-side guest-disk mount. */
static HANDLE g_telemetry = INVALID_HANDLE_VALUE;
/* Only the two SYSTEM services write COM2, under their shared mutex.
 * Secure and Default-desktop children write local logs for this service
 * to relay; they never compete for the serial port. */
static BOOL g_childMode;
static char g_transcript[PROBE_TRANSCRIPT_SIZE];
static DWORD g_transcriptLength;
static int g_serviceResult = 1;
static SERVICE_STATUS_HANDLE g_serviceHandle;
/* The deployment service supplies a name scoped to its immutable package id.
 * A previous one-shot service can remain marked-for-delete briefly on Vista;
 * binding every package to a distinct SCM name prevents that stale object
 * from blocking the next public D3D9 proof run. */
static char g_probeServiceName[PROBE_SERVICE_NAME_MAX] = PROBE_SERVICE_NAME;
static char g_probeResultPath[MAX_PATH];
static char g_probeResultNonce[PROBE_RESULT_NONCE_LENGTH + 1u];
/* The service performs a loader-only UMD preflight before it creates the
 * secure-desktop child.  Keep that diagnostic reference for the service
 * lifetime so static CRT teardown cannot race a diagnostic FreeLibrary. */
static HMODULE g_preflightUmdModule;
/* The KMT probe resolves Vista's gdi32 thunks dynamically.  Keep the module
 * loaded until process exit, like d3d9.dll itself does. */
static HMODULE g_gdi32Module;

static void probeLog(const char *format, ...);

static BOOL
probeValidHex(const char *value, size_t length)
{
    size_t index;
    if (!value || strlen(value) != length)
        return FALSE;
    for (index = 0; index < length; ++index) {
        char ch = value[index];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
            return FALSE;
    }
    return TRUE;
}

static BOOL
probeGenerateResultNonce(void)
{
    static const char hex[] = "0123456789abcdef";
    HCRYPTPROV provider = 0;
    BYTE random[PROBE_RESULT_NONCE_LENGTH / 2u];
    DWORD index;
    BOOL ok;

    if (!CryptAcquireContextA(&provider, NULL, NULL, PROV_RSA_FULL,
                              CRYPT_VERIFYCONTEXT | CRYPT_SILENT))
        return FALSE;
    ok = CryptGenRandom(provider, sizeof(random), random);
    CryptReleaseContext(provider, 0);
    if (!ok)
        return FALSE;
    for (index = 0; index < sizeof(random); ++index) {
        g_probeResultNonce[index * 2u] = hex[random[index] >> 4];
        g_probeResultNonce[index * 2u + 1u] = hex[random[index] & 0x0fu];
    }
    g_probeResultNonce[PROBE_RESULT_NONCE_LENGTH] = '\0';
    return TRUE;
}

static BOOL
probeValidResultPath(const char *value)
{
    static const char suffix[] = ".log";
    char windows[MAX_PATH];
    char prefix[MAX_PATH];
    const char *cursor;
    size_t length, prefixLength;
    if (!value || !GetWindowsDirectoryA(windows, sizeof(windows)) ||
        snprintf(prefix, sizeof(prefix),
                 "%s\\Temp\\triton9-service-", windows) <= 0)
        return FALSE;
    prefixLength = strlen(prefix);
    if (strncmp(value, prefix, prefixLength) != 0)
        return FALSE;
    length = strlen(value);
    if (length <= prefixLength + sizeof(suffix) - 1u ||
        length >= MAX_PATH ||
        strcmp(value + length - (sizeof(suffix) - 1u), suffix) != 0)
        return FALSE;
    for (cursor = value + prefixLength;
         cursor < value + length - (sizeof(suffix) - 1u); ++cursor) {
        if (!((*cursor >= '0' && *cursor <= '9') ||
              (*cursor >= 'a' && *cursor <= 'f') || *cursor == '-'))
            return FALSE;
    }
    return TRUE;
}

static BOOL
probeBuildChildLogPath(char *path, size_t capacity, const char *kind)
{
    char windows[MAX_PATH];
    int written;
    if (!path || !capacity || !kind ||
        !probeValidHex(g_probeResultNonce, PROBE_RESULT_NONCE_LENGTH) ||
        !GetWindowsDirectoryA(windows, sizeof(windows)))
        return FALSE;
    written = snprintf(path, capacity, "%s\\Temp\\triton9-%s-%s.log",
                       windows, kind, g_probeResultNonce);
    return written > 0 && (size_t)written < capacity;
}

static void
probeWriteTelemetry(const char *text, DWORD length)
{
    COMMTIMEOUTS timeouts;
    DWORD written, wait;
    HANDLE mutex;
    if (!text || !length || g_childMode)
        return;
    /* Serial devices have one open owner. Share ownership with the deploy
     * service per write, so its post-StartService markers are not lost. */
    mutex = CreateMutexA(NULL, FALSE, "Global\\TritonVistaTelemetry");
    if (!mutex) return;
    wait = WaitForSingleObject(mutex, 5000);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
        CloseHandle(mutex);
        return;
    }
    g_telemetry = CreateFileA("\\\\.\\COM2", GENERIC_WRITE, 0, NULL,
                              OPEN_EXISTING, 0, NULL);
    if (g_telemetry != INVALID_HANDLE_VALUE) {
        ZeroMemory(&timeouts, sizeof(timeouts));
        timeouts.WriteTotalTimeoutConstant = 1000;
        SetCommTimeouts(g_telemetry, &timeouts);
        WriteFile(g_telemetry, text, length, &written, NULL);
        CloseHandle(g_telemetry);
        g_telemetry = INVALID_HANDLE_VALUE;
    }
    ReleaseMutex(mutex);
    CloseHandle(mutex);
}

static void
probeCloseTelemetry(void)
{
    if (g_telemetry != INVALID_HANDLE_VALUE) {
        CloseHandle(g_telemetry);
        g_telemetry = INVALID_HANDLE_VALUE;
    }
}

/* The checked Vista SP2 x64 d3d9 runtime keeps the imported HAL caps and
 * FORMATOP array in CEnum.  This is a diagnostic-only view of that build's
 * state, not an ABI used by the driver.  It distinguishes the runtime's
 * global SoftwareOnly/HAL-disable gate from a malformed FORMATOP import
 * without patching d3d9.dll or depending on a debugger at the instant the
 * public API returns. */
static void
probeVistaCheckedEnumState(IDirect3D9Ex *d3d)
{
#if defined(_WIN64)
    BYTE *enumObject = (BYTE *)d3d;
    DWORD magic;
    DWORD halDisabled;
    BYTE *halCaps;
    DWORD formatCount;
    BYTE *formatRecords;
    DWORD index;

    if (!enumObject)
        return;
    magic = *(const DWORD *)(enumObject + 0x40);
    probeLog("checked CEnum magic=0x%08lx\r\n", (unsigned long)magic);
    if (magic != 0xd3d8d3d8u) {
        probeLog("checked CEnum layout          UNRECOGNIZED\r\n");
        return;
    }

    halDisabled = *(const DWORD *)(enumObject + 0x4f40);
    halCaps = enumObject + 0x1b0;
    formatCount = *(const DWORD *)(halCaps + 0x160);
    formatRecords = *(BYTE **)(halCaps + 0x168);
    probeLog("checked CEnum HAL-disabled=%lu formats=%lu records=%p\r\n",
             (unsigned long)halDisabled, (unsigned long)formatCount,
             formatRecords);
    if (!formatRecords || formatCount > 64) {
        probeLog("checked CEnum format table    INVALID\r\n");
        return;
    }
    for (index = 0; index < formatCount; ++index) {
        const BYTE *record = formatRecords + (SIZE_T)index * 0x78;
        DWORD format = *(const DWORD *)(record + 0x58);
        DWORD operations = *(const DWORD *)(record + 0x60);
        probeLog("checked FORMATOP[%lu] format=%lu ops=0x%08lx\r\n",
                 (unsigned long)index, (unsigned long)format,
                 (unsigned long)operations);
    }
#else
    (void)d3d;
#endif
}

static void
probeWrite(const char *text)
{
    DWORD length;
    DWORD written;
    HANDLE output;

    if (!text)
        return;
    length = (DWORD)strlen(text);
    if (length <= PROBE_TRANSCRIPT_SIZE - g_transcriptLength) {
        CopyMemory(g_transcript + g_transcriptLength, text, length);
        g_transcriptLength += length;
    }
    output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (output && output != INVALID_HANDLE_VALUE)
        WriteFile(output, text, length, &written, NULL);
    if (g_log != INVALID_HANDLE_VALUE)
        WriteFile(g_log, text, length, &written, NULL);
    probeWriteTelemetry(text, length);
}

static BOOL
probeProc(HMODULE module, const char *name, void *function, SIZE_T size)
{
    FARPROC address;

    if (!module || !name || !function || size != sizeof(address))
        return FALSE;
    address = GetProcAddress(module, name);
    if (!address)
        return FALSE;
    CopyMemory(function, &address, size);
    return TRUE;
}

static BOOL
probeUploadTo(const char *objectName)
{
    /* The probe is guest-owned.  It must not depend on a host web server or
     * spend up to ten seconds at every D3D boundary waiting on one.  The
     * service flushes its serial transcript.  Children expose their WriteFile
     * data to that service immediately and close the log on normal exit. */
    (void)objectName;
    /* WriteFile makes a child's log immediately visible to the service.  Do
     * not force an NTFS flush at every D3D boundary: checked Vista can block
     * that flush indefinitely while the display stack is transitioning. */
    if (g_childMode)
        return TRUE;
    if (g_log != INVALID_HANDLE_VALUE)
        return FlushFileBuffers(g_log);
    return TRUE;
}

static BOOL
probeUpload(void)
{
    return probeUploadTo("/__triton9_probe__");
}

static void
probePreflightUmdLoader(void)
{
    FARPROC openAdapter;
    DWORD error;

    SetLastError(NO_ERROR);
    g_preflightUmdModule =
        LoadLibraryA("C:\\Windows\\System32\\neptune_d3d9.dll");
    if (!g_preflightUmdModule) {
        error = GetLastError();
        probeLog("LoadLibrary(neptune_d3d9)   error=%lu FAIL\r\n",
                 (unsigned long)error);
        return;
    }
    openAdapter = GetProcAddress(g_preflightUmdModule, "OpenAdapter");
    if (!openAdapter) {
        error = GetLastError();
        probeLog("GetProcAddress(OpenAdapter) error=%lu FAIL\r\n",
                 (unsigned long)error);
        return;
    }
    probeLog("LoadLibrary(neptune_d3d9)      PASS\r\n");
    probeLog("GetProcAddress(OpenAdapter)    PASS\r\n");
}

static void
probeLog(const char *format, ...)
{
    char buffer[512];
    va_list args;
    int length;

    va_start(args, format);
    length = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (length <= 0)
        return;
    buffer[sizeof(buffer) - 1] = '\0';
    probeWrite(buffer);
}

static void
probeHr(const char *gate, HRESULT hr)
{
    probeLog("%-30s hr=0x%08lx %s\r\n", gate, (unsigned long)hr,
             SUCCEEDED(hr) ? "PASS" : "FAIL");
    /* A returned HRESULT is an independently useful bring-up boundary.  Do
     * not defer it until process cleanup: Vista can stop a checked process on
     * the next malformed runtime/driver interaction. */
    probeUpload();
}

static void
probeEnter(const char *gate)
{
    probeLog("%-30s ENTER\r\n", gate);
    probeUpload();
}

static LONG WINAPI
probeUnhandledException(EXCEPTION_POINTERS *exception)
{
    DWORD code = 0;
    void *address = NULL;

    if (exception && exception->ExceptionRecord) {
        code = exception->ExceptionRecord->ExceptionCode;
        address = exception->ExceptionRecord->ExceptionAddress;
    }
    probeLog("UNHANDLED EXCEPTION code=0x%08lx address=%p\r\n",
             (unsigned long)code, address);
    if (g_log != INVALID_HANDLE_VALUE)
        FlushFileBuffers(g_log);
    return EXCEPTION_EXECUTE_HANDLER;
}

static void
probeAppendFileTail(const char *path, const char *label, DWORD maximumBytes)
{
    HANDLE file;
    DWORD size;
    DWORD offset = 0;
    DWORD total = 0;
    DWORD read;
    char buffer[4097];
    WCHAR wide[2048];
    char utf8[8193];
    BYTE prefix[2];
    BOOL unicode = FALSE;

    file = CreateFileA(path, GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        probeLog("--- %s unavailable error=%lu ---\r\n", label,
                 GetLastError());
        return;
    }
    size = GetFileSize(file, NULL);
    if (size == INVALID_FILE_SIZE && GetLastError() != NO_ERROR) {
        probeLog("--- %s size error=%lu ---\r\n", label, GetLastError());
        CloseHandle(file);
        return;
    }
    if (ReadFile(file, prefix, sizeof(prefix), &read, NULL) && read == 2)
        unicode = (prefix[0] == 0xff && prefix[1] == 0xfe) ||
                  (prefix[0] != 0 && prefix[1] == 0);
    SetFilePointer(file, 0, NULL, FILE_BEGIN);
    if (size > maximumBytes)
        offset = size - maximumBytes;
    if (unicode)
        offset = (offset + 1) & ~1u;
    if (offset && SetFilePointer(file, (LONG)offset, NULL, FILE_BEGIN) ==
                      INVALID_SET_FILE_POINTER) {
        probeLog("--- %s seek error=%lu ---\r\n", label, GetLastError());
        CloseHandle(file);
        return;
    }
    probeLog("--- %s tail offset=%lu size=%lu ---\r\n", label,
             (unsigned long)offset, (unsigned long)size);
    while (total < maximumBytes &&
           ReadFile(file, buffer,
                    min((DWORD)(sizeof(buffer) - 1), maximumBytes - total),
                    &read, NULL) && read) {
        if (unicode) {
            int count;
            DWORD skip = (offset == 0 && total == 0 && read >= 2 &&
                          (BYTE)buffer[0] == 0xff && (BYTE)buffer[1] == 0xfe) ? 2 : 0;
            memcpy(wide, buffer + skip, (read - skip) & ~1u);
            count = WideCharToMultiByte(CP_UTF8, 0, wide, (read - skip) / 2,
                                        utf8, sizeof(utf8) - 1, NULL, NULL);
            if (count > 0) {
                utf8[count] = 0;
                probeWrite(utf8);
            }
        } else {
            buffer[read] = '\0';
            probeWrite(buffer);
        }
        total += read;
    }
    probeLog("\r\n--- %s end ---\r\n", label);
    CloseHandle(file);
}

/* Relay new child bytes while the child is still running.  The Default and
 * Winlogon desktop processes share the file with this service, so WriteFile
 * data is visible here without forcing NTFS to flush during a display-stack
 * transition.  The offset also makes the transcript monotonic: each child
 * byte reaches COM2 once, even when the child later stalls or times out. */
static BOOL
probeRelayFileGrowth(const char *path, const char *label, DWORD maximumBytes,
                     DWORD *offset)
{
    HANDLE file;
    DWORD size;
    DWORD remaining;
    DWORD read;
    DWORD total = 0;
    char buffer[4097];

    if (!path || !label || !maximumBytes || !offset)
        return FALSE;
    file = CreateFileA(path, GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return FALSE;
    size = GetFileSize(file, NULL);
    if ((size == INVALID_FILE_SIZE && GetLastError() != NO_ERROR) ||
        size > maximumBytes) {
        CloseHandle(file);
        return FALSE;
    }
    if (*offset > size)
        *offset = 0;
    if (*offset == size) {
        CloseHandle(file);
        return FALSE;
    }
    if (*offset == 0)
        probeLog("--- %s live ---\r\n", label);
    if (*offset && SetFilePointer(file, (LONG)*offset, NULL, FILE_BEGIN) ==
                       INVALID_SET_FILE_POINTER) {
        CloseHandle(file);
        return FALSE;
    }
    remaining = size - *offset;
    while (total < remaining &&
           ReadFile(file, buffer,
                    min((DWORD)(sizeof(buffer) - 1), remaining - total),
                    &read, NULL) && read) {
        buffer[read] = '\0';
        probeWrite(buffer);
        total += read;
    }
    CloseHandle(file);
    *offset += total;
    /* probeWrite already sends these bytes through the service-owned COM2
     * handle.  Do not call probeUploadTo here: its NTFS FlushFileBuffers can
     * block while checked Vista is inside a D3D/display transition, which
     * would defeat both this relay and the independent timeout deadline. */
    return total != 0;
}

static BOOL
probeFileContainsText(const char *path, const char *marker, DWORD maximumBytes)
{
    HANDLE file = INVALID_HANDLE_VALUE;
    DWORD size;
    DWORD read = 0;
    char *data = NULL;
    BOOL found = FALSE;

    if (!path || !marker || !marker[0] || !maximumBytes)
        return FALSE;
    file = CreateFileA(path, GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        goto done;
    size = GetFileSize(file, NULL);
    if ((size == INVALID_FILE_SIZE && GetLastError() != NO_ERROR) ||
        !size || size > maximumBytes)
        goto done;
    data = (char *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)size + 1u);
    if (!data || !ReadFile(file, data, size, &read, NULL) || read != size)
        goto done;
    data[read] = '\0';
    found = strstr(data, marker) != NULL;

done:
    if (data)
        HeapFree(GetProcessHeap(), 0, data);
    if (file != INVALID_HANDLE_VALUE)
        CloseHandle(file);
    return found;
}

static LRESULT CALLBACK
probeWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_CLOSE) {
        DestroyWindow(window);
        return 0;
    }
    return DefWindowProcA(window, message, wparam, lparam);
}

static HWND
probeCreateWindow(HINSTANCE instance)
{
    WNDCLASSEXA windowClass;
    HWND window;

    ZeroMemory(&windowClass, sizeof(windowClass));
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = probeWindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursor(NULL, IDC_ARROW);
    windowClass.lpszClassName = "Triton9RuntimeProbe";
    if (!RegisterClassExA(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return NULL;
    window = CreateWindowExA(0, windowClass.lpszClassName,
                             "Triton D3D9Ex probe",
                             WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             360, 300, NULL, NULL, instance, NULL);
    return window;
}

/* This window is deliberately an ordinary USER window on the logged-in
 * session's Default desktop.  The existing D3D9 probe runs on Winlogon's
 * secure desktop, which is the right place to prove the UMD can load but is
 * not evidence that Vista accepted the adapter for the user's compositor. */
static LRESULT CALLBACK
probeAeroWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    ProbeAeroWindowState *state =
        (ProbeAeroWindowState *)GetWindowLongPtrA(window, GWLP_USERDATA);

    if (message == WM_NCCREATE) {
        const CREATESTRUCTA *create = (const CREATESTRUCTA *)lparam;

        SetWindowLongPtrA(window, GWLP_USERDATA, (LONG_PTR)create->lpCreateParams);
    } else if (message == WM_DWMCOMPOSITIONCHANGED && state) {
        state->reapplyPending = TRUE;
        state->compositionChanged = TRUE;
        state->attributesChanged = TRUE;
        probeLog("AERO composition-changed tick=%lu reapply=PENDING\r\n",
                 (unsigned long)GetTickCount());
    } else if ((message == WM_DWMCOLORIZATIONCOLORCHANGED ||
                message == WM_DWMNCRENDERINGCHANGED) && state) {
        state->attributesChanged = TRUE;
        probeLog("AERO attributes-changed message=0x%04x tick=%lu\r\n",
                 message, (unsigned long)GetTickCount());
    } else if (message == WM_NCDESTROY) {
        SetWindowLongPtrA(window, GWLP_USERDATA, 0);
    }
    if (message == WM_ERASEBKGND)
        return 1;
    if (message == WM_PAINT) {
        PAINTSTRUCT paint;
        RECT label;
        HDC dc = BeginPaint(window, &paint);

        /* Initialize the glass plate to transparent black as required
         * by DWM. Leaving the client pixels unpainted does not establish
         * their alpha values. DWM alone supplies the backdrop and blur. */
        RECT client;
        GetClientRect(window, &client);
        FillRect(dc, &client, (HBRUSH)GetStockObject(BLACK_BRUSH));
        /* The verifier samples below this small identifying label. */
        SetRect(&label, 8, 6, PROBE_AERO_GLASS_WIDTH - 8, 28);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        DrawTextA(dc, "Triton glass pixel proof", -1, &label,
                  DT_CENTER | DT_TOP | DT_SINGLELINE);
        EndPaint(window, &paint);
        return 0;
    }
    if (message == WM_CLOSE) {
        DestroyWindow(window);
        return 0;
    }
    return DefWindowProcA(window, message, wparam, lparam);
}

static LRESULT CALLBACK
probeAeroBackdropProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

static HWND
probeCreateAeroBackdrop(HINSTANCE instance)
{
    WNDCLASSEXA windowClass;

    ZeroMemory(&windowClass, sizeof(windowClass));
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = probeAeroBackdropProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursor(NULL, IDC_ARROW);
    windowClass.lpszClassName = PROBE_AERO_BACKDROP_CLASS;
    if (!RegisterClassExA(&windowClass) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return NULL;
    /* Keep the bounded proof scene visible above auto-opened media windows
     * and diagnostic consoles without changing who has keyboard focus. */
    return CreateWindowExA(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, PROBE_AERO_BACKDROP_CLASS,
                           "", WS_POPUP | WS_VISIBLE,
                           PROBE_AERO_BACKDROP_X, PROBE_AERO_BACKDROP_Y,
                           PROBE_AERO_BACKDROP_WIDTH,
                           PROBE_AERO_BACKDROP_HEIGHT,
                           NULL, NULL, instance, NULL);
}

static LRESULT CALLBACK
probeAeroBackdropProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_ERASEBKGND)
        return 1;
    if (message == WM_PAINT) {
        PAINTSTRUCT paint;
        RECT client;
        RECT stripe;
        HDC dc = BeginPaint(window, &paint);
        int x;
        unsigned stripeIndex = 0;

        GetClientRect(window, &client);
        for (x = client.left; x < client.right; x += PROBE_AERO_STRIPE_WIDTH) {
            HBRUSH brush = CreateSolidBrush((stripeIndex & 1u)
                                                ? RGB(245, 215, 30)
                                                : RGB(20, 35, 235));

            stripe.left = x;
            stripe.top = client.top;
            stripe.right = min(x + PROBE_AERO_STRIPE_WIDTH, client.right);
            stripe.bottom = client.bottom;
            FillRect(dc, &stripe, brush);
            DeleteObject(brush);
            ++stripeIndex;
        }
        EndPaint(window, &paint);
        return 0;
    }
    if (message == WM_CLOSE) {
        DestroyWindow(window);
        return 0;
    }
    return DefWindowProcA(window, message, wparam, lparam);
}

static HWND
probeCreateAeroWindow(HINSTANCE instance, ProbeAeroWindowState *state)
{
    WNDCLASSEXA windowClass;

    ZeroMemory(&windowClass, sizeof(windowClass));
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = probeAeroWindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursor(NULL, IDC_ARROW);
    windowClass.lpszClassName = PROBE_AERO_WINDOW_CLASS;
    if (!RegisterClassExA(&windowClass) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return NULL;
    return CreateWindowExA(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, PROBE_AERO_WINDOW_CLASS,
                           "", WS_POPUP | WS_VISIBLE,
                           PROBE_AERO_GLASS_X, PROBE_AERO_GLASS_Y,
                           PROBE_AERO_GLASS_WIDTH, PROBE_AERO_GLASS_HEIGHT,
                           NULL, NULL, instance, state);
}

static void
probeAeroUxSmsState(void)
{
    SC_HANDLE manager = NULL;
    SC_HANDLE service = NULL;
    SERVICE_STATUS_PROCESS status;
    DWORD bytes = 0;

    manager = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT);
    if (!manager) {
        probeLog("AERO OpenSCManager error=%lu FAIL\r\n",
                 (unsigned long)GetLastError());
        return;
    }
    service = OpenServiceA(manager, "UxSms", SERVICE_QUERY_STATUS);
    if (!service) {
        probeLog("AERO OpenService(UxSms) error=%lu FAIL\r\n",
                 (unsigned long)GetLastError());
        CloseServiceHandle(manager);
        return;
    }
    ZeroMemory(&status, sizeof(status));
    if (QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, (BYTE *)&status,
                             sizeof(status), &bytes)) {
        probeLog("AERO UxSms state=%lu pid=%lu PASS\r\n",
                 (unsigned long)status.dwCurrentState,
                 (unsigned long)status.dwProcessId);
    } else {
        probeLog("AERO QueryServiceStatusEx(UxSms) error=%lu FAIL\r\n",
                 (unsigned long)GetLastError());
    }
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
}

/* WinSAT's DWM assessment is Vista's own hardware-admission fallback.  Run it
 * only after a bounded DwmEnableComposition transition cannot activate an
 * already-assessed compositor.  Re-running WinSAT while DWM is starting makes
 * Vista turn composition off for another assessment and can strand the
 * otherwise-qualified session.  Its exit code and the UMD flight recorder
 * distinguish a missing policy assessment from a rejected Triton DDI
 * operation. */
static BOOL
probeRunWinSatDwm(void)
{
    char systemDirectory[MAX_PATH];
    char application[MAX_PATH];
    char commandLine[MAX_PATH + 32];
    const char *logPath = "C:\\Windows\\Temp\\triton9-winsat-dwm.log";
    STARTUPINFOA startup;
    PROCESS_INFORMATION process;
    HANDLE output = INVALID_HANDLE_VALUE;
    DWORD waitResult;
    DWORD exitCode = ERROR_GEN_FAILURE;
    BOOL ok = FALSE;
    SECURITY_ATTRIBUTES inherit = {sizeof(SECURITY_ATTRIBUTES), NULL, TRUE};

    if (!GetSystemDirectoryA(systemDirectory, sizeof(systemDirectory)) ||
        snprintf(application, sizeof(application), "%s\\winsat.exe",
                 systemDirectory) <= 0 ||
        snprintf(commandLine, sizeof(commandLine), "\"%s\" dwm", application) <= 0) {
        probeLog("AERO WinSAT(DWM) path error=%lu FAIL\r\n",
                 (unsigned long)GetLastError());
        return FALSE;
    }
    output = CreateFileA(logPath, GENERIC_WRITE, FILE_SHARE_READ, &inherit,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (output == INVALID_HANDLE_VALUE) {
        probeLog("AERO WinSAT(DWM) log error=%lu FAIL\r\n",
                 (unsigned long)GetLastError());
        return FALSE;
    }
    ZeroMemory(&startup, sizeof(startup));
    ZeroMemory(&process, sizeof(process));
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    startup.wShowWindow = SW_HIDE;
    startup.hStdInput = output;
    startup.hStdOutput = output;
    startup.hStdError = output;
    probeLog("AERO WinSAT(DWM) start\r\n");
    if (!CreateProcessA(application, commandLine, NULL, NULL, TRUE,
                        CREATE_NO_WINDOW, NULL, "C:\\Windows\\Temp",
                        &startup, &process)) {
        probeLog("AERO WinSAT(DWM) launch error=%lu FAIL\r\n",
                 (unsigned long)GetLastError());
        CloseHandle(output);
        return FALSE;
    }
    CloseHandle(output);
    output = INVALID_HANDLE_VALUE;
    waitResult = WaitForSingleObject(process.hProcess,
                                     PROBE_AERO_WINSAT_TIMEOUT_MS);
    if (waitResult == WAIT_OBJECT_0 &&
        GetExitCodeProcess(process.hProcess, &exitCode)) {
        ok = exitCode == ERROR_SUCCESS;
        probeLog("AERO WinSAT(DWM) exit=%lu %s\r\n",
                 (unsigned long)exitCode, ok ? "PASS" : "FAIL");
    } else {
        DWORD error = waitResult == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();

        if (waitResult == WAIT_TIMEOUT)
            TerminateProcess(process.hProcess, ERROR_TIMEOUT);
        probeLog("AERO WinSAT(DWM) wait error=%lu FAIL\r\n",
                 (unsigned long)error);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    probeAppendFileTail(logPath, "WINSAT-DWM-LOG", 32768);
    probeUpload();
    return ok;
}

/* UxSms can report RUNNING before the compositor finishes its Default-desktop
 * transition.  Ask through Vista's documented API and retry only its explicit
 * transition rejection.  A successful request is not enough: the boolean
 * query must become true before this helper reports readiness. */
static HRESULT
probeAeroEnableCompositionWhenReady(
    PFN_DWMISCOMPOSITIONENABLED isCompositionEnabled,
    PFN_DWMENABLECOMPOSITION enableComposition,
    BOOL *enabled)
{
    DWORD deadline = GetTickCount() + PROBE_AERO_TRANSITION_TIMEOUT_MS;
    DWORD attempt = 0;
    HRESULT hr = S_OK;

    if (!enabled)
        return E_POINTER;
    *enabled = FALSE;
    for (;;) {
        hr = isCompositionEnabled(enabled);
        if (FAILED(hr) || *enabled)
            return hr;

        ++attempt;
        hr = enableComposition(DWM_EC_ENABLECOMPOSITION);
        if (FAILED(hr) && hr != DWM_E_COMPOSITIONDISABLED)
            return hr;
        if (attempt == 1 || (attempt % 10) == 0)
            probeLog("AERO composition transition attempt=%lu request-hr=0x%08lx WAIT\r\n",
                     (unsigned long)attempt, (unsigned long)hr);
        if ((LONG)(GetTickCount() - deadline) >= 0)
            return hr;
        Sleep(1000);
    }
}

/* WinSAT can return after it enables composition but before DWM accepts
 * per-window glass calls.  Retry only the documented transition error and
 * preserve every other HRESULT as an immediate, actionable failure. */
static HRESULT
probeAeroExtendFrameWhenReady(PFN_DWMEXTENDFRAMEINTOCLIENTAREA extendFrame,
                              HWND window, const MARGINS *margins)
{
    DWORD deadline = GetTickCount() + PROBE_AERO_TRANSITION_TIMEOUT_MS;
    DWORD attempt = 0;
    HRESULT hr;

    for (;;) {
        ++attempt;
        hr = extendFrame(window, margins);
        if (hr != DWM_E_COMPOSITIONDISABLED ||
            (LONG)(GetTickCount() - deadline) >= 0)
            return hr;
        if (attempt == 1 || (attempt % 10) == 0)
            probeLog("AERO DwmExtendFrame transition attempt=%lu WAIT\r\n",
                     (unsigned long)attempt);
        Sleep(1000);
    }
}

static HRESULT
probeAeroEnableBlurWhenReady(PFN_DWMENABLEBLURBEHINDWINDOW enableBlurBehind,
                             HWND window,
                             const DWM_BLURBEHIND *blurBehind)
{
    DWORD deadline = GetTickCount() + PROBE_AERO_TRANSITION_TIMEOUT_MS;
    DWORD attempt = 0;
    HRESULT hr;

    for (;;) {
        ++attempt;
        hr = enableBlurBehind(window, blurBehind);
        if (hr != DWM_E_COMPOSITIONDISABLED ||
            (LONG)(GetTickCount() - deadline) >= 0)
            return hr;
        if (attempt == 1 || (attempt % 10) == 0)
            probeLog("AERO DwmEnableBlur transition attempt=%lu WAIT\r\n",
                     (unsigned long)attempt);
        Sleep(1000);
    }
}

static HRESULT
probeAeroLogState(HWND window, const char *event,
                  PFN_DWMISCOMPOSITIONENABLED isCompositionEnabled,
                  PFN_DWMGETCOLORIZATIONCOLOR getColorizationColor,
                  PFN_DWMGETWINDOWATTRIBUTE getWindowAttribute,
                  BOOL *enabled)
{
    DWORD color = 0;
    BOOL opaque = TRUE;
    BOOL ncEnabled = FALSE;
    HRESULT compositionHr;
    HRESULT colorHr;
    HRESULT windowHr;

    *enabled = FALSE;
    compositionHr = isCompositionEnabled(enabled);
    colorHr = getColorizationColor(&color, &opaque);
    windowHr = getWindowAttribute(window, DWMWA_NCRENDERING_ENABLED,
                                  &ncEnabled, sizeof(ncEnabled));
    probeLog("AERO state event=%s tick=%lu composition=%lu hr=0x%08lx "
             "color=0x%08lx opaque=%lu color-hr=0x%08lx nc=%lu nc-hr=0x%08lx\r\n",
             event, (unsigned long)GetTickCount(), (unsigned long)*enabled,
             (unsigned long)compositionHr, (unsigned long)color,
             (unsigned long)opaque, (unsigned long)colorHr,
             (unsigned long)ncEnabled, (unsigned long)windowHr);
    if (FAILED(compositionHr))
        return compositionHr;
    if (*enabled && FAILED(colorHr))
        return colorHr;
    if (*enabled && FAILED(windowHr))
        return windowHr;
    return S_OK;
}

/* DwmEnableComposition is not a capability override.  It is the documented
 * request that asks the currently logged-in DWM to use its hardware
 * compositor.  A DWM_E_COMPOSITIONDISABLED result is the useful negative
 * outcome here: the driver was not accepted and we must fix its real DDI
 * workload rather than alter milcore or the user's theme settings.
 *
 * This probe deliberately does NOT label successful DWM calls as Aero.  The
 * Aero Basic compositor accepts these calls too.  Real Aero requires a later
 * pixel-level check for translucent glass and background blur. */
static int
probeAeroRun(BOOL useBlurBehind)
{
    HMODULE dwmapi = NULL;
    PFN_DWMISCOMPOSITIONENABLED isCompositionEnabled;
    PFN_DWMENABLECOMPOSITION enableComposition;
    PFN_DWMEXTENDFRAMEINTOCLIENTAREA extendFrame;
    PFN_DWMENABLEBLURBEHINDWINDOW enableBlurBehind;
    PFN_DWMGETCOLORIZATIONCOLOR getColorizationColor;
    PFN_DWMGETWINDOWATTRIBUTE getWindowAttribute;
    HINSTANCE instance = GetModuleHandleA(NULL);
    HWND backdrop = NULL;
    HWND window = NULL;
    MARGINS margins;
    DWM_BLURBEHIND blurBehind;
    RECT client;
    HRGN region = NULL;
    MSG message;
    BOOL enabledBefore = FALSE;
    BOOL enabledAfter = FALSE;
    HRESULT hr;
    DWORD deadline;
    DWORD nextStateLog;
    DWORD nextReapply;
    ProbeAeroWindowState windowState = {0};
    char resultLog[MAX_PATH];
    int result = 1;

    if (!probeBuildChildLogPath(resultLog, sizeof(resultLog), "aero"))
        return 1;
    g_log = CreateFileA(resultLog,
                        GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_log == INVALID_HANDLE_VALUE)
        return 1;
    probeLog("TRITON9-RUN nonce=%s child=aero\r\n", g_probeResultNonce);
    probeLog("TRITON9-AERO begin arch=%u\r\n",
             (unsigned)(sizeof(void *) * 8));
    probeAeroUxSmsState();
    dwmapi = LoadLibraryA("dwmapi.dll");
    if (!dwmapi) {
        probeLog("AERO LoadLibrary(dwmapi) error=%lu FAIL\r\n",
                 (unsigned long)GetLastError());
        goto cleanup;
    }
    if (!probeProc(dwmapi, "DwmIsCompositionEnabled", &isCompositionEnabled,
                   sizeof(isCompositionEnabled)) ||
        !probeProc(dwmapi, "DwmEnableComposition", &enableComposition,
                   sizeof(enableComposition)) ||
        !probeProc(dwmapi, "DwmExtendFrameIntoClientArea", &extendFrame,
                   sizeof(extendFrame)) ||
        !probeProc(dwmapi, "DwmEnableBlurBehindWindow", &enableBlurBehind,
                   sizeof(enableBlurBehind)) ||
        !probeProc(dwmapi, "DwmGetColorizationColor", &getColorizationColor,
                   sizeof(getColorizationColor)) ||
        !probeProc(dwmapi, "DwmGetWindowAttribute", &getWindowAttribute,
                   sizeof(getWindowAttribute))) {
        probeLog("AERO resolve dwmapi exports error=%lu FAIL\r\n",
                 (unsigned long)GetLastError());
        goto cleanup;
    }
    hr = isCompositionEnabled(&enabledBefore);
    probeHr("AERO DwmIsCompositionEnabled before", hr);
    probeLog("AERO composition before=%lu\r\n", (unsigned long)enabledBefore);
    if (FAILED(hr))
        goto cleanup;
    if (!enabledBefore) {
        hr = probeAeroEnableCompositionWhenReady(isCompositionEnabled,
                                                 enableComposition,
                                                 &enabledAfter);
        probeHr("AERO DwmEnableComposition initial", hr);
        probeLog("AERO composition after initial request=%lu\r\n",
                 (unsigned long)enabledAfter);
        if (FAILED(hr) && hr != DWM_E_COMPOSITIONDISABLED)
            goto cleanup;
        if (!enabledAfter) {
            (void)probeRunWinSatDwm();
            hr = probeAeroEnableCompositionWhenReady(isCompositionEnabled,
                                                     enableComposition,
                                                     &enabledAfter);
            probeHr("AERO DwmEnableComposition after WinSAT", hr);
            probeLog("AERO composition after WinSAT=%lu\r\n",
                     (unsigned long)enabledAfter);
            if (FAILED(hr) || !enabledAfter)
                goto cleanup;
        } else {
            probeLog("AERO WinSAT(DWM) skipped=composition-enabled PASS\r\n");
        }
    } else {
        enabledAfter = TRUE;
        probeLog("AERO DwmEnableComposition skipped=already-enabled PASS\r\n");
    }
    hr = isCompositionEnabled(&enabledAfter);
    probeHr("AERO DwmIsCompositionEnabled after", hr);
    probeLog("AERO composition after=%lu\r\n", (unsigned long)enabledAfter);
    if (FAILED(hr) || !enabledAfter)
        goto cleanup;

    backdrop = probeCreateAeroBackdrop(instance);
    if (!backdrop) {
        probeLog("AERO CreateBackdrop(Default) error=%lu FAIL\r\n",
                 (unsigned long)GetLastError());
        goto cleanup;
    }
    window = probeCreateAeroWindow(instance, &windowState);
    if (!window) {
        probeLog("AERO CreateWindow(Default) error=%lu FAIL\r\n",
                 (unsigned long)GetLastError());
        goto cleanup;
    }
    ShowWindow(window, SW_SHOWNORMAL);
    UpdateWindow(window);
    margins.cxLeftWidth = -1;
    margins.cxRightWidth = -1;
    margins.cyTopHeight = -1;
    margins.cyBottomHeight = -1;
    /* Use explicit client blur for this borderless plate. Keep frame
     * extension as a separate diagnostic so their results are independent. */
    probeLog("AERO mechanism=%s\r\n",
             useBlurBehind ? "blur-behind" : "extended-frame");
    if (useBlurBehind) {
        GetClientRect(window, &client);
        region = CreateRectRgn(client.left, client.top, client.right, client.bottom);
        if (!region) {
            probeLog("AERO CreateRectRgn error=%lu FAIL\r\n",
                     (unsigned long)GetLastError());
            goto cleanup;
        }
        ZeroMemory(&blurBehind, sizeof(blurBehind));
        blurBehind.dwFlags = DWM_BB_ENABLE | DWM_BB_BLURREGION;
        blurBehind.fEnable = TRUE;
        blurBehind.hRgnBlur = region;
        hr = probeAeroEnableBlurWhenReady(enableBlurBehind, window, &blurBehind);
        probeHr("AERO DwmEnableBlurBehindWindow", hr);
        if (FAILED(hr))
            goto cleanup;
    } else {
        hr = probeAeroExtendFrameWhenReady(extendFrame, window, &margins);
        probeHr("AERO DwmExtendFrameIntoClientArea", hr);
        if (FAILED(hr))
            goto cleanup;
    }
    InvalidateRect(window, NULL, FALSE);
    UpdateWindow(window);
    hr = probeAeroLogState(window, "initial", isCompositionEnabled,
                           getColorizationColor, getWindowAttribute,
                           &enabledAfter);
    if (FAILED(hr) && hr != DWM_E_COMPOSITIONDISABLED) {
        probeHr("AERO initial state", hr);
        goto cleanup;
    }
    if (!enabledAfter || hr == DWM_E_COMPOSITIONDISABLED)
        windowState.reapplyPending = TRUE;
    probeLog("TRITON9-AERO API-PATH proof-pattern backdrop=%dx%d+%d+%d glass=%dx%d+%d+%d stripe=%d hold-ms=%lu VISUAL-UNVERIFIED\r\n",
             PROBE_AERO_BACKDROP_WIDTH, PROBE_AERO_BACKDROP_HEIGHT,
             PROBE_AERO_BACKDROP_X, PROBE_AERO_BACKDROP_Y,
             PROBE_AERO_GLASS_WIDTH, PROBE_AERO_GLASS_HEIGHT,
             PROBE_AERO_GLASS_X, PROBE_AERO_GLASS_Y,
             PROBE_AERO_STRIPE_WIDTH,
             (unsigned long)PROBE_AERO_WINDOW_HOLD_MS);
    probeUpload();

    deadline = GetTickCount() + PROBE_AERO_WINDOW_HOLD_MS;
    nextStateLog = GetTickCount() + 5000u;
    nextReapply = GetTickCount();
    for (;;) {
        DWORD now;

        while (PeekMessageA(&message, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageA(&message);
        }
        if (!IsWindow(window) || !IsWindow(backdrop)) {
            probeLog("AERO proof window closed before hold completed FAIL\r\n");
            goto cleanup;
        }
        now = GetTickCount();
        if (windowState.compositionChanged) {
            windowState.reapplyPending = TRUE;
            nextReapply = now;
            windowState.compositionChanged = FALSE;
        }
        if (windowState.attributesChanged ||
            (LONG)(now - nextStateLog) >= 0) {
            const char *event = windowState.attributesChanged
                                    ? "changed" : "periodic";

            windowState.attributesChanged = FALSE;
            hr = probeAeroLogState(window, event, isCompositionEnabled,
                                   getColorizationColor, getWindowAttribute,
                                   &enabledAfter);
            if (FAILED(hr) && hr != DWM_E_COMPOSITIONDISABLED) {
                probeHr("AERO hold state", hr);
                goto cleanup;
            }
            if (!enabledAfter || hr == DWM_E_COMPOSITIONDISABLED)
                windowState.reapplyPending = TRUE;
            nextStateLog = now + 5000u;
        }
        if (windowState.reapplyPending && (LONG)(now - nextReapply) >= 0) {
            /* DWM calls may dispatch a newer composition notification. */
            windowState.reapplyPending = FALSE;
            hr = isCompositionEnabled(&enabledAfter);
            if (SUCCEEDED(hr)) {
                if (!enabledAfter)
                    hr = DWM_E_COMPOSITIONDISABLED;
                else if (useBlurBehind)
                    hr = enableBlurBehind(window, &blurBehind);
                else
                    hr = extendFrame(window, &margins);
            }
            probeLog("AERO reapply mechanism=%s tick=%lu hr=0x%08lx %s\r\n",
                     useBlurBehind ? "blur-behind" : "extended-frame",
                     (unsigned long)now, (unsigned long)hr,
                     SUCCEEDED(hr) ? "PASS" :
                     hr == DWM_E_COMPOSITIONDISABLED ? "WAIT" : "FAIL");
            if (SUCCEEDED(hr)) {
                windowState.attributesChanged = TRUE;
                InvalidateRect(window, NULL, FALSE);
                UpdateWindow(window);
            } else if (hr == DWM_E_COMPOSITIONDISABLED) {
                windowState.reapplyPending = TRUE;
            } else {
                goto cleanup;
            }
            nextReapply = now + 1000u;
        }
        if ((LONG)(now - deadline) >= 0)
            break;
        Sleep(50);
    }
    hr = probeAeroLogState(window, "final", isCompositionEnabled,
                           getColorizationColor, getWindowAttribute,
                           &enabledAfter);
    if (FAILED(hr) || !enabledAfter || windowState.reapplyPending ||
        windowState.compositionChanged) {
        probeLog("AERO hold ended without an active effect hr=0x%08lx "
                 "composition=%lu reapply-pending=%lu composition-changed=%lu FAIL\r\n",
                 (unsigned long)hr, (unsigned long)enabledAfter,
                 (unsigned long)windowState.reapplyPending,
                 (unsigned long)windowState.compositionChanged);
        goto cleanup;
    }
    result = 0;
    probeLog("TRITON9-AERO API-PATH PASS VISUAL-UNVERIFIED\r\n");

cleanup:
    if (result)
        probeLog("TRITON9-AERO FAIL\r\n");
    if (region)
        DeleteObject(region);
    if (window)
        DestroyWindow(window);
    if (backdrop)
        DestroyWindow(backdrop);
    if (g_log != INVALID_HANDLE_VALUE) {
        CloseHandle(g_log);
        g_log = INVALID_HANDLE_VALUE;
    }
    (void)dwmapi;
    probeCloseTelemetry();
    return result;
}

static void
probeKmtUmdFilename(void)
{
    PFN_D3DKMTOPENADAPTERFROMHDC openAdapter;
    PFN_D3DKMTQUERYADAPTERINFO queryAdapterInfo;
    PFN_D3DKMTCLOSEADAPTER closeAdapter;
    PROBE_D3DKMT_OPENADAPTERFROMHDC open;
    PROBE_D3DKMT_QUERYADAPTERINFO query;
    PROBE_D3DKMT_UMDFILENAMEINFO filename;
    PROBE_D3DKMT_CLOSEADAPTER close;
    char narrowName[MAX_PATH * 3];
    HDC dc = NULL;
    LONG status;
    LONG closeStatus;
    int converted;

    probeEnter("KMT UMDRIVERNAME(DX9)");
    g_gdi32Module = LoadLibraryA("gdi32.dll");
    if (!g_gdi32Module) {
        probeLog("LoadLibrary(gdi32.dll) error=%lu FAIL\r\n",
                 (unsigned long)GetLastError());
        return;
    }
    if (!probeProc(g_gdi32Module, "D3DKMTOpenAdapterFromHdc", &openAdapter,
                   sizeof(openAdapter)) ||
        !probeProc(g_gdi32Module, "D3DKMTQueryAdapterInfo",
                   &queryAdapterInfo, sizeof(queryAdapterInfo)) ||
        !probeProc(g_gdi32Module, "D3DKMTCloseAdapter", &closeAdapter,
                   sizeof(closeAdapter))) {
        probeLog("resolve Vista D3DKMT thunks error=%lu FAIL\r\n",
                 (unsigned long)GetLastError());
        return;
    }

    dc = GetDC(NULL);
    if (!dc) {
        probeLog("GetDC(NULL) error=%lu FAIL\r\n",
                 (unsigned long)GetLastError());
        return;
    }
    ZeroMemory(&open, sizeof(open));
    open.hDc = dc;
    status = openAdapter(&open);
    probeLog("D3DKMTOpenAdapterFromHdc status=0x%08lx handle=0x%08lx "
             "luid=%08lx:%08lx source=%lu %s\r\n",
             (unsigned long)status, (unsigned long)open.hAdapter,
             (unsigned long)open.AdapterLuid.HighPart,
             (unsigned long)open.AdapterLuid.LowPart,
             (unsigned long)open.VidPnSourceId,
             status >= 0 ? "PASS" : "FAIL");
    ReleaseDC(NULL, dc);
    dc = NULL;
    if (status < 0 || !open.hAdapter) {
        probeUpload();
        return;
    }

    ZeroMemory(&filename, sizeof(filename));
    filename.Version = PROBE_KMTUMDVERSION_DX9;
    ZeroMemory(&query, sizeof(query));
    query.hAdapter = open.hAdapter;
    query.Type = PROBE_KMTQAITYPE_UMDRIVERNAME;
    query.pPrivateDriverData = &filename;
    query.PrivateDriverDataSize = sizeof(filename);
    status = queryAdapterInfo(&query);
    filename.UmdFileName[MAX_PATH - 1] = L'\0';
    narrowName[0] = '\0';
    converted = WideCharToMultiByte(CP_ACP, 0, filename.UmdFileName, -1,
                                    narrowName, sizeof(narrowName), NULL,
                                    NULL);
    if (!converted)
        narrowName[0] = '\0';
    probeLog("D3DKMTQueryAdapterInfo type=1 version=0 size=%lu "
             "status=0x%08lx name=\"%s\" %s\r\n",
             (unsigned long)sizeof(filename), (unsigned long)status,
             narrowName, status >= 0 && narrowName[0] ? "PASS" : "FAIL");

    close.hAdapter = open.hAdapter;
    closeStatus = closeAdapter(&close);
    probeLog("D3DKMTCloseAdapter status=0x%08lx %s\r\n",
             (unsigned long)closeStatus,
             closeStatus >= 0 ? "PASS" : "FAIL");
    probeUpload();
}

static HRESULT
probeReadPixel(IDirect3DDevice9Ex *device, IDirect3DSurface9 *source,
               IDirect3DSurface9 *readback, UINT x, UINT y, DWORD *pixel)
{
    D3DLOCKED_RECT locked;
    HRESULT hr;

    if (!device || !source || !readback || !pixel)
        return E_INVALIDARG;
    hr = IDirect3DDevice9Ex_GetRenderTargetData(device, source, readback);
    if (FAILED(hr))
        return hr;
    ZeroMemory(&locked, sizeof(locked));
    hr = IDirect3DSurface9_LockRect(readback, &locked, NULL, D3DLOCK_READONLY);
    if (FAILED(hr))
        return hr;
    if (!locked.pBits || locked.Pitch < (INT)(PROBE_WIDTH * sizeof(DWORD))) {
        IDirect3DSurface9_UnlockRect(readback);
        return E_FAIL;
    }
    *pixel = *(const DWORD *)((const BYTE *)locked.pBits +
                             (SIZE_T)y * (SIZE_T)locked.Pitch +
                             (SIZE_T)x * sizeof(DWORD));
    hr = IDirect3DSurface9_UnlockRect(readback);
    return hr;
}

static BOOL
probeNearRed(DWORD pixel)
{
    return ((pixel >> 16) & 0xffu) >= 0xe0u &&
           ((pixel >> 8) & 0xffu) <= 0x20u &&
           (pixel & 0xffu) <= 0x20u;
}

static BOOL
probeNearColor(DWORD pixel, DWORD expected)
{
    DWORD shift;

    for (shift = 0; shift <= 24; shift += 8) {
        DWORD actualComponent = (pixel >> shift) & 0xffu;
        DWORD expectedComponent = (expected >> shift) & 0xffu;
        DWORD delta = actualComponent > expectedComponent
            ? actualComponent - expectedComponent
            : expectedComponent - actualComponent;
        if (delta > 4)
            return FALSE;
    }
    return TRUE;
}

/* Mirror the state-only portion of CD3DDeviceLevel1::TestLevel1Device from
 * the checked Vista milcore.  That test runs immediately after device
 * creation, before MIL creates a render target or submits a draw.  Keeping
 * it as a public D3D9Ex probe prevents an omitted state value from looking
 * like a later resource, rendering, or presentation failure. */
static HRESULT
probeEightTextureStages(IDirect3DDevice9Ex *device, IDirect3DSurface9 *target,
                        IDirect3DSurface9 *readback)
{
    struct { float x,y,z; DWORD color; float uv[8][2]; } vertices[3];
    IDirect3DStateBlock9 *saved = NULL;
    IDirect3DTexture9 *texture = NULL;
    IDirect3DSurface9 *surface = NULL;
    D3DMATRIX identity = {{{0}}};
    HRESULT hr = IDirect3DDevice9Ex_CreateStateBlock(device, D3DSBT_ALL, &saved);
    DWORD pixel = 0;
    if (FAILED(hr)) goto done;
#define STAGE_CHECK(call) do { hr = (call); if (FAILED(hr)) goto done; } while (0)
    STAGE_CHECK(IDirect3DDevice9Ex_CreateTexture(device, 1, 1, 1,
        D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &texture, NULL));
    STAGE_CHECK(IDirect3DTexture9_GetSurfaceLevel(texture, 0, &surface));
    STAGE_CHECK(IDirect3DDevice9Ex_ColorFill(device, surface, NULL, 0xffe0e0e0));
    ZeroMemory(vertices, sizeof(vertices));
    vertices[0].x = -0.75f; vertices[0].y = -0.75f;
    vertices[1].x = 0.0f; vertices[1].y = 0.75f;
    vertices[2].x = 0.75f; vertices[2].y = -0.75f;
    for (UINT v = 0; v < 3; ++v) {
        vertices[v].z = 0.5f; vertices[v].color = 0xffffffff;
        for (UINT stage = 0; stage < 8; ++stage)
            vertices[v].uv[stage][0] = vertices[v].uv[stage][1] = 0.5f;
    }
    identity.m[0][0] = identity.m[1][1] = identity.m[2][2] = identity.m[3][3] = 1;
    STAGE_CHECK(IDirect3DDevice9Ex_SetVertexShader(device, NULL));
    STAGE_CHECK(IDirect3DDevice9Ex_SetPixelShader(device, NULL));
    STAGE_CHECK(IDirect3DDevice9Ex_SetFVF(device, D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX8));
    STAGE_CHECK(IDirect3DDevice9Ex_SetTransform(device, D3DTS_WORLD, &identity));
    STAGE_CHECK(IDirect3DDevice9Ex_SetTransform(device, D3DTS_VIEW, &identity));
    STAGE_CHECK(IDirect3DDevice9Ex_SetTransform(device, D3DTS_PROJECTION, &identity));
    STAGE_CHECK(IDirect3DDevice9Ex_SetRenderState(device, D3DRS_LIGHTING, FALSE));
    STAGE_CHECK(IDirect3DDevice9Ex_SetRenderState(device, D3DRS_ZENABLE, FALSE));
    STAGE_CHECK(IDirect3DDevice9Ex_SetRenderState(device, D3DRS_CULLMODE, D3DCULL_NONE));
    STAGE_CHECK(IDirect3DDevice9Ex_SetRenderState(device, D3DRS_ALPHABLENDENABLE, FALSE));
    STAGE_CHECK(IDirect3DDevice9Ex_SetRenderState(device, D3DRS_ALPHATESTENABLE, FALSE));
    STAGE_CHECK(IDirect3DDevice9Ex_SetRenderState(device, D3DRS_FOGENABLE, FALSE));
    /* Render into the texture, then bind it before selecting the next pass's
     * output. There is no read/write feedback during a draw. DWM uses this
     * state order when recycling intermediate render targets. */
    STAGE_CHECK(IDirect3DDevice9Ex_SetRenderTarget(device, 0, surface));
    STAGE_CHECK(IDirect3DDevice9Ex_Clear(device, 0, NULL, D3DCLEAR_TARGET,
                                       0xffe0e0e0, 1, 0));
    for (UINT stage = 0; stage < 8; ++stage) {
        STAGE_CHECK(IDirect3DDevice9Ex_SetTexture(device, stage, (IDirect3DBaseTexture9 *)texture));
        STAGE_CHECK(IDirect3DDevice9Ex_SetTextureStageState(device, stage, D3DTSS_TEXCOORDINDEX, stage));
        STAGE_CHECK(IDirect3DDevice9Ex_SetTextureStageState(device, stage, D3DTSS_COLOROP, D3DTOP_MODULATE));
        STAGE_CHECK(IDirect3DDevice9Ex_SetTextureStageState(device, stage, D3DTSS_COLORARG1, D3DTA_TEXTURE));
        STAGE_CHECK(IDirect3DDevice9Ex_SetTextureStageState(device, stage, D3DTSS_COLORARG2, D3DTA_CURRENT));
        STAGE_CHECK(IDirect3DDevice9Ex_SetTextureStageState(device, stage, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1));
        STAGE_CHECK(IDirect3DDevice9Ex_SetTextureStageState(device, stage, D3DTSS_ALPHAARG1, D3DTA_TEXTURE));
    }
    STAGE_CHECK(IDirect3DDevice9Ex_SetRenderTarget(device, 0, target));
    probeLog("Texture binding before output switch PASS\r\n");
    STAGE_CHECK(IDirect3DDevice9Ex_Clear(device, 0, NULL, D3DCLEAR_TARGET, 0xff112233, 1, 0));
    STAGE_CHECK(IDirect3DDevice9Ex_BeginScene(device));
    hr = IDirect3DDevice9Ex_DrawPrimitiveUP(device, D3DPT_TRIANGLELIST, 1, vertices, sizeof(vertices[0]));
    {
        HRESULT end = IDirect3DDevice9Ex_EndScene(device);
        if (FAILED(hr)) goto done;
        if (FAILED(end)) { hr = end; goto done; }
    }
    STAGE_CHECK(probeReadPixel(device, target, readback, 64, 64, &pixel));
    /* Eight multiplications: round(255 * (224/255)^8) = 90. */
    probeLog("Eight texture stages pixel=%08lx expected=ff5a5a5a %s\r\n",
        (unsigned long)pixel, probeNearColor(pixel, 0xff5a5a5a) ? "PASS" : "FAIL");
    if (!probeNearColor(pixel, 0xff5a5a5a)) { hr = E_FAIL; goto done; }
    /* COLOR factors also define alpha in D3D9's combined blend mode.
     * D3D11 requires scalar equivalents in the separate alpha slots. */
    STAGE_CHECK(IDirect3DDevice9Ex_SetRenderState(device, D3DRS_SEPARATEALPHABLENDENABLE, FALSE));
    STAGE_CHECK(IDirect3DDevice9Ex_SetRenderState(device, D3DRS_ALPHABLENDENABLE, TRUE));
    STAGE_CHECK(IDirect3DDevice9Ex_SetRenderState(device, D3DRS_SRCBLEND, D3DBLEND_SRCCOLOR));
    STAGE_CHECK(IDirect3DDevice9Ex_SetRenderState(device, D3DRS_DESTBLEND, D3DBLEND_ZERO));
    STAGE_CHECK(IDirect3DDevice9Ex_SetRenderState(device, D3DRS_BLENDOP, D3DBLENDOP_ADD));
    STAGE_CHECK(IDirect3DDevice9Ex_BeginScene(device));
    hr = IDirect3DDevice9Ex_DrawPrimitiveUP(device, D3DPT_TRIANGLELIST, 1, vertices, sizeof(vertices[0]));
    {
        HRESULT end = IDirect3DDevice9Ex_EndScene(device);
        if (FAILED(hr)) goto done;
        if (FAILED(end)) { hr = end; goto done; }
    }
    STAGE_CHECK(probeReadPixel(device, target, readback, 64, 64, &pixel));
    probeLog("Combined color blend pixel=%08lx expected=ff202020 %s\r\n",
        (unsigned long)pixel, probeNearColor(pixel, 0xff202020) ? "PASS" : "FAIL");
    if (!probeNearColor(pixel, 0xff202020)) hr = E_FAIL;
done:
    if (saved) {
        HRESULT restoreTarget = IDirect3DDevice9Ex_SetRenderTarget(device, 0, target);
        if (SUCCEEDED(hr)) hr = restoreTarget;
        HRESULT restore = IDirect3DStateBlock9_Apply(saved);
        if (SUCCEEDED(hr)) hr = restore;
        IDirect3DStateBlock9_Release(saved);
    }
    if (surface) IDirect3DSurface9_Release(surface);
    if (texture) IDirect3DTexture9_Release(texture);
#undef STAGE_CHECK
    return hr;
}

static HRESULT
probeStretchRectPixels(IDirect3DDevice9Ex *device)
{
    IDirect3DSurface9 *source = NULL, *destination = NULL, *readback = NULL;
    const DWORD sentinel = 0xff112233;
    HRESULT hr;
    struct StretchCase {
        RECT source, destination;
        D3DTEXTUREFILTERTYPE filter;
        UINT x, y, grey;
    } cases[] = {
        {{0,0,4,4}, {3,2,5,4}, D3DTEXF_LINEAR, 3,2,24},
        {{0,0,4,4}, {3,2,5,4}, D3DTEXF_LINEAR, 4,3,120},
        {{0,0,4,4}, {3,2,5,4}, D3DTEXF_POINT, 3,2,48},
        {{1,1,3,3}, {2,2,5,5}, D3DTEXF_LINEAR, 3,3,72},
        {{1,1,3,3}, {2,2,5,5}, D3DTEXF_LINEAR, 2,2,48},
        {{1,1,3,3}, {2,2,5,5}, D3DTEXF_POINT, 4,4,96},
    };
    hr = IDirect3DDevice9Ex_CreateRenderTarget(device, 4, 4, D3DFMT_A8R8G8B8,
        D3DMULTISAMPLE_NONE, 0, FALSE, &source, NULL);
    if (FAILED(hr)) goto done;
    hr = IDirect3DDevice9Ex_CreateRenderTarget(device, 8, 8, D3DFMT_A8R8G8B8,
        D3DMULTISAMPLE_NONE, 0, FALSE, &destination, NULL);
    if (FAILED(hr)) goto done;
    hr = IDirect3DDevice9Ex_CreateOffscreenPlainSurface(device, 8, 8,
        D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &readback, NULL);
    if (FAILED(hr)) goto done;
    for (UINT y = 0; y < 4; ++y) {
        for (UINT x = 0; x < 4; ++x) {
            RECT pixel = {(LONG)x, (LONG)y, (LONG)x+1, (LONG)y+1};
            DWORD grey = x * 40 + y * 8;
            hr = IDirect3DDevice9Ex_ColorFill(device, source, &pixel,
                                             0xff000000 | grey * 0x010101);
            if (FAILED(hr)) goto done;
        }
    }
    for (UINT i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        D3DLOCKED_RECT locked;
        DWORD actual, outside;
        DWORD expected = 0xff000000 | cases[i].grey * 0x010101;
        hr = IDirect3DDevice9Ex_ColorFill(device, destination, NULL, sentinel);
        if (FAILED(hr)) goto done;
        hr = IDirect3DDevice9Ex_StretchRect(device, source, &cases[i].source,
            destination, &cases[i].destination, cases[i].filter);
        probeHr("StretchRect GPU operation", hr);
        if (FAILED(hr)) goto done;
        hr = IDirect3DDevice9Ex_GetRenderTargetData(device, destination, readback);
        if (FAILED(hr)) goto done;
        hr = IDirect3DSurface9_LockRect(readback, &locked, NULL, D3DLOCK_READONLY);
        if (FAILED(hr)) goto done;
        if (!locked.pBits || locked.Pitch < 32) {
            IDirect3DSurface9_UnlockRect(readback);
            hr = E_FAIL;
            goto done;
        }
        actual = *(DWORD *)((BYTE *)locked.pBits + cases[i].y * locked.Pitch + cases[i].x * 4);
        outside = *(DWORD *)locked.pBits;
        hr = IDirect3DSurface9_UnlockRect(readback);
        probeLog("StretchRect case=%u pixel=%08lx expected=%08lx outside=%08lx %s\r\n",
            i, (unsigned long)actual, (unsigned long)expected, (unsigned long)outside,
            probeNearColor(actual, expected) && outside == sentinel ? "PASS" : "FAIL");
        if (FAILED(hr) || !probeNearColor(actual, expected) || outside != sentinel) {
            hr = E_FAIL;
            goto done;
        }
    }
done:
    if (readback) IDirect3DSurface9_Release(readback);
    if (destination) IDirect3DSurface9_Release(destination);
    if (source) IDirect3DSurface9_Release(source);
    return hr;
}

static HRESULT
probeMilSetFilterMode(IDirect3DDevice9Ex *device, UINT stage,
                      UINT interpolation)
{
    DWORD minFilter;
    DWORD magFilter;
    DWORD mipFilter;
    HRESULT hr;

    switch (interpolation) {
    case 0: /* MILBitmapInterpolationModeNearestNeighbor */
        minFilter = D3DTEXF_POINT;
        magFilter = D3DTEXF_POINT;
        mipFilter = D3DTEXF_POINT;
        break;
    case 1: /* MILBitmapInterpolationModeLinear */
        minFilter = D3DTEXF_LINEAR;
        magFilter = D3DTEXF_LINEAR;
        mipFilter = D3DTEXF_POINT;
        break;
    case 4: /* MILBitmapInterpolationModeTriLinear */
        minFilter = D3DTEXF_LINEAR;
        magFilter = D3DTEXF_LINEAR;
        mipFilter = D3DTEXF_LINEAR;
        break;
    default:
        return E_INVALIDARG;
    }
    hr = IDirect3DDevice9Ex_SetSamplerState(device, stage, D3DSAMP_MINFILTER,
                                             minFilter);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9Ex_SetSamplerState(device, stage,
                                                 D3DSAMP_MAGFILTER, magFilter);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9Ex_SetSamplerState(device, stage,
                                                 D3DSAMP_MIPFILTER, mipFilter);
    return hr;
}

static HRESULT
probeMilSetTextureState(IDirect3DDevice9Ex *device, UINT blendArgument,
                        UINT interpolation, UINT masks)
{
    HRESULT hr;
    UINT stage;
    DWORD colorOperation;
    DWORD colorArgument2;

    if (!device || blendArgument > 2 || masks > 1)
        return E_INVALIDARG;
    /* TBM_DEFAULT always selects premultiplied source-over. */
    hr = IDirect3DDevice9Ex_SetPixelShader(device, NULL);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9Ex_SetVertexShader(device, NULL);
    if (SUCCEEDED(hr))
        hr = probeMilSetFilterMode(device, 0, interpolation);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9Ex_SetRenderState(device, D3DRS_ALPHABLENDENABLE,
                                                TRUE);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9Ex_SetRenderState(device, D3DRS_SRCBLEND,
                                                D3DBLEND_ONE);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9Ex_SetRenderState(device, D3DRS_DESTBLEND,
                                                D3DBLEND_INVSRCALPHA);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9Ex_SetTexture(device, 0, NULL);

    /* TBA_None is select-texture. TBA_Diffuse and TBA_Specular are texture
     * modulation with COLOR0 and COLOR1 respectively. */
    colorOperation = blendArgument == 0 ? D3DTOP_SELECTARG1 : D3DTOP_MODULATE;
    colorArgument2 = blendArgument == 1 ? D3DTA_DIFFUSE :
                     blendArgument == 2 ? D3DTA_SPECULAR : D3DTA_CURRENT;
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9Ex_SetTextureStageState(device, 0, D3DTSS_COLOROP,
                                                     colorOperation);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9Ex_SetTextureStageState(device, 0,
                                                     D3DTSS_COLORARG1,
                                                     D3DTA_TEXTURE);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9Ex_SetTextureStageState(device, 0,
                                                     D3DTSS_COLORARG2,
                                                     colorArgument2);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9Ex_SetTextureStageState(device, 0, D3DTSS_ALPHAOP,
                                                     D3DTOP_SELECTARG1);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9Ex_SetTextureStageState(device, 0,
                                                     D3DTSS_ALPHAARG1,
                                                     D3DTA_TEXTURE);

    for (stage = 1; SUCCEEDED(hr) && stage <= masks; ++stage) {
        hr = probeMilSetFilterMode(device, stage, interpolation);
        if (SUCCEEDED(hr))
            hr = IDirect3DDevice9Ex_SetTextureStageState(device, stage,
                                                         D3DTSS_COLOROP,
                                                         D3DTOP_MODULATE);
        if (SUCCEEDED(hr))
            hr = IDirect3DDevice9Ex_SetTextureStageState(device, stage,
                                                         D3DTSS_COLORARG1,
                                                         D3DTA_TEXTURE);
        if (SUCCEEDED(hr))
            hr = IDirect3DDevice9Ex_SetTextureStageState(device, stage,
                                                         D3DTSS_COLORARG2,
                                                         D3DTA_CURRENT);
        if (SUCCEEDED(hr))
            hr = IDirect3DDevice9Ex_SetTextureStageState(device, stage,
                                                         D3DTSS_ALPHAOP,
                                                         D3DTOP_SELECTARG1);
        if (SUCCEEDED(hr))
            hr = IDirect3DDevice9Ex_SetTextureStageState(device, stage,
                                                         D3DTSS_ALPHAARG1,
                                                         D3DTA_TEXTURE);
    }
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9Ex_SetTextureStageState(device, masks + 1,
                                                     D3DTSS_COLOROP,
                                                     D3DTOP_DISABLE);
    return hr;
}

static HRESULT
probeMilLevel1StateContract(IDirect3DDevice9Ex *device)
{
    static const UINT blendArguments[] = {0, 1, 2};
    static const UINT interpolations[] = {0, 1, 4};
    HRESULT hr;
    UINT argumentIndex;
    UINT interpolationIndex;
    UINT masks;

    if (!device)
        return E_INVALIDARG;

    /* SetRenderState_AlphaSolidBrush. */
    hr = IDirect3DDevice9Ex_SetRenderState(device, D3DRS_ALPHABLENDENABLE,
                                            TRUE);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9Ex_SetRenderState(device, D3DRS_SRCBLEND,
                                                D3DBLEND_ONE);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9Ex_SetRenderState(device, D3DRS_DESTBLEND,
                                                D3DBLEND_INVSRCALPHA);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9Ex_SetPixelShader(device, NULL);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9Ex_SetVertexShader(device, NULL);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9Ex_SetTextureStageState(device, 0, D3DTSS_COLOROP,
                                                     D3DTOP_SELECTARG1);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9Ex_SetTextureStageState(device, 0,
                                                     D3DTSS_COLORARG1,
                                                     D3DTA_DIFFUSE);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9Ex_SetTextureStageState(device, 1, D3DTSS_COLOROP,
                                                     D3DTOP_DISABLE);
    if (FAILED(hr))
        return hr;

    for (masks = 0; masks <= 1; ++masks) {
        for (argumentIndex = 0;
             argumentIndex < sizeof(blendArguments) / sizeof(blendArguments[0]);
             ++argumentIndex) {
            for (interpolationIndex = 0;
                 interpolationIndex < sizeof(interpolations) /
                                      sizeof(interpolations[0]);
                 ++interpolationIndex) {
                probeLog("MIL Level1 texture state arg=%u filter=%u masks=%u ENTER\\r\\n",
                         blendArguments[argumentIndex],
                         interpolations[interpolationIndex], masks);
                hr = probeMilSetTextureState(device, blendArguments[argumentIndex],
                                             interpolations[interpolationIndex],
                                             masks);
                if (FAILED(hr))
                    return hr;
            }
        }
    }
    return S_OK;
}

/* Generate a deliberately small, valid SM2 pair without requiring D3DX or a
 * compiler DLL on the Vista guest.  The tokens use the same documented D3D9
 * encodings as Triton's fixed-function shader generator, but take the public
 * CreateVertexShader/CreatePixelShader route that MIL's shader pipeline uses.
 *
 * vs_2_0                         ps_2_0
 * def c0, 1, 0, 0, 0             dcl_color v0
 * dcl_position v0                mov oC0, v0
 * dcl_color v1
 * mov oPos.xyz, v0
 * mov oPos.w, c0.x
 * mov oD0, v1
 *
 * Keep the complete position write.  The Vista-era VirtualBox WDDM DDI
 * exercises this exact SM2 shape: the input declaration supplies FLOAT3,
 * and the shader must establish clip-space w explicitly before the runtime
 * accepts a hardware draw.  It also gives Triton's converter the normal
 * inline-constant path used by compositor shaders.
 */
static DWORD
probeShaderRegisterType(D3DSHADER_PARAM_REGISTER_TYPE type)
{
    return ((DWORD)type << D3DSP_REGTYPE_SHIFT & D3DSP_REGTYPE_MASK) |
           ((DWORD)type << D3DSP_REGTYPE_SHIFT2 & D3DSP_REGTYPE_MASK2);
}

static DWORD
probeShaderDestination(D3DSHADER_PARAM_REGISTER_TYPE type, DWORD index)
{
    return 0x80000000u | probeShaderRegisterType(type) |
           (index & D3DSP_REGNUM_MASK) | D3DSP_WRITEMASK_ALL;
}

static DWORD
probeShaderMaskedDestination(D3DSHADER_PARAM_REGISTER_TYPE type, DWORD index,
                             DWORD writeMask)
{
    return 0x80000000u | probeShaderRegisterType(type) |
           (index & D3DSP_REGNUM_MASK) | writeMask;
}

static DWORD
probeShaderSource(D3DSHADER_PARAM_REGISTER_TYPE type, DWORD index)
{
    return 0x80000000u | probeShaderRegisterType(type) |
           (index & D3DSP_REGNUM_MASK) | D3DSP_NOSWIZZLE;
}

/* A masked destination selects the corresponding source component.  For
 * "mov oPos.w, c0.x", all four source selectors must therefore be x (zero),
 * not the normal xyzw swizzle used by probeShaderSource(). */
static DWORD
probeShaderSourceReplicateX(D3DSHADER_PARAM_REGISTER_TYPE type, DWORD index)
{
    return triton9_sm2_replicate_x(probeShaderSource(type, index));
}

static DWORD
probeShaderInstruction(D3DSHADER_INSTRUCTION_OPCODE_TYPE opcode,
                       DWORD parameterCount)
{
    return (DWORD)opcode | (parameterCount << D3DSI_INSTLENGTH_SHIFT);
}

static void
probeBuildSm2PassthroughShaders(DWORD vertex[23], DWORD pixel[8])
{
    vertex[0] = D3DVS_VERSION(2, 0);
    vertex[1] = probeShaderInstruction(D3DSIO_DEF, 5);
    vertex[2] = probeShaderDestination(D3DSPR_CONST, 0);
    vertex[3] = 0x3f800000u;
    vertex[4] = 0x00000000u;
    vertex[5] = 0x00000000u;
    vertex[6] = 0x00000000u;
    vertex[7] = probeShaderInstruction(D3DSIO_DCL, 2);
    vertex[8] = triton9_sm2_dcl_semantic(D3DDECLUSAGE_POSITION, 0);
    vertex[9] = probeShaderDestination(D3DSPR_INPUT, 0);
    vertex[10] = probeShaderInstruction(D3DSIO_DCL, 2);
    vertex[11] = triton9_sm2_dcl_semantic(D3DDECLUSAGE_COLOR, 0);
    vertex[12] = probeShaderDestination(D3DSPR_INPUT, 1);
    vertex[13] = probeShaderInstruction(D3DSIO_MOV, 2);
    vertex[14] = probeShaderMaskedDestination(
        D3DSPR_RASTOUT, D3DSRO_POSITION,
        D3DSP_WRITEMASK_0 | D3DSP_WRITEMASK_1 | D3DSP_WRITEMASK_2);
    vertex[15] = probeShaderSource(D3DSPR_INPUT, 0);
    vertex[16] = probeShaderInstruction(D3DSIO_MOV, 2);
    vertex[17] = probeShaderMaskedDestination(D3DSPR_RASTOUT,
                                               D3DSRO_POSITION,
                                               D3DSP_WRITEMASK_3);
    vertex[18] = probeShaderSourceReplicateX(D3DSPR_CONST, 0);
    vertex[19] = probeShaderInstruction(D3DSIO_MOV, 2);
    vertex[20] = probeShaderDestination(D3DSPR_ATTROUT, 0);
    vertex[21] = probeShaderSource(D3DSPR_INPUT, 1);
    vertex[22] = D3DVS_END();

    pixel[0] = D3DPS_VERSION(2, 0);
    pixel[1] = probeShaderInstruction(D3DSIO_DCL, 2);
    /* ps_2_0 colour inputs have fixed v# semantics. */
    pixel[2] = triton9_sm2_dcl_semantic(0, 0);
    pixel[3] = probeShaderDestination(D3DSPR_INPUT, 0);
    pixel[4] = probeShaderInstruction(D3DSIO_MOV, 2);
    pixel[5] = probeShaderDestination(D3DSPR_COLOROUT, 0);
    pixel[6] = probeShaderSource(D3DSPR_INPUT, 0);
    pixel[7] = D3DPS_END();
}

/* A real MIL shader draw binds a texture and pixel constants, so a coloured
 * pass-through primitive is not enough evidence that the D3D9 shader path
 * works.  This intentionally modest ps_2_0 sequence exercises exactly those
 * runtime/DDI boundaries without requiring D3DX or a compiler DLL on Vista.
 *
 * vs_2_0                         ps_2_0
 * dcl_position v0                dcl t0
 * dcl_color v1                   dcl_2d s0
 * dcl_texcoord v2                texld r0, t0, s0
 * mov oPos, v0                   mul r0, r0, c0
 * mov oD0, v1                    mov oC0, r0
 * mov oT0, v2
 */
static void
probeBuildSm2TexturedShaders(DWORD vertex[26], DWORD pixel[19])
{
    vertex[0] = D3DVS_VERSION(2, 0);
    vertex[1] = probeShaderInstruction(D3DSIO_DCL, 2);
    vertex[2] = triton9_sm2_dcl_semantic(D3DDECLUSAGE_POSITION, 0);
    vertex[3] = probeShaderDestination(D3DSPR_INPUT, 0);
    vertex[4] = probeShaderInstruction(D3DSIO_DCL, 2);
    vertex[5] = triton9_sm2_dcl_semantic(D3DDECLUSAGE_COLOR, 0);
    vertex[6] = probeShaderDestination(D3DSPR_INPUT, 1);
    vertex[7] = probeShaderInstruction(D3DSIO_DCL, 2);
    vertex[8] = triton9_sm2_dcl_semantic(D3DDECLUSAGE_TEXCOORD, 0);
    vertex[9] = probeShaderDestination(D3DSPR_INPUT, 2);
    vertex[10] = probeShaderInstruction(D3DSIO_DCL, 2);
    vertex[11] = triton9_sm2_dcl_semantic(D3DDECLUSAGE_TEXCOORD, 1);
    vertex[12] = probeShaderDestination(D3DSPR_INPUT, 3);
    vertex[13] = probeShaderInstruction(D3DSIO_MOV, 2);
    vertex[14] = probeShaderDestination(D3DSPR_RASTOUT, D3DSRO_POSITION);
    vertex[15] = probeShaderSource(D3DSPR_INPUT, 0);
    vertex[16] = probeShaderInstruction(D3DSIO_MOV, 2);
    vertex[17] = probeShaderDestination(D3DSPR_ATTROUT, 0);
    vertex[18] = probeShaderSource(D3DSPR_INPUT, 1);
    /* Output coordinate declarations are unnecessary in vs_2_0: oT0/oT1
     * have fixed TEXCOORD0/TEXCOORD1 meanings. */
    vertex[19] = probeShaderInstruction(D3DSIO_MOV, 2);
    vertex[20] = probeShaderDestination(D3DSPR_TEXCRDOUT, 0);
    vertex[21] = probeShaderSource(D3DSPR_INPUT, 2);
    vertex[22] = probeShaderInstruction(D3DSIO_MOV, 2);
    vertex[23] = probeShaderDestination(D3DSPR_TEXCRDOUT, 1);
    vertex[24] = probeShaderSource(D3DSPR_INPUT, 3);
    vertex[25] = D3DVS_END();

    pixel[0] = D3DPS_VERSION(2, 0);
    pixel[1] = probeShaderInstruction(D3DSIO_DCL, 2);
    pixel[2] = triton9_sm2_dcl_semantic(0, 0);
    pixel[3] = probeShaderDestination(D3DSPR_TEXTURE, 0);
    pixel[4] = probeShaderInstruction(D3DSIO_DCL, 2);
    pixel[5] = triton9_sm2_dcl_sampler(D3DSTT_2D);
    pixel[6] = probeShaderDestination(D3DSPR_SAMPLER, 0);
    /* Vista's d3d9types.h names the ps_2_0 texld opcode D3DSIO_TEX. */
    pixel[7] = probeShaderInstruction(D3DSIO_TEX, 3);
    pixel[8] = probeShaderDestination(D3DSPR_TEMP, 0);
    pixel[9] = probeShaderSource(D3DSPR_TEXTURE, 0);
    pixel[10] = probeShaderSource(D3DSPR_SAMPLER, 0);
    pixel[11] = probeShaderInstruction(D3DSIO_MUL, 3);
    pixel[12] = probeShaderDestination(D3DSPR_TEMP, 0);
    pixel[13] = probeShaderSource(D3DSPR_TEMP, 0);
    pixel[14] = probeShaderSource(D3DSPR_CONST, 0);
    pixel[15] = probeShaderInstruction(D3DSIO_MOV, 2);
    pixel[16] = probeShaderDestination(D3DSPR_COLOROUT, 0);
    pixel[17] = probeShaderSource(D3DSPR_TEMP, 0);
    pixel[18] = D3DPS_END();
}

/* DWM's blur uses t0..t7 with distinct offsets. Uniform textures cannot
 * detect a converter that aliases these eight inputs to TEXCOORD0. */
static HRESULT
probeSm2DistinctCoordinates(IDirect3DDevice9Ex *device,
                            IDirect3DSurface9 *target, IDirect3DSurface9 *readback)
{
    struct { float x,y,z; DWORD color; float uv[8][2]; } vertices[3];
    IDirect3DStateBlock9 *saved = NULL;
    IDirect3DTexture9 *texture = NULL;
    IDirect3DSurface9 *surface = NULL;
    IDirect3DPixelShader9 *shader = NULL;
    DWORD tokens[128], n = 0, pixel = 0;
    const float weight[4] = { .125f, .125f, .125f, .125f };
    D3DMATRIX identity = {{{0}}};
    HRESULT hr = IDirect3DDevice9Ex_CreateStateBlock(device, D3DSBT_ALL, &saved);
    if (FAILED(hr)) goto done;
#define COORD_CHECK(call) do { hr = (call); if (FAILED(hr)) { probeHr(#call, hr); goto done; } } while (0)
    tokens[n++] = D3DPS_VERSION(2, 0);
    for (UINT i = 0; i < 8; ++i) {
        tokens[n++] = probeShaderInstruction(D3DSIO_DCL, 2);
        tokens[n++] = triton9_sm2_dcl_semantic(0, 0);
        tokens[n++] = probeShaderDestination(D3DSPR_TEXTURE, i);
    }
    tokens[n++] = probeShaderInstruction(D3DSIO_DCL, 2);
    tokens[n++] = triton9_sm2_dcl_sampler(D3DSTT_2D);
    tokens[n++] = probeShaderDestination(D3DSPR_SAMPLER, 0);
    for (UINT i = 0; i < 8; ++i) {
        tokens[n++] = probeShaderInstruction(D3DSIO_TEX, 3);
        tokens[n++] = probeShaderDestination(D3DSPR_TEMP, i);
        tokens[n++] = probeShaderSource(D3DSPR_TEXTURE, i);
        tokens[n++] = probeShaderSource(D3DSPR_SAMPLER, 0);
    }
    /* Rewriting a TEXLD destination counts as a dependent read in SM2.
     * Use independent temporaries, as DWM's real eight-tap shader does. */
    for (UINT i = 1; i < 8; ++i) {
        tokens[n++] = probeShaderInstruction(D3DSIO_ADD, 3);
        tokens[n++] = probeShaderDestination(D3DSPR_TEMP, 0);
        tokens[n++] = probeShaderSource(D3DSPR_TEMP, 0);
        tokens[n++] = probeShaderSource(D3DSPR_TEMP, i);
    }
    tokens[n++] = probeShaderInstruction(D3DSIO_MUL, 3);
    tokens[n++] = probeShaderDestination(D3DSPR_TEMP, 0);
    tokens[n++] = probeShaderSource(D3DSPR_TEMP, 0);
    tokens[n++] = probeShaderSource(D3DSPR_CONST, 0);
    /* SM2 output registers only accept an unmodified full-width MOV. */
    tokens[n++] = probeShaderInstruction(D3DSIO_MOV, 2);
    tokens[n++] = probeShaderDestination(D3DSPR_COLOROUT, 0);
    tokens[n++] = probeShaderSource(D3DSPR_TEMP, 0);
    tokens[n++] = D3DPS_END();
    COORD_CHECK(IDirect3DDevice9Ex_CreatePixelShader(device, tokens, &shader));
    COORD_CHECK(IDirect3DDevice9Ex_CreateTexture(device, 8, 1, 1,
        D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &texture, NULL));
    COORD_CHECK(IDirect3DTexture9_GetSurfaceLevel(texture, 0, &surface));
    for (UINT i = 0; i < 8; ++i) {
        RECT rect = { (LONG)i, 0, (LONG)i + 1, 1 };
        COORD_CHECK(IDirect3DDevice9Ex_ColorFill(device, surface, &rect,
            0xff000000u | (i * 32u * 0x010101u)));
    }
    ZeroMemory(vertices, sizeof(vertices));
    vertices[0].x = -.75f; vertices[0].y = -.75f;
    vertices[1].x = 0; vertices[1].y = .75f;
    vertices[2].x = .75f; vertices[2].y = -.75f;
    for (UINT v = 0; v < 3; ++v) {
        vertices[v].z = .5f; vertices[v].color = 0xffffffff;
        for (UINT i = 0; i < 8; ++i) {
            vertices[v].uv[i][0] = (i + .5f) / 8;
            vertices[v].uv[i][1] = .5f;
        }
    }
    identity.m[0][0] = identity.m[1][1] = identity.m[2][2] = identity.m[3][3] = 1;
    COORD_CHECK(IDirect3DDevice9Ex_SetVertexShader(device, NULL));
    COORD_CHECK(IDirect3DDevice9Ex_SetPixelShader(device, shader));
    COORD_CHECK(IDirect3DDevice9Ex_SetFVF(device, D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX8));
    COORD_CHECK(IDirect3DDevice9Ex_SetTransform(device, D3DTS_WORLD, &identity));
    COORD_CHECK(IDirect3DDevice9Ex_SetTransform(device, D3DTS_VIEW, &identity));
    COORD_CHECK(IDirect3DDevice9Ex_SetTransform(device, D3DTS_PROJECTION, &identity));
    COORD_CHECK(IDirect3DDevice9Ex_SetRenderState(device, D3DRS_LIGHTING, FALSE));
    COORD_CHECK(IDirect3DDevice9Ex_SetRenderState(device, D3DRS_ZENABLE, FALSE));
    COORD_CHECK(IDirect3DDevice9Ex_SetRenderState(device, D3DRS_CULLMODE, D3DCULL_NONE));
    COORD_CHECK(IDirect3DDevice9Ex_SetRenderState(device, D3DRS_ALPHABLENDENABLE, FALSE));
    COORD_CHECK(IDirect3DDevice9Ex_SetRenderState(device, D3DRS_ALPHATESTENABLE, FALSE));
    COORD_CHECK(IDirect3DDevice9Ex_SetRenderState(device, D3DRS_FOGENABLE, FALSE));
    COORD_CHECK(IDirect3DDevice9Ex_SetRenderTarget(device, 0, target));
    COORD_CHECK(IDirect3DDevice9Ex_SetTexture(device, 0, (IDirect3DBaseTexture9 *)texture));
    COORD_CHECK(IDirect3DDevice9Ex_SetSamplerState(device, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT));
    COORD_CHECK(IDirect3DDevice9Ex_SetSamplerState(device, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT));
    COORD_CHECK(IDirect3DDevice9Ex_SetPixelShaderConstantF(device, 0, weight, 1));
    COORD_CHECK(IDirect3DDevice9Ex_BeginScene(device));
    hr = IDirect3DDevice9Ex_DrawPrimitiveUP(device, D3DPT_TRIANGLELIST, 1, vertices, sizeof(vertices[0]));
    {
        HRESULT end = IDirect3DDevice9Ex_EndScene(device);
        if (FAILED(hr)) goto done;
        if (FAILED(end)) { hr = end; goto done; }
    }
    COORD_CHECK(probeReadPixel(device, target, readback, 64, 64, &pixel));
    probeLog("SM2 eight distinct coordinates pixel=%08lx expected=ff707070 %s\r\n",
        (unsigned long)pixel, probeNearColor(pixel, 0xff707070) ? "PASS" : "FAIL");
    if (!probeNearColor(pixel, 0xff707070)) hr = E_FAIL;
done:
    if (saved) {
        HRESULT restore = IDirect3DStateBlock9_Apply(saved);
        if (SUCCEEDED(hr)) hr = restore;
        IDirect3DStateBlock9_Release(saved);
    }
    if (shader) IDirect3DPixelShader9_Release(shader);
    if (surface) IDirect3DSurface9_Release(surface);
    if (texture) IDirect3DTexture9_Release(texture);
#undef COORD_CHECK
    return hr;
}

static BOOL
probeDeleteOwnService(void)
{
    SC_HANDLE manager;
    SC_HANDLE service;
    BOOL deleted = FALSE;

    manager = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT);
    if (!manager)
        return FALSE;
    service = OpenServiceA(manager, g_probeServiceName, DELETE);
    if (service) {
        deleted = DeleteService(service);
        CloseServiceHandle(service);
    }
    CloseServiceHandle(manager);
    return deleted;
}

static BOOL
probeEnablePrivilege(const char *name)
{
    HANDLE token;
    TOKEN_PRIVILEGES privileges;
    BOOL adjusted;

    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return FALSE;
    ZeroMemory(&privileges, sizeof(privileges));
    privileges.PrivilegeCount = 1;
    if (!LookupPrivilegeValueA(NULL, name,
                               &privileges.Privileges[0].Luid)) {
        CloseHandle(token);
        return FALSE;
    }
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    SetLastError(NO_ERROR);
    adjusted = AdjustTokenPrivileges(token, FALSE, &privileges, 0, NULL, NULL);
    if (adjusted && GetLastError() == ERROR_NOT_ALL_ASSIGNED)
        adjusted = FALSE;
    CloseHandle(token);
    return adjusted;
}

static DWORD
probeFindProcessInSession(DWORD sessionId, const char *imageName)
{
    HANDLE snapshot;
    PROCESSENTRY32 entry;
    DWORD processSession;
    DWORD processId = 0;

    snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return 0;
    ZeroMemory(&entry, sizeof(entry));
    entry.dwSize = sizeof(entry);
    if (!imageName || !imageName[0]) {
        CloseHandle(snapshot);
        return 0;
    }
    if (Process32First(snapshot, &entry)) {
        do {
            if (lstrcmpiA(entry.szExeFile, imageName) == 0 &&
                ProcessIdToSessionId(entry.th32ProcessID, &processSession) &&
                processSession == sessionId) {
                processId = entry.th32ProcessID;
                break;
            }
        } while (Process32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return processId;
}

static DWORD
probeFindWinlogon(DWORD sessionId)
{
    return probeFindProcessInSession(sessionId, "winlogon.exe");
}

static BOOL
probeLaunchPublicDesktop(PROCESS_INFORMATION *processInfo,
                         DWORD *desktopProcessId)
{
    DWORD sessionId;
    DWORD processId;
    HANDLE process = NULL;
    HANDLE token = NULL;
    HANDLE primaryToken = NULL;
    STARTUPINFOA startup;
    char application[MAX_PATH];
    char commandLine[MAX_PATH + 192];
    BOOL launched = FALSE;

    if (!processInfo)
        return FALSE;
    ZeroMemory(processInfo, sizeof(*processInfo));
    sessionId = WTSGetActiveConsoleSessionId();
    if (sessionId == 0xffffffffu) {
        SetLastError(ERROR_NO_SUCH_LOGON_SESSION);
        return FALSE;
    }
    /* This independent public D3D9 contract already passed end to end on the
     * interactive session's Winlogon desktop.  Checked Vista hangs after its
     * KMT adapter preflight when the same diagnostic process is moved to
     * Default.  Keep this non-visual gate on its proven desktop; the separate
     * Aero process below owns the user's Default desktop. */
    processId = probeFindWinlogon(sessionId);
    if (!processId) {
        SetLastError(ERROR_NOT_FOUND);
        return FALSE;
    }
    if (desktopProcessId)
        *desktopProcessId = processId;
    process = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, processId);
    if (!process)
        goto cleanup;
    if (!OpenProcessToken(process, TOKEN_DUPLICATE | TOKEN_QUERY |
                          TOKEN_ASSIGN_PRIMARY, &token))
        goto cleanup;
    if (!DuplicateTokenEx(token, MAXIMUM_ALLOWED, NULL,
                          SecurityImpersonation, TokenPrimary, &primaryToken))
        goto cleanup;
    if (!GetModuleFileNameA(NULL, application, sizeof(application)))
        goto cleanup;
    if (!probeValidHex(g_probeResultNonce, PROBE_RESULT_NONCE_LENGTH) ||
        snprintf(commandLine, sizeof(commandLine),
                 "\"%s\" --secure-probe --result-nonce %s",
                 application, g_probeResultNonce) <= 0) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        goto cleanup;
    }
    ZeroMemory(&startup, sizeof(startup));
    startup.cb = sizeof(startup);
    startup.lpDesktop = (LPSTR)"winsta0\\Winlogon";
    launched = CreateProcessAsUserA(
        primaryToken, application, commandLine, NULL, NULL, FALSE,
        CREATE_NEW_PROCESS_GROUP, NULL, "C:\\Windows\\Temp", &startup,
        processInfo);

cleanup:
    if (primaryToken)
        CloseHandle(primaryToken);
    if (token)
        CloseHandle(token);
    if (process)
        CloseHandle(process);
    return launched;
}

/* Run DWM's public API as the logged-in Explorer user on Default.  DWM, theme
 * state, and the controlled glass windows are per-user resources.  This exact
 * context previously reached DwmIsCompositionEnabled and WinSAT; using a
 * Winlogon token here would prove a different desktop contract. */
static BOOL
probeLaunchAeroDesktop(PROCESS_INFORMATION *processInfo,
                       DWORD *desktopProcessId)
{
    DWORD sessionId;
    DWORD processId;
    HANDLE process = NULL;
    HANDLE token = NULL;
    HANDLE primaryToken = NULL;
    TOKEN_LINKED_TOKEN linkedToken = {0};
    TOKEN_ELEVATION_TYPE elevationType;
    DWORD tokenBytes;
    STARTUPINFOA startup;
    char application[MAX_PATH];
    char commandLine[MAX_PATH + 192];
    BOOL launched = FALSE;

    if (!processInfo)
        return FALSE;
    ZeroMemory(processInfo, sizeof(*processInfo));
    sessionId = WTSGetActiveConsoleSessionId();
    if (sessionId == 0xffffffffu) {
        SetLastError(ERROR_NO_SUCH_LOGON_SESSION);
        return FALSE;
    }
    processId = probeFindProcessInSession(sessionId, "explorer.exe");
    if (!processId) {
        SetLastError(ERROR_NOT_FOUND);
        return FALSE;
    }
    if (desktopProcessId)
        *desktopProcessId = processId;
    process = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, processId);
    if (!process)
        goto cleanup;
    if (!OpenProcessToken(process, TOKEN_DUPLICATE | TOKEN_QUERY |
                          TOKEN_ASSIGN_PRIMARY, &token))
        goto cleanup;
    /* WinSAT is an elevation-required executable. Use this same user's
     * linked administrator token when UAC filtered Explorer's token. */
    if (GetTokenInformation(token, TokenElevationType, &elevationType,
                            sizeof(elevationType), &tokenBytes) &&
        elevationType == TokenElevationTypeLimited) {
        if (!GetTokenInformation(token, TokenLinkedToken, &linkedToken,
                                 sizeof(linkedToken), &tokenBytes))
            goto cleanup;
        CloseHandle(token);
        token = linkedToken.LinkedToken;
        probeLog("AERO desktop token            same-user elevated PASS\r\n");
    }
    if (!DuplicateTokenEx(token, MAXIMUM_ALLOWED, NULL,
                          SecurityImpersonation, TokenPrimary, &primaryToken))
        goto cleanup;
    if (!GetModuleFileNameA(NULL, application, sizeof(application)))
        goto cleanup;
    if (!probeValidHex(g_probeResultNonce, PROBE_RESULT_NONCE_LENGTH) ||
        snprintf(commandLine, sizeof(commandLine),
                 "\"%s\" --aero-probe --result-nonce %s",
                 application, g_probeResultNonce) <= 0) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        goto cleanup;
    }
    ZeroMemory(&startup, sizeof(startup));
    startup.cb = sizeof(startup);
    startup.lpDesktop = (LPSTR)"winsta0\\Default";
    launched = CreateProcessAsUserA(
        primaryToken, application, commandLine, NULL, NULL, FALSE,
        CREATE_NEW_PROCESS_GROUP, NULL, "C:\\Windows\\Temp", &startup,
        processInfo);

cleanup:
    if (primaryToken)
        CloseHandle(primaryToken);
    if (token)
        CloseHandle(token);
    if (process)
        CloseHandle(process);
    return launched;
}

/* UxSms owns Vista's per-session DWM launch.  The deployment service already
 * runs as LocalSystem in normal mode, so it can make this documented SCM
 * request without a keyboard, a registry policy edit, or host VM control.
 * Do not mistake a stopped demand-start service for a D3D9 driver verdict:
 * wait for it to run and then let DWM itself accept or reject Triton. */
static BOOL
probeEnsureUxSmsRunning(void)
{
    SC_HANDLE manager = NULL;
    SC_HANDLE service = NULL;
    SERVICE_STATUS_PROCESS status;
    DWORD bytes = 0;
    DWORD attempt;
    BOOL started = FALSE;
    BOOL ready = FALSE;

    manager = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT);
    if (!manager) {
        probeLog("AERO OpenSCManager(UxSms) error=%lu FAIL\r\n",
                 (unsigned long)GetLastError());
        goto done;
    }
    service = OpenServiceA(manager, "UxSms", SERVICE_QUERY_STATUS |
                           SERVICE_START);
    if (!service) {
        probeLog("AERO OpenService(UxSms,start) error=%lu FAIL\r\n",
                 (unsigned long)GetLastError());
        goto done;
    }
    ZeroMemory(&status, sizeof(status));
    if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, (BYTE *)&status,
                              sizeof(status), &bytes)) {
        probeLog("AERO QueryServiceStatusEx(UxSms,start) error=%lu FAIL\r\n",
                 (unsigned long)GetLastError());
        goto done;
    }
    probeLog("AERO UxSms before-start state=%lu pid=%lu\r\n",
             (unsigned long)status.dwCurrentState,
             (unsigned long)status.dwProcessId);
    if (status.dwCurrentState == SERVICE_RUNNING) {
        ready = TRUE;
        goto done;
    }
    if (status.dwCurrentState == SERVICE_STOPPED) {
        if (!StartServiceA(service, 0, NULL)) {
            DWORD error = GetLastError();

            if (error != ERROR_SERVICE_ALREADY_RUNNING) {
                probeLog("AERO StartService(UxSms) error=%lu FAIL\r\n",
                         (unsigned long)error);
                goto done;
            }
        }
        started = TRUE;
    }
    for (attempt = 0; attempt < 120; ++attempt) {
        Sleep(500);
        ZeroMemory(&status, sizeof(status));
        if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                                  (BYTE *)&status, sizeof(status), &bytes)) {
            probeLog("AERO QueryServiceStatusEx(UxSms,wait) error=%lu FAIL\r\n",
                     (unsigned long)GetLastError());
            goto done;
        }
        if (status.dwCurrentState == SERVICE_RUNNING) {
            ready = TRUE;
            break;
        }
        if (status.dwCurrentState == SERVICE_STOPPED)
            break;
    }

done:
    probeLog("AERO UxSms start-request=%lu final-state=%lu %s\r\n",
             (unsigned long)started, (unsigned long)status.dwCurrentState,
             ready ? "PASS" : "FAIL");
    if (service)
        CloseServiceHandle(service);
    if (manager)
        CloseServiceHandle(manager);
    return ready;
}

static int
probeRun(int argc, char **argv)
{
    const DWORD clearColor = 0xff102030u;
    const DWORD requiredStencilCaps =
        D3DSTENCILCAPS_KEEP | D3DSTENCILCAPS_ZERO |
        D3DSTENCILCAPS_REPLACE | D3DSTENCILCAPS_INCRSAT |
        D3DSTENCILCAPS_DECRSAT | D3DSTENCILCAPS_INVERT |
        D3DSTENCILCAPS_INCR | D3DSTENCILCAPS_DECR |
        D3DSTENCILCAPS_TWOSIDED;
    HINSTANCE instance = GetModuleHandleA(NULL);
    HMODULE d3d9Module = NULL;
    FARPROC create9ExAddress;
    PFN_DIRECT3DCREATE9EX create9Ex;
    IDirect3D9Ex *d3d = NULL;
    IDirect3DDevice9Ex *device = NULL;
    IDirect3DDevice9Ex *pureDevice = NULL;
    IDirect3DSurface9 *target = NULL;
    IDirect3DSurface9 *depthStencil = NULL;
    IDirect3DSurface9 *readback = NULL;
    IDirect3DSurface9 *backbuffer = NULL;
    IDirect3DSwapChain9 *additionalSwapChain = NULL;
    IDirect3DSurface9 *additionalBackbuffer = NULL;
    IDirect3DVertexDeclaration9 *shaderDeclarationObject = NULL;
    IDirect3DVertexDeclaration9 *texturedDeclarationObject = NULL;
    IDirect3DVertexShader9 *vertexShader = NULL;
    IDirect3DPixelShader9 *pixelShader = NULL;
    IDirect3DPixelShader9 *inlinePixelShader = NULL;
    IDirect3DVertexShader9 *texturedVertexShader = NULL;
    IDirect3DPixelShader9 *texturedPixelShader = NULL;
    IDirect3DTexture9 *sampleTexture = NULL;
    IDirect3DTexture9 *defaultTexture = NULL;
    IDirect3DSurface9 *sysmemSurface = NULL;
    IDirect3DSurface9 *defaultSurface = NULL;
    D3DADAPTER_IDENTIFIER9 identifier;
    D3DDISPLAYMODE mode;
    D3DCAPS9 caps;
    D3DPRESENT_PARAMETERS present;
    D3DVIEWPORT9 viewport;
    D3DMATERIAL9 material;
    D3DMATRIX identity;
    D3DLOCKED_RECT textureLock;
    PROBE_VERTEX triangle[3];
    PROBE_SPECULAR_VERTEX specularTriangle[3];
    PROBE_TEXTURE_VERTEX textureTriangle[3];
    DWORD vertexShaderTokens[23];
    DWORD pixelShaderTokens[8];
    DWORD texturedVertexShaderTokens[26];
    DWORD texturedPixelShaderTokens[19];
    D3DVERTEXELEMENT9 shaderDeclaration[] = {
        { 0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
          D3DDECLUSAGE_POSITION, 0 },
        { 0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT,
          D3DDECLUSAGE_COLOR, 0 },
        D3DDECL_END()
    };
    D3DVERTEXELEMENT9 texturedDeclaration[] = {
        { 0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
          D3DDECLUSAGE_POSITION, 0 },
        { 0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT,
          D3DDECLUSAGE_COLOR, 0 },
        { 0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT,
          D3DDECLUSAGE_TEXCOORD, 0 },
        { 0, 24, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT,
          D3DDECLUSAGE_TEXCOORD, 1 },
        D3DDECL_END()
    };
    const FLOAT textureModulate[4] = { 0.5f, 0.5f, 1.0f, 1.0f };
    const DWORD textureColor = 0xffc06020u;
    const DWORD texturedExpected = 0xff603020u;
    const DWORD bitmapTextureColor = 0xff40a0c0u;
    const DWORD bitmapExpected = 0xff2050c0u;
    DWORD pixel;
    HWND window = NULL;
    HRESULT hr;
    HRESULT pureHr = E_FAIL;
    int result = 1;
    BOOL pureProfilePassed = FALSE;
    BOOL hold = argc > 1 && strcmp(argv[1], "--hold") == 0;
    DWORD sessionId = 0xffffffffu;
    char desktopName[128] = "unknown";
    char resultLog[MAX_PATH];
    DWORD desktopNameLength = 0;

    if (!probeBuildChildLogPath(resultLog, sizeof(resultLog), "probe"))
        return 1;
    g_log = CreateFileA(resultLog,
                        GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_log == INVALID_HANDLE_VALUE)
        return 1;
    probeLog("TRITON9-RUN nonce=%s child=probe\r\n", g_probeResultNonce);
    probeLog("TRITON9-PROBE begin arch=%u\r\n", (unsigned)(sizeof(void *) * 8));
    ProcessIdToSessionId(GetCurrentProcessId(), &sessionId);
    GetUserObjectInformationA(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME,
                              desktopName, sizeof(desktopName),
                              &desktopNameLength);
    probeLog("execution session=%lu desktop=%s\r\n",
             (unsigned long)sessionId, desktopName);
    /* Publish the execution context before entering d3d9.dll.  If the Vista
     * runtime blocks while enumerating a malformed adapter, the host still
     * gets an exact last-completed boundary instead of a missing result. */
    probeUpload();

    probeKmtUmdFilename();

    probeEnter("LoadLibrary(d3d9.dll)");
    d3d9Module = LoadLibraryA("d3d9.dll");
    if (!d3d9Module) {
        probeLog("LoadLibrary(d3d9.dll) error=%lu FAIL\r\n", GetLastError());
        goto cleanup;
    }
    probeLog("LoadLibrary(d3d9.dll)         PASS\r\n");
    probeUpload();
    create9ExAddress = GetProcAddress(d3d9Module, "Direct3DCreate9Ex");
    if (!create9ExAddress) {
        probeLog("GetProcAddress(Direct3DCreate9Ex) error=%lu FAIL\r\n",
                 GetLastError());
        goto cleanup;
    }
    /* ISO C does not define an object-pointer conversion for function
     * pointers.  Win32 guarantees GetProcAddress returns the correct entry
     * address; copying the same-sized pointer avoids a diagnostic-only cast
     * disagreement in MinGW's x64 headers. */
    memcpy(&create9Ex, &create9ExAddress, sizeof(create9Ex));
    probeLog("Direct3DCreate9Ex export       PASS\r\n");
    probeUpload();

    probeEnter("Direct3DCreate9Ex");
    hr = create9Ex(D3D_SDK_VERSION, &d3d);
    probeHr("Direct3DCreate9Ex", hr);
    if (FAILED(hr) || !d3d)
        goto cleanup;

    ZeroMemory(&identifier, sizeof(identifier));
    probeEnter("GetAdapterIdentifier");
    hr = IDirect3D9Ex_GetAdapterIdentifier(d3d, D3DADAPTER_DEFAULT, 0,
                                            &identifier);
    probeHr("GetAdapterIdentifier", hr);
    if (SUCCEEDED(hr))
        probeLog("adapter driver=%s description=%s\r\n",
                 identifier.Driver, identifier.Description);

    ZeroMemory(&mode, sizeof(mode));
    probeEnter("GetAdapterDisplayMode");
    hr = IDirect3D9Ex_GetAdapterDisplayMode(d3d, D3DADAPTER_DEFAULT, &mode);
    probeHr("GetAdapterDisplayMode", hr);
    if (FAILED(hr))
        goto cleanup;
    probeLog("mode=%ux%u@%u format=%u\r\n", mode.Width, mode.Height,
             mode.RefreshRate, (unsigned)mode.Format);

    probeVistaCheckedEnumState(d3d);

    /* Vista's CEnum::CheckDeviceType searches the imported FORMATOP table
     * for this adapter format with both FORMATOP_DISPLAYMODE (0x400) and
     * FORMATOP_3DACCELERATION (0x800).  The checked runtime strips the latter
     * when IsD3DHALSupported rejects any D3DCAPS9 invariant, so the CEnum dump
     * above distinguishes that admission failure from a missing display mode. */
    probeEnter("CheckDeviceType windowed");
    hr = IDirect3D9Ex_CheckDeviceType(d3d, D3DADAPTER_DEFAULT,
                                      D3DDEVTYPE_HAL, mode.Format,
                                      mode.Format, TRUE);
    probeHr("CheckDeviceType windowed", hr);

    ZeroMemory(&caps, sizeof(caps));
    probeEnter("GetDeviceCaps(HAL)");
    hr = IDirect3D9Ex_GetDeviceCaps(d3d, D3DADAPTER_DEFAULT,
                                    D3DDEVTYPE_HAL, &caps);
    probeHr("GetDeviceCaps(HAL)", hr);
    if (FAILED(hr))
        goto cleanup;
    probeLog("caps DevCaps=0x%08lx Prim=0x%08lx Stencil=0x%08lx "
             "Filter=0x%08lx Stages=%lu Textures=%lu RTs=%lu "
             "VS=0x%08lx PS=0x%08lx\r\n",
             (unsigned long)caps.DevCaps,
             (unsigned long)caps.PrimitiveMiscCaps,
             (unsigned long)caps.StencilCaps,
             (unsigned long)caps.TextureFilterCaps,
             (unsigned long)caps.MaxTextureBlendStages,
             (unsigned long)caps.MaxSimultaneousTextures,
             (unsigned long)caps.NumSimultaneousRTs,
             (unsigned long)caps.VertexShaderVersion,
             (unsigned long)caps.PixelShaderVersion);
    if (!(caps.DevCaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT)) {
        probeLog("caps HW transform+light      FAIL\r\n");
        probeUpload();
        goto cleanup;
    }
    probeLog("caps HW transform+light      PASS\r\n");
    /* Checked Vista validates the raw DDI FOGINFVF bit before it returns
     * from GetDeviceCaps.  The public D3DCAPS9 is then normalized and does
     * not preserve that legacy bit.  Success above is the public admission
     * signal; testing the filtered caps for 0x2000 produces a false failure. */
    probeLog("caps checked SM2 fog-in-FVF admission via GetDeviceCaps PASS\r\n");
    if ((caps.StencilCaps & requiredStencilCaps) != requiredStencilCaps) {
        probeLog("caps D3D9_1 stencil contract missing=0x%08lx FAIL\r\n",
                 (unsigned long)(requiredStencilCaps & ~caps.StencilCaps));
        probeUpload();
        goto cleanup;
    }
    probeLog("caps D3D9_1 stencil contract PASS\r\n");
    probeUpload();

    probeEnter("CheckFormat A8R8G8B8 RT");
    hr = IDirect3D9Ex_CheckDeviceFormat(d3d, D3DADAPTER_DEFAULT,
                                        D3DDEVTYPE_HAL, mode.Format,
                                        D3DUSAGE_RENDERTARGET,
                                        D3DRTYPE_SURFACE, D3DFMT_A8R8G8B8);
    probeHr("CheckFormat A8R8G8B8 RT", hr);
    if (FAILED(hr))
        goto cleanup;
    probeEnter("CheckFormat X8R8G8B8 RT");
    hr = IDirect3D9Ex_CheckDeviceFormat(d3d, D3DADAPTER_DEFAULT,
                                        D3DDEVTYPE_HAL, mode.Format,
                                        D3DUSAGE_RENDERTARGET,
                                        D3DRTYPE_SURFACE, D3DFMT_X8R8G8B8);
    probeHr("CheckFormat X8R8G8B8 RT", hr);
    if (FAILED(hr))
        goto cleanup;
    /* DWM creates this depth/stencil surface immediately after its
     * composition colour targets. Test the public FORMATOP boundary first. */
    probeEnter("CheckFormat D24S8 depth/stencil");
    hr = IDirect3D9Ex_CheckDeviceFormat(d3d, D3DADAPTER_DEFAULT,
                                        D3DDEVTYPE_HAL, mode.Format,
                                        D3DUSAGE_DEPTHSTENCIL,
                                        D3DRTYPE_SURFACE, D3DFMT_D24S8);
    probeHr("CheckFormat D24S8 depth/stencil", hr);
    if (FAILED(hr))
        goto cleanup;

    window = probeCreateWindow(instance);
    if (!window) {
        probeLog("CreateWindow error=%lu FAIL\r\n", GetLastError());
        goto cleanup;
    }
    probeLog("CreateWindow                   PASS\r\n");
    probeUpload();

    ZeroMemory(&present, sizeof(present));
    present.BackBufferWidth = 320;
    present.BackBufferHeight = 240;
    present.BackBufferFormat = mode.Format;
    present.BackBufferCount = 1;
    present.MultiSampleType = D3DMULTISAMPLE_NONE;
    present.SwapEffect = D3DSWAPEFFECT_DISCARD;
    present.hDeviceWindow = window;
    present.Windowed = TRUE;
    present.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    /* The checked Vista renderer always requests hardware vertex processing
     * for its HAL path. PUREDEVICE is a second, conditional renderer profile,
     * so probe both rather than treating a failure of either profile as proof
     * about the other. The strict profile becomes the device used for the
     * readback and PresentEx checks when it succeeds. */
    probeEnter("CreateDeviceEx HWP");
    hr = IDirect3D9Ex_CreateDeviceEx(d3d, D3DADAPTER_DEFAULT,
                                     D3DDEVTYPE_HAL, window,
                                     D3DCREATE_FPU_PRESERVE |
                                     D3DCREATE_MULTITHREADED |
                                     D3DCREATE_HARDWARE_VERTEXPROCESSING,
                                     &present, NULL, &device);
    probeHr("CreateDeviceEx HWP", hr);
    if (FAILED(hr) || !device)
        goto cleanup;

    probeEnter("CreateDeviceEx HWP+PURE");
    for (UINT attempt = 1; attempt <= 6; ++attempt) {
    pureHr = IDirect3D9Ex_CreateDeviceEx(d3d, D3DADAPTER_DEFAULT,
                                         D3DDEVTYPE_HAL, window,
                                         D3DCREATE_FPU_PRESERVE |
                                         D3DCREATE_MULTITHREADED |
                                         D3DCREATE_HARDWARE_VERTEXPROCESSING |
                                         D3DCREATE_PUREDEVICE,
                                         &present, NULL, &pureDevice);
        if (pureHr != E_OUTOFMEMORY || attempt == 6)
            break;
        probeLog("CreateDeviceEx HWP+PURE allocation attempt=%u hr=0x%08lx retry-ms=5000\r\n",
                 attempt, (unsigned long)pureHr);
        if (pureDevice) {
            IDirect3DDevice9Ex_Release(pureDevice);
            pureDevice = NULL;
        }
        Sleep(5000);
    }
    probeHr("CreateDeviceEx HWP+PURE", pureHr);
    if (SUCCEEDED(pureHr) && pureDevice) {
        IDirect3DDevice9Ex_Release(device);
        device = pureDevice;
        pureDevice = NULL;
        pureProfilePassed = TRUE;
        probeLog("CreateDeviceEx strict profile PASS\r\n");
    } else {
        probeLog("CreateDeviceEx strict profile FAIL; continuing base-HWP diagnostics\r\n");
    }

    /* This is the state block Vista MIL applies while it validates a newly
     * created hardware device.  Keep it ahead of the first resource call:
     * a failure here is a D3D9 UMD contract error, not a draw or presentation
     * failure. */
    ZeroMemory(&material, sizeof(material));
    material.Diffuse.r = material.Diffuse.g = material.Diffuse.b =
        material.Diffuse.a = 1.0f;
    material.Ambient = material.Diffuse;
    probeEnter("MIL default state contract");
    hr = IDirect3DDevice9Ex_SetRenderState(device, D3DRS_LASTPIXEL, FALSE);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9Ex_SetRenderState(device,
                                                D3DRS_TWOSIDEDSTENCILMODE,
                                                FALSE);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9Ex_SetRenderState(device,
                                                D3DRS_DIFFUSEMATERIALSOURCE,
                                                D3DMCS_COLOR1);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9Ex_SetRenderState(device,
                                                D3DRS_SPECULARMATERIALSOURCE,
                                                D3DMCS_COLOR1);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9Ex_SetRenderState(device,
                                                D3DRS_AMBIENTMATERIALSOURCE,
                                                D3DMCS_MATERIAL);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9Ex_SetRenderState(device, D3DRS_AMBIENT, 0);
    if (SUCCEEDED(hr)) {
        UINT stage;
        for (stage = 0; stage < caps.MaxTextureBlendStages; ++stage) {
            hr = IDirect3DDevice9Ex_SetTextureStageState(device, stage,
                                                          D3DTSS_TEXCOORDINDEX,
                                                          stage);
            if (FAILED(hr))
                break;
        }
    }
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9Ex_SetMaterial(device, &material);
    probeHr("MIL default state contract", hr);
    if (FAILED(hr))
        goto cleanup;

    probeEnter("MIL Level1 state contract");
    hr = probeMilLevel1StateContract(device);
    probeHr("MIL Level1 state contract", hr);
    if (FAILED(hr))
        goto cleanup;

    /* These are not incidental Ex methods.  Vista's compositor checks the
     * device state while it schedules frames, then waits for vblank in its
     * adaptive presentation loop.  Exercise both public runtime boundaries
     * before any private render path can hide a missing WDDM contract. */
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    probeEnter("CheckDeviceState");
    hr = IDirect3DDevice9Ex_CheckDeviceState(device, window);
    probeHr("CheckDeviceState", hr);
    if (FAILED(hr))
        goto cleanup;
    probeEnter("WaitForVBlank");
    hr = IDirect3DDevice9Ex_WaitForVBlank(device, 0);
    probeHr("WaitForVBlank", hr);
    if (FAILED(hr))
        goto cleanup;

    probeEnter("CreateRenderTarget");
    hr = IDirect3DDevice9Ex_CreateRenderTarget(device, PROBE_WIDTH,
                                               PROBE_HEIGHT,
                                               D3DFMT_A8R8G8B8,
                                               D3DMULTISAMPLE_NONE, 0, FALSE,
                                               &target, NULL);
    probeHr("CreateRenderTarget", hr);
    if (FAILED(hr) || !target)
        goto cleanup;
    probeEnter("Create D24S8 depth/stencil");
    hr = IDirect3DDevice9Ex_CreateDepthStencilSurface(
        device, PROBE_WIDTH, PROBE_HEIGHT, D3DFMT_D24S8,
        D3DMULTISAMPLE_NONE, 0, FALSE, &depthStencil, NULL);
    probeHr("Create D24S8 depth/stencil", hr);
    if (FAILED(hr) || !depthStencil)
        goto cleanup;
    probeEnter("Create readback surface");
    hr = IDirect3DDevice9Ex_CreateOffscreenPlainSurface(device, PROBE_WIDTH,
                                                        PROBE_HEIGHT,
                                                        D3DFMT_A8R8G8B8,
                                                        D3DPOOL_SYSTEMMEM,
                                                        &readback, NULL);
    probeHr("Create readback surface", hr);
    if (FAILED(hr) || !readback)
        goto cleanup;
    probeEnter("SetRenderTarget");
    hr = IDirect3DDevice9Ex_SetRenderTarget(device, 0, target);
    probeHr("SetRenderTarget", hr);
    if (FAILED(hr))
        goto cleanup;
    probeEnter("Set D24S8 depth/stencil");
    hr = IDirect3DDevice9Ex_SetDepthStencilSurface(device, depthStencil);
    probeHr("Set D24S8 depth/stencil", hr);
    if (FAILED(hr))
        goto cleanup;

    ZeroMemory(&viewport, sizeof(viewport));
    viewport.Width = PROBE_WIDTH;
    viewport.Height = PROBE_HEIGHT;
    viewport.MaxZ = 1.0f;
    probeEnter("SetViewport");
    hr = IDirect3DDevice9Ex_SetViewport(device, &viewport);
    probeHr("SetViewport", hr);
    if (FAILED(hr))
        goto cleanup;
    probeEnter("Clear offscreen target");
    hr = IDirect3DDevice9Ex_Clear(device, 0, NULL, D3DCLEAR_TARGET,
                                  0xff112233u, 1.0f, 0);
    probeHr("Clear offscreen target", hr);
    if (FAILED(hr))
        goto cleanup;
    probeEnter("Clear D24S8 depth");
    hr = IDirect3DDevice9Ex_Clear(device, 0, NULL,
                                  D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
    probeHr("Clear D24S8 depth", hr);
    if (FAILED(hr))
        goto cleanup;
    probeEnter("Clear readback");
    hr = probeReadPixel(device, target, readback, 64, 64, &pixel);
    probeHr("Clear readback", hr);
    if (FAILED(hr))
        goto cleanup;
    probeLog("clear pixel=0x%08lx expected=0xff112233 %s\r\n",
             (unsigned long)pixel, pixel == 0xff112233u ? "PASS" : "FAIL");
    probeUpload();
    if (pixel != 0xff112233u)
        goto cleanup;

    /* These are object-space coordinates.  A hardware-vertex-processing
     * device must send their world/view/projection conversion to Triton's
     * D3D9 shader path; XYZRHW would skip that exact contract. */
    triangle[0] = (PROBE_VERTEX){-0.75f, -0.75f, 0.5f, 0xffff0000u };
    triangle[1] = (PROBE_VERTEX){ 0.00f,  0.75f, 0.5f, 0xffff0000u };
    triangle[2] = (PROBE_VERTEX){ 0.75f, -0.75f, 0.5f, 0xffff0000u };
    ZeroMemory(&identity, sizeof(identity));
    identity.m[0][0] = 1.0f;
    identity.m[1][1] = 1.0f;
    identity.m[2][2] = 1.0f;
    identity.m[3][3] = 1.0f;
    probeEnter("Configure HWP fixed pipeline");
    hr = IDirect3DDevice9Ex_Clear(device, 0, NULL, D3DCLEAR_TARGET,
                                  clearColor, 1.0f, 0);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9Ex_SetRenderState(device, D3DRS_LIGHTING, FALSE);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9Ex_SetRenderState(device, D3DRS_ZENABLE, FALSE);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9Ex_SetRenderState(device, D3DRS_CULLMODE,
                                               D3DCULL_NONE);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9Ex_SetTexture(device, 0, NULL);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9Ex_SetTextureStageState(device, 0,
                                                     D3DTSS_COLOROP,
                                                     D3DTOP_SELECTARG1);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9Ex_SetTextureStageState(device, 0,
                                                     D3DTSS_COLORARG1,
                                                     D3DTA_DIFFUSE);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9Ex_SetTextureStageState(device, 0,
                                                     D3DTSS_ALPHAOP,
                                                     D3DTOP_SELECTARG1);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9Ex_SetTextureStageState(device, 0,
                                                     D3DTSS_ALPHAARG1,
                                                     D3DTA_DIFFUSE);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9Ex_SetTransform(device, D3DTS_WORLD, &identity);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9Ex_SetTransform(device, D3DTS_VIEW, &identity);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9Ex_SetTransform(device, D3DTS_PROJECTION,
                                              &identity);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9Ex_SetFVF(device, D3DFVF_XYZ | D3DFVF_DIFFUSE);
    probeHr("Configure HWP fixed pipeline", hr);
    if (FAILED(hr))
        goto cleanup;
    probeEnter("DrawPrimitiveUP triangle");
    hr = IDirect3DDevice9Ex_BeginScene(device);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9Ex_DrawPrimitiveUP(device, D3DPT_TRIANGLELIST, 1,
                                                triangle, sizeof(triangle[0]));
        if (SUCCEEDED(hr))
            hr = IDirect3DDevice9Ex_EndScene(device);
    }
    probeHr("DrawPrimitiveUP triangle", hr);
    if (FAILED(hr))
        goto cleanup;
    probeEnter("Triangle readback");
    hr = probeReadPixel(device, target, readback, 64, 64, &pixel);
    probeHr("Triangle readback", hr);
    if (FAILED(hr))
        goto cleanup;
    probeLog("triangle center=0x%08lx %s\r\n", (unsigned long)pixel,
             probeNearRed(pixel) ? "PASS" : "FAIL");
    probeUpload();
    if (!probeNearRed(pixel))
        goto cleanup;
    probeEnter("Background readback");
    hr = probeReadPixel(device, target, readback, 4, 4, &pixel);
    probeHr("Background readback", hr);
    if (FAILED(hr))
        goto cleanup;
    probeLog("triangle outside=0x%08lx expected=0x%08lx %s\r\n",
             (unsigned long)pixel, (unsigned long)clearColor,
             pixel == clearColor ? "PASS" : "FAIL");
    probeUpload();
    if (pixel != clearColor)
        goto cleanup;

    /* COLOR1 is not decorative capability data: Vista MIL uses
     * D3DTA_SPECULAR in its Level-1 test. Draw it through the same HWP FVF
     * fallback and prove the result by CPU readback. */
    specularTriangle[0] = (PROBE_SPECULAR_VERTEX){
        -0.75f, -0.75f, 0.5f, 0xffffffffu, 0xff00ff00u };
    specularTriangle[1] = (PROBE_SPECULAR_VERTEX){
         0.00f,  0.75f, 0.5f, 0xffffffffu, 0xff00ff00u };
    specularTriangle[2] = (PROBE_SPECULAR_VERTEX){
         0.75f, -0.75f, 0.5f, 0xffffffffu, 0xff00ff00u };
    probeEnter("Configure HWP COLOR1 fixed pipeline");
    hr = IDirect3DDevice9Ex_Clear(device, 0, NULL, D3DCLEAR_TARGET,
                                  clearColor, 1.0f, 0);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9Ex_SetRenderState(device, D3DRS_ALPHABLENDENABLE,
                                                FALSE);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9Ex_SetTexture(device, 0, NULL);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9Ex_SetTextureStageState(device, 0,
                                                     D3DTSS_COLOROP,
                                                     D3DTOP_SELECTARG1);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9Ex_SetTextureStageState(device, 0,
                                                     D3DTSS_COLORARG1,
                                                     D3DTA_SPECULAR);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9Ex_SetTextureStageState(device, 0,
                                                     D3DTSS_ALPHAOP,
                                                     D3DTOP_SELECTARG1);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9Ex_SetTextureStageState(device, 0,
                                                     D3DTSS_ALPHAARG1,
                                                     D3DTA_DIFFUSE);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9Ex_SetTextureStageState(device, 1,
                                                     D3DTSS_COLOROP,
                                                     D3DTOP_DISABLE);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9Ex_SetFVF(device, D3DFVF_XYZ | D3DFVF_DIFFUSE |
                                                D3DFVF_SPECULAR);
    probeHr("Configure HWP COLOR1 fixed pipeline", hr);
    if (FAILED(hr))
        goto cleanup;
    probeEnter("DrawPrimitiveUP COLOR1 triangle");
    hr = IDirect3DDevice9Ex_BeginScene(device);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9Ex_DrawPrimitiveUP(device, D3DPT_TRIANGLELIST, 1,
                                                specularTriangle,
                                                sizeof(specularTriangle[0]));
        if (SUCCEEDED(hr))
            hr = IDirect3DDevice9Ex_EndScene(device);
    }
    probeHr("DrawPrimitiveUP COLOR1 triangle", hr);
    if (FAILED(hr))
        goto cleanup;
    probeEnter("COLOR1 triangle readback");
    hr = probeReadPixel(device, target, readback, 64, 64, &pixel);
    probeHr("COLOR1 triangle readback", hr);
    if (FAILED(hr))
        goto cleanup;
    probeLog("COLOR1 triangle center=0x%08lx expected=0xff00ff00 %s\\r\\n",
             (unsigned long)pixel,
             probeNearColor(pixel, 0xff00ff00u) ? "PASS" : "FAIL");
    probeUpload();
    if (!probeNearColor(pixel, 0xff00ff00u))
        goto cleanup;

    /* Vista MIL's normal path is programmable VS/PS 2.0, not the
     * fixed-function fallback exercised above.  Use a tiny pass-through
     * shader pair and the same public draw/readback boundary to prove token
     * validation, conversion, linkage, declaration, and shader binding. */
    probeBuildSm2PassthroughShaders(vertexShaderTokens, pixelShaderTokens);
    probeEnter("CreateVertexShader vs_2_0");
    hr = IDirect3DDevice9Ex_CreateVertexShader(device, vertexShaderTokens,
                                                &vertexShader);
    probeHr("CreateVertexShader vs_2_0", hr);
    if (FAILED(hr) || !vertexShader)
        goto cleanup;
    probeEnter("CreatePixelShader ps_2_0");
    hr = IDirect3DDevice9Ex_CreatePixelShader(device, pixelShaderTokens,
                                               &pixelShader);
    probeHr("CreatePixelShader ps_2_0", hr);
    if (FAILED(hr) || !pixelShader)
        goto cleanup;
    probeEnter("CreateVertexDeclaration SM2");
    hr = IDirect3DDevice9Ex_CreateVertexDeclaration(device, shaderDeclaration,
                                                     &shaderDeclarationObject);
    probeHr("CreateVertexDeclaration SM2", hr);
    if (FAILED(hr) || !shaderDeclarationObject)
        goto cleanup;
    probeEnter("Configure SM2 shader pipeline");
    hr = IDirect3DDevice9Ex_SetVertexDeclaration(device, shaderDeclarationObject);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9Ex_SetVertexShader(device, vertexShader);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9Ex_SetPixelShader(device, pixelShader);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9Ex_Clear(device, 0, NULL, D3DCLEAR_TARGET,
                                      0xff304050u, 1.0f, 0);
    probeHr("Configure SM2 shader pipeline", hr);
    if (FAILED(hr))
        goto cleanup;
    probeEnter("DrawPrimitiveUP SM2 triangle");
    hr = IDirect3DDevice9Ex_BeginScene(device);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9Ex_DrawPrimitiveUP(device, D3DPT_TRIANGLELIST, 1,
                                                triangle, sizeof(triangle[0]));
        if (SUCCEEDED(hr))
            hr = IDirect3DDevice9Ex_EndScene(device);
    }
    probeHr("DrawPrimitiveUP SM2 triangle", hr);
    if (FAILED(hr))
        goto cleanup;
    probeEnter("SM2 triangle readback");
    hr = probeReadPixel(device, target, readback, 64, 64, &pixel);
    probeHr("SM2 triangle readback", hr);
    if (FAILED(hr))
        goto cleanup;
    probeLog("SM2 triangle center=0x%08lx %s\r\n", (unsigned long)pixel,
             probeNearRed(pixel) ? "PASS" : "FAIL");
    probeUpload();
    if (!probeNearRed(pixel))
        goto cleanup;

    /* A compositor brush is a texture plus sampler state and shader
     * constants, not merely an interpolated vertex colour.  Exercise that
     * path with the same POSITION/COLOR/TEXCOORD0/TEXCOORD1 declaration shape
     * MIL uses for its 2D shader vertices. */
    textureTriangle[0] = (PROBE_TEXTURE_VERTEX){
        -0.75f, -0.75f, 0.5f, 0xffffffffu, 0.0f, 0.0f, 0.0f, 0.0f };
    textureTriangle[1] = (PROBE_TEXTURE_VERTEX){
         0.00f,  0.75f, 0.5f, 0xffffffffu, 0.5f, 1.0f, 0.5f, 1.0f };
    textureTriangle[2] = (PROBE_TEXTURE_VERTEX){
         0.75f, -0.75f, 0.5f, 0xffffffffu, 1.0f, 0.0f, 1.0f, 0.0f };
    probeEnter("Create Vista-managed lockable shader texture");
    hr = IDirect3DDevice9Ex_CreateTexture(device, 1, 1, 1, 0,
                                           D3DFMT_A8R8G8B8,
                                           TRITON9_VISTA_MANAGED_POOL,
                                           &sampleTexture, NULL);
    probeHr("Create Vista-managed lockable shader texture", hr);
    if (FAILED(hr) || !sampleTexture)
        goto cleanup;
    ZeroMemory(&textureLock, sizeof(textureLock));
    probeEnter("Lock/upload shader texture");
    hr = IDirect3DTexture9_LockRect(sampleTexture, 0, &textureLock, NULL, 0);
    if (SUCCEEDED(hr)) {
        if (!textureLock.pBits || textureLock.Pitch < (INT)sizeof(DWORD)) {
            hr = E_FAIL;
        } else {
            *(DWORD *)textureLock.pBits = textureColor;
        }
        {
            HRESULT unlockHr = IDirect3DTexture9_UnlockRect(sampleTexture, 0);
            if (SUCCEEDED(hr))
                hr = unlockHr;
        }
    }
    probeHr("Lock/upload shader texture", hr);
    if (FAILED(hr))
        goto cleanup;

    probeBuildSm2TexturedShaders(texturedVertexShaderTokens,
                                 texturedPixelShaderTokens);
    probeEnter("CreateVertexShader textured SM2");
    hr = IDirect3DDevice9Ex_CreateVertexShader(device, texturedVertexShaderTokens,
                                                &texturedVertexShader);
    probeHr("CreateVertexShader textured SM2", hr);
    if (FAILED(hr) || !texturedVertexShader)
        goto cleanup;
    probeEnter("CreatePixelShader textured SM2");
    hr = IDirect3DDevice9Ex_CreatePixelShader(device, texturedPixelShaderTokens,
                                               &texturedPixelShader);
    probeHr("CreatePixelShader textured SM2", hr);
    if (FAILED(hr) || !texturedPixelShader)
        goto cleanup;
    probeEnter("CreateVertexDeclaration textured SM2");
    hr = IDirect3DDevice9Ex_CreateVertexDeclaration(device, texturedDeclaration,
                                                     &texturedDeclarationObject);
    probeHr("CreateVertexDeclaration textured SM2", hr);
    if (FAILED(hr) || !texturedDeclarationObject)
        goto cleanup;
    probeEnter("Configure textured SM2 pipeline");
    hr = IDirect3DDevice9Ex_SetVertexDeclaration(device, texturedDeclarationObject);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9Ex_SetVertexShader(device, texturedVertexShader);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9Ex_SetPixelShader(device, texturedPixelShader);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9Ex_SetTexture(device, 0,
                                            (IDirect3DBaseTexture9 *)sampleTexture);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9Ex_SetSamplerState(device, 0, D3DSAMP_ADDRESSU,
                                                 D3DTADDRESS_CLAMP);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9Ex_SetSamplerState(device, 0, D3DSAMP_ADDRESSV,
                                                 D3DTADDRESS_CLAMP);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9Ex_SetSamplerState(device, 0, D3DSAMP_MINFILTER,
                                                 D3DTEXF_POINT);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9Ex_SetSamplerState(device, 0, D3DSAMP_MAGFILTER,
                                                 D3DTEXF_POINT);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9Ex_SetSamplerState(device, 0, D3DSAMP_MIPFILTER,
                                                 D3DTEXF_POINT);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9Ex_SetPixelShaderConstantF(device, 0,
                                                         textureModulate, 1);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9Ex_Clear(device, 0, NULL, D3DCLEAR_TARGET,
                                      0xff304050u, 1.0f, 0);
    probeHr("Configure textured SM2 pipeline", hr);
    if (FAILED(hr))
        goto cleanup;
    probeEnter("DrawPrimitiveUP textured SM2 triangle");
    hr = IDirect3DDevice9Ex_BeginScene(device);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9Ex_DrawPrimitiveUP(device, D3DPT_TRIANGLELIST, 1,
                                                textureTriangle,
                                                sizeof(textureTriangle[0]));
        if (SUCCEEDED(hr))
            hr = IDirect3DDevice9Ex_EndScene(device);
    }
    probeHr("DrawPrimitiveUP textured SM2 triangle", hr);
    if (FAILED(hr))
        goto cleanup;
    probeEnter("Textured SM2 triangle readback");
    hr = probeReadPixel(device, target, readback, 64, 64, &pixel);
    probeHr("Textured SM2 triangle readback", hr);
    if (FAILED(hr))
        goto cleanup;
    probeLog("textured SM2 center=0x%08lx expected=0x%08lx %s\r\n",
             (unsigned long)pixel, (unsigned long)texturedExpected,
             probeNearColor(pixel, texturedExpected) ? "PASS" : "FAIL");
    probeUpload();
    if (!probeNearColor(pixel, texturedExpected))
        goto cleanup;

    /* Shader-local DEF must not overwrite c0 set by the application above.
     * Alternate shaders without repeating SetPixelShaderConstantF. */
    {
        DWORD tokens[] = {
            D3DPS_VERSION(2, 0),
            probeShaderInstruction(D3DSIO_DEF, 5),
            probeShaderDestination(D3DSPR_CONST, 0),
            0x3e800000u, 0x3f000000u, 0x3f400000u, 0x3f800000u,
            probeShaderInstruction(D3DSIO_MOV, 2),
            probeShaderDestination(D3DSPR_COLOROUT, 0),
            probeShaderSource(D3DSPR_CONST, 0), D3DPS_END()
        };
        hr = IDirect3DDevice9Ex_CreatePixelShader(device, tokens, &inlinePixelShader);
        if (FAILED(hr)) goto cleanup;
        for (UINT pass = 0; pass < 6; ++pass) {
            const DWORD expected = pass == 4 ? 0xff204080 :
                (pass & 1) ? texturedExpected : 0xff4080bf;
            if (pass == 4) {
#define FIXED_CONSTANT_CHECK(call) do { hr = (call); if (FAILED(hr)) goto cleanup; } while (0)
                FIXED_CONSTANT_CHECK(IDirect3DDevice9Ex_SetRenderState(device,
                    D3DRS_TEXTUREFACTOR, expected));
                FIXED_CONSTANT_CHECK(IDirect3DDevice9Ex_SetTextureStageState(device,
                    0, D3DTSS_COLOROP, D3DTOP_SELECTARG1));
                FIXED_CONSTANT_CHECK(IDirect3DDevice9Ex_SetTextureStageState(device,
                    0, D3DTSS_COLORARG1, D3DTA_TFACTOR));
                FIXED_CONSTANT_CHECK(IDirect3DDevice9Ex_SetTextureStageState(device,
                    0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1));
                FIXED_CONSTANT_CHECK(IDirect3DDevice9Ex_SetTextureStageState(device,
                    0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR));
                FIXED_CONSTANT_CHECK(IDirect3DDevice9Ex_SetTextureStageState(device,
                    1, D3DTSS_COLOROP, D3DTOP_DISABLE));
#undef FIXED_CONSTANT_CHECK
            }
            hr = IDirect3DDevice9Ex_SetPixelShader(device,
                pass == 4 ? NULL : (pass & 1) ? texturedPixelShader : inlinePixelShader);
            if (FAILED(hr)) goto cleanup;
            hr = IDirect3DDevice9Ex_BeginScene(device);
            if (FAILED(hr)) goto cleanup;
            hr = IDirect3DDevice9Ex_DrawPrimitiveUP(device, D3DPT_TRIANGLELIST, 1,
                textureTriangle, sizeof(textureTriangle[0]));
            {
                HRESULT end = IDirect3DDevice9Ex_EndScene(device);
                if (FAILED(hr)) goto cleanup;
                if (FAILED(end)) { hr = end; goto cleanup; }
            }
            hr = probeReadPixel(device, target, readback, 64, 64, &pixel);
            if (FAILED(hr)) goto cleanup;
            probeLog("Shader-local constants pass=%u pixel=%08lx expected=%08lx %s\r\n",
                pass, (unsigned long)pixel, (unsigned long)expected,
                probeNearColor(pixel, expected) ? "PASS" : "FAIL");
            if (!probeNearColor(pixel, expected)) { hr = E_FAIL; goto cleanup; }
        }
    }

    /* Normal bitmap brushes do not use the solid-colour lockable path above.
     * MIL locks a SYSTEMMEM update surface and calls UpdateSurface into the
     * DEFAULT-pool texture it will sample.  Drive that exact public D3D9
     * sequence, including residency validation, then sample the destination
     * through the compositor-shaped shader pipeline already configured. */
    probeEnter("Create SYSTEMMEM bitmap update surface");
    hr = IDirect3DDevice9Ex_CreateOffscreenPlainSurface(
        device, 1, 1, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sysmemSurface,
        NULL);
    probeHr("Create SYSTEMMEM bitmap update surface", hr);
    if (FAILED(hr) || !sysmemSurface)
        goto cleanup;
    probeEnter("Lock/upload SYSTEMMEM bitmap update surface");
    ZeroMemory(&textureLock, sizeof(textureLock));
    hr = IDirect3DSurface9_LockRect(sysmemSurface, &textureLock, NULL, 0);
    if (SUCCEEDED(hr)) {
        if (!textureLock.pBits || textureLock.Pitch < (INT)sizeof(DWORD)) {
            hr = E_FAIL;
        } else {
            *(DWORD *)textureLock.pBits = bitmapTextureColor;
        }
        {
            HRESULT unlockHr = IDirect3DSurface9_UnlockRect(sysmemSurface);
            if (SUCCEEDED(hr))
                hr = unlockHr;
        }
    }
    probeHr("Lock/upload SYSTEMMEM bitmap update surface", hr);
    if (FAILED(hr))
        goto cleanup;
    probeEnter("Create DEFAULT bitmap sample texture");
    hr = IDirect3DDevice9Ex_CreateTexture(device, 1, 1, 1, 0,
                                           D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                           &defaultTexture, NULL);
    probeHr("Create DEFAULT bitmap sample texture", hr);
    if (FAILED(hr) || !defaultTexture)
        goto cleanup;
    probeEnter("Get DEFAULT bitmap sample surface");
    hr = IDirect3DTexture9_GetSurfaceLevel(defaultTexture, 0, &defaultSurface);
    probeHr("Get DEFAULT bitmap sample surface", hr);
    if (FAILED(hr) || !defaultSurface)
        goto cleanup;
    probeEnter("UpdateSurface bitmap texture");
    hr = IDirect3DDevice9Ex_UpdateSurface(device, sysmemSurface, NULL,
                                           defaultSurface, NULL);
    probeHr("UpdateSurface bitmap texture", hr);
    if (FAILED(hr))
        goto cleanup;
    {
        IDirect3DResource9 *residencyResource =
            (IDirect3DResource9 *)defaultTexture;

        probeEnter("CheckResourceResidency bitmap texture");
        hr = IDirect3DDevice9Ex_CheckResourceResidency(device,
                                                        &residencyResource, 1);
        probeHr("CheckResourceResidency bitmap texture", hr);
        if (FAILED(hr))
            goto cleanup;
    }
    probeEnter("Configure bitmap UpdateSurface pipeline");
    hr = IDirect3DDevice9Ex_SetTexture(device, 0,
                                        (IDirect3DBaseTexture9 *)defaultTexture);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9Ex_Clear(device, 0, NULL, D3DCLEAR_TARGET,
                                      0xff304050u, 1.0f, 0);
    probeHr("Configure bitmap UpdateSurface pipeline", hr);
    if (FAILED(hr))
        goto cleanup;
    probeEnter("DrawPrimitiveUP bitmap UpdateSurface triangle");
    hr = IDirect3DDevice9Ex_BeginScene(device);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9Ex_DrawPrimitiveUP(device, D3DPT_TRIANGLELIST, 1,
                                                textureTriangle,
                                                sizeof(textureTriangle[0]));
        if (SUCCEEDED(hr))
            hr = IDirect3DDevice9Ex_EndScene(device);
    }
    probeHr("DrawPrimitiveUP bitmap UpdateSurface triangle", hr);
    if (FAILED(hr))
        goto cleanup;
    probeEnter("Bitmap UpdateSurface triangle readback");
    hr = probeReadPixel(device, target, readback, 64, 64, &pixel);
    probeHr("Bitmap UpdateSurface triangle readback", hr);
    if (FAILED(hr))
        goto cleanup;
    probeLog("bitmap UpdateSurface center=0x%08lx expected=0x%08lx %s\r\n",
             (unsigned long)pixel, (unsigned long)bitmapExpected,
             probeNearColor(pixel, bitmapExpected) ? "PASS" : "FAIL");
    probeUpload();
    if (!probeNearColor(pixel, bitmapExpected))
        goto cleanup;

    /* The subsequent swapchain test is intentionally fixed-function-neutral;
     * clear does not depend on the shader bindings left above. */

    probeEnter("StretchRect filtered pixels");
    hr = probeSm2DistinctCoordinates(device, target, readback);
    probeHr("SM2 eight distinct coordinates", hr);
    if (FAILED(hr)) goto cleanup;
    hr = probeEightTextureStages(device, target, readback);
    probeHr("Eight texture stages", hr);
    if (FAILED(hr)) goto cleanup;
    hr = probeStretchRectPixels(device);
    probeHr("StretchRect filtered pixels", hr);
    if (FAILED(hr)) goto cleanup;
    /* The helper never binds a public render target. A clear must still hit
     * the caller's original target after the deferred GPU blits. */
    hr = IDirect3DDevice9Ex_Clear(device, 0, NULL, D3DCLEAR_TARGET, 0xff365a7e, 1, 0);
    if (SUCCEEDED(hr)) hr = probeReadPixel(device, target, readback, 0, 0, &pixel);
    if (SUCCEEDED(hr) && !probeNearColor(pixel, 0xff365a7e)) hr = E_FAIL;
    probeHr("StretchRect preserves target", hr);
    if (FAILED(hr)) goto cleanup;
    hr = IDirect3DDevice9Ex_BeginScene(device);
    if (SUCCEEDED(hr)) {
        hr = IDirect3DDevice9Ex_DrawPrimitiveUP(device, D3DPT_TRIANGLELIST, 1,
            textureTriangle, sizeof(textureTriangle[0]));
        if (SUCCEEDED(hr)) hr = IDirect3DDevice9Ex_EndScene(device);
    }
    if (SUCCEEDED(hr)) hr = probeReadPixel(device, target, readback, 64, 64, &pixel);
    if (SUCCEEDED(hr) && !probeNearColor(pixel, bitmapExpected)) hr = E_FAIL;
    probeHr("StretchRect preserves shader pipeline", hr);
    if (FAILED(hr)) goto cleanup;

    probeEnter("Prepare backbuffer");
    hr = IDirect3DDevice9Ex_GetBackBuffer(device, 0, 0,
                                          D3DBACKBUFFER_TYPE_MONO,
                                          &backbuffer);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9Ex_SetRenderTarget(device, 0, backbuffer);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9Ex_Clear(device, 0, NULL, D3DCLEAR_TARGET,
                                      0xff2060c0u, 1.0f, 0);
    probeHr("Prepare backbuffer", hr);
    if (FAILED(hr))
        goto cleanup;
    probeEnter("PresentEx");
    hr = IDirect3DDevice9Ex_PresentEx(device, NULL, NULL, window, NULL, 0);
    probeHr("PresentEx", hr);
    if (FAILED(hr))
        goto cleanup;

    /* MIL creates an additional swapchain for each hardware window target
     * and presents through IDirect3DSwapChain9::Present, rather than through
     * the device-level PresentEx call above.  Drive that same allocation and
     * KMD present route here; this makes a probe pass meaningful for DWM. */
    probeEnter("CreateAdditionalSwapChain");
    hr = IDirect3DDevice9Ex_CreateAdditionalSwapChain(device, &present,
                                                       &additionalSwapChain);
    probeHr("CreateAdditionalSwapChain", hr);
    if (FAILED(hr) || !additionalSwapChain)
        goto cleanup;
    probeEnter("AdditionalSwapChain GetBackBuffer");
    hr = IDirect3DSwapChain9_GetBackBuffer(additionalSwapChain, 0,
                                            D3DBACKBUFFER_TYPE_MONO,
                                            &additionalBackbuffer);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9Ex_SetRenderTarget(device, 0,
                                                 additionalBackbuffer);
    if (SUCCEEDED(hr))
        hr = IDirect3DDevice9Ex_Clear(device, 0, NULL, D3DCLEAR_TARGET,
                                      0xff40a050u, 1.0f, 0);
    probeHr("AdditionalSwapChain GetBackBuffer", hr);
    if (FAILED(hr))
        goto cleanup;
    probeEnter("AdditionalSwapChain Present");
    hr = IDirect3DSwapChain9_Present(additionalSwapChain, NULL, NULL,
                                      window, NULL, 0);
    probeHr("AdditionalSwapChain Present", hr);
    if (FAILED(hr))
        goto cleanup;

    if (!pureProfilePassed) {
        probeLog("TRITON9-PROBE strict HWP+PURE profile FAIL\r\n");
        goto cleanup;
    }
    result = 0;
    probeLog("TRITON9-PROBE PASS\r\n");
    if (hold) {
        probeLog("holding presented window for 15 seconds\r\n");
        Sleep(15000);
    }

cleanup:
    if (result) {
        probeLog("TRITON9-PROBE FAIL\r\n");
        /* Publish the terminal result before releasing the checked Vista
         * runtime.  A rejected HAL can fault in its diagnostic teardown;
         * that secondary fault must not hide the capability result. */
        probeUpload();
    }
    if (additionalBackbuffer)
        IDirect3DSurface9_Release(additionalBackbuffer);
    if (additionalSwapChain)
        IDirect3DSwapChain9_Release(additionalSwapChain);
    if (defaultSurface)
        IDirect3DSurface9_Release(defaultSurface);
    if (sysmemSurface)
        IDirect3DSurface9_Release(sysmemSurface);
    if (defaultTexture)
        IDirect3DTexture9_Release(defaultTexture);
    if (sampleTexture)
        IDirect3DTexture9_Release(sampleTexture);
    if (texturedDeclarationObject)
        IDirect3DVertexDeclaration9_Release(texturedDeclarationObject);
    if (texturedPixelShader)
        IDirect3DPixelShader9_Release(texturedPixelShader);
    if (texturedVertexShader)
        IDirect3DVertexShader9_Release(texturedVertexShader);
    if (shaderDeclarationObject)
        IDirect3DVertexDeclaration9_Release(shaderDeclarationObject);
    if (pixelShader)
        IDirect3DPixelShader9_Release(pixelShader);
    if (inlinePixelShader)
        IDirect3DPixelShader9_Release(inlinePixelShader);
    if (vertexShader)
        IDirect3DVertexShader9_Release(vertexShader);
    if (backbuffer)
        IDirect3DSurface9_Release(backbuffer);
    if (readback)
        IDirect3DSurface9_Release(readback);
    if (depthStencil)
        IDirect3DSurface9_Release(depthStencil);
    if (target)
        IDirect3DSurface9_Release(target);
    if (pureDevice)
        IDirect3DDevice9Ex_Release(pureDevice);
    if (device)
        IDirect3DDevice9Ex_Release(device);
    if (d3d)
        IDirect3D9Ex_Release(d3d);
    if (window)
        DestroyWindow(window);
    /* Keep d3d9.dll loaded until process teardown.  Vista's checked runtime
     * can still have adapter cleanup state on its process heap after the last
     * public COM reference is released; explicitly unloading the module here
     * obscures the original probe result with a secondary heap assertion. */
    (void)d3d9Module;
    if (g_log != INVALID_HANDLE_VALUE) {
        CloseHandle(g_log);
        g_log = INVALID_HANDLE_VALUE;
    }
    probeAppendFileTail("C:\\triton9-ddi.log",
                        "TRITON9-DDI-LOG", PROBE_DDI_LOG_TAIL_SIZE);
    probeAppendFileTail("C:\\Windows\\Temp\\triton9-ddi.log",
                        "TRITON9-DDI-TEMP-LOG", PROBE_DDI_LOG_TAIL_SIZE);
    probeAppendFileTail("C:\\Windows\\Temp\\triton9-d3d9-proof.log",
                        "TRITON9-PROOF-EVENTS", 16384);
    probeUpload();
    probeCloseTelemetry();
    return result;
}

static void
probeReportServiceStatus(DWORD state, DWORD win32ExitCode,
                         DWORD serviceExitCode, DWORD waitHint,
                         DWORD checkpoint)
{
    SERVICE_STATUS status;

    if (!g_serviceHandle)
        return;
    ZeroMemory(&status, sizeof(status));
    status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    status.dwCurrentState = state;
    status.dwWin32ExitCode = win32ExitCode;
    status.dwServiceSpecificExitCode = serviceExitCode;
    status.dwWaitHint = waitHint;
    status.dwCheckPoint = checkpoint;
    SetServiceStatus(g_serviceHandle, &status);
}

static VOID WINAPI
probeServiceControl(DWORD control)
{
    (void)control;
}

static VOID WINAPI
probeServiceMain(DWORD argc, LPSTR *argv)
{
    DWORD attempt;
    DWORD error = NO_ERROR;
    DWORD exitCode = 1;
    DWORD waitResult;
    DWORD aeroDeadline;
    DWORD publicDesktopProcessId = 0;
    DWORD aeroDesktopProcessId = 0;
    PROCESS_INFORMATION child;
    PROCESS_INFORMATION aeroChild;
    char secureLogPath[MAX_PATH];
    char aeroLogPath[MAX_PATH];
    BOOL secureLogPublished = FALSE;
    BOOL aeroScenePublished = FALSE;
    DWORD secureLogOffset = 0;
    DWORD secureDeadline;

    (void)argc;
    (void)argv;
    g_serviceHandle = RegisterServiceCtrlHandlerA(g_probeServiceName,
                                                   probeServiceControl);
    if (!g_serviceHandle)
        return;
    if (!probeValidResultPath(g_probeResultPath) ||
        !probeValidHex(g_probeResultNonce, PROBE_RESULT_NONCE_LENGTH) ||
        !probeBuildChildLogPath(secureLogPath, sizeof(secureLogPath),
                                "probe") ||
        !probeBuildChildLogPath(aeroLogPath, sizeof(aeroLogPath), "aero")) {
        g_serviceResult = 1;
        probeReportServiceStatus(SERVICE_STOPPED,
                                 ERROR_SERVICE_SPECIFIC_ERROR,
                                 ERROR_INVALID_DATA, 0, 0);
        return;
    }
    g_log = CreateFileA(g_probeResultPath,
                        GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_log == INVALID_HANDLE_VALUE) {
        g_serviceResult = 1;
        probeReportServiceStatus(SERVICE_STOPPED,
                                 ERROR_SERVICE_SPECIFIC_ERROR,
                                 GetLastError(), 0, 0);
        return;
    }
    /* This small text-only file is owned by the current one-shot service.
     * Clear it before DWM starts so the UMD's first depth-clear record cannot
     * be confused with a previous immutable deployment. */
    (void)DeleteFileA("C:\\Windows\\Temp\\triton9-d3d9-proof.log");
    (void)DeleteFileA(secureLogPath);
    (void)DeleteFileA(aeroLogPath);
    probeLog("TRITON9-RUN nonce=%s\r\n", g_probeResultNonce);
    probeLog("TRITON9-SERVICE begin arch=%u name=%s\r\n",
             (unsigned)(sizeof(void *) * 8), g_probeServiceName);
    probeLog("delete one-shot service        %s\r\n",
             probeDeleteOwnService() ? "PASS" : "FAIL");
    probeLog("enable SeDebugPrivilege        %s\r\n",
             probeEnablePrivilege(SE_DEBUG_NAME) ? "PASS" : "FAIL");
    probeLog("enable SeAssignPrimaryToken    %s\r\n",
             probeEnablePrivilege(SE_ASSIGNPRIMARYTOKEN_NAME) ? "PASS" : "FAIL");
    probeLog("enable SeIncreaseQuota         %s\r\n",
             probeEnablePrivilege(SE_INCREASE_QUOTA_NAME) ? "PASS" : "FAIL");
    probePreflightUmdLoader();

    /* The public D3D9 contract is the first gate.  UxSms can block while DWM
     * starts a rejected adapter, so compositor startup must not run before
     * this independent probe proves clear/readback, triangle/readback, and
     * PresentEx.  This also keeps DWM traffic from obscuring the first failing
     * UMD callback in the bounded proof log. */
    ZeroMemory(&child, sizeof(child));
    for (attempt = 1; attempt <= 60; ++attempt) {
        probeReportServiceStatus(SERVICE_START_PENDING, NO_ERROR, 0,
                                 70000, attempt);
        if (probeLaunchPublicDesktop(&child, &publicDesktopProcessId))
            break;
        error = GetLastError();
        Sleep(1000);
    }
    if (!child.hProcess) {
        probeLog("secure probe launch           error=%lu FAIL\r\n",
                 (unsigned long)error);
        probeUploadTo("/__triton9_service__");
        goto stopped;
    }
    probeLog("secure probe launch           pid=%lu desktop-pid=%lu PASS\r\n",
             (unsigned long)child.dwProcessId,
             (unsigned long)publicDesktopProcessId);
    /* This heartbeat distinguishes an SCM/token/desktop-launch failure from
     * a child blocked later inside the public D3D9 runtime. */
    probeUploadTo("/__triton9_service__");
    CloseHandle(child.hThread);
    child.hThread = NULL;
    probeReportServiceStatus(SERVICE_RUNNING, NO_ERROR, 0, 0, 0);
    secureDeadline = GetTickCount() + PROBE_PUBLIC_SERVICE_WAIT_MS;
    for (;;) {
        waitResult = WaitForSingleObject(child.hProcess, 1000);
        if (probeRelayFileGrowth(secureLogPath, "SECURE-PROBE-LOG-LIVE",
                                 32768, &secureLogOffset))
            secureLogPublished = TRUE;
        if (waitResult != WAIT_TIMEOUT ||
            (LONG)(GetTickCount() - secureDeadline) >= 0)
            break;
    }
    if (waitResult == WAIT_OBJECT_0 &&
        GetExitCodeProcess(child.hProcess, &exitCode)) {
        probeLog("secure probe exit             code=%lu\r\n",
                 (unsigned long)exitCode);
        g_serviceResult = (int)exitCode;
    } else {
        error = waitResult == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
        if (waitResult == WAIT_TIMEOUT)
            TerminateProcess(child.hProcess, ERROR_TIMEOUT);
        probeLog("secure probe wait             error=%lu FAIL\r\n",
                 (unsigned long)error);
        g_serviceResult = 1;
    }
    if (secureLogPublished)
        probeLog("--- SECURE-PROBE-LOG-LIVE end size=%lu ---\r\n",
                 (unsigned long)secureLogOffset);
    CloseHandle(child.hProcess);
    child.hProcess = NULL;

    if (g_serviceResult) {
        probeLog("AERO probe skipped            D3D9 gate failed\r\n");
        goto collect;
    }
    if (!probeFileContainsText(secureLogPath, "TRITON9-PROBE PASS", 32768)) {
        probeLog("secure probe result marker    missing FAIL\r\n");
        g_serviceResult = 1;
        goto collect;
    }
    (void)probeEnsureUxSmsRunning();
    ZeroMemory(&aeroChild, sizeof(aeroChild));
    for (attempt = 1; attempt <= 60; ++attempt) {
        probeReportServiceStatus(SERVICE_START_PENDING, NO_ERROR, 0,
                                 70000, attempt);
        if (probeLaunchAeroDesktop(&aeroChild, &aeroDesktopProcessId))
            break;
        error = GetLastError();
        Sleep(1000);
    }
    if (!aeroChild.hProcess) {
        probeLog("AERO probe launch             error=%lu FAIL\r\n",
                 (unsigned long)error);
        g_serviceResult = 1;
    } else {
        probeLog("AERO probe launch             pid=%lu desktop-pid=%lu PASS\r\n",
                 (unsigned long)aeroChild.dwProcessId,
                 (unsigned long)aeroDesktopProcessId);
        CloseHandle(aeroChild.hThread);
        aeroChild.hThread = NULL;
    }
    if (aeroChild.hProcess) {
        aeroDeadline = GetTickCount() + PROBE_AERO_SERVICE_WAIT_MS;
        for (;;) {
            waitResult = WaitForSingleObject(aeroChild.hProcess, 1000);
            if (!aeroScenePublished &&
                probeFileContainsText(
                    aeroLogPath,
                    "TRITON9-AERO API-PATH proof-pattern", 32768)) {
                probeAppendFileTail(aeroLogPath, "AERO-PROBE-LIVE", 32768);
                probeUploadTo("/__triton9_service__");
                aeroScenePublished = TRUE;
            }
            if (waitResult != WAIT_TIMEOUT ||
                (LONG)(GetTickCount() - aeroDeadline) >= 0)
                break;
        }
        if (waitResult == WAIT_OBJECT_0 &&
            GetExitCodeProcess(aeroChild.hProcess, &exitCode)) {
            probeLog("AERO probe exit             code=%lu\r\n",
                     (unsigned long)exitCode);
            if (exitCode)
                g_serviceResult = 1;
        } else {
            error = waitResult == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
            if (waitResult == WAIT_TIMEOUT)
                TerminateProcess(aeroChild.hProcess, ERROR_TIMEOUT);
            probeLog("AERO probe wait             error=%lu FAIL\r\n",
                     (unsigned long)error);
            g_serviceResult = 1;
        }
        CloseHandle(aeroChild.hProcess);
        aeroChild.hProcess = NULL;
    }

collect:
    probeAppendFileTail(aeroLogPath, "AERO-PROBE-LOG", 32768);
    if (!secureLogPublished)
        probeAppendFileTail(secureLogPath, "SECURE-PROBE-LOG", 32768);
    probeAppendFileTail("C:\\triton9-ddi.log",
                        "TRITON9-DDI-LOG", PROBE_DDI_LOG_TAIL_SIZE);
    probeAppendFileTail("C:\\Windows\\Temp\\triton9-ddi.log",
                        "TRITON9-DDI-TEMP-LOG", PROBE_DDI_LOG_TAIL_SIZE);
    probeAppendFileTail("C:\\Windows\\Temp\\triton9-d3d9-proof.log",
                        "TRITON9-PROOF-EVENTS", 16384);
    probeUploadTo("/__triton9_service__");

stopped:
    if (g_log != INVALID_HANDLE_VALUE) {
        CloseHandle(g_log);
        g_log = INVALID_HANDLE_VALUE;
    }
    probeReportServiceStatus(
        SERVICE_STOPPED,
        g_serviceResult ? ERROR_SERVICE_SPECIFIC_ERROR : NO_ERROR,
        g_serviceResult ? (DWORD)g_serviceResult : 0, 0, 0);
    probeCloseTelemetry();
}

int
main(int argc, char **argv)
{
    SERVICE_TABLE_ENTRYA serviceTable[] = {
        { g_probeServiceName, probeServiceMain },
        { NULL, NULL }
    };
    DWORD error;
    int index;

    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX |
                 SEM_NOOPENFILEERRORBOX);
    SetUnhandledExceptionFilter(probeUnhandledException);

    for (index = 1; index < argc; ++index) {
        const char *value;
        size_t length;
        if (strcmp(argv[index], "--service-name") != 0 &&
            strcmp(argv[index], "--result-log") != 0 &&
            strcmp(argv[index], "--result-nonce") != 0)
            continue;
        if (index + 1 >= argc) {
            fprintf(stderr, "missing option value\r\n");
            return 1;
        }
        value = argv[++index];
        length = strlen(value);
        if (strcmp(argv[index - 1], "--service-name") == 0) {
            if (length == 0 || length >= sizeof(g_probeServiceName)) {
                fprintf(stderr, "invalid service name\r\n");
                return 1;
            }
            memcpy(g_probeServiceName, value, length + 1);
        } else if (strcmp(argv[index - 1], "--result-log") == 0) {
            if (!probeValidResultPath(value)) {
                fprintf(stderr, "invalid result path\r\n");
                return 1;
            }
            memcpy(g_probeResultPath, value, length + 1);
        } else {
            if (!probeValidHex(value, PROBE_RESULT_NONCE_LENGTH)) {
                fprintf(stderr, "invalid result nonce\r\n");
                return 1;
            }
            memcpy(g_probeResultNonce, value, length + 1);
        }
    }

    /* The one-shot service deliberately launches this explicit mode with a
     * duplicated Winlogon token.  Such a process is not an SCM child, and on
     * checked Vista StartServiceCtrlDispatcher can fail with ACCESS_DENIED
     * rather than ERROR_FAILED_SERVICE_CONTROLLER_CONNECT.  Dispatch the
     * command-line contract first so the secure-desktop probe reaches D3D9
     * independently of that version-specific SCM error. */
    if (argc > 1 && strcmp(argv[1], "--secure-probe") == 0) {
        g_childMode = TRUE;
        return probeRun(argc, argv);
    }
    if (argc > 1 && (strcmp(argv[1], "--aero-probe") == 0 ||
                     strcmp(argv[1], "--aero-blur-probe") == 0 ||
                     strcmp(argv[1], "--aero-frame-probe") == 0)) {
        g_childMode = TRUE;
        return probeAeroRun(strcmp(argv[1], "--aero-frame-probe") != 0);
    }

    if (StartServiceCtrlDispatcherA(serviceTable))
        return g_serviceResult;
    error = GetLastError();
    if (error != ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
        fprintf(stderr, "StartServiceCtrlDispatcher error=%lu\r\n",
                (unsigned long)error);
        return 1;
    }
    if (!probeValidHex(g_probeResultNonce, PROBE_RESULT_NONCE_LENGTH) &&
        !probeGenerateResultNonce()) {
        fprintf(stderr, "cannot create direct-probe nonce error=%lu\r\n",
                (unsigned long)GetLastError());
        return 1;
    }
    g_childMode = TRUE;
    return probeRun(argc, argv);
}
