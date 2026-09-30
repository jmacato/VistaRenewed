/* Exercise the production helper's teardown with injected EGL failures. */
#include "qemu/osdep.h"
#include "ui/egl-external-copy.c"

static unsigned checks, deletes, contexts, images;
static EGLenum api;
static EGLContext current;
static EGLSurface draw, read_surface;
static EGLint egl_error;
static const char *scenario;
static const EGLDisplay display = (EGLDisplay)(uintptr_t)1;
static const EGLContext desktop = (EGLContext)(uintptr_t)2;
static const EGLContext gles = (EGLContext)(uintptr_t)3;
static const EGLSurface saved_draw = (EGLSurface)(uintptr_t)4;
static const EGLSurface saved_read = (EGLSurface)(uintptr_t)5;
static const EGLImageKHR destination = (EGLImageKHR)(uintptr_t)6;

enum Failure {
    FAIL_NONE, FAIL_GLES_BIND, FAIL_GLES_CURRENT, FAIL_RESTORE_BIND,
    FAIL_RESTORE_CURRENT, FAIL_DESTROY_CONTEXT, FAIL_DESTROY_IMAGE,
};
static enum Failure failure;

#define CHECK(expr) do { \
    checks++; \
    if (!(expr)) { \
        fprintf(stderr, "%s: %s (line %d)\n", scenario, #expr, __LINE__); \
        exit(1); \
    } \
} while (0)

static EGLenum EGLAPIENTRY mock_query_api(void) { return api; }
static EGLDisplay EGLAPIENTRY mock_display(void)
{
    return current == EGL_NO_CONTEXT ? EGL_NO_DISPLAY : display;
}
static EGLContext EGLAPIENTRY mock_context(void) { return current; }
static EGLSurface EGLAPIENTRY mock_surface(EGLint which)
{
    return which == EGL_DRAW ? draw : read_surface;
}
static EGLint EGLAPIENTRY mock_error(void)
{
    EGLint result = egl_error;
    egl_error = EGL_SUCCESS;
    return result;
}
static EGLBoolean EGLAPIENTRY mock_bind(EGLenum value)
{
    if ((value == EGL_OPENGL_ES_API && failure == FAIL_GLES_BIND) ||
        (value == EGL_OPENGL_API && failure == FAIL_RESTORE_BIND)) {
        egl_error = EGL_BAD_ACCESS;
        return EGL_FALSE;
    }
    api = value;
    return EGL_TRUE;
}
static EGLBoolean EGLAPIENTRY mock_current(EGLDisplay d, EGLSurface out,
                                           EGLSurface in, EGLContext ctx)
{
    CHECK(d == display);
    if ((ctx == gles && failure == FAIL_GLES_CURRENT) ||
        (ctx == desktop && failure == FAIL_RESTORE_CURRENT) ||
        (ctx == desktop && api != EGL_OPENGL_API) ||
        (ctx == gles && api != EGL_OPENGL_ES_API)) {
        egl_error = EGL_BAD_MATCH;
        return EGL_FALSE;
    }
    current = ctx;
    draw = out;
    read_surface = in;
    return EGL_TRUE;
}
static EGLBoolean EGLAPIENTRY mock_destroy_context(EGLDisplay d, EGLContext ctx)
{
    CHECK(d == display && ctx == gles);
    contexts++;
    if (failure == FAIL_DESTROY_CONTEXT) {
        egl_error = EGL_BAD_CONTEXT;
        return EGL_FALSE;
    }
    return EGL_TRUE;
}
static EGLBoolean EGLAPIENTRY mock_destroy_image(EGLDisplay d, EGLImageKHR image)
{
    CHECK(d == display && image == destination);
    images++;
    if (failure == FAIL_DESTROY_IMAGE) {
        egl_error = EGL_BAD_PARAMETER;
        return EGL_FALSE;
    }
    return EGL_TRUE;
}
static void GLAPIENTRY mock_finish(void) { CHECK(current == gles); }
static void GLAPIENTRY mock_delete(GLsizei count, const GLuint *objects)
{
    CHECK(current == gles && count == 1 && *objects != 0);
    deletes++;
}
static void GLAPIENTRY mock_delete_program(GLuint object)
{
    CHECK(current == gles && object != 0);
    deletes++;
}

static QemuEGLExternalCopy *new_copy(void)
{
    QemuEGLExternalCopy *copy = g_new0(QemuEGLExternalCopy, 1);
    copy->thread = g_thread_self();
    copy->display = display;
    copy->desktop = desktop;
    copy->gles = gles;
    copy->destination_image = destination;
    copy->framebuffer = 1;
    copy->destination_alias = 2;
    copy->source = 3;
    copy->vao = 4;
    copy->program = 5;
    return copy;
}

static void reset(enum Failure fail)
{
    api = EGL_OPENGL_API;
    current = desktop;
    draw = saved_draw;
    read_surface = saved_read;
    egl_error = EGL_SUCCESS;
    failure = fail;
    deletes = contexts = images = 0;
}

static void check_restored(void)
{
    CHECK(api == EGL_OPENGL_API && current == desktop);
    CHECK(draw == saved_draw && read_surface == saved_read);
}

int main(void)
{
    QemuEGLExternalCopyError error;
    QemuEGLExternalCopy *copy, *original;
    const char *names[] = { "normal", "bind GLES", "make GLES current",
        "restore API", "restore context", "destroy context", "destroy image" };

    /* Libepoxy dispatch pointers let the entire actual helper compile while
     * replacing only calls exercised here. No GL/EGL driver is initialized. */
    epoxy_eglQueryAPI = mock_query_api;
    epoxy_eglGetCurrentDisplay = mock_display;
    epoxy_eglGetCurrentContext = mock_context;
    epoxy_eglGetCurrentSurface = mock_surface;
    epoxy_eglGetError = mock_error;
    epoxy_eglBindAPI = mock_bind;
    epoxy_eglMakeCurrent = mock_current;
    epoxy_eglDestroyContext = mock_destroy_context;
    epoxy_eglDestroyImageKHR = mock_destroy_image;
    epoxy_glFinish = mock_finish;
    epoxy_glDeleteFramebuffers = mock_delete;
    epoxy_glDeleteTextures = mock_delete;
    epoxy_glDeleteVertexArrays = mock_delete;
    epoxy_glDeleteProgram = mock_delete_program;

    for (enum Failure fail = FAIL_NONE; fail <= FAIL_DESTROY_IMAGE; fail++) {
        scenario = names[fail];
        reset(fail);
        original = copy = new_copy();
        bool result = qemu_egl_external_copy_destroy(&copy, &error);
        CHECK(result == (fail == FAIL_NONE));
        CHECK(error.context_restore_failed ==
              (fail == FAIL_RESTORE_BIND || fail == FAIL_RESTORE_CURRENT));
        if (fail == FAIL_GLES_BIND || fail == FAIL_GLES_CURRENT) {
            CHECK(copy == original);
            CHECK(!deletes && !contexts && !images);
            check_restored();
            failure = FAIL_NONE;
            CHECK(qemu_egl_external_copy_destroy(&copy, &error));
        }
        CHECK(copy == NULL);
        CHECK(deletes == 5 && contexts == 1 && images == 1);
        if (!error.context_restore_failed) {
            check_restored();
        }
        /* Consumed handles must be safe for unconditional repeated cleanup. */
        CHECK(qemu_egl_external_copy_destroy(&copy, &error));
        CHECK(deletes == 5 && contexts == 1 && images == 1);
    }

    scenario = "wrong thread retains ownership";
    reset(FAIL_NONE);
    original = copy = new_copy();
    copy->thread = NULL;
    CHECK(!qemu_egl_external_copy_destroy(&copy, &error));
    CHECK(copy == original && !deletes && !contexts && !images);
    check_restored();
    copy->thread = g_thread_self();
    CHECK(qemu_egl_external_copy_destroy(&copy, &error));
    CHECK(copy == NULL);

    scenario = "no current caller context";
    reset(FAIL_NONE);
    current = EGL_NO_CONTEXT;
    draw = read_surface = EGL_NO_SURFACE;
    copy = new_copy();
    CHECK(qemu_egl_external_copy_destroy(&copy, &error));
    CHECK(!copy && current == EGL_NO_CONTEXT && api == EGL_OPENGL_API);
    CHECK(draw == EGL_NO_SURFACE && read_surface == EGL_NO_SURFACE);

    scenario = "invalid owner";
    CHECK(!qemu_egl_external_copy_destroy(NULL, &error));
    printf("%u lifecycle checks passed; no GPU execution\n", checks);
    return 0;
}
