#include <cassert>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <pixman.h>
struct DisplaySurface { int w, h; pixman_format_code_t format; };
struct Console { DisplaySurface *surface = nullptr; };
struct virtio_gpu_scanout { DisplaySurface *ds; Console *con; };
struct virtio_gpu_framebuffer { unsigned bytes_pp; pixman_format_code_t format; };
struct virtio_gpu_rect { unsigned x, y, width, height; };
static unsigned allocations, switches;
static int surface_width(DisplaySurface *s) { return s->w; }
static int surface_height(DisplaySurface *s) { return s->h; }
static pixman_format_code_t surface_format(DisplaySurface *s) { return s->format; }
static DisplaySurface *qemu_create_displaysurface_from(unsigned w, unsigned h,
    pixman_format_code_t f, uint64_t, void *) {
    allocations++; return new DisplaySurface{int(w), int(h), f};
}
static void dpy_gl_scanout_disable(Console *) {}
static void dpy_gfx_replace_surface(Console *c, DisplaySurface *s) {
    switches++; delete c->surface; c->surface = s;
}
static DisplaySurface *qemu_console_surface(Console *c) { return c->surface; }
/* SOURCE_UNDER_TEST */
int main() {
    Console c;
    virtio_gpu_scanout s{nullptr, &c};
    virtio_gpu_rect r{0, 0, 1280, 720};
    virtio_gpu_framebuffer f{4, PIXMAN_a8r8g8b8};
    assert(virtio_gpu_neptune_prepare_scanout(&s, &f, &r));
    for (int i = 0; i < 100; i++) {
        f.format = i & 1 ? PIXMAN_x8r8g8b8 : PIXMAN_a8r8g8b8;
        assert(virtio_gpu_neptune_prepare_scanout(&s, &f, &r));
    }
    assert(allocations == 1 && switches == 1);
    assert(s.ds->format == PIXMAN_x8r8g8b8);
    f.format = PIXMAN_a8b8g8r8;
    assert(virtio_gpu_neptune_prepare_scanout(&s, &f, &r));
    assert(allocations == 2 && s.ds->format == PIXMAN_x8b8g8r8);
    f.format = PIXMAN_x8b8g8r8;
    assert(virtio_gpu_neptune_prepare_scanout(&s, &f, &r));
    assert(allocations == 2);
    r.width = 1920;
    assert(virtio_gpu_neptune_prepare_scanout(&s, &f, &r));
    assert(allocations == 3);
    r.width = 0;
    assert(!virtio_gpu_neptune_prepare_scanout(&s, &f, &r));
    r.width = UINT_MAX;
    assert(!virtio_gpu_neptune_prepare_scanout(&s, &f, &r));
    assert(allocations == 3);
    delete c.surface;
    puts("PASS scanout alpha/padding reuse, channel-layout/size changes, invalid bounds");
}
