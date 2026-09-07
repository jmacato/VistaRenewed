/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Shared interface between Wine PE DLLs and the nptunix.so unixlib
 * bridge.  enum npt_unix_funcs enumerates every operation the .so
 * exports.
 */

#ifndef NPT_UNIXLIB_H
#define NPT_UNIXLIB_H

#include <stdint.h>

#include "../npt_display.h"

#define NPT_WSI_MAX_IMAGES 4

struct npt_unix_connect_params {
   const char *path;
   int result_fd;
};

struct npt_unix_rw_params {
   int fd;
   void *buf;
   uint64_t size;
   int64_t result;
};

struct npt_unix_close_params {
   int fd;
};

struct npt_unix_dup_params {
   int fd;
   /* >=0 the duplicated fd, -1 on error. */
   int result;
};

struct npt_unix_wait_fd_params {
   int fd;
   int timeout_ms;
   /* >0 signaled, 0 timeout, <0 error. */
   int result;
};

struct npt_unix_receive_fd_params {
   int sock_fd;
   int result_fd;
};

struct npt_unix_mmap_params {
   uint64_t size;
   int fd;
   void *result_ptr;
};

struct npt_unix_munmap_params {
   void *addr;
   uint64_t size;
};

struct npt_unix_wsi_init_params {
   uint32_t x11_window;
   uint32_t num_images;
   int fds[NPT_WSI_MAX_IMAGES];
   uint32_t strides[NPT_WSI_MAX_IMAGES];
   uint32_t virgl_format;
   uint32_t width;
   uint32_t height;
   int result;
};

/*
 * wait_fence_fd: sync_file (or -1) signalling host GPU done.  The
 * unixlib must close it after passing to xcb_dri3_fence_from_fd
 * (DRI3 >= 1.2) or after a poll(2) fallback; sets wait_fence_consumed
 * if it did, otherwise the PE side closes via the bridge.
 *
 * Per-image release tracking goes through npt_unix_wsi_drain instead.
 */
struct npt_unix_wsi_present_params {
   uint32_t image_index;
   int result;
   int wait_fence_fd;
   uint32_t wait_fence_consumed;
};

/*
 * Per-image release COUNTS (NOT a bitmask): a fast client can fire
 * multiple idles for the same image between drain polls; a bitmask
 * would collapse them and stall the host's pending_presents.
 * timeout_ms caps the wait so the forwarder can recheck its exit flag.
 */
struct npt_unix_wsi_drain_params {
   uint32_t timeout_ms;
   uint32_t released_count[NPT_WSI_MAX_IMAGES];
   int result;
   uint32_t pad;
};

struct npt_unix_virtgpu_open_params {
   int result_fd;
};

struct npt_unix_virtgpu_getparam_params {
   int fd;
   uint64_t param;
   uint64_t value;
   int result;
};

struct npt_unix_virtgpu_get_caps_params {
   int fd;
   uint32_t cap_set_id;
   uint32_t cap_set_ver;
   void *capset;
   uint32_t size;
   int result;
};

struct npt_unix_virtgpu_context_init_params {
   int fd;
   uint32_t capset_id;
   int result;
};

struct npt_unix_virtgpu_create_blob_params {
   int fd;
   uint32_t blob_mem;
   uint32_t blob_flags;
   uint64_t size;
   uint64_t blob_id;
   uint32_t bo_handle;
   uint32_t res_handle;
   int result;
};

struct npt_unix_virtgpu_map_params {
   int fd;
   uint32_t gem_handle;
   uint64_t size;
   void *result_ptr;
};

struct npt_unix_virtgpu_execbuffer_params {
   int fd;
   uint32_t flags;
   uint32_t size;
   const void *command;
   uint32_t ring_idx;
   /* Out if FENCE_FD_OUT, else -1. */
   int fence_fd;
   int result;
   /* With FENCE_FD_OUT: unixlib poll(2)s + closes the fence; PE has
    * no poll(). */
   uint32_t wait_for_fence;
};

struct npt_unix_virtgpu_gem_close_params {
   int fd;
   uint32_t gem_handle;
};

struct npt_unix_virtgpu_prime_handle_to_fd_params {
   int fd;
   uint32_t gem_handle;
   uint32_t flags;
   int result_fd;
};

/* Walks every connected connector on the picked DRM device (viogpu
 * preferred, KMS fallback).  list.num_outputs == 0 if no DRM device
 * was usable; caller fabricates a synthetic output in that case. */
struct npt_unix_enumerate_outputs_params {
   struct npt_output_list list;
   /* When zero, the mode list is capped to the active CRTC scanout; the
    * PE side sets it from NPT_DEBUG=expose_all_modes. */
   uint32_t expose_all_modes;
};

/*
 * Process-wide npt_device singleton bridge (see npt_device.c).
 *
 * acquire: caller obtains the current device.  If `won_creation=1`,
 *   the caller has been elected to create the device and must call
 *   publish afterwards (success or failure).  Concurrent acquirers
 *   sleep on a cond var while creation is in flight.
 * publish: caller (won_creation==1) reports the result of npt_device_create.
 * release: dec refcount; if `device_out != NULL`, caller must destroy it.
 */
struct npt_unix_singleton_acquire_params {
   void *device_out;
   uint32_t won_creation;
   uint32_t pad;
};

struct npt_unix_singleton_publish_params {
   void *device;
   uint32_t success;
   uint32_t pad;
};

struct npt_unix_singleton_release_params {
   void *device_out;
   uint32_t pad0, pad1;
};

/*
 * Process-wide guest object id allocator.  npt_com_allocate_next_id
 * generates monotonically-increasing 64-bit ids that the encoder
 * stashes into output COM-handle slots before submit.  These ids
 * must be unique across PE DLLs in the same Wine process because
 * they share one host context (see the singleton bridge above) and
 * therefore one host-side object table.
 */
struct npt_unix_alloc_obj_id_params {
   uint64_t result_id;
};

enum npt_unix_funcs {
   npt_unix_connect,
   npt_unix_read,
   npt_unix_write,
   npt_unix_close,
   npt_unix_receive_fd,
   npt_unix_mmap,
   npt_unix_munmap,
   npt_unix_wait_fd,
   npt_unix_wsi_init,
   npt_unix_wsi_present,
   npt_unix_wsi_destroy,
   npt_unix_wsi_drain,
   npt_unix_virtgpu_open,
   npt_unix_virtgpu_getparam,
   npt_unix_virtgpu_get_caps,
   npt_unix_virtgpu_context_init,
   npt_unix_virtgpu_create_blob,
   npt_unix_virtgpu_map,
   npt_unix_virtgpu_execbuffer,
   npt_unix_virtgpu_gem_close,
   npt_unix_virtgpu_prime_handle_to_fd,
   npt_unix_enumerate_outputs,
   npt_unix_dup,
   npt_unix_singleton_acquire,
   npt_unix_singleton_publish,
   npt_unix_singleton_release,
   npt_unix_alloc_obj_id,
   npt_unix_funcs_count,
};

#endif /* NPT_UNIXLIB_H */
