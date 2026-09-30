/* SPDX-License-Identifier: GPL-2.0-or-later
 * Exercise the production-suitable helper directly; reuse only allocation and
 * oracle utilities from the separately reviewed storage fixture.
 */
#include <epoxy/gl.h>
#include <epoxy/egl.h>
extern "C" {
#include "ui/egl-external-copy.h"
}
#ifndef EGL_DRM_RENDER_NODE_FILE_EXT
#define EGL_DRM_RENDER_NODE_FILE_EXT 0x3377
#endif
#include "external_copy_fixture.h"

static void copyCheck(bool ok, const char *stage, const QemuEGLExternalCopyError& e) {
    event(stage, ok ? "passed" : "failed", "\"operation\":" + quote(e.operation ? e.operation : "") +
          "," + number("egl_error", e.egl_error) + "," + number("gl_error", e.gl_error) +
          ",\"detail\":" + quote(e.detail) + ",\"context_restore_failed\":" +
          (e.context_restore_failed ? "true" : "false"));
    if (e.context_restore_failed) _exit(3); // Never clean GL objects in an unknown context.
    if (!ok) throw Stop(stage);
}

struct Desktop {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface draw = EGL_NO_SURFACE, read = EGL_NO_SURFACE;
    explicit Desktop(Candidate& a) {
      try {
        EGLDeviceEXT devices[32]; EGLint count = 0;
        if (!eglQueryDevicesEXT(32, devices, &count)) throw Stop("query EGL devices");
        for (EGLint i = 0; i < count; i++) {
            const char *path = eglQueryDeviceStringEXT(devices[i], EGL_DRM_RENDER_NODE_FILE_EXT);
            struct stat st{};
            if (!path || stat(path, &st) || major(st.st_rdev) != a.v.renderMajor ||
                minor(st.st_rdev) != a.v.renderMinor) continue;
            display = eglGetPlatformDisplayEXT(EGL_PLATFORM_DEVICE_EXT, devices[i], nullptr);
            if (display != EGL_NO_DISPLAY && eglInitialize(display, nullptr, nullptr)) break;
            display = EGL_NO_DISPLAY;
        }
        if (display == EGL_NO_DISPLAY) throw Stop("matching EGL display missing", true);
        if (!eglBindAPI(EGL_OPENGL_API)) throw Stop("bind desktop API");
        const EGLint attrs[]{EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE};
        EGLConfig config; EGLint n = 0;
        if (!eglChooseConfig(display, attrs, &config, 1, &n) || !n) throw Stop("desktop config missing");
        context = eglCreateContext(display, config, EGL_NO_CONTEXT, nullptr);
        const EGLint pbuffer[]{EGL_WIDTH, 8, EGL_HEIGHT, 8, EGL_NONE};
        draw = eglCreatePbufferSurface(display, config, pbuffer);
        read = eglCreatePbufferSurface(display, config, pbuffer);
        if (context == EGL_NO_CONTEXT || draw == EGL_NO_SURFACE || read == EGL_NO_SURFACE ||
            !eglMakeCurrent(display, draw, read, context)) throw Stop("desktop context/pbuffers failed");
        if (!glExternalIdentity(a)) throw Stop("desktop Vulkan UUID mismatch");
      } catch (...) {
        cleanup();
        throw;
      }
    }
    void cleanup() {
        if (display != EGL_NO_DISPLAY) {
            eglBindAPI(EGL_OPENGL_API);
            eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
            if (draw != EGL_NO_SURFACE) eglDestroySurface(display, draw);
            if (read != EGL_NO_SURFACE) eglDestroySurface(display, read);
            eglTerminate(display);
            display = EGL_NO_DISPLAY;
        }
    }
    ~Desktop() { cleanup(); }
};

struct CallerState {
    EGLDisplay display = eglGetCurrentDisplay();
    EGLContext context = eglGetCurrentContext();
    EGLenum api = eglQueryAPI();
    EGLSurface draw = eglGetCurrentSurface(EGL_DRAW), read = eglGetCurrentSurface(EGL_READ);
    GLint viewport[4]{}, active = 0, framebuffer = 0, texture = 0;
    GLboolean blend = glIsEnabled(GL_BLEND), scissor = glIsEnabled(GL_SCISSOR_TEST);
    CallerState() {
        glGetIntegerv(GL_VIEWPORT, viewport); glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
    }
    void check() const {
        CallerState now;
        bool ok = display == now.display && context == now.context && api == now.api &&
            draw == now.draw && read == now.read && active == now.active &&
            framebuffer == now.framebuffer && texture == now.texture &&
            blend == now.blend && scissor == now.scissor &&
            !memcmp(viewport, now.viewport, sizeof(viewport));
        event("caller_state", ok ? "passed" : "failed",
              "\"distinct_draw_read_surfaces\":true,\"desktop_gl_state_checked\":true");
        if (!ok) throw Stop("helper changed caller context/API/surfaces/GL state");
    }
};

static void poisonSource(Candidate& a) {
    externalOwnership(a, false);
    auto cmd = a.v.begin();
    VkClearColorValue poison{}; poison.float32[0] = poison.float32[2] = 1;
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, a.image, VK_IMAGE_LAYOUT_GENERAL, &poison, 1, &range);
    a.v.submit(cmd);
    externalOwnership(a, true);
}

static void restoreSource(Candidate& a) {
    externalOwnership(a, false);
    gpuPattern(a);
    externalOwnership(a, true);
}

static void destinationCase(Candidate& a, EGLImageKHR source, int width, int height, bool rgb) {
    event("destination_case", "starting", number("width", width) + "," + number("height", height) +
          ",\"format\":" + quote(rgb ? "RGB8" : "RGBA8"));
    const Pixel initial{13, 173, 91, uint8_t(rgb ? 255 : 77)};
    std::vector<Pixel> wanted(size_t(width) * height, initial);
    struct Objects {
        GLuint texture = 0, framebuffer = 0;
        QemuEGLExternalCopy *copy = nullptr;
        ~Objects() {
            if (copy) { QemuEGLExternalCopyError e{}; if (!qemu_egl_external_copy_destroy(&copy, &e)) _exit(3); }
            glDeleteFramebuffers(1, &framebuffer); glDeleteTextures(1, &texture);
        }
    } objects;
    glActiveTexture(GL_TEXTURE0);
    glGenTextures(1, &objects.texture); glBindTexture(GL_TEXTURE_2D, objects.texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, rgb ? GL_RGB8 : GL_RGBA8, width, height, 0,
                 rgb ? GL_RGB : GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenFramebuffers(1, &objects.framebuffer); glBindFramebuffer(GL_FRAMEBUFFER, objects.framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, objects.texture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) throw Stop("desktop destination FBO incomplete");
    glDisable(GL_SCISSOR_TEST); glDisable(GL_DITHER); glDisable(GL_FRAMEBUFFER_SRGB);
    glClearColor(initial[0]/255.f, initial[1]/255.f, initial[2]/255.f, initial[3]/255.f);
    glClear(GL_COLOR_BUFFER_BIT);
    // Non-default desktop state must survive the private context and GL work.
    glEnable(GL_BLEND); glEnable(GL_SCISSOR_TEST); glScissor(1, 2, 3, 4);
    glViewport(7, 11, 23, 29); glActiveTexture(GL_TEXTURE3);
    CallerState caller;
    QemuEGLExternalCopyError error{};
    objects.copy = qemu_egl_external_copy_new(objects.texture, width, height, &error);
    copyCheck(objects.copy, "helper_create", error); caller.check();
    struct Rect { int sx, sy, dx, dy, w, h; bool flip, opaque; };
    const Rect rectangles[]{
        {2, 3, 17, 29, 113, 79, false, false},
        {370, 185, 11, 13, 63, 47, true, false},
        {0, 0, 0, 0, width, height, true, false},
        {0, 0, 0, 0, width, height, false, true},
    };
    auto verify = [&](const char *stage) {
        std::vector<Pixel> actual(size_t(width) * height);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, actual.data());
        GLenum glError = glGetError();
        uint64_t errors = 0;
        for (size_t i = 0; i < actual.size(); i++) errors += actual[i] != wanted[i];
        event(stage, !errors && !glError ? "passed" : "failed", number("pixels", actual.size()) +
              "," + number("errors", errors) + "," + number("gl_error", glError) + ",\"oracle_readback\":true");
        if (errors || glError) throw Stop(stage);
    };
    verify("preserved_destination_before_copy");
    for (const auto& r : rectangles) {
        restoreSource(a);
        bool ok = qemu_egl_external_copy_run(objects.copy, source, a.o.width, a.o.height,
            r.sx, r.sy, r.dx, r.dy, r.w, r.h, r.flip, r.opaque, &error);
        copyCheck(ok, "gpu_external_copy", error); caller.check();
        // Source is overwritten before the desktop oracle: success must mean
        // the prior source pixels have already been consumed into destination.
        poisonSource(a);
        for (int y = 0; y < r.h; y++) for (int x = 0; x < r.w; x++) {
            Pixel value = expected(a.o, r.sx+x, r.sy+(r.flip ? r.h-1-y : y), false);
            if (rgb || r.opaque) value[3] = 255;
            wanted[size_t(r.dy+y)*width+r.dx+x] = value;
        }
        verify("desktop_destination_pixels_after_source_overwrite");
    }
    bool rejected = !qemu_egl_external_copy_run(objects.copy, source, a.o.width, a.o.height,
        -1, 0, 0, 0, 1, 1, false, false, &error);
    event("invalid_rectangle", rejected ? "rejected" : "failed");
    if (!rejected) throw Stop("invalid rectangle accepted");
    caller.check(); verify("rejected_copy_preserves_destination");
    bool destroyed = qemu_egl_external_copy_destroy(&objects.copy, &error);
    copyCheck(destroyed, "helper_destroy", error); caller.check();
    verify("desktop_destination_survives_alias_teardown");
}

int main() {
    try {
        Options o; o.width = 800; o.height = 600; o.tiling = "drm-linear";
        Vulkan v(o); Candidate a(v, o);
        gpuPattern(a); a.exportFd();
        Desktop desktop(a);
        externalOwnership(a, true);
        const EGLint attrs[]{EGL_WIDTH, 800, EGL_HEIGHT, 600,
            EGL_LINUX_DRM_FOURCC_EXT, 0x34325241, EGL_DMA_BUF_PLANE0_FD_EXT, a.fd,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGLint(a.layout.offset), EGL_DMA_BUF_PLANE0_PITCH_EXT, EGLint(a.layout.rowPitch),
            EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, 0, EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, 0,
            EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
        if (!epoxy_has_egl_extension(desktop.display, "EGL_EXT_image_dma_buf_import_modifiers"))
            throw Stop("explicit DMA-BUF modifier import missing", true);
        EGLImageKHR source = eglCreateImageKHR(desktop.display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attrs);
        if (source == EGL_NO_IMAGE_KHR) throw Stop("source image import failed");
        struct Image { EGLDisplay d; EGLImageKHR i; ~Image() { eglDestroyImageKHR(d, i); } } release{desktop.display, source};
        destinationCase(a, source, 800, 600, false);
        destinationCase(a, source, 800, 600, true);
        destinationCase(a, source, 320, 240, false);
        event("result", "gpu_copy_bridge_pass", "\"candidate_cpu_intermediate\":false,\"current_qemu_integration_proven\":false,\"application_performance_proven\":false");
        return 0;
    } catch (const std::exception& error) {
        event("result", "failed", "\"reason\":" + quote(error.what())); return 2;
    }
}
