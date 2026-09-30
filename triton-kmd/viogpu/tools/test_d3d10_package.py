#!/usr/bin/env python3
"""Check native/WOW discovery and prove old/reordered registration fails."""
from pathlib import Path
import importlib.util
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
TOOLS = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('inf_check', TOOLS / 'check_vista_inf.py')
inf = importlib.util.module_from_spec(spec)
spec.loader.exec_module(inf)
for arch, name in [('x64', 'viogpu3d-diagnostic.inf'), ('x86', 'viogpu3d-diagnostic-x86.inf')]:
    path = ROOT / 'packaging' / name
    errors = inf.validate(path, arch, None)
    assert not errors, errors
    current = path.read_text()
    baseline = subprocess.check_output(['git', 'show', 'HEAD:packaging/' + name], cwd=ROOT, text=True)
    mutants = [baseline, current.replace('neptune_d3d9.dll,neptune_d3d10.dll',
                                         'neptune_d3d10.dll,neptune_d3d9.dll'),
               current.replace('neptune_d3d10.dll,,,0x00004000', ''),
               current.replace('7.15.1.0', '7.14.1.0')]
    if arch == 'x64':
        mutants.append(current.replace('neptune_d3d9_wow.dll,neptune_d3d10_wow.dll',
                                        'neptune_d3d9_wow.dll,neptune_d3d10.dll'))
    with tempfile.TemporaryDirectory(prefix='triton10-inf-') as tmp:
        for index, text in enumerate(mutants):
            mutant = Path(tmp) / f'invalid-{index}.inf'
            mutant.write_text(text)
            assert inf.validate(mutant, arch, None), (arch, index)
    print(f'D3D10 {arch} INF checks and {len(mutants)} negative controls passed')
