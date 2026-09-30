#!/usr/bin/env python3
import importlib.util
from pathlib import Path
import subprocess, tempfile
root=Path(__file__).resolve().parents[2]
def extract_static_helper(source: str, name: str) -> str:
    marker = f"static HRESULT\n{name}(TRITON9_DEVICE *device)"
    start = source.find(marker)
    if start < 0:
        raise RuntimeError(f"could not find {name} with the expected signature")
    body = source.find("{", start)
    depth = 0
    for index in range(body, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
    raise RuntimeError(f"unterminated helper {name}")


helper=extract_static_helper((root/'triton-umd/src/virtio/neptune/vista-d3d9/triton9_resource.c').read_text(),'triton9WaitForPresentGpuCompletion')
pre=r'''
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
typedef int HRESULT;
typedef uint32_t DWORD;
typedef uint64_t UINT64;
typedef int ID3D11Device5;
typedef int ID3D11DeviceContext4;
typedef int ID3D11Fence;
typedef struct {void *hostDevice,*hostContext; ID3D11Fence *presentFence; ID3D11DeviceContext4 *presentContext; UINT64 presentFenceValue; int deviceLost;} TRITON9_DEVICE;
#define S_OK 0
#define E_FAIL -1
#define E_INVALIDARG -2
#define D3DDDIERR_DEVICEREMOVED -3
#define TRUE 1
#define FAILED(x) ((x)<0)
#define SUCCEEDED(x) ((x)>=0)
#define IID_ID3D11Device5 1
#define IID_ID3D11DeviceContext4 2
#define IID_ID3D11Fence 3
#define D3D11_FENCE_FLAG_NONE 0
#define TRITON9_HOST_DRAIN_TIMEOUT_MS 15000u
static int obj,qihr,createhr,signalhr,release_count,create_count,signal_count,sleeps,reads,logs;
static UINT64 values[16],targets[16];
static DWORD tick,step;
static HRESULT qi(void **p){if(!qihr)*p=&obj;return qihr;}
static HRESULT create(void **p){create_count++;if(!createhr)*p=&obj;return createhr;}
static HRESULT signal(UINT64 v){targets[signal_count++]=v;return signalhr;}
#define ID3D11Device1_QueryInterface(d,i,p) qi(p)
#define ID3D11DeviceContext1_QueryInterface(d,i,p) qi(p)
#define ID3D11Device5_CreateFence(d,v,f,i,p) create(p)
#define ID3D11Device5_Release(d) ((void)(d),release_count++)
#define ID3D11Fence_Release(d) ((void)(d),release_count++)
#define ID3D11DeviceContext4_Release(d) ((void)(d),release_count++)
#define ID3D11DeviceContext4_Signal(c,f,v) signal(v)
#define ID3D11DeviceContext1_Flush(c) ((void)0)
#define ID3D11Fence_GetCompletedValue(f) values[reads++]
static DWORD GetTickCount(void){DWORD v=tick;tick+=step;return v;}
#define Sleep(ms) (++sleeps)
#define triton9PollHostDevice(d) S_OK
#define triton9MapDeviceFailure(d,h) (h)
#define triton9Diag(s) (++logs)
static void reset(void){qihr=createhr=signalhr=release_count=create_count=signal_count=sleeps=reads=logs=0;tick=step=0;memset(values,0,sizeof(values));}
'''
tests=r'''
int main(void){
 TRITON9_DEVICE d={.hostDevice=&obj,.hostContext=&obj};
 reset();values[0]=0;values[1]=1;
 assert(triton9WaitForPresentGpuCompletion(&d)==S_OK);
 assert(sleeps==1&&create_count==1&&targets[0]==1&&logs==1);
 values[2]=1;values[3]=1;values[4]=2;
 assert(triton9WaitForPresentGpuCompletion(&d)==S_OK);
 assert(sleeps==3&&reads==5&&create_count==1&&targets[1]==2&&logs==1);
 reset();values[0]=UINT64_MAX;
 assert(triton9WaitForPresentGpuCompletion(&d)==D3DDDIERR_DEVICEREMOVED&&d.deviceLost);
 reset();d.deviceLost=0;tick=0xfffffff0u;step=15001;
 assert(triton9WaitForPresentGpuCompletion(&d)==D3DDDIERR_DEVICEREMOVED&&d.deviceLost);
 reset();d.presentFenceValue=UINT64_MAX-1;
 assert(triton9WaitForPresentGpuCompletion(&d)==D3DDDIERR_DEVICEREMOVED&&signal_count==0);
 reset();d.presentFenceValue=0;signalhr=E_FAIL;
 assert(triton9WaitForPresentGpuCompletion(&d)==E_FAIL&&reads==0);
 reset();d.presentFence=NULL;d.presentContext=NULL;createhr=E_FAIL;
 assert(triton9WaitForPresentGpuCompletion(&d)==E_FAIL&&release_count==2&&!d.presentFence&&!d.presentContext);
 reset();qihr=E_FAIL;
 assert(triton9WaitForPresentGpuCompletion(&d)==E_FAIL&&create_count==0);
 puts("PASS: frame reuse rejects stale completion; initialization cleanup; signal failure; device removal; timeout wrap; counter exhaustion");
}
'''
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp);(p/'test.c').write_text(pre+helper+tests)
 build=subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],capture_output=True,text=True)
 run=None if build.returncode else subprocess.run([str(p/'test')],capture_output=True,text=True)
 report=build.stdout+build.stderr+(run.stdout+run.stderr if run else '')
 print(report)
 raise SystemExit(build.returncode or run.returncode)
