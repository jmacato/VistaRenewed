#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
typedef int HRESULT;
typedef void IUnknown;
typedef struct {void *hostContext, *hostDevice; bool lost;} TRITON9_DEVICE;
#define S_OK 0
#define FAILED(h) ((h)<0)
static bool healthy=true, fail_during_rpc;
static int calls, host_result;
static HRESULT triton9EnsureHostDevice(TRITON9_DEVICE *d) {return d->lost ? -1 : 0;}
static bool triton9TransportHealthy(void *o) {assert(o);return healthy;}
static HRESULT triton9DeviceRemoved(TRITON9_DEVICE *d) {d->lost=true;return -1;}
static HRESULT ID3D11Device1_GetDeviceRemovedReason(void *d) {
    assert(d); calls++;
    if (fail_during_rpc) healthy=false;
    return host_result;
}
/* SOURCE_UNDER_TEST */
int main(void) {
    TRITON9_DEVICE d={.hostContext=(void *)1,.hostDevice=(void *)2};
    for (int i=0;i<100000;i++) assert(triton9CheckHostDevice(&d)==S_OK);
    assert(calls==0);
    assert(triton9PollHostDevice(&d)==S_OK && calls==1);
    host_result=-2;
    assert(triton9PollHostDevice(&d)<0 && d.lost && calls==2);
    assert(triton9CheckHostDevice(&d)<0 && calls==2);
    d.lost=false; host_result=0; healthy=false;
    assert(triton9CheckHostDevice(&d)<0 && d.lost && calls==2);
    d.lost=false; healthy=true; fail_during_rpc=true;
    assert(triton9PollHostDevice(&d)<0 && d.lost && calls==3);
    puts("PASS hot checks make no RPC; boundary detects removal and mid-call transport failure");
}
