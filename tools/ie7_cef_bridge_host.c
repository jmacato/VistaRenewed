/* Late-bound target test for the private IE7 CEF bridge facade. */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <objbase.h>
#include <oaidl.h>
#include <oleauto.h>
#include <stdio.h>
#include <wchar.h>

static const CLSID CLSID_TritonCefHtmlDocument =
    {0xa9e4d11a, 0x3f5c, 0x4e8a, {0x9c, 0x77, 0x93, 0x71, 0x2b, 0xc6, 0x48, 0x02}};
static const IID triton_iid_null = {0, 0, 0, {0, 0, 0, 0, 0, 0, 0, 0}};

static HRESULT dispid_for(IDispatch *object, const WCHAR *name, DISPID *id)
{
    LPOLESTR names[] = {(LPOLESTR)name};
    return IDispatch_GetIDsOfNames(object, &triton_iid_null, names, 1, LOCALE_SYSTEM_DEFAULT, id);
}

static HRESULT invoke_no_args(IDispatch *object, const WCHAR *name)
{
    DISPID id;
    DISPPARAMS parameters;
    HRESULT hr = dispid_for(object, name, &id);
    if(FAILED(hr)) return hr;
    ZeroMemory(&parameters, sizeof(parameters));
    return IDispatch_Invoke(object, id, &triton_iid_null, LOCALE_SYSTEM_DEFAULT, DISPATCH_METHOD,
                            &parameters, NULL, NULL, NULL);
}

static HRESULT invoke_bstr_method(IDispatch *object, const WCHAR *name, const WCHAR *text)
{
    DISPID id;
    DISPPARAMS parameters;
    VARIANT argument;
    HRESULT hr = dispid_for(object, name, &id);
    if(FAILED(hr)) return hr;
    VariantInit(&argument);
    argument.vt = VT_BSTR;
    argument.bstrVal = SysAllocString(text);
    if(!argument.bstrVal) return E_OUTOFMEMORY;
    ZeroMemory(&parameters, sizeof(parameters));
    parameters.rgvarg = &argument;
    parameters.cArgs = 1;
    hr = IDispatch_Invoke(object, id, &triton_iid_null, LOCALE_SYSTEM_DEFAULT, DISPATCH_METHOD,
                          &parameters, NULL, NULL, NULL);
    VariantClear(&argument);
    return hr;
}

static HRESULT put_bstr(IDispatch *object, const WCHAR *name, const WCHAR *text)
{
    DISPID id, put = DISPID_PROPERTYPUT;
    DISPPARAMS parameters;
    VARIANT argument;
    HRESULT hr = dispid_for(object, name, &id);
    if(FAILED(hr)) return hr;
    VariantInit(&argument);
    argument.vt = VT_BSTR;
    argument.bstrVal = SysAllocString(text);
    if(!argument.bstrVal) return E_OUTOFMEMORY;
    ZeroMemory(&parameters, sizeof(parameters));
    parameters.rgvarg = &argument;
    parameters.rgdispidNamedArgs = &put;
    parameters.cArgs = 1;
    parameters.cNamedArgs = 1;
    hr = IDispatch_Invoke(object, id, &triton_iid_null, LOCALE_SYSTEM_DEFAULT, DISPATCH_PROPERTYPUT,
                          &parameters, NULL, NULL, NULL);
    VariantClear(&argument);
    return hr;
}

static HRESULT get_value(IDispatch *object, const WCHAR *name, VARIANT *result)
{
    DISPID id;
    DISPPARAMS parameters;
    HRESULT hr = dispid_for(object, name, &id);
    if(FAILED(hr)) return hr;
    ZeroMemory(&parameters, sizeof(parameters));
    VariantInit(result);
    return IDispatch_Invoke(object, id, &triton_iid_null, LOCALE_SYSTEM_DEFAULT, DISPATCH_PROPERTYGET,
                            &parameters, result, NULL, NULL);
}

static int expect_bstr(const VARIANT *value, const WCHAR *expected)
{
    return value->vt == VT_BSTR && value->bstrVal && !wcscmp(value->bstrVal, expected);
}

int wmain(int argc, wchar_t **argv)
{
    IDispatch *document = NULL, *body = NULL;
    VARIANT active_x_policy, backend, url, ready_state, title, body_html;
    HRESULT hr;
    int status = 1;
    BOOL initialized = FALSE;
    const WCHAR *expected_backend = L"mock-cef-adapter";

    if(argc > 2) {
        fwprintf(stderr, L"usage: %ls [absolute-supermium-chrome.exe]\n", argv[0]);
        return 2;
    }
    if(argc == 2) {
        if(!SetEnvironmentVariableW(L"TRITON_SUPERMIUM_EXE", argv[1])) {
            fwprintf(stderr, L"cannot set Supermium executable path: %lu\n", GetLastError());
            return 2;
        }
        expected_backend = L"supermium-144-headless-adapter";
    }

    VariantInit(&active_x_policy);
    VariantInit(&backend);
    VariantInit(&url);
    VariantInit(&ready_state);
    VariantInit(&title);
    VariantInit(&body_html);
    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if(FAILED(hr)) goto done;
    initialized = TRUE;
    hr = CoCreateInstance(&CLSID_TritonCefHtmlDocument, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IDispatch, (void **)&document);
    if(FAILED(hr)) goto done;
#define REQUIRE_CALL(expression) do { \
    hr = (expression); \
    if(FAILED(hr)) { \
        wprintf(L"IE7 bridge dispatch failure: 0x%08lx\n", (unsigned long)hr); \
        goto done; \
    } \
} while(0)
    REQUIRE_CALL(invoke_no_args(document, L"open"));
    REQUIRE_CALL(invoke_bstr_method(document, L"write", L"<p>alpha</p>"));
    REQUIRE_CALL(invoke_bstr_method(document, L"writeln", L"<p>beta</p>"));
    REQUIRE_CALL(put_bstr(document, L"title", L"IE7 CEF bridge"));
    REQUIRE_CALL(invoke_no_args(document, L"close"));
    REQUIRE_CALL(get_value(document, L"backend", &backend));
    REQUIRE_CALL(get_value(document, L"activeXPolicy", &active_x_policy));
    REQUIRE_CALL(get_value(document, L"url", &url));
    REQUIRE_CALL(get_value(document, L"readyState", &ready_state));
    REQUIRE_CALL(get_value(document, L"title", &title));
    REQUIRE_CALL(get_value(document, L"body", &body_html));
#undef REQUIRE_CALL
    if(body_html.vt != VT_DISPATCH || !body_html.pdispVal) goto done;
    body = body_html.pdispVal;
    body_html.vt = VT_EMPTY;
    body_html.pdispVal = NULL;
    if(FAILED(get_value(body, L"innerHTML", &body_html))) goto done;
    if(!expect_bstr(&active_x_policy, L"unsupported-stubbed") ||
       !expect_bstr(&backend, expected_backend) || !expect_bstr(&url, L"about:blank") ||
       !expect_bstr(&ready_state, L"complete") || !expect_bstr(&title, L"IE7 CEF bridge") ||
       body_html.vt != VT_BSTR || !body_html.bstrVal ||
       !wcsstr(body_html.bstrVal, L"<p>alpha</p>") ||
       !wcsstr(body_html.bstrVal, L"<p>beta</p>")) {
        wprintf(L"IE7 bridge result mismatch: backend=%ls url=%ls ready=%ls title=%ls html=%ls\n",
                backend.vt == VT_BSTR && backend.bstrVal ? backend.bstrVal : L"<invalid>",
                url.vt == VT_BSTR && url.bstrVal ? url.bstrVal : L"<invalid>",
                ready_state.vt == VT_BSTR && ready_state.bstrVal ? ready_state.bstrVal : L"<invalid>",
                title.vt == VT_BSTR && title.bstrVal ? title.bstrVal : L"<invalid>",
                body_html.vt == VT_BSTR && body_html.bstrVal ? body_html.bstrVal : L"<invalid>");
        goto done;
    }
    wprintf(L"BRIDGE_BACKEND=%ls\n", backend.bstrVal);
    wprintf(L"BRIDGE_ACTIVEX_POLICY=%ls\n", active_x_policy.bstrVal);
    wprintf(L"BRIDGE_READY_STATE=%ls\n", ready_state.bstrVal);
    wprintf(L"BRIDGE_URL=%ls\n", url.bstrVal);
    wprintf(L"BRIDGE_TITLE=%ls\n", title.bstrVal);
    wprintf(L"BRIDGE_HTML=%ls\n", body_html.bstrVal);
    wprintf(argc == 2 ? L"IE7 SUPERMIUM ADAPTER HOST VERIFIED\n"
                      : L"IE7 CEF BRIDGE HOST VERIFIED\n");
    status = 0;
done:
    if(status) wprintf(L"IE7 bridge host failed: 0x%08lx\n", (unsigned long)hr);
    if(body) IDispatch_Release(body);
    if(document) IDispatch_Release(document);
    VariantClear(&active_x_policy);
    VariantClear(&backend);
    VariantClear(&url);
    VariantClear(&ready_state);
    VariantClear(&title);
    VariantClear(&body_html);
    if(initialized) CoUninitialize();
    return status;
}
