/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Wine virtgpu backend.  All DRM ioctls go through the unixlib
 * bridge.  WSI ops are shared with vtest_wine.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winternl.h>

#include "npt_renderer_wine_common.h"
#include "nptunix/npt_unixlib.h"

#include "neptune-protocol/npt_protocol_defs.h"

/* DRM UAPI constants; must match the unixlib's kernel headers. */
#define VIRTGPU_BLOB_MEM_HOST3D        0x0002
#define VIRTGPU_BLOB_FLAG_USE_MAPPABLE 0x0001
#define VIRTGPU_BLOB_FLAG_USE_SHAREABLE 0x0002
#define VIRTGPU_EXECBUF_FENCE_FD_OUT   0x02
#define VIRTGPU_EXECBUF_RING_IDX       0x04
#define VIRTGPU_PARAM_3D_FEATURES      1
#define VIRTGPU_PARAM_CAPSET_QUERY_FIX 2
#define VIRTGPU_PARAM_RESOURCE_BLOB    3
#define VIRTGPU_PARAM_HOST_VISIBLE     4
#define VIRTGPU_PARAM_CONTEXT_INIT     6
/* Linux flag values (not Windows) -- passed through to PRIME_HANDLE_TO_FD. */
#ifndef DRM_CLOEXEC
#define DRM_CLOEXEC 0x00080000
#endif
#ifndef DRM_RDWR
#define DRM_RDWR    0x00000002
#endif

#include "npt_renderer.h"

/* util/u_math.h conflicts with npt_common.h's Windows threading defs. */
static inline uint64_t npt_align64(uint64_t v, uint64_t a)
{
   return (v + a - 1) & ~(a - 1);
}
#define align64 npt_align64

struct npt_virtgpu_wine_shmem {
   struct npt_renderer_shmem base;
   uint32_t gem_handle;
};

struct npt_virtgpu_wine {
   struct npt_renderer base;

   CRITICAL_SECTION cs;
   int drm_fd;
};

static int unixlib_virtgpu_open(void)
{
   struct npt_unix_virtgpu_open_params p = { .result_fd = -1 };
   npt_wine_unix_call(npt_unix_virtgpu_open, &p);
   return p.result_fd;
}

static uint64_t unixlib_virtgpu_getparam(int fd, uint64_t param)
{
   struct npt_unix_virtgpu_getparam_params p = { .fd = fd, .param = param };
   npt_wine_unix_call(npt_unix_virtgpu_getparam, &p);
   return p.result ? 0 : p.value;
}

static int unixlib_virtgpu_get_caps(int fd, uint32_t id, uint32_t ver,
                                    void *capset, uint32_t size)
{
   struct npt_unix_virtgpu_get_caps_params p = {
      .fd = fd, .cap_set_id = id, .cap_set_ver = ver,
      .capset = capset, .size = size,
   };
   npt_wine_unix_call(npt_unix_virtgpu_get_caps, &p);
   return p.result;
}

static int unixlib_virtgpu_context_init(int fd, uint32_t capset_id)
{
   struct npt_unix_virtgpu_context_init_params p = {
      .fd = fd, .capset_id = capset_id,
   };
   npt_wine_unix_call(npt_unix_virtgpu_context_init, &p);
   return p.result;
}

static int unixlib_virtgpu_create_blob(int fd, uint32_t blob_mem,
                                       uint32_t blob_flags, uint64_t size,
                                       uint64_t blob_id,
                                       uint32_t *bo_handle, uint32_t *res_handle)
{
   struct npt_unix_virtgpu_create_blob_params p = {
      .fd = fd, .blob_mem = blob_mem, .blob_flags = blob_flags,
      .size = size, .blob_id = blob_id,
   };
   npt_wine_unix_call(npt_unix_virtgpu_create_blob, &p);
   if (p.result == 0) {
      *bo_handle = p.bo_handle;
      *res_handle = p.res_handle;
   }
   return p.result;
}

static void *unixlib_virtgpu_map(int fd, uint32_t gem_handle, uint64_t size)
{
   struct npt_unix_virtgpu_map_params p = {
      .fd = fd, .gem_handle = gem_handle, .size = size,
   };
   npt_wine_unix_call(npt_unix_virtgpu_map, &p);
   return p.result_ptr;
}

static int unixlib_virtgpu_execbuffer(int fd, uint32_t flags, uint32_t size,
                                      const void *cmd, uint32_t ring_idx,
                                      int *fence_fd)
{
   struct npt_unix_virtgpu_execbuffer_params p = {
      .fd = fd, .flags = flags, .size = size,
      .command = cmd, .ring_idx = ring_idx,
   };
   npt_wine_unix_call(npt_unix_virtgpu_execbuffer, &p);
   if (fence_fd)
      *fence_fd = p.fence_fd;
   return p.result;
}

static void unixlib_virtgpu_gem_close(int fd, uint32_t gem_handle)
{
   struct npt_unix_virtgpu_gem_close_params p = {
      .fd = fd, .gem_handle = gem_handle,
   };
   npt_wine_unix_call(npt_unix_virtgpu_gem_close, &p);
}

static int unixlib_virtgpu_prime_handle_to_fd(int fd, uint32_t gem_handle)
{
   struct npt_unix_virtgpu_prime_handle_to_fd_params p = {
      .fd = fd, .gem_handle = gem_handle,
      .flags = DRM_CLOEXEC | DRM_RDWR,
   };
   npt_wine_unix_call(npt_unix_virtgpu_prime_handle_to_fd, &p);
   return p.result_fd;
}

static struct npt_renderer_shmem *
npt_vgw_shmem_create(struct npt_renderer *r, size_t size)
{
   struct npt_virtgpu_wine *gpu = (struct npt_virtgpu_wine *)r;

   size_t alloc_size = (size_t)align64(size, 4096);

   uint32_t bo_handle, res_handle;
   if (unixlib_virtgpu_create_blob(gpu->drm_fd,
                                   VIRTGPU_BLOB_MEM_HOST3D,
                                   VIRTGPU_BLOB_FLAG_USE_MAPPABLE,
                                   alloc_size, 0,
                                   &bo_handle, &res_handle)) {
      npt_log("virtgpu shmem_create: create_blob failed");
      return NULL;
   }

   void *ptr = unixlib_virtgpu_map(gpu->drm_fd, bo_handle, alloc_size);
   if (!ptr) {
      npt_log("virtgpu shmem_create: map failed");
      unixlib_virtgpu_gem_close(gpu->drm_fd, bo_handle);
      return NULL;
   }

   struct npt_virtgpu_wine_shmem *s = npt_alloc(sizeof(*s));
   if (!s) {
      npt_wine_unixlib_munmap(ptr, alloc_size);
      unixlib_virtgpu_gem_close(gpu->drm_fd, bo_handle);
      return NULL;
   }
   npt_refcount_init(&s->base.refcount);
   s->base.res_id = res_handle;
   s->base.size = alloc_size;
   s->base.mmap_ptr = ptr;
   s->gem_handle = bo_handle;

   return &s->base;
}

static void
npt_vgw_shmem_destroy(struct npt_renderer *r, struct npt_renderer_shmem *_s)
{
   struct npt_virtgpu_wine *gpu = (struct npt_virtgpu_wine *)r;
   struct npt_virtgpu_wine_shmem *s = (struct npt_virtgpu_wine_shmem *)_s;
   if (!s)
      return;
   npt_wine_unixlib_munmap(s->base.mmap_ptr, s->base.size);
   unixlib_virtgpu_gem_close(gpu->drm_fd, s->gem_handle);
   free(s);
}

static bool
npt_vgw_submit_cmd(struct npt_renderer *r, const void *data, size_t size)
{
   struct npt_virtgpu_wine *gpu = (struct npt_virtgpu_wine *)r;

   EnterCriticalSection(&gpu->cs);
   int ret = unixlib_virtgpu_execbuffer(gpu->drm_fd,
                                        VIRTGPU_EXECBUF_RING_IDX,
                                        (uint32_t)size, data, 0, NULL);
   LeaveCriticalSection(&gpu->cs);

   if (ret) {
      npt_log("virtgpu submit_cmd failed");
      return false;
   }
   return true;
}

/* EXECBUF_FENCE_FD_OUT + unixlib poll(2) on the fence. */
static bool
npt_vgw_submit_cmd_sync(struct npt_renderer *r, const void *data, size_t size)
{
   struct npt_virtgpu_wine *gpu = (struct npt_virtgpu_wine *)r;

   struct npt_unix_virtgpu_execbuffer_params p = {
      .fd = gpu->drm_fd,
      .flags = VIRTGPU_EXECBUF_RING_IDX | VIRTGPU_EXECBUF_FENCE_FD_OUT,
      .size = (uint32_t)size,
      .command = data,
      .ring_idx = 0,
      .wait_for_fence = 1,
   };

   EnterCriticalSection(&gpu->cs);
   npt_wine_unix_call(npt_unix_virtgpu_execbuffer, &p);
   LeaveCriticalSection(&gpu->cs);

   if (p.result) {
      npt_log("virtgpu submit_cmd_sync failed");
      return false;
   }
   return true;
}

static int
npt_vgw_create_host_blob(struct npt_renderer *r,
                         uint64_t blob_id, uint64_t size)
{
   struct npt_virtgpu_wine *gpu = (struct npt_virtgpu_wine *)r;

   size = align64(size, 4096);

   uint32_t bo_handle, res_handle;
   if (unixlib_virtgpu_create_blob(gpu->drm_fd,
                                   VIRTGPU_BLOB_MEM_HOST3D,
                                   VIRTGPU_BLOB_FLAG_USE_MAPPABLE |
                                   VIRTGPU_BLOB_FLAG_USE_SHAREABLE,
                                   size, blob_id,
                                   &bo_handle, &res_handle)) {
      npt_log("virtgpu create_host_blob: create_blob failed for blob_id=%llu",
              (unsigned long long)blob_id);
      return -1;
   }

   int fd = unixlib_virtgpu_prime_handle_to_fd(gpu->drm_fd, bo_handle);
   unixlib_virtgpu_gem_close(gpu->drm_fd, bo_handle);

   if (fd < 0)
      npt_log("virtgpu create_host_blob: prime failed for blob_id=%llu",
              (unsigned long long)blob_id);
   else
      npt_debug("virtgpu create_host_blob: blob_id=%llu -> fd %d",
                (unsigned long long)blob_id, fd);

   return fd;
}

static int
npt_vgw_submit_present_fence(struct npt_renderer *r, uint32_t ring_idx)
{
   struct npt_virtgpu_wine *gpu = (struct npt_virtgpu_wine *)r;

   int fence_fd = -1;
   EnterCriticalSection(&gpu->cs);
   int ret = unixlib_virtgpu_execbuffer(
      gpu->drm_fd,
      VIRTGPU_EXECBUF_RING_IDX | VIRTGPU_EXECBUF_FENCE_FD_OUT,
      0, NULL, ring_idx, &fence_fd);
   LeaveCriticalSection(&gpu->cs);

   if (ret) {
      npt_log("virtgpu submit_present_fence failed: ring_idx=%u", ring_idx);
      return -1;
   }
   return fence_fd;
}

static void
npt_vgw_destroy(struct npt_renderer *r)
{
   struct npt_virtgpu_wine *gpu = (struct npt_virtgpu_wine *)r;

   if (gpu->drm_fd >= 0)
      npt_wine_unixlib_close(gpu->drm_fd);

   DeleteCriticalSection(&gpu->cs);
   free(gpu);
}

struct npt_renderer *
npt_renderer_create_virtgpu(void)
{
   if (npt_unixlib_ensure_init() < 0)
      return NULL;

   int drm_fd = unixlib_virtgpu_open();
   if (drm_fd < 0) {
      npt_debug("virtgpu: no virtio_gpu DRM device found");
      return NULL;
   }

   static const uint64_t required[] = {
      VIRTGPU_PARAM_3D_FEATURES,
      VIRTGPU_PARAM_CAPSET_QUERY_FIX,
      VIRTGPU_PARAM_RESOURCE_BLOB,
      VIRTGPU_PARAM_CONTEXT_INIT,
      VIRTGPU_PARAM_HOST_VISIBLE,
   };
   for (uint32_t i = 0; i < sizeof(required) / sizeof(required[0]); i++) {
      if (!unixlib_virtgpu_getparam(drm_fd, required[i])) {
         npt_debug("virtgpu: missing param %llu", (unsigned long long)required[i]);
         npt_wine_unixlib_close(drm_fd);
         return NULL;
      }
   }

   struct npt_capset capset;
   memset(&capset, 0, sizeof(capset));
   if (unixlib_virtgpu_get_caps(drm_fd, NPT_CAPSET_ID, 0,
                                &capset, sizeof(capset))) {
      npt_debug("virtgpu: failed to get Neptune capset");
      npt_wine_unixlib_close(drm_fd);
      return NULL;
   }
   if (capset.wire_format_version != NPT_PROTOCOL_WIRE_VERSION) {
      npt_debug("virtgpu: wire format mismatch: host 0x%08x, guest 0x%08x",
                capset.wire_format_version,
                (unsigned)NPT_PROTOCOL_WIRE_VERSION);
      npt_wine_unixlib_close(drm_fd);
      return NULL;
   }

   if (unixlib_virtgpu_context_init(drm_fd, NPT_CAPSET_ID)) {
      npt_debug("virtgpu: context init failed");
      npt_wine_unixlib_close(drm_fd);
      return NULL;
   }

   struct npt_virtgpu_wine *gpu = npt_alloc(sizeof(*gpu));
   if (!gpu) {
      npt_wine_unixlib_close(drm_fd);
      return NULL;
   }

   gpu->drm_fd = drm_fd;
   InitializeCriticalSection(&gpu->cs);

   gpu->base.info.max_timeline_count = 64;

   gpu->base.ops.destroy = npt_vgw_destroy;
   gpu->base.ops.submit_cmd = npt_vgw_submit_cmd;
   gpu->base.ops.submit_cmd_sync = npt_vgw_submit_cmd_sync;
   gpu->base.ops.create_host_blob = npt_vgw_create_host_blob;
   gpu->base.ops.submit_present_fence = npt_vgw_submit_present_fence;
   npt_wine_install_wsi_ops(&gpu->base.ops);
   gpu->base.shmem_ops.create = npt_vgw_shmem_create;
   gpu->base.shmem_ops.destroy = npt_vgw_shmem_destroy;

   npt_log("virtgpu renderer created (drm_fd=%d, wire_format=0x%08x)",
           gpu->drm_fd, capset.wire_format_version);

   return &gpu->base;
}
