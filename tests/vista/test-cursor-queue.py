#!/usr/bin/env python3
"""Exercise the production cursor submission at page boundaries and queue errors."""
from pathlib import Path
from harness import extract_section, run_harness
source = Path(__file__).resolve().parents[2] / 'triton-kmd/viogpu/common/viogpu_queue.cpp'
code = extract_section(source, 'static BOOLEAN VioGpuBuildSgRange(', '\n// Vista\'s x86 kernel')
code += extract_section(source, 'UINT CrsrQueue::QueueCursor(', '\nPGPU_VBUFFER CrsrQueue::DequeueCursor')
run_harness(Path(__file__).with_suffix('.cpp'), code)
