#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define unlikely(x) (x)
#define NPT_RING_STATUS_IDLE_BIT 1u
struct npt_cs_encoder { uint8_t *cur; const uint8_t *end; bool fatal; };
struct npt_ring {
    struct { uint32_t cur; } buffer;
    uint32_t tail, status;
    bool failed;
};
struct npt_ring_submit_command {
    void *cmd_data;
    size_t cmd_size, reply_size;
    struct npt_cs_encoder enc;
};
static bool npt_ring_is_failed(struct npt_ring *r) { return r->failed; }
static bool npt_ring_mark_failed(struct npt_ring *r, const char *reason) {
    (void)reason;
    r->failed = true;
    return false;
}
static int interleave, point;
static bool notified;
static void guest_publish(struct npt_ring *r) {
    r->tail = 24;
    notified = !!(r->status & NPT_RING_STATUS_IDLE_BIT);
}
static void schedule(struct npt_ring *r) {
    if (point++ == interleave) guest_publish(r);
}
static void npt_ring_set_status_bits(struct npt_ring *r, uint32_t bits) {
    schedule(r);
    r->status |= bits;
    schedule(r);
}
static uint32_t npt_ring_load_tail(struct npt_ring *r) {
    schedule(r);
    uint32_t tail = r->tail;
    schedule(r);
    return tail;
}
/* SOURCE_UNDER_TEST */
int main(void) {
    /* Publish at every boundary of the real host sleep decision. A command
     * must either prevent sleeping or cause a doorbell under the host mutex. */
    for (interleave = 0; interleave < 5; interleave++) {
        struct npt_ring r = {0};
        point = 0; notified = false;
        bool sleep = npt_ring_prepare_wait(&r);
        schedule(&r);
        assert(r.tail == 24);
        assert(!sleep || notified);
    }
    struct npt_ring r = {.failed = true};
    struct npt_ring_submit_command submit;
    uint8_t bytes[32], before[32];
    memset(bytes, 0xa5, sizeof(bytes));
    memcpy(before, bytes, sizeof(bytes));
    struct npt_cs_encoder *enc = npt_ring_submit_command_init(
        &r, &submit, bytes, sizeof(bytes), 0);
    assert(!enc && !submit.cmd_size && !submit.cmd_data);
    uint32_t command = 42;
    npt_cs_encoder_write(enc, sizeof(command), &command, sizeof(command));
    assert(!memcmp(bytes, before, sizeof(bytes)));
    r.failed = false;
    enc = npt_ring_submit_command_init(&r, &submit, bytes, sizeof(bytes), 0);
    assert(enc);
    npt_cs_encoder_write(enc, sizeof(command), &command, sizeof(command));
    assert(!memcmp(bytes, &command, sizeof(command)));
    assert(enc->cur == bytes + sizeof(command));
    npt_cs_encoder_write(enc, sizeof(bytes), bytes, sizeof(bytes));
    assert(enc->cur == bytes + sizeof(command) && enc->fatal);
    npt_cs_encoder_write(enc, sizeof(command), &command, sizeof(command));
    assert(enc->cur == bytes + sizeof(command));
    assert(!npt_ring_submit_command_init(&r, &submit, NULL, 0, 0) && r.failed);
    puts("PASS idle/publication interleavings; failed ring encoder; valid write and bounds");
}
