#!/usr/bin/env python3
"""Exercise production DMA-flip translation, including Vista's no-wait flag."""
from pathlib import Path
from harness import extract_section, run_harness
root = Path(__file__).resolve().parents[2]
source = root / 'triton-kmd/viogpu/viogpu3d/viogpu_device.cpp'
branch = extract_section(source, '    if (pPresent->Flags.Flip)\n', '\n    DbgPrint(TRACE_LEVEL_VERBOSE,\n             ("<---> %s Flags=')
helper = 'NTSTATUS VioGpuDevice::Present(DXGKARG_PRESENT *pPresent) {\n' + branch + '\nreturn STATUS_NOT_SUPPORTED;\n}\n'
run_harness(Path(__file__).with_suffix('.cpp'), helper)
