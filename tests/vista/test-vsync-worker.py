#!/usr/bin/env python3
"""Run the actual vblank loop against early/late timer wakes and a busy copy."""
from pathlib import Path
from harness import extract_section, run_harness

source = Path(__file__).resolve().parents[2] / 'triton-kmd/viogpu/viogpu3d/viogpu_vidpn.cpp'
helper = extract_section(source, 'void VioGpuVidPN::Flip()', '\nD3DDDI_RATIONAL VioGpuVidPN::GetActiveRefreshRate()')
helper += extract_section(source, 'static ULONGLONG VioGpuVsyncNow(', '\nvoid VioGpuVidPN::PublishRasterTiming(')
helper += extract_section(source, 'void VioGpuVidPN::VsyncThread(', '\nNTSTATUS VioGpuVidPN::SetVidPnSourceAddress(')
run_harness(Path(__file__).with_suffix('.cpp'), helper)
