/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Bounded diagnostic capture for cross-domain guest/host correlation.
 * All producers and QMP commands execute under the BQL. Only export does I/O.
 * The downstream file format is documented in docs/interop/triton-trace.rst.
 */
#include "qemu/osdep.h"
#include <zlib.h>
#include "qemu/bswap.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-ui.h"
#include "ui/console.h"
#include "ui/triton-trace.h"

#define TRITON_TRACE_MAGIC UINT64_C(0x3152435454495254)
#define TRITON_TRACE_CAPACITY 131072
#define TRITON_TRACE_HEADER_SIZE 128
#define TRITON_TRACE_RECORD_SIZE 80

typedef struct TritonTraceCapture {
    uint64_t run;
    uint64_t start_ticks;
    uint64_t stop_ticks;
    uint32_t count;
    uint32_t dropped;
    bool enabled;
} TritonTraceCapture;

typedef struct TritonTraceDisplay {
    uint64_t cookie;
    unsigned context;
    uint32_t resource;
} TritonTraceDisplay;

static TritonTraceCapture capture;
static uint8_t *records;
static GHashTable *displays;

void triton_trace_record(unsigned kind, uint64_t cookie, unsigned context,
                         uint64_t a, uint64_t b, uint64_t c)
{
    uint8_t *r;

    if (!capture.enabled) {
        return;
    }
    if (capture.count == TRITON_TRACE_CAPACITY) {
        /* The version 1 wire field is a signed 32-bit loss counter. */
        if (capture.dropped < INT32_MAX) {
            capture.dropped++;
        }
        return;
    }
    r = records + capture.count * TRITON_TRACE_RECORD_SIZE;
    memset(r, 0, TRITON_TRACE_RECORD_SIZE);
    stq_le_p(r, capture.count);
    stq_le_p(r + 8, qemu_clock_get_ns(QEMU_CLOCK_REALTIME));
    stq_le_p(r + 16, capture.run);
    stq_le_p(r + 32, cookie);
    stq_le_p(r + 40, a);
    stq_le_p(r + 48, b);
    stq_le_p(r + 56, c);
    stl_le_p(r + 64, kind);
    stl_le_p(r + 68, getpid());
    stl_le_p(r + 76, context);
    capture.count++;
}

void triton_trace_scanout(QemuConsole *con, uint64_t cookie,
                          unsigned context, uint32_t resource)
{
    TritonTraceDisplay *display;

    if (!capture.enabled) {
        return;
    }
    if (!displays) {
        displays = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                          NULL, g_free);
    }
    display = g_hash_table_lookup(displays, con);
    if (!display) {
        display = g_new(TritonTraceDisplay, 1);
        g_hash_table_insert(displays, con, display);
    }
    *display = (TritonTraceDisplay) { cookie, context, resource };
    triton_trace_record(TT_HOST_SCANOUT, cookie, context, resource,
                        qemu_console_get_index(con), 0);
}

void triton_trace_display(TritonTraceKind kind, QemuConsole *con)
{
    TritonTraceDisplay *display;

    if (!capture.enabled || !displays) {
        return;
    }
    display = g_hash_table_lookup(displays, con);
    if (display) {
        triton_trace_record(kind, display->cookie, display->context,
                            display->resource, qemu_console_get_index(con), 0);
    }
}

void triton_trace_invalidate(QemuConsole *con)
{
    if (displays) {
        g_hash_table_remove(displays, con);
    }
}

static TritonTraceStatus *trace_status(void)
{
    TritonTraceStatus *result = g_new0(TritonTraceStatus, 1);

    result->run = capture.run;
    result->records = capture.count;
    result->dropped = capture.dropped;
    result->enabled = capture.enabled;
    return result;
}

TritonTraceStatus *qmp_triton_trace_start(uint64_t run, Error **errp)
{
    uint8_t *fresh;

    if (!run || capture.enabled) {
        error_setg(errp,
                   "Trace run must be nonzero and no capture may be active");
        return NULL;
    }
    fresh = g_try_malloc_n(TRITON_TRACE_CAPACITY, TRITON_TRACE_RECORD_SIZE);
    if (!fresh) {
        error_setg(errp, "Cannot allocate bounded trace storage");
        return NULL;
    }
    g_free(records);
    records = fresh;
    capture = (TritonTraceCapture) {
        .run = run,
        .start_ticks = qemu_clock_get_ns(QEMU_CLOCK_REALTIME),
        .enabled = true,
    };
    if (displays) {
        g_hash_table_remove_all(displays);
    }
    return trace_status();
}

TritonTraceStatus *qmp_triton_trace_stop(const char *path, Error **errp)
{
    uint8_t header[TRITON_TRACE_HEADER_SIZE] = { 0 };
    uint32_t crc;
    size_t size;
    int fd;

    if (!records) {
        error_setg(errp, "No trace recording exists");
        return NULL;
    }
    if (capture.enabled) {
        capture.enabled = false;
        capture.stop_ticks = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    }
    if (g_str_has_prefix(path, "/dev/fdset/")) {
        error_setg(errp, "Trace export requires a new file, not an fdset");
        return NULL;
    }
    size = capture.count * TRITON_TRACE_RECORD_SIZE;
    stq_le_p(header, TRITON_TRACE_MAGIC);
    stl_le_p(header + 8, 1);
    stl_le_p(header + 12, TRITON_TRACE_RECORD_SIZE);
    stq_le_p(header + 16, capture.run);
    stq_le_p(header + 24, NANOSECONDS_PER_SECOND);
    stl_le_p(header + 32, TRITON_TRACE_CAPACITY);
    stl_le_p(header + 36, capture.count);
    stl_le_p(header + 40, capture.dropped);
    stl_le_p(header + 52, 1);
    stq_le_p(header + 56, capture.start_ticks);
    stq_le_p(header + 64, capture.stop_ticks);
    crc = crc32(0, header, sizeof(header));
    stq_le_p(header + 80, crc32(crc, records, size));

    fd = qemu_create(path, O_WRONLY | O_EXCL | O_BINARY, 0600, errp);
    if (fd < 0) {
        return NULL;
    }
    if (qemu_write_full(fd, header, sizeof(header)) != sizeof(header) ||
        qemu_write_full(fd, records, size) != (ssize_t)size) {
        error_setg_errno(errp, errno, "Cannot finish trace file '%s'", path);
        close(fd);
        unlink(path);
        return NULL;
    }
    if (close(fd) < 0) {
        error_setg_errno(errp, errno, "Cannot close trace file '%s'", path);
        unlink(path);
        return NULL;
    }
    return trace_status();
}
