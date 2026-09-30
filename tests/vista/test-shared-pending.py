#!/usr/bin/env python3
"""Run production pending-export functions with source-derived fault controls."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[2]
HOST=ROOT/'triton-virglrenderer/src/neptune'
GUEST=ROOT/'triton-umd/src/virtio/neptune'

def function(source,name):
    match=re.search(r'(?:bool|void)\n'+name+r'\(',source)
    assert match,name
    body=source.index('{',match.start());end=body+1;depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[match.start():end]

def main():
    current=(HOST/'npt_context.c').read_text()
    cancel=function(current,'npt_context_cancel_pending_blob')
    register=function(current,'npt_context_register_pending_blob')
    duplicate_guard='!_mesa_hash_table_search(ctx->pending_blob_table, &blob_id) &&'
    assert register.count(duplicate_guard)==1
    duplicate_overwrite=register.replace(duplicate_guard,'')
    assert cancel.count('close(pb->fd);')==1
    fixture=(ROOT/'tests/vista/test-shared-pending.c').read_text()
    layout=re.search(r'struct virgl_attachment_layout \{.*?\};',
        (ROOT/'triton-virglrenderer/src/virgl_resource.h').read_text(),re.S).group()
    pending=re.search(r'struct npt_pending_blob \{.*?\};',
        (HOST/'npt_context.h').read_text(),re.S).group()
    fixture=fixture.replace('// PRODUCTION_PENDING_TYPES',layout+'\n'+pending)
    with tempfile.TemporaryDirectory(prefix='npt-pending-') as tmp:
        source=Path(tmp)/'test.c';exe=Path(tmp)/'test'
        for label,body,negative in [('current',register,False),('duplicate-overwrite',duplicate_overwrite,True),
            ('cancel-leak',register,True),
            ('lost-layout',register.replace('pb->layout = *layout;', '(void)layout;'),True)]:
            cleanup=cancel.replace('close(pb->fd);','(void)pb->fd;') if label=='cancel-leak' else cancel
            source.write_text(fixture.replace('// PRODUCTION_PENDING_FUNCTIONS',cleanup+'\n'+body))
            subprocess.run([os.environ.get('CC','clang'),'-std=c11','-O1','-g','-Wall','-Wextra','-Werror',
                '-fsanitize=address,undefined',str(source),'-pthread','-o',str(exe)],check=True)
            result=subprocess.run([str(exe)],capture_output=True,text=True)
            if negative:
                assert result.returncode!=0 and 'Assertion' in result.stderr,result.stderr
                print('Pending export negative control rejected: '+label)
            else:
                assert result.returncode==0,result.stdout+result.stderr
                print(result.stdout.strip())
    # The independently compiled endpoints must agree about their addition;
    # established wire command IDs are never repurposed.
    wire=[]
    for directory in [HOST,GUEST]:
        text=(directory/'npt_transport_defs.h').read_text()
        for name,value in [('EXPORT_BLOB',0),('OPEN_RES',1),('QUERY_LAYOUT',2),('CANCEL_EXPORT',3)]:
            assert re.search(r'#define NPT_TRANSPORT_SHARED_'+name+r'\s+'+str(value)+'u',text)
        wire.append(re.search(r'struct npt_cmd_shared_cancel_export \{.*?\};.*?struct npt_cmd_shared_cancel_export_reply \{.*?\};',text,re.S).group())
    assert wire[0]==wire[1]
    print('Shared pending export checks passed')

if __name__=='__main__':main()
