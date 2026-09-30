#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
typedef uint32_t pixman_format_code_t;
enum {
    PIXMAN_a8r8g8b8 = 1, PIXMAN_x8r8g8b8,
    PIXMAN_a8b8g8r8, PIXMAN_x8b8g8r8,
};
typedef struct { uint32_t w, h, format; int stride; } DisplaySurface;
struct console { DisplaySurface *surface; };
struct virtio_gpu_scanout { DisplaySurface *ds; struct console *con; };
struct virtio_gpu_framebuffer { uint32_t bytes_pp, format; };
struct virtio_gpu_rect { uint32_t x, y, width, height; };
static unsigned allocations, switches, disables;
#define surface_width(s) ((s)->w)
#define surface_height(s) ((s)->h)
#define surface_format(s) ((s)->format)
static DisplaySurface *qemu_create_displaysurface_from(
    uint32_t w, uint32_t h, uint32_t format, int stride, void *data)
{
    assert(!data);
    DisplaySurface *s = malloc(sizeof(*s));
    assert(s);
    *s = (DisplaySurface){w,h,format,stride};
    allocations++;
    return s;
}
static void dpy_gl_scanout_disable(struct console *con) {
    assert(con); disables++;
}
static void dpy_gfx_replace_surface(struct console *con, DisplaySurface *s) {
    free(con->surface); con->surface=s; switches++;
}
static DisplaySurface *qemu_console_surface(struct console *con) {
    return con->surface;
}
/* SOURCE_UNDER_TEST */
int main(void) {
    struct console con={0};
    struct virtio_gpu_scanout scan={.con=&con};
    struct virtio_gpu_framebuffer fb={4,1};
    struct virtio_gpu_rect rect={0,0,1280,720};
    assert(virtio_gpu_neptune_prepare_scanout(&scan,&fb,&rect));
    DisplaySurface *first=scan.ds;
    assert(allocations==1 && switches==1 && disables==1);
    for (unsigned i=0;i<10000;i++) {
        rect.x=i%8; rect.y=i%4;
        assert(virtio_gpu_neptune_prepare_scanout(&scan,&fb,&rect));
        assert(scan.ds==first);
    }
    assert(allocations==1 && switches==1 && disables==1);
    rect.width=1920;
    assert(virtio_gpu_neptune_prepare_scanout(&scan,&fb,&rect));
    assert(allocations==2 && scan.ds->stride==7680);
    fb.format=PIXMAN_x8r8g8b8;
    assert(virtio_gpu_neptune_prepare_scanout(&scan,&fb,&rect));
    assert(allocations==2);
    fb.format=PIXMAN_a8b8g8r8;
    assert(virtio_gpu_neptune_prepare_scanout(&scan,&fb,&rect));
    assert(allocations==3);
    fb.format=PIXMAN_x8b8g8r8;
    assert(virtio_gpu_neptune_prepare_scanout(&scan,&fb,&rect));
    assert(allocations==3);
    rect.width=UINT32_MAX;
    assert(!virtio_gpu_neptune_prepare_scanout(&scan,&fb,&rect));
    rect.width=INT_MAX;
    assert(!virtio_gpu_neptune_prepare_scanout(&scan,&fb,&rect));
    rect.width=0;
    assert(!virtio_gpu_neptune_prepare_scanout(&scan,&fb,&rect));
    assert(allocations==3 && switches==3);
    free(con.surface);
    puts("PASS repeated binds reuse surface; resize/format replace; invalid bounds preserve surface");
}
