/* CPU fixture: extracted QEMU reset, queue and scanout functions; no EGL. */
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stddef.h>
#include <sys/uio.h>
#include "qemu/queue.h"

static atomic_uint checks;
static const char *scenario;
#define CHECK(x) do { \
    checks++; \
    if (!(x)) { \
        fprintf(stderr, "CHECK FAILED: %s: %s, line %d\n", \
                scenario, #x, __LINE__); \
        exit(1); \
    } \
} while (0)

typedef struct VirtIOGPU VirtIOGPU;
typedef VirtIOGPU VirtIOGPUGL;
typedef struct VirtIODevice { unsigned unused; } VirtIODevice;
typedef struct Error { int ignored; } Error;
typedef VirtIODevice DeviceState;
typedef struct { bool scheduled; unsigned count; } QEMUBH;
typedef struct { bool armed; } QEMUTimer;
typedef struct VirtQueue {
    bool ready;
    void *element;
    bool cursor;
} VirtQueue;
typedef struct {
    struct iovec *out_sg;
    unsigned out_num;
} VirtQueueElement;
struct virtio_gpu_update_cursor { uint32_t resource_id; };
struct virtio_gpu_scanout {
    uint32_t resource_id, width, height, x, y, invalidate;
    void *con, *ds;
    struct { unsigned words[8]; } fb;
    struct { uint32_t width, height, data[4]; } *current_cursor;
};
struct virtio_gpu_simple_resource {
    uint32_t resource_id, scanout_bitmask;
    QTAILQ_ENTRY(virtio_gpu_simple_resource) next;
};
struct virtio_gpu_virgl_resource {
    struct virtio_gpu_simple_resource base;
    void *external_copy;
};
#define container_of(p, type, field) ((type *)((char *)(p) - offsetof(type, field)))
struct virtio_gpu_ctrl_command {
    VirtQueue *vq;
    int error;
    bool finished, suspended, context_fence;
    struct { unsigned flags, type; } cmd_hdr;
    QTAILQ_ENTRY(virtio_gpu_ctrl_command) next;
};
typedef struct {
    VirtIODevice parent_obj;
    struct { unsigned max_outputs; bool stats; } conf;
    struct virtio_gpu_scanout scanout[2];
    int renderer_blocked;
} VirtIOGPUBase;
typedef struct VirtIOGPUClass {
    bool (*reset)(VirtIOGPU *g);
    void (*handle_ctrl)(VirtIODevice *vdev, VirtQueue *vq);
    void (*process_cmd)(VirtIOGPU *g, struct virtio_gpu_ctrl_command *cmd);
    void (*resource_destroy)(VirtIOGPU *g,
                             struct virtio_gpu_simple_resource *res,
                             Error **errp);
} VirtIOGPUClass;
typedef struct { void (*reset)(VirtIODevice *vdev); } VirtioDeviceClass;
enum { RS_START, RS_INIT_FAILED, RS_INITED, RS_RESET };
struct VirtIOGPU {
    VirtIOGPUBase parent_obj;
    VirtIOGPUClass klass;
    VirtioDeviceClass vdev_class;
    bool reset_pending, processing_cmdq;
    QEMUBH *ctrl_bh, *cursor_bh;
    VirtQueue *ctrl_vq, *cursor_vq;
    QTAILQ_HEAD(, virtio_gpu_simple_resource) reslist;
    QTAILQ_HEAD(, virtio_gpu_ctrl_command) cmdq, fenceq;
    unsigned inflight;
    struct { unsigned requests, max_inflight; } stats;
    int renderer_state;
    unsigned scanout_texture[2];
    unsigned scanout_pending_texture[2];
    void *scanout_external_copy[2], *scanout_pending_copy[2];
    void *context_failure;
    bool scanout_gpu[2];
    QEMUTimer *fence_poll, *print_stats;
    QEMUBH *cmdq_resume_bh;
    void *capset_ids;
};
#define VIRGL_VERSION_MAJOR 1
#define VIRGL_RENDERER_CALLBACKS_VERSION 3
#define VIRGL_RENDERER_VENUS 1
#define VIRGL_RENDERER_RENDER_SERVER 2
#define QEMU_CLOCK_VIRTUAL 1
#define DEVICE(g) ((DeviceState *)(g))
#define g_clear_pointer(pp, destroy) do { \
    if (*(pp)) { destroy(*(pp)); *(pp) = NULL; } \
} while (0)
#define virtio_gpu_venus_enabled(conf) false
#define VIRTIO_GPU(g) ((VirtIOGPU *)(g))
#define VIRTIO_GPU_GL(g) ((VirtIOGPUGL *)(g))
#define VIRTIO_DEVICE(g) ((VirtIODevice *)(g))
#define VIRTIO_GPU_BASE(g) (&VIRTIO_GPU(g)->parent_obj)
#define VIRTIO_GPU_GET_CLASS(g) (&VIRTIO_GPU(g)->klass)
#define OBJECT(g) (g)
#define VIRTIO_GPU_FLAG_FENCE 1
#define LOG_GUEST_ERROR 1
#define g_free free
#define virtio_gpu_stats_enabled(conf) ((conf).stats)

static VirtIOGPU *active;
static pthread_t main_thread;
static unsigned gl_calls, ui_calls, destroyed, processed, base_resets;
static unsigned renderer_resets, renderer_inits, cursor_reads, cursor_updates;
static unsigned errors, notifications, resource_errors;
static unsigned timer_allocations, bh_allocations, live_timers, live_bhs;
static bool qemu_egl_angle_native_device;
static struct { unsigned version; } virtio_gpu_3d_cbs;
static bool reenter, init_fail, expect_gl_cleanup;
static bool recover_fail, copy_destroy_fail, resource_retained;
static unsigned recover_calls, copy_destroy_calls;
static unsigned renderer_cleanups;
static unsigned order, scanout_reset_order, first_destroy_order, base_reset_order;
static void virtio_gpu_reset_bh(VirtIOGPU *g);
static void virtio_gpu_ctrl_bh(void *opaque);
static void virtio_gpu_cursor_bh(void *opaque);
static void virtio_gpu_gl_update_cursor_data(VirtIOGPU *g,
    struct virtio_gpu_scanout *s, uint32_t resource_id);
static void virtio_gpu_disable_scanout(VirtIOGPU *g, int scanout_id);
void virtio_gpu_reset(VirtIODevice *vdev);
void virtio_gpu_process_cmdq(VirtIOGPU *g);
int virtio_gpu_virgl_init(VirtIOGPU *g);
static void virtio_gpu_gl_device_unrealize(DeviceState *qdev);

static bool qemu_in_vcpu_thread(void)
{
    return !pthread_equal(pthread_self(), main_thread);
}

static void main_only(void)
{
    CHECK(!qemu_in_vcpu_thread());
}

static void qemu_bh_schedule(QEMUBH *bh)
{
    bh->scheduled = true;
    bh->count++;
}

static bool virtio_queue_ready(VirtQueue *vq)
{
    main_only();
    return vq->ready;
}

static void *virtqueue_pop(VirtQueue *vq, size_t size)
{
    void *element = vq->element;
    main_only();
    CHECK(!active->reset_pending);
    vq->element = NULL;
    return element;
}

static void virtqueue_push(VirtQueue *vq, VirtQueueElement *elem, unsigned len)
{
    main_only();
    CHECK(vq->cursor);
}

static void virtio_notify(VirtIODevice *vdev, VirtQueue *vq)
{
    main_only();
    CHECK(!active->reset_pending);
    notifications++;
}

static size_t iov_to_buf(struct iovec *iov, unsigned count, size_t offset,
                         void *dest, size_t length)
{
    CHECK(count == 1 && offset == 0 && iov[0].iov_len == length);
    memcpy(dest, iov[0].iov_base, length);
    return length;
}

static void virtio_gpu_bswap_32(void *p, size_t size) { }
static void qemu_log_mask(int mask, const char *format, ...) { }
static void trace_virtio_gpu_cmd_suspended(unsigned type) { }
static void trace_virtio_gpu_inc_inflight_fences(unsigned inflight) { }
static const char *object_get_typename(void *obj) { return "virtio-gpu-gl"; }
static void error_append_hint(Error **error, const char *format, ...) { }
static void error_report_err(Error *error) { errors++; free(error); }

static struct virtio_gpu_simple_resource *virtio_gpu_find_resource(
    VirtIOGPU *g, uint32_t id)
{
    struct virtio_gpu_simple_resource *res;
    QTAILQ_FOREACH(res, &g->reslist, next) {
        if (res->resource_id == id) {
            return res;
        }
    }
    return NULL;
}

static void dpy_gfx_replace_surface(void *con, void *surface)
{
    main_only();
    CHECK(surface == NULL);
    ui_calls++;
    if (reenter) {
        virtio_gpu_process_cmdq(active);
    }
}

static void dpy_gl_scanout_disable(void *con)
{
    main_only();
    ui_calls++;
}

static void virgl_renderer_force_ctx_0(void)
{
    main_only();
    gl_calls++;
    if (!scanout_reset_order) {
        scanout_reset_order = ++order;
    }
}

static void glDeleteTextures(int count, unsigned *texture)
{
    main_only();
    CHECK(count == 1 && *texture != 0);
    gl_calls++;
}

static bool virtio_gpu_virgl_recover_context(VirtIOGPU *g)
{
    main_only();
    recover_calls++;
    if (recover_fail) {
        return false;
    }
    g->context_failure = NULL;
    g->parent_obj.renderer_blocked = 0;
    return true;
}

static bool virtio_gpu_neptune_copy_destroy(VirtIOGPU *g, void **copy)
{
    main_only();
    if (*copy) {
        copy_destroy_calls++;
        if (copy_destroy_fail) {
            g->context_failure = (void *)1;
            g->parent_obj.renderer_blocked = 1;
            return false;
        }
        *copy = NULL;
    }
    return true;
}

static void virtio_gpu_base_reset(VirtIOGPUBase *base)
{
    main_only();
    CHECK(QTAILQ_EMPTY(&active->reslist));
    CHECK(QTAILQ_EMPTY(&active->cmdq));
    CHECK(QTAILQ_EMPTY(&active->fenceq));
    CHECK(active->inflight == 0);
    base_reset_order = ++order;
    base_resets++;
}

static void resource_destroy(VirtIOGPU *g,
                             struct virtio_gpu_simple_resource *res,
                             Error **errp)
{
    main_only();
    if (resource_retained) {
        g->context_failure = (void *)1;
        g->parent_obj.renderer_blocked = 1;
        *errp = calloc(1, sizeof(Error));
        return;
    }
    if (!first_destroy_order) {
        first_destroy_order = ++order;
    }
    if (expect_gl_cleanup) {
        CHECK(scanout_reset_order != 0);
        CHECK(g->renderer_state == RS_RESET);
        CHECK(res->scanout_bitmask == 0);
        CHECK(g->scanout_texture[0] == 0 && g->scanout_texture[1] == 0);
    } else {
        for (unsigned i = 0; i < 2; i++) {
            if (res->scanout_bitmask & (1u << i)) {
                virtio_gpu_disable_scanout(g, i);
            }
        }
    }
    QTAILQ_REMOVE(&g->reslist, res, next);
    free(res);
    destroyed++;
    if (resource_errors) {
        *errp = calloc(1, sizeof(Error));
        resource_errors--;
    }
}

static void process_cmd(VirtIOGPU *g, struct virtio_gpu_ctrl_command *cmd)
{
    main_only();
    CHECK(!g->reset_pending);
    if (g->klass.reset) {
        CHECK(g->renderer_state == RS_INITED);
    }
    CHECK(base_resets != 0);
    cmd->finished = true;
    processed++;
}

static void virtio_gpu_virgl_reset(VirtIOGPU *g)
{
    main_only();
    CHECK(!g->reset_pending);
    CHECK(QTAILQ_EMPTY(&g->reslist));
    CHECK(g->renderer_state == RS_RESET);
    CHECK(g->scanout_texture[0] == 0 && g->scanout_texture[1] == 0);
    renderer_resets++;
}

static int virgl_renderer_init(VirtIOGPU *g, unsigned flags, void *cbs)
{
    main_only();
    CHECK(!g->reset_pending);
    renderer_inits++;
    return init_fail ? -1 : 0;
}

static void error_report(const char *format, ...) { }
static void virtio_gpu_fence_poll(void *g) { main_only(); }
static void virtio_gpu_print_stats(void *g) { main_only(); }
static void virtio_gpu_virgl_resume_cmdq_bh(void *g)
{
    main_only();
    virtio_gpu_process_cmdq(g);
}
static QEMUTimer *timer_new_ms(int clock, void (*cb)(void *), void *g)
{
    main_only();
    timer_allocations++;
    live_timers++;
    return calloc(1, sizeof(QEMUTimer));
}
static void timer_mod(QEMUTimer *timer, uint64_t deadline)
{
    main_only();
    CHECK(timer != NULL);
    timer->armed = true;
}
static uint64_t qemu_clock_get_ms(int clock) { return 0; }
static QEMUBH *virtio_bh_io_new_guarded(DeviceState *dev,
                                       void (*cb)(void *), void *opaque)
{
    main_only();
    bh_allocations++;
    live_bhs++;
    return calloc(1, sizeof(QEMUBH));
}
static void timer_free(QEMUTimer *timer)
{
    main_only();
    CHECK(timer != NULL && live_timers != 0);
    live_timers--;
    free(timer);
}
static void qemu_bh_delete(QEMUBH *bh)
{
    main_only();
    CHECK(bh != NULL && live_bhs != 0);
    live_bhs--;
    free(bh);
}
static void virtio_gpu_virgl_clear_fence_watches(VirtIOGPU *g) { main_only(); }
static void virgl_renderer_cleanup(void *opaque)
{
    struct virtio_gpu_simple_resource *res;
    main_only();
    QTAILQ_FOREACH(res, &active->reslist, next) {
        struct virtio_gpu_virgl_resource *vres =
            container_of(res, struct virtio_gpu_virgl_resource, base);
        CHECK(!vres->external_copy);
    }
    renderer_cleanups++;
}
static void g_array_unref(void *array) { }

static void virtio_gpu_virgl_fence_poll(VirtIOGPU *g)
{
    main_only();
    CHECK(g->renderer_state == RS_INITED && !g->reset_pending);
}

static uint32_t *virgl_renderer_get_cursor_data(uint32_t id,
                                               uint32_t *width,
                                               uint32_t *height)
{
    main_only();
    CHECK(!active->reset_pending && active->renderer_state == RS_INITED);
    uint32_t *data = malloc(4 * sizeof(*data));
    for (unsigned i = 0; i < 4; i++) {
        data[i] = 0x12340000 + i;
    }
    *width = *height = 2;
    cursor_reads++;
    return data;
}

static void update_cursor(VirtIOGPU *g,
                           struct virtio_gpu_update_cursor *info)
{
    main_only();
    CHECK(!g->reset_pending);
    CHECK(base_resets != 0);
    cursor_updates++;
    virtio_gpu_gl_update_cursor_data(g, &g->parent_obj.scanout[0],
                                      info->resource_id);
}

/* SOURCE_UNDER_TEST */

static QEMUBH ctrl_bh, cursor_bh;
static VirtQueue ctrl_vq, cursor_vq;
static struct virtio_gpu_update_cursor cursor_info = {100};
static struct iovec cursor_iov = {&cursor_info, sizeof(cursor_info)};
static struct { uint32_t width, height, data[4]; } cursor_image;

static void initialize(VirtIOGPU *g, const char *name, int state, bool gl)
{
    if (active) {
        virtio_gpu_gl_device_unrealize(DEVICE(active));
        CHECK(live_timers == 0 && live_bhs == 0);
    }
    scenario = name;
    memset(g, 0, sizeof(*g));
    active = g;
    gl_calls = ui_calls = destroyed = processed = base_resets = 0;
    renderer_resets = renderer_inits = cursor_reads = cursor_updates = 0;
    errors = notifications = resource_errors = 0;
    timer_allocations = bh_allocations = 0;
    order = scanout_reset_order = first_destroy_order = base_reset_order = 0;
    reenter = init_fail = false;
    recover_fail = copy_destroy_fail = resource_retained = false;
    recover_calls = copy_destroy_calls = 0;
    renderer_cleanups = 0;
    expect_gl_cleanup = gl && state == RS_INITED;
    ctrl_bh = (QEMUBH){0};
    cursor_bh = (QEMUBH){0};
    ctrl_vq = (VirtQueue){0};
    cursor_vq = (VirtQueue){.cursor = true};
    cursor_image.width = cursor_image.height = 2;
    memset(cursor_image.data, 0, sizeof(cursor_image.data));
    g->ctrl_bh = &ctrl_bh;
    g->cursor_bh = &cursor_bh;
    g->ctrl_vq = &ctrl_vq;
    g->cursor_vq = &cursor_vq;
    g->parent_obj.conf.max_outputs = 2;
    g->parent_obj.conf.stats = true;
    g->parent_obj.scanout[0].current_cursor = (void *)&cursor_image;
    g->renderer_state = state;
    g->klass.handle_ctrl = virtio_gpu_gl_handle_ctrl;
    g->klass.process_cmd = process_cmd;
    g->klass.resource_destroy = resource_destroy;
    install_reset_callbacks(&g->vdev_class, &g->klass);
    if (!gl) {
        g->klass.reset = NULL;
        g->vdev_class.reset = virtio_gpu_reset;
    }
    QTAILQ_INIT(&g->reslist);
    QTAILQ_INIT(&g->cmdq);
    QTAILQ_INIT(&g->fenceq);
    if (gl && state >= RS_INITED) {
        CHECK(virtio_gpu_virgl_init(g) == 0);
        renderer_inits = 0;
    }
}

static struct virtio_gpu_ctrl_command *new_command(void)
{
    return calloc(1, sizeof(struct virtio_gpu_ctrl_command));
}

static void seed_live_resources(VirtIOGPU *g)
{
    for (unsigned i = 0; i < 2; i++) {
        struct virtio_gpu_simple_resource *res =
            calloc(1, sizeof(struct virtio_gpu_virgl_resource));
        res->resource_id = 100 + i;
        res->scanout_bitmask = 1u << i;
        QTAILQ_INSERT_TAIL(&g->reslist, res, next);
        g->parent_obj.scanout[i].resource_id = 100 + i;
        g->parent_obj.scanout[i].width = 640;
        g->parent_obj.scanout[i].height = 480;
        g->parent_obj.scanout[i].con = &g->parent_obj.scanout[i];
        if (expect_gl_cleanup) {
            g->scanout_texture[i] = 500 + i;
            g->scanout_gpu[i] = true;
        }
    }
    struct virtio_gpu_ctrl_command *cmd = new_command();
    cmd->suspended = true;
    QTAILQ_INSERT_TAIL(&g->cmdq, cmd, next);
    cmd = new_command();
    QTAILQ_INSERT_TAIL(&g->fenceq, cmd, next);
    g->inflight = 1;
}

static void queue_cursor(void)
{
    VirtQueueElement *elem = calloc(1, sizeof(*elem));
    elem->out_sg = &cursor_iov;
    elem->out_num = 1;
    cursor_vq.ready = true;
    cursor_vq.element = elem;
}

struct ThreadRequest { unsigned resets; bool kick_cursor, kick_ctrl; };
static void *vcpu_run(void *opaque)
{
    struct ThreadRequest *request = opaque;
    CHECK(qemu_in_vcpu_thread());
    for (unsigned i = 0; i < request->resets; i++) {
        active->vdev_class.reset(VIRTIO_DEVICE(active));
    }
    if (request->kick_cursor) {
        virtio_gpu_handle_cursor_cb(VIRTIO_DEVICE(active), active->cursor_vq);
    }
    if (request->kick_ctrl) {
        virtio_gpu_handle_ctrl_cb(VIRTIO_DEVICE(active), active->ctrl_vq);
    }
    CHECK(gl_calls == 0 && ui_calls == 0 && destroyed == 0);
    return NULL;
}

static void run_vcpu(unsigned resets, bool kick_cursor, bool kick_ctrl)
{
    pthread_t thread;
    struct ThreadRequest request = {resets, kick_cursor, kick_ctrl};
    CHECK(pthread_create(&thread, NULL, vcpu_run, &request) == 0);
    CHECK(pthread_join(thread, NULL) == 0);
}

static void pump_ctrl(void)
{
    CHECK(ctrl_bh.scheduled);
    ctrl_bh.scheduled = false;
    virtio_gpu_ctrl_bh(active);
}

static void pump_cursor(void)
{
    CHECK(cursor_bh.scheduled);
    cursor_bh.scheduled = false;
    virtio_gpu_cursor_bh(active);
}

static void verify_clean(VirtIOGPU *g, unsigned count)
{
    CHECK(!g->reset_pending);
    CHECK(base_resets == count);
    CHECK(QTAILQ_EMPTY(&g->reslist));
    CHECK(QTAILQ_EMPTY(&g->cmdq));
    CHECK(QTAILQ_EMPTY(&g->fenceq));
    CHECK(g->inflight == 0 && !g->processing_cmdq);
    if (destroyed) {
        CHECK(first_destroy_order < base_reset_order);
    }
    for (unsigned i = 0; i < 2; i++) {
        CHECK(g->scanout_texture[i] == 0 && !g->scanout_gpu[i]);
        CHECK(g->parent_obj.scanout[i].resource_id == 0);
    }
}

int main(void)
{
    VirtIOGPU gpu;
    main_thread = pthread_self();

    initialize(&gpu, "vCPU reset remains deferred; control drains once",
               RS_INITED, true);
    seed_live_resources(&gpu);
    reenter = true;
    resource_errors = 1;
    run_vcpu(2, false, false);
    CHECK(gpu.reset_pending && ctrl_bh.count == 2);
    CHECK(gpu.renderer_state == RS_INITED);
    /* A flush/finalizer callback arriving before the reset BH cannot run old work. */
    virtio_gpu_process_cmdq(&gpu);
    CHECK(processed == 0);
    pump_ctrl();
    verify_clean(&gpu, 1);
    CHECK(destroyed == 2 && gl_calls == 3 && errors == 1);
    CHECK(gpu.renderer_state == RS_RESET && renderer_resets == 0);
    CHECK(scanout_reset_order < first_destroy_order);
    virtio_gpu_ctrl_bh(&gpu);
    CHECK(gl_calls == 3 && base_resets == 1);
    /* A second reset before renderer restart must not re-delete GL state. */
    gpu.vdev_class.reset(VIRTIO_DEVICE(&gpu));
    verify_clean(&gpu, 2);
    CHECK(gl_calls == 3 && gpu.renderer_state == RS_RESET);
    ctrl_vq.ready = true;
    ctrl_vq.element = new_command();
    virtio_gpu_ctrl_bh(&gpu);
    CHECK(renderer_resets == 1 && renderer_inits == 1 && processed == 1);
    CHECK(gpu.renderer_state == RS_INITED);
    CHECK(timer_allocations == 2 && bh_allocations == 1);
    CHECK(live_timers == 2 && live_bhs == 1);
    queue_cursor();
    virtio_gpu_cursor_bh(&gpu);
    CHECK(cursor_reads == 1 && notifications == 1);
    CHECK(cursor_image.data[0] == 0x12340000 && cursor_image.data[3] == 0x12340003);

    initialize(&gpu, "cursor BH wins reset race", RS_INITED, true);
    seed_live_resources(&gpu);
    queue_cursor();
    run_vcpu(1, true, false);
    pump_cursor();
    verify_clean(&gpu, 1);
    CHECK(cursor_updates == 1 && cursor_reads == 0);
    CHECK(gpu.renderer_state == RS_RESET);
    pump_ctrl();
    CHECK(base_resets == 1 && gl_calls == 3);

    initialize(&gpu, "ready control queue restarts only after teardown",
               RS_INITED, true);
    seed_live_resources(&gpu);
    ctrl_vq.ready = true;
    ctrl_vq.element = new_command();
    run_vcpu(1, false, true);
    pump_ctrl();
    verify_clean(&gpu, 1);
    CHECK(renderer_resets == 1 && renderer_inits == 1 && processed == 1);
    CHECK(gpu.renderer_state == RS_INITED);

    initialize(&gpu, "owned textures survive resource unref until reset",
               RS_INITED, true);
    gpu.scanout_texture[0] = 501;
    gpu.scanout_gpu[0] = true;
    run_vcpu(1, false, false);
    pump_ctrl();
    verify_clean(&gpu, 1);
    CHECK(gl_calls == 2 && destroyed == 0 && gpu.renderer_state == RS_RESET);

    initialize(&gpu, "main-thread reset is synchronous", RS_INITED, true);
    seed_live_resources(&gpu);
    gpu.vdev_class.reset(VIRTIO_DEVICE(&gpu));
    verify_clean(&gpu, 1);
    CHECK(!ctrl_bh.scheduled && gpu.renderer_state == RS_RESET);

    initialize(&gpu, "software GPU needs no optional reset hook", RS_START, false);
    seed_live_resources(&gpu);
    run_vcpu(1, false, false);
    pump_ctrl();
    verify_clean(&gpu, 1);
    CHECK(gl_calls == 0 && destroyed == 2 && renderer_inits == 0);

    for (int state = RS_START; state <= RS_RESET; state++) {
        if (state == RS_INITED) {
            continue;
        }
        initialize(&gpu, "uninitialized and already-pending renderer reset",
                   state, true);
        run_vcpu(2, false, false);
        pump_ctrl();
        verify_clean(&gpu, 1);
        CHECK(gl_calls == 0 && gpu.renderer_state == state);
        ctrl_vq.ready = true;
        virtio_gpu_ctrl_bh(&gpu);
        if (state == RS_INIT_FAILED) {
            CHECK(renderer_inits == 0 && gpu.renderer_state == RS_INIT_FAILED);
        } else {
            CHECK(renderer_inits == 1 && gpu.renderer_state == RS_INITED);
            CHECK(renderer_resets == (unsigned)(state == RS_RESET));
        }
    }

    initialize(&gpu, "renderer restart failure remains terminal",
               RS_INITED, true);
    seed_live_resources(&gpu);
    init_fail = true;
    ctrl_vq.ready = true;
    run_vcpu(1, false, false);
    pump_ctrl();
    verify_clean(&gpu, 1);
    CHECK(gpu.renderer_state == RS_INIT_FAILED);
    CHECK(renderer_resets == 1 && renderer_inits == 1);
    virtio_gpu_ctrl_bh(&gpu);
    CHECK(renderer_inits == 1 && processed == 0);
    initialize(&gpu, "statistics disabled across repeated reset",
               RS_START, true);
    gpu.parent_obj.conf.stats = false;
    ctrl_vq.ready = true;
    virtio_gpu_ctrl_bh(&gpu);
    CHECK(live_timers == 1 && live_bhs == 1 && gpu.print_stats == NULL);
    QEMUTimer *original_fence = gpu.fence_poll;
    QEMUBH *original_resume = gpu.cmdq_resume_bh;
    for (unsigned i = 0; i < 4; i++) {
        gpu.vdev_class.reset(VIRTIO_DEVICE(&gpu));
        virtio_gpu_ctrl_bh(&gpu);
        CHECK(gpu.fence_poll == original_fence);
        CHECK(gpu.cmdq_resume_bh == original_resume);
        CHECK(timer_allocations == 1 && bh_allocations == 1);
        CHECK(gpu.print_stats == NULL);
    }

    initialize(&gpu, "failed recovery completes reset without GL cleanup",
               RS_INITED, true);
    seed_live_resources(&gpu);
    gpu.context_failure = (void *)1;
    gpu.parent_obj.renderer_blocked = 1;
    recover_fail = true;
    run_vcpu(1, false, false);
    pump_ctrl();
    CHECK(!gpu.reset_pending && destroyed == 0 && base_resets == 0);
    CHECK(gl_calls == 0 && ui_calls == 0 && recover_calls == 1);
    CHECK(!QTAILQ_EMPTY(&gpu.reslist) && QTAILQ_EMPTY(&gpu.cmdq));
    CHECK(QTAILQ_EMPTY(&gpu.fenceq) && gpu.inflight == 0);
    virtio_gpu_ctrl_bh(&gpu);
    CHECK(recover_calls == 1 && gpu.context_failure);
    recover_fail = false;
    gpu.vdev_class.reset(VIRTIO_DEVICE(&gpu));
    verify_clean(&gpu, 1);
    CHECK(recover_calls == 2);

    initialize(&gpu, "failed helper destroy retains target until explicit reset",
               RS_INITED, true);
    seed_live_resources(&gpu);
    gpu.scanout_external_copy[0] = (void *)1;
    gpu.scanout_pending_texture[0] = 901;
    gpu.scanout_pending_copy[0] = (void *)2;
    copy_destroy_fail = true;
    gpu.vdev_class.reset(VIRTIO_DEVICE(&gpu));
    CHECK(!gpu.reset_pending && destroyed == 0 && base_resets == 0);
    CHECK(gpu.scanout_texture[0] == 500 && gpu.scanout_pending_texture[0] == 901);
    CHECK(copy_destroy_calls == 1 && gpu.context_failure);
    copy_destroy_fail = false;
    gpu.vdev_class.reset(VIRTIO_DEVICE(&gpu));
    verify_clean(&gpu, 1);
    CHECK(!gpu.scanout_external_copy[0] && !gpu.scanout_pending_copy[0]);
    CHECK(gpu.scanout_pending_texture[0] == 0 && copy_destroy_calls == 3);

    initialize(&gpu, "retained resource halts reset and clears stale queues",
               RS_INITED, true);
    seed_live_resources(&gpu);
    resource_retained = true;
    gpu.vdev_class.reset(VIRTIO_DEVICE(&gpu));
    CHECK(!gpu.reset_pending && destroyed == 0 && base_resets == 0);
    CHECK(gpu.context_failure && !QTAILQ_EMPTY(&gpu.reslist));
    CHECK(QTAILQ_EMPTY(&gpu.cmdq) && QTAILQ_EMPTY(&gpu.fenceq));
    CHECK(errors == 1 && gpu.inflight == 0);
    resource_retained = false;
    gpu.vdev_class.reset(VIRTIO_DEVICE(&gpu));
    verify_clean(&gpu, 1);

    initialize(&gpu, "unrealize retires ordinary aliases before renderer",
               RS_INITED, true);
    seed_live_resources(&gpu);
    struct virtio_gpu_virgl_resource *vres = container_of(
        QTAILQ_FIRST(&gpu.reslist), struct virtio_gpu_virgl_resource, base);
    vres->external_copy = (void *)1;
    virtio_gpu_gl_device_unrealize(DEVICE(&gpu));
    CHECK(renderer_cleanups == 1 && copy_destroy_calls == 1);
    CHECK(!vres->external_copy && !QTAILQ_EMPTY(&gpu.reslist));
    expect_gl_cleanup = false;
    gpu.vdev_class.reset(VIRTIO_DEVICE(&gpu));
    verify_clean(&gpu, 1);

    initialize(&gpu, "unrealize failure retains aliases and skips renderer",
               RS_INITED, true);
    seed_live_resources(&gpu);
    vres = container_of(QTAILQ_FIRST(&gpu.reslist),
                       struct virtio_gpu_virgl_resource, base);
    vres->external_copy = (void *)1;
    copy_destroy_fail = true;
    virtio_gpu_gl_device_unrealize(DEVICE(&gpu));
    CHECK(renderer_cleanups == 0 && copy_destroy_calls == 1);
    CHECK(vres->external_copy && gpu.context_failure);
    CHECK(live_timers == 0 && live_bhs == 0 && destroyed == 0);
    copy_destroy_fail = false;
    virtio_gpu_gl_device_unrealize(DEVICE(&gpu));
    CHECK(renderer_cleanups == 1 && !vres->external_copy);
    expect_gl_cleanup = false;
    gpu.vdev_class.reset(VIRTIO_DEVICE(&gpu));
    verify_clean(&gpu, 1);

    initialize(&gpu, "first initialization failure owns no timer or BH",
               RS_START, true);
    init_fail = true;
    ctrl_vq.ready = true;
    virtio_gpu_ctrl_bh(&gpu);
    CHECK(gpu.renderer_state == RS_INIT_FAILED);
    CHECK(live_timers == 0 && live_bhs == 0);
    virtio_gpu_gl_device_unrealize(DEVICE(&gpu));
    CHECK(live_timers == 0 && live_bhs == 0);
    active = NULL;
    printf("PASS: %u checks; main-thread GL/UI cleanup, resource order, "
           "control/cursor races, repeated reset, renderer restart\n", checks);
    return 0;
}
