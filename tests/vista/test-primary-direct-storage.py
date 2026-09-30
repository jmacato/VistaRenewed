#!/usr/bin/env python3
"""Build or explicitly run bounded direct-primary allocation capability probes.

Default is build only. --run starts GPU and KVM work; never use it during a VM
performance capture. A pass demonstrates a storage capability, not Vista driver
compatibility or acceptable QEMU-window performance.
"""
import argparse
from contextlib import contextmanager
import hashlib
import itertools
import json
import os
from pathlib import Path
import shlex
import subprocess
import time


@contextmanager
def reserve_output(out, run, parser):
    """Never reuse a started run, including one killed before its first result."""
    lock_path = out / "runner-lock.json"
    try:
        lock = lock_path.open("x")
    except FileExistsError:
        parser.error("output is reserved by an active/interrupted invocation; use a fresh --output")
    try:
        with lock:
            reservation = {"pid": os.getpid(), "start_unix": time.time(), "gpu_run_requested": run}
            lock.write(json.dumps(reservation) + "\n")
            lock.flush()
            os.fsync(lock.fileno())
            existing = [p for p in (out / "run-start.json", out / "results.json") if p.exists()]
            existing += list(out.glob("*.stdout.jsonl")) + list(out.glob("*.stderr.log"))
            if existing:
                parser.error("run evidence already exists; use a fresh --output directory")
            if run:
                # Exclusive and permanent: a crash, signal, parse failure, or
                # stale --no-build rejection must not authorize overwriting.
                with (out / "run-start.json").open("x") as marker:
                    marker.write(json.dumps(reservation) + "\n")
                    marker.flush()
                    os.fsync(marker.fileno())
            yield
    finally:
        lock_path.unlink()


def main():
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=root / ".unlazy/dx9-dx10-compat/cpu-roundtrip-audit/primary-capability")
    parser.add_argument("--run", action="store_true", help="Explicitly authorize GPU/KVM experiment execution")
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--device", default="NVIDIA")
    parser.add_argument("--cpu-map", choices=["vulkan", "dmabuf"], default="vulkan",
                        help="Exact candidate CPU/KVM pointer source; no fallback")
    parser.add_argument("--case", choices=[f"{f}-{w}-{t}" for f, w, t in itertools.product(
        ["bgra", "rgba"], [800, 1280, 1920], ["linear", "linear-dmabuf", "drm-linear"])])
    parser.add_argument("--timeout", type=float, default=30.0)
    args = parser.parse_args()
    if not 5 <= args.timeout <= 60:
        parser.error("--timeout must be between 5 and 60 seconds per case")
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    with reserve_output(out, args.run, parser):
        return execute(args, parser, root, out)


def execute(args, parser, root, out):
    source = root / "tests/vista/test-primary-direct-storage.cpp"
    binary = out / "primary-direct-storage"
    command = [os.environ.get("CXX", "c++"), "-std=c++17", "-O1", "-g", "-Wall", "-Wextra",
               "-Wno-missing-field-initializers", "-I" + str(root / "triton-dxvk/include/vulkan/include"),
               str(source), "-o", str(binary), "-l:libvulkan.so.1", "-lEGL", "-lGL"]
    identity = {"source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                "runner_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(), "compile_command": command}
    if args.no_build:
        saved = json.loads((out / "build-identity.json").read_text())
        for key in ("source_sha256", "runner_sha256", "compile_command"):
            if saved.get(key) != identity[key]:
                parser.error("--no-build rejected: build identity changed at " + key)
        actual_binary = hashlib.sha256(binary.read_bytes()).hexdigest()
        if saved.get("binary_sha256") != actual_binary:
            parser.error("--no-build rejected: binary hash differs from recorded build")
        identity = saved
    if not args.no_build:
        with (out / "build.log").open("w") as log:
            build = subprocess.run(command, cwd=root, stdout=log, stderr=subprocess.STDOUT, timeout=60)
        identity["build_exit_code"] = build.returncode
        (out / "build-identity.json").write_text(json.dumps(identity, indent=2) + "\n")
        if build.returncode:
            print((out / "build.log").read_text())
            return build.returncode
    identity["binary_sha256"] = hashlib.sha256(binary.read_bytes()).hexdigest()
    if not args.no_build:
        (out / "build-identity.json").write_text(json.dumps(identity, indent=2) + "\n")
    if not args.run:
        print("Built only; no Vulkan or KVM execution.")
        print("Run after the GPU slot is granted: " + shlex.join([
            "python3", str(Path(__file__).resolve()), "--run", "--no-build", "--output", str(out)]))
        return 0
    results = []
    for fmt, (width, height), tiling in itertools.product(
            ["bgra", "rgba"], [(800, 600), (1280, 720), (1920, 1080)],
            ["linear", "linear-dmabuf", "drm-linear"]):
        name = f"{fmt}-{width}-{tiling}"
        if args.case and args.case != name:
            continue
        cmd = [str(binary), "--width", str(width), "--height", str(height),
               "--format", fmt, "--tiling", tiling, "--device", args.device, "--cpu-map", args.cpu_map]
        start = time.time()
        with (out / (name + ".stdout.jsonl")).open("x") as stdout, (out / (name + ".stderr.log")).open("x") as stderr:
            try:
                p = subprocess.run(cmd, cwd=root, stdout=stdout, stderr=stderr, timeout=args.timeout)
                result = {"case": name, "exit_code": p.returncode, "timeout": False}
            except subprocess.TimeoutExpired:
                result = {"case": name, "exit_code": None, "timeout": True}
        result.update({"command": cmd, "start_unix": start, "elapsed_seconds": time.time() - start})
        records, parse_errors = [], []
        for line in (out / (name + ".stdout.jsonl")).read_text().splitlines():
            try:
                records.append(json.loads(line))
            except json.JSONDecodeError:
                parse_errors.append(line)
        result["records"] = records
        result["parse_errors"] = parse_errors
        result["complete_capability_pass"] = result["exit_code"] == 0 and not parse_errors and any(
            r.get("stage") == "result" and r.get("status") == "complete_capability_pass" for r in records)
        results.append(result)
        (out / "results.json").write_text(json.dumps({"identity": identity, "results": results,
            "application_performance_proven": False}, indent=2) + "\n")
        print(f"{name}: exit={result['exit_code']} timeout={result['timeout']} capability_pass={result['complete_capability_pass']}", flush=True)
    # Alternate layouts can be unsupported: each requested format/size still
    # needs at least one fully proven candidate. Individual failures survive.
    coverage = {}
    for result in results:
        fmt, width, _ = result["case"].split("-", 2)
        key = f"{fmt}-{width}"
        coverage[key] = coverage.get(key, False) or result["complete_capability_pass"]
    summary = {"identity": identity, "results": results, "format_size_coverage": coverage,
               "all_requested_format_sizes_proven": bool(coverage) and all(coverage.values()),
               "application_performance_proven": False, "current_qemu_integration_proven": False}
    (out / "results.json").write_text(json.dumps(summary, indent=2) + "\n")
    return 0 if summary["all_requested_format_sizes_proven"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
