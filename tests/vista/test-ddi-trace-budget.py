#!/usr/bin/env python3
"""Routine tracing must stop doing I/O without suppressing failure evidence."""
from pathlib import Path
from harness import extract_section, run_harness

root = Path(__file__).resolve().parents[2]
helper = extract_section(
    root / "triton-umd/src/virtio/neptune/vista-d3d9/triton9_ddi.c",
    "void\ntriton9Diag(const char *message)",
    "/* Do not use the CRT formatter",
)
helper += helper.replace("triton9Diag(", "triton9DiagVerbose(")
run_harness(Path(__file__).with_suffix(".c"), helper)
