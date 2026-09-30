#!/usr/bin/env python3
"""Exercise production flip addresses, generation rejection and ownership."""
from pathlib import Path
from harness import extract_section, run_harness
root = Path(__file__).resolve().parents[2]
source = root / 'triton-kmd/viogpu/viogpu3d/viogpu_vidpn.cpp'
helper = extract_section(source,
    'BOOLEAN VioGpuVidPN::SetScanoutSourceIfGeneration(',
    'D3DDDI_VIDEO_PRESENT_SOURCE_ID VioGpuVidPN::FindSourceForTarget(')
run_harness(Path(__file__).with_suffix('.cpp'), helper)
