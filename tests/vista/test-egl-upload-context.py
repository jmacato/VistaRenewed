#!/usr/bin/env python3
"""Check the production GTK EGL upload and renderer-context bindings."""
from pathlib import Path
import argparse
import subprocess
import tempfile
from harness import run_harness

root = Path(__file__).resolve().parents[2]
fixture = Path(__file__).with_suffix(".c")
parser = argparse.ArgumentParser()
parser.add_argument("--baseline-revision",
                    help="optional Git revision expected to acquire the window surface")
arguments = parser.parse_args()


def extract_text(text: str, start: str, end: str | None = None) -> str:
    first = text.index(start)
    return text[first:] if end is None else text[first:text.index(end, first + len(start))]


def helpers(gtk_source: str) -> str:
    context_source = (root / "triton-qemu/ui/egl-context.c").read_text()
    context = extract_text(context_source, "int qemu_egl_make_context_current(")
    upload = extract_text(gtk_source, "void gd_egl_update(DisplayChangeListener *dcl,",
                          "void gd_egl_refresh(DisplayChangeListener *dcl)")
    callback = extract_text(gtk_source, "int gd_egl_make_current(")
    return context + "\n" + upload + "\n" + callback


current_gtk = (root / "triton-qemu/ui/gtk-egl.c").read_text()
run_harness(fixture, helpers(current_gtk))

if arguments.baseline_revision:
    # A historical implementation must compile with this realistic fixture,
    # then abort because it tries to acquire the deliberately forbidden window
    # EGL surface. Keep the comparison source entirely in a temporary directory.
    baseline_gtk = subprocess.run(
        ["git", "show", f"{arguments.baseline_revision}:triton-qemu/ui/gtk-egl.c"],
        cwd=root, check=True, text=True, capture_output=True,
    ).stdout
    template = fixture.read_text()
    source = template.replace("/* SOURCE_UNDER_TEST */", helpers(baseline_gtk))
    with tempfile.TemporaryDirectory(prefix="test-egl-upload-baseline-") as temporary:
        temporary_path = Path(temporary)
        source_path = temporary_path / fixture.name
        binary_path = temporary_path / "test"
        source_path.write_text(source)
        subprocess.run([
            "clang", "-std=c11", "-Wall", "-Wextra", "-Werror",
            str(source_path), "-o", str(binary_path),
        ], check=True)
        baseline = subprocess.run([str(binary_path)], text=True,
                                  capture_output=True, check=False)
        if baseline.returncode == 0 or "draw == EGL_NO_SURFACE" not in baseline.stderr:
            raise AssertionError("baseline did not fail by acquiring the window surface")
