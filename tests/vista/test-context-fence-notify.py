#!/usr/bin/env python3
"""Exercise notification registration and teardown using production helpers."""
from pathlib import Path
import subprocess
import tempfile
from harness import extract_section, run_harness

root = Path(__file__).resolve().parents[2]
source = root / 'triton-qemu/hw/display/virtio-gpu-virgl.c'
code = extract_section(source, '#if VIRGL_VERSION_MAJOR >= 1\ntypedef struct VirtIOGPUContextFenceWatch {',
                       '\nstatic void virgl_cmd_context_create(')
# The production file is C; make its implicit void-pointer conversions explicit
# for the C++ fixture without changing the notification logic.
code = code.replace('watch = opaque;', 'watch = static_cast<VirtIOGPUContextFenceWatch *>(opaque);')
code = code.replace('watch = value;', 'watch = static_cast<VirtIOGPUContextFenceWatch *>(value);')
flags = subprocess.check_output(['pkg-config', '--cflags', '--libs', 'glib-2.0'], text=True).split()
run_harness(Path(__file__).with_suffix('.cpp'), code, flags=flags)

# The old renderer API must still compile warning-clean, without callbacks
# that only have users in the >= 1.0 implementation.
old = '''#include <glib.h>
#include <stdint.h>
#define VIRGL_VERSION_MAJOR 0
typedef struct { GHashTable *context_fence_watches; } VirtIOGPU;
#define VIRTIO_GPU_GL(g) (g)
'''+code+'''
int main(void) {
    VirtIOGPU gpu = { 0 };
    virtio_gpu_watch_context_fences(&gpu, 1);
    virtio_gpu_virgl_clear_fence_watches(&gpu);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='old-virgl-api-') as tmp:
    source = Path(tmp) / 'old.c'
    binary = Path(tmp) / 'old'
    source.write_text(old)
    subprocess.run(['clang', '-std=c11', '-Wall', '-Werror', str(source),
                    '-o', str(binary), *flags], check=True)
    subprocess.run([str(binary)], check=True)
print('Pre-1.0 virgl API compiles without unused helpers')
