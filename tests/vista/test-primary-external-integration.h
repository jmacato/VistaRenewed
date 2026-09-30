/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef TEST_PRIMARY_EXTERNAL_INTEGRATION_H
#define TEST_PRIMARY_EXTERNAL_INTEGRATION_H
#include <stdbool.h>
#include <stdint.h>
#include "ui/egl-external-copy.h"
void bridge_source(int fd, uint32_t width, uint32_t height, uint32_t fourcc,
                   uint32_t stride, uint32_t offset, uint64_t modifier);
int bridge_external_only(bool *external);
QemuEGLExternalCopy *bridge_new(GLuint target, int width, int height,
                                QemuEGLExternalCopyError *error);
bool bridge_run(QemuEGLExternalCopy *copy, EGLImageKHR ignored,
                int sw, int sh, int sx, int sy, int dx, int dy,
                int width, int height, bool flip, bool opaque,
                QemuEGLExternalCopyError *error);
bool bridge_destroy(QemuEGLExternalCopy **copy, QemuEGLExternalCopyError *error);
void bridge_retarget(QemuEGLExternalCopy *copy, GLuint target, int width, int height);
#endif
