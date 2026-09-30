#!/usr/bin/env python3
"""Execute actual paired color-copy dispatch with bounded transport/error models."""
from pathlib import Path
import importlib.util
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
NPT = ROOT / 'triton-umd/src/virtio/neptune'
HOST = ROOT / 'triton-virglrenderer/src/neptune'
spec = importlib.util.spec_from_file_location('extract', ROOT / 'tests/vista/test-d3d10-texture-map.py')
extract = importlib.util.module_from_spec(spec)
spec.loader.exec_module(extract)
def declaration(text, name):
    start = text.index('struct ' + name + ' {')
    return text[start:text.index('};', start) + 2]
wire = (NPT / 'npt_transport_defs.h').read_text()
host_wire = (HOST / 'npt_transport_defs.h').read_text()
protocol = (NPT / 'neptune-protocol/npt_protocol_defs.h').read_text()
structs = [declaration(protocol, n) for n in ('npt_command_header', 'npt_reply_header')]
for name in ('npt_cmd_resource_copy_color', 'npt_cmd_resource_copy_color_reply'):
    text = declaration(wire, name)
    assert text == declaration(host_wire, name), name
    structs.append(text)
guest = extract.function((NPT / 'npt_dispatch.c').read_text(), 'npt_dispatch_resource_copy_color')
host = extract.function((HOST / 'npt_dispatch.c').read_text(), 'npt_dispatch_resource_copy_color')
host = host.replace('npt_dispatch_resource_copy_color(', 'host_copy_color(', 1)
fixture = Path(__file__).with_suffix('.c').read_text()
for variant in ('current', 'wrong-object-type', 'ignore-backend-error', 'same-ring-only'):
    g, h = guest, host
    if variant == 'wrong-object-type':
        h = h.replace('NPT_OBJECT_TYPE_ID3D11SHADERRESOURCEVIEW', 'NPT_OBJECT_TYPE_ID3D11RENDERTARGETVIEW')
    elif variant == 'ignore-backend-error':
        h = h.replace('reply.header.cmd_return = (uint32_t)(int32_t)hr;', 'reply.header.cmd_return = 0; (void)hr;')
    elif variant == 'same-ring-only':
        g = g.replace('if (!ring || !device ||', 'if (npt_com_self_ring(src_srv) != ring || !ring || !device ||')
    source = fixture.replace('// WIRE', '\n'.join(structs)).replace('// HOST', h).replace('// GUEST', g)
    with tempfile.TemporaryDirectory(prefix='triton-color-transport-') as tmp:
        p = Path(tmp); (p / 'test.c').write_text(source)
        build = subprocess.run(['clang', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
            '-Wno-unused-parameter', '-fsanitize=address,undefined', str(p/'test.c'), '-o', str(p/'test')], capture_output=True, text=True)
        assert build.returncode == 0, build.stdout + build.stderr
        run = subprocess.run([str(p/'test')], capture_output=True, text=True)
        if variant == 'current':
            assert run.returncode == 0, run.stdout + run.stderr
            print(run.stdout.strip())
        else:
            assert run.returncode != 0 and 'FAIL transport' in run.stderr, run.stdout + run.stderr
            print(variant + ' negative control rejected')
