#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <cguid.h>
#include <mshtml.h>
#include <ocidl.h>
#include <stdio.h>
#include <wchar.h>

typedef HRESULT (WINAPI *GetClass)(REFCLSID, REFIID, void **);
typedef HRESULT (WINAPI *CanUnload)(void);
#define CHECK(x) do { if(!(x)) { printf("FAIL line=%d: %s\n", __LINE__, #x); goto done; } } while(0)
#define OK(x) do { HRESULT check_hr = (x); if(check_hr != S_OK) { printf("FAIL line=%d hr=%08lx: %s\n", __LINE__, (unsigned long)check_hr, #x); goto done; } } while(0)

static HRESULT lookup(IHTMLDocument3 *document, LPCWSTR name, IHTMLElement **out)
{
    BSTR value = SysAllocString(name);
    HRESULT hr = value ? IHTMLDocument3_getElementById(document, value, out) : E_OUTOFMEMORY;
    SysFreeString(value); return hr;
}

typedef HRESULT (STDMETHODCALLTYPE *StringPut)(IHTMLElement *, BSTR);
static HRESULT put_text(IHTMLElement *element, StringPut put, LPCWSTR text)
{
    BSTR value = SysAllocString(text);
    HRESULT hr = value ? put(element, value) : E_OUTOFMEMORY;
    SysFreeString(value); return hr;
}

int main(void)
{
    HMODULE module = NULL;
    IClassFactory *factory = NULL;
    IHTMLDocument2 *document = NULL;
    IHTMLDocument3 *document3 = NULL;
    IPersistStreamInit *persist = NULL;
    IOleObject *ole = NULL;
    IHTMLElement *elements[12] = {0}, *missing = NULL;
    IDispatch *owner = NULL;
    IUnknown *identity = NULL, *other = NULL;
    GetClass get_class;
    CanUnload can_unload = NULL;
    BSTR text = NULL, argument = NULL;
    unsigned i;
    int status = 1;
    static const WCHAR plain[] = L"<script>not code</script> \x4e16\x754c \xd83d\xde80";
    OK(CoInitialize(NULL));
    module = LoadLibraryW(L"C:\\TritonSupermiumBridge\\triton-ie7-mshtml-activation-probe.dll");
    CHECK(module);
    get_class = (GetClass)(void *)GetProcAddress(module, "DllGetClassObject");
    can_unload = (CanUnload)(void *)GetProcAddress(module, "DllCanUnloadNow");
    CHECK(get_class && can_unload && can_unload() == S_OK);
    OK(get_class(&CLSID_HTMLDocument, &IID_IClassFactory, (void **)&factory));
    OK(IClassFactory_CreateInstance(factory, NULL, &IID_IHTMLDocument2, (void **)&document));
    OK(IHTMLDocument2_QueryInterface(document, &IID_IHTMLDocument3, (void **)&document3));
    OK(IHTMLDocument2_QueryInterface(document, &IID_IPersistStreamInit, (void **)&persist));
    OK(IPersistStreamInit_InitNew(persist));
    CHECK(IHTMLDocument2_get_body(document, NULL) == E_POINTER);
    OK(IHTMLDocument2_get_body(document, &elements[0]));
    OK(IHTMLDocument2_get_body(document, &elements[1]));
    CHECK(elements[0] && elements[0] == elements[1]);
    OK(IHTMLDocument3_get_documentElement(document3, &elements[2]));
    OK(IHTMLElement_get_parentElement(elements[0], &elements[3]));
    CHECK(elements[2] && elements[2] == elements[3]);
    OK(IHTMLElement_get_document(elements[0], &owner));
    OK(IDispatch_QueryInterface(owner, &IID_IUnknown, (void **)&identity));
    OK(IHTMLDocument2_QueryInterface(document, &IID_IUnknown, (void **)&other));
    CHECK(identity == other);
    IUnknown_Release(identity); identity = NULL; IUnknown_Release(other); other = NULL;
    IDispatch_Release(owner); owner = NULL;
    OK(IHTMLElement_QueryInterface(elements[0], &IID_IUnknown, (void **)&identity));
    OK(IHTMLElement_QueryInterface(elements[1], &IID_IDispatch, (void **)&owner));
    OK(IDispatch_QueryInterface(owner, &IID_IUnknown, (void **)&other));
    CHECK(identity == other);
    IUnknown_Release(identity); identity = NULL; IUnknown_Release(other); other = NULL;
    IDispatch_Release(owner); owner = NULL;
    puts("MSHTML ELEMENT AND DOCUMENT COM IDENTITY PASSED");
    argument = SysAllocStringLen(NULL, 20000); CHECK(argument);
    for(i = 0; i < 20000; ++i) argument[i] = L'x';
    OK(IHTMLElement_put_className(elements[0], argument));
    SysFreeString(argument); argument = NULL;
    OK(IHTMLDocument2_get_body(document, &elements[11])); CHECK(elements[11] == elements[0]);
    OK(put_text(elements[0], elements[0]->lpVtbl->put_className, L""));
    puts("MSHTML LARGE ATTRIBUTE LOOKUP IDENTITY PASSED");

    OK(put_text(elements[0], elements[0]->lpVtbl->put_innerHTML,
                L"<section id='node'><b>alpha</b></section><p id='keep'>unchanged</p>"));
    OK(lookup(document3, L"node", &elements[4]));
    OK(lookup(document3, L"node", &elements[5]));
    CHECK(elements[4] && elements[4] == elements[5]);
    OK(lookup(document3, L"missing", &missing)); CHECK(!missing);
    OK(IHTMLElement_get_tagName(elements[4], &text)); CHECK(!lstrcmpW(text, L"SECTION"));
    SysFreeString(text); text = NULL;
    OK(put_text(elements[4], elements[4]->lpVtbl->put_className, L"first second"));
    OK(IHTMLElement_get_className(elements[4], &text)); CHECK(!lstrcmpW(text, L"first second"));
    SysFreeString(text); text = NULL;
    OK(put_text(elements[4], elements[4]->lpVtbl->put_innerText, plain));
    OK(IHTMLElement_get_innerText(elements[4], &text)); CHECK(!lstrcmpW(text, plain));
    SysFreeString(text); text = NULL;
    OK(IHTMLElement_get_innerHTML(elements[4], &text)); CHECK(wcsstr(text, L"&lt;script&gt;not code&lt;/script&gt;"));
    SysFreeString(text); text = NULL;
    OK(put_text(elements[4], elements[4]->lpVtbl->put_outerHTML, L"<p id='replacement'>new node</p>"));
    OK(lookup(document3, L"node", &missing)); CHECK(!missing);
    OK(lookup(document3, L"replacement", &elements[7])); CHECK(elements[7] != elements[4]);
    OK(IHTMLElement_get_innerText(elements[4], &text)); CHECK(!lstrcmpW(text, plain));
    SysFreeString(text); text = NULL;
    OK(IHTMLElement_get_parentElement(elements[4], &missing)); CHECK(!missing);
    argument = SysAllocString(L"article"); CHECK(argument);
    OK(IHTMLDocument2_createElement(document, argument, &elements[6]));
    SysFreeString(argument); argument = NULL;
    OK(put_text(elements[6], elements[6]->lpVtbl->put_innerText, L"detached"));
    OK(IHTMLElement_get_parentElement(elements[6], &missing)); CHECK(!missing);
    puts("MSHTML LIVE DOM MUTATION AND DETACHED NODE IDENTITY PASSED");

    argument = SysAllocString(L"about:blank#same-document"); CHECK(argument);
    OK(IHTMLDocument2_put_URL(document, argument));
    SysFreeString(argument); argument = NULL;
    OK(lookup(document3, L"replacement", &elements[8])); CHECK(elements[8] == elements[7]);
    argument = SysAllocString(L"data:text/html,<title>new-context</title><p id='fresh'>untouched</p>"); CHECK(argument);
    OK(IHTMLDocument2_put_URL(document, argument));
    SysFreeString(argument); argument = NULL;
    for(i = 0; i < 50; ++i) {
        BOOL ready;
        HRESULT hr = IHTMLDocument2_get_title(document, &text);
        ready = SUCCEEDED(hr) && !lstrcmpW(text, L"new-context");
        SysFreeString(text); text = NULL;
        if(ready) break;
        Sleep(100);
    }
    CHECK(i < 50);
    CHECK(FAILED(IHTMLElement_get_innerHTML(elements[0], &text)) && !text);
    CHECK(FAILED(put_text(elements[4], elements[4]->lpVtbl->put_innerText, L"wrong-page-write")));
    OK(lookup(document3, L"fresh", &elements[9])); CHECK(elements[9]);
    OK(IHTMLElement_get_innerText(elements[9], &text)); CHECK(!lstrcmpW(text, L"untouched"));
    SysFreeString(text); text = NULL;
    OK(IHTMLDocument2_QueryInterface(document, &IID_IOleObject, (void **)&ole));
    OK(IOleObject_Close(ole, OLECLOSE_NOSAVE));
    CHECK(IHTMLElement_get_innerText(elements[9], &text) == CO_E_OBJNOTCONNECTED && !text);
    OK(IHTMLDocument2_get_body(document, &elements[10])); CHECK(elements[10]);
    CHECK(FAILED(put_text(elements[9], elements[9]->lpVtbl->put_innerText, L"wrong-reopened-page")));
    puts("MSHTML NAVIGATION AND REOPEN STALE HANDLE ISOLATION PASSED");

    IOleObject_Release(ole); ole = NULL;
    IPersistStreamInit_Release(persist); persist = NULL;
    IHTMLDocument3_Release(document3); document3 = NULL;
    IHTMLDocument2_Release(document); document = NULL;
    IClassFactory_Release(factory); factory = NULL;
    CHECK(can_unload() == S_FALSE);
    OK(IHTMLElement_get_tagName(elements[10], &text)); CHECK(!lstrcmpW(text, L"BODY"));
    SysFreeString(text); text = NULL;
    for(i = 0; i < ARRAYSIZE(elements); ++i) if(elements[i]) {
        IHTMLElement_Release(elements[i]); elements[i] = NULL;
    }
    CHECK(can_unload() == S_OK);
    puts("MSHTML ELEMENT PROXIES PASSED");
    status = 0;
done:
    SysFreeString(text); SysFreeString(argument);
    if(identity) IUnknown_Release(identity);
    if(other) IUnknown_Release(other);
    if(owner) IDispatch_Release(owner);
    if(missing) IHTMLElement_Release(missing);
    for(i = 0; i < ARRAYSIZE(elements); ++i) if(elements[i]) IHTMLElement_Release(elements[i]);
    if(ole) IOleObject_Release(ole);
    if(persist) IPersistStreamInit_Release(persist);
    if(document3) IHTMLDocument3_Release(document3);
    if(document) IHTMLDocument2_Release(document);
    if(factory) IClassFactory_Release(factory);
    if(module && can_unload && can_unload() == S_OK) FreeLibrary(module);
    CoUninitialize(); return status;
}
