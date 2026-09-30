/*
 * GTK UI -- egl opengl code.
 *
 * Note that gtk 3.16+ (released 2015-03-23) has a GtkGLArea widget,
 * which is GtkDrawingArea like widget with opengl rendering support.
 *
 * This code handles opengl support on older gtk versions, using egl
 * to get a opengl context for the X11 window.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "qemu/error-report.h"

#include "trace.h"

#include "ui/console.h"
#include "ui/gtk.h"
#include "ui/egl-helpers.h"
#include "ui/shader.h"
#include "ui/triton-trace.h"

#include "system/system.h"

/* Opt-in host-window pacing diagnostics, independent of guest vblank counters. */
static int64_t rate_start, rate_draw_us, rate_switch_us;
static unsigned rate_draws, rate_switches, rate_updates;

static bool gd_egl_rate_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        enabled = g_strcmp0(g_getenv("TRITON_DISPLAY_STATS"), "1") == 0;
    }
    return enabled;
}

static void gd_egl_rate_report(void)
{
    int64_t now = g_get_monotonic_time();
    if (!rate_start) {
        rate_start = now;
    }
    if (now - rate_start >= 5000000) {
        double secs = (now - rate_start) / 1000000.0;
        error_report("TRITON-RATE draws=%.1f/s switches=%.1f/s "
                     "updates=%.1f/s draw_ms=%.3f switch_ms=%.3f",
                     rate_draws / secs, rate_switches / secs,
                     rate_updates / secs,
                     rate_draws ? rate_draw_us / 1000.0 / rate_draws : 0,
                     rate_switches ? rate_switch_us / 1000.0 / rate_switches : 0);
        rate_start = now;
        rate_draws = rate_switches = rate_updates = 0;
        rate_draw_us = rate_switch_us = 0;
    }
}

static void gtk_egl_set_scanout_mode(VirtualConsole *vc, bool scanout)
{
    if (vc->gfx.scanout_mode == scanout) {
        return;
    }

    vc->gfx.scanout_mode = scanout;
    if (!vc->gfx.scanout_mode) {
        eglMakeCurrent(qemu_egl_display, vc->gfx.esurface,
                       vc->gfx.esurface, vc->gfx.ectx);
        egl_fb_destroy(&vc->gfx.guest_fb);
        if (vc->gfx.surface) {
            surface_gl_destroy_texture(vc->gfx.gls, vc->gfx.ds);
            surface_gl_create_texture(vc->gfx.gls, vc->gfx.ds);
        }
    }
}

/** DisplayState Callbacks (opengl version) **/

void gd_egl_init(VirtualConsole *vc)
{
    GdkWindow *gdk_window = gtk_widget_get_window(vc->gfx.drawing_area);
    if (!gdk_window) {
        return;
    }

    Window x11_window = gdk_x11_window_get_xid(gdk_window);
    if (!x11_window) {
        return;
    }

    if (!vc->gfx.ectx) {
        vc->gfx.ectx = qemu_egl_init_ctx();
    }
    vc->gfx.esurface = qemu_egl_init_surface
        (vc->gfx.ectx, (EGLNativeWindowType)x11_window);

    assert(vc->gfx.esurface);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    /* The first, surfaceless binding initializes desktop GL buffers to NONE. */
    if (qemu_egl_mode == DISPLAY_GL_MODE_CORE) {
        glDrawBuffer(GL_BACK);
        glReadBuffer(GL_BACK);
    }

    /*
     * GTK schedules redraws; do not additionally wait for the host compositor
     * inside QEMU's main loop. Occluded X11 windows can otherwise hold a swap
     * for a second, delaying unrelated guest timers and input as well.
     */
    if (!eglSwapInterval(qemu_egl_display, 0)) {
        error_report("egl: disabling blocking swap interval failed: %s",
                     qemu_egl_get_error_string());
    }
}

static void gd_egl_set_surface_viewport(VirtualConsole *vc,
                                      int ww, int wh, int gs)
{
    int fbw = surface_width(vc->gfx.ds);
    int fbh = surface_height(vc->gfx.ds);
    int sw, sh, x, y;

    if (vc->s->free_scale) {
        vc->gfx.scale_x = vc->gfx.scale_y =
            MIN((double)ww / fbw, (double)wh / fbh);
    }

    /*
     * Honor the selected zoom even in fullscreen. Use the same logical
     * dimensions and centering as gd_motion_event so input follows pixels.
     * An oversized surface stays at the top left, matching that mapping.
     */
    sw = fbw * vc->gfx.scale_x;
    sh = fbh * vc->gfx.scale_y;
    x = MAX(0, (ww - sw) / 2);
    y = MAX(0, (wh - sh) / 2);
    glViewport(x * gs, (wh - y - sh) * gs, sw * gs, sh * gs);
}

void gd_egl_draw(VirtualConsole *vc)
{
    int64_t rate_t0 = gd_egl_rate_enabled() ? g_get_monotonic_time() : 0;
    GdkWindow *window;
#ifdef CONFIG_GBM
    QemuDmaBuf *dmabuf = vc->gfx.guest_fb.dmabuf;
    int fence_fd;
#endif
    int ww, wh, gs;

    if (!vc->gfx.gls || !vc->gfx.esurface) {
        return;
    }

    window = gtk_widget_get_window(vc->gfx.drawing_area);
    gs = gdk_window_get_scale_factor(window);
    ww = gdk_window_get_width(window);
    wh = gdk_window_get_height(window);

    if (vc->gfx.scanout_mode) {
#ifdef CONFIG_GBM
        if (dmabuf) {
            if (!qemu_dmabuf_get_draw_submitted(dmabuf)) {
                return;
            } else {
                qemu_dmabuf_set_draw_submitted(dmabuf, false);
            }
            graphic_hw_gl_block(vc->gfx.dcl.con, true);
        }
#endif
        gd_egl_scanout_flush(&vc->gfx.dcl, 0, 0, vc->gfx.w, vc->gfx.h);

        if (vc->gfx.guest_fb.dmabuf || vc->gfx.cursor_fb.texture) {
            vc->gfx.scale_x = (double)ww / surface_width(vc->gfx.ds);
            vc->gfx.scale_y = (double)wh / surface_height(vc->gfx.ds);
        }
        glFlush();
#ifdef CONFIG_GBM
        if (dmabuf) {
            /* a still-pending previous fence must be cancelled before
             * egl_dmabuf_create_fence overwrites fence_fd, or its fd
             * handler leaks (permanent main-loop spin) */
            gd_dmabuf_cancel_fence(vc, dmabuf);
            egl_dmabuf_create_fence(dmabuf);
            fence_fd = qemu_dmabuf_get_fence_fd(dmabuf);
            if (fence_fd >= 0) {
                qemu_set_fd_handler(fence_fd, gd_hw_gl_flushed, NULL, vc);
                return;
            }
            graphic_hw_gl_block(vc->gfx.dcl.con, false);
        }
#endif
    } else {
        if (!vc->gfx.ds) {
            return;
        }
        if (!eglMakeCurrent(qemu_egl_display, vc->gfx.esurface,
                            vc->gfx.esurface, vc->gfx.ectx)) {
            error_report("egl: eglMakeCurrent failed: %s",
                         qemu_egl_get_error_string());
            return;
        }
        triton_trace_display(TT_HOST_DISPLAY_BEGIN, vc->gfx.dcl.con);

        gd_egl_set_surface_viewport(vc, ww, wh, gs);
        surface_gl_render_texture(vc->gfx.gls, vc->gfx.ds);

        if (!eglSwapBuffers(qemu_egl_display, vc->gfx.esurface)) {
            error_report("egl: eglSwapBuffers failed: %s",
                         qemu_egl_get_error_string());
            return;
        }

        glFlush();
        triton_trace_display(TT_HOST_DISPLAY_END, vc->gfx.dcl.con);
    }
    if (rate_t0) {
        rate_draws++;
        rate_draw_us += g_get_monotonic_time() - rate_t0;
        gd_egl_rate_report();
    }
}

void gd_egl_update(DisplayChangeListener *dcl,
                   int x, int y, int w, int h)
{
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);

    if (!vc->gfx.gls || !vc->gfx.ds) {
        return;
    }

    /*
     * Texture uploads do not need the window's back buffer. Binding the
     * window here can block in the native display's buffer acquisition while
     * the window is occluded, starving QEMU's device timers and QMP input.
     * qemu_egl_init_ctx() already requires a surfaceless-capable context.
     */
    if (!eglMakeCurrent(qemu_egl_display, EGL_NO_SURFACE,
                        EGL_NO_SURFACE, vc->gfx.ectx)) {
        triton_trace_invalidate(dcl->con);
        error_report("egl: eglMakeCurrent failed: %s",
                     qemu_egl_get_error_string());
        return;
    }
    surface_gl_update_texture(vc->gfx.gls, vc->gfx.ds, x, y, w, h);
    vc->gfx.glupdates++;
    if (gd_egl_rate_enabled()) {
        rate_updates++;
    }
    eglMakeCurrent(qemu_egl_display, EGL_NO_SURFACE,
                   EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

void gd_egl_refresh(DisplayChangeListener *dcl)
{
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);

    gd_update_monitor_refresh_rate(
            vc, vc->window ? vc->window : vc->gfx.drawing_area);

    if (vc->gfx.esurface && vc->gfx.guest_fb.dmabuf &&
        qemu_dmabuf_get_draw_submitted(vc->gfx.guest_fb.dmabuf)) {
        gd_egl_draw(vc);
        return;
    }

    if (!vc->gfx.esurface) {
        gd_egl_init(vc);
        if (!vc->gfx.esurface) {
            return;
        }
        if (!vc->gfx.gls) {
            vc->gfx.gls = qemu_gl_init_shader();
        }
        if (vc->gfx.ds) {
            surface_gl_destroy_texture(vc->gfx.gls, vc->gfx.ds);
            surface_gl_create_texture(vc->gfx.gls, vc->gfx.ds);
        }
#ifdef CONFIG_GBM
        if (vc->gfx.guest_fb.dmabuf) {
            egl_dmabuf_release_texture(vc->gfx.guest_fb.dmabuf);
            gd_egl_scanout_dmabuf(dcl, vc->gfx.guest_fb.dmabuf);
        }
#endif
        gtk_widget_queue_draw(vc->gfx.drawing_area);
    }

    graphic_hw_update(dcl->con);

    if (vc->gfx.glupdates) {
        vc->gfx.glupdates = 0;
        gtk_egl_set_scanout_mode(vc, false);
        gd_egl_draw(vc);
    }
}

void gd_egl_switch(DisplayChangeListener *dcl,
                   DisplaySurface *surface)
{
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);
    bool resized = true;
    int64_t rate_t0 = gd_egl_rate_enabled() ? g_get_monotonic_time() : 0;

    trace_gd_switch(vc->label, surface_width(surface), surface_height(surface));

    if (vc->gfx.ds &&
        surface_width(vc->gfx.ds) == surface_width(surface) &&
        surface_height(vc->gfx.ds) == surface_height(surface)) {
        resized = false;
    }
    eglMakeCurrent(qemu_egl_display, vc->gfx.esurface,
                   vc->gfx.esurface, vc->gfx.ectx);

    surface_gl_destroy_texture(vc->gfx.gls, vc->gfx.ds);
    vc->gfx.ds = surface;
    if (vc->gfx.gls) {
        surface_gl_create_texture(vc->gfx.gls, vc->gfx.ds);
    }

    if (resized) {
        gd_update_windowsize(vc);
    }

    eglMakeCurrent(qemu_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                   EGL_NO_CONTEXT);
    if (rate_t0) {
        rate_switches++;
        rate_switch_us += g_get_monotonic_time() - rate_t0;
        gd_egl_rate_report();
    }
}

QEMUGLContext gd_egl_create_context(DisplayGLCtx *dgc,
                                    QEMUGLParams *params)
{
    VirtualConsole *vc = container_of(dgc, VirtualConsole, gfx.dgc);

    eglMakeCurrent(qemu_egl_display, vc->gfx.esurface,
                   vc->gfx.esurface, vc->gfx.ectx);
    return qemu_egl_create_context(dgc, params);
}

void gd_egl_scanout_disable(DisplayChangeListener *dcl)
{
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);

    vc->gfx.w = 0;
    vc->gfx.h = 0;
    gtk_egl_set_scanout_mode(vc, false);
}

void gd_egl_scanout_texture(DisplayChangeListener *dcl,
                            uint32_t backing_id, bool backing_y_0_top,
                            uint32_t backing_width, uint32_t backing_height,
                            uint32_t x, uint32_t y,
                            uint32_t w, uint32_t h,
                            ScanoutTextureNative native)
{
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);

    vc->gfx.x = x;
    vc->gfx.y = y;
    vc->gfx.w = w;
    vc->gfx.h = h;
    vc->gfx.y0_top = backing_y_0_top;
    vc->gfx.glupdates = 0;

    if (!vc->gfx.esurface) {
        gd_egl_init(vc);
        if (!vc->gfx.esurface) {
            return;
        }
    }

    eglMakeCurrent(qemu_egl_display, vc->gfx.esurface,
                   vc->gfx.esurface, vc->gfx.ectx);

    gtk_egl_set_scanout_mode(vc, true);
    egl_fb_setup_for_tex(&vc->gfx.guest_fb, backing_width, backing_height,
                         backing_id, false);
}

void gd_egl_scanout_dmabuf(DisplayChangeListener *dcl,
                           QemuDmaBuf *dmabuf)
{
#ifdef CONFIG_GBM
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);
    uint32_t x, y, width, height, backing_width, backing_height, texture;
    bool y0_top;

    eglMakeCurrent(qemu_egl_display, vc->gfx.esurface,
                   vc->gfx.esurface, vc->gfx.ectx);

    egl_dmabuf_import_texture(dmabuf);
    texture = qemu_dmabuf_get_texture(dmabuf);
    if (!texture) {
        return;
    }

    x = qemu_dmabuf_get_x(dmabuf);
    y = qemu_dmabuf_get_y(dmabuf);
    width = qemu_dmabuf_get_width(dmabuf);
    height = qemu_dmabuf_get_height(dmabuf);
    backing_width = qemu_dmabuf_get_backing_width(dmabuf);
    backing_height = qemu_dmabuf_get_backing_height(dmabuf);
    y0_top = qemu_dmabuf_get_y0_top(dmabuf);

    gd_egl_scanout_texture(dcl, texture, y0_top, backing_width, backing_height,
                           x, y, width, height, NO_NATIVE_TEXTURE);

    if (qemu_dmabuf_get_allow_fences(dmabuf)) {
        vc->gfx.guest_fb.dmabuf = dmabuf;
    }
#endif
}

void gd_egl_cursor_dmabuf(DisplayChangeListener *dcl,
                          QemuDmaBuf *dmabuf, bool have_hot,
                          uint32_t hot_x, uint32_t hot_y)
{
#ifdef CONFIG_GBM
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);
    uint32_t backing_width, backing_height, texture;

    if (dmabuf) {
        egl_dmabuf_import_texture(dmabuf);
        texture = qemu_dmabuf_get_texture(dmabuf);
        if (!texture) {
            return;
        }

        backing_width = qemu_dmabuf_get_backing_width(dmabuf);
        backing_height = qemu_dmabuf_get_backing_height(dmabuf);
        egl_fb_setup_for_tex(&vc->gfx.cursor_fb, backing_width, backing_height,
                             texture, false);
    } else {
        egl_fb_destroy(&vc->gfx.cursor_fb);
    }
#endif
}

void gd_egl_cursor_position(DisplayChangeListener *dcl,
                            uint32_t pos_x, uint32_t pos_y)
{
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);

    vc->gfx.cursor_x = pos_x * vc->gfx.scale_x;
    vc->gfx.cursor_y = pos_y * vc->gfx.scale_y;
}

void gd_egl_scanout_flush(DisplayChangeListener *dcl,
                          uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);
    GdkWindow *window;
    int ww, wh, ws;

    if (!vc->gfx.scanout_mode) {
        return;
    }
    if (!vc->gfx.guest_fb.framebuffer) {
        return;
    }

    eglMakeCurrent(qemu_egl_display, vc->gfx.esurface,
                   vc->gfx.esurface, vc->gfx.ectx);

    window = gtk_widget_get_window(vc->gfx.drawing_area);
    ws = gdk_window_get_scale_factor(window);
    ww = gdk_window_get_width(window) * ws;
    wh = gdk_window_get_height(window) * ws;
    egl_fb_setup_default(&vc->gfx.win_fb, ww, wh);
    if (vc->gfx.cursor_fb.texture) {
        egl_texture_blit(vc->gfx.gls, &vc->gfx.win_fb, &vc->gfx.guest_fb,
                         vc->gfx.y0_top);
        egl_texture_blend(vc->gfx.gls, &vc->gfx.win_fb, &vc->gfx.cursor_fb,
                          vc->gfx.y0_top,
                          vc->gfx.cursor_x, vc->gfx.cursor_y,
                          vc->gfx.scale_x, vc->gfx.scale_y);
    } else if (vc->gfx.guest_fb.dmabuf) {
        egl_fb_blit(&vc->gfx.win_fb, &vc->gfx.guest_fb, !vc->gfx.y0_top);
    } else {
        GLint viewport[4];
        int top = vc->gfx.y;
        int bottom = top + vc->gfx.h;

        if (vc->gfx.y0_top) {
            top = vc->gfx.guest_fb.height - top;
            bottom = vc->gfx.guest_fb.height - bottom;
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, vc->gfx.guest_fb.framebuffer);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        gd_egl_set_surface_viewport(vc, ww / ws, wh / ws, ws);
        glGetIntegerv(GL_VIEWPORT, viewport);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glBlitFramebuffer(vc->gfx.x, bottom, vc->gfx.x + vc->gfx.w, top,
            viewport[0], viewport[1], viewport[0] + viewport[2],
            viewport[1] + viewport[3], GL_COLOR_BUFFER_BIT, GL_LINEAR);
    }

#ifdef CONFIG_GBM
    if (vc->gfx.guest_fb.dmabuf) {
        egl_dmabuf_create_sync(vc->gfx.guest_fb.dmabuf);
    }
#endif

    eglSwapBuffers(qemu_egl_display, vc->gfx.esurface);
    if (!vc->gfx.guest_fb.dmabuf) {
        /* Other shared contexts may update an owned scanout after return. */
        glFinish();
    }
}

void gd_egl_flush(DisplayChangeListener *dcl,
                  uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    VirtualConsole *vc = container_of(dcl, VirtualConsole, gfx.dcl);
    GtkWidget *area = vc->gfx.drawing_area;

    if (vc->gfx.guest_fb.dmabuf &&
        !qemu_dmabuf_get_draw_submitted(vc->gfx.guest_fb.dmabuf)) {
        qemu_dmabuf_set_draw_submitted(vc->gfx.guest_fb.dmabuf, true);
        gtk_egl_set_scanout_mode(vc, true);
    }

    /* The scanout blit covers the widget, including zoom and letterboxing. */
    gtk_widget_queue_draw(area);
}

void gtk_egl_init(DisplayGLMode mode)
{
    GdkDisplay *gdk_display = gdk_display_get_default();
    Display *x11_display = gdk_x11_display_get_xdisplay(gdk_display);

    if (qemu_egl_init_dpy_x11(x11_display, mode) < 0) {
        return;
    }

    display_opengl = 1;
}

int gd_egl_make_current(DisplayGLCtx *dgc,
                        QEMUGLContext ctx)
{
    /*
     * Renderer commands target their own FBOs. Acquiring the GTK window's
     * back buffer here can stall the virtio command queue (and device timers)
     * in Mesa/XCB when the compositor withholds that buffer. Use the same
     * surfaceless binding as the EGL headless frontend; gd_egl_draw owns the
     * window binding needed for actual display output.
     */
    return qemu_egl_make_context_current(dgc, ctx);
}
