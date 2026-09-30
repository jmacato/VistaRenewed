#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef COBJMACROS
#define COBJMACROS
#endif
#define _WIN32_WINNT 0x0600
#include <windows.h>
#include <tlhelp32.h>
#include <wincrypt.h>
#include <objbase.h>
#include <stdio.h>
#include <wchar.h>

static int module_in_use(const wchar_t *wanted)
{
    PROCESSENTRY32W process;
    HANDLE processes;
    wchar_t full[MAX_PATH];
    if(!GetFullPathNameW(wanted, MAX_PATH, full, NULL)) return 2;
    processes = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if(processes == INVALID_HANDLE_VALUE) return 2;
    ZeroMemory(&process, sizeof(process));
    process.dwSize = sizeof(process);
    if(Process32FirstW(processes, &process)) do {
        MODULEENTRY32W module;
        HANDLE modules;
        if(process.th32ProcessID == GetCurrentProcessId()) continue;
        modules = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                           process.th32ProcessID);
        if(modules == INVALID_HANDLE_VALUE) continue;
        ZeroMemory(&module, sizeof(module));
        module.dwSize = sizeof(module);
        if(Module32FirstW(modules, &module)) do {
            if(!lstrcmpiW(full, module.szExePath)) {
                wprintf(L"pid=%lu image=%ls\n", process.th32ProcessID, process.szExeFile);
                CloseHandle(modules);
                CloseHandle(processes);
                return 0;
            }
        } while(Module32NextW(modules, &module));
        CloseHandle(modules);
    } while(Process32NextW(processes, &process));
    CloseHandle(processes);
    return 3;
}

static int hold_module(const wchar_t *path, const wchar_t *duration_text)
{
    wchar_t *end = NULL;
    unsigned long duration = wcstoul(duration_text, &end, 10);
    HMODULE module;
    if(!end || *end || duration < 1000 || duration > 300000) return 2;
    /* Match the COM loader's dependency search rooted at the in-proc server. */
    module = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if(!module) {
        fwprintf(stderr, L"load failed win32=%lu\n", GetLastError());
        return 4;
    }
    Sleep((DWORD)duration);
    FreeLibrary(module);
    return 0;
}

static int hold_com(const wchar_t *duration_text)
{
    static const GUID html_document =
        {0x25336920, 0x03f9, 0x11cf, {0x8f, 0xd0, 0x00, 0xaa, 0x00, 0x68, 0x6f, 0x13}};
    static const GUID unknown_iid =
        {0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
    wchar_t *end = NULL;
    unsigned long duration = wcstoul(duration_text, &end, 10);
    IUnknown *object = NULL;
    HRESULT hr;
    if(!end || *end || duration < 1000 || duration > 300000) return 2;
    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if(FAILED(hr)) return 5;
    hr = CoCreateInstance(&html_document, NULL, CLSCTX_INPROC_SERVER,
                          &unknown_iid, (void **)&object);
    if(FAILED(hr)) {
        fwprintf(stderr, L"CoCreateInstance failed hr=%08lx\n", (unsigned long)hr);
        CoUninitialize();
        return 6;
    }
    Sleep((DWORD)duration);
    IUnknown_Release(object);
    CoUninitialize();
    return 0;
}

int wmain(int argc, wchar_t **argv)
{
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    HANDLE file = INVALID_HANDLE_VALUE;
    BYTE buffer[65536], digest[32];
    DWORD count, size = sizeof(digest);
    unsigned i;
    int result = 1;
    if(argc == 3 && !lstrcmpW(argv[1], L"--delete-reboot"))
        return MoveFileExW(argv[2], NULL, MOVEFILE_DELAY_UNTIL_REBOOT) ? 0 : 1;
    if(argc == 3 && !lstrcmpW(argv[1], L"--module-in-use"))
        return module_in_use(argv[2]);
    if(argc == 4 && !lstrcmpW(argv[1], L"--hold-module"))
        return hold_module(argv[2], argv[3]);
    if(argc == 3 && !lstrcmpW(argv[1], L"--hold-com"))
        return hold_com(argv[2]);
    if(argc != 2) return 2;
    file = CreateFileW(argv[1], GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if(file == INVALID_HANDLE_VALUE) goto done;
    if(!CryptAcquireContextW(&provider, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) ||
       !CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash)) goto done;
    for(;;) {
        if(!ReadFile(file, buffer, sizeof(buffer), &count, NULL)) goto done;
        if(!count) break;
        if(!CryptHashData(hash, buffer, count, 0)) goto done;
    }
    if(!CryptGetHashParam(hash, HP_HASHVAL, digest, &size, 0) || size != sizeof(digest)) goto done;
    for(i = 0; i < sizeof(digest); ++i) wprintf(L"%02x", digest[i]);
    wprintf(L"\n");
    result = 0;
done:
    if(file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if(hash) CryptDestroyHash(hash);
    if(provider) CryptReleaseContext(provider, 0);
    if(result) fwprintf(stderr, L"hash failed win32=%lu\n", GetLastError());
    return result;
}
