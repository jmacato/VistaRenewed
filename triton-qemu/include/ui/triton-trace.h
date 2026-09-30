/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef QEMU_TRITON_TRACE_H
#define QEMU_TRITON_TRACE_H

#include "qemu/typedefs.h"

typedef enum TritonTraceKind {
    TT_HOST_COMMAND_BEGIN = 64,
    TT_HOST_COMMAND_END,
    TT_HOST_FENCE,
    TT_HOST_SCANOUT,
    TT_HOST_COPY_BEGIN,
    TT_HOST_COPY_END,
    TT_HOST_UPLOAD_BEGIN,
    TT_HOST_UPLOAD_END,
    TT_HOST_DISPLAY_BEGIN,
    TT_HOST_DISPLAY_END,
} TritonTraceKind;

void triton_trace_record(unsigned kind, uint64_t cookie, unsigned context,
                         uint64_t a, uint64_t b, uint64_t c);
void triton_trace_scanout(QemuConsole *con, uint64_t cookie,
                          unsigned context, uint32_t resource);
void triton_trace_display(TritonTraceKind kind, QemuConsole *con);
void triton_trace_invalidate(QemuConsole *con);

#endif
