#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <cguid.h>
#include <mshtml.h>
#include <mshtmdid.h>
#include <ocidl.h>
#include <stdio.h>
#include <string.h>
#include "mshtml_json.h"

typedef HRESULT (WINAPI *GetClass)(REFCLSID, REFIID, void **);
typedef HRESULT (WINAPI *CanUnload)(void);
#define CHECK(x) do { if(!(x)) { printf("FAIL line=%d: %s\n", __LINE__, #x); goto done; } } while(0)

static HRESULT rewind_stream(IStream *stream)
{
    LARGE_INTEGER zero;
    zero.QuadPart = 0;
    return IStream_Seek(stream, zero, STREAM_SEEK_SET, NULL);
}

static IStream *make_stream(const void *bytes, ULONG size)
{
    IStream *stream = NULL;
    ULONG written;
    if(FAILED(CreateStreamOnHGlobal(NULL, TRUE, &stream))) return NULL;
    if(FAILED(IStream_Write(stream, bytes, size, &written)) || written != size ||
       FAILED(rewind_stream(stream))) { IStream_Release(stream); return NULL; }
    return stream;
}

static BOOL contains_bytes(const void *data, size_t size, const char *needle)
{
    size_t i, length = strlen(needle);
    if(length > size) return FALSE;
    for(i = 0; i <= size - length; ++i)
        if(!memcmp((const char *)data + i, needle, length)) return TRUE;
    return FALSE;
}

static SAFEARRAY *text_array(BSTR first, BSTR second)
{
    SAFEARRAY *array = SafeArrayCreateVector(VT_VARIANT, 7, second ? 2 : 1);
    LONG index = 7;
    VARIANT value;
    if(!array) return NULL;
    VariantInit(&value); value.vt = VT_BSTR; value.bstrVal = first;
    if(FAILED(SafeArrayPutElement(array, &index, &value))) goto fail;
    if(second) {
        ++index; value.bstrVal = second;
        if(FAILED(SafeArrayPutElement(array, &index, &value))) goto fail;
    }
    return array;
fail:
    SafeArrayDestroy(array); return NULL;
}

static int stock_probe(void)
{
    IHTMLDocument2 *document = NULL;
    IPersistStreamInit *persist = NULL;
    IStream *stream = make_stream("<title>stock stream</title>", 27);
    HRESULT hr = CoCreateInstance(&CLSID_HTMLDocument, NULL, CLSCTX_INPROC_SERVER,
                                  &IID_IHTMLDocument2, (void **)&document);
    printf("STOCK_CREATE=%08lx\n", (unsigned long)hr);
    if(document) {
        hr = IHTMLDocument2_QueryInterface(document, &IID_IPersistStreamInit, (void **)&persist);
        printf("STOCK_STREAM=%08lx\n", (unsigned long)hr);
    }
    if(persist) {
        printf("STOCK_INIT=%08lx\n", (unsigned long)IPersistStreamInit_InitNew(persist));
        printf("STOCK_INIT_AGAIN=%08lx\n", (unsigned long)IPersistStreamInit_InitNew(persist));
        if(stream) printf("STOCK_LOAD_AFTER_INIT=%08lx\n", (unsigned long)IPersistStreamInit_Load(persist, stream));
        IPersistStreamInit_Release(persist);
    }
    if(document) IHTMLDocument2_Release(document);
    if(stream) IStream_Release(stream);
    return FAILED(hr);
}

int main(int argc, char **argv)
{
    HMODULE module = NULL;
    IClassFactory *factory = NULL;
    IHTMLDocument2 *document = NULL, *second = NULL;
    IPersistStreamInit *persist = NULL, *second_persist = NULL;
    IUnknown *identity = NULL, *stream_identity = NULL;
    SAFEARRAY *array = NULL;
    IStream *saved = NULL, *input = NULL;
    BSTR first = NULL, latter = NULL, title = NULL;
    VARIANT result, arguments[2];
    DISPPARAMS params = {0};
    DISPID id = 0, put = DISPID_PROPERTYPUT;
    LPOLESTR name;
    UINT bad = 99;
    ULARGE_INTEGER size;
    STATSTG stat;
    HGLOBAL memory;
    void *data;
    unsigned i;
    HRESULT load_hr;
    GetClass get_class;
    CanUnload can_unload = NULL;
    int status = 1;
    static const WCHAR injected_title[] = L"\";throw 42;// \x4e16\x754c \xd83d\xde80";
    static const WCHAR script[] = L"</p><script>const a={n:144};document.title=String(a?.n??0)"
        L"+':'+(window.hit=(window.hit||0)+1)+':'+String.fromCharCode(0x4e16,0x754c);</script>";
    VariantInit(&result); VariantInit(&arguments[0]); VariantInit(&arguments[1]);
    CHECK(SUCCEEDED(CoInitialize(NULL)));
    if(argc == 2 && !strcmp(argv[1], "--stock")) { status = stock_probe(); goto done; }
    CHECK(argc == 1);
    /* Parser controls: nested lookalikes, duplicate fields, broken Unicode,
     * malformed containers, UTF-8, escaped surrogates and embedded BSTR NUL. */
    CHECK(!mj_skip("{\"x\":[1,]}", 0));
    CHECK(!mj_skip("{\"x\":\"\\u12\"}", 0));
    CHECK(!mj_member("{\"result\":1,\"result\":2}", "result"));
    CHECK(!mj_member("{\"x\":{\"error\":1},\"text\":\"error\"}", "error"));
    CHECK(mj_member("{\"error\":1}", "error"));
    CHECK(mj_bstr("\"\xc3\xa9\\u0000\\ud83d\\ude80\"", &title) == S_OK);
    CHECK(SysStringLen(title) == 4 && title[0] == 0xe9 && !title[1] &&
          title[2] == 0xd83d && title[3] == 0xde80);
    SysFreeString(title); title = NULL;
    CHECK(FAILED(mj_bstr("\"\xff\"", &title)) && !title);
    puts("MSHTML JSON STRING AND NEGATIVE CONTROLS PASSED");

    module = LoadLibraryW(L"C:\\TritonSupermiumBridge\\triton-ie7-mshtml-activation-probe.dll");
    CHECK(module);
    get_class = (GetClass)(void *)GetProcAddress(module, "DllGetClassObject");
    can_unload = (CanUnload)(void *)GetProcAddress(module, "DllCanUnloadNow");
    CHECK(get_class && can_unload && can_unload() == S_OK);
    CHECK(get_class(&CLSID_HTMLDocument, &IID_IClassFactory, (void **)&factory) == S_OK);
    CHECK(IClassFactory_CreateInstance(factory, NULL, &IID_IHTMLDocument2, (void **)&document) == S_OK);
    CHECK(IHTMLDocument2_QueryInterface(document, &IID_IPersistStreamInit, (void **)&persist) == S_OK);
    CHECK(IHTMLDocument2_QueryInterface(document, &IID_IUnknown, (void **)&identity) == S_OK);
    CHECK(IPersistStreamInit_QueryInterface(persist, &IID_IUnknown, (void **)&stream_identity) == S_OK);
    CHECK(identity == stream_identity);
    IUnknown_Release(identity); identity = NULL;
    IUnknown_Release(stream_identity); stream_identity = NULL;
    CHECK(IPersistStreamInit_InitNew(persist) == S_OK);
    CHECK(IPersistStreamInit_IsDirty(persist) == S_FALSE);
    CHECK(IHTMLDocument2_get_title(document, NULL) == E_POINTER);
    CHECK(IHTMLDocument2_write(document, NULL) == E_INVALIDARG);
    array = SafeArrayCreateVector(VT_BSTR, 0, 1);
    CHECK(array && IHTMLDocument2_write(document, array) == E_INVALIDARG);
    SafeArrayDestroy(array); array = NULL;

    /* Larger than the old 14 KiB request / 8 KiB reply; non-zero SAFEARRAY base. */
    first = SysAllocStringLen(NULL, 100000);
    CHECK(first);
    memcpy(first, L"<!doctype html><p>", 18 * sizeof(WCHAR));
    for(i = 18; i < 100000; ++i) first[i] = (i % 2) ? L'A' : (WCHAR)0x4e16;
    latter = SysAllocString(script);
    CHECK(latter);
    array = text_array(first, latter);
    CHECK(array && IHTMLDocument2_write(document, array) == S_OK);
    SafeArrayDestroy(array); array = NULL;
    SysFreeString(first); first = NULL; SysFreeString(latter); latter = NULL;
    name = L"writeln";
    CHECK(IHTMLDocument2_GetIDsOfNames(document, &IID_NULL, &name, 1, 0, &id) == S_OK);
    arguments[0].vt = VT_BSTR; arguments[0].bstrVal = SysAllocString(L"</span>");
    arguments[1].vt = VT_BSTR; arguments[1].bstrVal = SysAllocString(L"<span>dispatch-order");
    CHECK(arguments[0].bstrVal && arguments[1].bstrVal);
    params.rgvarg = arguments; params.cArgs = 2;
    CHECK(IHTMLDocument2_Invoke(document, id, &IID_NULL, 0, DISPATCH_METHOD, &params, NULL, NULL, NULL) == S_OK);
    VariantClear(&arguments[0]); VariantClear(&arguments[1]); ZeroMemory(&params, sizeof(params));
    CHECK(IHTMLDocument2_close(document) == S_OK);
    CHECK(IHTMLDocument2_get_title(document, &title) == S_OK);
    CHECK(!lstrcmpW(title, L"144:1:\x4e16\x754c"));
    SysFreeString(title); title = NULL;
    CHECK(IPersistStreamInit_IsDirty(persist) == S_OK);
    CHECK(CreateStreamOnHGlobal(NULL, TRUE, &saved) == S_OK);
    CHECK(IPersistStreamInit_GetSizeMax(persist, &size) == S_OK);
    CHECK(IPersistStreamInit_Save(persist, saved, TRUE) == S_OK);
    CHECK(IStream_Stat(saved, &stat, STATFLAG_NONAME) == S_OK && size.QuadPart >= stat.cbSize.QuadPart);
    CHECK(stat.cbSize.QuadPart > 100000);
    CHECK(GetHGlobalFromStream(saved, &memory) == S_OK);
    data = GlobalLock(memory);
    CHECK(data && !memcmp(data, "\xef\xbb\xbf", 3));
    CHECK(contains_bytes(data, (size_t)stat.cbSize.QuadPart, "<span>dispatch-order</span>\n"));
    GlobalUnlock(memory);
    CHECK(IPersistStreamInit_IsDirty(persist) == S_FALSE);
    puts("MSHTML LARGE UNICODE WRITE SCRIPT AND STREAM SAVE PASSED");

    name = L"title";
    CHECK(IHTMLDocument2_GetIDsOfNames(document, &IID_NULL, &name, 1, 0, &id) == S_OK);
    arguments[0].vt = VT_I4; arguments[0].lVal = 42;
    params.rgvarg = arguments; params.cArgs = params.cNamedArgs = 1; params.rgdispidNamedArgs = &put;
    CHECK(IHTMLDocument2_Invoke(document, id, &IID_NULL, 0, DISPATCH_PROPERTYPUT, &params, NULL, NULL, &bad)
          == DISP_E_TYPEMISMATCH && bad == 0);
    arguments[0].vt = VT_BSTR; arguments[0].bstrVal = SysAllocString(injected_title);
    CHECK(arguments[0].bstrVal);
    CHECK(IHTMLDocument2_Invoke(document, id, &IID_NULL, 0, DISPATCH_PROPERTYPUT, &params, NULL, NULL, NULL) == S_OK);
    VariantClear(&arguments[0]); ZeroMemory(&params, sizeof(params));
    CHECK(IHTMLDocument2_Invoke(document, id, &IID_NULL, 0, DISPATCH_PROPERTYGET, &params, &result, NULL, NULL) == S_OK);
    CHECK(result.vt == VT_BSTR && !lstrcmpW(result.bstrVal, injected_title));
    VariantClear(&result);
    first = SysAllocStringLen(NULL, 1024 * 1024 + 1);
    CHECK(first && IHTMLDocument2_put_title(document, first) == E_INVALIDARG);
    SysFreeString(first); first = NULL;
    puts("MSHTML TITLE AUTOMATION UNICODE AND INJECTION REJECTION PASSED");

    CHECK(IClassFactory_CreateInstance(factory, NULL, &IID_IHTMLDocument2, (void **)&second) == S_OK);
    CHECK(IHTMLDocument2_QueryInterface(second, &IID_IPersistStreamInit, (void **)&second_persist) == S_OK);
    CHECK(rewind_stream(saved) == S_OK);
    load_hr = IPersistStreamInit_Load(second_persist, saved);
    printf("SECOND_DOCUMENT_STREAM_LOAD=%08lx\n", (unsigned long)load_hr);
    CHECK(load_hr == S_OK);
    CHECK(IHTMLDocument2_get_title(second, &title) == S_OK && !lstrcmpW(title, L"144:1:\x4e16\x754c"));
    SysFreeString(title); title = NULL;
    CHECK(IHTMLDocument2_get_title(document, &title) == S_OK && !lstrcmpW(title, injected_title));
    SysFreeString(title); title = NULL;
    CHECK(IPersistStreamInit_IsDirty(second_persist) == S_FALSE);
    input = make_stream("\xff", 1);
    CHECK(input && FAILED(IPersistStreamInit_Load(second_persist, input)));
    IStream_Release(input); input = NULL;
    CHECK(IHTMLDocument2_get_title(second, &title) == S_OK && !lstrcmpW(title, L"144:1:\x4e16\x754c"));
    SysFreeString(title); title = NULL;
    first = SysAllocString(L"\xfeff<title>UTF16 \x4e16\x754c</title><p>stream</p>");
    CHECK(first);
    input = make_stream(first, SysStringByteLen(first));
    CHECK(input && IPersistStreamInit_Load(second_persist, input) == S_OK);
    IStream_Release(input); input = NULL;
    SysFreeString(first); first = NULL;
    CHECK(IHTMLDocument2_get_title(second, &title) == S_OK && !lstrcmpW(title, L"UTF16 \x4e16\x754c"));
    SysFreeString(title); title = NULL;
    puts("MSHTML STREAM RELOAD ENCODING AND DOCUMENT ISOLATION PASSED");

    IPersistStreamInit_Release(second_persist); second_persist = NULL;
    IHTMLDocument2_Release(second); second = NULL;
    IPersistStreamInit_Release(persist); persist = NULL;
    IHTMLDocument2_Release(document); document = NULL;
    IClassFactory_Release(factory); factory = NULL;
    CHECK(can_unload() == S_OK);
    puts("MSHTML DOCUMENT CONTENT PASSED");
    status = 0;
done:
    if(identity) IUnknown_Release(identity);
    if(stream_identity) IUnknown_Release(stream_identity);
    if(array) SafeArrayDestroy(array);
    if(input) IStream_Release(input);
    if(saved) IStream_Release(saved);
    if(second_persist) IPersistStreamInit_Release(second_persist);
    if(second) IHTMLDocument2_Release(second);
    if(persist) IPersistStreamInit_Release(persist);
    if(document) IHTMLDocument2_Release(document);
    if(factory) IClassFactory_Release(factory);
    SysFreeString(first); SysFreeString(latter); SysFreeString(title);
    VariantClear(&result); VariantClear(&arguments[0]); VariantClear(&arguments[1]);
    if(module && can_unload && can_unload() == S_OK) FreeLibrary(module);
    CoUninitialize();
    return status;
}
