/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <xf86drm.h>
#include "drm-uapi/virtgpu_drm.h"

#include "npt_unix_internal.h"
#include "npt_unixlib.h"

NTSTATUS npt_do_virtgpu_open(void *args)
{
   struct npt_unix_virtgpu_open_params *p = args;
   p->result_fd = -1;

   char render[256];
   if (npt_pick_drm_device_paths(NULL, 0, render, sizeof(render)) != 0)
      return STATUS_SUCCESS;
   if (!render[0])
      return STATUS_SUCCESS;

   int fd = open(render, O_RDWR | O_CLOEXEC);
   if (fd < 0)
      return STATUS_SUCCESS;

   drmVersionPtr ver = drmGetVersion(fd);
   if (!ver || strcmp(ver->name, "virtio_gpu") || ver->version_major != 0) {
      if (ver) drmFreeVersion(ver);
      close(fd);
      return STATUS_SUCCESS;
   }
   drmFreeVersion(ver);
   p->result_fd = fd;
   return STATUS_SUCCESS;
}

NTSTATUS npt_do_virtgpu_getparam(void *args)
{
   struct npt_unix_virtgpu_getparam_params *p = args;
   p->value = 0;
   struct drm_virtgpu_getparam gp = {
      .param = p->param,
      .value = (uintptr_t)&p->value,
   };
   p->result = drmIoctl(p->fd, DRM_IOCTL_VIRTGPU_GETPARAM, &gp);
   return STATUS_SUCCESS;
}

NTSTATUS npt_do_virtgpu_get_caps(void *args)
{
   struct npt_unix_virtgpu_get_caps_params *p = args;
   struct drm_virtgpu_get_caps gc = {
      .cap_set_id = p->cap_set_id,
      .cap_set_ver = p->cap_set_ver,
      .addr = (uintptr_t)p->capset,
      .size = p->size,
   };
   p->result = drmIoctl(p->fd, DRM_IOCTL_VIRTGPU_GET_CAPS, &gc);
   return STATUS_SUCCESS;
}

NTSTATUS npt_do_virtgpu_context_init(void *args)
{
   struct npt_unix_virtgpu_context_init_params *p = args;
   struct drm_virtgpu_context_set_param params[3] = {
      { .param = VIRTGPU_CONTEXT_PARAM_CAPSET_ID,       .value = p->capset_id },
      { .param = VIRTGPU_CONTEXT_PARAM_NUM_RINGS,       .value = 64 },
      { .param = VIRTGPU_CONTEXT_PARAM_POLL_RINGS_MASK, .value = 0 },
   };
   struct drm_virtgpu_context_init ci = {
      .num_params = 3,
      .ctx_set_params = (uintptr_t)params,
   };
   p->result = drmIoctl(p->fd, DRM_IOCTL_VIRTGPU_CONTEXT_INIT, &ci);
   return STATUS_SUCCESS;
}

NTSTATUS npt_do_virtgpu_create_blob(void *args)
{
   struct npt_unix_virtgpu_create_blob_params *p = args;
   struct drm_virtgpu_resource_create_blob cb = {
      .blob_mem = p->blob_mem,
      .blob_flags = p->blob_flags,
      .size = p->size,
      .blob_id = p->blob_id,
   };
   p->result = drmIoctl(p->fd, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB, &cb);
   if (p->result == 0) {
      p->bo_handle = cb.bo_handle;
      p->res_handle = cb.res_handle;
   }
   return STATUS_SUCCESS;
}

NTSTATUS npt_do_virtgpu_map(void *args)
{
   struct npt_unix_virtgpu_map_params *p = args;
   p->result_ptr = NULL;

   struct drm_virtgpu_map mp = { .handle = p->gem_handle };
   if (drmIoctl(p->fd, DRM_IOCTL_VIRTGPU_MAP, &mp))
      return STATUS_SUCCESS;

   void *ptr = mmap(NULL, p->size, PROT_READ | PROT_WRITE,
                    MAP_SHARED, p->fd, mp.offset);
   if (ptr == MAP_FAILED)
      return STATUS_SUCCESS;

   p->result_ptr = ptr;
   return STATUS_SUCCESS;
}

NTSTATUS npt_do_virtgpu_execbuffer(void *args)
{
   struct npt_unix_virtgpu_execbuffer_params *p = args;
   struct drm_virtgpu_execbuffer eb = {
      .flags = p->flags,
      .size = p->size,
      .command = (uintptr_t)p->command,
      .ring_idx = p->ring_idx,
   };
   p->result = drmIoctl(p->fd, DRM_IOCTL_VIRTGPU_EXECBUFFER, &eb);
   p->fence_fd = (p->result == 0) ? eb.fence_fd : -1;

   /* PE has no poll(); the renderer sets wait_for_fence to delegate. */
   if (p->wait_for_fence && p->result == 0 && p->fence_fd >= 0) {
      struct pollfd pfd = { .fd = p->fence_fd, .events = POLLIN };
      int rc;
      do {
         rc = poll(&pfd, 1, -1);
      } while (rc < 0 && errno == EINTR);
      if (rc < 0)
         p->result = -1;
      close(p->fence_fd);
      p->fence_fd = -1;
   }
   return STATUS_SUCCESS;
}

NTSTATUS npt_do_virtgpu_gem_close(void *args)
{
   struct npt_unix_virtgpu_gem_close_params *p = args;
   struct drm_gem_close gc = { .handle = p->gem_handle };
   drmIoctl(p->fd, DRM_IOCTL_GEM_CLOSE, &gc);
   return STATUS_SUCCESS;
}

NTSTATUS npt_do_virtgpu_prime_handle_to_fd(void *args)
{
   struct npt_unix_virtgpu_prime_handle_to_fd_params *p = args;
   struct drm_prime_handle ph = {
      .handle = p->gem_handle,
      .flags = p->flags,
   };
   if (drmIoctl(p->fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &ph)) {
      p->result_fd = -1;
   } else {
      p->result_fd = ph.fd;
   }
   return STATUS_SUCCESS;
}
