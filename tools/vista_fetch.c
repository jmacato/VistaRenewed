/* Vista-compatible streaming HTTP download, hash verification, atomic install. */
#ifndef UNICODE
#define UNICODE
#endif
#define _UNICODE
#define _WIN32_WINNT 0x0600
#include <windows.h>
#include <winhttp.h>
#include <wincrypt.h>
#include <stdio.h>
#include <wchar.h>

int wmain(int argc, wchar_t **argv)
{
    URL_COMPONENTS url = {0};
    WCHAR host[256], path[2048], temporary[MAX_PATH], actual[65];
    HINTERNET session = NULL, connection = NULL, request = NULL;
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    HANDLE file = INVALID_HANDLE_VALUE;
    BYTE buffer[65536], digest[32];
    DWORD count, written, status, size = sizeof(status), digest_size = 32;
    ULONGLONG total = 0;
    unsigned i;
    int result = 1;
    BOOL created = FALSE;
    if(argc != 4 || wcslen(argv[2]) > MAX_PATH - 48 || wcslen(argv[3]) != 64) {
        fwprintf(stderr, L"usage: vista-fetch URL DEST SHA256\n");
        return 2;
    }
    for(i = 0; i < 64; ++i)
        if(!((argv[3][i] >= L'0' && argv[3][i] <= L'9') ||
             (argv[3][i] >= L'a' && argv[3][i] <= L'f'))) return 2;
    swprintf(temporary, MAX_PATH, L"%ls.triton-%lu-%lu.part", argv[2],
             (unsigned long)GetCurrentProcessId(), (unsigned long)GetTickCount());
    url.dwStructSize = sizeof(url);
    url.lpszHostName = host; url.dwHostNameLength = 256;
    url.lpszUrlPath = path; url.dwUrlPathLength = 2048;
    if(!WinHttpCrackUrl(argv[1], 0, 0, &url) || url.nScheme != INTERNET_SCHEME_HTTP) goto done;
    session = WinHttpOpen(L"TritonVistaFetch/1", WINHTTP_ACCESS_TYPE_NO_PROXY,
                         WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if(!session || !WinHttpSetTimeouts(session, 10000, 10000, 30000, 30000)) goto done;
    connection = WinHttpConnect(session, host, url.nPort, 0);
    if(!connection) goto done;
    request = WinHttpOpenRequest(connection, L"GET", path, NULL,
                                WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if(!request) goto done;
    status = WINHTTP_DISABLE_REDIRECTS;
    if(!WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &status, sizeof(status))) goto done;
    if(!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                          WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
       !WinHttpReceiveResponse(request, NULL) ||
       !WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, NULL) || status != 200) goto done;
    if(!CryptAcquireContextW(&provider, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) ||
       !CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash)) goto done;
    file = CreateFileW(temporary, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if(file == INVALID_HANDLE_VALUE) goto done;
    created = TRUE;
    for(;;) {
        if(!WinHttpReadData(request, buffer, sizeof(buffer), &count)) goto done;
        if(!count) break;
        if(!CryptHashData(hash, buffer, count, 0) ||
           !WriteFile(file, buffer, count, &written, NULL) || written != count) goto done;
        total += count;
    }
    if(!CryptGetHashParam(hash, HP_HASHVAL, digest, &digest_size, 0)) goto done;
    for(i = 0; i < 32; ++i) swprintf(actual + i * 2, 3, L"%02x", digest[i]);
    if(wcscmp(actual, argv[3])) {
        fwprintf(stderr, L"SHA256 mismatch; destination unchanged\n");
        SetLastError(ERROR_CRC);
        goto done;
    }
    if(!FlushFileBuffers(file)) goto done;
    CloseHandle(file); file = INVALID_HANDLE_VALUE;
    if(!MoveFileExW(temporary, argv[2], MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) goto done;
    wprintf(L"TCP TRANSFER VERIFIED bytes=%llu sha256=%ls\n", total, actual);
    result = 0;
done:
    if(result) fprintf(stderr, "fetch failed win32=%lu\n", (unsigned long)GetLastError());
    if(file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if(created && result) DeleteFileW(temporary);
    if(hash) CryptDestroyHash(hash);
    if(provider) CryptReleaseContext(provider, 0);
    if(request) WinHttpCloseHandle(request);
    if(connection) WinHttpCloseHandle(connection);
    if(session) WinHttpCloseHandle(session);
    return result;
}
