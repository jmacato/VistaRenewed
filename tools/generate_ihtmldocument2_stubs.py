#!/usr/bin/env python3
"""Generate exact x86 IHTMLDocument2 vtable stubs from the MinGW Vista header."""
from __future__ import annotations

import re
import sys
from pathlib import Path


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit("usage: generate_ihtmldocument2_stubs.py MSHTML_HEADER OUTPUT")
    header = Path(sys.argv[1]).read_text(encoding="utf-8")
    output = Path(sys.argv[2])
    start = header.find("typedef struct IHTMLDocument2Vtbl")
    end = header.find("} IHTMLDocument2Vtbl;", start)
    if start < 0 or end < 0:
        raise SystemExit("IHTMLDocument2Vtbl is unavailable in the supplied header")
    block = header[start:end]
    methods = re.findall(
        r"(HRESULT|ULONG) \(STDMETHODCALLTYPE \*(\w+)\)\(\s*"
        r"IHTMLDocument2 \*This(?:,\s*(.*?))?\);",
        block,
        re.DOTALL,
    )
    if len(methods) < 100:
        raise SystemExit(f"expected a complete IHTMLDocument2 vtable, found {len(methods)} methods")
    lines = ["/* Generated from the toolchain's IHTMLDocument2Vtbl; do not edit. */"]
    entries: list[str] = []
    for result, name, raw_params in methods:
        params = re.sub(r"\s+", " ", raw_params or "").strip()
        function = "html2_" + name
        signature = "IHTMLDocument2 *This" + (", " + params if params else "")
        lines.append(f"static {result} STDMETHODCALLTYPE {function}({signature})")
        lines.append("{")
        if name == "QueryInterface":
            lines.append("    return document_query_interface(from_html2(This), riid, ppvObject);")
        elif name == "AddRef":
            lines.append("    return document_add_ref(from_html2(This));")
        elif name == "Release":
            lines.append("    return document_release(from_html2(This));")
        elif name == "Invoke":
            lines.append("    return dispatch_invoke(&from_html2(This)->IDispatch_iface, dispIdMember, riid, lcid, wFlags, pDispParams, pVarResult, pExcepInfo, puArgErr);")
        elif name == "GetIDsOfNames":
            lines.append("    return dispatch_get_ids_of_names(&from_html2(This)->IDispatch_iface, riid, rgszNames, cNames, lcid, rgDispId);")
        elif name == "GetTypeInfoCount":
            lines.append("    return dispatch_get_type_info_count(&from_html2(This)->IDispatch_iface, pctinfo);")
        elif name == "GetTypeInfo":
            lines.append("    return dispatch_get_type_info(&from_html2(This)->IDispatch_iface, iTInfo, lcid, ppTInfo);")
        elif name == "put_URL":
            lines.append("    return html2_put_url(This, v);")
        elif name == "get_URL":
            lines.append("    return html2_get_url(This, p);")
        elif name == "get_readyState":
            lines.append("    return html2_ready_state(This, p);")
        elif name == "get_parentWindow":
            lines.append("    return document_parent_window(from_html2(This), p);")
        elif name == "get_title":
            lines.append("    return content_get_title(from_html2(This), p);")
        elif name == "put_title":
            lines.append("    return content_put_title(from_html2(This), v);")
        elif name in ("write", "writeln"):
            lines.append(f"    return content_write(from_html2(This), psarray, {'TRUE' if name == 'writeln' else 'FALSE'});")
        elif name == "close":
            lines.append("    return content_close(from_html2(This));")
        elif name == "get_body":
            lines.append('    return document_element(from_html2(This), L"function(){return this.body;}", NULL, FALSE, p);')
        elif name == "createElement":
            lines.append('    return document_element(from_html2(This), L"function(s){return this.createElement(s);}", eTag, TRUE, newElem);')
        else:
            lines.append(f"    return html2_unimplemented(This, L\"{name}\");")
        lines.append("}")
        entries.append(function)
    lines.append("static IHTMLDocument2Vtbl document2_vtbl = {")
    lines.extend(f"    {entry}," for entry in entries)
    lines.append("};")
    output.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"IHTMLDOCUMENT2_STUBS_GENERATED methods={len(methods)}")


if __name__ == "__main__":
    main()
