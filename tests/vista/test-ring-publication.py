#!/usr/bin/env python3
from pathlib import Path
from harness import extract_section, run_harness
source = Path(__file__).resolve().parents[2] / 'triton-umd/src/virtio/neptune/npt_ring.c'
helper = extract_section(source, 'static inline void\nnpt_ring_store_tail(', '\nstatic inline bool\nnpt_ring_has_space(')
helper += extract_section(source, 'static bool\nnpt_ring_submit_locked(', '\nstatic bool\nnpt_ring_submit_dispatch_locked(')
helper += extract_section(source, 'static bool\nnpt_ring_submit_locked_split(', '\n/* Indirect path twin')
run_harness(Path(__file__).with_suffix('.c'), helper)
