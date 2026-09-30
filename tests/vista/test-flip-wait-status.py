#!/usr/bin/env python3
from pathlib import Path
from harness import extract_section, run_harness
source = Path(__file__).resolve().parents[2] / 'triton-kmd/viogpu/viogpu3d/viogpu_command.cpp'
helper = extract_section(source, 'void VioGpuCommand::RecordFailure(NTSTATUS status)', 'void VioGpuCommand::RecordIssueFailure(')
block = extract_section(source, '    BOOLEAN synchronizedFlip = FALSE;',
    '    if (NT_SUCCESS((NTSTATUS)InterlockedCompareExchange(&m_failureStatus, 0, 0)) &&\n        m_pScanoutSourceCompletion != NULL)')
helper += 'bool VioGpuCommand::Begin() {\n' + block + '\nreturn synchronizedFlip;\n}\n'
run_harness(Path(__file__).with_suffix('.cpp'), helper)
