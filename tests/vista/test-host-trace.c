#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <glib.h>
#include <zlib.h>
#include "qemu/compiler.h"
#include "qemu/bswap.h"
#include "ui/triton-trace.h"

struct QemuConsole { int index; };
typedef struct Error { int code; } Error;
typedef struct TritonTraceStatus {
    uint64_t run, records, dropped;
    bool enabled;
} TritonTraceStatus;

#define QEMU_CLOCK_REALTIME 0
#define NANOSECONDS_PER_SECOND 1000000000
/* Distinguish this required flag even on a POSIX test host. */
#define O_BINARY 0x40000000
static uint64_t ticks = 1000;
static unsigned write_calls, fail_write;
static bool fail_close, fail_allocate;
static int qemu_console_get_index(QemuConsole *con) { return con->index; }
static int64_t qemu_clock_get_ns(int clock)
{
    assert(clock == QEMU_CLOCK_REALTIME);
    return ++ticks;
}
static void error_setg_errno(Error **errp, int code, const char *fmt, ...)
{
    (void)fmt;
    assert(!*errp);
    *errp = g_new0(Error, 1);
    (*errp)->code = code;
}
static void error_setg(Error **errp, const char *fmt, ...)
{
    error_setg_errno(errp, EINVAL, fmt);
}
static int qemu_create(const char *path, int flags, mode_t mode, Error **errp)
{
    assert((flags & (O_EXCL | O_BINARY)) == (O_EXCL | O_BINARY));
    int fd = open(path, (flags & ~O_BINARY) | O_CREAT, mode);
    if (fd < 0) {
        error_setg_errno(errp, errno, "open");
    }
    return fd;
}
static ssize_t qemu_write_full(int fd, const void *data, size_t length)
{
    write_calls++;
    if (write_calls == fail_write) {
        assert(length);
        ssize_t partial = write(fd, data, length / 2);
        errno = ENOSPC;
        return partial;
    }
    return write(fd, data, length);
}
static int test_close(int fd)
{
    int result = close(fd);
    if (fail_close) {
        fail_close = false;
        errno = EIO;
        return -1;
    }
    return result;
}
static void *test_allocate(size_t n, size_t size)
{
    return fail_allocate ? NULL : g_try_malloc_n(n, size);
}
#define close test_close
#define g_try_malloc_n test_allocate

/* SOURCE_UNDER_TEST */

#undef close
#undef g_try_malloc_n

static void clear_error(Error **error)
{
    assert(*error);
    g_clear_pointer(error, g_free);
}

int main(int argc, char **argv)
{
    Error *error = NULL;
    TritonTraceStatus *status;
    QemuConsole a = {1}, b = {2}, unrelated = {3};
    const uint64_t run = UINT64_C(0x0102030405060a0d);

    assert(argc == 2 && chdir(argv[1]) == 0);
    assert(!qmp_triton_trace_stop("absent.bin", &error));
    clear_error(&error);
    assert(!qmp_triton_trace_start(0, &error));
    clear_error(&error);
    status = qmp_triton_trace_start(run, &error);
    assert(status && status->run == run && status->enabled);
    g_free(status);
    assert(!qmp_triton_trace_start(run + 1, &error));
    clear_error(&error);

    triton_trace_record(TT_HOST_COMMAND_BEGIN, 0x0d0a, 0x10203040,
                        0x0102030405060708, 0x1122334455667788, 0xffeeddccbbaa0099);
    triton_trace_scanout(&a, 11, 101, 201);
    triton_trace_scanout(&b, 12, 102, 202);
    triton_trace_display(TT_HOST_DISPLAY_BEGIN, &a);
    triton_trace_display(TT_HOST_DISPLAY_END, &a);
    triton_trace_display(TT_HOST_DISPLAY_BEGIN, &b);
    triton_trace_display(TT_HOST_DISPLAY_END, &b);
    triton_trace_display(TT_HOST_DISPLAY_END, &unrelated);
    triton_trace_invalidate(&a);
    triton_trace_invalidate(&a);
    triton_trace_display(TT_HOST_DISPLAY_END, &a);
    assert(capture.count == 7);

    assert(!qmp_triton_trace_stop("/dev/fdset/1", &error));
    clear_error(&error);
    assert(g_file_set_contents("existing.bin", "preserve", 8, NULL));
    assert(!qmp_triton_trace_stop("existing.bin", &error));
    clear_error(&error);
    uint64_t frozen = capture.stop_ticks;
    assert(!capture.enabled);
    ticks += 100;
    triton_trace_record(TT_HOST_FENCE, 1, 1, 1, 1, 1);
    status = qmp_triton_trace_stop("capture.bin", &error);
    assert(status && !error && status->records == 7 && !status->enabled);
    assert(capture.stop_ticks == frozen);
    g_free(status);
    status = qmp_triton_trace_stop("retry.bin", &error);
    assert(status && capture.stop_ticks == frozen);
    g_free(status);

    for (unsigned stage = 1; stage <= 2; stage++) {
        write_calls = 0;
        fail_write = stage;
        assert(!qmp_triton_trace_stop("partial.bin", &error));
        assert(error->code == ENOSPC && access("partial.bin", F_OK) < 0);
        clear_error(&error);
    }
    fail_write = 0;
    fail_close = true;
    assert(!qmp_triton_trace_stop("close-error.bin", &error));
    assert(error->code == EIO && access("close-error.bin", F_OK) < 0);
    clear_error(&error);
    status = qmp_triton_trace_stop("after-errors.bin", &error);
    assert(status && capture.stop_ticks == frozen);
    g_free(status);

    uint8_t *saved_records = records;
    fail_allocate = true;
    assert(!qmp_triton_trace_start(run + 1, &error));
    assert(records == saved_records && capture.run == run);
    clear_error(&error);
    fail_allocate = false;
    status = qmp_triton_trace_start(run + 1, &error);
    assert(status && status->records == 0 && status->dropped == 0);
    g_free(status);
    triton_trace_display(TT_HOST_DISPLAY_END, &b);
    assert(capture.count == 0); /* A new run cannot reuse old publication. */
    for (unsigned i = 0; i < TRITON_TRACE_CAPACITY + 3; i++) {
        triton_trace_record(TT_HOST_FENCE, i, 1, 2, 3, 4);
    }
    assert(capture.count == TRITON_TRACE_CAPACITY && capture.dropped == 3);
    capture.dropped = INT32_MAX - 1;
    triton_trace_record(TT_HOST_FENCE, 0, 0, 0, 0, 0);
    triton_trace_record(TT_HOST_FENCE, 0, 0, 0, 0, 0);
    assert(capture.dropped == INT32_MAX);
    status = qmp_triton_trace_stop("full.bin", &error);
    assert(status && status->dropped == INT32_MAX);
    g_free(status);
    g_clear_pointer(&displays, g_hash_table_unref);
    g_free(records);
    puts("HOST TRACE LIFECYCLE PASS");
    return 0;
}
