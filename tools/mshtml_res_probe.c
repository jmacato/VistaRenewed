#define COBJMACROS
#define CONST_VTABLE
#include <windows.h>
#include <ole2.h>
#include <urlmon.h>
#include <stdio.h>
#include "mshtml_resource_bind.h"

int main(int argc, char **argv)
{
    const WCHAR *urls[] = {L"res://ieframe.dll/dnserror.htm", L"res://ieframe.dll/tabswelcome.htm",
                           L"res://ieframe.dll/triton-missing-resource.htm"};
    unsigned i;
    BOOL scripts = argc == 2 && !strcmp(argv[1], "--scripts");
    if(scripts) { urls[0] = L"res://ieframe.dll/httpErrorPagesScripts.js"; urls[1] = L"res://ieframe.dll/errorPageStrings.js"; }
    if(FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED))) return 1;
    for(i = 0; i < 3; ++i) {
        IStream *stream = NULL;
        BYTE data[1024]; ULONG got = 0, total = 0;
        WCHAR mime[128];
        HRESULT hr = native_resource_open(urls[i], &stream, mime, ARRAYSIZE(mime));
        printf("NATIVE RES url=%ls hr=%08lx\n", urls[i], (unsigned long)hr);
        if(i == 2) { if(stream) IStream_Release(stream); if(SUCCEEDED(hr)) return 2; continue; }
        if(FAILED(hr) || !stream) return 3;
        printf("NATIVE MIME=%ls\n", mime);
        if(!scripts && lstrcmpiW(mime, L"text/html")) return 6;
        do {
            hr = IStream_Read(stream, data, sizeof(data), &got);
            if(FAILED(hr)) return 4;
            if(scripts || (argc == 2 && !strcmp(argv[1], "--dump"))) fwrite(data, 1, got, stdout);
            else if(!total) printf("PREFIX=%.*s\n", (int)(got < 120 ? got : 120), data);
            total += got;
        } while(got);
        IStream_Release(stream);
        printf("RESOURCE_BYTES=%lu\n", (unsigned long)total);
        if(!total) return 5;
    }
    CoUninitialize();
    puts("ORIGINAL MSHTML RES BINDING PASSED");
    return 0;
}
