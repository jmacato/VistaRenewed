/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "ui/egl-external-copy.h"

struct QemuEGLExternalCopy {
    GThread *thread;
    EGLDisplay display;
    EGLContext desktop, gles;
    EGLImageKHR destination_image;
    int width, height;
    GLuint destination_alias, source, framebuffer, program, vao;
    GLint source_origin, destination_origin, rectangle_height, flip, opaque;
};

typedef struct CopyCaller {
    EGLenum api;
    EGLDisplay display;
    EGLContext context;
    EGLSurface draw, read;
} CopyCaller;

static CopyCaller copy_caller(void)
{
    CopyCaller caller = {
        eglQueryAPI(), eglGetCurrentDisplay(), eglGetCurrentContext(),
        eglGetCurrentSurface(EGL_DRAW), eglGetCurrentSurface(EGL_READ),
    };
    return caller;
}

static bool copy_error(QemuEGLExternalCopyError *error, const char *operation,
                       bool egl, bool gl)
{
    if (!error->operation) {
        error->operation = operation;
        error->egl_error = egl ? eglGetError() : EGL_SUCCESS;
        error->gl_error = gl ? glGetError() : GL_NO_ERROR;
    }
    return false;
}

static bool copy_restore(QemuEGLExternalCopy *copy, const CopyCaller *caller,
                         QemuEGLExternalCopyError *error)
{
    bool ok;

    if (caller->context == EGL_NO_CONTEXT) {
        ok = eglMakeCurrent(copy->display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                            EGL_NO_CONTEXT);
        ok = eglBindAPI(caller->api) && ok;
    } else {
        ok = eglBindAPI(caller->api);
        ok = eglMakeCurrent(caller->display, caller->draw, caller->read,
                            caller->context) && ok;
    }
    if (!ok) {
        /* Context restoration failure overrides a less serious copy error. */
        error->operation = NULL;
        error->context_restore_failed = true;
        return copy_error(error, "restore caller EGL state", true, false);
    }
    return true;
}

static bool copy_current(QemuEGLExternalCopy *copy,
                         QemuEGLExternalCopyError *error)
{
    if (!eglBindAPI(EGL_OPENGL_ES_API) ||
        !eglMakeCurrent(copy->display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                         copy->gles)) {
        return copy_error(error, "make private GLES context current",
                          true, false);
    }
    return true;
}

static GLuint copy_shader(GLenum type, const char *source,
                           QemuEGLExternalCopyError *error)
{
    GLuint shader = glCreateShader(type);
    GLint good = GL_FALSE;

    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    glGetShaderiv(shader, GL_COMPILE_STATUS, &good);
    if (!good) {
        glGetShaderInfoLog(shader, sizeof(error->detail), NULL, error->detail);
        copy_error(error, "compile external copy shader", false, true);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static bool copy_program(QemuEGLExternalCopy *copy,
                          QemuEGLExternalCopyError *error)
{
    static const char vertex[] =
        "#version 300 es\n"
        "void main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);"
        "gl_Position=vec4(p*2.0-1.0,0.0,1.0);}";
    static const char fragment[] =
        "#version 300 es\n"
        "#extension GL_OES_EGL_image_external_essl3 : require\n"
        "precision highp float;precision highp int;"
        "precision highp samplerExternalOES;"
        "uniform samplerExternalOES image;"
        "uniform ivec2 source_origin,destination_origin;"
        "uniform int rectangle_height,flip_y,opaque;out vec4 color;"
        "void main(){ivec2 p=ivec2(gl_FragCoord.xy)-destination_origin;"
        "if(flip_y!=0)p.y=rectangle_height-1-p.y;"
        "color=texelFetch(image,source_origin+p,0);"
        "if(opaque!=0)color.a=1.0;}";
    GLuint vs = copy_shader(GL_VERTEX_SHADER, vertex, error);
    GLuint fs = copy_shader(GL_FRAGMENT_SHADER, fragment, error);
    GLint good = GL_FALSE;

    if (vs && fs) {
        copy->program = glCreateProgram();
        glAttachShader(copy->program, vs);
        glAttachShader(copy->program, fs);
        glLinkProgram(copy->program);
        glGetProgramiv(copy->program, GL_LINK_STATUS, &good);
        if (!good) {
            glGetProgramInfoLog(copy->program, sizeof(error->detail), NULL,
                                error->detail);
            copy_error(error, "link external copy program", false, true);
        }
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!good) {
        return false;
    }
    glUseProgram(copy->program);
    glUniform1i(glGetUniformLocation(copy->program, "image"), 0);
    copy->source_origin = glGetUniformLocation(copy->program, "source_origin");
    copy->destination_origin = glGetUniformLocation(copy->program,
                                                   "destination_origin");
    copy->rectangle_height = glGetUniformLocation(copy->program,
                                                 "rectangle_height");
    copy->flip = glGetUniformLocation(copy->program, "flip_y");
    copy->opaque = glGetUniformLocation(copy->program, "opaque");
    return true;
}

static void copy_delete_gl(QemuEGLExternalCopy *copy)
{
    glFinish();
    glDeleteFramebuffers(1, &copy->framebuffer);
    glDeleteTextures(1, &copy->destination_alias);
    glDeleteTextures(1, &copy->source);
    glDeleteVertexArrays(1, &copy->vao);
    glDeleteProgram(copy->program);
}

QemuEGLExternalCopy *qemu_egl_external_copy_new(GLuint destination,
                                              int width, int height,
                                              QemuEGLExternalCopyError *error)
{
    const EGLint image_attributes[] = {
        EGL_GL_TEXTURE_LEVEL_KHR, 0,
        EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE,
    };
    const EGLint config_attributes[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR, EGL_NONE,
    };
    const EGLint context_attributes[] = {
        EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE,
    };
    CopyCaller caller = copy_caller();
    QemuEGLExternalCopy *copy;
    EGLConfig config;
    EGLint count = 0;
    GLenum gl_error;
    bool current = false;

    memset(error, 0, sizeof(*error));
    if (caller.api != EGL_OPENGL_API || caller.context == EGL_NO_CONTEXT ||
        caller.display == EGL_NO_DISPLAY || !destination ||
        width <= 0 || height <= 0) {
        copy_error(error, "desktop destination context/size required",
                   false, false);
        return NULL;
    }
    if (!epoxy_has_egl_extension(caller.display,
                                 "EGL_KHR_gl_texture_2D_image") ||
        !epoxy_has_egl_extension(caller.display, "EGL_KHR_image_base") ||
        !epoxy_has_egl_extension(caller.display,
                                 "EGL_KHR_surfaceless_context")) {
        copy_error(error, "required EGL image/surfaceless extensions absent",
                   false, false);
        return NULL;
    }
    copy = g_new0(QemuEGLExternalCopy, 1);
    copy->thread = g_thread_self();
    copy->display = caller.display;
    copy->desktop = caller.context;
    copy->width = width;
    copy->height = height;
    glFinish();
    gl_error = glGetError();
    if (gl_error != GL_NO_ERROR) {
        copy_error(error, "complete desktop destination work", false, false);
        error->gl_error = gl_error;
        goto fail;
    }
    copy->destination_image = eglCreateImageKHR(copy->display, copy->desktop,
        EGL_GL_TEXTURE_2D_KHR, (EGLClientBuffer)(uintptr_t)destination,
        image_attributes);
    if (copy->destination_image == EGL_NO_IMAGE_KHR) {
        copy_error(error, "preserved desktop destination EGLImage",
                   true, false);
        goto fail;
    }
    if (!eglBindAPI(EGL_OPENGL_ES_API) ||
        !eglChooseConfig(copy->display, config_attributes, &config, 1,
                          &count) || !count) {
        copy_error(error, "choose private GLES3 config", true, false);
        goto fail;
    }
    copy->gles = eglCreateContext(copy->display, config, EGL_NO_CONTEXT,
                                  context_attributes);
    if (copy->gles == EGL_NO_CONTEXT) {
        copy_error(error, "create private GLES3 context", true, false);
        goto fail;
    }
    current = copy_current(copy, error);
    if (!current) {
        goto fail;
    }
    if (!epoxy_has_gl_extension("GL_OES_EGL_image") ||
        !epoxy_has_gl_extension("GL_OES_EGL_image_external") ||
        !epoxy_has_gl_extension("GL_OES_EGL_image_external_essl3")) {
        copy_error(error, "required GLES image/sampling extensions absent",
                   false, false);
        goto fail;
    }
    glGenTextures(1, &copy->destination_alias);
    glBindTexture(GL_TEXTURE_2D, copy->destination_alias);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, copy->destination_image);
    gl_error = glGetError();
    if (gl_error != GL_NO_ERROR) {
        copy_error(error, "alias desktop destination in GLES", false, false);
        error->gl_error = gl_error;
        goto fail;
    }
    glGenFramebuffers(1, &copy->framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, copy->framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           copy->destination_alias, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        copy_error(error, "GLES destination alias framebuffer incomplete",
                   false, true);
        goto fail;
    }
    if (!copy_program(copy, error)) {
        goto fail;
    }
    glGenTextures(1, &copy->source);
    glGenVertexArrays(1, &copy->vao);
    if (!copy_restore(copy, &caller, error)) {
        goto fail;
    }
    return copy;

fail:
    if (current && eglGetCurrentContext() == copy->gles) {
        copy_delete_gl(copy);
    }
    copy_restore(copy, &caller, error);
    if (copy->gles != EGL_NO_CONTEXT) {
        eglDestroyContext(copy->display, copy->gles);
    }
    if (copy->destination_image != EGL_NO_IMAGE_KHR) {
        eglDestroyImageKHR(copy->display, copy->destination_image);
    }
    g_free(copy);
    return NULL;
}

bool qemu_egl_external_copy_run(QemuEGLExternalCopy *copy, EGLImageKHR source,
                                int source_width, int source_height,
                                int source_x, int source_y,
                                int destination_x, int destination_y,
                                int width, int height, bool flip_y, bool opaque,
                                QemuEGLExternalCopyError *error)
{
    CopyCaller caller = copy_caller();
    bool ok = false;
    GLenum gl_error;

    memset(error, 0, sizeof(*error));
    if (!copy || copy->thread != g_thread_self() ||
        caller.api != EGL_OPENGL_API ||
        caller.display != copy->display || caller.context != copy->desktop) {
        return copy_error(error, "copy requires owning desktop context/thread",
                          false, false);
    }
    if (source == EGL_NO_IMAGE_KHR || width <= 0 || height <= 0 ||
        source_x < 0 || source_y < 0 ||
        destination_x < 0 || destination_y < 0 ||
        source_width < width || source_height < height ||
        source_x > source_width - width || source_y > source_height - height ||
        copy->width < width || copy->height < height ||
        destination_x > copy->width - width ||
        destination_y > copy->height - height) {
        return copy_error(error, "invalid source/destination rectangle",
                          false, false);
    }
    /* Complete prior desktop accesses to this sibling before GLES writes. */
    glFinish();
    gl_error = glGetError();
    if (gl_error != GL_NO_ERROR) {
        copy_error(error, "complete desktop work before copy", false, false);
        error->gl_error = gl_error;
        return false;
    }
    if (!copy_current(copy, error)) {
        copy_restore(copy, &caller, error);
        return false;
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, copy->source);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, source);
    gl_error = glGetError();
    if (gl_error != GL_NO_ERROR) {
        copy_error(error, "import external source", false, false);
        error->gl_error = gl_error;
        goto out;
    }
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S,
                    GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T,
                    GL_CLAMP_TO_EDGE);
    glBindFramebuffer(GL_FRAMEBUFFER, copy->framebuffer);
    glViewport(destination_x, destination_y, width, height);
    glDisable(GL_BLEND);
    glDisable(GL_DITHER);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_RASTERIZER_DISCARD);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUseProgram(copy->program);
    glUniform2i(copy->source_origin, source_x, source_y);
    glUniform2i(copy->destination_origin, destination_x, destination_y);
    glUniform1i(copy->rectangle_height, height);
    glUniform1i(copy->flip, flip_y);
    glUniform1i(copy->opaque, opaque);
    glBindVertexArray(copy->vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glFinish();
    gl_error = glGetError();
    if (gl_error == GL_NO_ERROR) {
        ok = true;
    } else {
        copy_error(error, "external image GPU copy", false, false);
        error->gl_error = gl_error;
    }
out:
    /* Detach the completed source; the caller controls its lifetime. */
    glDeleteTextures(1, &copy->source);
    glGenTextures(1, &copy->source);
    return copy_restore(copy, &caller, error) && ok;
}

bool qemu_egl_external_copy_destroy(QemuEGLExternalCopy **owner,
                                    QemuEGLExternalCopyError *error)
{
    CopyCaller caller = copy_caller();
    QemuEGLExternalCopy *copy;
    bool ok;

    memset(error, 0, sizeof(*error));
    if (!owner) {
        return copy_error(error, "destroy requires an owner", false, false);
    }
    copy = *owner;
    if (!copy) {
        return true;
    }
    if (copy->thread != g_thread_self()) {
        return copy_error(error, "destroy requires owning thread",
                          false, false);
    }
    if (!copy_current(copy, error)) {
        copy_restore(copy, &caller, error);
        return false;
    }
    copy_delete_gl(copy);
    ok = copy_restore(copy, &caller, error);
    ok = eglDestroyContext(copy->display, copy->gles) && ok;
    ok = eglDestroyImageKHR(copy->display, copy->destination_image) && ok;
    if (!ok) {
        copy_error(error, "destroy external copy resources", true, false);
    }
    g_free(copy);
    *owner = NULL;
    return ok;
}
