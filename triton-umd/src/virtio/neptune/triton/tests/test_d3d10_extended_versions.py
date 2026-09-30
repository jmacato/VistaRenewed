#!/usr/bin/env python3
"""Exercise Vista extended DDI negotiation and bounded DXGI table installation."""
from pathlib import Path
import argparse
import json
import os
import re
import shlex
import subprocess
import tempfile

import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[6] / 'tests/vista'))
from test_config import native_headers, native_libraries, vista_compile_database, container_command

ROOT = Path(__file__).resolve().parents[6]
TRITON = ROOT / 'triton-umd/src/virtio/neptune/triton'


def function(source, name):
    match = re.search(r'(?m)^(?:static )?(?:HRESULT|D3D_FEATURE_LEVEL|void|SIZE_T|BOOL|bool)(?: APIENTRY)?\s+' + name + r'\s*\(', source)
    assert match, name
    start = source.index('{', match.start())
    depth = 1
    end = start + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]


def assemble(ddi, dxgi):
    create = function(ddi, 'tritonCreateDevice')
    validate = create[:create.index('    PTRITON_ADAPTER pAdapter')]
    validate = validate.replace('tritonCreateDevice(', 'validate_create(') + '    return S_OK;\n}'
    install = create[create.index('    HRESULT tableHr ='):create.index('    p->textFilterWidth')]
    install = ('static HRESULT install_requested(D3D10DDIARG_CREATEDEVICE *pArgs) {\n' +
               install + '    return S_OK;\n}\n')
    level = create[create.index('    const D3D11DDI_3DPIPELINELEVEL level ='):create.index('    /* Statically-linked internal factory.')]
    legacy = re.search(r'    const D3D_FEATURE_LEVEL legacyLevel = [^;]+;', create).group(0)
    level = ('static D3D_FEATURE_LEVEL select_level(D3D10DDIARG_CREATEDEVICE *pArgs) {\n'
             + legacy + '\n' + level + '\n return requested;\n}\n')
    return '\n'.join((function(ddi, 'tritonD3D10FeatureLevel'),
        function(ddi, 'tritonInstallDeviceFuncs'), validate, install, level,
        function(ddi, 'tritonGetSupportedVersions'), function(ddi, 'OpenAdapter10'),
        function(dxgi, 'triton_fill_base'), function(dxgi, 'triton_fill_1_1'),
        function(dxgi, 'tritonInstallDXGIFuncs')))


FIXTURE = r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define NPT_D3D10_RUNTIME_DDI
#define APIENTRY
#define TRUE 1
#define FALSE 0
#define S_OK 0
#define E_INVALIDARG -1
#define E_NOINTERFACE -2
#define E_OUTOFMEMORY -3
#define E_FAIL -4
#define FAILED(hr) ((hr)<0)
#define HEAP_ZERO_MEMORY 8
#define TR_LOG(...) ((void)0)
typedef int HRESULT;typedef uint32_t UINT,UINT32,D3D_FEATURE_LEVEL,D3D11DDI_3DPIPELINELEVEL;
typedef uint64_t UINT64;typedef size_t SIZE_T;
enum {D3D_FEATURE_LEVEL_10_0=0xa000,D3D_FEATURE_LEVEL_10_1=0xa100,D3D_FEATURE_LEVEL_11_0=0xb000,D3D_FEATURE_LEVEL_11_1=0xb100};
enum {D3D10_0_DDI_INTERFACE_VERSION=0xa0001,D3D10_1_DDI_INTERFACE_VERSION=0xa0002,
 D3D10_0_x_vista_DDI_INTERFACE_VERSION=0xa0003,D3D10_1_x_vista_DDI_INTERFACE_VERSION=0xa0004,
 D3D11_0_DDI_INTERFACE_VERSION=0xb000a,D3D11_1_DDI_INTERFACE_VERSION=0xb000b,
 D3DWDDM1_3_DDI_INTERFACE_VERSION=0xb000c,D3DWDDM2_0_DDI_INTERFACE_VERSION=0xb000d,D3DWDDM2_1_DDI_INTERFACE_VERSION=0xb000e};
#define D3D10_0_DDI_SUPPORTED UINT64_C(0x000a000100040000)
#define D3D10_1_DDI_SUPPORTED UINT64_C(0x000a000200010000)
#define D3D10_0_x_vista_DDI_SUPPORTED UINT64_C(0x000a000300000000)
#define D3D10_1_x_vista_DDI_SUPPORTED UINT64_C(0x000a000400000000)
enum {D3D11DDI_3DPIPELINELEVEL_10_0=1,D3D11DDI_3DPIPELINELEVEL_10_1,D3D11DDI_3DPIPELINELEVEL_11_0,D3D11_1DDI_3DPIPELINELEVEL_11_1};
#define D3D11DDI_EXTRACT_3DPIPELINELEVEL_FROM_FLAGS(flags) ((flags)&15)
typedef struct {void *pDrvPrivate;} D3D10DDI_HADAPTER;
typedef void (*Fn)(void);
static void tritonDestroyDevice(void) {}
typedef struct {Fn pfnDestroyDevice;UINT marker;} DeviceFuncs;
static void fill(DeviceFuncs *p,UINT tag){p->marker=tag;}
static void tritonFillD3D10DeviceFuncs(DeviceFuncs*p){fill(p,10);}
static void tritonFillD3D10_1DeviceFuncs(DeviceFuncs*p){fill(p,11);}
static void tritonFillD3D11DeviceFuncs(DeviceFuncs*p){fill(p,20);}
static void tritonFillD3D11_1DeviceFuncs(DeviceFuncs*p){fill(p,21);}
static void tritonFillWDDM1_3DeviceFuncs(DeviceFuncs*p){fill(p,23);}
static void tritonFillWDDM2_0DeviceFuncs(DeviceFuncs*p){fill(p,30);}
static void tritonFillWDDM2_1DeviceFuncs(DeviceFuncs*p){fill(p,31);}
static void tritonDxgiPresent(void) {}
static void tritonDxgiGetGammaCaps(void) {}
static void tritonDxgiSetDisplayMode(void) {}
static void tritonDxgiSetResourcePriority(void) {}
static void tritonDxgiQueryResourceResidency(void) {}
static void tritonDxgiRotateResourceIdentities(void) {}
static void tritonDxgiBlt(void) {}
static void tritonDxgiResolveSharedResource(void) {}
static void runtime_present(void) {}
#define BASE_FIELDS Fn pfnPresent,pfnGetGammaCaps,pfnSetDisplayMode,pfnSetResourcePriority,pfnQueryResourceResidency,pfnRotateResourceIdentities,pfnBlt
typedef struct {BASE_FIELDS;} DXGI_DDI_BASE_FUNCTIONS;
typedef struct {BASE_FIELDS;Fn pfnResolveSharedResource;} DXGI1_1_DDI_BASE_FUNCTIONS;
typedef struct {Fn pfnPresentCb;} Callbacks;
typedef struct {union {DXGI_DDI_BASE_FUNCTIONS *pDXGIDDIBaseFunctions;DXGI1_1_DDI_BASE_FUNCTIONS *pDXGIDDIBaseFunctions2;};Callbacks *pDXGIBaseCallbacks;} BaseArgs;
typedef struct {Fn pfnPresentCb;} Device,*PTRITON_DEVICE;
typedef struct {UINT Interface,Version,Flags;void *pKTCallbacks,*pUMCallbacks;
 union {DeviceFuncs *pDeviceFuncs,*p10_1DeviceFuncs,*p11DeviceFuncs,*p11_1DeviceFuncs,*pWDDM1_3DeviceFuncs,*pWDDM2_0DeviceFuncs,*pWDDM2_1DeviceFuncs;};BaseArgs DXGIBaseDDI;} D3D10DDIARG_CREATEDEVICE;
/* WDK IS_DXGI1_1_BASE_FUNCTIONS rule: the runtime product version, not the
 * interface minor number, determines whether the eighth slot exists. */
#define IS_DXGI1_1_BASE_FUNCTIONS(i,v) ((((i)>>16)==10&&((v)&0xffff)>=(6000|9))||(((i)>>16)==11&&((v)&0xffff)>=9)||((i)>>16)>11)
typedef struct {UINT value;} AdapterCallbacks;
typedef struct {D3D10DDI_HADAPTER hRTAdapter;AdapterCallbacks callbacks;} Adapter,*PTRITON_ADAPTER;
typedef struct {Fn pfnCalcPrivateDeviceSize,pfnCreateDevice,pfnCloseAdapter;} AdapterFuncs;
typedef struct {UINT Interface;D3D10DDI_HADAPTER hRTAdapter,hAdapter;AdapterCallbacks *pAdapterCallbacks;AdapterFuncs *pAdapterFuncs;} D3D10DDIARG_OPENADAPTER;
static int oom;static unsigned allocations;
static void *GetProcessHeap(void){return NULL;}
static void *HeapAlloc(void*h,UINT flags,SIZE_T n){(void)h;(void)flags;++allocations;return oom?NULL:calloc(1,n);}
static void tritonCalcPrivateDeviceSize(void) {}
static void tritonCreateDevice(void) {}
static void tritonCloseAdapter(void) {}
// PRODUCTION
static unsigned checks;
#define CHECK(c) do{++checks;if(!(c)){fprintf(stderr,"EXTENDED FAIL line %d: %s\n",__LINE__,#c);exit(1);}}while(0)
int main(void)
{
 const UINT interfaces[]={0xa0001,0xa0002,0xa0003,0xa0004};
 for(UINT i=0;i<4;++i){
  const UINT expected=i&1?0xa100:0xa000;
  CHECK(tritonD3D10FeatureLevel(interfaces[i])==expected);
  struct {AdapterFuncs funcs;uintptr_t tail;} guarded={{0},UINTPTR_MAX};AdapterCallbacks callbacks={123};
  D3D10DDIARG_OPENADAPTER open={0};open.Interface=interfaces[i];open.pAdapterFuncs=&guarded.funcs;open.pAdapterCallbacks=&callbacks;open.hRTAdapter.pDrvPrivate=&callbacks;
  CHECK(OpenAdapter10(&open)==S_OK);CHECK(guarded.tail==UINTPTR_MAX);
  CHECK(guarded.funcs.pfnCreateDevice==tritonCreateDevice&&guarded.funcs.pfnCloseAdapter==tritonCloseAdapter&&guarded.funcs.pfnCalcPrivateDeviceSize==tritonCalcPrivateDeviceSize);
  CHECK(((Adapter*)open.hAdapter.pDrvPrivate)->callbacks.value==123&&((Adapter*)open.hAdapter.pDrvPrivate)->hRTAdapter.pDrvPrivate==&callbacks);free(open.hAdapter.pDrvPrivate);
  struct {DeviceFuncs funcs;uintptr_t tail;} table={{0},UINTPTR_MAX};D3D10DDIARG_CREATEDEVICE args={0};args.Interface=interfaces[i];args.pDeviceFuncs=&table.funcs;args.pKTCallbacks=args.pUMCallbacks=&callbacks;
  CHECK(validate_create((D3D10DDI_HADAPTER){0},&args)==S_OK);CHECK(install_requested(&args)==S_OK);
  CHECK(table.funcs.marker==(i&1?11u:10u)&&table.funcs.pfnDestroyDevice==tritonDestroyDevice&&table.tail==UINTPTR_MAX);
  for(UINT flag=0;flag<16;++flag){args.Flags=flag;CHECK(select_level(&args)==expected);}
  for(UINT updated=0;updated<2;++updated){
   uintptr_t slots[10];for(UINT n=0;n<10;++n)slots[n]=UINTPTR_MAX;
   Device dev={0};Callbacks cb={runtime_present};args.Version=updated?6009:6008;args.DXGIBaseDDI.pDXGIDDIBaseFunctions=(void*)slots;args.DXGIBaseDDI.pDXGIBaseCallbacks=&cb;
   tritonInstallDXGIFuncs(&dev,&args);CHECK(dev.pfnPresentCb==runtime_present);
   Fn expectedSlots[]={tritonDxgiPresent,tritonDxgiGetGammaCaps,tritonDxgiSetDisplayMode,tritonDxgiSetResourcePriority,tritonDxgiQueryResourceResidency,tritonDxgiRotateResourceIdentities,tritonDxgiBlt,tritonDxgiResolveSharedResource};
   for(UINT n=0;n<7+updated;++n)CHECK(slots[n]==(uintptr_t)expectedSlots[n]);
   for(UINT n=7+updated;n<10;++n)CHECK(slots[n]==UINTPTR_MAX);
  }
 }
 const UINT unsupported[]={0,0xa0000,0xa0005,0xa0006,0xa0007,0xa0009,0xa000a,0xb000a,UINT32_MAX};
 for(UINT i=0;i<sizeof(unsupported)/sizeof(*unsupported);++i){
  CHECK(!tritonD3D10FeatureLevel(unsupported[i]));AdapterFuncs funcs={0};AdapterCallbacks callbacks={0};D3D10DDIARG_OPENADAPTER open={0};open.Interface=unsupported[i];open.pAdapterFuncs=&funcs;open.pAdapterCallbacks=&callbacks;unsigned before=allocations;
  CHECK(OpenAdapter10(&open)==E_NOINTERFACE&&allocations==before);
  DeviceFuncs table={0};D3D10DDIARG_CREATEDEVICE args={0};args.Interface=unsupported[i];args.pDeviceFuncs=&table;args.pKTCallbacks=args.pUMCallbacks=&callbacks;
  CHECK(validate_create((D3D10DDI_HADAPTER){0},&args)==E_NOINTERFACE);
 }
 CHECK(OpenAdapter10(NULL)==E_INVALIDARG);CHECK(validate_create((D3D10DDI_HADAPTER){0},NULL)==E_INVALIDARG);
 AdapterFuncs funcs={0};AdapterCallbacks callbacks={0};D3D10DDIARG_OPENADAPTER open={0};open.Interface=0xa0003;open.pAdapterFuncs=&funcs;open.pAdapterCallbacks=&callbacks;oom=1;
 CHECK(OpenAdapter10(&open)==E_OUTOFMEMORY&&!open.hAdapter.pDrvPrivate&&!funcs.pfnCreateDevice);
 UINT count=0;CHECK(tritonGetSupportedVersions((D3D10DDI_HADAPTER){0},&count,NULL)==S_OK&&count==4);
 UINT64 versions[5]={0};count=5;CHECK(tritonGetSupportedVersions((D3D10DDI_HADAPTER){0},&count,versions)==S_OK&&count==4&&!versions[4]);
 CHECK(versions[0]==D3D10_0_DDI_SUPPORTED&&versions[1]==D3D10_1_DDI_SUPPORTED&&versions[2]==D3D10_0_x_vista_DDI_SUPPORTED&&versions[3]==D3D10_1_x_vista_DDI_SUPPORTED);
 printf("D3D10 extended versions %u checks passed\n",checks);return 0;
}
'''


def cpu():
    ddi = (TRITON / 'tritonDDI.c').read_text(encoding='utf-8-sig')
    dxgi = (TRITON / 'tritonDxgi.c').read_text()
    original = assemble(ddi, dxgi)
    variants = {
        'current': original,
        'missing-extended-negotiation': original.replace('    case D3D10_0_x_vista_DDI_INTERFACE_VERSION:\n', ''),
        'wrong-extended-table': original.replace('tritonFillD3D10DeviceFuncs(pArgs->pDeviceFuncs);', 'tritonFillD3D10_1DeviceFuncs(pArgs->pDeviceFuncs);'),
        'missing-table-installation': original.replace('HRESULT tableHr = tritonInstallDeviceFuncs(pArgs);', 'HRESULT tableHr = S_OK;'),
        'lost-feature-level': original.replace('requested = legacyLevel;', 'requested = D3D_FEATURE_LEVEL_10_1;'),
        'missing-advertisement': original.replace('        D3D10_0_x_vista_DDI_SUPPORTED,\n', ''),
        'missing-resolve-slot': original.replace('if (IS_DXGI1_1_BASE_FUNCTIONS(pArgs->Interface, pArgs->Version))', 'if (FALSE)'),
        'overwritten-base-table': original.replace('if (IS_DXGI1_1_BASE_FUNCTIONS(pArgs->Interface, pArgs->Version))', 'if (TRUE)'),
        'interface-instead-of-runtime-version': original.replace('IS_DXGI1_1_BASE_FUNCTIONS(pArgs->Interface, pArgs->Version)', '(pArgs->Interface >= D3D10_0_x_vista_DDI_INTERFACE_VERSION)'),
    }
    with tempfile.TemporaryDirectory(prefix='triton-extended-') as tmp:
        tmp = Path(tmp)
        for name, code in variants.items():
            assert name == 'current' or code != original, name
            path, binary = tmp / 'test.c', tmp / 'test'
            path.write_text(FIXTURE.replace('// PRODUCTION', code))
            subprocess.run(['clang', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-function', '-Wno-unused-parameter', '-fsanitize=address,undefined',
                str(path), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            if name == 'current':
                assert result.returncode == 0, result.stdout + result.stderr
                print(result.stdout.strip())
            else:
                assert result.returncode != 0 and 'EXTENDED FAIL' in result.stderr, result.stdout + result.stderr
                print('D3D10 extended negative control rejected:', name)


def handoff():
    dxgi = (TRITON / 'tritonDxgi.c').read_text()
    bridge = (TRITON / 'tritonSharedBridge.c').read_text()
    original = function(bridge, 'tritonSharedBridgeDrain') + '\n' + function(dxgi, 'tritonDxgiResolveSharedResource')
    fixture = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#define APIENTRY
#define E_INVALIDARG -1
#define DXGI_ERROR_DEVICE_REMOVED -2
typedef int HRESULT;
struct npt_renderer {int markerFailure;};
struct npt_ring {unsigned pending,submitted,flushes;bool failed;};
struct npt_device {struct npt_renderer *renderer;};
typedef struct {struct npt_device *device;struct npt_ring *ring;} Context;
typedef struct {Context *pCtx1;HRESULT *pDev1;bool presentLockInit;unsigned presentLock;} Device,*PTRITON_DEVICE;
typedef struct {void *pResource;} Resource,*PTRITON_RESOURCE;
typedef struct {uintptr_t hDevice,hResource;} DXGI_DDI_ARG_RESOLVESHAREDRESOURCE;
static unsigned checks,sequence,flushAt,drainAt,markerAt,queryAt,locks,unlocks;
#define CHECK(c) do{++checks;if(!(c)){fprintf(stderr,"HANDOFF FAIL line %d: %s\n",__LINE__,#c);exit(1);}}while(0)
static void EnterCriticalSection(unsigned *lock){CHECK(!*lock);++*lock;++locks;}
static void LeaveCriticalSection(unsigned *lock){CHECK(*lock==1);--*lock;++unlocks;}
static void ID3D11DeviceContext1_Flush(Context *c){flushAt=++sequence;++c->ring->flushes;}
static HRESULT ID3D11Device1_GetDeviceRemovedReason(HRESULT *status){queryAt=++sequence;return *status;}
static struct npt_device *npt_com_self_device(void *p){return ((Context*)p)->device;}
static struct npt_ring *npt_com_self_ring(void *p){return ((Context*)p)->ring;}
static bool npt_ring_wait_all_timeout(struct npt_ring *r,uint32_t timeout){drainAt=++sequence;CHECK(timeout==15000);if(r->failed)return false;if(r->flushes)r->submitted=r->pending;return true;}
static bool npt_renderer_submit_cmd_sync(struct npt_renderer *r,const void *p,unsigned size){markerAt=++sequence;CHECK(!p&&!size);return !r->markerFailure;}
// PRODUCTION
int main(void){
 struct npt_renderer renderer={0};struct npt_ring contextRing={42,0,0,false},otherRing={99,0,0,false};struct npt_device transport={&renderer};Context context={&transport,&contextRing};HRESULT status=0;Device device={&context,&status,true,0};Resource resource={&otherRing};
 DXGI_DDI_ARG_RESOLVESHAREDRESOURCE args={(uintptr_t)&device,(uintptr_t)&resource};
 CHECK(tritonDxgiResolveSharedResource(&args)==0);CHECK(contextRing.submitted==42&&contextRing.flushes==1&&!otherRing.submitted);
 CHECK(flushAt<drainAt&&drainAt<markerAt&&markerAt<queryAt);CHECK(locks==1&&unlocks==1&&!device.presentLock);
 CHECK(tritonDxgiResolveSharedResource(NULL)==E_INVALIDARG);args.hDevice=0;CHECK(tritonDxgiResolveSharedResource(&args)==E_INVALIDARG);args.hDevice=(uintptr_t)&device;
 args.hResource=0;CHECK(tritonDxgiResolveSharedResource(&args)==E_INVALIDARG);args.hResource=(uintptr_t)&resource;resource.pResource=NULL;CHECK(tritonDxgiResolveSharedResource(&args)==E_INVALIDARG);resource.pResource=&otherRing;
 device.pCtx1=NULL;CHECK(tritonDxgiResolveSharedResource(&args)==E_INVALIDARG);device.pCtx1=&context;device.pDev1=NULL;CHECK(tritonDxgiResolveSharedResource(&args)==E_INVALIDARG);device.pDev1=&status;CHECK(locks==1&&contextRing.flushes==1);
 contextRing.failed=true;queryAt=markerAt=0;CHECK(tritonDxgiResolveSharedResource(&args)==DXGI_ERROR_DEVICE_REMOVED);CHECK(!queryAt&&!markerAt&&locks==unlocks);contextRing.failed=false;
 renderer.markerFailure=1;queryAt=0;CHECK(tritonDxgiResolveSharedResource(&args)==DXGI_ERROR_DEVICE_REMOVED);CHECK(!queryAt&&locks==unlocks);renderer.markerFailure=0;
 status=-7;CHECK(tritonDxgiResolveSharedResource(&args)==-7);CHECK(locks==unlocks);
 device.presentLockInit=false;unsigned before=locks;status=0;CHECK(tritonDxgiResolveSharedResource(&args)==0);CHECK(locks==before&&unlocks==before);
 printf("D3D10 shared handoff %u checks passed\n",checks);return 0;
}
'''
    mutations = {
        'missing-flush': ('   ID3D11DeviceContext1_Flush(pD->pCtx1);', ''),
        'missing-context-drain': ('tritonSharedBridgeDrain(pD->pCtx1, 15000u)', 'true'),
        'missing-device-status': ('ID3D11Device1_GetDeviceRemovedReason(pD->pDev1)', '0'),
        'ignored-drain-failure': ('return hr;', 'return 0;'),
    }
    with tempfile.TemporaryDirectory(prefix='triton-handoff-') as tmp:
        tmp = Path(tmp)
        for name in ('current', *mutations):
            code = original
            if name != 'current':
                old, new = mutations[name]
                assert old in code
                code = code.replace(old, new)
            path, binary = tmp / 'test.c', tmp / 'test'
            path.write_text(fixture.replace('// PRODUCTION', code))
            subprocess.run(['clang', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-function', '-Wno-unused-variable', '-fsanitize=address,undefined',
                str(path), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            if name == 'current':
                assert result.returncode == 0, result.stdout + result.stderr
                print(result.stdout.strip())
            else:
                assert result.returncode != 0 and 'HANDOFF FAIL' in result.stderr, result.stdout + result.stderr
                print('D3D10 handoff negative control rejected:', name)


def format_masks():
    original = function((ROOT / 'tests/vista/test-d3d10-runtime.c').read_text(), 'extended_format_caps_valid')
    fixture = r'''
#include <d3d10.h>
#include <cstdio>
#include <cstdlib>
// PRODUCTION
static unsigned checks;
#define CHECK(c) do{++checks;if(!(c)){std::fprintf(stderr,"FORMAT MASK FAIL line %d: %s\n",__LINE__,#c);std::exit(1);}}while(0)
int main(){
 /* Independent table transcribed from the published R/O/N/A columns.
  * Optional backbuffer cast reporting applies only to scanout formats. */
 const struct {DXGI_FORMAT format;UINT required,optional;} rows[]={
  {DXGI_FORMAT_B8G8R8A8_TYPELESS,0x1210f0,0},
  {DXGI_FORMAT_B8G8R8A8_UNORM,0x5ef3f3,0x1200000},
  {DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,0x5ef3f0,0x1200000},
  {DXGI_FORMAT_B8G8R8X8_TYPELESS,0x1210f0,0},
  {DXGI_FORMAT_B8G8R8X8_UNORM,0x56f3f3,0x200000},
  {DXGI_FORMAT_B8G8R8X8_UNORM_SRGB,0x56f3f0,0x200000}};
 for(const auto &r:rows){
  CHECK(extended_format_caps_valid(r.format,r.required));CHECK(extended_format_caps_valid(r.format,r.required|r.optional));
  for(UINT i=0;i<32;++i){UINT bit=UINT(1)<<i;
   if(r.required&bit)CHECK(!extended_format_caps_valid(r.format,r.required&~bit));
   else if(r.optional&bit)CHECK(extended_format_caps_valid(r.format,r.required|bit));
   else CHECK(!extended_format_caps_valid(r.format,r.required|bit));
  }
 }
 CHECK(!extended_format_caps_valid(DXGI_FORMAT_UNKNOWN,0));
 CHECK(!extended_format_caps_valid(DXGI_FORMAT_B8G8R8A8_UNORM,0x013ef3f3));
 CHECK(!extended_format_caps_valid(DXGI_FORMAT_B8G8R8X8_UNORM,0x0036f3f3));
 std::printf("D3D10 extended format masks %u checks passed\n",checks);return 0;
}
'''
    mutants = {
        'missing-required-resolve': original.replace('D3D10_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE', '0'),
        'missing-required-load': original.replace('D3D10_FORMAT_SUPPORT_MULTISAMPLE_LOAD', '0'),
        'forbidden-bits-accepted': original.replace(' && !(support&~(required|optional))', ''),
        'forbidden-depth-accepted': original.replace('UINT optional=0;', 'UINT optional=D3D10_FORMAT_SUPPORT_DEPTH_STENCIL;'),
        'optional-msaa-rejected': original.replace('optional=D3D10_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET;', 'optional=0;'),
    }
    with tempfile.TemporaryDirectory(prefix='triton-format-mask-') as tmp:
        tmp = Path(tmp)
        for name, code in {'current': original, **mutants}.items():
            assert name == 'current' or code != original, name
            path, binary = tmp / 'test.cpp', tmp / 'test'
            path.write_text(fixture.replace('// PRODUCTION', code))
            subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-unused-but-set-variable',
                '-fsanitize=address,undefined', *native_headers(),
                str(path), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            if name == 'current':
                assert result.returncode == 0, result.stdout + result.stderr
                print(result.stdout.strip())
            else:
                assert result.returncode != 0 and 'FORMAT MASK FAIL' in result.stderr, result.stdout + result.stderr
                print('D3D10 format mask negative control rejected:', name)


def abi():
    for arch in ('x64', 'x86'):
        entries = json.loads((vista_compile_database(arch)).read_text())
        for filename in ('tritonDDI.c', 'tritonDxgi.c'):
            entry = next(e for e in entries if e['file'].endswith('/triton/' + filename) and 'vista-d3d10' in e['command'])
            args, it = [], iter(shlex.split(entry['command']))
            for arg in it:
                if arg in ('-o', '-MF', '-MQ', '-MT'):
                    next(it)
                elif arg not in ('-c', '-MD', '-MMD', entry['file']):
                    args.append(arg)
            args += ['-Werror=incompatible-pointer-types', '-Werror=cast-function-type', '-fsyntax-only', '-x', 'c', '-']
            source = '#include "/workspace/triton-umd/src/virtio/neptune/triton/' + filename + '"\n'
            source += '_Static_assert(D3D10_0_x_vista_DDI_INTERFACE_VERSION==0xa0003 && D3D10_1_x_vista_DDI_INTERFACE_VERSION==0xa0004,"Vista extended interface IDs");\n'
            source += '_Static_assert(offsetof(DXGI1_1_DDI_BASE_FUNCTIONS,pfnResolveSharedResource)==sizeof(DXGI_DDI_BASE_FUNCTIONS),"DXGI 1.1 exact extension");\n'
            command = container_command(*args, workdir=entry['directory'])
            result = subprocess.run(command, input=source, capture_output=True, text=True)
            assert result.returncode == 0, result.stdout + result.stderr
            if filename == 'tritonDxgi.c':
                bad = source + 'void bad(DXGI1_1_DDI_BASE_FUNCTIONS *p){p->pfnResolveSharedResource=tritonDxgiBlt;}\n'
                result = subprocess.run(command, input=bad, capture_output=True, text=True)
                assert result.returncode != 0 and 'incompatible' in result.stderr, result.stderr
            print(f'D3D10 extended {arch} {filename} actual WDK ABI passed')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--abi', action='store_true', help='also compile actual WDK translation units without build outputs')
    args = parser.parse_args()
    cpu()
    handoff()
    format_masks()
    if args.abi:
        abi()
