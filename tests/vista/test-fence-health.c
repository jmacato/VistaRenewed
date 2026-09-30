#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef uint64_t UINT64;
typedef int32_t HRESULT;
#define NPT_STDMETHODCALLTYPE
struct npt_ring { bool healthy; } primary={true}, method={true};
struct npt_device {struct npt_ring *ring;} device={&primary};
struct npt_d3d11_fence_aux {struct {bool registered;} base;} aux={{true}};
struct npt_d3d11_fence_feedback_slot {atomic_uint_fast64_t completed_value;} slot={42};
static bool poison_on_rpc;
static int calls;
static HRESULT host_reason;
static int reason_calls;
static struct npt_device *npt_com_self_device(void *s) {(void)s;return &device;}
static struct npt_ring *npt_com_self_ring(void *s) {(void)s;return &method;}
static uint64_t npt_com_self_id(void *s) {(void)s;return 123;}
static bool npt_ring_is_healthy(struct npt_ring *r) {return r && r->healthy;}
static struct npt_d3d11_fence_aux *fence_aux(void *s) {(void)s;return &aux;}
static struct npt_d3d11_fence_feedback_slot *fence_slot(struct npt_d3d11_fence_aux *a) {(void)a;return &slot;}
static UINT64 npt_id3d11fence_default_GetCompletedValue(void *s) {
    (void)s; calls++;
    if (poison_on_rpc) method.healthy=false;
    return 17;
}
static HRESULT npt_call_ID3D11Device_GetDeviceRemovedReason(struct npt_ring *r, uint64_t id) {
    assert(r==&method && id==123);
    reason_calls++;
    if (poison_on_rpc) method.healthy=false;
    return host_reason;
}
/* SOURCE_UNDER_TEST */
int main(void) {
    assert(fence_GetCompletedValue_override(NULL)==42 && !calls);
    method.healthy=false;
    assert(fence_GetCompletedValue_override(NULL)==UINT64_MAX && !calls);
    method.healthy=true;primary.healthy=false;
    assert(fence_GetCompletedValue_override(NULL)==UINT64_MAX && !calls);
    primary.healthy=true;aux.base.registered=false;
    assert(fence_GetCompletedValue_override(NULL)==17 && calls==1);
    poison_on_rpc=true;
    assert(fence_GetCompletedValue_override(NULL)==UINT64_MAX && calls==2);
    method.healthy=true; poison_on_rpc=false;
    assert(dev_GetDeviceRemovedReason_override(NULL)==0 && reason_calls==1);
    host_reason=(HRESULT)0x887a0006; // Preserve the host's specific HUNG reason.
    assert(dev_GetDeviceRemovedReason_override(NULL)==host_reason && reason_calls==2);
    poison_on_rpc=true;
    assert(dev_GetDeviceRemovedReason_override(NULL)==(HRESULT)0x887a0005 && reason_calls==3);
    assert(dev_GetDeviceRemovedReason_override(NULL)==(HRESULT)0x887a0005 && reason_calls==3);
    puts("PASS cached fence rejects transport failure; device-status query returns real host result");
}
