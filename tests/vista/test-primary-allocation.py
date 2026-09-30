#!/usr/bin/env python3
"""An allocation must not display pixels or invalidate an already queued flip."""
from pathlib import Path
from harness import extract_section, run_harness

root = Path(__file__).resolve().parents[2]
allocation = root / 'triton-kmd/viogpu/viogpu3d/viogpu_allocation.cpp'
finish = extract_section(allocation,
    '    if (resource != NULL)\n    {\n        pCreateAllocation->hResource',
    '\nCreateAllocationFailed:')
vidpn = root / 'triton-kmd/viogpu/viogpu3d/viogpu_vidpn.cpp'
transition = extract_section(vidpn,
    'void VioGpuVidPN::SetScanoutSource(VioGpuAllocation *res)',
    '\nBOOLEAN VioGpuVidPN::IsScanoutSourceCompatible(')
helper = transition + '''
static int FinishAllocation(Resource *resource, VioGpuAllocation *primaryAllocation,
                            Adapter *adapter, Output *pCreateAllocation)
{
    (void)adapter;
''' + finish + '\n}\n'
run_harness(Path(__file__).with_suffix('.cpp'), helper)
