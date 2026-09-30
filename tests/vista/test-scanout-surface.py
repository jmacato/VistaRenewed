#!/usr/bin/env python3
"""Exercise production display storage reuse across primary format changes."""
from pathlib import Path
import subprocess
from harness import extract_section, run_harness

source = Path(__file__).resolve().parents[2] / 'triton-qemu/hw/display/virtio-gpu-virgl.c'
code = extract_section(source, 'static bool virtio_gpu_neptune_prepare_scanout(',
                       '\nstatic DisplaySurface *virtio_gpu_neptune_create_surface(')
flags = subprocess.check_output(['pkg-config', '--cflags', 'pixman-1'], text=True).split()
run_harness(Path(__file__).with_suffix('.cpp'), code,
            flags=[*flags, '-Wno-sign-compare'])
