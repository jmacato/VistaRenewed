#!/usr/bin/env python3
"""Fault-inject production shader cache/lifetime logic with real metadata types."""
import os
import hashlib
import json
import re
import subprocess
from pathlib import Path

ROOT = next(p for p in Path(__file__).resolve().parents if (p / 'triton-umd').is_dir())
OUT = Path(os.environ.get('TRITON_TEST_ARTIFACTS', ROOT / 'test-artifacts')) / 'dx9-shaders/cache'
OUT.mkdir(parents=True, exist_ok=True)
D9 = ROOT / 'triton-umd/src/virtio/neptune/vista-d3d9'
source = (D9 / 'triton9_shader.cpp').read_text()


def function(name):
    match = re.search(r'\n' + name + r'\([^;{}]*\)\s*\{', source)
    assert match, name
    start = source.rfind('\n\n', 0, match.start()) + 2
    end = source.index('{', match.start()) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


classes = source[source.index('struct Triton9Shader;'):source.index('struct Triton9InputElement {')]
groups = {
    'CACHE': ['triton9DropConvertedShader', 'triton9VariantMatches',
              'triton9RestoreShaderVariant', 'triton9StoreShaderVariant'],
    'CONVERT': ['triton9ConvertShader', 'triton9ConvertTransformedVertexShader'],
    'FIXED': ['triton9ConstantBufferSize', 'triton9EnsureConstantData',
              'triton9FixedTokenKey', 'triton9UploadFixedProgram', 'triton9FixedShaders',
              'triton9RememberFixedState', 'triton9EnsureFixedVertexShader',
              'triton9EnsureFixedPixelShader', 'triton9ReleaseConstantBuffers',
              'triton9ReleaseFixedFunctionShaders'],
}
code = Path(__file__).with_suffix('.cpp').read_text().replace('/* CLASSES */', classes)
for marker, names in groups.items():
    code = code.replace('/* ' + marker + ' */', '\n'.join(function(name) for name in names))
(OUT / 'current.cpp').write_text(code)
(OUT / 'checked-source-sha256.json').write_text(json.dumps({name: hashlib.sha256((D9 / name).read_bytes()).hexdigest()
     for name in ['triton9_shader.cpp', 'triton9_fixed.cpp', 'triton9_fixed.h',
                  'third_party/d3d9on12-shaderconverter/Inc/ShaderConv.h']}, indent=2) + '\n')
includes = [D9 / 'tests/native_compat', ROOT / 'triton-dxvk/include/native/directx', ROOT / 'triton-dxvk/include/native/windows', D9,
            D9 / 'third_party/d3d9on12-shaderconverter/Inc']
for mutant in [None, 'ordinary', 'transformed']:
    variant = code
    if mutant == 'ordinary':
        old = '''    try {
        if (declaration)
            convertedInputs = declaration->inputDecls;
    } catch (...) {
        return E_OUTOFMEMORY;
    }'''
        assert variant.count(old) == 1
        variant = variant.replace(old, '    if (declaration) convertedInputs = declaration->inputDecls;')
    elif mutant == 'transformed':
        old = '''    try {
        convertedInputs = declaration->inputDecls;
        hr = shader->converter.ConvertTLShader(args);'''
        assert variant.count(old) == 1
        variant = variant.replace(old, '''    convertedInputs = declaration->inputDecls;
    try {
        hr = shader->converter.ConvertTLShader(args);''')
    name = mutant or 'current'
    path = OUT / (name + '.cpp')
    path.write_text(variant)
    subprocess.run(['clang++', '-std=c++17', '-O1', '-g', '-fsanitize=address,undefined',
                    '-include', str(D9 / 'tests/native_compat/shaderconv_native.h'),
                    *['-I' + str(path) for path in includes], str(path),
                    str(D9 / 'triton9_fixed.cpp'), '-o', str(OUT / name)], check=True)
    result = subprocess.run([str(OUT / name)], text=True, capture_output=True)
    log = result.stdout + result.stderr
    (OUT / (name + '.log')).write_text(log)
    if mutant:
        assert result.returncode and 'conversion allocation exception escaped' in log, log
        print('Shader allocation exception control rejected:', mutant)
    else:
        print(log, end='')
        result.check_returncode()
