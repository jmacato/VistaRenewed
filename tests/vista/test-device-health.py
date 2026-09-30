#!/usr/bin/env python3
"""Hot DDI checks must not serialize draws; boundaries still detect removal."""
from pathlib import Path
from harness import extract_section, run_harness
source = Path(__file__).resolve().parents[2] / 'triton-umd/src/virtio/neptune/vista-d3d9/triton9.h'
helper = extract_section(source, 'static inline HRESULT triton9CheckHostDevice(', '\n#ifdef __cplusplus\n}\n#endif')
run_harness(Path(__file__).with_suffix('.c'), helper)
