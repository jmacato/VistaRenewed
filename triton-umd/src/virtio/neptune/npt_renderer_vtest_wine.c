/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Wine vtest backend; uses nptunix.dll for Unix socket / SCM_RIGHTS /
 * mmap via Wine's unixlib bridge.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winternl.h>

#include "npt_common.h"
#include "npt_renderer.h"
#include "npt_renderer_vtest_protocol.h"
#include "npt_renderer_wine_common.h"
#include "nptunix/npt_unixlib.h"
#include "vtest/vtest_protocol.h"

#include "neptune-protocol/npt_protocol_defs.h"

static int unixlib_connect(const char *path)
{
   struct npt_unix_connect_params p = { .path = path };
   npt_wine_unix_call(npt_unix_connect, &p);
   return p.result_fd;
}

static long unixlib_read(int fd, void *buf, size_t size)
{
   struct npt_unix_rw_params p = { .fd = fd, .buf = buf, .size = size };
   npt_wine_unix_call(npt_unix_read, &p);
   return (long)p.result;
}

static long unixlib_write(int fd, const void *buf, size_t size)
{
   struct npt_unix_rw_params p = { .fd = fd, .buf = (void *)buf, .size = size };
   npt_wine_unix_call(npt_unix_write, &p);
   return (long)p.result;
}

static int unixlib_receive_fd(int sock_fd)
{
   struct npt_unix_receive_fd_params p = { .sock_fd = sock_fd };
   npt_wine_unix_call(npt_unix_receive_fd, &p);
   return p.result_fd;
}

static void *unixlib_mmap(size_t sz, int fd)
{
   struct npt_unix_mmap_params p = { .size = sz, .fd = fd };
   npt_wine_unix_call(npt_unix_mmap, &p);
   return p.result_ptr;
}

/* sock_fd packed into ctx as intptr_t. */
static long wine_transport_read(void *ctx, void *buf, size_t size)
{
   return unixlib_read((int)(intptr_t)ctx, buf, size);
}

static long wine_transport_write(void *ctx, const void *buf, size_t size)
{
   return unixlib_write((int)(intptr_t)ctx, buf, size);
}

static int wine_transport_receive_fd(void *ctx)
{
   return unixlib_receive_fd((int)(intptr_t)ctx);
}

static const struct npt_vtest_transport_ops g_wine_transport_ops = {
   .read = wine_transport_read,
   .write = wine_transport_write,
   .receive_fd = wine_transport_receive_fd,
};

struct npt_vtest {
   struct npt_renderer base;
   struct npt_vtest_protocol proto;
   CRITICAL_SECTION sock_cs;
   int sock_fd;
};

static struct npt_renderer_shmem *
npt_vtest_shmem_create(struct npt_renderer *r, size_t sz)
{
   struct npt_vtest *v = (struct npt_vtest *)r;

   EnterCriticalSection(&v->sock_cs);
   int fd;
   uint32_t rid = npt_vtest_vcmd_resource_create_blob(
      &v->proto, VCMD_BLOB_TYPE_HOST3D_GUEST, VCMD_BLOB_FLAG_MAPPABLE,
      sz, 0, &fd);
   LeaveCriticalSection(&v->sock_cs);

   if (fd < 0) {
      npt_log("shmem_create: failed to receive memfd");
      return NULL;
   }

   void *ptr = unixlib_mmap(sz, fd);
   npt_wine_unixlib_close(fd);

   if (!ptr) {
      npt_log("shmem_create: mmap failed");
      EnterCriticalSection(&v->sock_cs);
      npt_vtest_vcmd_resource_unref(&v->proto, rid);
      LeaveCriticalSection(&v->sock_cs);
      return NULL;
   }

   struct npt_renderer_shmem *s = npt_alloc(sizeof(*s));
   if (!s) {
      npt_wine_unixlib_munmap(ptr, sz);
      EnterCriticalSection(&v->sock_cs);
      npt_vtest_vcmd_resource_unref(&v->proto, rid);
      LeaveCriticalSection(&v->sock_cs);
      return NULL;
   }
   npt_refcount_init(&s->refcount);
   s->res_id = rid;
   s->size = sz;
   s->mmap_ptr = ptr;
   return s;
}

static void
npt_vtest_shmem_destroy(struct npt_renderer *r, struct npt_renderer_shmem *s)
{
   struct npt_vtest *v = (struct npt_vtest *)r;
   if (!s) return;
   npt_wine_unixlib_munmap(s->mmap_ptr, s->size);
   EnterCriticalSection(&v->sock_cs);
   npt_vtest_vcmd_resource_unref(&v->proto, s->res_id);
   LeaveCriticalSection(&v->sock_cs);
   free(s);
}

static int
npt_vtest_create_host_blob(struct npt_renderer *r,
                           uint64_t blob_id, uint64_t size)
{
   struct npt_vtest *v = (struct npt_vtest *)r;

   EnterCriticalSection(&v->sock_cs);
   int fd;
   uint32_t res_id = npt_vtest_vcmd_resource_create_blob(
      &v->proto,
      VCMD_BLOB_TYPE_HOST3D,
      VCMD_BLOB_FLAG_MAPPABLE | VCMD_BLOB_FLAG_SHAREABLE,
      size,
      blob_id,
      &fd);
   LeaveCriticalSection(&v->sock_cs);

   (void)res_id;

   if (fd < 0)
      npt_log("create_host_blob: failed for blob_id=%llu", (unsigned long long)blob_id);
   else
      npt_debug("create_host_blob: blob_id=%llu -> fd %d", (unsigned long long)blob_id, fd);

   return fd;
}

static bool
npt_vtest_submit_cmd(struct npt_renderer *r, const void *data, size_t sz)
{
   struct npt_vtest *v = (struct npt_vtest *)r;
   EnterCriticalSection(&v->sock_cs);
   npt_vtest_vcmd_submit_cmd(&v->proto, data, sz);
   LeaveCriticalSection(&v->sock_cs);
   return true;
}

/* Empty SUBMIT_CMD2 batch with OUT_FENCE_FD + RING_IDX; fd via
 * unixlib SCM_RIGHTS (PE has no recvmsg). */
static int
npt_vtest_submit_present_fence(struct npt_renderer *r, uint32_t ring_idx)
{
   struct npt_vtest *v = (struct npt_vtest *)r;

   const uint32_t batch_count = 1;
   const uint32_t header_words = 1 + 8;

   uint32_t hdr[VTEST_HDR_SIZE];
   hdr[VTEST_CMD_LEN] = header_words;
   hdr[VTEST_CMD_ID] = VCMD_SUBMIT_CMD2;

   struct vcmd_submit_cmd2_batch batch;
   memset(&batch, 0, sizeof(batch));
   batch.flags = VCMD_SUBMIT_CMD2_FLAG_RING_IDX |
                 VCMD_SUBMIT_CMD2_FLAG_OUT_FENCE_FD;
   batch.cmd_offset = 0;
   batch.cmd_size = 0;
   batch.ring_idx = ring_idx;

   EnterCriticalSection(&v->sock_cs);
   npt_vtest_proto_write(&v->proto, hdr, sizeof(hdr));
   npt_vtest_proto_write(&v->proto, &batch_count, sizeof(batch_count));
   npt_vtest_proto_write(&v->proto, &batch, sizeof(batch));
   int fd = v->proto.transport->receive_fd(v->proto.transport_ctx);
   LeaveCriticalSection(&v->sock_cs);

   return fd;
}

static void
npt_vtest_destroy(struct npt_renderer *r)
{
   struct npt_vtest *v = (struct npt_vtest *)r;
   npt_wine_unixlib_close(v->sock_fd);
   DeleteCriticalSection(&v->sock_cs);
   free(v);
}

struct npt_renderer *
npt_renderer_create_vtest(void)
{
   if (npt_unixlib_ensure_init() < 0)
      return NULL;

   struct npt_vtest *v = npt_alloc(sizeof(*v));
   if (!v) return NULL;
   InitializeCriticalSection(&v->sock_cs);

   const char *sn = getenv("VTEST_SOCKET_NAME");
   if (!sn) sn = VTEST_DEFAULT_SOCKET_NAME;
   v->sock_fd = unixlib_connect(sn);
   if (v->sock_fd < 0) {
      npt_log("connect to %s failed (%d)", sn, v->sock_fd);
      DeleteCriticalSection(&v->sock_cs);
      free(v);
      return NULL;
   }

   v->proto.transport = &g_wine_transport_ops;
   v->proto.transport_ctx = (void *)(intptr_t)v->sock_fd;
   v->proto.protocol_version = 0;

   npt_vtest_vcmd_create_renderer(&v->proto, "neptune");
   if (npt_vtest_vcmd_ping_protocol_version(&v->proto))
      v->proto.protocol_version = npt_vtest_vcmd_protocol_version(&v->proto);

   if (v->proto.protocol_version < 3) {
      npt_log("protocol %u too old", v->proto.protocol_version);
      npt_wine_unixlib_close(v->sock_fd);
      DeleteCriticalSection(&v->sock_cs);
      free(v);
      return NULL;
   }
   npt_debug("vtest protocol: %u", v->proto.protocol_version);

   v->base.info.max_timeline_count =
      npt_vtest_vcmd_get_param(&v->proto, VCMD_PARAM_MAX_TIMELINE_COUNT);

   struct npt_capset cs;
   if (!npt_vtest_vcmd_get_capset(&v->proto, NPT_CAPSET_ID, 0, &cs, sizeof(cs))) {
      npt_log("failed to get Neptune capset");
      npt_wine_unixlib_close(v->sock_fd);
      DeleteCriticalSection(&v->sock_cs);
      free(v);
      return NULL;
   }
   if (cs.wire_format_version != NPT_PROTOCOL_WIRE_VERSION) {
      npt_log("wire format mismatch: host advertises 0x%08x, guest "
              "expects 0x%08x -- rebuild one side against the same "
              "neptune-protocol commit",
              cs.wire_format_version, (unsigned)NPT_PROTOCOL_WIRE_VERSION);
      npt_wine_unixlib_close(v->sock_fd);
      DeleteCriticalSection(&v->sock_cs);
      free(v);
      return NULL;
   }
   npt_debug("wire format: 0x%08x", cs.wire_format_version);

   npt_vtest_vcmd_context_init(&v->proto, NPT_CAPSET_ID);

   v->base.ops.destroy = npt_vtest_destroy;
   v->base.ops.submit_cmd = npt_vtest_submit_cmd;
   /* The TCP recv already blocks end-to-end. */
   v->base.ops.submit_cmd_sync = npt_vtest_submit_cmd;
   v->base.ops.create_host_blob = npt_vtest_create_host_blob;
   v->base.ops.submit_present_fence = npt_vtest_submit_present_fence;
   npt_wine_install_wsi_ops(&v->base.ops);
   v->base.shmem_ops.create = npt_vtest_shmem_create;
   v->base.shmem_ops.destroy = npt_vtest_shmem_destroy;

   return &v->base;
}
