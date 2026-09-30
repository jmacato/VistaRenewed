#!/usr/bin/env python3
"""Exercise GTK EGL viewport geometry used by the Vista framebuffer path."""
from pathlib import Path
from harness import extract_section, run_harness

root = Path(__file__).resolve().parents[2]
helper = extract_section(
    root / "triton-qemu/ui/gtk-egl.c",
    "static void gd_egl_set_surface_viewport(",
    "void gd_egl_update(DisplayChangeListener *dcl,",
)
helper += extract_section(
    root / "triton-qemu/ui/gtk-egl.c",
    "void gd_egl_scanout_flush(DisplayChangeListener *dcl,",
    "void gtk_egl_init(DisplayGLMode mode)",
)
run_harness(Path(__file__).with_suffix(".c"), helper,
            flags=("-Wno-unused-parameter",))
