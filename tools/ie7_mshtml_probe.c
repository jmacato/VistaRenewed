/*
 * Read-only MSHTML activation probe for the Vista IE7 integration bring-up.
 *
 * This is deliberately a client of the stock HTMLDocument class.  It does not
 * modify any registry key, module, class factory, or running IE process.  Its
 * job is to establish the binary interface floor before an IE-process shim is
 * attempted.  ActiveX is unsupported by the replacement path; this probe only
 * records that policy and never instantiates a control.
 */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <objbase.h>
#include <mshtml.h>
#include <objsafe.h>
#include <stdio.h>
#include <stdlib.h>
#include <tlhelp32.h>

static void print_query(IUnknown *unknown, const char *name, REFIID iid)
{
    void *value = NULL;
    HRESULT hr = IUnknown_QueryInterface(unknown, iid, &value);
    printf("%s=0x%08lx\n", name, (unsigned long)hr);
    if(value) IUnknown_Release((IUnknown *)value);
}

static int capture_iexplore_modules(DWORD process_id)
{
    HANDLE snapshot;
    MODULEENTRY32W module;
    BOOL found_mshtml = FALSE;
    BOOL found_shim = FALSE;
    HANDLE process, token;

    printf("IEXPLORE_PID=%lu\n", (unsigned long)process_id);
    process = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, process_id);
    if(process) {
        WCHAR image[MAX_PATH];
        DWORD length = ARRAYSIZE(image);
        if(QueryFullProcessImageNameW(process, 0, image, &length))
            printf("PROCESS_IMAGE=%ls\n", image);
        if(OpenProcessToken(process, TOKEN_QUERY, &token)) {
            DWORD size = 0;
            GetTokenInformation(token, TokenIntegrityLevel, NULL, 0, &size);
            if(size) {
                TOKEN_MANDATORY_LABEL *label = HeapAlloc(GetProcessHeap(), 0, size);
                if(label && GetTokenInformation(token, TokenIntegrityLevel, label, size, &size)) {
                    DWORD rid = *GetSidSubAuthority(label->Label.Sid,
                        *GetSidSubAuthorityCount(label->Label.Sid) - 1);
                    printf("IEXPLORE_INTEGRITY_RID=%lu\n", (unsigned long)rid);
                    printf("PROCESS_INTEGRITY_RID=%lu\n", (unsigned long)rid);
                }
                if(label) HeapFree(GetProcessHeap(), 0, label);
            }
            CloseHandle(token);
        }
        CloseHandle(process);
    }
    snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, process_id);
    if(snapshot == INVALID_HANDLE_VALUE) {
        printf("IEXPLORE_MODULE_ENUMERATION=0x%08lx\n", (unsigned long)GetLastError());
        return 1;
    }
    ZeroMemory(&module, sizeof(module));
    module.dwSize = sizeof(module);
    if(!Module32FirstW(snapshot, &module)) {
        printf("IEXPLORE_MODULE_ENUMERATION=0x%08lx\n", (unsigned long)GetLastError());
        CloseHandle(snapshot);
        return 1;
    }
    do {
        if(!lstrcmpiW(module.szModule, L"iexplore.exe"))
            printf("IEXPLORE_IMAGE=%ls\n", module.szExePath);
        if(!lstrcmpiW(module.szModule, L"mshtml.dll")) {
            printf("IEXPLORE_MSHTML_MODULE=%ls\n", module.szExePath);
            found_mshtml = TRUE;
        }
        if(!lstrcmpiW(module.szModule, L"triton-ie7-mshtml-activation-probe.dll")) {
            printf("IEXPLORE_SHIM_MODULE=%ls\n", module.szExePath);
            found_shim = TRUE;
        }
    } while(Module32NextW(snapshot, &module));
    CloseHandle(snapshot);
    printf("IEXPLORE_MSHTML_LOADED=%s\n", found_mshtml ? "yes" : "no");
    printf("IEXPLORE_SHIM_LOADED=%s\n", found_shim ? "yes" : "no");
    return found_mshtml ? 0 : 1;
}

int main(int argc, char **argv)
{
    IClassFactory *factory = NULL;
    IUnknown *document = NULL;
    WCHAR system_directory[MAX_PATH];
    HRESULT hr;
    int status = 1;

    if(argc > 2) {
        printf("usage: %s [iexplore-process-id]\n", argv[0]);
        return 2;
    }
    if(argc == 2) {
        char *end = NULL;
        unsigned long process_id = strtoul(argv[1], &end, 10);
        if(!argv[1][0] || !end || *end || process_id == 0 || process_id > 0xffffffffUL)
            return 2;
        return capture_iexplore_modules((DWORD)process_id);
    }
    if(!GetSystemDirectoryW(system_directory, ARRAYSIZE(system_directory))) {
        printf("IEXPLORE_PROBE_ERROR=GetSystemDirectory:%lu\n", GetLastError());
        return 1;
    }
    printf("MSHTML_MODULE=%ls\\mshtml.dll\n", system_directory);
    printf("ACTIVEX_POLICY=unsupported-stubbed\n");
    printf("ACTIVEX_NOTE=ActiveX is unsupported by the replacement path\n");

    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if(FAILED(hr)) {
        printf("IEXPLORE_PROBE_ERROR=CoInitialize:0x%08lx\n", (unsigned long)hr);
        return 1;
    }
    hr = CoGetClassObject(&CLSID_HTMLDocument, CLSCTX_INPROC_SERVER, NULL,
                          &IID_IClassFactory, (void **)&factory);
    printf("HTMLDOCUMENT_FACTORY=0x%08lx\n", (unsigned long)hr);
    if(FAILED(hr)) goto done;
    hr = IClassFactory_CreateInstance(factory, NULL, &IID_IUnknown, (void **)&document);
    printf("HTMLDOCUMENT_INSTANCE=0x%08lx\n", (unsigned long)hr);
    if(FAILED(hr)) goto done;

    print_query(document, "IHTMLDocument2", &IID_IHTMLDocument2);
    print_query(document, "IHTMLDocument3", &IID_IHTMLDocument3);
    print_query(document, "IHTMLDocument4", &IID_IHTMLDocument4);
    print_query(document, "IHTMLDocument5", &IID_IHTMLDocument5);
    print_query(document, "IHTMLDocument6", &IID_IHTMLDocument6);
    print_query(document, "IObjectSafety", &IID_IObjectSafety);
    print_query(document, "IOleObject", &IID_IOleObject);
    status = 0;
done:
    if(document) IUnknown_Release(document);
    if(factory) IClassFactory_Release(factory);
    CoUninitialize();
    if(!status) printf("IEXPLORE MSHTML PROBE VERIFIED\n");
    return status;
}
