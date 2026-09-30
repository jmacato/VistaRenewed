#!/usr/bin/env python3
"""CPU-only execution of real QEMU copy/cache/recovery functions with injected APIs."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[2]
SOURCE=ROOT/'triton-qemu/hw/display/virtio-gpu-virgl.c'
FIXTURE=Path(__file__).with_suffix('.c')
def function(text,name):
    match=re.search(r'^(?:static )?(?:struct VirtIOGPUExternalContext|void|int|bool)\s*\n?'+name+r'\([^;]*?\n\{.*?^\}',text,re.M|re.S)
    if not match: raise ValueError(name)
    return match.group(0)+'\n'
def run(directory):
    directory.mkdir(parents=True,exist_ok=True)
    (directory/'run-start').open('x').close()
    text=SOURCE.read_text()
    structs=text[text.index('struct VirtIOGPUExternalCopy {'):text.index('static struct VirtIOGPUExternalContext')]
    names=['virtio_gpu_external_context','virtio_gpu_external_stop','virtio_gpu_virgl_recover_context','virtio_gpu_neptune_copy_destroy','virtio_gpu_neptune_external_only','virtio_gpu_neptune_external_copy']
    wrappers=structs+'\n'.join(function(text,name) for name in names)
    scanout=function(text,'virtio_gpu_neptune_scanout_fallback')+'\n'+function(text,'virtio_gpu_neptune_gpu_scanout')
    template=FIXTURE.read_text()
    classification=function(text,'virtio_gpu_neptune_copy_resource').replace('virtio_gpu_neptune_copy_resource(', 'classify_resource(',1)
    transfer=function(text,'virtio_gpu_neptune_transfer_dmabuf').replace('virtio_gpu_neptune_transfer_dmabuf(', 'classify_transfer(',1)
    code=template.replace('/* WRAPPERS_UNDER_TEST */',wrappers).replace('/* SCANOUT_UNDER_TEST */',scanout).replace('/* CLASSIFICATION_UNDER_TEST */',classification).replace('/* TRANSFER_UNDER_TEST */',transfer)
    variants={'production':code,
      'modifier-zero-skipped':code.replace('if (modifiers[i] == modifier)', 'if (modifier && modifiers[i] == modifier)'),
      'flip-origin-wrong':code.replace('dy = flip_target ? target_y - height : target_y;', 'dy = target_y;'),
      'destroy-retained-pointer-lost':code.replace('if (!copy->helper) {\n        g_clear_pointer(slot, g_free);', 'if (true) {\n        g_clear_pointer(slot, g_free);'),
      'context-failure-fallback':code.replace('return gpu_required ? ret : 0;', 'return 0;'),
      'delete-pending-target':code.replace('gl->scanout_pending_texture[index] = texture;', 'glDeleteTextures(1, &texture); gl->scanout_pending_texture[index] = texture;'),
      'recover-unblocks-everything':code.replace('g->parent_obj.renderer_blocked--;','g->parent_obj.renderer_blocked = 0;'),
      'unknown-export-cpu-fallback':code.replace('    *gpu_required = true;\n    if (res->is_blob)', '    *gpu_required = false;\n    if (res->is_blob)'),
      'renderable-import-cpu-fallback':code.replace('    ret = -EIO;\n    direct_read =', '    *gpu_required = external_only;\n    ret = -EIO;\n    direct_read =')}
    flags=subprocess.check_output(['pkg-config','--cflags','--libs','glib-2.0','epoxy'],text=True).split()
    results={}
    for name,body in variants.items():
        path=directory/(name+'.c');path.write_text(body);binary=directory/name
        build=subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-I',str(ROOT/'triton-qemu/include'),str(path),'-o',str(binary),*flags],capture_output=True,text=True)
        (directory/(name+'-build.log')).write_text(build.stdout+build.stderr);build.check_returncode()
        result=subprocess.run([str(binary)],capture_output=True,text=True,timeout=10)
        (directory/(name+'.log')).write_text(result.stdout+result.stderr)
        results[name]={'exit_code':result.returncode,'output':result.stdout+result.stderr}
        if name=='production': result.check_returncode()
        elif result.returncode==0 or 'CHECK FAILED:' not in result.stderr: raise RuntimeError('undetected mutant: '+name)
        print(name+': '+str(result.returncode)+' '+(result.stdout+result.stderr).strip())
    (directory/'results.json').write_text(json.dumps(results,indent=2)+'\n')
    (directory/'source-hashes.json').write_text(json.dumps({str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in [SOURCE,FIXTURE,Path(__file__)]},indent=2)+'\n')
if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--evidence',type=Path);args=parser.parse_args()
    if args.evidence: run(args.evidence.resolve())
    else:
        with tempfile.TemporaryDirectory(prefix='external-copy-integration-') as d: run(Path(d))
