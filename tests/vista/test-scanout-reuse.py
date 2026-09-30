#!/usr/bin/env python3
"""Test the production Neptune scanout binding helper."""
from pathlib import Path
from harness import extract_section, run_harness

root = Path(__file__).resolve().parents[2]
helper = extract_section(
    root / "triton-qemu/hw/display/virtio-gpu-virgl.c",
    "static bool virtio_gpu_neptune_prepare_scanout(",
    "static DisplaySurface *virtio_gpu_neptune_create_surface(",
)
run_harness(Path(__file__).with_suffix(".c"), helper)
