#!/usr/bin/env python3
"""Exercise production ring sleep and failed-encoder helpers."""
from pathlib import Path
from harness import run_harness

root = Path(__file__).resolve().parents[2]

def function(path, name):
    text = (root / path).read_text()
    at = text.index(name + "(")
    start = text.rfind("\n", 0, text.rfind("\n", 0, at)) + 1
    brace = text.index("{", at)
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]

guest = "triton-umd/src/virtio/neptune/"
helpers = function(guest + "npt_cs.h", "npt_cs_encoder_write")
helpers += "\n" + function(guest + "npt_ring.c", "npt_ring_submit_command_init")
helpers += "\n" + function("triton-virglrenderer/src/neptune/npt_ring.c", "npt_ring_prepare_wait")
run_harness(Path(__file__).with_suffix(".c"), helpers)
