#!/usr/bin/env python3
"""Validate EGL export metadata and fd ownership with production helper code."""
from pathlib import Path
from harness import extract_section, run_harness
root=Path(__file__).resolve().parents[2]
helper=extract_section(root/'triton-virglrenderer/src/vrend/vrend_winsys_egl.c',
 'int virgl_egl_export_texture_query(', '\nint virgl_egl_get_fd_for_texture2(')
run_harness(Path(__file__).with_suffix('.c'),helper)
