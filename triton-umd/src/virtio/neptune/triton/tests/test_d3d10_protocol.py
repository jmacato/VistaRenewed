#!/usr/bin/env python3
"""Verify paired wire revision and execute each actual transport version guard.

This is a CPU branch/ordering regression, not a socket or GPU transport test.
The generated registry/scripts are absent from this checkout; the revision was
stamped consistently in the two generated definition headers, not regenerated.
"""
from pathlib import Path
import re
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[6]
NPT=ROOT/'triton-umd/src/virtio/neptune'
HOST=ROOT/'triton-virglrenderer/src/neptune'
versions=[]
for directory in (NPT,HOST):
    text=(directory/'neptune-protocol/npt_protocol_defs.h').read_text()
    versions.append(int(re.search(r'#define NPT_PROTOCOL_WIRE_VERSION (\d+)u',text).group(1)))
assert versions==[4,4],versions
assert 'c->wire_format_version = NPT_PROTOCOL_WIRE_VERSION' in (HOST/'npt_renderer.c').read_text()
paths=[('npt_renderer_virtgpu_win32.c','VIOGPU_CTX_INIT'),('npt_renderer_virtgpu_wine.c','unixlib_virtgpu_context_init'),('npt_renderer_vtest_wine.c','npt_vtest_vcmd_context_init')]
for name,init in paths:
    text=(NPT/name).read_text()
    match=re.search(r'if \((\w+\.wire_format_version != NPT_PROTOCOL_WIRE_VERSION)\) \{',text)
    assert match,name
    end=text.index('\n   }',match.end()) if name!='npt_renderer_virtgpu_win32.c' else text.index('\n   }',match.end())
    block=text[match.start():end]
    assert ('goto fail;' in block or 'return NULL;' in block),name
    assert text.index(init,match.start())>end,name
    expression=match.group(1).replace('NPT_PROTOCOL_WIRE_VERSION','guest')
    expression=re.sub(r'\w+\.wire_format_version','host',expression)
    for variant in ('current','one-way'):
        condition=expression if variant=='current' else expression.replace('!=','<')
        program='#include <stdio.h>\nstatic int rejected(unsigned guest,unsigned host){return '+condition+';}\nint main(void){if(!rejected(3,4)||!rejected(4,3)||rejected(4,4))return 1;puts("paired versions reject 3/4 and 4/3, accept 4/4");return 0;}\n'
        with tempfile.TemporaryDirectory(prefix='triton-wire-version-') as d:
            d=Path(d);(d/'test.c').write_text(program)
            subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror',str(d/'test.c'),'-o',str(d/'test')],check=True)
            result=subprocess.run([str(d/'test')],capture_output=True,text=True)
            assert (result.returncode==0)==(variant=='current'),name+variant
            print(name+' '+variant+': '+('PASS' if variant=='current' else 'negative control rejected'))
print('D3D10 protocol version checks passed (CPU guards; actual transport handshake pending)')
