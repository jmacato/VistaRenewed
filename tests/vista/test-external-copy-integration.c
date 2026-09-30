/* CPU failure injection for extracted QEMU external-copy integration. */
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <epoxy/egl.h>
#include <epoxy/gl.h>
#include "ui/egl-external-copy.h"
#include "standard-headers/drm/drm_fourcc.h"
#define CONFIG_LINUX 1
#define VIRTIO_GPU_MAX_SCANOUTS 2
#define RUN_STATE_INTERNAL_ERROR 1
#define PIXMAN_LE_x8r8g8b8 1
#define PIXMAN_LE_a8r8g8b8 2
#define PIXMAN_LE_x8b8g8r8 3
#define PIXMAN_LE_a8b8g8r8 4
#define NO_NATIVE_TEXTURE 0
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "CHECK FAILED: %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
struct virtio_gpu_rect { uint32_t x,y,width,height; };
struct virtio_gpu_framebuffer { uint32_t format,bytes_pp,width,height,stride,offset; };
struct virtio_gpu_scanout { void *con, *ds; struct virtio_gpu_framebuffer fb; };
struct virtio_gpu_virgl_resource { struct { unsigned resource_id; } base; bool is_blob; };
struct VirtIOGPUExternalCopy;
struct VirtIOGPUExternalContext;
typedef struct VirtIOGPU {
    struct { unsigned renderer_blocked; struct virtio_gpu_scanout scanout[2]; } parent_obj;
    GLuint scanout_texture[2], scanout_pending_texture[2];
    uint32_t scanout_texture_width[2], scanout_texture_height[2];
    bool scanout_gpu[2], scanout_needs_full_update[2];
    struct VirtIOGPUExternalCopy *scanout_external_copy[2], *scanout_pending_copy[2];
    struct VirtIOGPUExternalContext *context_failure;
    void *fence_poll, *cmdq_resume_bh;
} VirtIOGPU;
typedef VirtIOGPU VirtIOGPUGL;
#define VIRTIO_GPU_GL(g) (g)
struct QemuEGLExternalCopy { unsigned id; };
static EGLDisplay qemu_egl_display=(EGLDisplay)1;
static EGLContext current_context=(EGLContext)2;
static EGLenum current_api=EGL_OPENGL_API;
static EGLSurface current_draw=(EGLSurface)3, current_read=(EGLSurface)4;
static unsigned stops, timer_deletes, bh_cancels, watches_cleared;
static unsigned new_calls, run_calls, destroy_calls, live_helpers;
static unsigned bind_calls, make_calls, deletes, displays, updates, force_calls;
static GLuint deleted[32], next_texture=100;
static bool fail_bind, fail_make, fail_new, fail_run, fail_restore, fail_destroy;
static bool consume_destroy=true, required=true, valid_console=true;
static int resource_status, modifier_status, modifier_count=2, modifier_written=2;
static EGLuint64KHR query_modifiers[2]={0,0x1234};
static EGLBoolean query_external[2]={EGL_TRUE,EGL_FALSE};
static uint32_t copied[10];
static bool copied_flip, copied_opaque;
static bool qemu_in_main_thread(void) { return true; }
static void error_report(const char *fmt, ...) { }
static void timer_del(void *timer) { CHECK(timer); ++timer_deletes; }
static void qemu_bh_cancel(void *bh) { CHECK(bh); ++bh_cancels; }
static void virtio_gpu_virgl_clear_fence_watches(VirtIOGPU *g) { ++watches_cleared; }
static void qemu_system_vmstop_request(int state) { CHECK(state==RUN_STATE_INTERNAL_ERROR); ++stops; }
static EGLenum fake_query_api(void) { return current_api; }
static EGLDisplay fake_display(void) { return qemu_egl_display; }
static EGLContext fake_context(void) { return current_context; }
static EGLSurface fake_surface(EGLint which) { return which==EGL_DRAW?current_draw:current_read; }
static EGLBoolean fake_bind(EGLenum api) { ++bind_calls; if(fail_bind)return EGL_FALSE; current_api=api; return EGL_TRUE; }
static EGLBoolean fake_make(EGLDisplay d,EGLSurface draw,EGLSurface read,EGLContext ctx) { ++make_calls; CHECK(d==qemu_egl_display); if(fail_make)return EGL_FALSE; current_draw=draw; current_read=read; current_context=ctx; return EGL_TRUE; }
static EGLint fake_egl_error(void) { return EGL_BAD_ACCESS; }
static EGLBoolean fake_modifiers(EGLDisplay d,EGLint format,EGLint max,EGLuint64KHR *mods,EGLBoolean *ext,EGLint *count) { CHECK(d==qemu_egl_display&&(format==99||format==DRM_FORMAT_ARGB8888)); if(modifier_status==1||(!max&&modifier_status==2))return EGL_FALSE; if(!max){*count=modifier_count;return EGL_TRUE;} if(modifier_status==3)return EGL_FALSE; for(int i=0;i<MIN(max,2);i++){mods[i]=query_modifiers[i];ext[i]=query_external[i];} *count=modifier_written;return EGL_TRUE; }
#undef eglQueryAPI
#define eglQueryAPI fake_query_api
#undef eglGetCurrentDisplay
#define eglGetCurrentDisplay fake_display
#undef eglGetCurrentContext
#define eglGetCurrentContext fake_context
#undef eglGetCurrentSurface
#define eglGetCurrentSurface fake_surface
#undef eglBindAPI
#define eglBindAPI fake_bind
#undef eglMakeCurrent
#define eglMakeCurrent fake_make
#undef eglGetError
#define eglGetError fake_egl_error
#undef eglQueryDmaBufModifiersEXT
#define eglQueryDmaBufModifiersEXT fake_modifiers
static void copy_error(QemuEGLExternalCopyError *error) { memset(error,0,sizeof(*error));error->operation="injected";error->context_restore_failed=fail_restore; }
QemuEGLExternalCopy *qemu_egl_external_copy_new(unsigned tex,int w,int h,QemuEGLExternalCopyError *error) { CHECK(tex&&w&&h); ++new_calls; copy_error(error);if(fail_new)return NULL; QemuEGLExternalCopy *copy=g_new0(QemuEGLExternalCopy,1);copy->id=++live_helpers;return copy; }
bool qemu_egl_external_copy_run(QemuEGLExternalCopy *copy,EGLImageKHR image,int sw,int sh,int sx,int sy,int dx,int dy,int w,int h,bool flip,bool opaque,QemuEGLExternalCopyError *error) { CHECK(copy&&image);++run_calls;copy_error(error);uint32_t args[]={sw,sh,sx,sy,dx,dy,w,h};memcpy(copied,args,sizeof(args));copied_flip=flip;copied_opaque=opaque;return !fail_run; }
bool qemu_egl_external_copy_destroy(QemuEGLExternalCopy **copy,QemuEGLExternalCopyError *error) { CHECK(copy&&*copy);++destroy_calls;copy_error(error);if(!fail_destroy||consume_destroy){g_free(*copy);*copy=NULL;--live_helpers;}return !fail_destroy; }
static bool console_has_gl(void *con) { return valid_console; }
static bool console_gl_texture_read_sync(void *con) { return true; }
static int fake_gl_version(void) { return 45; }
static int surface_bytes_per_pixel(void *ds) { return 4; }
static int surface_format(void *ds) { return PIXMAN_LE_x8r8g8b8; }
static void fake_get(GLenum pname,GLint *value) { *value=0; }
static void fake_bind_buffer(GLenum target,GLuint id) { }
static void fake_gen(GLsizei n,GLuint *id) { CHECK(n==1);*id=next_texture++; }
static void fake_bind_texture(GLenum target,GLuint id) { }
static void fake_image(GLenum target,GLint level,GLint internal,GLsizei w,GLsizei h,GLint border,GLenum format,GLenum type,const void *data) { CHECK(internal==GL_RGB8&&!data); }
static void fake_parameter(GLenum target,GLenum pname,GLint value) { }
static GLenum fake_error(void) { return GL_NO_ERROR; }
static void fake_delete(GLsizei n,const GLuint *texture) { CHECK(n==1&&deletes<32);deleted[deletes++]=*texture; }
#undef glGetIntegerv
#define glGetIntegerv fake_get
#undef glBindBuffer
#define glBindBuffer fake_bind_buffer
#undef glGenTextures
#define glGenTextures fake_gen
#undef glBindTexture
#define glBindTexture fake_bind_texture
#undef glTexImage2D
#define glTexImage2D fake_image
#undef glTexParameteri
#define glTexParameteri fake_parameter
#undef glGetError
#define glGetError fake_error
#undef glDeleteTextures
#define glDeleteTextures fake_delete
#define epoxy_gl_version fake_gl_version
static void trace_virtio_gpu_neptune_gpu_scanout(unsigned id,unsigned w,unsigned h) { }
static bool console_gl_scanout_texture_is(void *con,GLuint tex) { return false; }
static void dpy_gl_scanout_texture(void *con,GLuint tex,bool flip,unsigned w,unsigned h,unsigned x,unsigned y,unsigned rw,unsigned rh,unsigned handle,void *opaque) { ++displays; }
static void dpy_gl_update(void *con,unsigned x,unsigned y,unsigned w,unsigned h) { ++updates; }
static void virgl_renderer_force_ctx_0(void) { ++force_calls; }
#define VIRGL_RENDERER_BLOB_FD_TYPE_DMABUF 1
#define VIRGL_RENDERER_BLOB_FD_TYPE_SHM 2
#define VIRTIO_GPU_RESOURCE_FLAG_Y_0_TOP 1
struct virgl_renderer_resource_info { unsigned tex_id,width,height,flags; };
static int export_status, export_fd=37, source_info_status, classified_copy_status;
static uint32_t export_type=VIRGL_RENDERER_BLOB_FD_TYPE_DMABUF;
static unsigned fd_closes, transfer_calls, texture_calls;
static int virgl_renderer_resource_export_blob(uint32_t id,uint32_t *type,int *fd) { *type=export_type;*fd=export_fd;return export_status; }
static int virgl_renderer_resource_get_info(uint32_t id,struct virgl_renderer_resource_info *info) { *info=(struct virgl_renderer_resource_info){99,800,600,0};return source_info_status; }
static int fake_close(int fd) { CHECK(fd==export_fd);++fd_closes;return 0; }
#define close fake_close
typedef struct DisplaySurface { uint32_t width,height,stride,format; uint8_t *data; } DisplaySurface;
static uint32_t surface_width(DisplaySurface *s) { return s->width; }
static uint32_t surface_height(DisplaySurface *s) { return s->height; }
static uint32_t surface_stride(DisplaySurface *s) { return s->stride; }
static uint8_t *surface_data(DisplaySurface *s) { return s->data; }
#define DISPLAY_GL_MODE_CORE 1
static int qemu_egl_mode=DISPLAY_GL_MODE_CORE;
#define VIRGL_RENDERER_STRUCTURE_TYPE_EXPORT_QUERY 1
struct virgl_renderer_export_query {
    struct { uint32_t stype,size; } hdr;
    uint32_t in_resource_id,out_num_fds,out_fourcc,out_strides[4],out_offsets[4];
    uint64_t out_modifier;
};
static int query_status;
static int virgl_renderer_execute(void *data,uint32_t size) { struct virgl_renderer_export_query *q=data;q->out_num_fds=1;q->out_fourcc=DRM_FORMAT_ARGB8888;q->out_strides[0]=3200;return query_status; }
static bool fake_egl_extension(EGLDisplay d,const char *name) { return true; }
static EGLImageKHR fake_import(EGLDisplay d,EGLContext c,EGLenum target,EGLClientBuffer buffer,const EGLint *attrs) { return EGL_NO_IMAGE_KHR; }
#define epoxy_has_egl_extension fake_egl_extension
#undef eglCreateImageKHR
#define eglCreateImageKHR fake_import
static int virtio_gpu_neptune_transfer_dmabuf(VirtIOGPU *g,int fd,uint32_t id,const struct virtio_gpu_framebuffer *fb,const struct virtio_gpu_rect *source,uint32_t x,uint32_t y,uint32_t w,uint32_t h,void *surface,uint32_t target,uint32_t dx,uint32_t dy,bool flip,struct VirtIOGPUExternalCopy **copy,uint32_t tw,uint32_t th,bool opaque,bool *gpu_required) { ++transfer_calls;return classified_copy_status; }
static int virtio_gpu_neptune_copy_texture(GLuint source,GLuint destination,int sx,int sy,int ex,int ey,int dx,int dy,int w,int h,bool flip) { ++texture_calls;return classified_copy_status; }
/* WRAPPERS_UNDER_TEST */
/* CLASSIFICATION_UNDER_TEST */
/* TRANSFER_UNDER_TEST */
static int virtio_gpu_neptune_copy_resource(VirtIOGPU *g,const struct virtio_gpu_virgl_resource *res,const struct virtio_gpu_framebuffer *fb,const struct virtio_gpu_rect *source,const struct virtio_gpu_rect *rect,uint32_t target,uint32_t x,uint32_t y,bool flip,struct VirtIOGPUExternalCopy **copy,uint32_t tw,uint32_t th,bool opaque,bool *gpu_required) {
    *gpu_required=required;
    if(!target)return -EINVAL;
    if(resource_status)return resource_status;
    return virtio_gpu_neptune_external_copy(g,copy,(EGLImageKHR)1,target,tw,th,800,600,rect->x,rect->y,x,y,rect->width,rect->height,flip,opaque);
}
/* SCANOUT_UNDER_TEST */
static int copy(VirtIOGPU *g,struct VirtIOGPUExternalCopy **slot,GLuint target,unsigned tw,unsigned th,unsigned dy,bool flip) { return virtio_gpu_neptune_external_copy(g,slot,(EGLImageKHR)1,target,tw,th,800,600,31,17,5,dy,40,20,flip,true); }
static void recover(VirtIOGPU *g) { fail_bind=fail_make=false;CHECK(virtio_gpu_virgl_recover_context(g));CHECK(!g->context_failure); }
int main(void) {
    VirtIOGPU g={0}; bool external;
    CHECK(virtio_gpu_neptune_external_only(99,0,&external)==0&&external);
    CHECK(virtio_gpu_neptune_external_only(99,0x1234,&external)==0&&!external);
    CHECK(virtio_gpu_neptune_external_only(99,77,&external)==-ENOTSUP&&!external);
    modifier_count=-1;CHECK(virtio_gpu_neptune_external_only(99,0,&external)==-EIO);
    modifier_count=4097;CHECK(virtio_gpu_neptune_external_only(99,0,&external)==-EIO);
    modifier_count=0;CHECK(virtio_gpu_neptune_external_only(99,0,&external)==-ENOTSUP);
    modifier_count=2;modifier_written=3;CHECK(virtio_gpu_neptune_external_only(99,0,&external)==-EIO);
    modifier_written=2;modifier_status=3;CHECK(virtio_gpu_neptune_external_only(99,0,&external)==-EIO);modifier_status=0;
    struct VirtIOGPUExternalCopy *slot=NULL;
    CHECK(copy(&g,&slot,10,100,90,70,true)==0);
    CHECK(copied[2]==31&&copied[3]==17&&copied[4]==5&&copied[5]==50&&copied_flip&&copied_opaque);
    CHECK(copy(&g,&slot,10,100,90,5,false)==0&&new_calls==1&&copied[5]==5);
    CHECK(copy(&g,&slot,10,100,90,10,true)==-EINVAL);
    CHECK(copy(&g,&slot,10,30,90,0,false)==-EINVAL);
    CHECK(copy(&g,&slot,11,100,90,0,false)==0&&new_calls==2&&destroy_calls==1);
    current_context=(EGLContext)22;
    CHECK(copy(&g,&slot,11,100,90,0,false)==0&&new_calls==3&&destroy_calls==2);
    CHECK(copy(&g,&slot,11,120,90,0,false)==0&&new_calls==4&&destroy_calls==3);
    fail_run=true;CHECK(copy(&g,&slot,11,120,90,0,false)==-EIO&&!g.context_failure);
    g.fence_poll=(void*)1;g.cmdq_resume_bh=(void*)2;g.parent_obj.renderer_blocked=3;
    fail_restore=true;CHECK(copy(&g,&slot,11,120,90,0,false)==-EOWNERDEAD);
    CHECK(g.context_failure&&g.parent_obj.renderer_blocked==4&&stops==1&&timer_deletes==1&&bh_cancels==1&&watches_cleared==1);
    unsigned before=run_calls;CHECK(copy(&g,&slot,11,120,90,0,false)==-EOWNERDEAD&&run_calls==before);
    CHECK(!virtio_gpu_neptune_copy_destroy(&g,&slot)&&slot);
    fail_bind=true;CHECK(!virtio_gpu_virgl_recover_context(&g)&&g.context_failure&&g.parent_obj.renderer_blocked==4);
    fail_bind=false;fail_make=true;CHECK(!virtio_gpu_virgl_recover_context(&g)&&g.context_failure);
    recover(&g);CHECK(g.parent_obj.renderer_blocked==3);
    fail_run=fail_restore=false;
    fail_destroy=true;consume_destroy=false;
    CHECK(!virtio_gpu_neptune_copy_destroy(&g,&slot)&&slot&&slot->helper);recover(&g);
    consume_destroy=true;CHECK(!virtio_gpu_neptune_copy_destroy(&g,&slot)&&!slot);recover(&g);
    fail_destroy=false;fail_new=true;CHECK(copy(&g,&slot,1,100,90,0,false)==-EIO&&!slot&&!g.context_failure);
    fail_restore=true;CHECK(copy(&g,&slot,1,100,90,0,false)==-EOWNERDEAD&&!slot);recover(&g);
    fail_new=fail_restore=false;
    struct virtio_gpu_virgl_resource resource={.base.resource_id=1};
    struct virtio_gpu_rect source={20,30,80,60},dirty={22,35,6,8};
    g.parent_obj.scanout[0].ds=(void*)1;g.parent_obj.renderer_blocked=0;
    valid_console=false;
    CHECK(virtio_gpu_neptune_gpu_scanout(&g,0,&resource,&source,&dirty)==-ENOTSUP);
    required=false;
    CHECK(virtio_gpu_neptune_gpu_scanout(&g,0,&resource,&source,&dirty)==0);
    required=valid_console=true;
    CHECK(virtio_gpu_neptune_gpu_scanout(&g,0,&resource,&source,&dirty)==1);
    CHECK(g.scanout_texture[0]==100&&g.scanout_external_copy[0]&&copied[2]==20&&copied[3]==30&&copied[6]==80&&copied[7]==60);
    CHECK(virtio_gpu_neptune_gpu_scanout(&g,0,&resource,&source,&dirty)==1);
    CHECK(copied[2]==22&&copied[3]==35&&copied[4]==2&&copied[5]==5&&copied[6]==6&&copied[7]==8);
    resource_status=-EIO;before=deletes;CHECK(virtio_gpu_neptune_gpu_scanout(&g,0,&resource,&source,&dirty)==-EIO&&deletes==before);
    required=false;CHECK(virtio_gpu_neptune_gpu_scanout(&g,0,&resource,&source,&dirty)==0);required=true;resource_status=0;
    source.width=90;fail_run=fail_restore=true;
    CHECK(virtio_gpu_neptune_gpu_scanout(&g,0,&resource,&source,&dirty)==-EOWNERDEAD);
    CHECK(g.scanout_texture[0]==100&&g.scanout_pending_texture[0]==101&&g.scanout_pending_copy[0]&&deletes==before);
    recover(&g);fail_run=fail_restore=false;
    CHECK(virtio_gpu_neptune_copy_destroy(&g,&g.scanout_pending_copy[0]));g.scanout_pending_texture[0]=0;
    CHECK(virtio_gpu_neptune_gpu_scanout(&g,0,&resource,&source,&dirty)==1);
    CHECK(g.scanout_texture[0]==102&&deletes==before+1&&deleted[before]==100);
    source.width=100;fail_destroy=true;consume_destroy=false;before=deletes;
    CHECK(virtio_gpu_neptune_gpu_scanout(&g,0,&resource,&source,&dirty)==-EOWNERDEAD);
    CHECK(g.scanout_texture[0]==103&&g.scanout_pending_texture[0]==102&&g.scanout_pending_copy[0]&&deletes==before);
    recover(&g);fail_destroy=false;
    CHECK(virtio_gpu_neptune_copy_destroy(&g,&g.scanout_pending_copy[0]));
    CHECK(virtio_gpu_neptune_copy_destroy(&g,&g.scanout_external_copy[0]));
    CHECK(live_helpers==0);
    resource.is_blob=true;struct virtio_gpu_framebuffer fb={0};bool gpu_required=false;
    classified_copy_status=-EIO;
    CHECK(classify_resource(&g,&resource,&fb,&source,&dirty,1,0,0,false,&slot,100,100,false,&gpu_required)==-EIO&&gpu_required&&transfer_calls==1&&fd_closes==1);
    export_status=-EIO;gpu_required=false;
    CHECK(classify_resource(&g,&resource,&fb,&source,&dirty,1,0,0,false,&slot,100,100,false,&gpu_required)==-EIO&&gpu_required&&fd_closes==1);
    export_status=0;export_type=VIRGL_RENDERER_BLOB_FD_TYPE_SHM;
    CHECK(classify_resource(&g,&resource,&fb,&source,&dirty,1,0,0,false,&slot,100,100,false,&gpu_required)==-ENOTSUP&&!gpu_required&&fd_closes==2);
    export_fd=-1;gpu_required=false;
    CHECK(classify_resource(&g,&resource,&fb,&source,&dirty,1,0,0,false,&slot,100,100,false,&gpu_required)==-EINVAL&&gpu_required&&fd_closes==2);
    export_fd=37;export_type=99;gpu_required=false;
    CHECK(classify_resource(&g,&resource,&fb,&source,&dirty,1,0,0,false,&slot,100,100,false,&gpu_required)==-ENOTSUP&&gpu_required&&fd_closes==3);
    resource.is_blob=false;source_info_status=-ENOENT;gpu_required=false;
    CHECK(classify_resource(&g,&resource,&fb,&source,&dirty,1,0,0,false,&slot,100,100,false,&gpu_required)==-ENOENT&&gpu_required&&!texture_calls);
    source_info_status=0;gpu_required=false;
    CHECK(classify_resource(&g,&resource,&fb,&source,&dirty,1,0,0,false,&slot,100,100,false,&gpu_required)==-EIO&&gpu_required&&texture_calls==1);
    source=(struct virtio_gpu_rect){0,0,800,600};fb=(struct virtio_gpu_framebuffer){PIXMAN_LE_a8r8g8b8,4,800,600,3200,0};
    query_external[0]=EGL_FALSE;gpu_required=false;
    CHECK(classify_transfer(&g,37,1,&fb,&source,0,0,20,20,NULL,99,0,0,false,&slot,800,600,false,&gpu_required)==-EIO&&gpu_required);
    query_status=-EINVAL;gpu_required=false;
    CHECK(classify_transfer(&g,37,1,&fb,&source,0,0,20,20,NULL,99,0,0,false,&slot,800,600,false,&gpu_required)==-EINVAL&&gpu_required);
    query_status=0;gpu_required=false;
    CHECK(classify_transfer(&g,37,1,&fb,&source,900,0,20,20,NULL,99,0,0,false,&slot,800,600,false,&gpu_required)==-EINVAL&&gpu_required);
    printf("PASS: %u checks; exact modifiers, coordinates, cache lifetime, no fallback, controlled stop/recovery, retained scanout targets\n",checks);
}
