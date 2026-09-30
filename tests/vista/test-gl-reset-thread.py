#!/usr/bin/env python3
"""Run production virtio-gpu reset paths across vCPU/main pthreads (no GPU)."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
BASE = ROOT / 'triton-qemu/hw/display/virtio-gpu.c'
GL = ROOT / 'triton-qemu/hw/display/virtio-gpu-gl.c'
VIRGL = ROOT / 'triton-qemu/hw/display/virtio-gpu-virgl.c'
HEADER = ROOT / 'triton-qemu/include/hw/virtio/virtio-gpu.h'
FIXTURE = Path(__file__).with_suffix('.c')


def function(text, name):
    # Extract from a full definition through its unindented closing brace.
    match = re.search(r'^(?:static )?(?:void|int|bool)\s*\n?' + name +
                      r'\([^;]*?\n\{.*?^\}', text, re.M | re.S)
    if not match:
        raise ValueError(f'Function definition missing: {name}')
    return match.group(0) + '\n'


def generated_sources():
    base, gl, virgl = (path.read_text() for path in (BASE, GL, VIRGL))
    assert 'bool (*reset)(VirtIOGPU *g);' in HEADER.read_text()
    base_class = function(base, 'virtio_gpu_class_init')
    gl_class = function(gl, 'virtio_gpu_gl_class_init')
    base_reset = re.findall(r'vdc->reset = .*?;', base_class)
    gl_reset = re.findall(r'(?:vdc|vgc)->reset = .*?;', gl_class)
    assert len(base_reset) == len(gl_reset) == 1
    registration = ('static void install_reset_callbacks(VirtioDeviceClass *vdc, '
                    'VirtIOGPUClass *vgc)\n{\n' + base_reset[0] + '\n' +
                    gl_reset[0] + '\n}\n')
    code = '\n'.join([
        function(base, 'virtio_gpu_disable_scanout'),
        function(virgl, 'virtio_gpu_virgl_reset_scanout'),
        function(virgl, 'virtio_gpu_virgl_destroy_resource_copies'),
        function(gl, 'virtio_gpu_gl_update_cursor_data'),
        function(base, 'virtio_gpu_process_cmdq'),
        function(virgl, 'virtio_gpu_virgl_init'),
        function(gl, 'virtio_gpu_gl_device_unrealize'),
        function(gl, 'virtio_gpu_gl_handle_ctrl'),
        function(gl, 'virtio_gpu_gl_reset'),
        function(base, 'virtio_gpu_reset_bh'),
        function(base, 'virtio_gpu_reset'),
        function(base, 'virtio_gpu_ctrl_bh'),
        function(base, 'virtio_gpu_handle_cursor'),
        function(base, 'virtio_gpu_cursor_bh'),
        function(base, 'virtio_gpu_handle_ctrl_cb'),
        function(base, 'virtio_gpu_handle_cursor_cb'),
        registration,
    ])
    old_reset = '''static void virtio_gpu_gl_reset(VirtIODevice *vdev)
{
    VirtIOGPU *g = VIRTIO_GPU(vdev);
    VirtIOGPUGL *gl = VIRTIO_GPU_GL(vdev);
    virtio_gpu_reset(vdev);
    if (gl->renderer_state == RS_INITED) {
        virtio_gpu_virgl_reset_scanout(g);
        gl->renderer_state = RS_RESET;
    }
}
'''
    old = code.replace(function(gl, 'virtio_gpu_gl_reset'), old_reset)
    old = old.replace('vgc->reset = virtio_gpu_gl_reset;',
                      'vgc->reset = NULL; vdc->reset = virtio_gpu_gl_reset;')
    callback = ('    if (vgc->reset) {\n        reset_ok = vgc->reset(g);\n    }\n'
                '    if (!reset_ok) {\n        goto discard_commands;\n    }\n\n')
    assert code.count(callback) == 1
    late = code.replace(callback, '')
    late = late.replace('    while (!QTAILQ_EMPTY(&g->cmdq)) {\n        cmd = QTAILQ_FIRST(&g->cmdq);\n        QTAILQ_REMOVE',
                        callback + '    while (!QTAILQ_EMPTY(&g->cmdq)) {\n        cmd = QTAILQ_FIRST(&g->cmdq);\n        QTAILQ_REMOVE')
    assert late != code
    return {
        'production': code,
        'old-vcpu-gl-reset': old,
        'reset-after-resource-destroy': late,
        'control-skips-reset': code.replace(
            '    virtio_gpu_reset_bh(g);\n    vgc->handle_ctrl',
            '    vgc->handle_ctrl'),
        'cursor-skips-reset': code.replace(
            '    virtio_gpu_reset_bh(g);\n    virtio_gpu_handle_cursor',
            '    virtio_gpu_handle_cursor'),
        'duplicate-reset-statistics': code.replace('if (!gl->print_stats)', 'if (true)'),
        'duplicate-reset-timers': code.replace('if (!gl->fence_poll)', 'if (true)'),
        'duplicate-reset-bh': code.replace('if (!gl->cmdq_resume_bh)', 'if (true)'),
        'reset-ignores-failure': code.replace('reset_ok = vgc->reset(g);',
                                             'vgc->reset(g);'),
        'unrealize-skips-resource-aliases': code.replace(
            ' ||\n            !virtio_gpu_virgl_destroy_resource_copies(g)', ''),
        'commands-during-reset': code.replace(
            'if (g->reset_pending || g->processing_cmdq)',
            'if (g->processing_cmdq)'),
    }


def run(directory):
    directory.mkdir(parents=True, exist_ok=True)
    template = FIXTURE.read_text()
    assert template.count('/* SOURCE_UNDER_TEST */') == 1
    hashes = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
              for p in (BASE, GL, VIRGL, HEADER, FIXTURE, Path(__file__))}
    (directory / 'source-hashes.json').write_text(json.dumps(hashes, indent=2) + '\n')
    results = {}
    for name, code in generated_sources().items():
        source, binary = directory / (name + '.c'), directory / name
        source.write_text(template.replace('/* SOURCE_UNDER_TEST */', code))
        build = subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra',
                                '-Werror', '-Wno-unused-parameter', '-Wno-sign-compare',
                                '-pthread',
                                '-I', str(ROOT / 'triton-qemu/include'),
                                str(source), '-o', str(binary)],
                               capture_output=True, text=True)
        (directory / (name + '-build.log')).write_text(build.stdout + build.stderr)
        build.check_returncode()
        result = subprocess.run([str(binary)], capture_output=True, text=True,
                                timeout=30)
        (directory / (name + '.log')).write_text(result.stdout + result.stderr)
        results[name] = {'exit_code': result.returncode,
                         'output': result.stdout + result.stderr}
        if name == 'production':
            result.check_returncode()
        else:
            if result.returncode == 0 or 'CHECK FAILED:' not in result.stderr:
                raise RuntimeError(f'Mutant was not rejected by the oracle: {name}')
        print(f'{name}: {result.returncode} {result.stdout.strip()} '
              f'{result.stderr.strip()}', flush=True)
    (directory / 'results.json').write_text(json.dumps(results, indent=2) + '\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--evidence', type=Path)
    args = parser.parse_args()
    if args.evidence:
        run(args.evidence.resolve())
    else:
        with tempfile.TemporaryDirectory(prefix='gl-reset-thread-') as temp:
            run(Path(temp))
