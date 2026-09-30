#!/usr/bin/env python3
"""Compile production D3D9 resource logic against native DXVK and run pixel oracles.
Only Vista allocation/transport entrypoints are stubbed; texture layout, creation,
views, uploads, copies, resolves, readback and rename are production functions.
"""
from pathlib import Path
import argparse, os, re, subprocess
from test_config import ARTIFACTS, HOST_PREFIX, QEMU_BUILD, native_headers, native_libraries, sdk_header, container_command, container_path, vista_compile_database

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / 'triton-umd/src/virtio/neptune/vista-d3d9'
OUT = ARTIFACTS / 'dx9-resources'

def function(text, name):
    match = re.search(r'(?:static (?:inline )?)?(?:HRESULT(?: APIENTRY)?|void|UINT|BOOL|SIZE_T|enum triton9_clear_contract_result|DXGI_FORMAT)\s*\n'+name+r'\([^;{}]*\)\s*\{', text)
    if not match: raise ValueError('missing function '+name)
    start = match.start(); pos = text.index('{',match.start()); depth=1; end=pos+1
    while depth:
        if text[end]=='{': depth+=1
        elif text[end]=='}': depth-=1
        end+=1
    return text[start:end]

def structure(text,name):
    m=re.search(r'typedef struct (?:_?'+name+r')?\s*\{',text)
    if not m: raise ValueError('missing struct '+name)
    end=text.index('} '+name+';',m.end())+len('} '+name+';')
    return text[m.start():end]

def generate(mutation=None):
    hdr=(SRC/'triton9.h').read_text(); resource=(SRC/'triton9_resource.c').read_text(); output=(SRC/'triton9_output.c').read_text()
    ddi=(sdk_header('d3dumddi.h', 'microsoft.windows.wdk.x64')).read_text()
    shared=(sdk_header('d3dukmdt.h', 'microsoft.windows.sdk.cpp')).read_text()
    formats=(SRC/'triton9_format.c').read_text().replace('#include "triton9.h"','').replace('_Static_assert','static_assert')
    types='\n'.join(structure(ddi,n) for n in ['D3DDDIBOX','D3DDDIRANGE','D3DDDIARG_CLEAR','D3DDDI_LOCKFLAGS','D3DDDIARG_LOCK','D3DDDI_UNLOCKFLAGS','D3DDDIARG_UNLOCK','D3DDDI_LOCKASYNCFLAGS','D3DDDIARG_LOCKASYNC','D3DDDI_UNLOCKASYNCFLAGS','D3DDDIARG_UNLOCKASYNC','D3DDDI_BLTFLAGS','D3DDDIARG_BLT','D3DDDIARG_BUFFERBLT','D3DDDIARG_SETRENDERTARGET','D3DDDIARG_SETDEPTHSTENCIL','D3DDDI_COLORFILLFLAGS','D3DDDIARG_COLORFILL'])
    types+='\n'+shared[shared.index('typedef struct _D3DDDI_RESOURCEFLAGS'):shared.index('typedef struct _D3DDDI_RESOURCEFLAGS2')].split('#if')[0]
    types+='\n'+structure(hdr,'TRITON9_RESOURCE')+'\n'+structure(hdr,'TRITON9_FORMAT')
    helpers='\n'.join(function(hdr,n) for n in ['triton9ResourcesShareBacking','triton9ResourceByteOffset','triton9ResourceBoxValid','triton9ResourceIsSystemMemory','triton9ResourceBelongsToDevice'])
    # Pointer-returning inline helpers have a different signature.
    a=hdr.index('static inline TRITON9_RESOURCE *');b=hdr.index('static inline BOOL\ntriton9ResourcesShareBacking',a)
    helpers=hdr[a:b]+helpers
    names=['triton9SwapPacked10Rows','triton9UpdateHostTexture','triton9FreeUnpublishedResource','triton9DisposeFailedResource','triton9SrgbViewFormat','triton9FullMipCount','triton9CreateShadow','triton9ClearLockRegion','triton9CreateHostBuffer','triton9CreateHostTexture','triton9EnsureResourceHost','triton9ReleaseResourceViews','triton9UploadShadowRegion','triton9UploadShadow','triton9UploadLockedShadow','triton9RefreshSystemMemoryBuffer','triton9PrepareResourceForHostRead','triton9PrepareBufferRangeForHostRead','triton9CommitSystemMemoryWrite','triton9GetRenderTargetViewEx','triton9GetRenderTargetView','triton9GetDepthStencilView','triton9GetShaderResourceViewEx','triton9GetShaderResourceView','triton9EnsureStagingResource','triton9MapStagingForRead','triton9CopyStagingBoxToShadow','triton9CopyStagingSurfaceToShadow','triton9CopyStagingBufferToShadow','triton9ResolveResource','triton9ReadbackShadow','triton9ReadbackShadowAsync','triton9CreateTextureResource','triton9SharedFormat','triton9CreateSingleResource','triton9RenameTexture','triton9Lock','triton9Unlock','triton9LockAsync','triton9UnlockAsync']
    funcs=[function(resource,n) for n in names]+[function(output,n) for n in ['triton9ValidateColorRects','triton9ColorFill','triton9HideResourceViews','triton9SetRenderTarget','triton9SetDepthStencil','triton9HasCompleteD24S8ClearContract','triton9BindOutputs','triton9BltColorFormat','triton9BltFlagsSupported','triton9BltImpl','triton9CreateCopyScratch','triton9CopyBox','triton9BufBlt','triton9CopyTextureChain','triton9ClearRectIsValid','triton9IntersectClearRect','triton9NormalizeClearRects','triton9ConvertClearColor','triton9ClearRectsCoverResource','triton9ClearRenderTargetRects','triton9ClearD16Rects','triton9BuildClearLimits','triton9TraceClearEntry','triton9TraceClearReturn','triton9Clear','triton9GenerateMipSubLevels']]
    state=(SRC/'triton9_state.cpp').read_text()
    funcs += [function(state,n) for n in ['triton9SamplerIndex','triton9ColorToFloat','triton9SetTexture','triton9PreparePipelineState']]
    if mutation == 'packed':
        funcs=[f[:f.index('{')]+'{ triton9CpuCopyRows(destination, destinationPitch, source, sourcePitch, rowBytes, rows); }' if '\ntriton9SwapPacked10Rows(' in f else f for f in funcs]
    if mutation == 'binding':
        funcs=[f.replace('if (triton9ResourcesShareBacking(device->textures[stage], resource)) {','if (triton9ResourcesShareBacking(device->textures[stage], resource)) {\n            device->textures[stage] = NULL;') for f in funcs]
    if mutation == 'subresource':
        funcs=[x.replace('resource->subresourceIndex, box, source, rowPitch, slicePitch','0, box, source, rowPitch, slicePitch') for x in funcs]
    if mutation == 'nonblocking':
        funcs=[f[:f.index('{')]+'{ return D3DDDIERR_WASSTILLDRAWING; }' if '\ntriton9ReadbackShadowAsync(' in f else f for f in funcs]
    if mutation == 'pitch':
        funcs=[x.replace('((uint64_t)resource->width + bw - 1) / bw * bytes','(uint64_t)resource->width * bytes') for x in funcs]
    if mutation == 'async-system-memory':
        old = '    args->hCookie = NULL;'
        bad = old + '\n    if (triton9ResourceIsSystemMemory(resource) && (args->Flags.Discard || args->Flags.NoOverwrite)) return D3DDDIERR_INVALIDCALL;'
        assert sum(old in f for f in funcs) == 1
        funcs=[f.replace(old, bad) for f in funcs]
    if mutation == 'async-discard-order':
        old = '    if (args->Flags.Discard && triton9ResourceIsSystemMemory(resource))\n        return E_NOTIMPL;\n    if (args->Flags.Discard) {'
        bad = '    if (args->Flags.Discard && !triton9ResourceIsSystemMemory(resource)) {'
        assert sum(old in f for f in funcs) == 1
        funcs=[f.replace(old, bad) for f in funcs]
    if mutation == 'system-memory-snapshot':
        funcs=[f.replace('    if (valid) {','    if (snapshot) {') if '\ntriton9RefreshSystemMemoryBuffer(' in f else f for f in funcs]
    if mutation == 'system-memory-alias':
        funcs=[f.replace('    if (valid) {','    if (valid) return triton9CheckHostDevice(device);\n    if (valid) {') if '\ntriton9RefreshSystemMemoryBuffer(' in f else f for f in funcs]
    if mutation == 'draw-range-full':
        funcs=[f.replace('if (valid) {', 'first = 0; end = resource->width;\n    if (valid) {') if '\ntriton9RefreshSystemMemoryBuffer(' in f else f for f in funcs]
    if mutation == 'draw-range-baseline':
        funcs=[f.replace('if (!valid) {', 'if (FALSE) {') if '\ntriton9RefreshSystemMemoryBuffer(' in f else f for f in funcs]
    if mutation == 'shadow-range-cache':
        funcs=[f.replace('BOOL track = resource->isBuffer', 'BOOL track = FALSE && resource->isBuffer') if '\ntriton9UploadShadowRegion(' in f else f for f in funcs]
    if mutation == 'shadow-range-stale':
        funcs=[f.replace('resource->systemMemorySnapshotSerial == resource->contentSerial', 'TRUE') if '\ntriton9UploadShadowRegion(' in f else f for f in funcs]
    if mutation == 'shadow-range-failure':
        funcs=[f.replace('if (track && SUCCEEDED(hr))', 'if (track)') if '\ntriton9UploadShadowRegion(' in f else f for f in funcs]
    if mutation == 'shadow-range-bounds':
        funcs=[f.replace('box->right > resource->width', 'FALSE') if '\ntriton9UploadShadowRegion(' in f else f for f in funcs]
    prototypes='\n'.join(f[:f.index('{')].rstrip()+';' for f in funcs)
    constants='\n'.join(re.findall(r'^\s*#define FORMATOP_[^\n]+',ddi,re.M))
    tokens=set(re.findall(r'D3DDDIFMT_(\w+)',resource+formats+(SRC/'triton9_blit.c').read_text())) | set(re.findall(r'(?:FMT|SRGB|BC|DEPTH)\((\w+),',formats))
    tokens.discard('d')
    constants+='\n'+'\n'.join('#define D3DDDIFMT_'+n+' D3DFMT_'+n for n in sorted(tokens))
    blit=(SRC/'triton9_blit.c').read_text()
    blit='\n'.join(line for line in blit.splitlines() if not line.startswith('#include'))
    blit=blit.replace('= {0};','= {};').replace('= device->stretchBlitState;','= (TRITON9_STRETCH_STATE *)device->stretchBlitState;')
    production=prototypes+'\n'+formats+'\n'+blit+'\n'+'\n'.join(funcs)
    macros=[]
    for name in sorted(set(re.findall(r'ID3D11\w+_\w+(?=\()',production))):
        macros.append('#define '+name+'(obj, ...) (obj)->'+name.split('_',1)[1]+'(__VA_ARGS__)')
    macros += ['#undef ID3D11DeviceContext_UpdateSubresource', '#define ID3D11DeviceContext_UpdateSubresource(...) recordUpdate(__VA_ARGS__)']
    macros += ['#undef ID3D11DeviceContext1_CopySubresourceRegion', '#define ID3D11DeviceContext1_CopySubresourceRegion(...) recordCopy(__VA_ARGS__)']
    # C permits void* heap conversions; use a converting allocation wrapper in C++.
    text=(ROOT/'tests/vista/test-d3d9-resources.cpp').read_text()
    return text.replace('// GENERATED_CONSTANTS',constants).replace('// GENERATED_TYPES',types).replace('// GENERATED_HELPERS',helpers).replace('// GENERATED_PRODUCTION','\n'.join(macros)+'\n'+production)

def host():
    OUT.mkdir(parents=True,exist_ok=True)
    prefix=HOST_PREFIX; env=dict(os.environ,LD_LIBRARY_PATH=os.pathsep.join(map(str, native_libraries())),DXVK_WSI_DRIVER='Headless')
    for mutation in [None,'subresource','pitch','nonblocking','binding','packed','async-system-memory','async-discard-order','system-memory-snapshot','system-memory-alias','shadow-range-cache','shadow-range-stale','shadow-range-failure','shadow-range-bounds','draw-range-full','draw-range-baseline']:
        path=OUT/('native-resources'+('-'+mutation if mutation else '')+'.cpp');exe=path.with_suffix('')
        path.write_text(generate(mutation))
        build=subprocess.run(['g++','-std=c++17','-O1','-g','-fpermissive','-Wno-write-strings',*native_headers(),'-I'+str(ROOT),str(path),str(SRC/'triton9_cpu_layout.c'),*['-L'+str(p) for p in native_libraries()],'-ldxvk_d3d11','-ldxvk_dxgi','-o',str(exe)],capture_output=True,text=True)
        (OUT/(path.stem+'-build.log')).write_text(build.stdout+build.stderr)
        if build.returncode: print(build.stderr[-20000:]);raise SystemExit(build.returncode)
        result=subprocess.run([str(exe)],env=env,capture_output=True,text=True)
        (OUT/(path.stem+'.log')).write_text(result.stdout+result.stderr)
        print('\n'.join(line for line in result.stdout.splitlines() if 'SUMMARY' in line or 'TYPED-SRGB' in line))
        if mutation:
            if not result.returncode or '[FAIL]' not in result.stdout: raise SystemExit('negative control did not fail behavior: '+mutation)
            print('D3D9 negative control rejected:',mutation)
        elif result.returncode: print(result.stderr[-4000:]);raise SystemExit(result.returncode)
    subprocess.run([os.environ.get('PYTHON','python3'),str(ROOT/'tests/vista/test-d3d9-resources.py')],check=True)
    print('D3D9 resource checks passed')

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--host',action='store_true',required=True);parser.parse_args();host()
