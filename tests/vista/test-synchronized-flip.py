#!/usr/bin/env python3
from pathlib import Path
from harness import extract_section, run_harness
source = Path(__file__).resolve().parents[2] / 'triton-kmd/viogpu/viogpu3d/viogpu_vidpn.cpp'
helper = extract_section(source, 'NTSTATUS VioGpuVidPN::BeginSynchronizedFlip(', 'void VioGpuVidPN::VsyncThread(')
run_harness(Path(__file__).with_suffix('.cpp'), helper)
