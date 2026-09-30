/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef QEMU_EGL_EXTERNAL_COPY_H
#define QEMU_EGL_EXTERNAL_COPY_H

#include <epoxy/gl.h>
#include <epoxy/egl.h>

typedef struct QemuEGLExternalCopy QemuEGLExternalCopy;

typedef struct QemuEGLExternalCopyError {
    const char *operation;
    EGLint egl_error;
    GLenum gl_error;
    bool context_restore_failed;
    char detail[1024];
} QemuEGLExternalCopyError;

/*
 * Main/UI-thread only. The current desktop GL context owns destination.
 * Destination must remain allocated without storage respecification until
 * destroy; destroy this helper before destination deletion, resize or reset.
 * A private GLES context aliases the preserved destination storage. No
 * cross-API shareContext, CPU pixel access, readback or upload is used.
 */
QemuEGLExternalCopy *qemu_egl_external_copy_new(GLuint destination,
                                              int width, int height,
                                              QemuEGLExternalCopyError *error);

/*
 * Source is a ready EGLImage on the same EGLDisplay. Its producer must have
 * completed writes and transferred any required external ownership first.
 * Rectangles use pixel coordinates with GL lower-left origins; flip_y flips
 * within the source rectangle. No scaling. Pixels outside the destination
 * rectangle are preserved. opaque forces alpha to one (RGB8 is also opaque).
 * Success means source consumption and destination writes are complete.
 * The caller's API/context/draw/read surfaces and desktop GL state survive.
 * If error.context_restore_failed is set, the caller must stop GL work;
 * cleanup/rendering in an assumed desktop context would be unsafe.
 */
bool qemu_egl_external_copy_run(QemuEGLExternalCopy *copy, EGLImageKHR source,
                                int source_width, int source_height,
                                int source_x, int source_y,
                                int destination_x, int destination_y,
                                int width, int height, bool flip_y, bool opaque,
                                QemuEGLExternalCopyError *error);

/*
 * Same thread; display and owning desktop context must still be alive.
 * Nulls *copy when ownership is consumed, including a late teardown failure.
 * An early rejection leaves *copy intact so the caller can retry destruction.
 */
bool qemu_egl_external_copy_destroy(QemuEGLExternalCopy **copy,
                                    QemuEGLExternalCopyError *error);

#endif
