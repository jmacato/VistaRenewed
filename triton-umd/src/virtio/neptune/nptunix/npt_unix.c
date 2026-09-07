/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Unix side of the Wine unixlib (mmdevapi helper-builtin pattern):
 * built as a plain ELF .so paired with nptunix.dll.  This file holds
 * the basic POSIX wrappers and the dispatch table; WSI and virtgpu
 * handlers live in npt_unix_wsi.c / npt_unix_virtgpu.c.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include "../npt_display.h"
#include "npt_unix_internal.h"
#include "npt_unixlib.h"

static NTSTATUS npt_do_connect(void *args)
{
   struct npt_unix_connect_params *p = args;
   int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
   if (s < 0) { p->result_fd = -errno; return STATUS_SUCCESS; }

   struct sockaddr_un un;
   memset(&un, 0, sizeof(un));
   un.sun_family = AF_UNIX;
   snprintf(un.sun_path, sizeof(un.sun_path), "%s", p->path);

   if (connect(s, (struct sockaddr *)&un, sizeof(un)) < 0) {
      int e = errno; close(s); p->result_fd = -e;
      return STATUS_SUCCESS;
   }
   p->result_fd = s;
   return STATUS_SUCCESS;
}

static NTSTATUS npt_do_read(void *args)
{
   struct npt_unix_rw_params *p = args;
   p->result = read(p->fd, p->buf, p->size);
   return STATUS_SUCCESS;
}

static NTSTATUS npt_do_write(void *args)
{
   struct npt_unix_rw_params *p = args;
   p->result = write(p->fd, p->buf, p->size);
   return STATUS_SUCCESS;
}

static NTSTATUS npt_do_close(void *args)
{
   struct npt_unix_close_params *p = args;
   close(p->fd);
   return STATUS_SUCCESS;
}

static NTSTATUS npt_do_dup(void *args)
{
   struct npt_unix_dup_params *p = args;
   p->result = (p->fd >= 0) ? dup(p->fd) : -1;
   return STATUS_SUCCESS;
}

static NTSTATUS npt_do_wait_fd(void *args)
{
   struct npt_unix_wait_fd_params *p = args;
   struct pollfd pfd = { .fd = p->fd, .events = POLLIN, .revents = 0 };
   int ret;
   do { ret = poll(&pfd, 1, p->timeout_ms); } while (ret < 0 && errno == EINTR);
   p->result = ret;
   return STATUS_SUCCESS;
}

static NTSTATUS npt_do_receive_fd(void *args)
{
   struct npt_unix_receive_fd_params *p = args;
   char cmsg_buf[CMSG_SPACE(sizeof(int))];
   char dummy;
   struct iovec iov = { .iov_base = &dummy, .iov_len = 1 };
   struct msghdr msg = {
      .msg_iov = &iov, .msg_iovlen = 1,
      .msg_control = cmsg_buf, .msg_controllen = sizeof(cmsg_buf),
   };
   if (recvmsg(p->sock_fd, &msg, 0) < 0) {
      p->result_fd = -1; return STATUS_SUCCESS;
   }
   struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
   p->result_fd = (c && c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS)
                  ? *(int *)CMSG_DATA(c) : -1;
   return STATUS_SUCCESS;
}

static NTSTATUS npt_do_mmap(void *args)
{
   struct npt_unix_mmap_params *p = args;
   void *r = mmap(NULL, p->size, PROT_READ | PROT_WRITE, MAP_SHARED, p->fd, 0);
   p->result_ptr = (r == MAP_FAILED) ? NULL : r;
   return STATUS_SUCCESS;
}

static NTSTATUS npt_do_munmap(void *args)
{
   struct npt_unix_munmap_params *p = args;
   munmap(p->addr, p->size);
   return STATUS_SUCCESS;
}

static NTSTATUS npt_do_enumerate_outputs(void *args)
{
   struct npt_unix_enumerate_outputs_params *p = args;
   bool expose_all_modes = p->expose_all_modes != 0;
   memset(&p->list, 0, sizeof(p->list));
   (void)npt_enumerate_outputs(&p->list, expose_all_modes);
   return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------
 * Process-wide singleton state.  The unixlib is loaded once per Wine
 * process and its static data is shared across all PE DLLs (d3d11.dll,
 * dxgi.dll) that use it, so this is the natural home for the
 * npt_device singleton.  See struct npt_unix_singleton_*_params in
 * npt_unixlib.h for the protocol.
 * --------------------------------------------------------------------- */
static pthread_mutex_t g_singleton_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_singleton_cond = PTHREAD_COND_INITIALIZER;
static void *g_singleton_device;
static uint32_t g_singleton_use_count;
static int g_singleton_creating;

static NTSTATUS npt_do_singleton_acquire(void *args)
{
   struct npt_unix_singleton_acquire_params *p = args;
   pthread_mutex_lock(&g_singleton_lock);
   while (g_singleton_creating)
      pthread_cond_wait(&g_singleton_cond, &g_singleton_lock);
   if (g_singleton_device) {
      g_singleton_use_count++;
      p->device_out = g_singleton_device;
      p->won_creation = 0;
   } else {
      g_singleton_creating = 1;
      p->device_out = NULL;
      p->won_creation = 1;
   }
   pthread_mutex_unlock(&g_singleton_lock);
   return STATUS_SUCCESS;
}

static NTSTATUS npt_do_singleton_publish(void *args)
{
   struct npt_unix_singleton_publish_params *p = args;
   pthread_mutex_lock(&g_singleton_lock);
   if (p->success) {
      g_singleton_device = p->device;
      g_singleton_use_count = 1;
   }
   g_singleton_creating = 0;
   pthread_cond_broadcast(&g_singleton_cond);
   pthread_mutex_unlock(&g_singleton_lock);
   return STATUS_SUCCESS;
}

static NTSTATUS npt_do_singleton_release(void *args)
{
   struct npt_unix_singleton_release_params *p = args;
   p->device_out = NULL;
   pthread_mutex_lock(&g_singleton_lock);
   if (g_singleton_use_count > 0 && --g_singleton_use_count == 0) {
      p->device_out = g_singleton_device;
      g_singleton_device = NULL;
   }
   pthread_mutex_unlock(&g_singleton_lock);
   return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------
 * Process-wide guest object id allocator.  See struct
 * npt_unix_alloc_obj_id_params in npt_unixlib.h.
 * --------------------------------------------------------------------- */
static pthread_mutex_t g_obj_id_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_obj_id_next = 1;  /* 0 is reserved for "no object" */

static NTSTATUS npt_do_alloc_obj_id(void *args)
{
   struct npt_unix_alloc_obj_id_params *p = args;
   pthread_mutex_lock(&g_obj_id_lock);
   p->result_id = g_obj_id_next++;
   pthread_mutex_unlock(&g_obj_id_lock);
   return STATUS_SUCCESS;
}

extern NTSTATUS npt_do_wsi_init(void *);
extern NTSTATUS npt_do_wsi_present(void *);
extern NTSTATUS npt_do_wsi_destroy(void *);
extern NTSTATUS npt_do_wsi_drain(void *);
extern NTSTATUS npt_do_virtgpu_open(void *);
extern NTSTATUS npt_do_virtgpu_getparam(void *);
extern NTSTATUS npt_do_virtgpu_get_caps(void *);
extern NTSTATUS npt_do_virtgpu_context_init(void *);
extern NTSTATUS npt_do_virtgpu_create_blob(void *);
extern NTSTATUS npt_do_virtgpu_map(void *);
extern NTSTATUS npt_do_virtgpu_execbuffer(void *);
extern NTSTATUS npt_do_virtgpu_gem_close(void *);
extern NTSTATUS npt_do_virtgpu_prime_handle_to_fd(void *);

DECLSPEC_EXPORT const unixlib_entry_t __wine_unix_call_funcs[] = {
   npt_do_connect,
   npt_do_read,
   npt_do_write,
   npt_do_close,
   npt_do_receive_fd,
   npt_do_mmap,
   npt_do_munmap,
   npt_do_wait_fd,
   npt_do_wsi_init,
   npt_do_wsi_present,
   npt_do_wsi_destroy,
   npt_do_wsi_drain,
   npt_do_virtgpu_open,
   npt_do_virtgpu_getparam,
   npt_do_virtgpu_get_caps,
   npt_do_virtgpu_context_init,
   npt_do_virtgpu_create_blob,
   npt_do_virtgpu_map,
   npt_do_virtgpu_execbuffer,
   npt_do_virtgpu_gem_close,
   npt_do_virtgpu_prime_handle_to_fd,
   npt_do_enumerate_outputs,
   npt_do_dup,
   npt_do_singleton_acquire,
   npt_do_singleton_publish,
   npt_do_singleton_release,
   npt_do_alloc_obj_id,
};
