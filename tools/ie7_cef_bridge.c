/*
 * Private IE7 automation facade for the CEF bridge experiment.
 *
 * This DLL never registers a stock browser class.  It proves that a native
 * Vista COM apartment can preserve the most useful IE7 DISPIDs while the
 * renderer lives behind a replaceable adapter.  The default adapter is a
 * deliberately visible in-memory mock; it is not a CEF runtime.  A test host
 * may explicitly select the separately staged Supermium executable through
 * TRITON_SUPERMIUM_EXE.  That diagnostic path serializes a Chromium DOM on
 * close(), but it still never takes over a stock browser registration.
 */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <objbase.h>
#include <oaidl.h>
#include <oleauto.h>
#include <wchar.h>

#define DISPIDS_DOCUMENT_WRITE 1054
#define DISPIDS_DOCUMENT_WRITELN 1055
#define DISPIDS_DOCUMENT_OPEN 1056
#define DISPIDS_DOCUMENT_CLOSE 1057
#define DISPIDS_DOCUMENT_URL 1025
#define DISPIDS_DOCUMENT_READYSTATE 1018
#define DISPIDS_DOCUMENT_TITLE 1012
#define DISPIDS_DOCUMENT_BODY 1004
#define DISPIDS_DOCUMENT_GETELEMENTBYID 1088
#define DISPIDS_DOCUMENT_BACKEND 0x6001
#define DISPIDS_DOCUMENT_ACTIVEX_POLICY 0x6002
#define DISPIDS_BODY_INNERHTML (-2147417086L)
#define DISPIDS_BODY_OUTERHTML (-2147417084L)

/* Keep this CLSID private to the experiment. */
static const CLSID CLSID_TritonCefHtmlDocument =
    {0xa9e4d11a, 0x3f5c, 0x4e8a, {0x9c, 0x77, 0x93, 0x71, 0x2b, 0xc6, 0x48, 0x02}};
static const WCHAR triton_progid[] = L"Triton.CefHtmlDocument.1";
static const WCHAR triton_description[] = L"Triton private IE7 CEF bridge document";
static HINSTANCE module_handle;

typedef struct RendererState {
    LONG references;
    BSTR url;
    BSTR title;
    BSTR html;
    BSTR supermium_exe;
    BOOL closed;
} RendererState;

typedef struct Body Body;
typedef struct Document Document;

struct Body {
    IDispatch IDispatch_iface;
    LONG references;
    RendererState *state;
};

struct Document {
    IDispatch IDispatch_iface;
    LONG references;
    RendererState *state;
    Body *body;
};

typedef struct ClassFactory {
    IClassFactory IClassFactory_iface;
    LONG references;
} ClassFactory;

static HRESULT STDMETHODCALLTYPE document_query_interface(IDispatch *iface, REFIID iid,
                                                           void **out);
static ULONG STDMETHODCALLTYPE document_add_ref(IDispatch *iface);
static ULONG STDMETHODCALLTYPE document_release(IDispatch *iface);
static HRESULT STDMETHODCALLTYPE document_get_type_info_count(IDispatch *iface, UINT *count);
static HRESULT STDMETHODCALLTYPE document_get_type_info(IDispatch *iface, UINT index, LCID locale,
                                                         ITypeInfo **info);
static HRESULT STDMETHODCALLTYPE document_get_ids_of_names(IDispatch *iface, REFIID iid,
                                                            LPOLESTR *names, UINT count,
                                                            LCID locale, DISPID *ids);
static HRESULT STDMETHODCALLTYPE document_invoke(IDispatch *iface, DISPID id, REFIID iid,
                                                 LCID locale, WORD flags, DISPPARAMS *params,
                                                 VARIANT *result, EXCEPINFO *exception,
                                                 UINT *argument_error);

static HRESULT STDMETHODCALLTYPE body_query_interface(IDispatch *iface, REFIID iid, void **out);
static ULONG STDMETHODCALLTYPE body_add_ref(IDispatch *iface);
static ULONG STDMETHODCALLTYPE body_release(IDispatch *iface);
static HRESULT STDMETHODCALLTYPE body_get_type_info_count(IDispatch *iface, UINT *count);
static HRESULT STDMETHODCALLTYPE body_get_type_info(IDispatch *iface, UINT index, LCID locale,
                                                     ITypeInfo **info);
static HRESULT STDMETHODCALLTYPE body_get_ids_of_names(IDispatch *iface, REFIID iid,
                                                        LPOLESTR *names, UINT count,
                                                        LCID locale, DISPID *ids);
static HRESULT STDMETHODCALLTYPE body_invoke(IDispatch *iface, DISPID id, REFIID iid,
                                             LCID locale, WORD flags, DISPPARAMS *params,
                                             VARIANT *result, EXCEPINFO *exception,
                                             UINT *argument_error);

static IDispatchVtbl document_vtbl = {
    document_query_interface, document_add_ref, document_release,
    document_get_type_info_count, document_get_type_info, document_get_ids_of_names,
    document_invoke,
};

static IDispatchVtbl body_vtbl = {
    body_query_interface, body_add_ref, body_release,
    body_get_type_info_count, body_get_type_info, body_get_ids_of_names, body_invoke,
};

static RendererState *renderer_create(void)
{
    RendererState *state = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*state));
    WCHAR executable[MAX_PATH];
    DWORD length;
    if(!state) return NULL;
    state->references = 1;
    state->url = SysAllocString(L"about:blank");
    state->title = SysAllocString(L"");
    state->html = SysAllocString(L"");
    length = GetEnvironmentVariableW(L"TRITON_SUPERMIUM_EXE", executable, ARRAYSIZE(executable));
    if(length && length < ARRAYSIZE(executable)) state->supermium_exe = SysAllocString(executable);
    if(!state->url || !state->title || !state->html || (length && !state->supermium_exe)) {
        SysFreeString(state->url);
        SysFreeString(state->title);
        SysFreeString(state->html);
        SysFreeString(state->supermium_exe);
        HeapFree(GetProcessHeap(), 0, state);
        return NULL;
    }
    return state;
}

static void renderer_add_ref(RendererState *state)
{
    InterlockedIncrement(&state->references);
}

static void renderer_release(RendererState *state)
{
    if(InterlockedDecrement(&state->references) == 0) {
        SysFreeString(state->url);
        SysFreeString(state->title);
        SysFreeString(state->html);
        SysFreeString(state->supermium_exe);
        HeapFree(GetProcessHeap(), 0, state);
    }
}

static HRESULT set_bstr(BSTR *destination, const WCHAR *text)
{
    BSTR replacement = SysAllocString(text ? text : L"");
    if(!replacement) return E_OUTOFMEMORY;
    SysFreeString(*destination);
    *destination = replacement;
    return S_OK;
}

static HRESULT write_utf8_file(const WCHAR *path, BSTR text)
{
    HANDLE file;
    DWORD written, bytes;
    int required;
    char *buffer;
    UINT length = text ? SysStringLen(text) : 0;

    required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text ? text : L"", length,
                                   NULL, 0, NULL, NULL);
    if(!required && length) return HRESULT_FROM_WIN32(GetLastError());
    buffer = HeapAlloc(GetProcessHeap(), 0, required ? (SIZE_T)required : 1);
    if(!buffer) return E_OUTOFMEMORY;
    if(required && !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, length, buffer,
                                        required, NULL, NULL)) {
        DWORD error = GetLastError();
        HeapFree(GetProcessHeap(), 0, buffer);
        return HRESULT_FROM_WIN32(error);
    }
    file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if(file == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        HeapFree(GetProcessHeap(), 0, buffer);
        return HRESULT_FROM_WIN32(error);
    }
    bytes = (DWORD)required;
    if(!WriteFile(file, buffer, bytes, &written, NULL) || written != bytes) {
        DWORD error = GetLastError();
        CloseHandle(file);
        HeapFree(GetProcessHeap(), 0, buffer);
        return HRESULT_FROM_WIN32(error ? error : ERROR_WRITE_FAULT);
    }
    CloseHandle(file);
    HeapFree(GetProcessHeap(), 0, buffer);
    return S_OK;
}

static HRESULT read_utf8_file(const WCHAR *path, BSTR *result)
{
    HANDLE file;
    LARGE_INTEGER size;
    DWORD read;
    int characters;
    char *bytes;
    BSTR text;

    *result = NULL;
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if(file == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
    if(!GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > 1024 * 1024) {
        DWORD error = GetLastError();
        CloseHandle(file);
        return HRESULT_FROM_WIN32(error ? error : ERROR_FILE_TOO_LARGE);
    }
    bytes = HeapAlloc(GetProcessHeap(), 0, (SIZE_T)size.QuadPart + 1);
    if(!bytes) {
        CloseHandle(file);
        return E_OUTOFMEMORY;
    }
    if(!ReadFile(file, bytes, (DWORD)size.QuadPart, &read, NULL) || read != (DWORD)size.QuadPart) {
        DWORD error = GetLastError();
        CloseHandle(file);
        HeapFree(GetProcessHeap(), 0, bytes);
        return HRESULT_FROM_WIN32(error ? error : ERROR_READ_FAULT);
    }
    CloseHandle(file);
    characters = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes, read, NULL, 0);
    if(!characters && read) {
        DWORD error = GetLastError();
        HeapFree(GetProcessHeap(), 0, bytes);
        return HRESULT_FROM_WIN32(error);
    }
    text = SysAllocStringLen(NULL, characters);
    if(!text) {
        HeapFree(GetProcessHeap(), 0, bytes);
        return E_OUTOFMEMORY;
    }
    if(characters && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes, read, text,
                                           characters)) {
        DWORD error = GetLastError();
        SysFreeString(text);
        HeapFree(GetProcessHeap(), 0, bytes);
        return HRESULT_FROM_WIN32(error);
    }
    HeapFree(GetProcessHeap(), 0, bytes);
    *result = text;
    return S_OK;
}

static HRESULT supermium_serialize(RendererState *state)
{
    WCHAR temporary[MAX_PATH], input[MAX_PATH], output[MAX_PATH], url[MAX_PATH + 12], command[2048];
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    SECURITY_ATTRIBUTES inheritable;
    HANDLE stdin_file = INVALID_HANDLE_VALUE, stdout_file = INVALID_HANDLE_VALUE;
    HANDLE stderr_file = INVALID_HANDLE_VALUE;
    DWORD temporary_length, wait, exit_code, error = ERROR_GEN_FAILURE;
    HRESULT hr;
    BSTR serialized = NULL;
    UINT index;

    if(!state->supermium_exe || !state->supermium_exe[0]) return E_UNEXPECTED;
    temporary_length = GetTempPathW(ARRAYSIZE(temporary), temporary);
    if(!temporary_length || temporary_length >= ARRAYSIZE(temporary))
        return HRESULT_FROM_WIN32(GetLastError() ? GetLastError() : ERROR_BUFFER_OVERFLOW);
    if(!GetTempFileNameW(temporary, L"TSI", 0, input)) return HRESULT_FROM_WIN32(GetLastError());
    {
        WCHAR *extension = wcsrchr(input, L'.');
        if(!extension || !DeleteFileW(input)) {
            error = GetLastError();
            DeleteFileW(input);
            return HRESULT_FROM_WIN32(error ? error : ERROR_INVALID_NAME);
        }
        lstrcpyW(extension, L".html");
    }
    if(!GetTempFileNameW(temporary, L"TSO", 0, output)) {
        error = GetLastError();
        DeleteFileW(input);
        return HRESULT_FROM_WIN32(error);
    }
    hr = write_utf8_file(input, state->html);
    if(FAILED(hr)) goto done;
    lstrcpyW(url, L"file:///");
    lstrcatW(url, input);
    for(index = 0; url[index]; index++) if(url[index] == L'\\') url[index] = L'/';
    if(swprintf(command, ARRAYSIZE(command),
                L"%ls --headless --no-sandbox --disable-gpu --no-first-run "
                L"--disable-background-networking --disable-component-update --disable-default-apps "
                L"--metrics-recording-only --user-data-dir=C:\\TritonSupermiumBridgeProfile "
                L"--dump-dom %ls", state->supermium_exe, url) < 0) {
        hr = E_FAIL;
        goto done;
    }
    inheritable.nLength = sizeof(inheritable);
    inheritable.lpSecurityDescriptor = NULL;
    inheritable.bInheritHandle = TRUE;
    stdin_file = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    stdout_file = CreateFileW(output, GENERIC_WRITE, FILE_SHARE_READ, &inheritable, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    stderr_file = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if(stdin_file == INVALID_HANDLE_VALUE || stdout_file == INVALID_HANDLE_VALUE ||
       stderr_file == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        hr = HRESULT_FROM_WIN32(error);
        goto done;
    }
    ZeroMemory(&startup, sizeof(startup));
    ZeroMemory(&process, sizeof(process));
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = stdin_file;
    startup.hStdOutput = stdout_file;
    startup.hStdError = stderr_file;
    if(!CreateProcessW(NULL, command, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL,
                       &startup, &process)) {
        hr = HRESULT_FROM_WIN32(GetLastError());
        goto done;
    }
    wait = WaitForSingleObject(process.hProcess, 45000);
    if(wait != WAIT_OBJECT_0) {
        if(wait == WAIT_TIMEOUT) TerminateProcess(process.hProcess, ERROR_TIMEOUT);
        hr = HRESULT_FROM_WIN32(wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError());
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        goto done;
    }
    if(!GetExitCodeProcess(process.hProcess, &exit_code) || exit_code != 0) {
        error = GetLastError();
        hr = HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        goto done;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CloseHandle(stdin_file);
    CloseHandle(stdout_file);
    CloseHandle(stderr_file);
    stdin_file = INVALID_HANDLE_VALUE;
    stdout_file = INVALID_HANDLE_VALUE;
    stderr_file = INVALID_HANDLE_VALUE;
    hr = read_utf8_file(output, &serialized);
    if(SUCCEEDED(hr)) {
        SysFreeString(state->html);
        state->html = serialized;
        serialized = NULL;
    }
done:
    if(stdin_file != INVALID_HANDLE_VALUE) CloseHandle(stdin_file);
    if(stdout_file != INVALID_HANDLE_VALUE) CloseHandle(stdout_file);
    if(stderr_file != INVALID_HANDLE_VALUE) CloseHandle(stderr_file);
    SysFreeString(serialized);
    DeleteFileW(input);
    DeleteFileW(output);
    return hr;
}

static HRESULT append_text(RendererState *state, const WCHAR *text, UINT text_length)
{
    UINT old_length = SysStringLen(state->html);
    BSTR replacement;
    if(text_length > 0xffffffffU - old_length) return E_OUTOFMEMORY;
    replacement = SysAllocStringLen(NULL, old_length + text_length);
    if(!replacement) return E_OUTOFMEMORY;
    if(old_length) CopyMemory(replacement, state->html, old_length * sizeof(WCHAR));
    if(text_length) CopyMemory(replacement + old_length, text, text_length * sizeof(WCHAR));
    SysFreeString(state->html);
    state->html = replacement;
    return S_OK;
}

static HRESULT append_variant(RendererState *state, const VARIANT *value)
{
    HRESULT hr;
    LONG lower, upper, index;

    if(value->vt == VT_BSTR)
        return append_text(state, value->bstrVal ? value->bstrVal : L"",
                           value->bstrVal ? SysStringLen(value->bstrVal) : 0);
    if(value->vt == (VT_ARRAY | VT_BSTR)) {
        hr = SafeArrayGetLBound(value->parray, 1, &lower);
        if(FAILED(hr)) return hr;
        hr = SafeArrayGetUBound(value->parray, 1, &upper);
        if(FAILED(hr)) return hr;
        for(index = lower; index <= upper; index++) {
            BSTR item = NULL;
            hr = SafeArrayGetElement(value->parray, &index, &item);
            if(FAILED(hr)) return hr;
            hr = append_text(state, item ? item : L"", item ? SysStringLen(item) : 0);
            SysFreeString(item);
            if(FAILED(hr)) return hr;
        }
        return S_OK;
    }
    if(value->vt == (VT_ARRAY | VT_VARIANT)) {
        hr = SafeArrayGetLBound(value->parray, 1, &lower);
        if(FAILED(hr)) return hr;
        hr = SafeArrayGetUBound(value->parray, 1, &upper);
        if(FAILED(hr)) return hr;
        for(index = lower; index <= upper; index++) {
            VARIANT item;
            VariantInit(&item);
            hr = SafeArrayGetElement(value->parray, &index, &item);
            if(SUCCEEDED(hr)) hr = append_variant(state, &item);
            VariantClear(&item);
            if(FAILED(hr)) return hr;
        }
        return S_OK;
    }
    return DISP_E_TYPEMISMATCH;
}

static HRESULT set_from_variant(BSTR *destination, const VARIANT *value)
{
    if(value->vt != VT_BSTR) return DISP_E_TYPEMISMATCH;
    return set_bstr(destination, value->bstrVal);
}

static HRESULT result_bstr(VARIANT *result, const WCHAR *text)
{
    if(!result) return E_POINTER;
    VariantInit(result);
    result->vt = VT_BSTR;
    result->bstrVal = SysAllocString(text ? text : L"");
    return result->bstrVal ? S_OK : E_OUTOFMEMORY;
}

static HRESULT result_dispatch(VARIANT *result, IDispatch *dispatch)
{
    if(!result || !dispatch) return E_POINTER;
    VariantInit(result);
    result->vt = VT_DISPATCH;
    result->pdispVal = dispatch;
    IDispatch_AddRef(dispatch);
    return S_OK;
}

static Body *body_create(RendererState *state)
{
    Body *body = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*body));
    if(!body) return NULL;
    body->IDispatch_iface.lpVtbl = &body_vtbl;
    body->references = 1;
    body->state = state;
    renderer_add_ref(state);
    return body;
}

static HRESULT document_body(Document *document, VARIANT *result)
{
    if(!document->body) {
        document->body = body_create(document->state);
        if(!document->body) return E_OUTOFMEMORY;
    }
    return result_dispatch(result, &document->body->IDispatch_iface);
}

static HRESULT STDMETHODCALLTYPE document_query_interface(IDispatch *iface, REFIID iid,
                                                           void **out)
{
    if(!out) return E_POINTER;
    *out = NULL;
    if(IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IDispatch)) {
        *out = iface;
        document_add_ref(iface);
        return S_OK;
    }
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE document_add_ref(IDispatch *iface)
{
    Document *document = (Document *)iface;
    return (ULONG)InterlockedIncrement(&document->references);
}

static ULONG STDMETHODCALLTYPE document_release(IDispatch *iface)
{
    Document *document = (Document *)iface;
    LONG references = InterlockedDecrement(&document->references);
    if(references == 0) {
        if(document->body) IDispatch_Release(&document->body->IDispatch_iface);
        renderer_release(document->state);
        HeapFree(GetProcessHeap(), 0, document);
    }
    return (ULONG)references;
}

static HRESULT STDMETHODCALLTYPE document_get_type_info_count(IDispatch *iface, UINT *count)
{
    (void)iface;
    if(!count) return E_POINTER;
    *count = 0;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE document_get_type_info(IDispatch *iface, UINT index, LCID locale,
                                                         ITypeInfo **info)
{
    (void)iface;
    (void)index;
    (void)locale;
    if(info) *info = NULL;
    return DISP_E_BADINDEX;
}

static HRESULT document_name_to_id(const WCHAR *name, DISPID *id)
{
    if(!lstrcmpiW(name, L"write")) *id = DISPIDS_DOCUMENT_WRITE;
    else if(!lstrcmpiW(name, L"writeln")) *id = DISPIDS_DOCUMENT_WRITELN;
    else if(!lstrcmpiW(name, L"open")) *id = DISPIDS_DOCUMENT_OPEN;
    else if(!lstrcmpiW(name, L"close")) *id = DISPIDS_DOCUMENT_CLOSE;
    else if(!lstrcmpiW(name, L"url")) *id = DISPIDS_DOCUMENT_URL;
    else if(!lstrcmpiW(name, L"readyState")) *id = DISPIDS_DOCUMENT_READYSTATE;
    else if(!lstrcmpiW(name, L"title")) *id = DISPIDS_DOCUMENT_TITLE;
    else if(!lstrcmpiW(name, L"body")) *id = DISPIDS_DOCUMENT_BODY;
    else if(!lstrcmpiW(name, L"getElementById")) *id = DISPIDS_DOCUMENT_GETELEMENTBYID;
    else if(!lstrcmpiW(name, L"backend")) *id = DISPIDS_DOCUMENT_BACKEND;
    /* This private facade has no ActiveX object, control, or plug-in path. */
    else if(!lstrcmpiW(name, L"activeXPolicy")) *id = DISPIDS_DOCUMENT_ACTIVEX_POLICY;
    else return DISP_E_UNKNOWNNAME;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE document_get_ids_of_names(IDispatch *iface, REFIID iid,
                                                            LPOLESTR *names, UINT count,
                                                            LCID locale, DISPID *ids)
{
    UINT index;
    HRESULT hr;
    (void)iface;
    (void)iid;
    (void)locale;
    if(!names || !ids) return E_POINTER;
    for(index = 0; index < count; index++) {
        hr = document_name_to_id(names[index], &ids[index]);
        if(FAILED(hr)) return hr;
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE document_invoke(IDispatch *iface, DISPID id, REFIID iid,
                                                 LCID locale, WORD flags, DISPPARAMS *params,
                                                 VARIANT *result, EXCEPINFO *exception,
                                                 UINT *argument_error)
{
    Document *document = (Document *)iface;
    HRESULT hr;
    (void)iid;
    (void)locale;
    (void)exception;
    if(argument_error) *argument_error = 0;
    if(result) VariantInit(result);
    switch(id) {
    case DISPIDS_DOCUMENT_OPEN:
        if(!(flags & DISPATCH_METHOD)) return DISP_E_MEMBERNOTFOUND;
        hr = set_bstr(&document->state->url, L"about:blank");
        if(FAILED(hr)) return hr;
        hr = set_bstr(&document->state->html, L"");
        if(FAILED(hr)) return hr;
        document->state->closed = FALSE;
        return S_OK;
    case DISPIDS_DOCUMENT_WRITE:
    case DISPIDS_DOCUMENT_WRITELN:
        if(!(flags & DISPATCH_METHOD)) return DISP_E_MEMBERNOTFOUND;
        if(!params || params->cArgs != 1) return DISP_E_BADPARAMCOUNT;
        hr = append_variant(document->state, &params->rgvarg[0]);
        if(FAILED(hr)) return hr;
        if(id == DISPIDS_DOCUMENT_WRITELN)
            return append_text(document->state, L"\r\n", 2);
        return S_OK;
    case DISPIDS_DOCUMENT_CLOSE:
        if(!(flags & DISPATCH_METHOD)) return DISP_E_MEMBERNOTFOUND;
        if(document->state->supermium_exe) {
            hr = supermium_serialize(document->state);
            if(FAILED(hr)) return hr;
        }
        document->state->closed = TRUE;
        return S_OK;
    case DISPIDS_DOCUMENT_URL:
        return (flags & DISPATCH_PROPERTYGET) ? result_bstr(result, document->state->url)
                                              : DISP_E_MEMBERNOTFOUND;
    case DISPIDS_DOCUMENT_READYSTATE:
        return (flags & DISPATCH_PROPERTYGET)
            ? result_bstr(result, document->state->closed ? L"complete" : L"loading")
            : DISP_E_MEMBERNOTFOUND;
    case DISPIDS_DOCUMENT_TITLE:
        if(flags & DISPATCH_PROPERTYGET) return result_bstr(result, document->state->title);
        if(flags & (DISPATCH_PROPERTYPUT | DISPATCH_PROPERTYPUTREF)) {
            if(!params || params->cArgs != 1) return DISP_E_BADPARAMCOUNT;
            return set_from_variant(&document->state->title, &params->rgvarg[0]);
        }
        return DISP_E_MEMBERNOTFOUND;
    case DISPIDS_DOCUMENT_BODY:
        return (flags & DISPATCH_PROPERTYGET) ? document_body(document, result)
                                              : DISP_E_MEMBERNOTFOUND;
    case DISPIDS_DOCUMENT_GETELEMENTBYID:
        if(!(flags & DISPATCH_METHOD)) return DISP_E_MEMBERNOTFOUND;
        if(!params || params->cArgs != 1) return DISP_E_BADPARAMCOUNT;
        return document_body(document, result);
    case DISPIDS_DOCUMENT_BACKEND:
        if(!(flags & DISPATCH_PROPERTYGET)) return DISP_E_MEMBERNOTFOUND;
        return result_bstr(result, document->state->supermium_exe
                                    ? L"supermium-144-headless-adapter"
                                    : L"mock-cef-adapter");
    case DISPIDS_DOCUMENT_ACTIVEX_POLICY:
        if(!(flags & DISPATCH_PROPERTYGET)) return DISP_E_MEMBERNOTFOUND;
        return result_bstr(result, L"unsupported-stubbed");
    default:
        return DISP_E_MEMBERNOTFOUND;
    }
}

static HRESULT STDMETHODCALLTYPE body_query_interface(IDispatch *iface, REFIID iid, void **out)
{
    if(!out) return E_POINTER;
    *out = NULL;
    if(IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IDispatch)) {
        *out = iface;
        body_add_ref(iface);
        return S_OK;
    }
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE body_add_ref(IDispatch *iface)
{
    Body *body = (Body *)iface;
    return (ULONG)InterlockedIncrement(&body->references);
}

static ULONG STDMETHODCALLTYPE body_release(IDispatch *iface)
{
    Body *body = (Body *)iface;
    LONG references = InterlockedDecrement(&body->references);
    if(references == 0) {
        renderer_release(body->state);
        HeapFree(GetProcessHeap(), 0, body);
    }
    return (ULONG)references;
}

static HRESULT STDMETHODCALLTYPE body_get_type_info_count(IDispatch *iface, UINT *count)
{
    (void)iface;
    if(!count) return E_POINTER;
    *count = 0;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE body_get_type_info(IDispatch *iface, UINT index, LCID locale,
                                                     ITypeInfo **info)
{
    (void)iface;
    (void)index;
    (void)locale;
    if(info) *info = NULL;
    return DISP_E_BADINDEX;
}

static HRESULT STDMETHODCALLTYPE body_get_ids_of_names(IDispatch *iface, REFIID iid,
                                                        LPOLESTR *names, UINT count,
                                                        LCID locale, DISPID *ids)
{
    UINT index;
    (void)iface;
    (void)iid;
    (void)locale;
    if(!names || !ids) return E_POINTER;
    for(index = 0; index < count; index++) {
        if(!lstrcmpiW(names[index], L"innerHTML")) ids[index] = DISPIDS_BODY_INNERHTML;
        else if(!lstrcmpiW(names[index], L"outerHTML")) ids[index] = DISPIDS_BODY_OUTERHTML;
        else return DISP_E_UNKNOWNNAME;
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE body_invoke(IDispatch *iface, DISPID id, REFIID iid,
                                             LCID locale, WORD flags, DISPPARAMS *params,
                                             VARIANT *result, EXCEPINFO *exception,
                                             UINT *argument_error)
{
    Body *body = (Body *)iface;
    (void)iid;
    (void)locale;
    (void)exception;
    if(argument_error) *argument_error = 0;
    if(result) VariantInit(result);
    if(id != DISPIDS_BODY_INNERHTML && id != DISPIDS_BODY_OUTERHTML)
        return DISP_E_MEMBERNOTFOUND;
    if(flags & DISPATCH_PROPERTYGET) return result_bstr(result, body->state->html);
    if(flags & (DISPATCH_PROPERTYPUT | DISPATCH_PROPERTYPUTREF)) {
        if(!params || params->cArgs != 1) return DISP_E_BADPARAMCOUNT;
        return set_from_variant(&body->state->html, &params->rgvarg[0]);
    }
    return DISP_E_MEMBERNOTFOUND;
}

static HRESULT create_document(REFIID iid, void **out)
{
    Document *document;
    HRESULT hr;
    if(!out) return E_POINTER;
    *out = NULL;
    document = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*document));
    if(!document) return E_OUTOFMEMORY;
    document->IDispatch_iface.lpVtbl = &document_vtbl;
    document->references = 1;
    document->state = renderer_create();
    if(!document->state) {
        HeapFree(GetProcessHeap(), 0, document);
        return E_OUTOFMEMORY;
    }
    hr = document_query_interface(&document->IDispatch_iface, iid, out);
    document_release(&document->IDispatch_iface);
    return hr;
}

static HRESULT STDMETHODCALLTYPE factory_query_interface(IClassFactory *iface, REFIID iid,
                                                          void **out)
{
    if(!out) return E_POINTER;
    *out = NULL;
    if(IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IClassFactory)) {
        *out = iface;
        IClassFactory_AddRef(iface);
        return S_OK;
    }
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE factory_add_ref(IClassFactory *iface)
{
    ClassFactory *factory = (ClassFactory *)iface;
    return (ULONG)InterlockedIncrement(&factory->references);
}

static ULONG STDMETHODCALLTYPE factory_release(IClassFactory *iface)
{
    ClassFactory *factory = (ClassFactory *)iface;
    LONG references = InterlockedDecrement(&factory->references);
    if(references == 0) HeapFree(GetProcessHeap(), 0, factory);
    return (ULONG)references;
}

static HRESULT STDMETHODCALLTYPE factory_create_instance(IClassFactory *iface, IUnknown *outer,
                                                         REFIID iid, void **out)
{
    (void)iface;
    if(outer) return CLASS_E_NOAGGREGATION;
    return create_document(iid, out);
}

static HRESULT STDMETHODCALLTYPE factory_lock_server(IClassFactory *iface, BOOL lock)
{
    (void)iface;
    (void)lock;
    return S_OK;
}

static IClassFactoryVtbl factory_vtbl = {
    factory_query_interface, factory_add_ref, factory_release,
    factory_create_instance, factory_lock_server,
};

__declspec(dllexport) HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void **out)
{
    ClassFactory *factory;
    HRESULT hr;
    if(!IsEqualCLSID(clsid, &CLSID_TritonCefHtmlDocument)) return CLASS_E_CLASSNOTAVAILABLE;
    factory = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*factory));
    if(!factory) return E_OUTOFMEMORY;
    factory->IClassFactory_iface.lpVtbl = &factory_vtbl;
    factory->references = 1;
    hr = factory_query_interface(&factory->IClassFactory_iface, iid, out);
    factory_release(&factory->IClassFactory_iface);
    return hr;
}

__declspec(dllexport) HRESULT WINAPI DllCanUnloadNow(void)
{
    return S_OK;
}

static HRESULT set_registry_string(HKEY key, const WCHAR *name, const WCHAR *value)
{
    LONG error = RegSetValueExW(key, name, 0, REG_SZ, (const BYTE *)value,
                                (DWORD)((lstrlenW(value) + 1) * sizeof(WCHAR)));
    return error == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(error);
}

__declspec(dllexport) HRESULT WINAPI DllRegisterServer(void)
{
    WCHAR guid[40], path[256], module[MAX_PATH];
    HKEY key = NULL, server_key = NULL, progid_key = NULL, progid_clsid_key = NULL;
    LONG error;
    HRESULT hr;

    if(!StringFromGUID2(&CLSID_TritonCefHtmlDocument, guid, ARRAYSIZE(guid))) return E_FAIL;
    if(!GetModuleFileNameW(module_handle, module, ARRAYSIZE(module))) return HRESULT_FROM_WIN32(GetLastError());
    lstrcpyW(path, L"Software\\Classes\\CLSID\\");
    lstrcatW(path, guid);
    error = RegCreateKeyExW(HKEY_LOCAL_MACHINE, path, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL);
    if(error != ERROR_SUCCESS) return HRESULT_FROM_WIN32(error);
    hr = set_registry_string(key, NULL, triton_description);
    if(FAILED(hr)) goto done;
    error = RegCreateKeyExW(key, L"InprocServer32", 0, NULL, 0, KEY_WRITE, NULL, &server_key, NULL);
    if(error != ERROR_SUCCESS) { hr = HRESULT_FROM_WIN32(error); goto done; }
    hr = set_registry_string(server_key, NULL, module);
    if(SUCCEEDED(hr)) hr = set_registry_string(server_key, L"ThreadingModel", L"Apartment");
    if(FAILED(hr)) goto done;
    lstrcpyW(path, L"Software\\Classes\\");
    lstrcatW(path, triton_progid);
    error = RegCreateKeyExW(HKEY_LOCAL_MACHINE, path, 0, NULL, 0, KEY_WRITE, NULL, &progid_key, NULL);
    if(error != ERROR_SUCCESS) { hr = HRESULT_FROM_WIN32(error); goto done; }
    hr = set_registry_string(progid_key, NULL, triton_description);
    if(FAILED(hr)) goto done;
    error = RegCreateKeyExW(progid_key, L"CLSID", 0, NULL, 0, KEY_WRITE, NULL, &progid_clsid_key, NULL);
    if(error != ERROR_SUCCESS) { hr = HRESULT_FROM_WIN32(error); goto done; }
    hr = set_registry_string(progid_clsid_key, NULL, guid);
done:
    if(progid_clsid_key) RegCloseKey(progid_clsid_key);
    if(progid_key) RegCloseKey(progid_key);
    if(server_key) RegCloseKey(server_key);
    if(key) RegCloseKey(key);
    return hr;
}

__declspec(dllexport) HRESULT WINAPI DllUnregisterServer(void)
{
    WCHAR guid[40], path[256];
    LONG error;
    if(!StringFromGUID2(&CLSID_TritonCefHtmlDocument, guid, ARRAYSIZE(guid))) return E_FAIL;
    lstrcpyW(path, L"Software\\Classes\\CLSID\\");
    lstrcatW(path, guid);
    error = RegDeleteTreeW(HKEY_LOCAL_MACHINE, path);
    if(error != ERROR_SUCCESS && error != ERROR_FILE_NOT_FOUND) return HRESULT_FROM_WIN32(error);
    lstrcpyW(path, L"Software\\Classes\\");
    lstrcatW(path, triton_progid);
    error = RegDeleteTreeW(HKEY_LOCAL_MACHINE, path);
    return (error == ERROR_SUCCESS || error == ERROR_FILE_NOT_FOUND) ? S_OK : HRESULT_FROM_WIN32(error);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if(reason == DLL_PROCESS_ATTACH) {
        module_handle = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
