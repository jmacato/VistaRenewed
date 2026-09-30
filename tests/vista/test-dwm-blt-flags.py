#!/usr/bin/env python3
from pathlib import Path
from harness import extract_function, run_harness
source=Path(__file__).resolve().parents[2]/'triton-umd/src/virtio/neptune/vista-d3d9/triton9_output.c'
helper=extract_function(source, 'static BOOL\ntriton9BltFlagsSupported(')
run_harness(Path(__file__).with_suffix('.cpp'),helper)
