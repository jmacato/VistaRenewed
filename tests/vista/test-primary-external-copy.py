#!/usr/bin/env python3
"""Build the exact QEMU helper and its bounded native GPU-copy proof.

Default: CPU compilation only. --run must be authorized by the owner of the
single GPU test slot. A pass does not establish installed QEMU integration.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shlex
import subprocess
import time


from test_config import QEMU_BUILD, required_file

def main():
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--run", action="store_true")
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument('--device', help='Required with --run: Vulkan device-name filter')
    args = parser.parse_args()
    if args.run and not args.device:
        parser.error('--run requires --device')
    required_file(QEMU_BUILD / "config-host.h", "Configure/build public QEMU first, or set VISTA_QEMU_BUILD_DIR.")
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    reservation_source = root / "tests/vista/evidence.py"
    spec = importlib.util.spec_from_file_location("storage_runner", reservation_source)
    storage_runner = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(storage_runner)
    with storage_runner.reserve_output(out, args.run, parser):
        sources = [Path(__file__).resolve(), root / "tests/vista/test-primary-external-copy.cpp",
                   root / "triton-qemu/ui/egl-external-copy.c", root / "triton-qemu/include/ui/egl-external-copy.h",
                   root / "tests/vista/external_copy_fixture.h", reservation_source,
                   QEMU_BUILD / "config-host.h"]
        cflags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "glib-2.0", "epoxy"], text=True))
        libraries = shlex.split(subprocess.check_output(["pkg-config", "--libs", "glib-2.0", "epoxy"], text=True))
        binary = out / "primary-external-copy"
        helper = out / "egl-external-copy.o"
        includes = ["-I" + str(root / "triton-qemu"), "-I" + str(root / "triton-qemu/include"),
                    "-I" + str(QEMU_BUILD)]
        commands = [[os.environ.get("CC", "cc"), "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra",
                     *includes, *cflags, "-c", str(sources[2]), "-o", str(helper)],
                    [os.environ.get("CXX", "c++"), "-std=c++17", "-O1", "-g", "-Wall", "-Wextra",
                     "-Wno-missing-field-initializers", *includes,
                     "-I" + str(root / "triton-dxvk/include/vulkan/include"), *cflags,
                     str(sources[1]), str(helper), "-o", str(binary), "-l:libvulkan.so.1", *libraries]]
        identity = {"source_sha256": {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources},
                    "commands": commands}
        if args.no_build:
            saved = json.loads((out / "build-identity.json").read_text())
            for key in identity:
                if identity[key] != saved.get(key):
                    parser.error("--no-build identity changed: " + key)
            if hashlib.sha256(binary.read_bytes()).hexdigest() != saved["binary_sha256"]:
                parser.error("--no-build binary differs from recorded build")
            identity = saved
        else:
            with (out / "build.log").open("w") as log:
                for command in commands:
                    build = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=60)
                    if build.returncode:
                        identity["build_exit_code"] = build.returncode
                        (out / "build-identity.json").write_text(json.dumps(identity, indent=2) + "\n")
                        print((out / "build.log").read_text())
                        return build.returncode
            identity.update(build_exit_code=0, binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest())
            (out / "build-identity.json").write_text(json.dumps(identity, indent=2) + "\n")
        if not args.run:
            print("Built helper and native proof only; no GPU/KVM/VM execution.")
            print(shlex.join(["python3", str(Path(__file__).resolve()), "--run", "--device", "DEVICE_NAME", "--no-build", "--output", str(out)]))
            return 0
        started = time.time()
        with (out / "copy.stdout.jsonl").open("x") as stdout, (out / "copy.stderr.log").open("x") as stderr:
            try:
                run = subprocess.run([str(binary)], env=dict(os.environ, TRITON_TEST_DEVICE=args.device), stdout=stdout, stderr=stderr, timeout=30)
                result = {"exit_code": run.returncode, "timeout": False}
            except subprocess.TimeoutExpired:
                result = {"exit_code": None, "timeout": True}
        records, errors = [], []
        for line in (out / "copy.stdout.jsonl").read_text(errors="replace").splitlines():
            try:
                record = json.loads(line)
                if not isinstance(record, dict):
                    raise ValueError("record must be an object")
                records.append(record)
            except ValueError:
                errors.append(line)
        result.update(start_unix=started, elapsed_seconds=time.time() - started, records=records, parse_errors=errors)
        result["gpu_copy_bridge_pass"] = result["exit_code"] == 0 and not errors and any(
            r.get("stage") == "result" and r.get("status") == "gpu_copy_bridge_pass" for r in records)
        (out / "results.json").write_text(json.dumps({"identity": identity, "result": result,
            "current_qemu_integration_proven": False, "application_performance_proven": False}, indent=2) + "\n")
        print(json.dumps({k: v for k, v in result.items() if k not in ("records", "parse_errors")}))
        return 0 if result["gpu_copy_bridge_pass"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
