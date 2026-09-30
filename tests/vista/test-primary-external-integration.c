/* SPDX-License-Identifier: GPL-2.0-or-later
 * Real GL/EGL calls and exact extracted production functions. Only QEMU device
 * bookkeeping and the virgl metadata lookup are simulated. No pixel API mocks.
 */
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <epoxy/gl.h>
#include <epoxy/egl.h>
#include "standard-headers/drm/drm_fourcc.h"
#include "test-primary-external-integration.h"
#define CONFIG_LINUX 1
#define RUN_STATE_INTERNAL_ERROR 1
#define DISPLAY_GL_MODE_CORE 1
#define PIXMAN_LE_x8r8g8b8 1
#define PIXMAN_LE_a8r8g8b8 2
#define PIXMAN_LE_x8b8g8r8 3
#define PIXMAN_LE_a8b8g8r8 4
#define VIRGL_RENDERER_STRUCTURE_TYPE_EXPORT_QUERY 1
static const int qemu_egl_mode = DISPLAY_GL_MODE_CORE;
static EGLDisplay qemu_egl_display;
struct VirtIOGPUExternalContext;
typedef struct VirtIOGPU {
    struct { unsigned renderer_blocked; } parent_obj;
    struct VirtIOGPUExternalContext *context_failure;
    void *fence_poll, *cmdq_resume_bh;
} VirtIOGPU;
typedef VirtIOGPU VirtIOGPUGL;
#define VIRTIO_GPU_GL(g) (g)
struct virtio_gpu_rect { uint32_t x, y, width, height; };
struct virtio_gpu_framebuffer { uint32_t format, bytes_pp, width, height, stride, offset; };
typedef struct DisplaySurface { uint8_t *data; uint32_t width,height,stride,format; } DisplaySurface;
static uint32_t surface_width(DisplaySurface *s) { return s->width; }
static uint32_t surface_height(DisplaySurface *s) { return s->height; }
static uint32_t surface_stride(DisplaySurface *s) { return s->stride; }
static uint32_t surface_format(DisplaySurface *s) { return s->format; }
static uint8_t *surface_data(DisplaySurface *s) { return s->data; }
struct virgl_renderer_resource_info { uint32_t width,height; };
struct virgl_renderer_export_query {
    struct { uint32_t stype,size; } hdr;
    uint32_t in_resource_id;
    uint32_t out_num_fds,out_fourcc,out_strides[4],out_offsets[4];
    uint64_t out_modifier;
};
static struct { int fd; uint32_t width,height,fourcc,stride,offset; uint64_t modifier; } metadata;
static bool qemu_in_main_thread(void) { return true; }
static void error_report(const char *format, ...) { va_list args;va_start(args,format);vfprintf(stderr,format,args);fputc('\n',stderr);va_end(args); }
static void timer_del(void *timer) { abort(); }
static void qemu_bh_cancel(void *bh) { abort(); }
static void virtio_gpu_virgl_clear_fence_watches(VirtIOGPU *g) { }
static void qemu_system_vmstop_request(int state) { assert(state==RUN_STATE_INTERNAL_ERROR); }
static int virgl_renderer_resource_get_info(uint32_t id,struct virgl_renderer_resource_info *info) { assert(id==1);*info=(struct virgl_renderer_resource_info){metadata.width,metadata.height};return 0; }
static int virgl_renderer_execute(void *data,uint32_t size) {
    struct virgl_renderer_export_query *query=data;
    assert(size==sizeof(*query)&&query->in_resource_id==1);
    query->out_num_fds=1;query->out_fourcc=metadata.fourcc;
    query->out_strides[0]=metadata.stride;query->out_offsets[0]=metadata.offset;
    query->out_modifier=metadata.modifier;
    return 0;
}
#include "production-transfer.inc"
struct Bridge { VirtIOGPU g; struct VirtIOGPUExternalCopy *cache; GLuint target; uint32_t width,height; unsigned calls; };
void bridge_source(int fd,uint32_t width,uint32_t height,uint32_t fourcc,uint32_t stride,uint32_t offset,uint64_t modifier) {
    metadata.fd=fd;metadata.width=width;metadata.height=height;metadata.fourcc=fourcc;
    metadata.stride=stride;metadata.offset=offset;metadata.modifier=modifier;
    qemu_egl_display=eglGetCurrentDisplay();
}
int bridge_external_only(bool *external) { return virtio_gpu_neptune_external_only(metadata.fourcc,metadata.modifier,external); }
QemuEGLExternalCopy *bridge_new(GLuint target,int width,int height,QemuEGLExternalCopyError *error) {
    memset(error,0,sizeof(*error));error->operation="bridge_new_deferred_cache";
    struct Bridge *b=g_new0(struct Bridge,1);b->target=target;b->width=width;b->height=height;
    return (QemuEGLExternalCopy *)b;
}
void bridge_retarget(QemuEGLExternalCopy *copy,GLuint target,int width,int height) {
    struct Bridge *b=(struct Bridge *)copy;b->target=target;b->width=width;b->height=height;
}
bool bridge_run(QemuEGLExternalCopy *copy,EGLImageKHR ignored,int sw,int sh,int sx,int sy,int dx,int dy,int width,int height,bool flip,bool opaque,QemuEGLExternalCopyError *error) {
    struct Bridge *b=(struct Bridge *)copy;
    struct virtio_gpu_rect source={sx,sy,width,height};
    bool bgra=metadata.fourcc==DRM_FORMAT_ARGB8888||metadata.fourcc==DRM_FORMAT_XRGB8888;
    struct virtio_gpu_framebuffer fb={bgra?PIXMAN_LE_a8r8g8b8:PIXMAN_LE_a8b8g8r8,4,sw,sh,metadata.stride,
        metadata.offset+(uint32_t)sy*metadata.stride+(uint32_t)sx*4};
    bool required=false;
    memset(error,0,sizeof(*error));error->operation="production_transfer_dmabuf";
    void *previous=b->cache;
    int ret=virtio_gpu_neptune_transfer_dmabuf(&b->g,metadata.fd,1,&fb,&source,
        sx,sy,width,height,NULL,b->target,dx,flip?dy+height:dy,flip,&b->cache,
        b->width,b->height,opaque,&required);
    error->context_restore_failed=b->g.context_failure!=NULL;
    snprintf(error->detail,sizeof(error->detail),"ret=%d required=%d",ret,required);
    printf("{\"stage\":\"production_transfer\",\"status\":\"%s\",\"ret\":%d,\"gpu_required\":%s,\"helper_cache_present\":%s,\"helper_cache_reused\":%s,\"call\":%u}\n",
        ret?"failed":"passed",ret,required?"true":"false",b->cache?"true":"false",previous&&previous==b->cache?"true":"false",++b->calls);
    fflush(stdout);
    return ret==0;
}
bool bridge_destroy(QemuEGLExternalCopy **copy,QemuEGLExternalCopyError *error) {
    struct Bridge *b=(struct Bridge *)*copy;
    memset(error,0,sizeof(*error));error->operation="production_copy_destroy";
    bool ok=virtio_gpu_neptune_copy_destroy(&b->g,&b->cache);
    error->context_restore_failed=b->g.context_failure!=NULL;
    if(!b->cache&&!b->g.context_failure){g_free(b);*copy=NULL;}
    return ok;
}
