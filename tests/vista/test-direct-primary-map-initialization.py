#!/usr/bin/env python3
"""Execute actual allocator/map bodies and reject CPU-clear regressions."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def function(source, signature):
    start = source.index(signature)
    cursor = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[cursor] == '{') - (source[cursor] == '}')
        cursor += 1
    return source[start:cursor]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--evidence', type=Path, required=True)
    args = parser.parse_args()
    out = args.evidence.resolve()
    out.mkdir(parents=True, exist_ok=False)
    root = Path(__file__).resolve().parents[2]
    source = root / 'triton-dxvk/src/dxvk/dxvk_memory.cpp'
    header = source.with_suffix('.h')
    fixture = Path(__file__).with_suffix('.cpp')
    bodies = '\n'.join(function(source.read_text(), signature) for signature in (
        '  Rc<DxvkResourceAllocation> DxvkMemoryAllocator::allocateDedicatedMemory(',
        '  void DxvkMemoryAllocator::mapDeviceMemory(',
    ))
    marker = '/* PRODUCTION_FUNCTIONS */'
    template = fixture.read_text()
    assert template.count(marker) == 1
    assert 'bool                  allowCpuInitialization = true' in header.read_text()
    variants = {'production': bodies}
    mutations = {
        'clear-shared-memory': ('allowCpuInitialization && m_device->config().zeroMappedMemory',
                                'm_device->config().zeroMappedMemory'),
        'allow-shared-initialization': (
            'allocationInfo.handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_FLAG_BITS_MAX_ENUM', 'true'),
        'disable-ordinary-debug-initialization': (
            'allowCpuInitialization && m_device->config().zeroMappedMemory', 'false'),
    }
    for name, (old, new) in mutations.items():
        assert bodies.count(old) == 1, name
        variants[name] = bodies.replace(old, new)
    report = {
        'cpu_only': True,
        'source_sha256': {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
                          for p in (source, header, fixture, Path(__file__).resolve())},
        'variants': {},
    }
    for name, body in variants.items():
        cpp = out / (name + '.cpp')
        exe = out / name
        cpp.write_text(template.replace(marker, body))
        command = ['c++', '-std=c++17', '-O2', '-Wall', '-Wextra', str(cpp), '-o', str(exe)]
        build = subprocess.run(command, capture_output=True, text=True, timeout=30)
        (out / (name + '-build.log')).write_text(build.stdout + build.stderr)
        if build.returncode:
            raise RuntimeError(f'{name} build failed; see retained build log')
        run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=5)
        (out / (name + '-run.log')).write_text(run.stdout + run.stderr)
        report['variants'][name] = {'build_command': command, 'exit_code': run.returncode,
                                    'stdout': run.stdout, 'stderr': run.stderr,
                                    'generated_sha256': hashlib.sha256(cpp.read_bytes()).hexdigest()}
    (out / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    assert report['variants']['production']['exit_code'] == 0
    for name in mutations:
        assert report['variants'][name]['exit_code'] == 1, name
    print(report['variants']['production']['stdout'].strip())
    print('All three CPU-initialization mutants rejected.')


if __name__ == '__main__':
    main()
