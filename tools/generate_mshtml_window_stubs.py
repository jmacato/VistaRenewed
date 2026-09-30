#!/usr/bin/env python3
"""Generate the exact SDK window vtable; unimplemented methods fail explicitly."""
import re
import sys
from pathlib import Path


def main():
    header = Path(sys.argv[1]).read_text()
    block = header.split('typedef struct IHTMLWindow2Vtbl {', 1)[1].split('} IHTMLWindow2Vtbl;', 1)[0]
    methods = re.findall(r'(HRESULT|ULONG) \(STDMETHODCALLTYPE \*(\w+)\)\(\s*IHTMLWindow2 \*This(?:,\s*(.*?))?\);', block, re.S)
    if len(methods) < 70:
        raise ValueError('incomplete IHTMLWindow2 SDK vtable')
    bodies = {
        'QueryInterface': 'return window_query_interface(from_window(This), riid, ppvObject);',
        'AddRef': 'return document_add_ref(from_window(This));',
        'Release': 'return document_release(from_window(This));',
        'GetTypeInfoCount': 'if(!pctinfo) return E_POINTER; *pctinfo = 0; return S_OK;',
        'GetTypeInfo': 'if(!ppTInfo) return E_POINTER; *ppTInfo = NULL; return DISP_E_BADINDEX;',
        'GetIDsOfNames': 'return window_ids(This, riid, rgszNames, cNames, rgDispId);',
        'Invoke': 'return window_invoke(This, dispIdMember, riid, wFlags, pDispParams, pVarResult);',
        'get_document': 'if(!p) return E_POINTER; *p = &from_window(This)->IHTMLDocument2_iface; document_add_ref(from_window(This)); return S_OK;',
        'navigate': 'return navigate_document_url(from_window(This), url);',
        'get_closed': 'if(!p) return E_POINTER; *p = from_window(This)->window_closed ? VARIANT_TRUE : VARIANT_FALSE; return S_OK;',
        'get_name': 'if(!p) return E_POINTER; *p = SysAllocString(from_window(This)->window_name ? from_window(This)->window_name : L""); return *p ? S_OK : E_OUTOFMEMORY;',
        'put_name': 'return window_set_name(from_window(This), v);',
        'focus': 'focus_supermium_window(from_window(This)); return S_OK;',
        'execScript': 'return window_exec_script(from_window(This), code, language, pvarRet);',
    }
    for name in ('get_parent', 'get_self', 'get_top', 'get_window'):
        bodies[name] = 'if(!p) return E_POINTER; *p = This; document_add_ref(from_window(This)); return S_OK;'
    bodies['get_frames'] = 'if(!p) return E_POINTER; *p = (IHTMLFramesCollection2 *)This; document_add_ref(from_window(This)); return S_OK;'
    lines = ['/* Generated from the SDK IHTMLWindow2Vtbl; do not edit. */']
    for result, name, params in methods:
        params = re.sub(r'\s+', ' ', params or '').strip()
        signature = 'IHTMLWindow2 *This' + (', ' + params if params else '')
        lines.extend([f'static {result} STDMETHODCALLTYPE window2_{name}({signature})', '{'])
        if name in bodies:
            lines.append('    ' + bodies[name].replace('; ', ';\n    '))
        else:
            # Initialize conventional output pointers without pretending to
            # implement the associated capability. Exact SDK ABI is retained.
            if name.startswith('get_') and re.search(r'\*\s*p$', params):
                if 'VARIANT *p' in params:
                    lines.append('    if(p) VariantInit(p);')
                else:
                    lines.append('    if(p) *p = 0;')
            lines.extend([f'    append_text(L"IHTMLWINDOW2_UNIMPLEMENTED={name}\\r\\n");', '    return E_NOTIMPL;'])
        lines.append('}')
    lines.append('static IHTMLWindow2Vtbl window2_vtbl = {')
    lines.extend('    window2_' + name + ',' for _, name, _ in methods)
    lines.append('};')
    Path(sys.argv[2]).write_text('\n'.join(lines) + '\n')
    print('IHTMLWINDOW2_VTABLE_GENERATED methods=' + str(len(methods)))


if __name__ == '__main__':
    main()
