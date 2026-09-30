#!/usr/bin/env python3
"""Compile the real EGL helper with CPU-only teardown failure injection."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    owner = tempfile.TemporaryDirectory(prefix="egl-copy-lifecycle-")
    out = args.output.resolve() if args.output else Path(owner.name)
    out.mkdir(parents=True, exist_ok=True)
    qemu = root / "triton-qemu"
    source = qemu / "ui/egl-external-copy.c"
    fixture = Path(__file__).with_suffix(".c")
    text = source.read_text()
    variants = {
        "production": text,
        "dangling-handle": text.replace("    *owner = NULL;\n", ""),
        "lost-early-handle": text.replace("    copy = *owner;\n", "    copy = *owner;\n    *owner = NULL;\n"),
    }
    assert len(set(variants.values())) == len(variants)
    cflags = shlex.split(subprocess.check_output(
        ["pkg-config", "--cflags", "glib-2.0", "epoxy"], text=True))
    libs = shlex.split(subprocess.check_output(
        ["pkg-config", "--libs", "glib-2.0", "epoxy"], text=True))
    results = {}
    (out / "qemu").mkdir(exist_ok=True)
    (out / "qemu/osdep.h").write_text("#pragma once\n#include <stdbool.h>\n#include <stdint.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n#include <glib.h>\n")
    for name, body in variants.items():
        path = out / name
        (path / "ui").mkdir(parents=True, exist_ok=True)
        (path / "ui/egl-external-copy.c").write_text(body)
        binary = path / "lifecycle"
        command = [os.environ.get("CC", "cc"), "-std=gnu11", "-O1", "-g",
                   "-Wall", "-Wextra", "-Werror", "-I" + str(path),
                   "-I" + str(out), "-I" + str(qemu / "include"),
                   *cflags, str(fixture), "-o", str(binary), *libs]
        build = subprocess.run(command, capture_output=True, text=True, timeout=60)
        (path / "build.log").write_text(build.stdout + build.stderr)
        if build.returncode:
            raise RuntimeError((path / "build.log").read_text())
        run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
        (path / "run.log").write_text(run.stdout + run.stderr)
        results[name] = {"exit_code": run.returncode, "stdout": run.stdout,
                         "stderr": run.stderr, "command": command}
    passed = results["production"]["exit_code"] == 0 and all(
        result["exit_code"] != 0 for name, result in results.items() if name != "production")
    files = [source, fixture, Path(__file__), qemu / "include/ui/egl-external-copy.h",
             out / "qemu/osdep.h"]
    evidence = {"passed": passed, "gpu_execution": False, "results": results,
                "sha256": {str(p): hashlib.sha256(p.read_bytes()).hexdigest()
                           for p in files}}
    (out / "results.json").write_text(json.dumps(evidence, indent=2) + "\n")
    print(results["production"]["stdout"], end="")
    print("Ownership mutants rejected:", passed)
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
