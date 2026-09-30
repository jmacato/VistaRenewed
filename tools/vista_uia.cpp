/* Bounded UIA desktop worker; invoked by the private Vista control channel. */
#define _WIN32_WINNT 0x0600
#include <windows.h>
#include <uiautomation.h>
#include <stdio.h>
#include <string>
#include <stdlib.h>

static void quoted(const wchar_t *value)
{
    putchar('"');
    for(; value && *value; ++value) {
        unsigned c = (unsigned)*value;
        if(c == '"' || c == '\\') printf("\\%c", c);
        else if(c < 32 || c > 126) printf("\\u%04x", c);
        else putchar((int)c);
    }
    putchar('"');
}

static bool unhex(const wchar_t *hex, std::wstring &out)
{
    std::string bytes;
    size_t length = wcslen(hex);
    if(length > 16000 || length % 2) return false;
    for(size_t i = 0; i < length; i += 2) {
        unsigned value = 0;
        for(size_t j = 0; j < 2; ++j) {
            wchar_t c = hex[i+j];
            if(c >= L'0' && c <= L'9') value = value * 16 + c - L'0';
            else if(c >= L'a' && c <= L'f') value = value * 16 + c - L'a' + 10;
            else return false;
        }
        if(!value) return false;
        bytes.push_back((char)value);
    }
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), (int)bytes.size(), NULL, 0);
    if(!count && !bytes.empty()) return false;
    out.resize(count);
    if(count) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), (int)bytes.size(), &out[0], count);
    return true;
}

static unsigned nodes;
static HRESULT tree(IUIAutomationTreeWalker *walker, IUIAutomationElement *element,
                    const std::string &path, unsigned depth)
{
    BSTR name = NULL, cls = NULL, id = NULL;
    int pid = 0, type = 0;
    BOOL enabled = FALSE, focused = FALSE;
    RECT bounds = {};
    element->get_CurrentName(&name); element->get_CurrentClassName(&cls);
    element->get_CurrentAutomationId(&id); element->get_CurrentProcessId(&pid);
    element->get_CurrentControlType(&type); element->get_CurrentIsEnabled(&enabled);
    element->get_CurrentHasKeyboardFocus(&focused); element->get_CurrentBoundingRectangle(&bounds);
    printf("{\"path\":\"%s\",\"pid\":%d,\"type\":%d,\"enabled\":%s,\"focused\":%s,\"name\":",
           path.c_str(), pid, type, enabled ? "true":"false", focused ? "true":"false");
    quoted(name); printf(",\"class\":"); quoted(cls); printf(",\"automation_id\":"); quoted(id);
    printf(",\"bounds\":[%ld,%ld,%ld,%ld]}\n", bounds.left, bounds.top, bounds.right, bounds.bottom);
    SysFreeString(name); SysFreeString(cls); SysFreeString(id);
    ++nodes;
    if(depth >= 8 || nodes >= 256) return S_OK;
    IUIAutomationElement *child = NULL;
    HRESULT hr = walker->GetFirstChildElement(element, &child);
    unsigned index = 0;
    while(SUCCEEDED(hr) && child && nodes < 256) {
        hr = tree(walker, child, path + "." + std::to_string(index++), depth + 1);
        IUIAutomationElement *next = NULL;
        if(SUCCEEDED(hr)) hr = walker->GetNextSiblingElement(child, &next);
        child->Release(); child = next;
    }
    if(child) child->Release();
    return hr;
}

int wmain(int argc, wchar_t **argv)
{
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if(FAILED(hr)) return 1;
    IUIAutomation *uia = NULL;
    IUIAutomationElement *element = NULL;
    IUIAutomationTreeWalker *walker = NULL;
    hr = CoCreateInstance(__uuidof(CUIAutomation), NULL, CLSCTX_INPROC_SERVER,
                          __uuidof(IUIAutomation), (void **)&uia);
    if(FAILED(hr)) {
        printf("{\"uia_available\":false,\"hresult\":\"0x%08lx\"}\n", (unsigned long)hr);
        CoUninitialize(); return 2;
    }
    if(argc == 2 && !wcscmp(argv[1], L"probe")) {
        puts("{\"uia_available\":true}"); uia->Release(); CoUninitialize(); return 0;
    }
    if(argc < 4) { hr = E_INVALIDARG; goto done; }
    {
        wchar_t *end;
        unsigned long long handle = wcstoull(argv[2], &end, 16);
        if(*end) { hr = E_INVALIDARG; goto done; }
        hr = handle ? uia->ElementFromHandle((HWND)(ULONG_PTR)handle, &element) : uia->GetRootElement(&element);
    }
    if(FAILED(hr)) goto done;
    hr = uia->get_ControlViewWalker(&walker);
    if(FAILED(hr)) goto done;
    {
        const wchar_t *path = argv[3];
        if(*path++ != L'0') { hr = E_INVALIDARG; goto done; }
        unsigned depth = 0;
        while(*path) {
            wchar_t *end;
            if(*path++ != L'.' || ++depth > 8 || *path < L'0' || *path > L'9') { hr = E_INVALIDARG; goto done; }
            unsigned long index = wcstoul(path, &end, 10); path = end;
            if(index > 255) { hr = E_INVALIDARG; goto done; }
            IUIAutomationElement *child = NULL;
            hr = walker->GetFirstChildElement(element, &child);
            while(SUCCEEDED(hr) && child && index--) {
                IUIAutomationElement *next = NULL;
                hr = walker->GetNextSiblingElement(child, &next); child->Release(); child = next;
            }
            element->Release(); element = child;
            if(FAILED(hr) || !element) { hr = UIA_E_ELEMENTNOTAVAILABLE; goto done; }
        }
    }
    if(!wcscmp(argv[1], L"tree")) { hr = tree(walker, element, "0", 0); goto done; }
    {
        std::wstring expected, value;
        BSTR actual = NULL;
        if(argc < 5 || !unhex(argv[4], expected)) { hr = E_INVALIDARG; goto done; }
        hr = element->get_CurrentName(&actual);
        bool matches = SUCCEEDED(hr) && expected == (actual ? actual : L"");
        SysFreeString(actual);
        if(!matches) { hr = HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH); goto done; }
        if(!wcscmp(argv[1], L"focus")) hr = element->SetFocus();
        else if(!wcscmp(argv[1], L"invoke")) {
            IUIAutomationInvokePattern *pattern = NULL;
            hr = element->GetCurrentPatternAs(UIA_InvokePatternId, __uuidof(IUIAutomationInvokePattern), (void **)&pattern);
            if(SUCCEEDED(hr)) { hr = pattern->Invoke(); pattern->Release(); }
        } else if(!wcscmp(argv[1], L"set-value") && argc == 6 && unhex(argv[5], value)) {
            IUIAutomationValuePattern *pattern = NULL;
            hr = element->GetCurrentPatternAs(UIA_ValuePatternId, __uuidof(IUIAutomationValuePattern), (void **)&pattern);
            if(SUCCEEDED(hr)) {
                BSTR text = SysAllocString(value.c_str());
                hr = text ? pattern->SetValue(text) : E_OUTOFMEMORY;
                SysFreeString(text); pattern->Release();
            }
        } else hr = E_INVALIDARG;
    }
done:
    printf("{\"hresult\":\"0x%08lx\",\"nodes\":%u}\n", (unsigned long)hr, nodes);
    if(walker) walker->Release();
    if(element) element->Release();
    uia->Release(); CoUninitialize();
    return FAILED(hr) ? 1 : 0;
}
