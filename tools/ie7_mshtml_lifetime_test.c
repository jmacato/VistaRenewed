#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <cguid.h>
#include <mshtml.h>
#include <docobj.h>
#include <urlmon.h>
#include <stdio.h>

typedef HRESULT (WINAPI *GetClass)(REFCLSID, REFIID, void **);
typedef HRESULT (WINAPI *CanUnload)(void);
#define CHECK(test) do { if(!(test)) { printf("FAIL line=%d\n", __LINE__); return 1; } } while(0)
int main(void)
{
    HMODULE module;
    GetClass get_class;
    CanUnload can_unload;
    IClassFactory *factory = NULL;
    IHTMLDocument2 *document = NULL;
    IHTMLDocument2 *second = NULL;
    BSTR url = NULL;
    IDispatch *dispatch = NULL;
    LPOLESTR name = L"URL";
    DISPID id;
    DISPPARAMS params = {0};
    VARIANT result;
    VARIANT argument;
    DISPID put_id = DISPID_PROPERTYPUT;
    UINT bad_argument = 99;
    unsigned attempt;
    IOleCommandTarget *commands = NULL;
    IOleCommandTarget *second_commands = NULL;
    IOleObject *ole = NULL;
    OLECMD refresh = {OLECMDID_REFRESH, 0};
    IPersistMoniker *persist = NULL;
    IMoniker *source = NULL, *current = NULL;
    CHECK(SUCCEEDED(CoInitialize(NULL)));
    module = LoadLibraryW(L"C:\\TritonSupermiumBridge\\triton-ie7-mshtml-activation-probe.dll");
    CHECK(module != NULL);
    get_class = (GetClass)(void *)GetProcAddress(module, "DllGetClassObject");
    can_unload = (CanUnload)(void *)GetProcAddress(module, "DllCanUnloadNow");
    CHECK(get_class && can_unload);
    CHECK(can_unload() == S_OK);
    CHECK(get_class(&CLSID_HTMLDocument, &IID_IClassFactory, (void **)&factory) == S_OK);
    CHECK(can_unload() == S_FALSE);
    CHECK(IClassFactory_LockServer(factory, TRUE) == S_OK);
    CHECK(IClassFactory_CreateInstance(factory, NULL, &IID_IHTMLDocument2, (void **)&document) == S_OK);
    CHECK(IHTMLDocument2_QueryInterface(document, &IID_IPersistMoniker, (void **)&persist) == S_OK);
    CHECK(IPersistMoniker_GetCurMoniker(persist, NULL) == E_INVALIDARG);
    CHECK(CreateURLMoniker(NULL, L"about:blank", &source) == S_OK);
    CHECK(IPersistMoniker_Load(persist, TRUE, source, NULL, STGM_READ) == S_OK);
    CHECK(IPersistMoniker_GetCurMoniker(persist, &current) == S_OK);
    CHECK(current == source);
    IMoniker_Release(current);
    IMoniker_Release(source);
    IPersistMoniker_Release(persist);
    puts("MSHTML LOADED MONIKER IDENTITY PASSED");
    CHECK(IHTMLDocument2_get_URL(document, &url) == S_OK);
    CHECK(lstrcmpW(url, L"about:blank") == 0);
    SysFreeString(url);
    CHECK(IHTMLDocument2_QueryInterface(document, &IID_IDispatch, (void **)&dispatch) == S_OK);
    CHECK(IDispatch_GetIDsOfNames(dispatch, &IID_NULL, &name, 1, 0, &id) == S_OK);
    VariantInit(&result);
    CHECK(IDispatch_Invoke(dispatch, id, &IID_NULL, 0, DISPATCH_PROPERTYGET,
                          &params, &result, NULL, NULL) == S_OK);
    CHECK(result.vt == VT_BSTR && !lstrcmpW(result.bstrVal, L"about:blank"));
    VariantClear(&result);
    CHECK(IDispatch_Invoke(dispatch, id, &IID_NULL, 0, DISPATCH_PROPERTYPUT,
                          &params, NULL, NULL, NULL) == DISP_E_BADPARAMCOUNT);
    VariantInit(&argument);
    argument.vt = VT_I4;
    argument.lVal = 42;
    params.cArgs = params.cNamedArgs = 1;
    params.rgvarg = &argument;
    params.rgdispidNamedArgs = &put_id;
    CHECK(IDispatch_Invoke(dispatch, id, &IID_NULL, 0, DISPATCH_PROPERTYPUT,
                          &params, NULL, NULL, &bad_argument) == DISP_E_TYPEMISMATCH);
    CHECK(bad_argument == 0);
    argument.vt = VT_BSTR;
    argument.bstrVal = SysAllocString(L"about:blank#automation");
    CHECK(argument.bstrVal != NULL);
    CHECK(IDispatch_Invoke(dispatch, id, &IID_NULL, 0, DISPATCH_PROPERTYPUT,
                          &params, NULL, NULL, NULL) == S_OK);
    VariantClear(&argument);
    ZeroMemory(&params, sizeof(params));
    CHECK(IHTMLDocument2_get_URL(document, &url) == S_OK);
    CHECK(!lstrcmpW(url, L"about:blank#automation"));
    SysFreeString(url);
    puts("MSHTML DISPATCH URL WRITE AND VALIDATION PASSED");
    CHECK(IHTMLDocument2_get_readyState(document, &url) == S_OK);
    CHECK(url && (!lstrcmpW(url, L"loading") || !lstrcmpW(url, L"interactive") ||
                  !lstrcmpW(url, L"complete")));
    SysFreeString(url);
    puts("MSHTML INITIAL RENDERER READYSTATE PASSED");
    Sleep(1500);
    url = SysAllocString(L"about:blank#second-navigation");
    CHECK(url != NULL);
    CHECK(IHTMLDocument2_put_URL(document, url) == S_OK);
    SysFreeString(url);
    CHECK(IHTMLDocument2_get_URL(document, &url) == S_OK);
    CHECK(!lstrcmpW(url, L"about:blank#second-navigation"));
    SysFreeString(url);
    puts("MSHTML PERSISTENT SECOND NAVIGATION PASSED");
    name = L"readyState";
    CHECK(IHTMLDocument2_GetIDsOfNames(document, &IID_NULL, &name, 1, 0, &id) == S_OK);
    for(attempt = 0; attempt < 50; ++attempt) {
        BOOL complete;
        CHECK(IHTMLDocument2_Invoke(document, id, &IID_NULL, 0, DISPATCH_PROPERTYGET,
                                   &params, &result, NULL, NULL) == S_OK);
        CHECK(result.vt == VT_BSTR);
        complete = !lstrcmpW(result.bstrVal, L"complete");
        VariantClear(&result);
        if(complete) break;
        Sleep(100);
    }
    CHECK(attempt < 50);
    CHECK(IHTMLDocument2_QueryInterface(document, &IID_IOleCommandTarget, (void **)&commands) == S_OK);
    CHECK(IOleCommandTarget_QueryStatus(commands, NULL, 1, &refresh, NULL) == S_OK);
    CHECK(refresh.cmdf == (OLECMDF_SUPPORTED | OLECMDF_ENABLED));
    CHECK(IOleCommandTarget_Exec(commands, &IID_IUnknown, OLECMDID_REFRESH, 0, NULL, NULL)
          == OLECMDERR_E_UNKNOWNGROUP);
    CHECK(IOleCommandTarget_Exec(commands, NULL, OLECMDID_PRINT, 0, NULL, NULL)
          == OLECMDERR_E_NOTSUPPORTED);
    CHECK(IOleCommandTarget_Exec(commands, NULL, OLECMDID_REFRESH, 0, NULL, NULL) == S_OK);
    CHECK(IOleCommandTarget_Exec(commands, NULL, OLECMDID_STOP, 0, NULL, NULL) == S_OK);
    CHECK(IHTMLDocument2_QueryInterface(document, &IID_IOleObject, (void **)&ole) == S_OK);
    CHECK(IOleObject_Close(ole, 99) == E_INVALIDARG);
    CHECK(IOleObject_Close(ole, OLECLOSE_NOSAVE) == S_OK);
    CHECK(IOleObject_Close(ole, OLECLOSE_NOSAVE) == S_OK);
    CHECK(IOleCommandTarget_QueryStatus(commands, NULL, 1, &refresh, NULL) == S_OK);
    CHECK(refresh.cmdf == OLECMDF_SUPPORTED);
    CHECK(IOleCommandTarget_Exec(commands, NULL, OLECMDID_REFRESH, 0, NULL, NULL)
          == OLECMDERR_E_DISABLED);
    IOleObject_Release(ole);
    puts("MSHTML CLOSE IDEMPOTENCE AND DISCONNECTED COMMANDS PASSED");
    url = SysAllocString(L"about:blank#reopened");
    CHECK(url != NULL);
    CHECK(IHTMLDocument2_put_URL(document, url) == S_OK);
    SysFreeString(url);
    CHECK(IOleCommandTarget_QueryStatus(commands, NULL, 1, &refresh, NULL) == S_OK);
    CHECK(refresh.cmdf == (OLECMDF_SUPPORTED | OLECMDF_ENABLED));
    CHECK(IHTMLDocument2_get_URL(document, &url) == S_OK);
    CHECK(!lstrcmpW(url, L"about:blank#reopened"));
    SysFreeString(url);
    puts("MSHTML NAVIGATION AFTER CLOSE PASSED");
    CHECK(IClassFactory_CreateInstance(factory, NULL, &IID_IHTMLDocument2, (void **)&second) == S_OK);
    IHTMLDocument2_Release(second);
    CHECK(IHTMLDocument2_get_readyState(document, &url) == S_OK);
    SysFreeString(url);
    puts("MSHTML UNUSED DOCUMENT RELEASE PRESERVES LIVE RUNTIME PASSED");
    CHECK(IClassFactory_CreateInstance(factory, NULL, &IID_IHTMLDocument2, (void **)&second) == S_OK);
    url = SysAllocString(L"about:blank#independent-document");
    CHECK(url != NULL);
    CHECK(IHTMLDocument2_put_URL(second, url) == S_OK);
    SysFreeString(url);
    CHECK(IHTMLDocument2_get_readyState(document, &url) == S_OK);
    SysFreeString(url);
    CHECK(IHTMLDocument2_QueryInterface(second, &IID_IOleCommandTarget, (void **)&second_commands) == S_OK);
    CHECK(IOleCommandTarget_QueryStatus(second_commands, NULL, 1, &refresh, NULL) == S_OK);
    CHECK(refresh.cmdf == (OLECMDF_SUPPORTED | OLECMDF_ENABLED));
    CHECK(IHTMLDocument2_QueryInterface(second, &IID_IOleObject, (void **)&ole) == S_OK);
    CHECK(IOleObject_Close(ole, OLECLOSE_NOSAVE) == S_OK);
    CHECK(IOleCommandTarget_QueryStatus(second_commands, NULL, 1, &refresh, NULL) == S_OK);
    CHECK(refresh.cmdf == OLECMDF_SUPPORTED);
    CHECK(IOleCommandTarget_Exec(second_commands, NULL, OLECMDID_REFRESH, 0, NULL, NULL) == OLECMDERR_E_DISABLED);
    CHECK(IOleCommandTarget_Exec(second_commands, NULL, OLECMDID_STOP, 0, NULL, NULL) == OLECMDERR_E_DISABLED);
    CHECK(IOleCommandTarget_Exec(commands, NULL, OLECMDID_REFRESH, 0, NULL, NULL) == S_OK);
    IOleObject_Release(ole);
    IOleCommandTarget_Release(second_commands);
    IHTMLDocument2_Release(second);
    CHECK(IHTMLDocument2_get_readyState(document, &url) == S_OK);
    SysFreeString(url);
    puts("MSHTML CLOSED DOCUMENT COMMAND ISOLATION PASSED");
    puts("MSHTML TWO LIVE DOCUMENTS AND INDEPENDENT RELEASE PASSED");
    IOleCommandTarget_Release(commands);
    puts("MSHTML REFRESH AND STOP COMMAND ACKNOWLEDGEMENTS PASSED");
    IDispatch_Release(dispatch);
    puts("MSHTML DISPATCH PROPERTY READS PASSED");
    CHECK(IClassFactory_LockServer(factory, FALSE) == S_OK);
    IClassFactory_Release(factory);
    CHECK(can_unload() == S_FALSE);
    IHTMLDocument2_Release(document);
    CHECK(can_unload() == S_OK);
    FreeLibrary(module);
    CoUninitialize();
    puts("MSHTML LIFETIME AND DIRECT DOCUMENT ACTIVATION PASSED");
    return 0;
}
