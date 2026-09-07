from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'triton-umd/src/virtio/neptune/vista-d3d9/triton9_resource.c').read_text()
a=s.index('static HRESULT\ntriton9WaitForPresentConsumption(');b=s.index('\nHRESULT APIENTRY\ntriton9Present(',a)
code=r'''
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
typedef int32_t HRESULT;typedef uint32_t UINT,DWORD;typedef uint8_t BYTE;typedef void*HANDLE;
#define TRUE 1
#define FALSE 0
#define S_OK 0
#define E_FAIL -1
#define D3DDDIERR_DEVICEREMOVED -2
#define FAILED(x) ((x)<0)
#define ZeroMemory(p,n) memset(p,0,n)
#define CopyMemory memcpy
#define VioGpuUmHandle(h) ((uint64_t)(uintptr_t)(h))
#define VIOGPU_CMD_SIGNAL_EVENT 6
#define WAIT_OBJECT_0 0
#define TRITON9_HOST_DRAIN_TIMEOUT_MS 15000
struct D3DDDICB_RENDER { UINT CommandLength,CommandOffset,NumAllocations,NumPatchLocations;HANDLE hContext;void*pNewCommandBuffer;UINT NewCommandBufferSize;void*pNewAllocationList;UINT NewAllocationListSize;void*pNewPatchLocationList;UINT NewPatchLocationListSize;};
typedef struct D3DDDICB_RENDER D3DDDICB_RENDER;
typedef struct {UINT type,size,flags,ring_idx;} VIOGPU_COMMAND_HDR;
typedef struct {uint64_t Event;} VIOGPU_SIGNAL_EVENT_CMD;
typedef struct {int kmContextLock;HANDLE hKMContext,hRTDevice;struct {HRESULT(*pfnRenderCb)(HANDLE,D3DDDICB_RENDER*);}callbacks;void*kmCommandBuffer;UINT kmCommandBufferSize;void*kmAllocationList;UINT kmAllocationListSize;void*kmPatchLocationList;UINT kmPatchLocationListSize;int deviceLost,presentConsumptionReported;} TRITON9_DEVICE;
static TRITON9_DEVICE*d;static BYTE oldbuf[64],newbuf[64];static int mode,closed,waited,locked;
static HANDLE CreateEventW(void*a,int b,int c,void*e){(void)a;(void)b;(void)c;(void)e;return mode==5?NULL:(void*)0x1234;}
static void EnterCriticalSection(int*p){(void)p;assert(!locked);locked=1;}
static void LeaveCriticalSection(int*p){(void)p;assert(locked);locked=0;}
static void CloseHandle(HANDLE h){assert(h==(void*)0x1234);closed++;}
static DWORD WaitForSingleObject(HANDLE h,DWORD timeout){assert(h==(void*)0x1234&&timeout==15000&&!locked&&d->kmCommandBuffer==newbuf);waited++;return mode==3?258:mode==4?0xffffffff:0;}
static HRESULT triton9MapDeviceFailure(TRITON9_DEVICE*x,HRESULT h){(void)x;return h;}
static void triton9Diag(const char*s){(void)s;}
static HRESULT render(HANDLE h,D3DDDICB_RENDER*r){assert(h==(void*)7&&locked&&r->hContext==(void*)8&&r->CommandLength==24&&!r->CommandOffset&&!r->NumAllocations&&!r->NumPatchLocations);
 VIOGPU_COMMAND_HDR header;VIOGPU_SIGNAL_EVENT_CMD signal;memcpy(&header,oldbuf,16);memcpy(&signal,oldbuf+16,8);assert(header.type==6&&header.size==8&&!header.flags&&!header.ring_idx&&signal.Event==0x1234);
 r->pNewCommandBuffer=newbuf;r->NewCommandBufferSize=64;r->pNewAllocationList=(void*)9;r->NewAllocationListSize=3;r->pNewPatchLocationList=(void*)10;r->NewPatchLocationListSize=4;
 if(mode==2)r->pNewPatchLocationList=NULL;return mode==1?E_FAIL:S_OK;
}
'''+s[a:b]+r'''
int main(void){for(mode=0;mode<6;mode++){
 TRITON9_DEVICE dev={.hKMContext=(void*)8,.hRTDevice=(void*)7,.callbacks={render},.kmCommandBuffer=oldbuf,.kmCommandBufferSize=64};d=&dev;closed=waited=locked=0;
 HRESULT h=triton9WaitForPresentConsumption(d);assert(!locked);
 if(!mode){assert(h==S_OK&&!d->deviceLost&&waited==1&&closed==1&&d->kmAllocationList==(void*)9&&d->kmPatchLocationListSize==4);}
 else {assert(h<0&&d->deviceLost);assert(closed==(mode==5?0:1));assert(waited==(mode==3||mode==4));if(mode==1||mode==2)assert(!d->kmCommandBuffer&&!d->kmAllocationList&&!d->kmPatchLocationList);}
 }puts("PASS same-context event packet, buffer rotation, callback failure, incomplete replacement, timeout, failed wait, event creation failure");}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.c';p.write_text(code);exe=Path(d)/'test'
 subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
