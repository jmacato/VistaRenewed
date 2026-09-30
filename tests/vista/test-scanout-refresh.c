#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
enum { VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID = 1,
       VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER, VIRTIO_GPU_RESP_ERR_UNSPEC, TT_HOST_COPY_BEGIN,
       TT_HOST_COPY_END, TRACE_VIRTIO_GPU_NEPTUNE_SCANOUT_PIXEL };
struct virtio_gpu_rect { uint32_t x, y, width, height; };
struct virtio_gpu_framebuffer { uint32_t width, height, format, stride, offset; };
typedef struct DisplaySurface { uint32_t pixels[16]; } DisplaySurface;
struct virtio_gpu_scanout {
    uint32_t resource_id, width, height;
    int32_t x, y;
    void *con;
    DisplaySurface *ds;
    struct virtio_gpu_framebuffer fb;
};
typedef struct { int max_outputs; bool neptune; } Config;
typedef struct VirtIOGPU {
    struct { Config conf; struct virtio_gpu_scanout scanout[2]; } parent_obj;
    bool scanout_needs_full_update[2];
    bool scanout_gpu[2];
    unsigned scanout_texture[2];
    void *scanout_external_copy[2];
} VirtIOGPU;
struct virtio_gpu_resource_flush {
    uint32_t resource_id;
    struct virtio_gpu_rect r;
};
struct virtio_gpu_ctrl_command {
    const void *payload;
    struct { uint64_t fence_id; uint32_t ctx_id; } cmd_hdr;
    int error;
};
struct virtio_gpu_virgl_resource { bool is_blob; };
struct virgl_renderer_resource_info { uint32_t width, height; };
#define VIRTIO_GPU_FILL_CMD(field) memcpy(&(field), cmd->payload, sizeof(field))
#define VIRTIO_GPU_GL(g) (g)
static struct virtio_gpu_virgl_resource resource;
static uint32_t backing[8 * 6];
static struct virtio_gpu_rect last_dirty;
static unsigned reads, updates, invalidations;
static bool prepare_ok = true;
static int read_error;
static bool virtio_gpu_neptune_enabled(Config conf) { return conf.neptune; }
static int surface_height(DisplaySurface *s) { (void)s; return 4; }
static int surface_width(DisplaySurface *s) { (void)s; return 4; }
static int surface_stride(DisplaySurface *s) { (void)s; return 16; }
static int surface_bytes_per_pixel(DisplaySurface *s) { (void)s; return 4; }
static uint8_t *surface_data(DisplaySurface *s) { return (void *)s->pixels; }
static void trace_virtio_gpu_cmd_res_flush(uint32_t r, uint32_t w, uint32_t h,
                                          uint32_t x, uint32_t y)
{ (void)r; (void)w; (void)h; (void)x; (void)y; }
static void trace_virtio_gpu_neptune_scanout_pixel(uint32_t r, uint32_t f,
                                                 uint32_t s, uint32_t o,
                                                 uint32_t p)
{ (void)r; (void)f; (void)s; (void)o; (void)p; }
static bool trace_event_get_state_backends(int event) { (void)event; return false; }
static void triton_trace_record(int kind, uint64_t cookie, uint32_t context,
                                uint64_t a, uint64_t b, uint64_t c)
{ (void)kind; (void)cookie; (void)context; (void)a; (void)b; (void)c; }
static void triton_trace_scanout(void *con, uint64_t cookie, uint32_t context,
                                 uint32_t res)
{ (void)con; (void)cookie; (void)context; (void)res; }
static void triton_trace_invalidate(void *con) { (void)con; invalidations++; }
static struct virtio_gpu_virgl_resource *virtio_gpu_virgl_find_resource(
    VirtIOGPU *g, uint32_t id)
{ (void)g; return id == 2 ? &resource : NULL; }
static int virgl_renderer_resource_get_info(uint32_t id,
                                           struct virgl_renderer_resource_info *info)
{ assert(id == 2); *info = (struct virgl_renderer_resource_info){8, 6}; return 0; }
static int virgl_status_to_virtio_error(int error) { return error; }
static void virtio_gpu_rect_update(VirtIOGPU *g, int i,
                                   uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{ (void)g; (void)i; (void)x; (void)y; (void)w; (void)h; }
static void dpy_gfx_update(void *con, uint32_t x, uint32_t y,
                            uint32_t w, uint32_t h)
{ (void)con; assert(x + w <= 4 && y + h <= 4); updates++; }
static bool virtio_gpu_neptune_prepare_scanout(struct virtio_gpu_scanout *s,
    struct virtio_gpu_framebuffer *fb, struct virtio_gpu_rect *source)
{ (void)s; (void)fb; (void)source; return prepare_ok; }
static int virtio_gpu_neptune_readback_surface(uint32_t id,
    struct virtio_gpu_virgl_resource *res, struct virtio_gpu_framebuffer *fb,
    struct virtio_gpu_rect *source, struct virtio_gpu_rect *dirty,
    DisplaySurface *surface, struct virtio_gpu_rect *updated)
{
    (void)res; (void)fb;
    assert(id == 2);
    reads++;
    last_dirty = *dirty;
    if (read_error) return read_error;
    uint32_t x = MAX(source->x, dirty->x), y = MAX(source->y, dirty->y);
    uint32_t right = MIN(source->x + source->width, dirty->x + dirty->width);
    uint32_t bottom = MIN(source->y + source->height, dirty->y + dirty->height);
    *updated = (struct virtio_gpu_rect){x - source->x, y - source->y,
                                      right - x, bottom - y};
    for (uint32_t row = y; row < bottom; row++) {
        for (uint32_t col = x; col < right; col++) {
            surface->pixels[(row - source->y) * 4 + col - source->x] =
                backing[row * 8 + col];
        }
    }
    return 0;
}

/* Model the GPU classifier boundary explicitly: zero is the known SHM
 * route; positive consumed the GPU update; negative must never fall back. */
static int gpu_result;
static int virtio_gpu_neptune_gpu_scanout(VirtIOGPU *g, int i,
    struct virtio_gpu_virgl_resource *res, struct virtio_gpu_rect *source,
    struct virtio_gpu_rect *dirty)
{
    (void)res; (void)source; (void)dirty;
    if (gpu_result > 0) g->scanout_needs_full_update[i] = false;
    return gpu_result;
}
/* These unused transition APIs must not run in a stable SHM/GPU binding. */
static int surface_format(DisplaySurface *s) { (void)s; return 0; }
static DisplaySurface *qemu_create_displaysurface_from(int w,int h,int f,int stride,void *data)
{ (void)w;(void)h;(void)f;(void)stride;(void)data;assert(false);return NULL; }
static void dpy_gl_scanout_disable(void *con) { (void)con;assert(false); }
static void dpy_gfx_replace_surface(void *con, DisplaySurface *s)
{ (void)con;(void)s;assert(false); }
static DisplaySurface *qemu_console_surface(void *con) { (void)con;assert(false);return NULL; }
static void virgl_renderer_force_ctx_0(void) { assert(false); }
static bool virtio_gpu_neptune_copy_destroy(VirtIOGPU *g, void **copy)
{ (void)g;(void)copy;assert(false);return false; }
static void glDeleteTextures(int count, const unsigned *textures)
{ (void)count;(void)textures;assert(false); }

/* SOURCE_UNDER_TEST */

int main(void)
{
    DisplaySurface surface = {0};
    VirtIOGPU gpu = {.parent_obj.conf = {1, true}};
    struct virtio_gpu_scanout *scanout = &gpu.parent_obj.scanout[0];
    *scanout = (struct virtio_gpu_scanout){.resource_id = 2, .width = 4,
        .height = 4, .x = 2, .y = 1, .ds = &surface, .fb = {8, 6, 0, 32, 0}};
    struct virtio_gpu_rect source = {2, 1, 4, 4};
    struct virtio_gpu_resource_flush flush = {2, {3, 2, 1, 1}};
    struct virtio_gpu_ctrl_command cmd = {.payload = &flush};
    for (unsigned i = 0; i < 48; i++) backing[i] = 1000 + i;
    assert(virtio_gpu_neptune_set_scanout(&gpu, 0, &scanout->fb, &source));
    assert(gpu.scanout_needs_full_update[0] && invalidations == 1);
    virgl_cmd_resource_flush(&gpu, &cmd);
    assert(!cmd.error && reads == 1 && updates == 1);
    assert(!gpu.scanout_needs_full_update[0]);
    assert(memcmp(&last_dirty, &source, sizeof(source)) == 0);
    for (unsigned y = 0; y < 4; y++) {
        for (unsigned x = 0; x < 4; x++) {
            assert(surface.pixels[y * 4 + x] == backing[(y + 1) * 8 + x + 2]);
        }
    }
    backing[2 * 8 + 3] = 9999;
    virgl_cmd_resource_flush(&gpu, &cmd);
    assert(reads == 2 && updates == 2 && surface.pixels[5] == 9999);
    assert(memcmp(&last_dirty, &flush.r, sizeof(flush.r)) == 0);
    assert(surface.pixels[0] == 1010);

    assert(virtio_gpu_neptune_set_scanout(&gpu, 0, &scanout->fb, &source));
    read_error = 99;
    virgl_cmd_resource_flush(&gpu, &cmd);
    assert(cmd.error == 99 && gpu.scanout_needs_full_update[0] && updates == 2);
    read_error = 0;
    cmd.error = 0;
    resource.is_blob = true;
    virgl_cmd_resource_flush(&gpu, &cmd);
    assert(!cmd.error && !gpu.scanout_needs_full_update[0] && updates == 3);

    unsigned saved_reads = reads, saved_updates = updates;
    gpu_result = 1;
    virgl_cmd_resource_flush(&gpu, &cmd);
    assert(!cmd.error && reads == saved_reads && updates == saved_updates);
    gpu_result = -17;
    virgl_cmd_resource_flush(&gpu, &cmd);
    assert(cmd.error == -17 && reads == saved_reads && updates == saved_updates);
    gpu_result = 0;

    prepare_ok = false;
    assert(!virtio_gpu_neptune_set_scanout(&gpu, 0, &scanout->fb, &source));
    assert(!gpu.scanout_needs_full_update[0] && invalidations == 2);
    puts("Fresh bindings replace stale pixels before partial updates; failures retry");
}
