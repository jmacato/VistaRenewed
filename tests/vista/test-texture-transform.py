#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""CPU SM2 execution of the production generator; no GPU/Windows acceptance claim."""
from pathlib import Path
import signal
import subprocess
import tempfile
from harness import extract_function, run_harness
from test_config import native_headers

root = Path(__file__).resolve().parents[2]
source = root / 'triton-umd/src/virtio/neptune/vista-d3d9'
upload = extract_function(source / 'triton9_shader.cpp', 'static HRESULT\ntriton9UploadFixedProgram(')
def run(generator, upload_source):
    run_harness(Path(__file__).with_suffix('.cpp'), upload_source, flags=(
        *native_headers(), '-I' + str(source), str(generator),
        '-fsanitize=undefined', '-fno-sanitize-recover=all'))


run(source / 'triton9_fixed.cpp', upload)
original = (source / 'triton9_fixed.cpp').read_text()
mutants = [
    ('matrix transpose', 'm.m[j][i]', 'm.m[i][j]'),
    ('stage matrix selection', 'c(72+stage*4)', 'c(72)'),
    ('homogeneous padding', '1u<<components),c(0,rep(1))', '1u<<components),c(0,0)'),
]
with tempfile.TemporaryDirectory(prefix='texture-transform-controls-') as temporary:
    for name, before, after in mutants:
        assert original.count(before) == 1, name
        candidate = Path(temporary) / 'generator.cpp'
        candidate.write_text(original.replace(before, after))
        try:
            run(candidate, upload)
        except subprocess.CalledProcessError as error:
            # A compile failure or sanitizer error is not proof the oracle caught it.
            assert error.returncode == -signal.SIGABRT, (name, error)
        else:
            raise AssertionError('surviving negative control: ' + name)
        print('rejected negative control: ' + name, flush=True)
    assert upload.count('buffer->dirty = TRUE;') == 1
    try:
        run(source / 'triton9_fixed.cpp', upload.replace('buffer->dirty = TRUE;', 'buffer->dirty = FALSE;'))
    except subprocess.CalledProcessError as error:
        assert error.returncode == -signal.SIGABRT, error
    else:
        raise AssertionError('surviving negative control: dirty upload')
    print('rejected negative control: dirty upload', flush=True)
