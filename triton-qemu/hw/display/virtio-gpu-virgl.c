/*
 * Virtio GPU Device
 *
 * Copyright Red Hat, Inc. 2013-2014
 *
 * Authors:
 *     Dave Airlie <airlied@redhat.com>
 *     Gerd Hoffmann <kraxel@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/iov.h"
#include "qemu/main-loop.h"
#include "system/runstate.h"
#include "trace.h"
#include "hw/virtio/virtio.h"
#include "hw/virtio/virtio-gpu.h"
#include "hw/virtio/virtio-gpu-bswap.h"
#include "hw/virtio/virtio-gpu-pixman.h"

#include "ui/egl-helpers.h"
#include "ui/triton-trace.h"

#include <sys/mman.h>
#include <sys/stat.h>
#ifdef CONFIG_LINUX
#include "standard-headers/drm/drm_fourcc.h"
#include "ui/egl-external-copy.h"
#endif

#define VIRGL_RENDERER_UNSTABLE_APIS
#include <virglrenderer.h>

#define NATIVE_HANDLE_SUPPORT_VERSION (1)

static uint32_t virgl_status_to_virtio_error(int status)
{
    int error = status < 0 ? -status : status;

    if (error == ENOMEM) {
        return VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY;
    }
    if (error == EINVAL || error == ENOENT) {
        return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
    }
    return VIRTIO_GPU_RESP_ERR_UNSPEC;
}

struct virtio_gpu_virgl_resource {
    struct virtio_gpu_simple_resource base;
    MemoryRegion *mr;
    bool is_blob;
    uint32_t blob_mem;
    uint32_t blob_flags;
    uint32_t blob_ctx_id;
    struct VirtIOGPUExternalCopy *external_copy;
};

#ifdef CONFIG_LINUX
struct VirtIOGPUExternalCopy {
    QemuEGLExternalCopy *helper;
    GLuint texture;
    uint32_t width, height;
    EGLContext context;
};

struct VirtIOGPUExternalContext {
    EGLenum api;
    EGLDisplay display;
    EGLContext context;
    EGLSurface draw, read;
};

static struct VirtIOGPUExternalContext virtio_gpu_external_context(void)
{
    struct VirtIOGPUExternalContext saved = {
        eglQueryAPI(), eglGetCurrentDisplay(), eglGetCurrentContext(),
        eglGetCurrentSurface(EGL_DRAW), eglGetCurrentSurface(EGL_READ),
    };
    return saved;
}

static void virtio_gpu_external_stop(
    VirtIOGPU *g, const struct VirtIOGPUExternalContext *saved,
    const QemuEGLExternalCopyError *error)
{
    VirtIOGPUGL *gl = VIRTIO_GPU_GL(g);

    assert(qemu_in_main_thread());
    if (!gl->context_failure) {
        gl->context_failure = g_memdup2(saved, sizeof(*saved));
        g->parent_obj.renderer_blocked++;
        error_report("Neptune external copy stopped the renderer: %s "
                     "(EGL=0x%x GL=0x%x)", error->operation,
                     error->egl_error, error->gl_error);
    }
    if (gl->fence_poll) {
        timer_del(gl->fence_poll);
    }
    if (gl->cmdq_resume_bh) {
        qemu_bh_cancel(gl->cmdq_resume_bh);
    }
    virtio_gpu_virgl_clear_fence_watches(g);
    qemu_system_vmstop_request(RUN_STATE_INTERNAL_ERROR);
}
#endif

bool virtio_gpu_virgl_recover_context(VirtIOGPU *g)
{
#ifdef CONFIG_LINUX
    VirtIOGPUGL *gl = VIRTIO_GPU_GL(g);
    struct VirtIOGPUExternalContext *saved = gl->context_failure;

    assert(qemu_in_main_thread());
    if (saved) {
        if (!eglBindAPI(saved->api) ||
            !eglMakeCurrent(saved->display, saved->draw, saved->read,
                             saved->context)) {
            error_report("Neptune EGL context recovery failed (EGL=0x%x)",
                         eglGetError());
            qemu_system_vmstop_request(RUN_STATE_INTERNAL_ERROR);
            return false;
        }
        g_clear_pointer(&gl->context_failure, g_free);
        assert(g->parent_obj.renderer_blocked);
        g->parent_obj.renderer_blocked--;
    }
#endif
    return true;
}

static bool virtio_gpu_neptune_copy_destroy(
    VirtIOGPU *g, struct VirtIOGPUExternalCopy **slot)
{
#ifdef CONFIG_LINUX
    struct VirtIOGPUExternalCopy *copy = *slot;
    struct VirtIOGPUExternalContext saved;
    QemuEGLExternalCopyError error;
    bool ok;

    if (!copy) {
        return true;
    }
    if (VIRTIO_GPU_GL(g)->context_failure) {
        return false;
    }
    saved = virtio_gpu_external_context();
    ok = qemu_egl_external_copy_destroy(&copy->helper, &error);
    if (!copy->helper) {
        g_clear_pointer(slot, g_free);
    }
    if (!ok) {
        /* Retain the destination until a controlled reset can clean it up. */
        virtio_gpu_external_stop(g, &saved, &error);
        return false;
    }
#endif
    return true;
}

/* Triton private extension: copy one completed SHM rectangle into the
 * retained, ordinary primary. It does not change scanout. */
#define VIRTIO_GPU_CMD_TRITON_PRESENT_BLT 0x0500
struct virtio_gpu_triton_present_blt {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t source_resource_id, destination_resource_id;
    uint32_t source_width, source_height, source_format, source_stride;
    uint32_t source_offset, reserved;
    struct virtio_gpu_rect source_rect;
    uint32_t destination_x, destination_y;
};
QEMU_BUILD_BUG_ON(sizeof(struct virtio_gpu_triton_present_blt) != 80);

static bool
virtio_gpu_virgl_legacy_neptune_context_fence(
    const struct virtio_gpu_ctrl_command *cmd)
{
#if VIRGL_VERSION_MAJOR >= 1
    uint32_t capset_id = 0;

    if (!(cmd->cmd_hdr.flags & VIRTIO_GPU_FLAG_FENCE) ||
        (cmd->cmd_hdr.flags & VIRTIO_GPU_FLAG_INFO_RING_IDX) ||
        cmd->cmd_hdr.ctx_id == 0) {
        return false;
    }

    return virgl_renderer_context_get_capset_id(cmd->cmd_hdr.ctx_id,
                                                &capset_id) == 0 &&
           capset_id == VIRTIO_GPU_CAPSET_NEPTUNE;
#else
    return false;
#endif
}

static struct virtio_gpu_virgl_resource *
virtio_gpu_virgl_find_resource(VirtIOGPU *g, uint32_t resource_id)
{
    struct virtio_gpu_simple_resource *res;

    res = virtio_gpu_find_resource(g, resource_id);
    if (!res) {
        return NULL;
    }

    return container_of(res, struct virtio_gpu_virgl_resource, base);
}

static void
virtio_gpu_virgl_disable_resource_scanouts(VirtIOGPU *g,
                                           struct virtio_gpu_simple_resource *res)
{
    for (uint32_t i = 0; i < g->parent_obj.conf.max_outputs; ++i) {
        if (res->scanout_bitmask & (1u << i)) {
            virtio_gpu_disable_scanout(g, i);
        }
    }
    /*
     * Display callbacks can release the current EGL context. Restore the
     * renderer context before destroying its GL objects and EGL images.
     */
    virgl_renderer_force_ctx_0();
}

#if VIRGL_RENDERER_CALLBACKS_VERSION >= 4
static void *
virgl_get_egl_display(G_GNUC_UNUSED void *cookie)
{
    return qemu_egl_display;
}
#endif

#if VIRGL_VERSION_MAJOR >= 1
struct virtio_gpu_virgl_hostmem_region {
    Object parent_obj;
    MemoryRegion mr;
    struct VirtIOGPU *g;
    bool finish_unmapping;
};

#define TYPE_VIRTIO_GPU_VIRGL_HOSTMEM_REGION "virtio-gpu-virgl-hostmem-region"

OBJECT_DECLARE_SIMPLE_TYPE(virtio_gpu_virgl_hostmem_region,
                           VIRTIO_GPU_VIRGL_HOSTMEM_REGION)

static struct virtio_gpu_virgl_hostmem_region *
to_hostmem_region(MemoryRegion *mr)
{
    return container_of(mr, struct virtio_gpu_virgl_hostmem_region, mr);
}

static void virtio_gpu_virgl_resume_cmdq_bh(void *opaque)
{
    VirtIOGPU *g = opaque;

    virtio_gpu_process_cmdq(g);
}

/*
 * MR could outlive the resource if MR's reference is held outside of
 * virtio-gpu. In order to prevent unmapping resource while MR is alive,
 * and thus, making the data pointer invalid, we will block virtio-gpu
 * command processing until MR is fully unreferenced and freed.
 */
static void virtio_gpu_virgl_hostmem_region_finalize(Object *obj)
{
    struct virtio_gpu_virgl_hostmem_region *vmr = VIRTIO_GPU_VIRGL_HOSTMEM_REGION(obj);
    VirtIOGPUBase *b;
    VirtIOGPUGL *gl;

    if (!vmr->g) {
        return;
    }

    vmr->finish_unmapping = true;

    b = VIRTIO_GPU_BASE(vmr->g);
    b->renderer_blocked--;

    /*
     * memory_region_unref() is executed from RCU thread context, while
     * virglrenderer works only on the main-loop thread that's holding GL
     * context.
     */
    gl = VIRTIO_GPU_GL(vmr->g);
    qemu_bh_schedule(gl->cmdq_resume_bh);
}

static const TypeInfo virtio_gpu_virgl_hostmem_region_info = {
    .parent = TYPE_OBJECT,
    .name = TYPE_VIRTIO_GPU_VIRGL_HOSTMEM_REGION,
    .instance_size = sizeof(struct virtio_gpu_virgl_hostmem_region),
    .instance_finalize = virtio_gpu_virgl_hostmem_region_finalize
};

static void virtio_gpu_virgl_types(void)
{
    type_register_static(&virtio_gpu_virgl_hostmem_region_info);
}

type_init(virtio_gpu_virgl_types)

static int
virtio_gpu_virgl_map_resource_blob(VirtIOGPU *g,
                                   struct virtio_gpu_virgl_resource *res,
                                   uint64_t offset)
{
    g_autofree char *name = NULL;
    struct virtio_gpu_virgl_hostmem_region *vmr;
    VirtIOGPUBase *b = VIRTIO_GPU_BASE(g);
    MemoryRegion *mr;
    uint64_t size;
    void *data;
    int ret;

    if (!virtio_gpu_hostmem_enabled(b->conf)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: hostmem disabled\n", __func__);
        return -EOPNOTSUPP;
    }

    ret = virgl_renderer_resource_map(res->base.resource_id, &data, &size);
    if (ret) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: failed to map virgl resource: %s\n",
                      __func__, strerror(-ret));
        return ret;
    }

    vmr = g_new0(struct virtio_gpu_virgl_hostmem_region, 1);
    name = g_strdup_printf("blob[%" PRIu32 "]", res->base.resource_id);
    object_initialize_child(OBJECT(g), name, vmr,
                            TYPE_VIRTIO_GPU_VIRGL_HOSTMEM_REGION);
    vmr->g = g;

    mr = &vmr->mr;
    memory_region_init_ram_ptr(mr, OBJECT(vmr), "mr", size, data);
    memory_region_add_subregion(&b->hostmem, offset, mr);
    memory_region_set_enabled(mr, true);

    res->mr = mr;

    return 0;
}

static int
virtio_gpu_virgl_unmap_resource_blob(VirtIOGPU *g,
                                     struct virtio_gpu_virgl_resource *res,
                                     bool *cmd_suspended)
{
    struct virtio_gpu_virgl_hostmem_region *vmr;
    VirtIOGPUBase *b = VIRTIO_GPU_BASE(g);
    MemoryRegion *mr = res->mr;
    int ret;

    if (!mr) {
        return 0;
    }

    vmr = to_hostmem_region(res->mr);

    /*
     * Perform async unmapping in 3 steps:
     *
     * 1. Begin async unmapping with memory_region_del_subregion()
     *    and suspend/block cmd processing.
     * 2. Wait for res->mr to be freed and cmd processing resumed
     *    asynchronously by virtio_gpu_virgl_hostmem_region_finalize().
     * 3. Finish the unmapping with final virgl_renderer_resource_unmap().
     */
    if (vmr->finish_unmapping) {
        res->mr = NULL;
        g_free(vmr);

        ret = virgl_renderer_resource_unmap(res->base.resource_id);
        if (ret) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: failed to unmap virgl resource: %s\n",
                          __func__, strerror(-ret));
            return ret;
        }
    } else {
        *cmd_suspended = true;

        /* render will be unblocked once MR is freed */
        b->renderer_blocked++;

        /* memory region owns self res->mr object and frees it by itself */
        memory_region_set_enabled(mr, false);
        memory_region_del_subregion(&b->hostmem, mr);
        object_unparent(OBJECT(vmr));
    }

    return 0;
}

static void
virtio_gpu_virgl_destroy_hostmem_region(VirtIOGPU *g,
                                        struct virtio_gpu_virgl_resource *res)
{
    struct virtio_gpu_virgl_hostmem_region *vmr = to_hostmem_region(res->mr);
    VirtIOGPUBase *b = VIRTIO_GPU_BASE(g);

    /*
     * vmr is not QOM-owned, so object_finalize() does not free it; the unmap
     * step 3 normally does. Reset can run here at any stage of an in-flight
     * unmap, where step 3 may not run. This and the finalizer both hold the
     * BQL, so free vmr or hand it to object_finalize() per the stage.
     */
    if (vmr->finish_unmapping) {
        /* Finalizer ran and balanced renderer_blocked; just free vmr. */
        res->mr = NULL;
        g_free(vmr);
        virgl_renderer_resource_unmap(res->base.resource_id);
        return;
    }

    if (res->mr->container != &b->hostmem) {
        /* Async unmap detached the subregion; let the finalizer free vmr. */
        OBJECT(vmr)->free = g_free;
        res->mr = NULL;
        return;
    }

    /* No unmap in flight; tear down here and neutralize the finalizer. */
    OBJECT(vmr)->free = g_free;
    vmr->g = NULL;
    memory_region_set_enabled(res->mr, false);
    memory_region_del_subregion(&b->hostmem, res->mr);
    object_unparent(OBJECT(vmr));
    res->mr = NULL;

    virgl_renderer_resource_unmap(res->base.resource_id);
}
#endif

void virtio_gpu_virgl_resource_destroy(VirtIOGPU *g,
                                       struct virtio_gpu_simple_resource *res,
                                       Error **errp)
{
    struct virtio_gpu_virgl_resource *vres =
        container_of(res, struct virtio_gpu_virgl_resource, base);
    struct iovec *res_iovs = NULL;
    int num_iovs = 0;

    if (VIRTIO_GPU_GL(g)->context_failure ||
        !virtio_gpu_neptune_copy_destroy(g, &vres->external_copy)) {
        error_setg(errp, "Neptune resource retained after EGL context failure");
        return;
    }

#if VIRGL_VERSION_MAJOR >= 1
    if (vres->mr) {
        virtio_gpu_virgl_destroy_hostmem_region(g, vres);
    }
#endif

    virtio_gpu_virgl_disable_resource_scanouts(g, res);

    virgl_renderer_resource_detach_iov(res->resource_id, &res_iovs, &num_iovs);
    if (res_iovs && num_iovs) {
        virtio_gpu_cleanup_mapping_iov(g, res_iovs, num_iovs);
    }
    /* The detached iov is the same allocation virgl took at attach time;
     * clear res->iov so the base destroy does not free it again. */
    res->iov = NULL;
    res->iov_cnt = 0;

    virgl_renderer_resource_unref(res->resource_id);

    virtio_gpu_resource_destroy(g, res, errp);
}

static void virgl_cmd_create_resource_2d(VirtIOGPU *g,
                                         struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_resource_create_2d c2d;
    struct virgl_renderer_resource_create_args args = { 0 };
    struct virtio_gpu_virgl_resource *res;
    int ret;

    VIRTIO_GPU_FILL_CMD(c2d);
    trace_virtio_gpu_cmd_res_create_2d(c2d.resource_id, c2d.format,
                                       c2d.width, c2d.height);

    if (c2d.resource_id == 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: resource id 0 is not allowed\n",
                      __func__);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }

    res = virtio_gpu_virgl_find_resource(g, c2d.resource_id);
    if (res) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: resource already exists %d\n",
                      __func__, c2d.resource_id);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }

    res = g_new0(struct virtio_gpu_virgl_resource, 1);
    res->base.width = c2d.width;
    res->base.height = c2d.height;
    res->base.format = c2d.format;
    res->base.resource_id = c2d.resource_id;
    res->base.dmabuf_fd = -1;
    args.handle = c2d.resource_id;
    args.target = 2;
    args.format = c2d.format;
    args.bind = (1 << 1);
    args.width = c2d.width;
    args.height = c2d.height;
    args.depth = 1;
    args.array_size = 1;
    args.last_level = 0;
    args.nr_samples = 0;
    args.flags = VIRTIO_GPU_RESOURCE_FLAG_Y_0_TOP;
    ret = virgl_renderer_resource_create(&args, NULL, 0);
    if (ret) {
        cmd->error = virgl_status_to_virtio_error(ret);
        g_free(res);
        return;
    }
    QTAILQ_INSERT_HEAD(&g->reslist, &res->base, next);
}

static void virgl_cmd_create_resource_3d(VirtIOGPU *g,
                                         struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_resource_create_3d c3d;
    struct virgl_renderer_resource_create_args args = { 0 };
    struct virtio_gpu_virgl_resource *res;
    int ret;

    VIRTIO_GPU_FILL_CMD(c3d);
    trace_virtio_gpu_cmd_res_create_3d(c3d.resource_id, c3d.format,
                                       c3d.width, c3d.height, c3d.depth);

    if (c3d.resource_id == 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: resource id 0 is not allowed\n",
                      __func__);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }

    res = virtio_gpu_virgl_find_resource(g, c3d.resource_id);
    if (res) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: resource already exists %d\n",
                      __func__, c3d.resource_id);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }

    res = g_new0(struct virtio_gpu_virgl_resource, 1);
    res->base.width = c3d.width;
    res->base.height = c3d.height;
    res->base.format = c3d.format;
    res->base.resource_id = c3d.resource_id;
    res->base.dmabuf_fd = -1;
    args.handle = c3d.resource_id;
    args.target = c3d.target;
    args.format = c3d.format;
    args.bind = c3d.bind;
    args.width = c3d.width;
    args.height = c3d.height;
    args.depth = c3d.depth;
    args.array_size = c3d.array_size;
    args.last_level = c3d.last_level;
    args.nr_samples = c3d.nr_samples;
    args.flags = c3d.flags;
    ret = virgl_renderer_resource_create(&args, NULL, 0);
    if (ret) {
        cmd->error = virgl_status_to_virtio_error(ret);
        g_free(res);
        return;
    }
    QTAILQ_INSERT_HEAD(&g->reslist, &res->base, next);
}

static void virgl_cmd_resource_unref(VirtIOGPU *g,
                                     struct virtio_gpu_ctrl_command *cmd,
                                     bool *cmd_suspended)
{
    struct virtio_gpu_resource_unref unref;
    struct virtio_gpu_virgl_resource *res;
    struct iovec *res_iovs = NULL;
    int num_iovs = 0;

    VIRTIO_GPU_FILL_CMD(unref);
    trace_virtio_gpu_cmd_res_unref(unref.resource_id);

    res = virtio_gpu_virgl_find_resource(g, unref.resource_id);
    if (!res) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: resource does not exist %d\n",
                      __func__, unref.resource_id);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }

    if (!virtio_gpu_neptune_copy_destroy(g, &res->external_copy)) {
        cmd->error = VIRTIO_GPU_RESP_ERR_UNSPEC;
        return;
    }

#if VIRGL_VERSION_MAJOR >= 1
    if (virtio_gpu_virgl_unmap_resource_blob(g, res, cmd_suspended)) {
        cmd->error = VIRTIO_GPU_RESP_ERR_UNSPEC;
        return;
    }
    if (*cmd_suspended) {
        return;
    }
#endif

    virtio_gpu_virgl_disable_resource_scanouts(g, &res->base);

    virgl_renderer_resource_detach_iov(unref.resource_id,
                                       &res_iovs,
                                       &num_iovs);
    if (res_iovs != NULL && num_iovs != 0) {
        virtio_gpu_cleanup_mapping_iov(g, res_iovs, num_iovs);
    }
    res->base.iov = NULL;
    res->base.iov_cnt = 0;
    virgl_renderer_resource_unref(unref.resource_id);
    virtio_gpu_resource_destroy(g, &res->base, NULL);
}

#if VIRGL_VERSION_MAJOR >= 1
typedef struct VirtIOGPUContextFenceWatch {
    VirtIOGPU *gpu;
    int fd;
    unsigned references;
} VirtIOGPUContextFenceWatch;

static void virtio_gpu_context_fence_ready(void *opaque)
{
    VirtIOGPUContextFenceWatch *watch = opaque;

    /*
     * Main-loop callback, like the fallback timer. Renderer polling drains
     * its eventfd and retires completed fences before resuming commands.
     */
    trace_virtio_gpu_neptune_fence_wake(watch->fd);
    virtio_gpu_virgl_fence_poll(watch->gpu);
}

static void virtio_gpu_context_fence_watch_free(void *opaque)
{
    VirtIOGPUContextFenceWatch *watch = opaque;

    if (--watch->references) {
        return;
    }
    qemu_set_fd_handler(watch->fd, NULL, NULL, NULL);
    /* The renderer owns the descriptor; unregister before it closes it. */
    g_free(watch);
}
#endif

void virtio_gpu_virgl_clear_fence_watches(VirtIOGPU *g)
{
    g_clear_pointer(&VIRTIO_GPU_GL(g)->context_fence_watches,
                    g_hash_table_unref);
}

static void virtio_gpu_watch_context_fences(VirtIOGPU *g, uint32_t ctx_id)
{
#if VIRGL_VERSION_MAJOR >= 1
    VirtIOGPUGL *gl = VIRTIO_GPU_GL(g);
    VirtIOGPUContextFenceWatch *watch;
    GHashTableIter iter;
    gpointer value;
    int fd;

    if (!virtio_gpu_neptune_enabled(g->parent_obj.conf)) {
        return;
    }
    fd = virgl_renderer_context_get_poll_fd(ctx_id);
    if (fd < 0) {
        return; /* Contexts without notification retain timer polling. */
    }
    if (!gl->context_fence_watches) {
        gl->context_fence_watches = g_hash_table_new_full(
            g_direct_hash, g_direct_equal, NULL,
            virtio_gpu_context_fence_watch_free);
    }
    /* vrend contexts share one renderer fd; proxy contexts have their own. */
    g_hash_table_iter_init(&iter, gl->context_fence_watches);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        watch = value;
        if (watch->fd == fd) {
            /* Retain before replacing the same context's existing entry. */
            watch->references++;
            g_hash_table_replace(gl->context_fence_watches,
                                 GUINT_TO_POINTER(ctx_id), watch);
            return;
        }
    }
    watch = g_new0(VirtIOGPUContextFenceWatch, 1);
    watch->gpu = g;
    watch->fd = fd;
    watch->references = 1;
    g_hash_table_replace(gl->context_fence_watches,
                         GUINT_TO_POINTER(ctx_id), watch);
    qemu_set_fd_handler(fd, virtio_gpu_context_fence_ready, NULL, watch);
#endif
}

static void virgl_cmd_context_create(VirtIOGPU *g,
                                     struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_ctx_create cc;

    VIRTIO_GPU_FILL_CMD(cc);
    trace_virtio_gpu_cmd_ctx_create(cc.hdr.ctx_id,
                                    cc.debug_name);

    if (cc.context_init) {
        if (!virtio_gpu_context_init_enabled(g->parent_obj.conf)) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: context_init disabled",
                          __func__);
            cmd->error = VIRTIO_GPU_RESP_ERR_UNSPEC;
            return;
        }

#if VIRGL_VERSION_MAJOR >= 1
        int ret = virgl_renderer_context_create_with_flags(cc.hdr.ctx_id,
                                                           cc.context_init,
                                                           cc.nlen,
                                                           cc.debug_name);
        if (ret) {
            cmd->error = virgl_status_to_virtio_error(ret);
        } else {
            virtio_gpu_watch_context_fences(g, cc.hdr.ctx_id);
        }
        return;
#endif
    }

    int ret = virgl_renderer_context_create(cc.hdr.ctx_id,
                                            cc.nlen,
                                            cc.debug_name);
    if (ret) {
        cmd->error = virgl_status_to_virtio_error(ret);
    } else {
        virtio_gpu_watch_context_fences(g, cc.hdr.ctx_id);
    }
}

static void virgl_cmd_context_destroy(VirtIOGPU *g,
                                      struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_ctx_destroy cd;

    VIRTIO_GPU_FILL_CMD(cd);
    trace_virtio_gpu_cmd_ctx_destroy(cd.hdr.ctx_id);

    if (VIRTIO_GPU_GL(g)->context_fence_watches) {
        g_hash_table_remove(VIRTIO_GPU_GL(g)->context_fence_watches,
                            GUINT_TO_POINTER(cd.hdr.ctx_id));
    }
    virgl_renderer_context_destroy(cd.hdr.ctx_id);
}

static void virtio_gpu_rect_update(VirtIOGPU *g, int idx, int x, int y,
                                int width, int height)
{
    if (!g->parent_obj.scanout[idx].con) {
        return;
    }

    dpy_gl_update(g->parent_obj.scanout[idx].con, x, y, width, height);
}

#ifdef CONFIG_LINUX
/* Snapshot mutable guest storage before acknowledging its consumption. */
static int virtio_gpu_neptune_copy_texture(GLuint source, GLuint destination,
                                          int sx, int sy, int ex, int ey,
                                          int dx, int dy, int width, int height,
                                          bool flip_target)
{
    GLint old_read, old_draw;
    GLuint fbo[2];
    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    bool has_srgb = qemu_egl_mode == DISPLAY_GL_MODE_CORE ||
                   epoxy_has_gl_extension("GL_EXT_sRGB_write_control");
    GLboolean srgb = has_srgb && glIsEnabled(GL_FRAMEBUFFER_SRGB);
    int ret = -EIO;

    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_read);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &old_draw);
    glGenFramebuffers(2, fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo[0]);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, source, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo[1]);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, destination, 0);
    if (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) ==
            GL_FRAMEBUFFER_COMPLETE &&
        glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) ==
            GL_FRAMEBUFFER_COMPLETE) {
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glDisable(GL_SCISSOR_TEST);
        if (has_srgb) {
            glDisable(GL_FRAMEBUFFER_SRGB);
        }
        glBlitFramebuffer(sx, sy, ex, ey, dx, dy, dx + width,
                          flip_target ? dy - height : dy + height,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
        ret = glGetError() == GL_NO_ERROR ? 0 : -EIO;
    }
    /* The guest can overwrite source as soon as the command completes. */
    glFinish();
    if (glGetError() != GL_NO_ERROR) {
        ret = -EIO;
    }
    if (scissor) {
        glEnable(GL_SCISSOR_TEST);
    }
    if (srgb) {
        glEnable(GL_FRAMEBUFFER_SRGB);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, old_read);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, old_draw);
    glDeleteFramebuffers(2, fbo);
    return ret;
}

/* DMA-BUF pixels must be interpreted using the exporter's image layout. */
static int virtio_gpu_neptune_external_only(uint32_t fourcc, uint64_t modifier,
                                            bool *external_only)
{
    EGLint count = 0, written = 0;
    g_autofree EGLuint64KHR *modifiers = NULL;
    g_autofree EGLBoolean *external = NULL;

    *external_only = false;
    if (!eglQueryDmaBufModifiersEXT(qemu_egl_display, fourcc, 0, NULL, NULL,
                                    &count) || count < 0 || count > 4096) {
        return -EIO;
    }
    if (!count) {
        return -ENOTSUP;
    }
    modifiers = g_new(EGLuint64KHR, count);
    external = g_new(EGLBoolean, count);
    if (!eglQueryDmaBufModifiersEXT(qemu_egl_display, fourcc, count, modifiers,
                                    external, &written) ||
        written < 0 || written > count) {
        return -EIO;
    }
    for (EGLint i = 0; i < written; i++) {
        if (modifiers[i] == modifier) {
            *external_only = external[i];
            return 0;
        }
    }
    return -ENOTSUP;
}

static int virtio_gpu_neptune_external_copy(
    VirtIOGPU *g, struct VirtIOGPUExternalCopy **slot, EGLImageKHR image,
    GLuint target, uint32_t target_width, uint32_t target_height,
    uint32_t source_width, uint32_t source_height,
    uint32_t left, uint32_t top, uint32_t target_x, uint32_t target_y,
    uint32_t width, uint32_t height, bool flip_target, bool opaque)
{
    struct VirtIOGPUExternalContext saved = virtio_gpu_external_context();
    struct VirtIOGPUExternalCopy *copy = *slot;
    QemuEGLExternalCopyError error;
    uint32_t dy;

    if (VIRTIO_GPU_GL(g)->context_failure) {
        return -EOWNERDEAD;
    }
    if (!target_width || !target_height || target_width > INT_MAX ||
        target_height > INT_MAX || (flip_target && target_y < height)) {
        return -EINVAL;
    }
    dy = flip_target ? target_y - height : target_y;
    if (target_x > target_width || width > target_width - target_x ||
        dy > target_height || height > target_height - dy) {
        return -EINVAL;
    }
    if (copy && (copy->texture != target || copy->width != target_width ||
                 copy->height != target_height ||
                 copy->context != saved.context)) {
        if (!virtio_gpu_neptune_copy_destroy(g, slot)) {
            return -EOWNERDEAD;
        }
        copy = NULL;
    }
    if (!copy) {
        copy = g_new0(struct VirtIOGPUExternalCopy, 1);
        copy->helper = qemu_egl_external_copy_new(target, target_width,
                                                 target_height, &error);
        if (!copy->helper) {
            g_free(copy);
            if (error.context_restore_failed) {
                virtio_gpu_external_stop(g, &saved, &error);
                return -EOWNERDEAD;
            }
            return -EIO;
        }
        copy->texture = target;
        copy->width = target_width;
        copy->height = target_height;
        copy->context = saved.context;
        *slot = copy;
    }
    if (!qemu_egl_external_copy_run(copy->helper, image,
            source_width, source_height, left, top, target_x, dy,
            width, height, flip_target, opaque, &error)) {
        if (error.context_restore_failed) {
            virtio_gpu_external_stop(g, &saved, &error);
            return -EOWNERDEAD;
        }
        return -EIO;
    }
    return 0;
}

static int virtio_gpu_neptune_transfer_dmabuf(
    VirtIOGPU *g, int fd, uint32_t resource_id,
    const struct virtio_gpu_framebuffer *fb,
    const struct virtio_gpu_rect *source,
    uint32_t left, uint32_t top, uint32_t width, uint32_t height,
    DisplaySurface *surface, GLuint target, uint32_t target_x,
    uint32_t target_y, bool flip_target,
    struct VirtIOGPUExternalCopy **copy, uint32_t target_width,
    uint32_t target_height, bool opaque, bool *gpu_required)
{
    struct virgl_renderer_resource_info info = { 0 };
    struct virgl_renderer_export_query query = {
        .hdr = {
            .stype = VIRGL_RENDERER_STRUCTURE_TYPE_EXPORT_QUERY,
            .size = sizeof(query),
        },
        .in_resource_id = resource_id,
    };
    EGLImageKHR image;
    EGLint attrs[23];
    GLuint texture = 0, framebuffer = 0;
    GLint old_texture, old_framebuffer, old_pack_buffer;
    GLint old_alignment, old_row_length, old_skip_rows, old_skip_pixels;
    GLint old_swap_bytes = 0;
    GLint old_image_height = 0, old_skip_images = 0;
    GLenum format;
    uint64_t offset, end, fb_offset;
    uint32_t stride = surface ? surface_stride(surface) : 0;
    bool desktop = qemu_egl_mode == DISPLAY_GL_MODE_CORE;
    bool modifiers;
    bool direct_read;
    bool external_only = false;
    int n = 0;
    int ret = -EIO;

    if (target) {
        *gpu_required = true;
    }
    if (!width || !height ||
        virgl_renderer_resource_get_info(resource_id, &info) ||
        virgl_renderer_execute(&query, sizeof(query)) ||
        query.out_num_fds != 1 || !info.width || !info.height ||
        info.width != fb->width || info.height != fb->height ||
        info.width > INT_MAX || info.height > INT_MAX ||
        query.out_modifier == DRM_FORMAT_MOD_INVALID ||
        query.out_strides[0] != fb->stride ||
        query.out_strides[0] > INT_MAX || query.out_offsets[0] > INT_MAX ||
        fb->bytes_pp != 4 || (!surface && !target) ||
        stride % 4 || stride > INT_MAX ||
        left < source->x || top < source->y ||
        left > info.width || width > info.width - left ||
        top > info.height || height > info.height - top) {
        return -EINVAL;
    }
    for (int i = 1; i < 4; i++) {
        if (query.out_strides[i] || query.out_offsets[i]) {
            return -EINVAL;
        }
    }
    fb_offset = (uint64_t)query.out_offsets[0] +
                (uint64_t)source->y * fb->stride + (uint64_t)source->x * 4;
    offset = (uint64_t)(top - source->y) * stride +
             (uint64_t)(left - source->x) * 4;
    end = offset + (uint64_t)(height - 1) * stride + (uint64_t)width * 4;
    if (fb_offset != fb->offset || (surface &&
        (!stride || end - offset > INT_MAX ||
        left - source->x > surface_width(surface) ||
        width > surface_width(surface) - (left - source->x) ||
        end > (uint64_t)stride * surface_height(surface)))) {
        return -EINVAL;
    }

    switch (query.out_fourcc) {
    case DRM_FORMAT_ARGB8888:
    case DRM_FORMAT_XRGB8888:
        if ((fb->format != PIXMAN_LE_a8r8g8b8 &&
             fb->format != PIXMAN_LE_x8r8g8b8) ||
            (surface && surface_format(surface) != PIXMAN_LE_a8r8g8b8 &&
             surface_format(surface) != PIXMAN_LE_x8r8g8b8)) {
            return -EINVAL;
        }
        format = GL_BGRA;
        break;
    case DRM_FORMAT_ABGR8888:
    case DRM_FORMAT_XBGR8888:
        if ((fb->format != PIXMAN_LE_a8b8g8r8 &&
             fb->format != PIXMAN_LE_x8b8g8r8) ||
            (surface && surface_format(surface) != PIXMAN_LE_a8b8g8r8 &&
             surface_format(surface) != PIXMAN_LE_x8b8g8r8)) {
            return -EINVAL;
        }
        format = GL_RGBA;
        break;
    default:
        return -EINVAL;
    }

    if (!qemu_egl_display || eglGetCurrentContext() == EGL_NO_CONTEXT ||
        epoxy_gl_version() < 30 ||
        !epoxy_has_egl_extension(qemu_egl_display,
                                "EGL_EXT_image_dma_buf_import")) {
        return -ENOTSUP;
    }
    modifiers = epoxy_has_egl_extension(qemu_egl_display,
                                       "EGL_EXT_image_dma_buf_import_modifiers");
    if (query.out_modifier && !modifiers) {
        return -ENOTSUP;
    }
    if (target && modifiers) {
        ret = virtio_gpu_neptune_external_only(query.out_fourcc,
                                              query.out_modifier,
                                              &external_only);
        if (ret) {
            return ret;
        }
    }
    ret = -EIO;
    direct_read = desktop &&
        (epoxy_gl_version() >= 45 ||
         epoxy_has_gl_extension("GL_ARB_get_texture_sub_image"));
    attrs[n++] = EGL_WIDTH;
    attrs[n++] = info.width;
    attrs[n++] = EGL_HEIGHT;
    attrs[n++] = info.height;
    attrs[n++] = EGL_LINUX_DRM_FOURCC_EXT;
    attrs[n++] = query.out_fourcc;
    attrs[n++] = EGL_DMA_BUF_PLANE0_FD_EXT;
    attrs[n++] = fd;
    attrs[n++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
    attrs[n++] = query.out_offsets[0];
    attrs[n++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
    attrs[n++] = query.out_strides[0];
    if (modifiers) {
        attrs[n++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
        attrs[n++] = (uint32_t)query.out_modifier;
        attrs[n++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
        attrs[n++] = query.out_modifier >> 32;
    }
    attrs[n++] = EGL_NONE;
    image = eglCreateImageKHR(qemu_egl_display, EGL_NO_CONTEXT,
                             EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
    if (image == EGL_NO_IMAGE_KHR) {
        return -EIO;
    }
    if (target && external_only) {
        /*
         * This source cannot attach to a desktop GL_TEXTURE_2D framebuffer.
         * Never hide a failed required GPU copy behind a CPU roundtrip.
         */
        ret = virtio_gpu_neptune_external_copy(g, copy, image, target,
            target_width, target_height, info.width, info.height,
            left, top, target_x, target_y, width, height, flip_target, opaque);
        /*
         * EGL image destruction needs no current GL context. The helper has
         * completed source consumption before a restoration failure.
         */
        eglDestroyImageKHR(qemu_egl_display, image);
        return ret;
    }

    glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_texture);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_framebuffer);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &old_pack_buffer);
    glGetIntegerv(GL_PACK_ALIGNMENT, &old_alignment);
    glGetIntegerv(GL_PACK_ROW_LENGTH, &old_row_length);
    glGetIntegerv(GL_PACK_SKIP_ROWS, &old_skip_rows);
    glGetIntegerv(GL_PACK_SKIP_PIXELS, &old_skip_pixels);
    if (desktop) {
        glGetIntegerv(GL_PACK_SWAP_BYTES, &old_swap_bytes);
        glGetIntegerv(GL_PACK_IMAGE_HEIGHT, &old_image_height);
        glGetIntegerv(GL_PACK_SKIP_IMAGES, &old_skip_images);
        glPixelStorei(GL_PACK_SWAP_BYTES, GL_FALSE);
        glPixelStorei(GL_PACK_IMAGE_HEIGHT, 0);
        glPixelStorei(GL_PACK_SKIP_IMAGES, 0);
    }
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);
    if (glGetError() != GL_NO_ERROR) {
        goto out_restore;
    }
    if (target) {
        ret = virtio_gpu_neptune_copy_texture(texture, target,
            left, top, left + width, top + height,
            target_x, target_y, width, height, flip_target);
        goto out_restore;
    }
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, texture, 0);
    if (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) ==
        GL_FRAMEBUFFER_COMPLETE) {
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glPixelStorei(GL_PACK_ROW_LENGTH, stride / 4);
        glPixelStorei(GL_PACK_SKIP_ROWS, 0);
        glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
        if (direct_read) {
            /* Imported images may not support reliable framebuffer reads. */
            glGetTextureSubImage(texture, 0, left, top, 0, width, height, 1,
                                 format, GL_UNSIGNED_BYTE, end - offset,
                                 surface_data(surface) + offset);
        } else {
            GLuint copy_texture, copy_framebuffer;
            GLint old_draw_framebuffer, old_unpack_buffer;
            GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
            bool copied = false;

            /* Read an owned attachment on GLES and older desktop GL. */
            glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &old_draw_framebuffer);
            glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &old_unpack_buffer);
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
            glGenTextures(1, &copy_texture);
            glBindTexture(GL_TEXTURE_2D, copy_texture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, old_unpack_buffer);
            glGenFramebuffers(1, &copy_framebuffer);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, copy_framebuffer);
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D, copy_texture, 0);
            if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) ==
                GL_FRAMEBUFFER_COMPLETE) {
                glDisable(GL_SCISSOR_TEST);
                glBlitFramebuffer(left, top, left + width, top + height,
                                  0, 0, width, height, GL_COLOR_BUFFER_BIT,
                                  GL_NEAREST);
                glBindFramebuffer(GL_READ_FRAMEBUFFER, copy_framebuffer);
                glReadBuffer(GL_COLOR_ATTACHMENT0);
                glReadPixels(0, 0, width, height,
                             desktop ? format : GL_RGBA, GL_UNSIGNED_BYTE,
                             surface_data(surface) + offset);
                copied = glGetError() == GL_NO_ERROR;
                if (copied && !desktop && format == GL_BGRA) {
                    for (uint32_t row = 0; row < height; row++) {
                        uint8_t *pixels = surface_data(surface) + offset +
                                          (uint64_t)row * stride;
                        for (uint32_t col = 0; col < width; col++) {
                            uint8_t red = pixels[col * 4];
                            pixels[col * 4] = pixels[col * 4 + 2];
                            pixels[col * 4 + 2] = red;
                        }
                    }
                }
            }
            if (scissor) {
                glEnable(GL_SCISSOR_TEST);
            }
            glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, old_draw_framebuffer);
            glDeleteFramebuffers(1, &copy_framebuffer);
            glDeleteTextures(1, &copy_texture);
            if (!copied) {
                goto out_restore;
            }
        }
        if (glGetError() == GL_NO_ERROR) {
            ret = 0;
        }
    }
out_restore:
    /* Complete external image reads before the flush response permits reuse. */
    if (!target) {
        glFinish();
    }
    if (glGetError() != GL_NO_ERROR) {
        ret = -EIO;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, old_framebuffer);
    glBindTexture(GL_TEXTURE_2D, old_texture);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, old_pack_buffer);
    glPixelStorei(GL_PACK_ALIGNMENT, old_alignment);
    glPixelStorei(GL_PACK_ROW_LENGTH, old_row_length);
    glPixelStorei(GL_PACK_SKIP_ROWS, old_skip_rows);
    glPixelStorei(GL_PACK_SKIP_PIXELS, old_skip_pixels);
    if (desktop) {
        glPixelStorei(GL_PACK_SWAP_BYTES, old_swap_bytes);
        glPixelStorei(GL_PACK_IMAGE_HEIGHT, old_image_height);
        glPixelStorei(GL_PACK_SKIP_IMAGES, old_skip_images);
    }
    glDeleteFramebuffers(1, &framebuffer);
    glDeleteTextures(1, &texture);
    eglDestroyImageKHR(qemu_egl_display, image);
    return ret;
}

static int virtio_gpu_neptune_readback_dmabuf(
    int fd, uint32_t resource_id,
    const struct virtio_gpu_framebuffer *fb,
    const struct virtio_gpu_rect *source,
    uint32_t left, uint32_t top, uint32_t width, uint32_t height,
    DisplaySurface *surface)
{
    return virtio_gpu_neptune_transfer_dmabuf(NULL, fd, resource_id, fb, source,
        left, top, width, height, surface, 0, 0, 0, false,
        NULL, 0, 0, false, NULL);
}
#endif

/* Darwin shared textures use linear SHM; Linux DMA-BUFs use GPU readback. */
static int virtio_gpu_neptune_readback_blob(
    const struct virtio_gpu_virgl_resource *res,
    const struct virtio_gpu_framebuffer *fb,
    const struct virtio_gpu_rect *source,
    uint32_t left, uint32_t top, uint32_t width, uint32_t height,
    DisplaySurface *surface)
{
    struct stat st;
    uint64_t source_offset;
    uint64_t source_end;
    uint64_t surface_offset;
    uint64_t dst_stride;
    uint64_t surface_end;
    uint64_t mapped_size;
    size_t map_len;
    uint8_t *map;
    uint8_t *dst;
    uint32_t fd_type;
    int fd = -1;
    int ret;

    if (!res || !res->is_blob || !fb || !surface || !width || !height ||
        surface_bytes_per_pixel(surface) != fb->bytes_pp) {
        return -EINVAL;
    }

    ret = virgl_renderer_resource_export_blob(res->base.resource_id,
                                              &fd_type, &fd);
    if (ret) {
        return ret;
    }
    if (fd < 0) {
        ret = -EINVAL;
        goto out_close;
    }
#ifdef CONFIG_LINUX
    if (fd_type == VIRGL_RENDERER_BLOB_FD_TYPE_DMABUF) {
        ret = virtio_gpu_neptune_readback_dmabuf(fd, res->base.resource_id,
                                               fb, source, left, top,
                                               width, height, surface);
        goto out_close;
    }
#endif
    if (fd_type != VIRGL_RENDERER_BLOB_FD_TYPE_SHM ||
        fstat(fd, &st) < 0 || st.st_size < 0) {
        ret = -EINVAL;
        goto out_close;
    }
    mapped_size = MIN(res->base.blob_size, (uint64_t)st.st_size);
    source_offset = (uint64_t)fb->offset +
                    (uint64_t)(top - source->y) * fb->stride +
                    (uint64_t)(left - source->x) * fb->bytes_pp;
    source_end = source_offset + (uint64_t)(height - 1) * fb->stride +
                 (uint64_t)width * fb->bytes_pp;
    dst_stride = surface_stride(surface);
    surface_offset = (uint64_t)(top - source->y) * dst_stride +
                     (uint64_t)(left - source->x) * fb->bytes_pp;
    surface_end = surface_offset + (uint64_t)(height - 1) * dst_stride +
                  (uint64_t)width * fb->bytes_pp;
    if (source_offset > mapped_size || source_end > mapped_size ||
        source_end > SIZE_MAX || !dst_stride ||
        surface_end > dst_stride * surface_height(surface)) {
        ret = -EINVAL;
        goto out_close;
    }

    map_len = source_end;
    map = mmap(NULL, map_len, PROT_READ, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        ret = -errno;
        goto out_close;
    }


    dst = surface_data(surface) + surface_offset;
    for (uint32_t row = 0; row < height; row++) {
        memcpy(dst + (uint64_t)row * dst_stride,
               map + source_offset + (uint64_t)row * fb->stride,
               (size_t)width * fb->bytes_pp);
    }
    ret = 0;
    munmap(map, map_len);

out_close:
    close(fd);
    return ret;
}

/* The producer has completed its GPU release before these commands arrive. */
static int virtio_gpu_neptune_copy_resource(
    VirtIOGPU *g,
    const struct virtio_gpu_virgl_resource *res,
    const struct virtio_gpu_framebuffer *fb,
    const struct virtio_gpu_rect *source,
    const struct virtio_gpu_rect *rect,
    uint32_t target, uint32_t x, uint32_t y, bool flip_target,
    struct VirtIOGPUExternalCopy **copy, uint32_t target_width,
    uint32_t target_height, bool opaque, bool *gpu_required)
{
    *gpu_required = false;
#ifdef CONFIG_LINUX
    struct virgl_renderer_resource_info info = { 0 };
    uint32_t fd_type;
    int fd = -1, ret;
    int top, bottom;

    /* Only an explicitly exported SHM source permits CPU-origin upload. */
    *gpu_required = true;
    if (res->is_blob) {
        ret = virgl_renderer_resource_export_blob(res->base.resource_id,
                                                  &fd_type, &fd);
        if (ret) {
            return ret;
        }
        if (fd < 0) {
            return -EINVAL;
        }
        if (fd_type == VIRGL_RENDERER_BLOB_FD_TYPE_SHM) {
            *gpu_required = false;
        }
        ret = fd_type == VIRGL_RENDERER_BLOB_FD_TYPE_DMABUF ?
            virtio_gpu_neptune_transfer_dmabuf(g, fd, res->base.resource_id,
                fb, source, rect->x, rect->y, rect->width, rect->height,
                NULL, target, x, y, flip_target, copy,
                target_width, target_height, opaque, gpu_required) : -ENOTSUP;
        close(fd);
        return ret;
    }
    if (!target) {
        return -EINVAL;
    }
    ret = virgl_renderer_resource_get_info(res->base.resource_id, &info);
    if (ret || !info.tex_id) {
        return ret ? ret : -ENOTSUP;
    }
    if ((uint64_t)rect->x + rect->width > info.width ||
        (uint64_t)rect->y + rect->height > info.height) {
        return -EINVAL;
    }
    top = rect->y;
    bottom = top + rect->height;
    if (info.flags & VIRTIO_GPU_RESOURCE_FLAG_Y_0_TOP) {
        top = info.height - top;
        bottom = info.height - bottom;
    }
    return virtio_gpu_neptune_copy_texture(info.tex_id, target,
        rect->x, top, rect->x + rect->width, bottom,
        x, y, rect->width, rect->height, flip_target);
#else
    return -ENOTSUP;
#endif
}

#ifdef CONFIG_LINUX
static int virtio_gpu_neptune_scanout_fallback(
    VirtIOGPU *g, const struct virtio_gpu_virgl_resource *res,
    const struct virtio_gpu_framebuffer *fb,
    const struct virtio_gpu_rect *source, int status)
{
    bool gpu_required;

    /* A zero target classifies SHM without issuing a GPU copy. */
    virtio_gpu_neptune_copy_resource(g, res, fb, source, source,
        0, 0, 0, false, NULL, 0, 0, false, &gpu_required);
    return gpu_required ? status : 0;
}
#endif

/* Positive: published; zero: legacy fallback allowed; negative: copy failed. */
static int virtio_gpu_neptune_gpu_scanout(
    VirtIOGPU *g, unsigned index,
    const struct virtio_gpu_virgl_resource *res,
    const struct virtio_gpu_rect *source,
    const struct virtio_gpu_rect *dirty)
{
#ifdef CONFIG_LINUX
    VirtIOGPUGL *gl = VIRTIO_GPU_GL(g);
    struct virtio_gpu_scanout *scanout = &g->parent_obj.scanout[index];
    GLuint texture = gl->scanout_texture[index];
    GLuint old_texture = texture;
    struct VirtIOGPUExternalCopy *new_copy = NULL;
    struct VirtIOGPUExternalCopy *old_copy = gl->scanout_external_copy[index];
    struct VirtIOGPUExternalCopy **copy;
    struct virtio_gpu_rect rect;
    uint32_t right, bottom;
    GLint binding, unpack;
    bool gpu_required;
    int ret;
    bool replace = !texture ||
        gl->scanout_texture_width[index] != source->width ||
        gl->scanout_texture_height[index] != source->height;

    if (!console_has_gl(scanout->con) ||
        !console_gl_texture_read_sync(scanout->con) ||
        epoxy_gl_version() < 30 ||
        !scanout->ds || surface_bytes_per_pixel(scanout->ds) != 4 ||
        (surface_format(scanout->ds) != PIXMAN_LE_x8r8g8b8 &&
         surface_format(scanout->ds) != PIXMAN_LE_a8r8g8b8 &&
         surface_format(scanout->ds) != PIXMAN_LE_x8b8g8r8 &&
         surface_format(scanout->ds) != PIXMAN_LE_a8b8g8r8)) {
        return virtio_gpu_neptune_scanout_fallback(g, res, &scanout->fb,
                                                  source, -ENOTSUP);
    }
    rect.x = MAX(source->x, dirty->x);
    rect.y = MAX(source->y, dirty->y);
    right = MIN(source->x + source->width, dirty->x + dirty->width);
    bottom = MIN(source->y + source->height, dirty->y + dirty->height);
    if (replace || !gl->scanout_gpu[index]) {
        rect = *source;
    } else if (right <= rect.x || bottom <= rect.y) {
        return true;
    } else {
        rect.width = right - rect.x;
        rect.height = bottom - rect.y;
    }
    if (replace) {
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &binding);
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpack);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        /* Display storage is opaque, including XRGB padding variants. */
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, source->width, source->height,
                     0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glBindTexture(GL_TEXTURE_2D, binding);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, unpack);
        if (glGetError() != GL_NO_ERROR) {
            glDeleteTextures(1, &texture);
            return virtio_gpu_neptune_scanout_fallback(g, res, &scanout->fb,
                                                      source, -EIO);
        }
    }
    copy = replace ? &new_copy : &gl->scanout_external_copy[index];
    ret = virtio_gpu_neptune_copy_resource(g, res, &scanout->fb, source, &rect,
            texture, rect.x - source->x, rect.y - source->y, false,
            copy, source->width, source->height, true, &gpu_required);
    if (ret) {
        if (replace) {
            if (gl->context_failure ||
                !virtio_gpu_neptune_copy_destroy(g, &new_copy)) {
                gl->scanout_pending_texture[index] = texture;
                gl->scanout_pending_copy[index] = new_copy;
                return -EOWNERDEAD;
            }
            glDeleteTextures(1, &texture);
        }
        return gpu_required ? ret : 0;
    }
    if (replace) {
        gl->scanout_external_copy[index] = new_copy;
    }
    gl->scanout_texture[index] = texture;
    gl->scanout_texture_width[index] = source->width;
    gl->scanout_texture_height[index] = source->height;
    gl->scanout_gpu[index] = true;
    gl->scanout_needs_full_update[index] = false;
    trace_virtio_gpu_neptune_gpu_scanout(res->base.resource_id,
                                        rect.width, rect.height);
    if (!console_gl_scanout_texture_is(scanout->con, texture)) {
        dpy_gl_scanout_texture(scanout->con, texture, false,
            source->width, source->height, 0, 0, source->width, source->height,
            NO_NATIVE_TEXTURE, NULL);
    }
    dpy_gl_update(scanout->con, rect.x - source->x, rect.y - source->y,
                   rect.width, rect.height);
    /* Display callbacks change contexts. Delete in the renderer share group. */
    virgl_renderer_force_ctx_0();
    if (replace && old_texture) {
        if (!virtio_gpu_neptune_copy_destroy(g, &old_copy)) {
            gl->scanout_pending_texture[index] = old_texture;
            gl->scanout_pending_copy[index] = old_copy;
            return -EOWNERDEAD;
        }
        glDeleteTextures(1, &old_texture);
    }
    return true;
#else
    return false;
#endif
}

static int virtio_gpu_neptune_readback_surface(
    uint32_t resource_id,
    const struct virtio_gpu_virgl_resource *res,
    const struct virtio_gpu_framebuffer *fb,
    const struct virtio_gpu_rect *source,
    const struct virtio_gpu_rect *update,
    DisplaySurface *surface,
    struct virtio_gpu_rect *updated)
{
    struct virtio_gpu_box box;
    struct iovec iov;
    uint32_t left, top, right, bottom;
    uint32_t stride;
    uint32_t bytes_pp;
    uint64_t offset;
    uint64_t surface_size;
    uint64_t flush_right;
    uint64_t flush_bottom;
    uint64_t scanout_right;
    uint64_t scanout_bottom;
    int ret;

    if (updated) {
        memset(updated, 0, sizeof(*updated));
    }
    if (!source || !update || !surface || !source->width || !source->height ||
        surface_width(surface) != source->width ||
        surface_height(surface) != source->height) {
        return EINVAL;
    }

    flush_right = (uint64_t)update->x + update->width;
    flush_bottom = (uint64_t)update->y + update->height;
    scanout_right = (uint64_t)source->x + source->width;
    scanout_bottom = (uint64_t)source->y + source->height;
    if (!update->width || !update->height ||
        flush_right > UINT32_MAX || flush_bottom > UINT32_MAX ||
        scanout_right > UINT32_MAX || scanout_bottom > UINT32_MAX) {
        return EINVAL;
    }

    left = MAX(update->x, source->x);
    top = MAX(update->y, source->y);
    right = MIN((uint32_t)flush_right, (uint32_t)scanout_right);
    bottom = MIN((uint32_t)flush_bottom, (uint32_t)scanout_bottom);
    if (right <= left || bottom <= top) {
        return 0;
    }

    stride = surface_stride(surface);
    bytes_pp = surface_bytes_per_pixel(surface);
    surface_size = (uint64_t)stride * surface_height(surface);
    if (!stride || !bytes_pp || surface_size > SIZE_MAX ||
        surface_size > UINT32_MAX) {
        return EINVAL;
    }
    offset = (uint64_t)(top - source->y) * stride +
             (uint64_t)(left - source->x) * bytes_pp;
    if (offset >= surface_size) {
        return EINVAL;
    }
    iov.iov_base = surface_data(surface);
    iov.iov_len = surface_size;
    box.x = left;
    box.y = top;
    box.z = 0;
    box.w = right - left;
    box.h = bottom - top;
    box.d = 1;

    if (res && res->is_blob) {
        ret = virtio_gpu_neptune_readback_blob(res, fb, source, left, top,
                                               box.w, box.h, surface);
    } else {
        ret = virgl_renderer_transfer_read_iov(resource_id, 0, 0, stride,
                                               surface_size,
                                               (struct virgl_box *)&box,
                                               offset, &iov, 1);
    }
    if (ret) {
        return ret;
    }

    if (updated) {
        updated->x = left - source->x;
        updated->y = top - source->y;
        updated->width = box.w;
        updated->height = box.h;
    }
    return 0;
}

/* Binding selects a resource; the following RESOURCE_FLUSH publishes its
 * pixels. Do not download the same frame twice or recreate a GL texture for
 * each buffer in the flip chain. The guest waits for that flush to complete. */
static bool virtio_gpu_neptune_prepare_scanout(
    struct virtio_gpu_scanout *scanout,
    const struct virtio_gpu_framebuffer *fb,
    const struct virtio_gpu_rect *source)
{
    uint64_t stride = (uint64_t)source->width * fb->bytes_pp;
    pixman_format_code_t format = fb->format;

    /*
     * Scanout is opaque. DWM and exclusive primaries can alternate alpha
     * and padding variants of the same byte layout; retaining their alpha
     * distinction reallocates the frontend texture on every transition.
     * This applies only to display storage, never to blit snapshots.
     */
    if (format == PIXMAN_a8r8g8b8) {
        format = PIXMAN_x8r8g8b8;
    } else if (format == PIXMAN_a8b8g8r8) {
        format = PIXMAN_x8b8g8r8;
    }
    if (!source->width || !source->height || !fb->bytes_pp || !fb->format ||
        source->width > INT_MAX || source->height > INT_MAX ||
        stride > INT_MAX) {
        return false;
    }
    if (!scanout->ds ||
        surface_width(scanout->ds) != source->width ||
        surface_height(scanout->ds) != source->height ||
        surface_format(scanout->ds) != format) {
        DisplaySurface *surface = qemu_create_displaysurface_from(
            source->width, source->height, format, stride, NULL);
        dpy_gl_scanout_disable(scanout->con);
        dpy_gfx_replace_surface(scanout->con, surface);
        scanout->ds = qemu_console_surface(scanout->con);
    }
    return true;
}

static DisplaySurface *virtio_gpu_neptune_create_surface(
    uint32_t resource_id,
    const struct virtio_gpu_virgl_resource *res,
    const struct virtio_gpu_framebuffer *fb,
    const struct virtio_gpu_rect *source,
    pixman_format_code_t format,
    int *readback_status)
{
    uint32_t bytes_pp = DIV_ROUND_UP(PIXMAN_FORMAT_BPP(format), 8);
    uint64_t stride = (uint64_t)source->width * bytes_pp;
    DisplaySurface *surface;

    *readback_status = -EINVAL;
    if (!format || !bytes_pp || source->width > INT_MAX ||
        source->height > INT_MAX || stride > INT_MAX) {
        return NULL;
    }

    surface = qemu_create_displaysurface_from(source->width, source->height,
                                              format, stride, NULL);
    *readback_status = virtio_gpu_neptune_readback_surface(
        resource_id, res, fb, source, source, surface, NULL);
    if (*readback_status) {
        qemu_free_displaysurface(surface);
        return NULL;
    }
    return surface;
}

static int virtio_gpu_neptune_present_blt(
    VirtIOGPU *g,
    const struct virtio_gpu_triton_present_blt *b,
    const struct virtio_gpu_virgl_resource *src,
    struct virtio_gpu_virgl_resource *dst)
{
    struct virtio_gpu_framebuffer fb = { 0 };
    struct virtio_gpu_box box = { 0 };
    struct iovec iov;
    DisplaySurface *snapshot;
    uint64_t offset;
    int64_t begin_ns, snapshot_ns;
    int ret;

    if (!src || !dst || !src->is_blob || dst->is_blob || src == dst ||
        b->reserved || !b->source_width || !b->source_height ||
        b->source_width > 4096 || b->source_height > 4096 ||
        !b->source_rect.width || !b->source_rect.height ||
        (uint64_t)b->source_rect.x + b->source_rect.width > b->source_width ||
        (uint64_t)b->source_rect.y + b->source_rect.height > b->source_height ||
        (uint64_t)b->destination_x + b->source_rect.width > dst->base.width ||
        (uint64_t)b->destination_y + b->source_rect.height > dst->base.height ||
        b->source_stride < (uint64_t)b->source_width * 4 ||
        (b->source_format != VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM &&
         b->source_format != VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM) ||
        (dst->base.format != VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM &&
         dst->base.format != VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM)) {
        return -EINVAL;
    }

    /* The SHM helper addresses relative to source_rect. Bake the absolute
     * source origin into offset, then snapshot only the requested rectangle. */
    offset = (uint64_t)b->source_offset +
             (uint64_t)b->source_rect.y * b->source_stride +
             (uint64_t)b->source_rect.x * 4;
    if (offset > UINT32_MAX) {
        return -EINVAL;
    }
    fb.format = virtio_gpu_get_pixman_format(b->source_format);
    fb.bytes_pp = 4;
    fb.width = b->source_width;
    fb.height = b->source_height;
    fb.stride = b->source_stride;
    fb.offset = offset;
    begin_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
#ifdef CONFIG_LINUX
    struct virgl_renderer_resource_info target_info = { 0 };
    bool gpu_required;
    int target_status;

    target_status = virgl_renderer_resource_get_info(
        b->destination_resource_id, &target_info);
    if (target_status) {
        memset(&target_info, 0, sizeof(target_info));
    }
    /*
     * Classify the source even when destination metadata is unavailable.
     * Only a confirmed SHM source may use the CPU-origin upload below.
     */
    ret = virtio_gpu_neptune_copy_resource(g, src, &fb, &b->source_rect,
            &b->source_rect, target_info.tex_id,
            b->destination_x,
            target_info.flags & VIRTIO_GPU_RESOURCE_FLAG_Y_0_TOP ?
                target_info.height - b->destination_y : b->destination_y,
            target_info.flags & VIRTIO_GPU_RESOURCE_FLAG_Y_0_TOP,
            &dst->external_copy, target_info.width, target_info.height,
            dst->base.format == VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM,
            &gpu_required);
    if (!ret) {
        trace_virtio_gpu_neptune_present_cost(b->source_resource_id, 0,
            qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - begin_ns, 0);
        return 0;
    }
    if (gpu_required) {
        return target_status ? target_status : ret;
    }
#endif
    snapshot = virtio_gpu_neptune_create_surface(
        b->source_resource_id, src, &fb, &b->source_rect, fb.format, &ret);
    if (!snapshot) {
        return ret;
    }
    snapshot_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);

    /* transfer_write_iov consumes this private snapshot synchronously.
     * Pixels outside the box remain in the destination resource. The later
     * ordinary-primary flush publishes the completed group of rectangles. */
    box.x = b->destination_x;
    box.y = b->destination_y;
    box.w = b->source_rect.width;
    box.h = b->source_rect.height;
    box.d = 1;
    iov.iov_base = surface_data(snapshot);
    iov.iov_len = (size_t)surface_stride(snapshot) * surface_height(snapshot);
    ret = virgl_renderer_transfer_write_iov(
        b->destination_resource_id, 0, 0, surface_stride(snapshot),
        iov.iov_len, (struct virgl_box *)&box, 0, &iov, 1);
    qemu_free_displaysurface(snapshot);
    trace_virtio_gpu_neptune_present_cost(b->source_resource_id,
        (uint64_t)b->source_rect.width * b->source_rect.height * 4,
        snapshot_ns - begin_ns,
        qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - snapshot_ns);
    return ret;
}

static void virgl_cmd_triton_present_blt(VirtIOGPU *g,
                                        struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_triton_present_blt b;
    struct virtio_gpu_virgl_resource *src, *dst;
    int ret;

    cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
    VIRTIO_GPU_FILL_CMD(b);
    virtio_gpu_bswap_32(&b, sizeof(b));
    if (!virtio_gpu_neptune_enabled(g->parent_obj.conf)) {
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
        return;
    }
    src = virtio_gpu_virgl_find_resource(g, b.source_resource_id);
    dst = virtio_gpu_virgl_find_resource(g, b.destination_resource_id);
    triton_trace_record(TT_HOST_COPY_BEGIN, cmd->cmd_hdr.fence_id,
        cmd->cmd_hdr.ctx_id, b.source_resource_id,
        b.destination_resource_id, 0);
    ret = virtio_gpu_neptune_present_blt(g, &b, src, dst);
    triton_trace_record(TT_HOST_COPY_END, cmd->cmd_hdr.fence_id,
        cmd->cmd_hdr.ctx_id, ret, 0, 0);
    cmd->error = ret ? virgl_status_to_virtio_error(ret) : 0;
}

static void virgl_cmd_resource_flush(VirtIOGPU *g,
                                     struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_resource_flush rf;
    struct virtio_gpu_virgl_resource *res;
    struct virgl_renderer_resource_info info;
    int i;
    int ret;

    VIRTIO_GPU_FILL_CMD(rf);
    trace_virtio_gpu_cmd_res_flush(rf.resource_id,
                                   rf.r.width, rf.r.height, rf.r.x, rf.r.y);

    res = virtio_gpu_virgl_find_resource(g, rf.resource_id);
    if (!res) {
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }
    if (!rf.r.width || !rf.r.height ||
        (uint64_t)rf.r.x + rf.r.width > INT_MAX ||
        (uint64_t)rf.r.y + rf.r.height > INT_MAX) {
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
        return;
    }
    if (!res->is_blob) {
        memset(&info, 0, sizeof(info));
        ret = virgl_renderer_resource_get_info(rf.resource_id, &info);
        if (ret) {
            cmd->error = virgl_status_to_virtio_error(ret);
            return;
        }
        if (rf.r.x > info.width || rf.r.y > info.height ||
            rf.r.width > info.width - rf.r.x ||
            rf.r.height > info.height - rf.r.y) {
            cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
            return;
        }
    }

    for (i = 0; i < g->parent_obj.conf.max_outputs; i++) {
        if (g->parent_obj.scanout[i].resource_id != rf.resource_id) {
            continue;
        }
        if (virtio_gpu_neptune_enabled(g->parent_obj.conf)) {
            struct virtio_gpu_scanout *scanout =
                &g->parent_obj.scanout[i];
            struct virtio_gpu_rect source;
            struct virtio_gpu_rect dirty = rf.r;
            struct virtio_gpu_rect updated;
            int ret;

            if (scanout->x < 0 || scanout->y < 0 || !scanout->ds) {
                cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
                return;
            }
            if (res->is_blob &&
                (rf.r.x > scanout->fb.width ||
                 rf.r.y > scanout->fb.height ||
                 rf.r.width > scanout->fb.width - rf.r.x ||
                 rf.r.height > scanout->fb.height - rf.r.y)) {
                cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
                return;
            }
            source.x = scanout->x;
            source.y = scanout->y;
            source.width = scanout->width;
            source.height = scanout->height;
            if (VIRTIO_GPU_GL(g)->scanout_needs_full_update[i]) {
                dirty = source;
            }
            ret = virtio_gpu_neptune_gpu_scanout(g, i, res, &source, &dirty);
            if (ret < 0) {
                cmd->error = virgl_status_to_virtio_error(ret);
                return;
            }
            if (ret > 0) {
                continue;
            }
            if (VIRTIO_GPU_GL(g)->scanout_gpu[i]) {
                DisplaySurface *surface = qemu_create_displaysurface_from(
                    source.width, source.height,
                    surface_format(scanout->ds), surface_stride(scanout->ds),
                    NULL);
                VIRTIO_GPU_GL(g)->scanout_needs_full_update[i] = true;
                dpy_gl_scanout_disable(scanout->con);
                dpy_gfx_replace_surface(scanout->con, surface);
                scanout->ds = qemu_console_surface(scanout->con);
                VIRTIO_GPU_GL(g)->scanout_gpu[i] = false;
                dirty = source;
                virgl_renderer_force_ctx_0();
                if (!virtio_gpu_neptune_copy_destroy(g,
                        &VIRTIO_GPU_GL(g)->scanout_external_copy[i])) {
                    cmd->error = VIRTIO_GPU_RESP_ERR_UNSPEC;
                    return;
                }
                glDeleteTextures(1, &VIRTIO_GPU_GL(g)->scanout_texture[i]);
                VIRTIO_GPU_GL(g)->scanout_texture[i] = 0;
            }
            triton_trace_record(TT_HOST_COPY_BEGIN, cmd->cmd_hdr.fence_id,
                cmd->cmd_hdr.ctx_id, rf.resource_id,
                (uint64_t)dirty.width * dirty.height * 4, 0);
            ret = virtio_gpu_neptune_readback_surface(
                rf.resource_id, res, &scanout->fb, &source, &dirty,
                scanout->ds, &updated);
            triton_trace_record(TT_HOST_COPY_END, cmd->cmd_hdr.fence_id,
                cmd->cmd_hdr.ctx_id, ret, 0, 0);
            if (ret) {
                cmd->error = virgl_status_to_virtio_error(ret);
                return;
            }
            VIRTIO_GPU_GL(g)->scanout_needs_full_update[i] = false;
            if (updated.width && updated.height) {
                if (trace_event_get_state_backends(
                        TRACE_VIRTIO_GPU_NEPTUNE_SCANOUT_PIXEL) &&
                    surface_bytes_per_pixel(scanout->ds) == 4) {
                    uint32_t pixel;
                    const uint8_t *center = surface_data(scanout->ds) +
                        (surface_height(scanout->ds) / 2) *
                            surface_stride(scanout->ds) +
                        (surface_width(scanout->ds) / 2) * 4;
                    memcpy(&pixel, center, sizeof(pixel));
                    trace_virtio_gpu_neptune_scanout_pixel(
                        rf.resource_id, scanout->fb.format,
                        scanout->fb.stride, scanout->fb.offset, pixel);
                }
                triton_trace_scanout(scanout->con, cmd->cmd_hdr.fence_id,
                                     cmd->cmd_hdr.ctx_id, rf.resource_id);
                dpy_gfx_update(scanout->con, updated.x, updated.y,
                               updated.width, updated.height);
            }
        } else {
            struct virtio_gpu_scanout *scanout =
                &g->parent_obj.scanout[i];
            uint32_t left = MAX(rf.r.x, (uint32_t)scanout->x);
            uint32_t top = MAX(rf.r.y, (uint32_t)scanout->y);
            uint32_t right = MIN(rf.r.x + rf.r.width,
                                 (uint32_t)scanout->x + scanout->width);
            uint32_t bottom = MIN(rf.r.y + rf.r.height,
                                  (uint32_t)scanout->y + scanout->height);
            if (right > left && bottom > top) {
                virtio_gpu_rect_update(g, i,
                                       left - scanout->x,
                                       top - scanout->y,
                                       right - left,
                                       bottom - top);
            }
        }
    }
}

static bool virtio_gpu_neptune_set_scanout(VirtIOGPU *g, unsigned scanout_id,
                                          struct virtio_gpu_framebuffer *fb,
                                          struct virtio_gpu_rect *source)
{
    struct virtio_gpu_scanout *scanout = &g->parent_obj.scanout[scanout_id];

    if (!virtio_gpu_neptune_prepare_scanout(scanout, fb, source)) {
        return false;
    }
    /*
     * Reused storage can still contain a different resource or source crop.
     * The first flush must refresh the whole binding even for a dirty rect.
     */
    VIRTIO_GPU_GL(g)->scanout_needs_full_update[scanout_id] = true;
    triton_trace_invalidate(scanout->con);
    return true;
}

static void virgl_cmd_set_scanout(VirtIOGPU *g,
                                  struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_set_scanout ss;
    struct virtio_gpu_virgl_resource *res;
    struct virtio_gpu_scanout *scanout;
    struct virtio_gpu_framebuffer fb = { 0 };
    struct virgl_renderer_resource_info info;
    ScanoutTextureNative native = NO_NATIVE_TEXTURE;
    uint64_t row_bytes;
    uint64_t offset;
    int ret;

    VIRTIO_GPU_FILL_CMD(ss);
    trace_virtio_gpu_cmd_set_scanout(ss.scanout_id, ss.resource_id,
                                     ss.r.width, ss.r.height, ss.r.x, ss.r.y);

    if (ss.scanout_id >= g->parent_obj.conf.max_outputs) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: illegal scanout id specified %d",
                      __func__, ss.scanout_id);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID;
        return;
    }

    if (!ss.resource_id) {
        virtio_gpu_disable_scanout(g, ss.scanout_id);
        return;
    }
    if (ss.r.width < 16 || ss.r.height < 16 ||
        ss.r.x > INT_MAX || ss.r.y > INT_MAX ||
        ss.r.width > INT_MAX || ss.r.height > INT_MAX) {
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
        return;
    }

    res = virtio_gpu_virgl_find_resource(g, ss.resource_id);
    if (!res) {
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }

#if VIRGL_VERSION_MAJOR >= 1
    struct virgl_renderer_resource_info_ext ext;
    memset(&ext, 0, sizeof(ext));
    ret = virgl_renderer_resource_get_info_ext(ss.resource_id, &ext);
    info = ext.base;
    /* fallback to older version */
    native = (ScanoutTextureNative){
        .type = ext.d3d_tex2d ? SCANOUT_TEXTURE_NATIVE_TYPE_D3D :
                                SCANOUT_TEXTURE_NATIVE_TYPE_NONE,
        .handle = ext.d3d_tex2d,
    };
#if VIRGL_RENDERER_RESOURCE_INFO_EXT_VERSION >= NATIVE_HANDLE_SUPPORT_VERSION
    if (ext.version >= VIRGL_RENDERER_RESOURCE_INFO_EXT_VERSION) {
        switch (ext.native_type) {
#ifdef CONFIG_METAL
        case VIRGL_NATIVE_HANDLE_METAL_TEXTURE:
            native.type = SCANOUT_TEXTURE_NATIVE_TYPE_METAL;
            native.handle = ext.native_handle;
            break;
#endif
        case VIRGL_NATIVE_HANDLE_NONE:
        case VIRGL_NATIVE_HANDLE_D3D_TEX2D:
            break;
        default:
            break;
        }
    }
#endif
#else /* VIRGL_VERSION_MAJOR < 1 */
    memset(&info, 0, sizeof(info));
    ret = virgl_renderer_resource_get_info(ss.resource_id, &info);
#endif
    if (ret) {
        cmd->error = virgl_status_to_virtio_error(ret);
        return;
    }

    fb.format = virtio_gpu_get_pixman_format(res->base.format);
    fb.bytes_pp = fb.format ? DIV_ROUND_UP(PIXMAN_FORMAT_BPP(fb.format), 8) : 0;
    fb.width = info.width;
    fb.height = info.height;
    row_bytes = (uint64_t)fb.width * fb.bytes_pp;
    if (!fb.format || !fb.bytes_pp || !fb.width || !fb.height ||
        ss.r.x > fb.width || ss.r.y > fb.height ||
        ss.r.width > fb.width - ss.r.x ||
        ss.r.height > fb.height - ss.r.y ||
        row_bytes > UINT32_MAX ||
        (info.stride && info.stride < row_bytes)) {
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
        return;
    }
    fb.stride = info.stride ? info.stride : (uint32_t)row_bytes;
    offset = (uint64_t)ss.r.y * fb.stride +
             (uint64_t)ss.r.x * fb.bytes_pp;
    if (offset > UINT32_MAX) {
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
        return;
    }
    fb.offset = offset;

    scanout = &g->parent_obj.scanout[ss.scanout_id];
    virgl_renderer_force_ctx_0();
    if (virtio_gpu_neptune_enabled(g->parent_obj.conf)) {
        /*
         * Ordinary fullscreen primaries switch through SET_SCANOUT too.
         * Reuse the display surface just as for blob scanouts; RESOURCE_FLUSH
         * performs the single readback and publishes the completed pixels.
         */
        if (!virtio_gpu_neptune_set_scanout(g, ss.scanout_id, &fb, &ss.r)) {
            cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
            return;
        }
    } else {
        qemu_console_resize(scanout->con, ss.r.width, ss.r.height);
        dpy_gl_scanout_texture(scanout->con, info.tex_id,
                               info.flags & VIRTIO_GPU_RESOURCE_FLAG_Y_0_TOP,
                               info.width, info.height,
                               ss.r.x, ss.r.y, ss.r.width, ss.r.height,
                               native, NULL);
    }
    virtio_gpu_update_scanout(g, ss.scanout_id, &res->base, &fb, &ss.r);
    g->parent_obj.enable = 1;
}

static void virgl_cmd_submit_3d(VirtIOGPU *g,
                                struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_cmd_submit cs;
    void *buf;
    size_t s;

    VIRTIO_GPU_FILL_CMD(cs);
    trace_virtio_gpu_cmd_ctx_submit(cs.hdr.ctx_id, cs.size);

    if (cs.size > VIRTIO_GPU_MAX_CMD_SUBMIT_SIZE || (cs.size & 3)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: command buffer too large (%u)\n",
                      __func__, cs.size);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
        return;
    }

    buf = g_malloc(cs.size);
    s = iov_to_buf(cmd->elem.out_sg, cmd->elem.out_num,
                   sizeof(cs), buf, cs.size);
    if (s != cs.size) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: size mismatch (%zd/%d)",
                      __func__, s, cs.size);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
        goto out;
    }

    if (virtio_gpu_stats_enabled(g->parent_obj.conf)) {
        g->stats.req_3d++;
        g->stats.bytes_3d += cs.size;
    }

    int ret = virgl_renderer_submit_cmd(buf, cs.hdr.ctx_id, cs.size / 4);
    if (ret) {
        cmd->error = virgl_status_to_virtio_error(ret);
    }

out:
    g_free(buf);
}

static void virgl_cmd_transfer_to_host_2d(VirtIOGPU *g,
                                          struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_transfer_to_host_2d t2d;
    struct virtio_gpu_box box;
    int ret;

    VIRTIO_GPU_FILL_CMD(t2d);
    trace_virtio_gpu_cmd_res_xfer_toh_2d(t2d.resource_id);

    box.x = t2d.r.x;
    box.y = t2d.r.y;
    box.z = 0;
    box.w = t2d.r.width;
    box.h = t2d.r.height;
    box.d = 1;

    ret = virgl_renderer_transfer_write_iov(t2d.resource_id,
                                            0,
                                            0,
                                            0,
                                            0,
                                            (struct virgl_box *)&box,
                                            t2d.offset, NULL, 0);
    if (ret) {
        cmd->error = virgl_status_to_virtio_error(ret);
    }
}

static void virgl_cmd_transfer_to_host_3d(VirtIOGPU *g,
                                          struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_transfer_host_3d t3d;
    int ret;

    VIRTIO_GPU_FILL_CMD(t3d);
    trace_virtio_gpu_cmd_res_xfer_toh_3d(t3d.resource_id);

    ret = virgl_renderer_transfer_write_iov(t3d.resource_id,
                                            t3d.hdr.ctx_id,
                                            t3d.level,
                                            t3d.stride,
                                            t3d.layer_stride,
                                            (struct virgl_box *)&t3d.box,
                                            t3d.offset, NULL, 0);
    if (ret) {
        cmd->error = virgl_status_to_virtio_error(ret);
    }
}

static void
virgl_cmd_transfer_from_host_3d(VirtIOGPU *g,
                                struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_transfer_host_3d tf3d;
    int ret;

    VIRTIO_GPU_FILL_CMD(tf3d);
    trace_virtio_gpu_cmd_res_xfer_fromh_3d(tf3d.resource_id);

    ret = virgl_renderer_transfer_read_iov(tf3d.resource_id,
                                           tf3d.hdr.ctx_id,
                                           tf3d.level,
                                           tf3d.stride,
                                           tf3d.layer_stride,
                                           (struct virgl_box *)&tf3d.box,
                                           tf3d.offset, NULL, 0);
    if (ret) {
        cmd->error = virgl_status_to_virtio_error(ret);
    }
}


static void virgl_resource_attach_backing(VirtIOGPU *g,
                                          struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_resource_attach_backing att_rb;
    struct virtio_gpu_virgl_resource *res;
    struct iovec *res_iovs;
    uint32_t res_niov;
    int ret;

    VIRTIO_GPU_FILL_CMD(att_rb);
    trace_virtio_gpu_cmd_res_back_attach(att_rb.resource_id);

    res = virtio_gpu_virgl_find_resource(g, att_rb.resource_id);
    if (!res) {
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }

    ret = virtio_gpu_create_mapping_iov(g, att_rb.nr_entries, sizeof(att_rb),
                                        cmd, NULL, &res_iovs, &res_niov);
    if (ret != 0) {
        error_report("triton-attach-backing map failed res=%u entries=%u "
                     "ret=%d blob=%u mem=%u flags=0x%x ctx=%u",
                     att_rb.resource_id, att_rb.nr_entries, ret,
                     res->is_blob, res->blob_mem, res->blob_flags,
                     res->blob_ctx_id);
        cmd->error = VIRTIO_GPU_RESP_ERR_UNSPEC;
        return;
    }

    ret = virgl_renderer_resource_attach_iov(att_rb.resource_id,
                                             res_iovs, res_niov);

    if (ret != 0) {
        error_report("triton-attach-backing failed res=%u entries=%u "
                     "iovs=%u ret=%d blob=%u mem=%u flags=0x%x ctx=%u",
                     att_rb.resource_id, att_rb.nr_entries, res_niov, ret,
                     res->is_blob, res->blob_mem, res->blob_flags,
                     res->blob_ctx_id);
        virtio_gpu_cleanup_mapping_iov(g, res_iovs, res_niov);
        cmd->error = virgl_status_to_virtio_error(ret);
    }
}

static void virgl_resource_detach_backing(VirtIOGPU *g,
                                          struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_resource_detach_backing detach_rb;
    struct iovec *res_iovs = NULL;
    int num_iovs = 0;

    VIRTIO_GPU_FILL_CMD(detach_rb);
    trace_virtio_gpu_cmd_res_back_detach(detach_rb.resource_id);

    if (!virtio_gpu_virgl_find_resource(g, detach_rb.resource_id)) {
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }

    virgl_renderer_resource_detach_iov(detach_rb.resource_id,
                                       &res_iovs,
                                       &num_iovs);
    if (res_iovs == NULL || num_iovs == 0) {
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
        return;
    }
    virtio_gpu_cleanup_mapping_iov(g, res_iovs, num_iovs);
}


static void virgl_cmd_ctx_attach_resource(VirtIOGPU *g,
                                          struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_ctx_resource att_res;

    VIRTIO_GPU_FILL_CMD(att_res);
    trace_virtio_gpu_cmd_ctx_res_attach(att_res.hdr.ctx_id,
                                        att_res.resource_id);

    if (!virtio_gpu_virgl_find_resource(g, att_res.resource_id)) {
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }

    virgl_renderer_ctx_attach_resource(att_res.hdr.ctx_id, att_res.resource_id);
}

static void virgl_cmd_ctx_detach_resource(VirtIOGPU *g,
                                          struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_ctx_resource det_res;

    VIRTIO_GPU_FILL_CMD(det_res);
    trace_virtio_gpu_cmd_ctx_res_detach(det_res.hdr.ctx_id,
                                        det_res.resource_id);

    if (!virtio_gpu_virgl_find_resource(g, det_res.resource_id)) {
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }

    virgl_renderer_ctx_detach_resource(det_res.hdr.ctx_id, det_res.resource_id);
}

static void virgl_cmd_get_capset_info(VirtIOGPU *g,
                                      struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_get_capset_info info;
    struct virtio_gpu_resp_capset_info resp;
    static unsigned int triton_capset_info_trace_count;

    VIRTIO_GPU_FILL_CMD(info);

    if (triton_capset_info_trace_count < 32) {
        error_report("triton-capset-info begin index=%u count=%u",
                     info.capset_index, g->capset_ids->len);
    }

    memset(&resp, 0, sizeof(resp));

    if (info.capset_index < g->capset_ids->len) {
        resp.capset_id = g_array_index(g->capset_ids, uint32_t,
                                       info.capset_index);
        virgl_renderer_get_cap_set(resp.capset_id,
                                   &resp.capset_max_version,
                                   &resp.capset_max_size);
    }
    if (triton_capset_info_trace_count < 32) {
        error_report("triton-capset-info end index=%u id=%u ver=%u size=%u",
                     info.capset_index, resp.capset_id,
                     resp.capset_max_version, resp.capset_max_size);
        triton_capset_info_trace_count++;
    }
    resp.hdr.type = VIRTIO_GPU_RESP_OK_CAPSET_INFO;
    virtio_gpu_ctrl_response(g, cmd, &resp.hdr, sizeof(resp));
}

static void virgl_cmd_get_capset(VirtIOGPU *g,
                                 struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_get_capset gc;
    struct virtio_gpu_resp_capset *resp;
    uint32_t max_ver, max_size;
    static unsigned int triton_capset_trace_count;
    VIRTIO_GPU_FILL_CMD(gc);

    if (gc.capset_id == VIRTIO_GPU_CAPSET_NEPTUNE &&
        triton_capset_trace_count < 32) {
        error_report("triton-capset begin id=%u version=%u",
                     gc.capset_id, gc.capset_version);
    }

    virgl_renderer_get_cap_set(gc.capset_id, &max_ver,
                               &max_size);
    if (!max_size) {
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
        return;
    }

    resp = g_malloc0(sizeof(*resp) + max_size);
    resp->hdr.type = VIRTIO_GPU_RESP_OK_CAPSET;
    virgl_renderer_fill_caps(gc.capset_id,
                             gc.capset_version,
                             (void *)resp->capset_data);
    if (gc.capset_id == VIRTIO_GPU_CAPSET_NEPTUNE &&
        triton_capset_trace_count < 32) {
        error_report("triton-capset end id=%u ver=%u size=%u",
                     gc.capset_id, max_ver, max_size);
        triton_capset_trace_count++;
    }
    virtio_gpu_ctrl_response(g, cmd, &resp->hdr, sizeof(*resp) + max_size);
    g_free(resp);
}

#if VIRGL_VERSION_MAJOR >= 1
static void virgl_cmd_resource_create_blob(VirtIOGPU *g,
                                           struct virtio_gpu_ctrl_command *cmd)
{
    struct virgl_renderer_resource_create_blob_args virgl_args = { 0 };
    g_autofree struct virtio_gpu_virgl_resource *res = NULL;
    struct virtio_gpu_resource_create_blob cblob;
    struct virgl_renderer_resource_info info;
    int ret;

    if (!virtio_gpu_blob_enabled(g->parent_obj.conf)) {
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
        return;
    }

    VIRTIO_GPU_FILL_CMD(cblob);
    virtio_gpu_create_blob_bswap(&cblob);
    trace_virtio_gpu_cmd_res_create_blob(cblob.resource_id, cblob.size);

    if (cblob.resource_id == 0 || cblob.size == 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: resource id 0 is not allowed\n",
                      __func__);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }

    if (virtio_gpu_virgl_find_resource(g, cblob.resource_id)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: resource already exists %d\n",
                      __func__, cblob.resource_id);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }

    res = g_new0(struct virtio_gpu_virgl_resource, 1);
    res->base.resource_id = cblob.resource_id;
    res->base.blob_size = cblob.size;
    res->base.dmabuf_fd = -1;
    res->is_blob = true;
    res->blob_mem = cblob.blob_mem;
    res->blob_flags = cblob.blob_flags;
    res->blob_ctx_id = cblob.hdr.ctx_id;

    if (cblob.blob_mem != VIRTIO_GPU_BLOB_MEM_HOST3D) {
        ret = virtio_gpu_create_mapping_iov(g, cblob.nr_entries, sizeof(cblob),
                                            cmd, &res->base.addrs,
                                            &res->base.iov, &res->base.iov_cnt);
        if (ret != 0) {
            cmd->error = VIRTIO_GPU_RESP_ERR_UNSPEC;
            return;
        }
    }

    virgl_args.res_handle = cblob.resource_id;
    virgl_args.ctx_id = cblob.hdr.ctx_id;
    virgl_args.blob_mem = cblob.blob_mem;
    virgl_args.blob_id = cblob.blob_id;
    virgl_args.blob_flags = cblob.blob_flags;
    virgl_args.size = cblob.size;
    virgl_args.iovecs = res->base.iov;
    virgl_args.num_iovs = res->base.iov_cnt;

    ret = virgl_renderer_resource_create_blob(&virgl_args);
    if (ret) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: virgl blob create error: %s\n",
                      __func__, strerror(-ret));
        cmd->error = virgl_status_to_virtio_error(ret);
        virtio_gpu_cleanup_mapping(g, &res->base);
        return;
    }

    ret = virgl_renderer_resource_get_info(cblob.resource_id, &info);
    if (ret) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: resource does not have info %d: %s\n",
                      __func__, cblob.resource_id, strerror(-ret));
        cmd->error = virgl_status_to_virtio_error(ret);
        virtio_gpu_cleanup_mapping(g, &res->base);
        virgl_renderer_resource_unref(cblob.resource_id);
        return;
    }

    res->base.dmabuf_fd = info.fd;

    /* Now live, cleaned up in virtio_gpu_virgl_resource_unref */
    QTAILQ_INSERT_HEAD(&g->reslist, &res->base, next);
    g_steal_pointer(&res);
}

static void virgl_cmd_resource_map_blob(VirtIOGPU *g,
                                        struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_resource_map_blob mblob;
    struct virtio_gpu_virgl_resource *res;
    struct virtio_gpu_resp_map_info resp;
    int ret;

    VIRTIO_GPU_FILL_CMD(mblob);
    virtio_gpu_map_blob_bswap(&mblob);

    res = virtio_gpu_virgl_find_resource(g, mblob.resource_id);
    if (!res) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: resource does not exist %d\n",
                      __func__, mblob.resource_id);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }
    if (res->mr) {
        error_report("triton-map-blob already-mapped res=%u offset=%" PRIu64
                     " mem=%u flags=0x%x ctx=%u",
                     mblob.resource_id, (uint64_t)mblob.offset,
                     res->blob_mem, res->blob_flags, res->blob_ctx_id);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
        return;
    }

    memset(&resp, 0, sizeof(resp));
    resp.hdr.type = VIRTIO_GPU_RESP_OK_MAP_INFO;
    ret = virgl_renderer_resource_get_map_info(mblob.resource_id,
                                               &resp.map_info);
    if (ret) {
        error_report("triton-map-blob map-info failed res=%u offset=%" PRIu64
                     " ret=%d mem=%u flags=0x%x ctx=%u",
                     mblob.resource_id, (uint64_t)mblob.offset, ret,
                     res->blob_mem, res->blob_flags, res->blob_ctx_id);
        cmd->error = virgl_status_to_virtio_error(ret);
        return;
    }

    ret = virtio_gpu_virgl_map_resource_blob(g, res, mblob.offset);
    if (ret) {
        error_report("triton-map-blob map failed res=%u offset=%" PRIu64
                     " map_info=0x%x ret=%d mem=%u flags=0x%x ctx=%u",
                     mblob.resource_id, (uint64_t)mblob.offset,
                     resp.map_info, ret, res->blob_mem, res->blob_flags,
                     res->blob_ctx_id);
        cmd->error = virgl_status_to_virtio_error(ret);
        return;
    }

    virtio_gpu_ctrl_response(g, cmd, &resp.hdr, sizeof(resp));
}

static void virgl_cmd_resource_unmap_blob(VirtIOGPU *g,
                                          struct virtio_gpu_ctrl_command *cmd,
                                          bool *cmd_suspended)
{
    struct virtio_gpu_resource_unmap_blob ublob;
    struct virtio_gpu_virgl_resource *res;
    int ret;

    VIRTIO_GPU_FILL_CMD(ublob);
    virtio_gpu_unmap_blob_bswap(&ublob);

    res = virtio_gpu_virgl_find_resource(g, ublob.resource_id);
    if (!res) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: resource does not exist %d\n",
                      __func__, ublob.resource_id);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }
    if (!res->mr) {
        if (virtio_gpu_neptune_enabled(g->parent_obj.conf)) {
            static unsigned int already_unmapped_trace_count;

            /*
             * Vista's installed legacy Neptune KMD can retire a shared
             * allocation's UNMAP after the owning blob has gone away.  By
             * then the resource id can name an ordinary 3D resource.  That
             * replacement has no blob mapping, so treating the stale UNMAP as
             * an idempotent success cannot change it.  Keep the exception
             * Neptune-only and report enough immutable metadata to distinguish
             * a duplicate blob UNMAP from resource-id reuse.
             */
            if (already_unmapped_trace_count < 32) {
                error_report("triton-unmap-blob already-unmapped res=%u "
                             "blob=%u mem=%u flags=0x%x ctx=%u "
                             "size=%" PRIu64 " width=%u height=%u "
                             "format=0x%x scanout=0x%x",
                             ublob.resource_id, res->is_blob,
                             res->blob_mem, res->blob_flags,
                             res->blob_ctx_id, res->base.blob_size,
                             res->base.width, res->base.height,
                             res->base.format, res->base.scanout_bitmask);
                already_unmapped_trace_count++;
            }
            return;
        }
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
        return;
    }

    ret = virtio_gpu_virgl_unmap_resource_blob(g, res, cmd_suspended);
    if (ret) {
        cmd->error = virgl_status_to_virtio_error(ret);
        return;
    }
}

#if defined(HAVE_VIRGL_RENDERER_NATIVE_SCANOUT)
static void virgl_scanout_native_blob_cleanup(ScanoutTextureNative *native)
{
    assert(native->type == SCANOUT_TEXTURE_NATIVE_TYPE_METAL);
    virgl_renderer_release_handle_for_scanout(VIRGL_NATIVE_HANDLE_METAL_TEXTURE,
                                              native->handle);
}

static bool virgl_scanout_native_blob(VirtIOGPU *g,
                                      struct virtio_gpu_set_scanout_blob *ss)
{
    struct virtio_gpu_scanout *scanout = &g->parent_obj.scanout[ss->scanout_id];
    enum virgl_renderer_native_handle_type type;
    virgl_renderer_native_handle handle;
    ScanoutTextureNative native;

    type = virgl_renderer_create_handle_for_scanout(ss->resource_id,
                                                    ss->width,
                                                    ss->height,
                                                    ss->format,
                                                    ss->padding,
                                                    ss->strides[0],
                                                    ss->offsets[0],
                                                    &handle);
#ifdef CONFIG_METAL
    if (type == VIRGL_NATIVE_HANDLE_METAL_TEXTURE) {
        native = (ScanoutTextureNative){
            .type = SCANOUT_TEXTURE_NATIVE_TYPE_METAL,
            .handle = handle,
        };
        qemu_console_resize(scanout->con,
                            ss->r.width, ss->r.height);
        dpy_gl_scanout_texture(
            scanout->con, 0,
            false,
            ss->width, ss->height,
            ss->r.x, ss->r.y, ss->r.width, ss->r.height,
            native, virgl_scanout_native_blob_cleanup);
        return true;
    }
#endif

    /* don't leak memory if handle type is unknown */
    if (type != VIRGL_NATIVE_HANDLE_NONE) {
        virgl_renderer_release_handle_for_scanout(type, handle);
    }

    return false;
}
#endif

static void virgl_cmd_set_scanout_blob(VirtIOGPU *g,
                                       struct virtio_gpu_ctrl_command *cmd)
{
    struct virtio_gpu_framebuffer fb = { 0 };
    struct virtio_gpu_virgl_resource *res;
    struct virtio_gpu_set_scanout_blob ss;

    VIRTIO_GPU_FILL_CMD(ss);
    virtio_gpu_scanout_blob_bswap(&ss);
    trace_virtio_gpu_cmd_set_scanout_blob(ss.scanout_id, ss.resource_id,
                                          ss.r.width, ss.r.height, ss.r.x,
                                          ss.r.y);

    if (ss.scanout_id >= g->parent_obj.conf.max_outputs) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: illegal scanout id specified %d",
                      __func__, ss.scanout_id);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID;
        return;
    }

    if (ss.resource_id == 0) {
        virtio_gpu_disable_scanout(g, ss.scanout_id);
        return;
    }

    res = virtio_gpu_virgl_find_resource(g, ss.resource_id);
    if (!res) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: resource does not exist %d\n",
                      __func__, ss.resource_id);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
        return;
    }

    if (!virtio_gpu_scanout_blob_to_fb(&fb, &ss, res->base.blob_size)) {
        static unsigned int invalid_fb_count;
        if (invalid_fb_count++ < 8) {
            error_report("triton-scanout-blob invalid-fb res=%u blob=%u "
                         "size=%" PRIu64 " image=%ux%u format=%u "
                         "rect=%u,%u,%u,%u stride=%u offset=%u",
                         ss.resource_id, res->is_blob, res->base.blob_size,
                         ss.width, ss.height, ss.format, ss.r.x, ss.r.y,
                         ss.r.width, ss.r.height, ss.strides[0], ss.offsets[0]);
        }
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
        return;
    }

#if defined(HAVE_VIRGL_RENDERER_NATIVE_SCANOUT)
    if (!virtio_gpu_neptune_enabled(g->parent_obj.conf) &&
        virgl_scanout_native_blob(g, &ss)) {
        virtio_gpu_update_scanout(g, ss.scanout_id, &res->base, &fb, &ss.r);
        g->parent_obj.enable = 1;
        return;
    }
#endif

    if (virtio_gpu_neptune_enabled(g->parent_obj.conf)) {
        if (!virtio_gpu_neptune_set_scanout(g, ss.scanout_id, &fb, &ss.r)) {
            cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
            return;
        }
        virtio_gpu_update_scanout(g, ss.scanout_id, &res->base, &fb, &ss.r);
        g->parent_obj.enable = 1;
        return;
    }

    if (res->base.dmabuf_fd < 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: resource not backed by dmabuf %d\n",
                      __func__, ss.resource_id);
        cmd->error = VIRTIO_GPU_RESP_ERR_UNSPEC;
        return;
    }

    if (virtio_gpu_update_dmabuf(g, ss.scanout_id, &res->base, &fb, &ss.r)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: failed to update dmabuf\n",
                      __func__);
        cmd->error = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
        return;
    }

    virtio_gpu_update_scanout(g, ss.scanout_id, &res->base, &fb, &ss.r);
    g->parent_obj.enable = 1;
}
#endif

void virtio_gpu_virgl_process_cmd(VirtIOGPU *g,
                                      struct virtio_gpu_ctrl_command *cmd)
{
    bool cmd_suspended = false;

    VIRTIO_GPU_FILL_CMD(cmd->cmd_hdr);
    triton_trace_record(TT_HOST_COMMAND_BEGIN, cmd->cmd_hdr.fence_id,
        cmd->cmd_hdr.ctx_id, cmd->cmd_hdr.type, cmd->cmd_hdr.flags, 0);

    cmd->context_fence =
        virtio_gpu_virgl_legacy_neptune_context_fence(cmd);

    if (VIRTIO_GPU_GL(g)->context_failure) {
        cmd->error = VIRTIO_GPU_RESP_ERR_UNSPEC;
        goto respond;
    }
    virgl_renderer_force_ctx_0();
    switch (cmd->cmd_hdr.type) {
    case VIRTIO_GPU_CMD_TRITON_PRESENT_BLT:
        virgl_cmd_triton_present_blt(g, cmd);
        break;
    case VIRTIO_GPU_CMD_CTX_CREATE:
        virgl_cmd_context_create(g, cmd);
        break;
    case VIRTIO_GPU_CMD_CTX_DESTROY:
        virgl_cmd_context_destroy(g, cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_CREATE_2D:
        virgl_cmd_create_resource_2d(g, cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_CREATE_3D:
        virgl_cmd_create_resource_3d(g, cmd);
        break;
    case VIRTIO_GPU_CMD_SUBMIT_3D:
        virgl_cmd_submit_3d(g, cmd);
        break;
    case VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D:
        virgl_cmd_transfer_to_host_2d(g, cmd);
        break;
    case VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D:
        virgl_cmd_transfer_to_host_3d(g, cmd);
        break;
    case VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D:
        virgl_cmd_transfer_from_host_3d(g, cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING:
        virgl_resource_attach_backing(g, cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING:
        virgl_resource_detach_backing(g, cmd);
        break;
    case VIRTIO_GPU_CMD_SET_SCANOUT:
        virgl_cmd_set_scanout(g, cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_FLUSH:
        virgl_cmd_resource_flush(g, cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_UNREF:
        virgl_cmd_resource_unref(g, cmd, &cmd_suspended);
        break;
    case VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE:
        /* TODO add security */
        virgl_cmd_ctx_attach_resource(g, cmd);
        break;
    case VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE:
        /* TODO add security */
        virgl_cmd_ctx_detach_resource(g, cmd);
        break;
    case VIRTIO_GPU_CMD_GET_CAPSET_INFO:
        virgl_cmd_get_capset_info(g, cmd);
        break;
    case VIRTIO_GPU_CMD_GET_CAPSET:
        virgl_cmd_get_capset(g, cmd);
        break;
    case VIRTIO_GPU_CMD_GET_DISPLAY_INFO:
        virtio_gpu_get_display_info(g, cmd);
        break;
    case VIRTIO_GPU_CMD_GET_EDID:
        virtio_gpu_get_edid(g, cmd);
        break;
#if VIRGL_VERSION_MAJOR >= 1
    case VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB:
        virgl_cmd_resource_create_blob(g, cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB:
        virgl_cmd_resource_map_blob(g, cmd);
        break;
    case VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB:
        virgl_cmd_resource_unmap_blob(g, cmd, &cmd_suspended);
        break;
    case VIRTIO_GPU_CMD_SET_SCANOUT_BLOB:
        virgl_cmd_set_scanout_blob(g, cmd);
        break;
#endif
    default:
        cmd->error = VIRTIO_GPU_RESP_ERR_UNSPEC;
        break;
    }

    if (VIRTIO_GPU_GL(g)->context_failure) {
        cmd->error = VIRTIO_GPU_RESP_ERR_UNSPEC;
    }
respond:
    cmd->suspended = cmd_suspended;
    triton_trace_record(TT_HOST_COMMAND_END, cmd->cmd_hdr.fence_id,
        cmd->cmd_hdr.ctx_id, cmd->error, cmd_suspended, cmd->finished);

    if (cmd_suspended || cmd->finished) {
        return;
    }
    if (cmd->error) {
        fprintf(stderr, "%s: ctrl 0x%x, error 0x%x\n", __func__,
                cmd->cmd_hdr.type, cmd->error);
        virtio_gpu_ctrl_response_nodata(g, cmd, cmd->error);
        return;
    }
    if (!(cmd->cmd_hdr.flags & VIRTIO_GPU_FLAG_FENCE)) {
        virtio_gpu_ctrl_response_nodata(g, cmd, VIRTIO_GPU_RESP_OK_NODATA);
        return;
    }

    trace_virtio_gpu_fence_ctrl(cmd->cmd_hdr.fence_id, cmd->cmd_hdr.type);

    /* RESOURCE_UNREF removes the resource from QEMU and virglrenderer before
     * this point.  Renderer-side object deletion owns any deferred GPU
     * lifetime.  A legacy Neptune KMD uses the fence only to reclaim its
     * resource id, so the successful control return is the exact retirement
     * point and must not depend on the unrelated GL context-zero timeline. */
    if (virtio_gpu_neptune_enabled(g->parent_obj.conf) &&
        cmd->cmd_hdr.type == VIRTIO_GPU_CMD_RESOURCE_UNREF &&
        cmd->cmd_hdr.ctx_id == 0) {
        virtio_gpu_ctrl_response_nodata(g, cmd,
                                        VIRTIO_GPU_RESP_OK_NODATA);
        return;
    }
#if VIRGL_VERSION_MAJOR >= 1
    if ((cmd->cmd_hdr.flags & VIRTIO_GPU_FLAG_INFO_RING_IDX) ||
        cmd->context_fence) {
        static unsigned int legacy_fence_trace_count;
        uint32_t ring_idx =
            (cmd->cmd_hdr.flags & VIRTIO_GPU_FLAG_INFO_RING_IDX)
                ? cmd->cmd_hdr.ring_idx
                : 0;

        /* Context destruction is synchronous and removes the context.  Its
         * successful return is therefore the retirement point; asking the
         * removed context to create another fence would be invalid. */
        if (cmd->cmd_hdr.type == VIRTIO_GPU_CMD_CTX_DESTROY) {
            virtio_gpu_ctrl_response_nodata(g, cmd,
                                            VIRTIO_GPU_RESP_OK_NODATA);
            return;
        }
        if (cmd->context_fence && legacy_fence_trace_count < 32) {
            error_report("triton-fence-route legacy-neptune ctx=%u type=0x%x "
                         "ring=0 fence=%" PRIu64,
                         cmd->cmd_hdr.ctx_id, cmd->cmd_hdr.type,
                         (uint64_t)cmd->cmd_hdr.fence_id);
            legacy_fence_trace_count++;
        }
        int ret = virgl_renderer_context_create_fence(cmd->cmd_hdr.ctx_id,
                                            VIRGL_RENDERER_FENCE_FLAG_MERGEABLE,
                                            ring_idx,
                                            cmd->cmd_hdr.fence_id);
        if (ret) {
            fprintf(stderr,
                    "%s: create_fence failed (%d) for dead ctx %u; "
                    "failing fence %" PRIu64 "\n",
                    __func__, ret, cmd->cmd_hdr.ctx_id,
                    (uint64_t)cmd->cmd_hdr.fence_id);
            virtio_gpu_ctrl_response_nodata(
                g, cmd, virgl_status_to_virtio_error(ret));
        }
        return;
    }
#endif
    static unsigned int generic_fence_trace_count;
    if (generic_fence_trace_count < 32) {
        error_report("triton-fence-route generic ctx=%u type=0x%x "
                     "flags=0x%x fence=%" PRIu64,
                     cmd->cmd_hdr.ctx_id, cmd->cmd_hdr.type,
                     cmd->cmd_hdr.flags,
                     (uint64_t)cmd->cmd_hdr.fence_id);
        generic_fence_trace_count++;
    }
    int ret = virgl_renderer_create_fence(cmd->cmd_hdr.fence_id,
                                          cmd->cmd_hdr.type);
    if (ret) {
        virtio_gpu_ctrl_response_nodata(g, cmd,
                                        virgl_status_to_virtio_error(ret));
    }
}

static void virgl_write_fence(void *opaque, uint32_t fence)
{
    VirtIOGPU *g = opaque;
    struct virtio_gpu_ctrl_command *cmd, *tmp;

    QTAILQ_FOREACH_SAFE(cmd, &g->fenceq, next, tmp) {
        /*
         * the guest can end up emitting fences out of order
         * so we should check all fenced cmds not just the first one.
         */
#if VIRGL_VERSION_MAJOR >= 1
        if ((cmd->cmd_hdr.flags & VIRTIO_GPU_FLAG_INFO_RING_IDX) ||
            cmd->context_fence) {
            continue;
        }
#endif
        if (cmd->cmd_hdr.fence_id > fence) {
            continue;
        }
        trace_virtio_gpu_fence_resp(cmd->cmd_hdr.fence_id);
        triton_trace_record(TT_HOST_FENCE, cmd->cmd_hdr.fence_id,
            cmd->cmd_hdr.ctx_id, 0, 0, 0);
        virtio_gpu_ctrl_response_nodata(g, cmd, VIRTIO_GPU_RESP_OK_NODATA);
        QTAILQ_REMOVE(&g->fenceq, cmd, next);
        g_free(cmd);
        g->inflight--;
        if (virtio_gpu_stats_enabled(g->parent_obj.conf)) {
            trace_virtio_gpu_dec_inflight_fences(g->inflight);
        }
    }
}

#if VIRGL_VERSION_MAJOR >= 1
static void virgl_write_context_fence(void *opaque, uint32_t ctx_id,
                                      uint32_t ring_idx, uint64_t fence_id) {
    VirtIOGPU *g = opaque;
    struct virtio_gpu_ctrl_command *cmd, *tmp;

    QTAILQ_FOREACH_SAFE(cmd, &g->fenceq, next, tmp) {
        if (((cmd->cmd_hdr.flags & VIRTIO_GPU_FLAG_INFO_RING_IDX) ||
             cmd->context_fence) &&
            cmd->cmd_hdr.ctx_id == ctx_id && cmd->cmd_hdr.ring_idx == ring_idx &&
            cmd->cmd_hdr.fence_id <= fence_id) {
            trace_virtio_gpu_fence_resp(cmd->cmd_hdr.fence_id);
            triton_trace_record(TT_HOST_FENCE, cmd->cmd_hdr.fence_id,
                cmd->cmd_hdr.ctx_id, 0, 0, 0);
            virtio_gpu_ctrl_response_nodata(g, cmd, VIRTIO_GPU_RESP_OK_NODATA);
            QTAILQ_REMOVE(&g->fenceq, cmd, next);
            g_free(cmd);
            g->inflight--;
            if (virtio_gpu_stats_enabled(g->parent_obj.conf)) {
                trace_virtio_gpu_dec_inflight_fences(g->inflight);
            }
        }
    }
}
#endif

static virgl_renderer_gl_context
virgl_create_context(void *opaque, int scanout_idx,
                     struct virgl_renderer_gl_ctx_param *params)
{
    VirtIOGPU *g = opaque;
    QEMUGLContext ctx;
    QEMUGLParams qparams;

    qparams.major_ver = params->major_ver;
    qparams.minor_ver = params->minor_ver;

    ctx = dpy_gl_ctx_create(g->parent_obj.scanout[scanout_idx].con, &qparams);
    return (virgl_renderer_gl_context)ctx;
}

static void virgl_destroy_context(void *opaque, virgl_renderer_gl_context ctx)
{
    VirtIOGPU *g = opaque;
    QEMUGLContext qctx = (QEMUGLContext)ctx;

    dpy_gl_ctx_destroy(g->parent_obj.scanout[0].con, qctx);
}

static int virgl_make_context_current(void *opaque, int scanout_idx,
                                      virgl_renderer_gl_context ctx)
{
    VirtIOGPU *g = opaque;
    QEMUGLContext qctx = (QEMUGLContext)ctx;

    return dpy_gl_ctx_make_current(g->parent_obj.scanout[scanout_idx].con,
                                   qctx);
}

static struct virgl_renderer_callbacks virtio_gpu_3d_cbs = {
#if VIRGL_VERSION_MAJOR >= 1
    .version             = 3,
#else
    .version             = 1,
#endif
    .write_fence         = virgl_write_fence,
    .create_gl_context   = virgl_create_context,
    .destroy_gl_context  = virgl_destroy_context,
    .make_current        = virgl_make_context_current,
#if VIRGL_VERSION_MAJOR >= 1
    .write_context_fence = virgl_write_context_fence,
#endif
};

static void virtio_gpu_print_stats(void *opaque)
{
    VirtIOGPU *g = opaque;
    VirtIOGPUGL *gl = VIRTIO_GPU_GL(g);

    if (g->stats.requests) {
        fprintf(stderr, "stats: vq req %4d, %3d -- 3D %4d (%5d)\n",
                g->stats.requests,
                g->stats.max_inflight,
                g->stats.req_3d,
                g->stats.bytes_3d);
        g->stats.requests     = 0;
        g->stats.max_inflight = 0;
        g->stats.req_3d       = 0;
        g->stats.bytes_3d     = 0;
    } else {
        fprintf(stderr, "stats: idle\r");
    }
    timer_mod(gl->print_stats, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1000);
}

static void virtio_gpu_fence_poll(void *opaque)
{
    VirtIOGPU *g = opaque;
    VirtIOGPUGL *gl = VIRTIO_GPU_GL(g);

    if (gl->context_failure) {
        return;
    }
    virgl_renderer_poll();
    virtio_gpu_process_cmdq(g);
    if (!gl->context_failure &&
        (!QTAILQ_EMPTY(&g->cmdq) || !QTAILQ_EMPTY(&g->fenceq))) {
        /*
         * Per-context eventfds wake Neptune promptly. Keep this fallback for
         * contexts without an eventfd and other pending command work. It
         * only re-arms while commands or fences remain outstanding.
         */
        timer_mod(gl->fence_poll, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1);
    }
}

void virtio_gpu_virgl_fence_poll(VirtIOGPU *g)
{
    virtio_gpu_fence_poll(g);
}

bool virtio_gpu_virgl_readback(VirtIOGPUBase *base, QemuConsole *con,
                                Error **errp)
{
#ifdef CONFIG_LINUX
    VirtIOGPU *g = VIRTIO_GPU(base);
    VirtIOGPUGL *gl = VIRTIO_GPU_GL(g);
    struct virtio_gpu_scanout *scanout = NULL;
    unsigned index;
    GLint old_read, old_pack, value[7];
    const GLenum pack[] = { GL_PACK_ALIGNMENT, GL_PACK_ROW_LENGTH,
        GL_PACK_SKIP_ROWS, GL_PACK_SKIP_PIXELS, GL_PACK_SWAP_BYTES,
        GL_PACK_IMAGE_HEIGHT, GL_PACK_SKIP_IMAGES };
    GLuint fbo;
    uint8_t *pixels;
    DisplaySurface *surface;
    unsigned width, height;
    bool desktop = qemu_egl_mode == DISPLAY_GL_MODE_CORE;
    bool ok;

    if (gl->context_failure) {
        error_setg(errp, "Neptune renderer stopped after EGL context failure");
        return false;
    }
    for (index = 0; index < base->conf.max_outputs; index++) {
        if (base->scanout[index].con == con) {
            scanout = &base->scanout[index];
            break;
        }
    }
    if (!scanout || !scanout->resource_id || !gl->scanout_gpu[index]) {
        return true;
    }
    surface = qemu_console_surface(con);
    width = gl->scanout_texture_width[index];
    height = gl->scanout_texture_height[index];
    if (!surface || width != surface_width(surface) ||
        height != surface_height(surface) ||
        surface_bytes_per_pixel(surface) != 4 ||
        (uint64_t)surface_stride(surface) < (uint64_t)width * 4 ||
        (surface_format(surface) != PIXMAN_LE_x8r8g8b8 &&
         surface_format(surface) != PIXMAN_LE_a8r8g8b8 &&
         surface_format(surface) != PIXMAN_LE_x8b8g8r8 &&
         surface_format(surface) != PIXMAN_LE_a8b8g8r8)) {
        error_setg(errp, "Neptune scanout is changing dimensions");
        return false;
    }
    virgl_renderer_force_ctx_0();
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_read);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &old_pack);
    for (unsigned j = 0; j < (desktop ? 7 : 4); j++) {
        glGetIntegerv(pack[j], &value[j]);
        glPixelStorei(pack[j], j == 0 ? 1 : 0);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, gl->scanout_texture[index], 0);
    pixels = g_malloc((size_t)width * height * 4);
    ok = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) ==
         GL_FRAMEBUFFER_COMPLETE;
    if (ok) {
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        ok = glGetError() == GL_NO_ERROR;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, old_read);
    glDeleteFramebuffers(1, &fbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, old_pack);
    for (unsigned j = 0; j < (desktop ? 7 : 4); j++) {
        glPixelStorei(pack[j], value[j]);
    }
    if (ok) {
        bool bgra = surface_format(surface) == PIXMAN_LE_x8r8g8b8 ||
                    surface_format(surface) == PIXMAN_LE_a8r8g8b8;
        for (unsigned y = 0; y < height; y++) {
            uint8_t *out = surface_data(surface) + y * surface_stride(surface);
            const uint8_t *in = pixels + (size_t)y * width * 4;
            for (unsigned x = 0; x < width; x++) {
                out[x * 4] = in[x * 4 + (bgra ? 2 : 0)];
                out[x * 4 + 1] = in[x * 4 + 1];
                out[x * 4 + 2] = in[x * 4 + (bgra ? 0 : 2)];
                out[x * 4 + 3] = 255;
            }
        }
    } else {
        error_setg(errp, "Neptune scanout readback failed");
    }
    g_free(pixels);
    return ok;
#else
    return true;
#endif
}

bool virtio_gpu_virgl_reset_scanout(VirtIOGPU *g)
{
    VirtIOGPUGL *gl = VIRTIO_GPU_GL(g);
    int i;

    if (gl->context_failure) {
        return false;
    }
    for (i = 0; i < g->parent_obj.conf.max_outputs; i++) {
        virtio_gpu_disable_scanout(g, i);
    }
    virgl_renderer_force_ctx_0();
    for (i = 0; i < g->parent_obj.conf.max_outputs; i++) {
        if (!virtio_gpu_neptune_copy_destroy(g,
                                            &gl->scanout_external_copy[i]) ||
            !virtio_gpu_neptune_copy_destroy(g, &gl->scanout_pending_copy[i])) {
            return false;
        }
        if (gl->scanout_pending_texture[i]) {
            glDeleteTextures(1, &gl->scanout_pending_texture[i]);
            gl->scanout_pending_texture[i] = 0;
        }
        if (gl->scanout_texture[i]) {
            glDeleteTextures(1, &gl->scanout_texture[i]);
            gl->scanout_texture[i] = 0;
        }
        gl->scanout_gpu[i] = false;
    }
    return true;
}

bool virtio_gpu_virgl_destroy_resource_copies(VirtIOGPU *g)
{
    struct virtio_gpu_simple_resource *res;

    if (VIRTIO_GPU_GL(g)->context_failure) {
        return false;
    }
    QTAILQ_FOREACH(res, &g->reslist, next) {
        struct virtio_gpu_virgl_resource *vres =
            container_of(res, struct virtio_gpu_virgl_resource, base);

        if (!virtio_gpu_neptune_copy_destroy(g, &vres->external_copy)) {
            return false;
        }
    }
    return true;
}

void virtio_gpu_virgl_reset(VirtIOGPU *g)
{
    virtio_gpu_virgl_clear_fence_watches(g);
    /* Scanout reset callbacks may have released the EGL context again. */
    virgl_renderer_force_ctx_0();
    virgl_renderer_reset();
}

int virtio_gpu_virgl_init(VirtIOGPU *g)
{
    int ret;
    uint32_t flags = 0;
    VirtIOGPUGL *gl = VIRTIO_GPU_GL(g);

#if VIRGL_RENDERER_CALLBACKS_VERSION >= 4
    if (qemu_egl_display) {
        virtio_gpu_3d_cbs.version = 4;
        virtio_gpu_3d_cbs.get_egl_display = virgl_get_egl_display;
    }
#endif
    if (qemu_egl_angle_native_device) {
#if defined(VIRGL_RENDERER_NATIVE_SHARE_TEXTURE)
        flags |= VIRGL_RENDERER_NATIVE_SHARE_TEXTURE;
#elif defined(VIRGL_RENDERER_D3D11_SHARE_TEXTURE) && defined(WIN32)
        flags |= VIRGL_RENDERER_D3D11_SHARE_TEXTURE;
#endif
    }
#if VIRGL_VERSION_MAJOR >= 1
    if (virtio_gpu_venus_enabled(g->parent_obj.conf)) {
        flags |= VIRGL_RENDERER_VENUS;
        flags |= VIRGL_RENDERER_RENDER_SERVER;
    }
#endif
#ifdef VIRGL_RENDERER_NEPTUNE
    if (virtio_gpu_neptune_enabled(g->parent_obj.conf)) {
        flags |= VIRGL_RENDERER_NEPTUNE;
        flags |= VIRGL_RENDERER_RENDER_SERVER;
        flags |= VIRGL_RENDERER_THREAD_SYNC;
    }
#endif

    ret = virgl_renderer_init(g, flags, &virtio_gpu_3d_cbs);
    if (ret != 0) {
        error_report("virgl could not be initialized: %d", ret);
        return ret;
    }

    if (!gl->fence_poll) {
        gl->fence_poll = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                      virtio_gpu_fence_poll, g);
    }

    if (virtio_gpu_stats_enabled(g->parent_obj.conf)) {
        if (!gl->print_stats) {
            gl->print_stats = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                           virtio_gpu_print_stats, g);
        }
        timer_mod(gl->print_stats,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1000);
    }

#if VIRGL_VERSION_MAJOR >= 1
    if (!gl->cmdq_resume_bh) {
        gl->cmdq_resume_bh =
            virtio_bh_io_new_guarded(DEVICE(g),
                                   virtio_gpu_virgl_resume_cmdq_bh, g);
    }
#endif

    return 0;
}

static void virtio_gpu_virgl_add_capset(GArray *capset_ids, uint32_t capset_id)
{
    g_array_append_val(capset_ids, capset_id);
}

GArray *virtio_gpu_virgl_get_capsets(VirtIOGPU *g)
{
    uint32_t capset_max_ver, capset_max_size;
    GArray *capset_ids;

    capset_ids = g_array_new(false, false, sizeof(uint32_t));

    /* VIRGL is always supported. */
    virtio_gpu_virgl_add_capset(capset_ids, VIRTIO_GPU_CAPSET_VIRGL);

    virgl_renderer_get_cap_set(VIRTIO_GPU_CAPSET_VIRGL2,
                               &capset_max_ver,
                               &capset_max_size);
    if (capset_max_ver) {
        virtio_gpu_virgl_add_capset(capset_ids, VIRTIO_GPU_CAPSET_VIRGL2);
    }

    if (virtio_gpu_venus_enabled(g->parent_obj.conf)) {
        virgl_renderer_get_cap_set(VIRTIO_GPU_CAPSET_VENUS,
                                   &capset_max_ver,
                                   &capset_max_size);
        if (capset_max_size) {
            virtio_gpu_virgl_add_capset(capset_ids, VIRTIO_GPU_CAPSET_VENUS);
        }
    }

#ifdef VIRGL_RENDERER_NEPTUNE
    if (virtio_gpu_neptune_enabled(g->parent_obj.conf) &&
        VIRTIO_GPU_GL(g)->neptune_capset) {
        virgl_renderer_get_cap_set(VIRTIO_GPU_CAPSET_NEPTUNE,
                                   &capset_max_ver,
                                   &capset_max_size);
        if (capset_max_size) {
            virtio_gpu_virgl_add_capset(capset_ids, VIRTIO_GPU_CAPSET_NEPTUNE);
        }
    }
#endif

    return capset_ids;
}
