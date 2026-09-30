#!/usr/bin/env python3
"""Check the production EDID and vblank deadline arithmetic."""
from pathlib import Path
import subprocess
import tempfile
source = Path(__file__).with_suffix('.cpp')
with tempfile.TemporaryDirectory(prefix='triton-vsync-') as tmp:
    binary = Path(tmp) / 'test'
    subprocess.run(['clang++', '-std=c++11', '-Wall', '-Wextra', '-Werror',
                    str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
