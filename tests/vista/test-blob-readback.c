/* Focused executable contract test for virtio_gpu_neptune_readback_blob. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
#define CONFIG_LINUX 1
#include <sys/ioctl.h>
#include <linux/dma-buf.h>
#endif

#define VIRGL_RENDERER_BLOB_FD_TYPE_SHM 3
#define VIRGL_RENDERER_BLOB_FD_TYPE_DMABUF 1
#define MIN(a, b) ((a) < (b) ? (a) : (b))

struct resource_base { uint32_t resource_id; uint64_t blob_size; };
struct virtio_gpu_virgl_resource {
    struct resource_base base;
    int is_blob;
};
struct virtio_gpu_framebuffer {
    uint32_t bytes_pp, width, height, stride, offset;
};
struct virtio_gpu_rect { uint32_t x, y, width, height; };
typedef struct DisplaySurface {
    uint8_t *data;
    uint32_t width, height, stride, bytes_pp;
} DisplaySurface;
static uint8_t *surface_data(DisplaySurface *s) { return s->data; }
static uint32_t surface_stride(DisplaySurface *s) { return s->stride; }
static uint32_t surface_height(DisplaySurface *s) { return s->height; }
static uint32_t surface_bytes_per_pixel(DisplaySurface *s) { return s->bytes_pp; }

static int exported_fd = -1;
static uint32_t exported_type = VIRGL_RENDERER_BLOB_FD_TYPE_SHM;
#ifdef CONFIG_LINUX
static int gpu_calls, gpu_result;
static int virtio_gpu_neptune_readback_dmabuf(
    int fd, uint32_t id, const struct virtio_gpu_framebuffer *fb,
    const struct virtio_gpu_rect *source, uint32_t left, uint32_t top,
    uint32_t width, uint32_t height, DisplaySurface *surface)
{
    if (fd < 0 || !id || !fb || !source || !width || !height || !surface ||
        left < source->x || top < source->y) abort();
    gpu_calls++;
    return gpu_result;
}
#endif
static int virgl_renderer_resource_export_blob(uint32_t id, uint32_t *type,
                                               int *fd)
{
    (void)id;
    *type = exported_type;
    *fd = dup(exported_fd);
    return *fd < 0 ? -errno : 0;
}

/* BEGIN_EXTRACTED_HELPER: rewritten by test-blob-readback.py before compile. */
static int virtio_gpu_neptune_readback_blob(
    const struct virtio_gpu_virgl_resource *res,
    const struct virtio_gpu_framebuffer *fb,
    const struct virtio_gpu_rect *source,
    uint32_t left, uint32_t top, uint32_t width, uint32_t height,
    DisplaySurface *surface)
{
    struct stat st;
    uint64_t source_offset;
    uint64_t source_end;
    uint64_t surface_offset;
    uint64_t dst_stride;
    uint64_t surface_end;
    uint64_t mapped_size;
    size_t map_len;
    uint8_t *map;
    uint8_t *dst;
    uint32_t fd_type;
    int fd = -1;
    int ret;

    if (!res || !res->is_blob || !fb || !surface || !width || !height ||
        surface_bytes_per_pixel(surface) != fb->bytes_pp) {
        return -EINVAL;
    }

    ret = virgl_renderer_resource_export_blob(res->base.resource_id,
                                              &fd_type, &fd);
    if (ret) {
        return ret;
    }
    if (fd < 0 || fd_type != VIRGL_RENDERER_BLOB_FD_TYPE_SHM ||
        fstat(fd, &st) < 0 || st.st_size < 0) {
        ret = -EINVAL;
        goto out_close;
    }

    mapped_size = MIN(res->base.blob_size, (uint64_t)st.st_size);
    source_offset = (uint64_t)fb->offset +
                    (uint64_t)(top - source->y) * fb->stride +
                    (uint64_t)(left - source->x) * fb->bytes_pp;
    source_end = source_offset + (uint64_t)(height - 1) * fb->stride +
                 (uint64_t)width * fb->bytes_pp;
    dst_stride = surface_stride(surface);
    surface_offset = (uint64_t)(top - source->y) * dst_stride +
                     (uint64_t)(left - source->x) * fb->bytes_pp;
    surface_end = surface_offset + (uint64_t)(height - 1) * dst_stride +
                  (uint64_t)width * fb->bytes_pp;
    if (source_offset > mapped_size || source_end > mapped_size ||
        source_end > SIZE_MAX || !dst_stride ||
        surface_end > dst_stride * surface_height(surface)) {
        ret = -EINVAL;
        goto out_close;
    }

    map_len = source_end;
    map = mmap(NULL, map_len, PROT_READ, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        ret = -errno;
        goto out_close;
    }

    dst = surface_data(surface) + surface_offset;
    for (uint32_t row = 0; row < height; row++) {
        memcpy(dst + (uint64_t)row * dst_stride,
               map + source_offset + (uint64_t)row * fb->stride,
               (size_t)width * fb->bytes_pp);
    }
    munmap(map, map_len);
    ret = 0;

out_close:
    close(fd);
    return ret;
}
/* END_EXTRACTED_HELPER */

static int count_fds(void)
{
    int n = 0;
    for (int i = 0; i < 256; i++) {
        if (fcntl(i, F_GETFD) != -1 || errno != EBADF) n++;
    }
    return n;
}

static void fail(const char *message)
{
    fprintf(stderr, "FAIL: %s\n", message);
    exit(1);
}

int main(void)
{
    char path[] = "/tmp/triton-blob-readback.XXXXXX";
    uint8_t blob[160], dst[64];
    int fd = mkstemp(path);
    struct virtio_gpu_virgl_resource res = { .base = { 33, sizeof(blob) }, .is_blob = 1 };
    struct virtio_gpu_framebuffer fb = { .bytes_pp = 4, .width = 5, .height = 4,
                                         .stride = 28, .offset = 40 };
    struct virtio_gpu_rect source = { 1, 1, 3, 2 };
    DisplaySurface surface = { .data = dst, .width = 3, .height = 2,
                               .stride = 16, .bytes_pp = 4 };
    int fds;

    if (fd < 0) fail("mkstemp");
    unlink(path);
    memset(blob, 0xa5, sizeof(blob));
    /* BGRX pixels at base offset 8, with 28-byte padded source pitch. */
    for (uint32_t y = 0; y < 4; y++) {
        for (uint32_t x = 0; x < 5; x++) {
            uint8_t *p = blob + 8 + y * 28 + x * 4;
            p[0] = (uint8_t)(0x10 + x); p[1] = (uint8_t)(0x20 + y);
            p[2] = (uint8_t)(0x30 + x + y); p[3] = 0xff;
        }
    }
    if (write(fd, blob, sizeof(blob)) != sizeof(blob)) fail("write blob");
    exported_fd = fd;
    memset(dst, 0xcc, sizeof(dst));
    fds = count_fds();
    /* Cropped source and a partial flush: only global (2,2)+(2,1). */
    if (virtio_gpu_neptune_readback_blob(&res, &fb, &source, 2, 2, 2, 1,
                                         &surface) != 0) fail("partial copy");
    if (count_fds() != fds) fail("fd leak after copy");
    if (memcmp(dst + 16 + 4, blob + 8 + 2 * 28 + 2 * 4, 8) != 0)
        fail("BGRX/padded/nonzero-offset bytes");
    for (int i = 0; i < 64; i++) {
        if ((i < 20 || i >= 28) && dst[i] != 0xcc) fail("partial guard changed");
    }
    /* A short exported file must fail before changing the destination. */
    if (ftruncate(fd, 20) != 0) fail("truncate");
    memset(dst, 0xcc, sizeof(dst));
    if (virtio_gpu_neptune_readback_blob(&res, &fb, &source, 1, 1, 1, 1,
                                         &surface) != -EINVAL) fail("short file");
    for (int i = 0; i < 64; i++) if (dst[i] != 0xcc) fail("short guard changed");
    if (count_fds() != fds) fail("fd leak after short file");
    if (ftruncate(fd, sizeof(blob)) != 0) fail("restore size");
    exported_type = 2;
    if (virtio_gpu_neptune_readback_blob(&res, &fb, &source, 1, 1, 1, 1,
                                         &surface) != -EINVAL) fail("non-SHM type");
    if (count_fds() != fds) fail("fd leak after non-SHM");
#ifdef CONFIG_LINUX
    exported_type = VIRGL_RENDERER_BLOB_FD_TYPE_DMABUF;
    if (pwrite(fd, blob, sizeof(blob), 0) != sizeof(blob)) fail("restore pixels");
    memset(dst, 0xcc, sizeof(dst));
    gpu_calls = 0;
    gpu_result = 0;
    if (virtio_gpu_neptune_readback_blob(&res, &fb, &source, 2, 2, 2, 1,
                                         &surface) != 0) fail("dma-buf GPU dispatch");
    if (gpu_calls != 1) fail("dma-buf did not use GPU");
    if (count_fds() != fds) fail("dma-buf fd leak");
    gpu_result = -EIO;
    if (virtio_gpu_neptune_readback_blob(&res, &fb, &source, 1, 1, 1, 1,
                                         &surface) != -EIO) fail("GPU failure propagation");
    if (count_fds() != fds) fail("GPU failure fd leak");
    gpu_result = 0;
#endif
    exported_type = VIRGL_RENDERER_BLOB_FD_TYPE_SHM;
    for (int i = 0; i < 100; i++) {
        if (virtio_gpu_neptune_readback_blob(&res, &fb, &source, 1, 1, 1, 1,
                                             &surface) != 0) fail("repeat copy");
    }
    if (count_fds() != fds) fail("fd leak after repeat");
    close(fd);
    puts("PASS: blob readback BGRX, crop, padded pitch, short/type guards, fd cleanup");
    return 0;
}
