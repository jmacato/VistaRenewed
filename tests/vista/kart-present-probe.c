/* SPDX-License-Identifier: MIT */
/*
 * SuperTuxKart-only measurement proxy. All D3D calls execute in the system
 * D3D9 runtime. Only existing public method slots are patched atomically;
 * object identity, vtable identity and all private runtime entries survive.
 * The proxy and its small per-table records remain pinned until process exit.
 *
 * Place beside the official game executable as d3d9.dll. Set the unique
 * TRITON_KART_TIMINGS output path before launch. Remove this DLL for normal
 * gameplay and the uninstrumented performance control.
 */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

struct slot_hook {
    void **slot;
    void *original;
    void *replacement;
    struct slot_hook *next;
};

static CRITICAL_SECTION lock;
static INIT_ONCE runtime_once = INIT_ONCE_STATIC_INIT;
static struct slot_hook *slots;
static unsigned frame;
static unsigned active_presents;
static HMODULE system_d3d9;
static BOOL instrumentation_ready;
static DWORD present_depth = TLS_OUT_OF_INDEXES;
static HANDLE output = INVALID_HANDLE_VALUE;
static BOOL output_failed;
static BOOL header_written;
static char pending[65536];
static DWORD pending_size;

IDirect3D9 *WINAPI Direct3DCreate9(UINT sdk_version);

static void flush_locked(void)
{
    DWORD offset = 0;

    if (output == INVALID_HANDLE_VALUE || output_failed) {
        return;
    }
    while (offset < pending_size) {
        DWORD written = 0;
        if (!WriteFile(output, pending + offset, pending_size - offset,
                       &written, NULL) || !written) {
            output_failed = TRUE;
            return;
        }
        offset += written;
    }
    pending_size = 0;
}

static void record(const char *format, ...)
{
    char text[2048];
    int size;
    va_list args;

    va_start(args, format);
    size = vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    EnterCriticalSection(&lock);
    if (size < 0 || (size_t)size >= sizeof(text)) {
        output_failed = TRUE;
    } else if (output != INVALID_HANDLE_VALUE && !output_failed) {
        if (pending_size + (DWORD)size > sizeof(pending)) {
            flush_locked();
        }
        if (!output_failed) {
            memcpy(pending + pending_size, text, (size_t)size);
            pending_size += (DWORD)size;
        }
    }
    LeaveCriticalSection(&lock);
}

/* Installation is serialized, but no runtime call is made under this lock.
 * Publish immutable forwarding metadata before the pointer-sized atomic swap:
 * an already-running caller can safely enter a newly installed hook. No COM
 * references or per-object records are held, and Release is never intercepted.
 */
static BOOL install_slot(void **slot, void *replacement)
{
    struct slot_hook *hook;
    DWORD protection, ignored;
    BOOL success = TRUE;

    EnterCriticalSection(&lock);
    for (hook = slots; hook; hook = hook->next) {
        if (hook->slot == slot) {
            success = hook->replacement == replacement && *slot == replacement;
            goto done;
        }
    }
    hook = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*hook));
    if (!hook) {
        success = FALSE;
        goto done;
    }
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_EXECUTE_READWRITE,
                        &protection)) {
        HeapFree(GetProcessHeap(), 0, hook);
        success = FALSE;
        goto done;
    }
    hook->slot = slot;
    hook->original = *slot;
    hook->replacement = replacement;
    hook->next = slots;
    slots = hook;
    InterlockedExchangePointer((PVOID volatile *)slot, replacement);
    if (!VirtualProtect(slot, sizeof(*slot), protection, &ignored)) {
        success = FALSE;
    }
done:
    LeaveCriticalSection(&lock);
    if (!success) {
        record("# instrumentation_error=method interception failed\n");
    }
    return success;
}

static void *original_slot(void **slot)
{
    struct slot_hook *hook;
    void *original = NULL;

    EnterCriticalSection(&lock);
    for (hook = slots; hook; hook = hook->next) {
        if (hook->slot == slot) {
            original = hook->original;
            break;
        }
    }
    LeaveCriticalSection(&lock);
    return original;
}

/* Device::Present may call SwapChain::Present internally. Count only the
 * outermost presentation on each thread. Concurrent outer presents invalidate
 * the timing capture without serializing or changing the runtime calls.
 */
static uintptr_t begin_present(LARGE_INTEGER *begin)
{
    uintptr_t depth = (uintptr_t)TlsGetValue(present_depth);

    if (!TlsSetValue(present_depth, (void *)(depth + 1))) {
        record("# instrumentation_error=present TLS failed\n");
    }
    if (!depth) {
        EnterCriticalSection(&lock);
        if (active_presents++) {
            record("# instrumentation_error=concurrent presents\n");
        }
        LeaveCriticalSection(&lock);
        QueryPerformanceCounter(begin);
    }
    return depth;
}

static void end_present(uintptr_t depth, LARGE_INTEGER begin, HRESULT result)
{
    LARGE_INTEGER end;

    if (!depth) {
        QueryPerformanceCounter(&end);
        EnterCriticalSection(&lock);
        record("%u,%llu,%llu,%08x\n", frame++,
               (unsigned long long)begin.QuadPart,
               (unsigned long long)end.QuadPart, (unsigned)result);
        --active_presents;
        LeaveCriticalSection(&lock);
    }
    if (!TlsSetValue(present_depth, (void *)depth)) {
        record("# instrumentation_error=present TLS restore failed\n");
    }
}

static HRESULT WINAPI present_swapchain(IDirect3DSwapChain9 *object,
                                        const RECT *source,
                                        const RECT *destination, HWND override,
                                        const RGNDATA *dirty, DWORD flags)
{
    typedef HRESULT (WINAPI *function)(IDirect3DSwapChain9 *, const RECT *,
                                      const RECT *, HWND, const RGNDATA *, DWORD);
    DWORD entry_error = GetLastError();
    function original = (function)(uintptr_t)original_slot(
        (void **)&object->lpVtbl->Present);
    LARGE_INTEGER begin = {0};
    uintptr_t depth = begin_present(&begin);
    HRESULT result;
    DWORD result_error;

    SetLastError(entry_error);
    result = original(object, source, destination, override, dirty, flags);
    result_error = GetLastError();

    end_present(depth, begin, result);
    SetLastError(result_error);
    return result;
}

static HRESULT WINAPI present_device(IDirect3DDevice9 *object,
                                     const RECT *source,
                                     const RECT *destination, HWND override,
                                     const RGNDATA *dirty)
{
    typedef HRESULT (WINAPI *function)(IDirect3DDevice9 *, const RECT *,
                                      const RECT *, HWND, const RGNDATA *);
    DWORD entry_error = GetLastError();
    function original = (function)(uintptr_t)original_slot(
        (void **)&object->lpVtbl->Present);
    LARGE_INTEGER begin = {0};
    uintptr_t depth = begin_present(&begin);
    HRESULT result;
    DWORD result_error;

    SetLastError(entry_error);
    result = original(object, source, destination, override, dirty);
    result_error = GetLastError();

    end_present(depth, begin, result);
    SetLastError(result_error);
    return result;
}

static HRESULT WINAPI get_swapchain(IDirect3DDevice9 *object, UINT index,
                                    IDirect3DSwapChain9 **result)
{
    typedef HRESULT (WINAPI *function)(IDirect3DDevice9 *, UINT,
                                      IDirect3DSwapChain9 **);
    DWORD entry_error = GetLastError();
    function original = (function)(uintptr_t)original_slot(
        (void **)&object->lpVtbl->GetSwapChain);
    HRESULT status;
    DWORD result_error;

    SetLastError(entry_error);
    status = original(object, index, result);
    result_error = GetLastError();

    if (SUCCEEDED(status) && result && *result) {
        install_slot((void **)&(*result)->lpVtbl->Present,
                     (void *)(uintptr_t)present_swapchain);
    }
    SetLastError(result_error);
    return status;
}

static HRESULT WINAPI reset(IDirect3DDevice9 *object,
                            D3DPRESENT_PARAMETERS *parameters)
{
    typedef HRESULT (WINAPI *function)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);
    DWORD entry_error = GetLastError();
    function original = (function)(uintptr_t)original_slot(
        (void **)&object->lpVtbl->Reset);
    HRESULT result;
    DWORD result_error;

    SetLastError(entry_error);
    result = original(object, parameters);
    result_error = GetLastError();

    /* Device loss/resize changes the workload, but not the Reset behavior. */
    record("# reset=%08x\n", (unsigned)result);
    SetLastError(result_error);
    return result;
}

static void describe_device(IDirect3D9 *adapter, IDirect3DDevice9 *device,
                            UINT ordinal, D3DDEVTYPE type, DWORD flags)
{
    LARGE_INTEGER frequency;
    OSVERSIONINFOW os;
    D3DADAPTER_IDENTIFIER9 identifier;
    IDirect3DSurface9 *backbuffer = NULL;
    D3DSURFACE_DESC desc;
    char umd_path[MAX_PATH] = "";
    char system_directory[MAX_PATH] = "";
    char expected_path[MAX_PATH];
    BOOL wow64 = FALSE;
    BOOL architecture_known = IsWow64Process(GetCurrentProcess(), &wow64);
#ifdef _WIN64
    const char *process_arch = "x64";
    architecture_known = architecture_known && !wow64;
#else
    const char *process_arch = "x86";
#endif
    const char *umd_name = wow64 ? "neptune_d3d9_wow.dll" : "neptune_d3d9.dll";
    HMODULE umd = GetModuleHandleW(wow64 ? L"neptune_d3d9_wow.dll" :
                                         L"neptune_d3d9.dll");
    UINT directory_length = wow64 ?
        GetSystemWow64DirectoryA(system_directory, sizeof(system_directory)) :
        GetSystemDirectoryA(system_directory, sizeof(system_directory));
    DWORD module_length = umd ?
        GetModuleFileNameA(umd, umd_path, sizeof(umd_path)) : 0;
    int expected_length;

    system_directory[sizeof(system_directory) - 1] = 0;
    umd_path[sizeof(umd_path) - 1] = 0;
    expected_length = snprintf(expected_path, sizeof(expected_path), "%s\\%s",
                               system_directory, umd_name);
    if (!architecture_known || !directory_length ||
        directory_length >= sizeof(system_directory) || !module_length ||
        module_length >= sizeof(umd_path) || expected_length < 0 ||
        (size_t)expected_length >= sizeof(expected_path) ||
        lstrcmpiA(expected_path, umd_path)) {
        record("# instrumentation_error=loaded UMD identity mismatch\n");
    }

    ZeroMemory(&os, sizeof(os));
    os.dwOSVersionInfoSize = sizeof(os);
    GetVersionExW(&os);
    ZeroMemory(&identifier, sizeof(identifier));
    IDirect3D9_GetAdapterIdentifier(adapter, ordinal, 0, &identifier);
    ZeroMemory(&desc, sizeof(desc));
    if (SUCCEEDED(IDirect3DDevice9_GetBackBuffer(device, 0, 0,
                  D3DBACKBUFFER_TYPE_MONO, &backbuffer))) {
        IDirect3DSurface9_GetDesc(backbuffer, &desc);
        IDirect3DSurface9_Release(backbuffer);
    }
    QueryPerformanceFrequency(&frequency);
    record("# format=triton-kart-present-v2\n"
           "# os=%lu.%lu\n# qpc_frequency=%llu\n"
           "# process_arch=%s\n# wow64=%u\n# system_directory=%s\n"
           "# device_type=%u\n# behavior_flags=%08lx\n"
           "# width=%u\n# height=%u\n# umd_path=%s\n"
           "# adapter_driver=%s\n# adapter_description=%s\n"
           "frame,qpc_begin,qpc_end,hresult\n",
           (unsigned long)os.dwMajorVersion, (unsigned long)os.dwMinorVersion,
           (unsigned long long)frequency.QuadPart, process_arch,
           (unsigned)wow64, system_directory, (unsigned)type,
           (unsigned long)flags, desc.Width, desc.Height, umd_path,
           identifier.Driver, identifier.Description);
}

static HRESULT WINAPI create_device(IDirect3D9 *object, UINT ordinal,
                                    D3DDEVTYPE type, HWND focus, DWORD flags,
                                    D3DPRESENT_PARAMETERS *parameters,
                                    IDirect3DDevice9 **result)
{
    typedef HRESULT (WINAPI *function)(IDirect3D9 *, UINT, D3DDEVTYPE, HWND,
                                      DWORD, D3DPRESENT_PARAMETERS *,
                                      IDirect3DDevice9 **);
    DWORD entry_error = GetLastError();
    function original = (function)(uintptr_t)original_slot(
        (void **)&object->lpVtbl->CreateDevice);
    HRESULT status;
    DWORD result_error;
    BOOL describe;

    SetLastError(entry_error);
    status = original(object, ordinal, type, focus, flags, parameters, result);
    result_error = GetLastError();

    if (FAILED(status) || !result || !*result) {
        return status;
    }
    install_slot((void **)&(*result)->lpVtbl->Reset, (void *)(uintptr_t)reset);
    install_slot((void **)&(*result)->lpVtbl->GetSwapChain,
                 (void *)(uintptr_t)get_swapchain);
    install_slot((void **)&(*result)->lpVtbl->Present,
                 (void *)(uintptr_t)present_device);
    EnterCriticalSection(&lock);
    describe = !header_written;
    header_written = TRUE;
    LeaveCriticalSection(&lock);
    if (describe) {
        describe_device(object, *result, ordinal, type, flags);
    } else {
        record("# instrumentation_error=multiple devices\n");
    }
    SetLastError(result_error);
    return status;
}

static BOOL CALLBACK initialize_runtime(PINIT_ONCE once, PVOID parameter,
                                        PVOID *context)
{
    wchar_t path[MAX_PATH];
    HMODULE self;
    UINT length;

    (void)once;
    (void)parameter;
    (void)context;
    length = GetSystemDirectoryW(path, MAX_PATH);
    if (!length || length >= MAX_PATH - 10) {
        return TRUE;
    }
    memcpy(path + length, L"\\d3d9.dll", sizeof(L"\\d3d9.dll"));
    system_d3d9 = LoadLibraryW(path);
    if (!system_d3d9) {
        return TRUE;
    }
    length = GetEnvironmentVariableW(L"TRITON_KART_TIMINGS", path, MAX_PATH);
    if (length && length < MAX_PATH) {
        output = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                             CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    }
    /* Shared runtime tables can outlive any returned COM object. Pin our code
     * before installing a single slot, including across explicit FreeLibrary.
     */
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_PIN,
                           (LPCWSTR)(uintptr_t)Direct3DCreate9, &self)) {
        record("# instrumentation_error=module pin failed\n");
        return TRUE;
    }
    present_depth = TlsAlloc();
    instrumentation_ready = present_depth != TLS_OUT_OF_INDEXES;
    if (!instrumentation_ready) {
        record("# instrumentation_error=TLS allocation failed\n");
    }
    return TRUE;
}

IDirect3D9 *WINAPI Direct3DCreate9(UINT sdk_version)
{
    typedef IDirect3D9 *(WINAPI *create_function)(UINT);
    create_function create;
    IDirect3D9 *object;
    DWORD entry_error = GetLastError(), result_error;

    InitOnceExecuteOnce(&runtime_once, initialize_runtime, NULL, NULL);
    if (!system_d3d9) {
        return NULL;
    }
    create = (create_function)(uintptr_t)GetProcAddress(system_d3d9,
                                                       "Direct3DCreate9");
    if (!create) {
        return NULL;
    }
    SetLastError(entry_error);
    object = create(sdk_version);
    result_error = GetLastError();
    if (object && instrumentation_ready) {
        install_slot((void **)&object->lpVtbl->CreateDevice,
                     (void *)(uintptr_t)create_device);
    }
    SetLastError(result_error);
    return object;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        InitializeCriticalSection(&lock);
        DisableThreadLibraryCalls(instance);
    } else if (reason == DLL_PROCESS_DETACH) {
        /* Pinned modules detach only at process shutdown. Other threads have
         * stopped; do not acquire a lock they could have owned at termination.
         */
        char footer[80];
        int length = snprintf(footer, sizeof(footer),
                              active_presents ?
                              "# instrumentation_error=active presents at exit\n" :
                              "# capture_complete=%u\n", frame);
        flush_locked();
        if (length > 0 && (size_t)length < sizeof(footer)) {
            memcpy(pending, footer, (size_t)length);
            pending_size = (DWORD)length;
            flush_locked();
        }
        if (output != INVALID_HANDLE_VALUE) {
            CloseHandle(output);
        }
    }
    return TRUE;
}
