#include <glib.h>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <map>

#define VIRGL_VERSION_MAJOR 1
struct VirtIOGPU {
    struct { bool conf; } parent_obj;
    GHashTable *context_fence_watches = nullptr;
};
using VirtIOGPUGL = VirtIOGPU;
#define VIRTIO_GPU_GL(g) (g)
static bool virtio_gpu_neptune_enabled(bool conf) { return conf; }
struct Handler { void (*read)(void *); void *opaque; };
static std::map<int, Handler> handlers;
static std::map<uint32_t, int> descriptors;
static unsigned polls;
static void qemu_set_fd_handler(int fd, void (*read)(void *), void *, void *opaque)
{
    if (read) handlers[fd] = {read, opaque};
    else handlers.erase(fd);
}
static int virgl_renderer_context_get_poll_fd(uint32_t id) { return descriptors.at(id); }
static void virtio_gpu_virgl_fence_poll(VirtIOGPU *) { ++polls; }
static void trace_virtio_gpu_neptune_fence_wake(int) {}
/* SOURCE_UNDER_TEST */

int main()
{
    VirtIOGPU gpu{};
    descriptors = {{1, 10}, {2, 11}, {3, -1}, {4, 10}};
    virtio_gpu_watch_context_fences(&gpu, 1);
    assert(handlers.empty()); // Other backends retain their existing path.
    gpu.parent_obj.conf = true;
    virtio_gpu_watch_context_fences(&gpu, 3);
    assert(handlers.empty()); // Missing fd retains timer fallback.
    virtio_gpu_watch_context_fences(&gpu, 1);
    virtio_gpu_watch_context_fences(&gpu, 2);
    virtio_gpu_watch_context_fences(&gpu, 4);
    assert(handlers.size() == 2);
    handlers.at(10).read(handlers.at(10).opaque);
    assert(polls == 1);
    virtio_gpu_watch_context_fences(&gpu, 1);
    assert(handlers.size() == 2); // Replacing a context retains its shared fd.
    g_hash_table_remove(gpu.context_fence_watches, GUINT_TO_POINTER(1));
    assert(handlers.count(10) && handlers.count(11));
    handlers.at(10).read(handlers.at(10).opaque);
    assert(polls == 2);
    g_hash_table_remove(gpu.context_fence_watches, GUINT_TO_POINTER(4));
    assert(!handlers.count(10) && handlers.count(11));
    virtio_gpu_virgl_clear_fence_watches(&gpu);
    assert(!gpu.context_fence_watches && handlers.empty());
    virtio_gpu_virgl_clear_fence_watches(&gpu); // Repeated reset is harmless.
    virtio_gpu_watch_context_fences(&gpu, 1); // Fresh context after reset.
    virtio_gpu_watch_context_fences(&gpu, 4);
    assert(handlers.size() == 1);
    g_hash_table_remove(gpu.context_fence_watches, GUINT_TO_POINTER(4));
    assert(handlers.size() == 1); // Destroy in the opposite order too.
    descriptors[1] = 12;
    virtio_gpu_watch_context_fences(&gpu, 1);
    assert(!handlers.count(10) && handlers.count(12));
    virtio_gpu_virgl_clear_fence_watches(&gpu);
    puts("CONTEXT FENCE NOTIFICATION LIFETIME PASS");
}
