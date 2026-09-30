#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

typedef void *EGLDisplay;
typedef void *EGLSurface;
typedef void *EGLContext;
typedef EGLContext QEMUGLContext;
typedef struct DisplayGLCtx { int marker; } DisplayGLCtx;
typedef struct DisplaySurface { int marker; } DisplaySurface;
typedef struct SurfaceGL { int marker; } SurfaceGL;
typedef struct DisplayChangeListener { void *con; } DisplayChangeListener;

#define EGL_NO_SURFACE ((EGLSurface)0)
#define EGL_NO_CONTEXT ((EGLContext)0)
#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))

#include <stddef.h>

typedef struct VirtualConsole {
    struct {
        DisplayChangeListener dcl;
        DisplayGLCtx dgc;
        SurfaceGL *gls;
        DisplaySurface *ds;
        EGLContext ectx;
        EGLSurface esurface;
        unsigned glupdates;
    } gfx;
} VirtualConsole;

static EGLDisplay qemu_egl_display = (EGLDisplay)0x11;
static EGLContext current_context;
static EGLSurface current_draw;
static EGLSurface current_read;
static unsigned make_current_calls;
static unsigned texture_updates;
static unsigned error_reports;
static unsigned invalidations;
static bool make_current_succeeds;
static bool rate_enabled;
static unsigned rate_updates;
static SurfaceGL *expected_gls;
static DisplaySurface *expected_ds;
static EGLContext expected_active_context;
static int expected_x, expected_y, expected_w, expected_h;

static int eglMakeCurrent(EGLDisplay display, EGLSurface draw,
                          EGLSurface read, EGLContext context)
{
    assert(display == qemu_egl_display);
    /* A window surface is deliberately forbidden in this upload fixture: it
     * models an occluded native drawable whose acquisition would block. */
    assert(draw == EGL_NO_SURFACE);
    assert(read == EGL_NO_SURFACE);
    make_current_calls++;
    if (!make_current_succeeds) {
        return 0;
    }
    current_draw = draw;
    current_read = read;
    current_context = context;
    return 1;
}

static const char *qemu_egl_get_error_string(void)
{
    return "fixture failure";
}

static void error_report(const char *format, ...)
{
    (void)format;
    error_reports++;
}

static void triton_trace_invalidate(void *con)
{
    (void)con;
    invalidations++;
}

static void surface_gl_update_texture(SurfaceGL *gls, DisplaySurface *ds,
                                      int x, int y, int w, int h)
{
    assert(current_context == expected_active_context);
    assert(current_draw == EGL_NO_SURFACE && current_read == EGL_NO_SURFACE);
    assert(gls == expected_gls && ds == expected_ds);
    assert(x == expected_x && y == expected_y && w == expected_w && h == expected_h);
    texture_updates++;
}

static bool gd_egl_rate_enabled(void)
{
    return rate_enabled;
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
/* SOURCE_UNDER_TEST */
#pragma clang diagnostic pop

static void reset_observations(void)
{
    current_context = (EGLContext)0x99;
    current_draw = (EGLSurface)0x88;
    current_read = (EGLSurface)0x77;
    make_current_calls = 0;
    texture_updates = 0;
    error_reports = 0;
    invalidations = 0;
    make_current_succeeds = true;
    rate_enabled = false;
    rate_updates = 0;
}

int main(void)
{
    (void)triton_trace_invalidate; /* Older comparison source lacks this hook. */
    VirtualConsole vc = {0};
    SurfaceGL gls = {1};
    DisplaySurface ds = {2};

    vc.gfx.gls = &gls;
    vc.gfx.ds = &ds;
    vc.gfx.ectx = (EGLContext)0x22;
    vc.gfx.esurface = (EGLSurface)0x33;
    expected_gls = &gls;
    expected_ds = &ds;
    expected_active_context = vc.gfx.ectx;
    expected_x = 7;
    expected_y = 11;
    expected_w = 13;
    expected_h = 17;

    QEMUGLContext renderer_context = (QEMUGLContext)0x44;

    reset_observations();
    assert(gd_egl_make_current(&vc.gfx.dgc, renderer_context) == 0);
    assert(make_current_calls == 1 && error_reports == 0);
    assert(current_context == renderer_context);
    assert(current_draw == EGL_NO_SURFACE && current_read == EGL_NO_SURFACE);

    reset_observations();
    make_current_succeeds = false;
    assert(gd_egl_make_current(&vc.gfx.dgc, renderer_context) == -1);
    assert(make_current_calls == 1 && error_reports == 1);
    assert(current_context == (EGLContext)0x99);

    reset_observations();
    gd_egl_update(&vc.gfx.dcl, expected_x, expected_y, expected_w, expected_h);
    assert(texture_updates == 1 && make_current_calls == 2);
    assert(vc.gfx.glupdates == 1 && rate_updates == 0);
    assert(current_context == EGL_NO_CONTEXT);
    assert(current_draw == EGL_NO_SURFACE && current_read == EGL_NO_SURFACE);

    reset_observations();
    rate_enabled = true;
    expected_x = -3;
    expected_y = 5;
    expected_w = 19;
    expected_h = 23;
    gd_egl_update(&vc.gfx.dcl, -3, 5, 19, 23);
    assert(texture_updates == 1 && make_current_calls == 2);
    assert(vc.gfx.glupdates == 2 && rate_updates == 1);
    assert(current_context == EGL_NO_CONTEXT);

    reset_observations();
    make_current_succeeds = false;
    gd_egl_update(&vc.gfx.dcl, 1, 2, 3, 4);
    assert(texture_updates == 0 && make_current_calls == 1);
    assert(error_reports == 1 && vc.gfx.glupdates == 2 && invalidations == 1);

    reset_observations();
    vc.gfx.gls = NULL;
    gd_egl_update(&vc.gfx.dcl, 1, 2, 3, 4);
    assert(texture_updates == 0 && make_current_calls == 0);
    assert(vc.gfx.glupdates == 2 && rate_updates == 0);

    vc.gfx.gls = &gls;
    vc.gfx.ds = NULL;
    rate_enabled = true;
    gd_egl_update(&vc.gfx.dcl, 1, 2, 3, 4);
    assert(texture_updates == 0 && make_current_calls == 0);
    assert(vc.gfx.glupdates == 2 && rate_updates == 0);

    puts("PASS EGL uploads use the surfaceless context, unbind, and no-op safely");
}
