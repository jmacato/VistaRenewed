#!/usr/bin/env python3
"""Exercise same-context presentation consumption and failure cleanup."""
from pathlib import Path

from harness import extract_function, run_harness

ROOT = Path(__file__).resolve().parents[2]
source = ROOT / 'triton-umd/src/virtio/neptune/vista-d3d9/triton9_resource.c'
helper = extract_function(
    source,
    'static HRESULT\ntriton9WaitForPresentConsumption(',
)
harness = Path(__file__).with_suffix('.c')
run_harness(harness, helper)
