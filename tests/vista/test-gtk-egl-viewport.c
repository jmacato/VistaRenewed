#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))
typedef int GLint;
typedef void GtkWidget;
typedef struct DisplayChangeListener { void *con; } DisplayChangeListener;
typedef struct egl_fb {
    unsigned texture, framebuffer;
    int width, height;
    void *dmabuf;
} egl_fb;

typedef struct DisplaySurface {
    int width, height;
} DisplaySurface;
typedef struct GtkDisplayState {
    bool free_scale;
} GtkDisplayState;
typedef struct VirtualConsole {
    GtkDisplayState *s;
    struct {
        DisplaySurface *ds;
        double scale_x, scale_y;
        void *gls, *drawing_area, *esurface, *ectx;
        DisplayChangeListener dcl;
        bool scanout_mode;
        bool y0_top;
        int x, y, w, h, cursor_x, cursor_y;
        egl_fb guest_fb, cursor_fb, win_fb;
    } gfx;
} VirtualConsole;

typedef struct { int width, height, scale; } GdkWindow;
static GdkWindow window = {2880, 1620, 1};
static void *qemu_egl_display;
static bool context_ok = true, swap_ok = true;
static unsigned rendered, swaps, errors, begins, ends, flushes;
static unsigned rate_draws;
static unsigned redraws;
static int64_t rate_draw_us;
enum { TT_HOST_DISPLAY_BEGIN, TT_HOST_DISPLAY_END };
static bool gd_egl_rate_enabled(void) { return false; }
static int64_t g_get_monotonic_time(void) { return 1; }
static void gd_egl_rate_report(void) {}
static GdkWindow *gtk_widget_get_window(void *widget)
{ (void)widget; return &window; }
static int gdk_window_get_scale_factor(GdkWindow *w) { return w->scale; }
static int gdk_window_get_width(GdkWindow *w) { return w->width; }
static int gdk_window_get_height(GdkWindow *w) { return w->height; }
void gd_egl_scanout_flush(DisplayChangeListener *dcl,
                         uint32_t x, uint32_t y, uint32_t w, uint32_t h);
static bool eglMakeCurrent(void *display, void *draw, void *read, void *ctx)
{ (void)display; (void)draw; (void)read; (void)ctx; return context_ok; }
static bool eglSwapBuffers(void *display, void *surface)
{ (void)display; (void)surface; swaps++; return swap_ok; }
static const char *qemu_egl_get_error_string(void) { return "fixture error"; }
static void error_report(const char *format, ...) { (void)format; errors++; }
static void triton_trace_display(int kind, void *con)
{ (void)con; if (kind == TT_HOST_DISPLAY_BEGIN) begins++; else ends++; }
static void surface_gl_render_texture(void *gls, DisplaySurface *surface)
{ assert(gls && surface); rendered++; }
static void glFlush(void) { flushes++; }
static void glFinish(void) {}
static bool qemu_dmabuf_get_draw_submitted(void *dmabuf)
{ (void)dmabuf; return false; }
static void qemu_dmabuf_set_draw_submitted(void *dmabuf, bool submitted)
{ (void)dmabuf; (void)submitted; }
static void gtk_egl_set_scanout_mode(VirtualConsole *vc, bool scanout)
{ vc->gfx.scanout_mode = scanout; }
static void gtk_widget_queue_draw(GtkWidget *widget)
{ (void)widget; redraws++; }
static void egl_fb_setup_default(egl_fb *fb, int w, int h)
{ fb->width = w; fb->height = h; }
static void egl_texture_blit(void *gls, egl_fb *dst, egl_fb *src, bool top)
{ (void)gls; (void)dst; (void)src; (void)top; }
static void egl_texture_blend(void *gls, egl_fb *dst, egl_fb *src, bool top,
                              int x, int y, double sx, double sy)
{ (void)gls; (void)dst; (void)src; (void)top;
  (void)x; (void)y; (void)sx; (void)sy; }
static void egl_fb_blit(egl_fb *dst, egl_fb *src, bool flip)
{ (void)dst; (void)src; (void)flip; }

enum { GL_READ_FRAMEBUFFER, GL_DRAW_FRAMEBUFFER, GL_VIEWPORT,
       GL_COLOR_BUFFER_BIT, GL_LINEAR };
static int blit_src[4], blit_dst[4];
static void glBindFramebuffer(int target, unsigned fbo)
{ (void)target; (void)fbo; }
static void glClearColor(float r, float g, float b, float a)
{ (void)r; (void)g; (void)b; (void)a; }
static void glClear(int mask) { (void)mask; }
static void glBlitFramebuffer(int x0, int y0, int x1, int y1,
                              int dx0, int dy0, int dx1, int dy1,
                              int mask, int filter)
{
    blit_src[0] = x0; blit_src[1] = y0;
    blit_src[2] = x1; blit_src[3] = y1;
    blit_dst[0] = dx0; blit_dst[1] = dy0;
    blit_dst[2] = dx1; blit_dst[3] = dy1;
    assert(mask == GL_COLOR_BUFFER_BIT && filter == GL_LINEAR);
}

static int viewport[4];
static void glGetIntegerv(int name, int *value)
{
    assert(name == GL_VIEWPORT);
    for (int i = 0; i < 4; ++i) value[i] = viewport[i];
}
static int surface_width(DisplaySurface *s) { return s->width; }
static int surface_height(DisplaySurface *s) { return s->height; }
static void glViewport(int x, int y, int w, int h)
{
    viewport[0] = x;
    viewport[1] = y;
    viewport[2] = w;
    viewport[3] = h;
}

/* SOURCE_UNDER_TEST */

static void expect(int x, int y, int w, int h)
{
    assert(viewport[0] == x);
    assert(viewport[1] == y);
    assert(viewport[2] == w);
    assert(viewport[3] == h);
}

int main(void)
{
    GtkDisplayState state = {false};
    DisplaySurface surface = {1920, 1080};
    VirtualConsole vc = {.s = &state, .gfx = {
        .ds = &surface, .scale_x = 1.0, .scale_y = 1.0,
    }};

    /* The live fullscreen case must retain a centered, unscaled desktop. */
    gd_egl_set_surface_viewport(&vc, 2880, 1620, 1);
    expect(480, 270, 1920, 1080);
    assert(vc.gfx.scale_x == 1.0 && vc.gfx.scale_y == 1.0);

    /* Windowed, odd margins, HiDPI and explicit zoom use logical pixels. */
    gd_egl_set_surface_viewport(&vc, 1920, 1080, 1);
    expect(0, 0, 1920, 1080);
    gd_egl_set_surface_viewport(&vc, 2881, 1621, 1);
    expect(480, 271, 1920, 1080);
    gd_egl_set_surface_viewport(&vc, 1440, 810, 2);
    expect(0, -540, 3840, 2160);
    vc.gfx.scale_x = vc.gfx.scale_y = 0.5;
    gd_egl_set_surface_viewport(&vc, 1440, 810, 2);
    expect(480, 270, 1920, 1080);
    vc.gfx.scale_x = vc.gfx.scale_y = 1.25;
    gd_egl_set_surface_viewport(&vc, 2880, 1620, 1);
    expect(240, 135, 2400, 1350);

    /* Fit is opt-in and keeps the image and input at one aspect ratio. */
    state.free_scale = true;
    gd_egl_set_surface_viewport(&vc, 2880, 1800, 1);
    expect(0, 90, 2880, 1620);
    assert(vc.gfx.scale_x == 1.5 && vc.gfx.scale_y == 1.5);
    gd_egl_set_surface_viewport(&vc, 960, 720, 2);
    expect(0, 180, 1920, 1080);
    assert(vc.gfx.scale_x == 0.5 && vc.gfx.scale_y == 0.5);

    /* A too-small window clips at the top left, as mouse mapping expects. */
    state.free_scale = false;
    vc.gfx.scale_x = vc.gfx.scale_y = 1.0;
    gd_egl_set_surface_viewport(&vc, 1280, 720, 1);
    expect(0, -360, 1920, 1080);

    vc.gfx.gls = &vc;
    /* Exposes during window reparenting wait for surface reconstruction. */
    gd_egl_draw(&vc);
    assert(rendered == 0 && swaps == 0);
    vc.gfx.esurface = &vc;
    gd_egl_draw(&vc);
    expect(480, 270, 1920, 1080);
    assert(rendered == 1 && swaps == 1 && flushes == 1);
    assert(begins == 1 && ends == 1 && errors == 0);
    context_ok = false;
    gd_egl_draw(&vc);
    assert(rendered == 1 && begins == 1 && ends == 1 && errors == 1);
    context_ok = true;
    swap_ok = false;
    gd_egl_draw(&vc);
    assert(rendered == 2 && swaps == 2 && flushes == 1);
    assert(begins == 2 && ends == 1 && errors == 2);

    /* Owned texture scanouts retain the zoom used by mouse coordinates. */
    vc.gfx.scanout_mode = true;
    vc.gfx.scale_x = vc.gfx.scale_y = 1.0;
    gd_egl_draw(&vc);
    assert(vc.gfx.scale_x == 1.0 && vc.gfx.scale_y == 1.0);
    /* Cursor composition keeps its existing window-scaled mapping. */
    vc.gfx.cursor_fb.texture = 1;
    gd_egl_draw(&vc);
    assert(vc.gfx.scale_x == 1.5 && vc.gfx.scale_y == 1.5);

    /* A noncentral crop must select the same guest rows for both layouts. */
    vc.gfx.cursor_fb.texture = 0;
    vc.gfx.guest_fb.framebuffer = 1;
    vc.gfx.guest_fb.height = 100;
    vc.gfx.x = 7; vc.gfx.y = 10; vc.gfx.w = 30; vc.gfx.h = 20;
    surface.width = 30; surface.height = 20;
    window.width = 80; window.height = 60;
    vc.gfx.scale_x = vc.gfx.scale_y = 1.0;
    vc.gfx.y0_top = false;
    gd_egl_scanout_flush(&vc.gfx.dcl, 0, 0, 30, 20);
    assert(blit_src[0] == 7 && blit_src[2] == 37);
    assert(blit_src[1] == 30 && blit_src[3] == 10);
    assert(blit_dst[0] == 25 && blit_dst[1] == 20);
    assert(blit_dst[2] == 55 && blit_dst[3] == 40);
    vc.gfx.y0_top = true;
    gd_egl_scanout_flush(&vc.gfx.dcl, 0, 0, 30, 20);
    assert(blit_src[1] == 70 && blit_src[3] == 90);
    /* Guest-space damage outside a scaled widget still requests a draw. */
    surface.width = 1280; surface.height = 720;
    window.width = 640; window.height = 360;
    vc.gfx.scale_x = vc.gfx.scale_y = 0.5;
    gd_egl_flush(&vc.gfx.dcl, 700, 500, 40, 40);
    assert(redraws == 1);
    puts("GTK EGL viewport checks passed");
    return 0;
}
