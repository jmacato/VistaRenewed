#!/usr/bin/env python3
"""Generate exact toolchain vtables for the implemented MSHTML node facade."""
import re
import sys
from pathlib import Path


def generate(header, interface, prefix, vtable):
    begin = header.index(f'typedef struct {interface}Vtbl')
    end = header.index(f'}} {interface}Vtbl;', begin)
    methods = re.findall(r'(HRESULT|ULONG) \(STDMETHODCALLTYPE \*(\w+)\)\(\s*'
                         + interface + r' \*This(?:,\s*(.*?))?\);', header[begin:end], re.S)
    if len(methods) < 40:
        raise ValueError(f'incomplete {interface} vtable')
    output, entries = [], []
    is_element = interface == 'IHTMLElement'
    owner = 'element_from_iface(This)' if is_element else 'document_from_html3(This)'
    for result, name, raw in methods:
        params = re.sub(r'\s+', ' ', raw or '').strip()
        function = prefix + '_' + name
        output += [f'static {result} STDMETHODCALLTYPE {function}({interface} *This'
                   + (', ' + params if params else '') + ')', '{']
        if name == 'QueryInterface':
            body = f'return {"element_query" if is_element else "document_query_interface"}({owner}, riid, ppvObject);'
        elif name in ('AddRef', 'Release'):
            operation = 'add_ref' if name == 'AddRef' else 'release'
            body = f'return {"element" if is_element else "document"}_{operation}({owner});'
        elif name == 'GetTypeInfoCount':
            body = 'if(!pctinfo) return E_POINTER;\n    *pctinfo = 0;\n    return S_OK;'
        elif name == 'GetTypeInfo':
            body = 'if(!ppTInfo) return E_POINTER;\n    *ppTInfo = NULL;\n    return DISP_E_BADINDEX;'
        elif name == 'GetIDsOfNames':
            body = ('return element_names(riid, rgszNames, cNames, rgDispId);' if is_element else
                    f'return dispatch_get_ids_of_names(&{owner}->IDispatch_iface, riid, rgszNames, cNames, lcid, rgDispId);')
        elif name == 'Invoke':
            body = (f'return element_invoke({owner}, dispIdMember, riid, wFlags, pDispParams, pVarResult, puArgErr);' if is_element else
                    f'return dispatch_invoke(&{owner}->IDispatch_iface, dispIdMember, riid, lcid, wFlags, pDispParams, pVarResult, pExcepInfo, puArgErr);')
        elif is_element and name in ('get_parentElement', 'get_document'):
            body = f'return element_{"parent" if name == "get_parentElement" else "document"}({owner}, p);'
        elif is_element and name.startswith(('get_', 'put_')) and name[4:] in (
                'innerHTML', 'outerHTML', 'innerText', 'outerText', 'id', 'className', 'tagName', 'title'):
            body = f'return element_{name[:3]}_property({owner}, L"{name[4:]}", {"p" if name.startswith("get_") else "v"});'
        elif not is_element and name == 'get_documentElement':
            body = f'return document_element({owner}, L"function(){{return this.documentElement;}}", NULL, FALSE, p);'
        elif not is_element and name == 'getElementById':
            body = f'return document_element({owner}, L"function(s){{return this.getElementById(s);}}", v, TRUE, pel);'
        else:
            body = 'return E_NOTIMPL;'
        output += ['    ' + body, '}']
        entries.append(function)
    output += [f'static {interface}Vtbl {vtable} = {{', *('    ' + e + ',' for e in entries), '};']
    print(f'{interface}_VTABLE_GENERATED methods={len(methods)}')
    return '\n'.join(output)


def main():
    if len(sys.argv) != 3:
        raise SystemExit('usage: generate_mshtml_element_stubs.py MSHTML_HEADER OUTPUT')
    header = Path(sys.argv[1]).read_text()
    output = generate(header, 'IHTMLElement', 'element', 'element_vtbl')
    output += '\n' + generate(header, 'IHTMLDocument3', 'document3', 'document3_vtbl')
    Path(sys.argv[2]).write_text('/* Generated vtables; edit the generator, not this file. */\n' + output + '\n')


if __name__ == '__main__':
    main()
