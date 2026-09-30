#!/usr/bin/env python3
"""Run the extracted UMD color-copy helper on native DXVK (not Vista acceptance).

Build DXVK separately first. --dxvk-libdir accepts either a native Meson build
from test_config import ARTIFACTS, HOST_PREFIX, QEMU_BUILD, native_headers, native_libraries, sdk_header, container_command, container_path, vista_compile_database

root, an installed library directory, or a colon-separated library search path.
No VM is launched and no production binaries are installed by this runner.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "triton-umd/src/virtio/neptune/triton"
DEFAULT_OUT = ARTIFACTS / 'integration'


def function(text, name):
    match = re.search(r"(?:static\s+)?(?:HRESULT|DXGI_FORMAT)\s*\n" +
                      re.escape(name) + r"\([^;{}]*\)\s*\{", text)
    if not match:
        raise ValueError("Missing production function: " + name)
    depth, end = 1, text.index("{", match.start()) + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[match.start():end]


def generate(out):
    source_names = {"tritonDxgi.c": ["tritonRawColorFormat", "tritonResourceCopyConverted"],
                    "tritonView.c": ["tritonResourceHostViewFormat"]}
    extracted, manifest = {}, {"sources": {}, "functions": {}}
    for filename, names in source_names.items():
        path = SOURCE / filename
        source = path.read_bytes()
        manifest["sources"][str(path.relative_to(ROOT))] = hashlib.sha256(source).hexdigest()
        for name in names:
            extracted[name] = function(source.decode(), name)
            manifest["functions"][name] = hashlib.sha256(extracted[name].encode()).hexdigest()
    production = "\n\n".join(extracted[name] for name in
                              ["tritonResourceHostViewFormat", "tritonRawColorFormat",
                               "tritonResourceCopyConverted"])
    macros = ["#define " + name + "(obj, ...) (obj)->" + name.split("_", 1)[1] +
              "(__VA_ARGS__)" for name in sorted(set(re.findall(
                  r"ID3D11\w+_\w+(?=\()", production)))]
    template = (ROOT / "tests/vista/test-color-copy-runtime.cpp").read_text()
    text = template.replace("// GENERATED_PRODUCTION", "\n".join(macros) + "\n" + production)
    text = text.replace("GENERATED_HELPER_SHA256", manifest["functions"]["tritonResourceCopyConverted"])
    path = out / "color-copy-runtime.cpp"
    path.write_text(text)
    manifest["generated_sha256"] = hashlib.sha256(text.encode()).hexdigest()
    manifest["fixture_sha256"] = hashlib.sha256(template.encode()).hexdigest()
    (out / "source-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return path, manifest


def library_paths(spec):
    result = []
    for item in spec.split(os.pathsep):
        path = Path(item).resolve()
        if (path / "src/d3d11").is_dir() and (path / "src/dxgi").is_dir():
            result.extend([path / "src/d3d11", path / "src/dxgi"])
        else:
            result.append(path)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=DEFAULT_OUT)
    parser.add_argument("--dxvk-libdir", default=os.environ.get(
        "TRITON_TEST_DXVK_LIBDIR", str(HOST_PREFIX / "lib/x86_64-linux-gnu")))
    parser.add_argument("--device-filter", help="DXVK device-name filter, e.g. NVIDIA")
    parser.add_argument("--generate-only", action="store_true")
    parser.add_argument("--build-only", action="store_true")
    parser.add_argument("--timeout", type=float, default=180)
    args = parser.parse_args()
    out = args.build_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    source, manifest = generate(out)
    print("Production helper SHA256:", manifest["functions"]["tritonResourceCopyConverted"], flush=True)
    if args.generate_only:
        return
    paths = library_paths(args.dxvk_libdir)
    executable = out / "color-copy-runtime"
    command = [os.environ.get("CXX", "g++"), "-std=c++17", "-O2", "-g",
               *native_headers(), "-I" + str(ROOT),
               str(source)]
    command += ["-L" + str(path) for path in paths]
    command += ["-ldxvk_d3d11", "-ldxvk_dxgi", "-o", str(executable)]
    (out / "build-command.json").write_text(json.dumps(command, indent=2) + "\n")
    with (out / "build.log").open("w") as log:
        result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
    if result.returncode:
        print((out / "build.log").read_text()[-16000:])
        raise SystemExit(result.returncode)
    if args.build_only:
        return
    env = dict(os.environ, LD_LIBRARY_PATH=os.pathsep.join(map(str, paths)),
               DXVK_WSI_DRIVER="Headless", DXVK_LOG_PATH=str(out),
               DXVK_SHADER_CACHE_PATH=str(out / "cache"))
    if args.device_filter:
        env["DXVK_FILTER_DEVICE_NAME"] = args.device_filter
    metadata = {"command": [str(executable)], "library_paths": list(map(str, paths)),
                "device_filter": env.get("DXVK_FILTER_DEVICE_NAME"),
                "source_manifest": manifest, "status": "RUNNING"}
    start = time.monotonic()
    (out / "run.json").write_text(json.dumps(metadata, indent=2) + "\n")
    try:
        with (out / "runtime.log").open("w") as log:
            result = subprocess.run([str(executable)], env=env, stdout=log,
                                    stderr=subprocess.STDOUT, timeout=args.timeout)
        output = (out / "runtime.log").read_text()
        summary = re.search(r"COLOR-COPY-SUMMARY checks=(\d+) failures=(\d+) cases=(\d+)", output)
        valid = bool(summary and int(summary[1]) > 0 and not int(summary[2]) and int(summary[3]) >= 90)
        metadata["returncode"] = result.returncode
        metadata["status"] = "PASS" if result.returncode == 0 and valid else "FAIL"
        metadata["summary"] = summary.group(0) if summary else None
    except (subprocess.TimeoutExpired, KeyboardInterrupt) as error:
        metadata["status"] = "TIMEOUT" if isinstance(error, subprocess.TimeoutExpired) else "INTERRUPTED"
        raise
    finally:
        metadata["elapsed_seconds"] = time.monotonic() - start
        (out / "run.json").write_text(json.dumps(metadata, indent=2) + "\n")
    print(output[-20000:])
    if result.returncode or not valid:
        raise SystemExit(result.returncode or 1)


if __name__ == "__main__":
    main()
