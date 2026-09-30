#!/usr/bin/env python3
"""Verify query dispatch and creation, with the old feedback path as a control."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
NPT = ROOT / "triton-umd/src/virtio/neptune"


def function(source, name):
    match = re.search(r"\n" + name + r"\([^;{}]*\)\s*\{", source)
    if not match:
        raise ValueError("missing production function " + name)
    start = source.rfind("\n\n", 0, match.start()) + 2
    brace = source.index("{", match.start())
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def without_includes(source):
    return re.sub(r'^#include [^\n]+\n', '', source, flags=re.M)


def generate(context, query):
    client = (NPT / "neptune-protocol/npt_protocol_client_id3d11devicecontext.c").read_text()
    methods = "\n".join(function(client, "npt_id3d11devicecontext_default_" + name)
                        for name in ("Begin", "End", "GetData"))
    tables = []
    for tier in range(5):
        name = "npt_id3d11devicecontext" + (str(tier) if tier else "") + "_fill_default_vtbl"
        assignments = re.findall(r"dst->(?:Begin|End|GetData) = [^;]+;", function(client, name))
        assert len(assignments) == 3, name
        tables.extend(line.replace("dst->", f"contexts[{tier}].") for line in assignments)
    first = context.index("#define NPT_REGISTER_OVERRIDE_D3D11_DEVICE_CONTEXT4")
    last = context.index("static uint32_t", first)
    context_source = context[first:last]
    if "ctx_query_slot(" in context:
        context_source += "\n".join(function(context, name) for name in
                                     ("ctx_query_slot", "ctx_Begin_override", "ctx_GetData_override"))
    context_source += function(context, "npt_overrides_d3d11_context_init")
    constants = []
    for token in sorted(set(re.findall(r'\bD3D11_QUERY_[A-Z0-9_]+', query))):
        if not token.startswith(("D3D11_QUERY_DATA_", "D3D11_QUERY_DESC")):
            constants.append(token)
    declarations = "enum { " + ", ".join(constants) + " };\n"
    for index, token in enumerate(sorted(set(re.findall(r'\bNPT_IID_\w+', context_source + query)))):
        declarations += f"static const GUID {token} = {index};\n"
    for token in sorted(set(re.findall(r'\bnpt_\w+_default_vtbl_storage', query))):
        declarations += f"static const int {token};\n"
    template = Path(__file__).with_suffix('.c').read_text()
    replacements = {"GENERATED_CONSTANTS": declarations, "AUX_SOURCE": without_includes((NPT / "npt_overrides_d3d11_feedback.h").read_text()),
                    "DEFAULT_METHODS": methods, "DEFAULT_TABLES": "\n".join(tables),
                    "QUERY_SOURCE": without_includes(query), "CONTEXT_SOURCE": context_source}
    for marker, value in replacements.items():
        template = template.replace(f"/* {marker} */", value)
    return template


context = (NPT / "npt_overrides_d3d11_context.c").read_text()
query = (NPT / "npt_overrides_d3d11_query.c").read_text()
# Explicit source-derived fault controls reproduce the unsafe outcomes without
# requiring a local git history or review snapshot.
with tempfile.TemporaryDirectory(prefix="query-transport-") as tmp:
    tmp = Path(tmp)
    for variant in ("current", "registration", "stale-result", "stale-generation"):
        code = generate(context, query)
        if variant == "registration":
            old = "aux->query_data_size = npt_query_data_size_for_type(type);"
            assert old in code
            code = code.replace(old, "{ " + old + " npt_dispatch_feedback_register_query(dev->ring, com->base.id, 1, 0, aux->query_data_size); }")
        elif variant in ("stale-result", "stale-generation"):
            old = function((NPT / "neptune-protocol/npt_protocol_client_id3d11devicecontext.c").read_text(), "npt_id3d11devicecontext_default_GetData")
            assert old in code
            at = old.index("{") + 1
            wrong = ("if (pData && DataSize) memcpy(pData, slot.result, DataSize); return NPT_S_OK;"
                     if variant == "stale-result" else "return NPT_S_FALSE;")
            # Retain the function signature, replace the ordered body.
            signature = old[:at]
            code = code.replace(old, signature + "(void)self;(void)pAsync;(void)pData;(void)DataSize;(void)GetDataFlags;" + wrong + "}")
        c_file = tmp / "query.c";binary = tmp / "query";c_file.write_text(code)
        subprocess.run(["clang", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-function", "-fsanitize=address,undefined",
                        str(c_file), "-o", str(binary)], check=True)
        mode = {"stale-result":"end-only", "stale-generation":"begin-end"}.get(variant,"all")
        result = subprocess.run([str(binary), mode], capture_output=True, text=True)
        if variant != "current":
            expected = {"registration":"query registered shared feedback",
                        "stale-result":"reissued END-only query returned stale completion",
                        "stale-generation":"completed BEGIN/END query remains pending"}[variant]
            assert result.returncode and expected in result.stderr, result.stdout + result.stderr
            print("Query transport source mutant rejected:", variant)
        else:
            assert result.returncode == 0, result.stdout + result.stderr
            print(result.stdout.strip())
subprocess.run(["python3", str(ROOT / "tests/vista/test-fence-health.py")], check=True)
print("Query transport checks passed")
