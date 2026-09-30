/* Enumerate Vista's stock MSHTML typelib and its HTMLDocument COM surface. */
#define INITGUID
#define COBJMACROS
#include <windows.h>
#include <oaidl.h>
#include <ole2.h>
#include <mshtml.h>
#include <servprov.h>
#include <stdio.h>
#include <wchar.h>

typedef struct {
    const char *name;
    const IID *iid;
} interface_probe;

static const interface_probe document_interfaces[] = {
    {"IUnknown", &IID_IUnknown},
    {"IDispatch", &IID_IDispatch},
    {"IHTMLDocument2", &IID_IHTMLDocument2},
    {"IHTMLDocument3", &IID_IHTMLDocument3},
    {"IHTMLDocument4", &IID_IHTMLDocument4},
    {"IHTMLDocument5", &IID_IHTMLDocument5},
    {"IHTMLDocument6", &IID_IHTMLDocument6},
    {"IHTMLDocument7", &IID_IHTMLDocument7},
    {"IPersist", &IID_IPersist},
    {"IPersistStreamInit", &IID_IPersistStreamInit},
    {"IPersistMoniker", &IID_IPersistMoniker},
    {"IOleObject", &IID_IOleObject},
    {"IOleInPlaceObject", &IID_IOleInPlaceObject},
    {"IViewObject", &IID_IViewObject},
    {"IViewObject2", &IID_IViewObject2},
    {"IServiceProvider", &IID_IServiceProvider},
    {"IConnectionPointContainer", &IID_IConnectionPointContainer},
};

static const interface_probe window_interfaces[] = {
    {"IUnknown", &IID_IUnknown},
    {"IDispatch", &IID_IDispatch},
    {"IHTMLFramesCollection2", &IID_IHTMLFramesCollection2},
    {"IHTMLWindow2", &IID_IHTMLWindow2},
    {"IHTMLWindow3", &IID_IHTMLWindow3},
    {"IHTMLWindow4", &IID_IHTMLWindow4},
    {"IHTMLWindow5", &IID_IHTMLWindow5},
    {"IHTMLWindow6", &IID_IHTMLWindow6},
    {"IHTMLWindow7", &IID_IHTMLWindow7},
    {"IServiceProvider", &IID_IServiceProvider},
    {"IConnectionPointContainer", &IID_IConnectionPointContainer},
};

static const interface_probe element_interfaces[] = {
    {"IUnknown", &IID_IUnknown},
    {"IDispatch", &IID_IDispatch},
    {"IHTMLDOMNode", &IID_IHTMLDOMNode},
    {"IHTMLDOMNode2", &IID_IHTMLDOMNode2},
    {"IHTMLDOMNode3", &IID_IHTMLDOMNode3},
    {"IHTMLElement", &IID_IHTMLElement},
    {"IHTMLElement2", &IID_IHTMLElement2},
    {"IHTMLElement3", &IID_IHTMLElement3},
    {"IHTMLElement4", &IID_IHTMLElement4},
    {"IHTMLElement5", &IID_IHTMLElement5},
    {"IHTMLElement6", &IID_IHTMLElement6},
    {"IHTMLElement7", &IID_IHTMLElement7},
    {"IServiceProvider", &IID_IServiceProvider},
    {"IConnectionPointContainer", &IID_IConnectionPointContainer},
};

static const interface_probe location_interfaces[] = {
    {"IUnknown", &IID_IUnknown},
    {"IDispatch", &IID_IDispatch},
    {"IHTMLLocation", &IID_IHTMLLocation},
    {"IServiceProvider", &IID_IServiceProvider},
};

static const char *typekind_name(TYPEKIND kind)
{
    switch(kind) {
    case TKIND_ENUM: return "enum";
    case TKIND_RECORD: return "record";
    case TKIND_MODULE: return "module";
    case TKIND_INTERFACE: return "interface";
    case TKIND_DISPATCH: return "dispatch";
    case TKIND_COCLASS: return "coclass";
    case TKIND_ALIAS: return "alias";
    case TKIND_UNION: return "union";
    default: return "unknown";
    }
}

static void write_guid(FILE *output, REFGUID guid)
{
    WCHAR text[40];
    if(StringFromGUID2(guid, text, ARRAYSIZE(text)))
        fprintf(output, "%ls", text);
    else
        fputs("{invalid-guid}", output);
}

static void dump_typeinfo(FILE *output, const char *scope, ITypeInfo *info)
{
    TYPEATTR *attributes = NULL;
    BSTR type_name = NULL;
    HRESULT hr;
    UINT function_index;

    hr = ITypeInfo_GetTypeAttr(info, &attributes);
    fprintf(output, "%s TYPEINFO attr=0x%08lx\n", scope, (unsigned long)hr);
    if(FAILED(hr)) return;
    ITypeInfo_GetDocumentation(info, MEMBERID_NIL, &type_name, NULL, NULL, NULL);
    fprintf(output, "%s TYPEINFO kind=%s name=%ls guid=", scope,
            typekind_name(attributes->typekind), type_name ? type_name : L"(unnamed)");
    write_guid(output, &attributes->guid);
    fprintf(output, " funcs=%u vars=%u impls=%u\n", attributes->cFuncs,
            attributes->cVars, attributes->cImplTypes);
    for(function_index = 0; function_index < attributes->cFuncs; function_index++) {
        FUNCDESC *function = NULL;
        BSTR names[16] = {NULL};
        UINT names_count = 0;
        if(SUCCEEDED(ITypeInfo_GetFuncDesc(info, function_index, &function))) {
            ITypeInfo_GetNames(info, function->memid, names, ARRAYSIZE(names), &names_count);
            fprintf(output, "%s MEMBER memid=%ld invoke=%u params=%u optional=%d name=%ls\n",
                    scope, (long)function->memid, function->invkind, function->cParams,
                    (int)function->cParamsOpt,
                    names_count ? names[0] : L"(unnamed)");
            while(names_count) SysFreeString(names[--names_count]);
            ITypeInfo_ReleaseFuncDesc(info, function);
        }
    }
    SysFreeString(type_name);
    ITypeInfo_ReleaseTypeAttr(info, attributes);
}

static void dump_dispatch_typeinfo(FILE *output, const char *scope, IDispatch *dispatch)
{
    UINT count = 0;
    UINT index;
    HRESULT hr = IDispatch_GetTypeInfoCount(dispatch, &count);
    fprintf(output, "%s DISPATCH typeinfo-count=0x%08lx count=%u\n", scope,
            (unsigned long)hr, count);
    if(FAILED(hr)) return;
    for(index = 0; index < count; index++) {
        ITypeInfo *info = NULL;
        hr = IDispatch_GetTypeInfo(dispatch, index, LOCALE_SYSTEM_DEFAULT, &info);
        fprintf(output, "%s DISPATCH typeinfo[%u]=0x%08lx\n", scope, index,
                (unsigned long)hr);
        if(SUCCEEDED(hr)) {
            dump_typeinfo(output, scope, info);
            ITypeInfo_Release(info);
        }
    }
}

static void probe_interfaces(FILE *output, const char *scope, IUnknown *object,
                             const interface_probe *interfaces, size_t count)
{
    size_t index;
    HRESULT hr;
    for(index = 0; index < count; index++) {
        void *interface_pointer = NULL;
        hr = IUnknown_QueryInterface(object, interfaces[index].iid, &interface_pointer);
        fprintf(output, "%s QI name=%s hr=0x%08lx\n", scope, interfaces[index].name,
                (unsigned long)hr);
        if(interface_pointer) IUnknown_Release((IUnknown *)interface_pointer);
    }
}

static void dump_connection_points(FILE *output, const char *scope, IUnknown *object)
{
    IConnectionPointContainer *container = NULL;
    IEnumConnectionPoints *enumerator = NULL;
    HRESULT hr;
    ULONG fetched;
    unsigned int index = 0;

    hr = IUnknown_QueryInterface(object, &IID_IConnectionPointContainer, (void **)&container);
    fprintf(output, "%s CONNECTIONS container=0x%08lx\n", scope, (unsigned long)hr);
    if(FAILED(hr)) return;
    hr = IConnectionPointContainer_EnumConnectionPoints(container, &enumerator);
    fprintf(output, "%s CONNECTIONS enumerate=0x%08lx\n", scope, (unsigned long)hr);
    if(SUCCEEDED(hr)) {
        for(;;) {
            IConnectionPoint *point = NULL;
            IID event_iid;
            fetched = 0;
            hr = IEnumConnectionPoints_Next(enumerator, 1, &point, &fetched);
            if(hr != S_OK || fetched != 1) break;
            hr = IConnectionPoint_GetConnectionInterface(point, &event_iid);
            fprintf(output, "%s CONNECTION index=%u iid=", scope, index++);
            if(SUCCEEDED(hr)) write_guid(output, &event_iid);
            else fprintf(output, "(error 0x%08lx)", (unsigned long)hr);
            fputc('\n', output);
            IConnectionPoint_Release(point);
        }
        IEnumConnectionPoints_Release(enumerator);
    }
    IConnectionPointContainer_Release(container);
}

static void dump_typelib(FILE *output)
{
    WCHAR path[MAX_PATH];
    size_t prefix_length;
    ITypeLib *library = NULL;
    HRESULT hr;
    UINT index, count;

    if(!GetSystemDirectoryW(path, ARRAYSIZE(path)) ||
       wcslen(path) + wcslen(L"\\mshtml.dll") >= ARRAYSIZE(path)) {
        fputs("ERROR system-directory\n", output);
        return;
    }
    wcscat(path, L"\\mshtml.dll");
    hr = LoadTypeLibEx(path, REGKIND_NONE, &library);
    fprintf(output, "TYPELIB load=0x%08lx path=%ls\n", (unsigned long)hr, path);
    if(FAILED(hr)) {
        prefix_length = wcslen(path) - wcslen(L"mshtml.dll");
        wcscpy(path + prefix_length, L"mshtml.tlb");
        hr = LoadTypeLibEx(path, REGKIND_NONE, &library);
        fprintf(output, "TYPELIB fallback-load=0x%08lx path=%ls\n",
                (unsigned long)hr, path);
        if(FAILED(hr)) return;
    }

    count = ITypeLib_GetTypeInfoCount(library);
    fprintf(output, "TYPELIB count=%u\n", count);
    for(index = 0; index < count; index++) {
        ITypeInfo *info = NULL;
        TYPEATTR *attributes = NULL;
        BSTR name = NULL;
        TYPEKIND kind;
        UINT function_index;

        hr = ITypeLib_GetTypeInfoType(library, index, &kind);
        if(FAILED(hr) || FAILED(ITypeLib_GetTypeInfo(library, index, &info)) ||
           FAILED(ITypeInfo_GetTypeAttr(info, &attributes))) {
            fprintf(output, "TYPE index=%u error=0x%08lx\n", index, (unsigned long)hr);
            if(info) ITypeInfo_Release(info);
            continue;
        }
        ITypeLib_GetDocumentation(library, index, &name, NULL, NULL, NULL);
        fprintf(output, "TYPE index=%u kind=%s name=%ls guid=", index, typekind_name(kind),
                name ? name : L"(unnamed)");
        write_guid(output, &attributes->guid);
        fprintf(output, " funcs=%u vars=%u impls=%u\n", attributes->cFuncs,
                attributes->cVars, attributes->cImplTypes);
        for(function_index = 0; function_index < attributes->cFuncs; function_index++) {
            FUNCDESC *function = NULL;
            BSTR names[4] = {NULL, NULL, NULL, NULL};
            UINT names_count = 0;
            if(SUCCEEDED(ITypeInfo_GetFuncDesc(info, function_index, &function))) {
                ITypeInfo_GetNames(info, function->memid, names, ARRAYSIZE(names), &names_count);
                fprintf(output, "  MEMBER memid=%ld invoke=%u params=%u name=%ls\n",
                        (long)function->memid, function->invkind, function->cParams,
                        names_count ? names[0] : L"(unnamed)");
                while(names_count) SysFreeString(names[--names_count]);
                ITypeInfo_ReleaseFuncDesc(info, function);
            }
        }
        SysFreeString(name);
        ITypeInfo_ReleaseTypeAttr(info, attributes);
        ITypeInfo_Release(info);
    }
    ITypeLib_Release(library);
}

static void dump_document_interfaces(FILE *output)
{
    IClassFactory *factory = NULL;
    IUnknown *document = NULL;
    IDispatch *dispatch = NULL;
    IHTMLDocument2 *document2 = NULL;
    HRESULT hr;

    hr = CoGetClassObject(&CLSID_HTMLDocument, CLSCTX_INPROC_SERVER, NULL,
                          &IID_IClassFactory, (void **)&factory);
    fprintf(output, "CLASS CLSID_HTMLDocument factory=0x%08lx\n", (unsigned long)hr);
    if(FAILED(hr)) return;
    hr = IClassFactory_CreateInstance(factory, NULL, &IID_IUnknown, (void **)&document);
    IClassFactory_Release(factory);
    fprintf(output, "CLASS CLSID_HTMLDocument instance=0x%08lx\n", (unsigned long)hr);
    if(FAILED(hr)) return;
    probe_interfaces(output, "DOCUMENT", document, document_interfaces,
                     ARRAYSIZE(document_interfaces));
    hr = IUnknown_QueryInterface(document, &IID_IDispatch, (void **)&dispatch);
    if(SUCCEEDED(hr)) {
        dump_dispatch_typeinfo(output, "DOCUMENT", dispatch);
        IDispatch_Release(dispatch);
    }
    dump_connection_points(output, "DOCUMENT", document);

    hr = IUnknown_QueryInterface(document, &IID_IHTMLDocument2, (void **)&document2);
    fprintf(output, "DOCUMENT IHTMLDocument2=0x%08lx\n", (unsigned long)hr);
    if(SUCCEEDED(hr)) {
        SAFEARRAY *source = SafeArrayCreateVector(VT_VARIANT, 0, 1);
        if(source) {
            VARIANT *item = NULL;
            hr = SafeArrayAccessData(source, (void **)&item);
            if(SUCCEEDED(hr)) {
                VariantInit(item);
                item->vt = VT_BSTR;
                item->bstrVal = SysAllocString(L"<html><body id='cef-probe'>probe</body></html>");
                SafeArrayUnaccessData(source);
                hr = IHTMLDocument2_write(document2, source);
                fprintf(output, "DOCUMENT write=0x%08lx\n", (unsigned long)hr);
                hr = IHTMLDocument2_close(document2);
                fprintf(output, "DOCUMENT close=0x%08lx\n", (unsigned long)hr);
            } else {
                fprintf(output, "DOCUMENT write-access=0x%08lx\n", (unsigned long)hr);
            }
            SafeArrayDestroy(source);
        } else {
            fputs("DOCUMENT write-allocation=failed\n", output);
        }

        {
            IHTMLWindow2 *window = NULL;
            hr = IHTMLDocument2_get_parentWindow(document2, &window);
            fprintf(output, "WINDOW acquire=0x%08lx\n", (unsigned long)hr);
            if(SUCCEEDED(hr)) {
                probe_interfaces(output, "WINDOW", (IUnknown *)window, window_interfaces,
                                 ARRAYSIZE(window_interfaces));
                dump_dispatch_typeinfo(output, "WINDOW", (IDispatch *)window);
                dump_connection_points(output, "WINDOW", (IUnknown *)window);
                IHTMLWindow2_Release(window);
            }
        }
        {
            IHTMLLocation *location = NULL;
            hr = IHTMLDocument2_get_location(document2, &location);
            fprintf(output, "LOCATION acquire=0x%08lx\n", (unsigned long)hr);
            if(SUCCEEDED(hr)) {
                probe_interfaces(output, "LOCATION", (IUnknown *)location, location_interfaces,
                                 ARRAYSIZE(location_interfaces));
                dump_dispatch_typeinfo(output, "LOCATION", (IDispatch *)location);
                IHTMLLocation_Release(location);
            }
        }
        {
            IHTMLElement *body = NULL;
            hr = IHTMLDocument2_get_body(document2, &body);
            fprintf(output, "BODY acquire=0x%08lx\n", (unsigned long)hr);
            if(SUCCEEDED(hr)) {
                probe_interfaces(output, "BODY", (IUnknown *)body, element_interfaces,
                                 ARRAYSIZE(element_interfaces));
                dump_dispatch_typeinfo(output, "BODY", (IDispatch *)body);
                dump_connection_points(output, "BODY", (IUnknown *)body);
                IHTMLElement_Release(body);
            }
        }
        {
            IServiceProvider *provider = NULL;
            IHTMLWindow2 *service_window = NULL;
            hr = IUnknown_QueryInterface(document, &IID_IServiceProvider, (void **)&provider);
            fprintf(output, "SERVICE provider=0x%08lx\n", (unsigned long)hr);
            if(SUCCEEDED(hr)) {
                hr = IServiceProvider_QueryService(provider, &SID_SHTMLWindow,
                                                   &IID_IHTMLWindow2,
                                                   (void **)&service_window);
                fprintf(output, "SERVICE SID_SHTMLWindow/IHTMLWindow2=0x%08lx\n",
                        (unsigned long)hr);
                if(service_window) IHTMLWindow2_Release(service_window);
                IServiceProvider_Release(provider);
            }
        }
        IHTMLDocument2_Release(document2);
    }
    IUnknown_Release(document);
}

int wmain(int argc, WCHAR **argv)
{
    const WCHAR *destination = argc == 2 ? argv[1] : L"C:\\Windows\\Temp\\mshtml-interface-dump.txt";
    FILE *output;
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if(FAILED(hr)) return 1;
    output = _wfopen(destination, L"wb");
    if(!output) {
        CoUninitialize();
        return 2;
    }
    fprintf(output, "MSHTML_INTERFACE_DUMP_V3\n");
    dump_typelib(output);
    dump_document_interfaces(output);
    fclose(output);
    CoUninitialize();
    return 0;
}
