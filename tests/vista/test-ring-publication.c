#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define unlikely(x) (x)
#define NPT_RING_STATUS_IDLE_BIT 1u
#define NPT_RING_STATUS_FATAL_BIT 2u
struct npt_ring {
    atomic_uint *tail, *status;
    uint32_t cur;
    bool failed;
};
static unsigned notifications;
static bool notify_ok=true;
static bool npt_ring_notify(struct npt_ring *r) {
    assert(atomic_load(r->tail)==r->cur);
    notifications++; return notify_ok;
}
static bool npt_ring_wait_space(struct npt_ring *r, uint32_t s) {
    return !r->failed && s<=4096;
}
static void npt_ring_write_buffer(struct npt_ring *r, const void *d, uint32_t s) {
    assert(d); r->cur+=s;
}
static void npt_ring_write_buffer_at(struct npt_ring *r, uint32_t c, const void *d, uint32_t s) {
    (void)r; (void)c; (void)s; assert(d);
}
static bool npt_ring_mark_failed(struct npt_ring *r, const char *why) {
    (void)why; r->failed=true; return false;
}
/* SOURCE_UNDER_TEST */
int main(void) {
    atomic_uint tail=0, status=NPT_RING_STATUS_IDLE_BIT;
    struct npt_ring r={.tail=&tail,.status=&status};
    uint32_t data=0;
    // The host may re-enter IDLE immediately after a feedback/spurious wake.
    // Every observed IDLE needs a doorbell even within one millisecond.
    assert(npt_ring_submit_locked(&r,&data,4));
    assert(npt_ring_submit_locked(&r,&data,4));
    assert(npt_ring_submit_locked_split(&r,&data,4,&data,4,8));
    assert(npt_ring_submit_locked_split(&r,&data,4,&data,4,8));
    assert(notifications==4 && atomic_load(&tail)==32);
    atomic_store(&status,0);
    assert(npt_ring_submit_locked(&r,&data,4) && notifications==4);
    atomic_store(&status,NPT_RING_STATUS_IDLE_BIT);
    notify_ok=false;
    assert(!npt_ring_submit_locked(&r,&data,4));
    atomic_store(&status,NPT_RING_STATUS_FATAL_BIT);
    assert(!npt_ring_submit_locked_split(&r,&data,4,&data,4,8) && r.failed);
    puts("PASS direct/split publication; repeated idle notification; fatal and doorbell failures");
}
