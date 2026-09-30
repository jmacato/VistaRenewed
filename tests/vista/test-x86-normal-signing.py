#!/usr/bin/env python3
"""Exercise clean installs and legacy boot-policy migration on both architectures."""
from pathlib import Path

from harness import extract_section, run_harness

ROOT = Path(__file__).resolve().parents[2]
source = ROOT / 'packaging/vista-driver-deploy-service.c'
helper = extract_section(
    source,
    'static BOOL configure_boot_integrity(void)',
    'static BOOL buffer_contains_ascii',
)
harness = Path(__file__).with_suffix('.c')
for architecture in ("x86", "x64"):
    print(f"Testing {architecture} boot policy", flush=True)
    flags = ["-D_WIN64"] if architecture == "x64" else []
    run_harness(harness, helper, flags=flags)
