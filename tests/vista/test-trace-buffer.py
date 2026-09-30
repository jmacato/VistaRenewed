#!/usr/bin/env python3
"""Stress the production UMD recorder across concurrent STOP/START."""
from pathlib import Path
from harness import extract_section, run_harness
root = Path(__file__).resolve().parents[2]
source = root / 'triton-umd/src/virtio/neptune/vista-d3d9/triton9_trace.c'
helper = extract_section(source, 'static BOOL triton9TraceAcquire(', '\nvoid triton9TraceEvent(')
run_harness(Path(__file__).with_suffix('.c'), helper,
            flags=('-pthread', '-I' + str(root / 'triton-kmd/viogpu/shared')))
