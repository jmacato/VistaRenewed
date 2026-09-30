/* Read-only native COM oracle: no registry activation, navigation or UI. */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ole2.h>
#include <ocidl.h>
#include <mshtml.h>
#include <mshtmdid.h>
#include <olectl.h>
#include <stdio.h>
#include <urlmon.h>
#include <docobj.h>
#include "mshtml_private_window.h"

typedef HRESULT (WINAPI *GetClassObjectFn)(REFCLSID, REFIID, void **);
static const CLSID html_document_class = {0x25336920,0x03f9,0x11cf,{0x8f,0xd0,0,0xaa,0,0x68,0x6f,0x13}};
static const IID private_window4 = {0x3050f594,0x98b5,0x11cf,{0xbb,0x82,0,0xaa,0,0xbd,0xce,0x0b}};

int main(void)
{
    HMODULE module;
    GetClassObjectFn get_class;
    IClassFactory *factory = NULL;
    IUnknown *document = NULL, *private_window = NULL;
    IPersistStreamInit *persist = NULL;
    IHTMLDocument2 *html = NULL;
    IHTMLWindow2 *window = NULL;
    IDispatch *dispatch = NULL;
    IOleObject *ole = NULL;
    IConnectionPointContainer *connections = NULL;
    IConnectionPoint *point = NULL;
    WCHAR path[MAX_PATH];
    BSTR ready = NULL;
    VARIANT state;
    DISPPARAMS params = {0};
    HRESULT hr;
    DWORD misc = 0;
    int failed = 0;
    if (FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED))) return 1;
    module = LoadLibraryW(L"C:\\Windows\\SysWOW64\\mshtml.dll");
    if (!module) return 2;
    GetModuleFileNameW(module, path, MAX_PATH);
    printf("NATIVE_MODULE=%ls\n", path);
    get_class = (GetClassObjectFn)(void *)GetProcAddress(module, "DllGetClassObject");
    if (!get_class) return 3;
    hr = get_class(&html_document_class, &IID_IClassFactory, (void **)&factory);
    printf("GET_CLASS=0x%08lx\n", (unsigned long)hr);
    if (FAILED(hr)) return 4;
    hr = IClassFactory_CreateInstance(factory, NULL, &IID_IUnknown, (void **)&document);
    IClassFactory_Release(factory);
    printf("CREATE=0x%08lx\n", (unsigned long)hr);
    if (FAILED(hr)) return 5;
    hr = IUnknown_QueryInterface(document, &IID_IPersistStreamInit, (void **)&persist);
    if (SUCCEEDED(hr)) {
        hr = IPersistStreamInit_InitNew(persist);
        IPersistStreamInit_Release(persist);
    }
    printf("INIT_NEW=0x%08lx\n", (unsigned long)hr);
    failed |= FAILED(hr);
    hr = IUnknown_QueryInterface(document, &IID_IOleObject, (void **)&ole);
    if (SUCCEEDED(hr)) {
        hr = IOleObject_GetMiscStatus(ole, DVASPECT_CONTENT, &misc);
        IOleObject_Release(ole);
    }
    printf("OLE_MISC_STATUS HR=0x%08lx FLAGS=0x%08lx ALWAYSRUN=%u\n",
           (unsigned long)hr, (unsigned long)misc, !!(misc & OLEMISC_ALWAYSRUN));
    /* Record, do not assume these are the browser-service flags. */
    failed |= FAILED(hr);
    hr = IUnknown_QueryInterface(document, &IID_IDispatch, (void **)&dispatch);
    if (SUCCEEDED(hr)) {
        VariantInit(&state);
        hr = IDispatch_Invoke(dispatch, DISPID_READYSTATE, &IID_NULL, LOCALE_USER_DEFAULT,
                              DISPATCH_PROPERTYGET, &params, &state, NULL, NULL);
        printf("DISPID_READYSTATE=%ld HR=0x%08lx VT=%u VALUE=%ld\n",
               (long)DISPID_READYSTATE, (unsigned long)hr, state.vt,
               state.vt == VT_I4 ? (long)state.lVal : -1L);
        failed |= FAILED(hr) || state.vt != VT_I4;
        VariantClear(&state);
        IDispatch_Release(dispatch);
    } else failed = 1;
    hr = IUnknown_QueryInterface(document, &IID_IHTMLDocument2, (void **)&html);
    if (SUCCEEDED(hr)) {
        hr = IHTMLDocument2_get_readyState(html, &ready);
        printf("TYPED_READYSTATE HR=0x%08lx VALUE=%ls\n", (unsigned long)hr, ready ? ready : L"NULL");
        failed |= FAILED(hr);
        SysFreeString(ready);
        hr = IHTMLDocument2_get_parentWindow(html, &window);
        printf("PARENT_WINDOW=0x%08lx\n", (unsigned long)hr);
        failed |= FAILED(hr) || !window;
        if (window) {
            TritonPrivateWindow4 *legacy = NULL;
            BSTR address = NULL;
            hr = IHTMLWindow2_QueryInterface(window, &private_window4, (void **)&private_window);
            printf("IHTMLPrivateWindow4=0x%08lx\n", (unsigned long)hr);
            failed |= FAILED(hr) || !private_window;
            if (private_window) IUnknown_Release(private_window);
            hr = IHTMLWindow2_QueryInterface(window, &triton_private_window_iid, (void **)&legacy);
            printf("IHTMLPrivateWindow=0x%08lx\n", (unsigned long)hr);
            failed |= FAILED(hr) || !legacy;
            if(legacy) {
                hr = legacy->lpVtbl->GetAddressBarUrl(legacy, &address);
                printf("PRIVATE_ADDRESS_BAR_URL HR=0x%08lx VALUE=%ls\n",
                       (unsigned long)hr, address ? address : L"NULL");
                failed |= FAILED(hr) || !address;
                SysFreeString(address);
                legacy->lpVtbl->Release(legacy);
            }
            IHTMLWindow2_Release(window);
        }
        IHTMLDocument2_Release(html);
    } else failed = 1;
    hr = IUnknown_QueryInterface(document, &IID_IConnectionPointContainer, (void **)&connections);
    if (SUCCEEDED(hr)) {
        hr = IConnectionPointContainer_FindConnectionPoint(connections, &IID_IPropertyNotifySink, &point);
        failed |= FAILED(hr) || !point;
        if (point) IConnectionPoint_Release(point);
        IConnectionPointContainer_Release(connections);
    }
    printf("PROPERTY_NOTIFY_CONNECTION_POINT=0x%08lx\n", (unsigned long)hr);
    failed |= FAILED(hr);
    IUnknown_Release(document);
    CoUninitialize();
    FreeLibrary(module);
    if (!failed) puts("NATIVE MSHTML WINDOW AND READYSTATE CONTRACTS CONFIRMED");
    return failed ? 6 : 0;
}
