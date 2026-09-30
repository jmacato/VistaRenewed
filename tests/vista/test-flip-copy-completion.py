#!/usr/bin/env python3
"""Exercise copy completion while a timer owns the pending flip."""
from pathlib import Path

from harness import extract_section, run_harness

ROOT = Path(__file__).resolve().parents[2]
source = ROOT / 'triton-kmd/viogpu/viogpu3d/viogpu_vidpn.cpp'
helper = extract_section(
    source,
    'BOOLEAN VioGpuVidPN::TryPromoteFlip()',
    'BOOLEAN VioGpuVidPN::TryPromoteFlipLocked()',
)
harness = Path(__file__).with_suffix('.cpp')
run_harness(harness, helper, flags=["-pthread"])
