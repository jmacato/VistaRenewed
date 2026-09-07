/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 */

#include <stdlib.h>
#include <string.h>

#include "npt_common.h"
#include "npt_renderer_vtest_protocol.h"
#include "vtest/vtest_protocol.h"

void
npt_vtest_proto_read(struct npt_vtest_protocol *p, void *buf, size_t size)
{
   uint8_t *ptr = buf;
   while (size) {
      long r = p->transport->read(p->transport_ctx, ptr, size);
      if (r <= 0) {
         npt_log("vtest read failed: r=%ld, remaining=%zu", r, size);
         abort();
      }
      ptr += r;
      size -= r;
   }
}

void
npt_vtest_proto_write(struct npt_vtest_protocol *p, const void *buf, size_t size)
{
   const uint8_t *ptr = buf;
   while (size) {
      long r = p->transport->write(p->transport_ctx, ptr, size);
      if (r <= 0) {
         npt_log("vtest write failed: r=%ld, remaining=%zu", r, size);
         abort();
      }
      ptr += r;
      size -= r;
   }
}

void
npt_vtest_vcmd_create_renderer(struct npt_vtest_protocol *p, const char *name)
{
   const size_t sz = strlen(name) + 1;
   uint32_t h[VTEST_HDR_SIZE];
   h[VTEST_CMD_LEN] = sz;
   h[VTEST_CMD_ID] = VCMD_CREATE_RENDERER;
   npt_vtest_proto_write(p, h, sizeof(h));
   npt_vtest_proto_write(p, name, sz);
}

bool
npt_vtest_vcmd_ping_protocol_version(struct npt_vtest_protocol *p)
{
   uint32_t h[VTEST_HDR_SIZE];
   h[VTEST_CMD_LEN] = VCMD_PING_PROTOCOL_VERSION_SIZE;
   h[VTEST_CMD_ID] = VCMD_PING_PROTOCOL_VERSION;
   npt_vtest_proto_write(p, h, sizeof(h));

   uint32_t busy[VCMD_BUSY_WAIT_SIZE];
   h[VTEST_CMD_LEN] = VCMD_BUSY_WAIT_SIZE;
   h[VTEST_CMD_ID] = VCMD_RESOURCE_BUSY_WAIT;
   busy[VCMD_BUSY_WAIT_HANDLE] = 0;
   busy[VCMD_BUSY_WAIT_FLAGS] = 0;
   npt_vtest_proto_write(p, h, sizeof(h));
   npt_vtest_proto_write(p, busy, sizeof(busy));

   npt_vtest_proto_read(p, h, sizeof(h));
   if (h[VTEST_CMD_ID] == VCMD_PING_PROTOCOL_VERSION) {
      npt_vtest_proto_read(p, h, sizeof(h));
      uint32_t dummy;
      for (uint32_t i = 0; i < h[VTEST_CMD_LEN]; i++)
         npt_vtest_proto_read(p, &dummy, sizeof(dummy));
      return true;
   }
   uint32_t dummy;
   for (uint32_t i = 0; i < h[VTEST_CMD_LEN]; i++)
      npt_vtest_proto_read(p, &dummy, sizeof(dummy));
   return false;
}

uint32_t
npt_vtest_vcmd_protocol_version(struct npt_vtest_protocol *p)
{
   uint32_t h[VTEST_HDR_SIZE], d[VCMD_PROTOCOL_VERSION_SIZE];
   h[VTEST_CMD_LEN] = VCMD_PROTOCOL_VERSION_SIZE;
   h[VTEST_CMD_ID] = VCMD_PROTOCOL_VERSION;
   d[VCMD_PROTOCOL_VERSION_VERSION] = VTEST_PROTOCOL_VERSION;
   npt_vtest_proto_write(p, h, sizeof(h));
   npt_vtest_proto_write(p, d, sizeof(d));
   npt_vtest_proto_read(p, h, sizeof(h));
   npt_vtest_proto_read(p, d, sizeof(d));
   return d[VCMD_PROTOCOL_VERSION_VERSION];
}

uint32_t
npt_vtest_vcmd_get_param(struct npt_vtest_protocol *p, uint32_t param)
{
   uint32_t h[VTEST_HDR_SIZE], d[VCMD_GET_PARAM_SIZE];
   h[VTEST_CMD_LEN] = VCMD_GET_PARAM_SIZE;
   h[VTEST_CMD_ID] = VCMD_GET_PARAM;
   d[VCMD_GET_PARAM_PARAM] = param;
   npt_vtest_proto_write(p, h, sizeof(h));
   npt_vtest_proto_write(p, d, sizeof(d));
   npt_vtest_proto_read(p, h, sizeof(h));
   uint32_t resp[2];
   npt_vtest_proto_read(p, resp, sizeof(resp));
   return resp[0] ? resp[1] : 0;
}

bool
npt_vtest_vcmd_get_capset(struct npt_vtest_protocol *p,
                          uint32_t id, uint32_t version,
                          void *capset, size_t capset_size)
{
   uint32_t h[VTEST_HDR_SIZE], d[VCMD_GET_CAPSET_SIZE];
   h[VTEST_CMD_LEN] = VCMD_GET_CAPSET_SIZE;
   h[VTEST_CMD_ID] = VCMD_GET_CAPSET;
   d[VCMD_GET_CAPSET_ID] = id;
   d[VCMD_GET_CAPSET_VERSION] = version;
   npt_vtest_proto_write(p, h, sizeof(h));
   npt_vtest_proto_write(p, d, sizeof(d));
   npt_vtest_proto_read(p, h, sizeof(h));

   uint32_t valid;
   npt_vtest_proto_read(p, &valid, sizeof(valid));
   size_t read_size = (h[VTEST_CMD_LEN] - 1) * 4;
   if (!valid) {
      while (read_size > 0) {
         uint32_t dummy;
         size_t n = read_size < sizeof(dummy) ? read_size : sizeof(dummy);
         npt_vtest_proto_read(p, &dummy, n);
         read_size -= n;
      }
      return false;
   }
   if (capset_size >= read_size) {
      npt_vtest_proto_read(p, capset, read_size);
      memset((uint8_t *)capset + read_size, 0, capset_size - read_size);
   } else {
      npt_vtest_proto_read(p, capset, capset_size);
      size_t skip = read_size - capset_size;
      while (skip > 0) {
         uint32_t dummy;
         size_t n = skip < sizeof(dummy) ? skip : sizeof(dummy);
         npt_vtest_proto_read(p, &dummy, n);
         skip -= n;
      }
   }
   return true;
}

void
npt_vtest_vcmd_context_init(struct npt_vtest_protocol *p, uint32_t capset_id)
{
   uint32_t h[VTEST_HDR_SIZE], d[VCMD_CONTEXT_INIT_SIZE];
   h[VTEST_CMD_LEN] = VCMD_CONTEXT_INIT_SIZE;
   h[VTEST_CMD_ID] = VCMD_CONTEXT_INIT;
   d[VCMD_CONTEXT_INIT_CAPSET_ID] = capset_id;
   npt_vtest_proto_write(p, h, sizeof(h));
   npt_vtest_proto_write(p, d, sizeof(d));
}

uint32_t
npt_vtest_vcmd_resource_create_blob(struct npt_vtest_protocol *p,
                                    uint32_t blob_type, uint32_t blob_flags,
                                    uint64_t size, uint64_t blob_id,
                                    int *out_fd)
{
   uint32_t h[VTEST_HDR_SIZE], d[VCMD_RES_CREATE_BLOB_SIZE];
   h[VTEST_CMD_LEN] = VCMD_RES_CREATE_BLOB_SIZE;
   h[VTEST_CMD_ID] = VCMD_RESOURCE_CREATE_BLOB;
   d[VCMD_RES_CREATE_BLOB_TYPE] = blob_type;
   d[VCMD_RES_CREATE_BLOB_FLAGS] = blob_flags;
   d[VCMD_RES_CREATE_BLOB_SIZE_LO] = (uint32_t)size;
   d[VCMD_RES_CREATE_BLOB_SIZE_HI] = (uint32_t)(size >> 32);
   d[VCMD_RES_CREATE_BLOB_ID_LO] = (uint32_t)blob_id;
   d[VCMD_RES_CREATE_BLOB_ID_HI] = (uint32_t)(blob_id >> 32);
   npt_vtest_proto_write(p, h, sizeof(h));
   npt_vtest_proto_write(p, d, sizeof(d));

   npt_vtest_proto_read(p, h, sizeof(h));
   uint32_t res_id;
   npt_vtest_proto_read(p, &res_id, sizeof(res_id));

   *out_fd = p->transport->receive_fd(p->transport_ctx);

   return res_id;
}

void
npt_vtest_vcmd_resource_unref(struct npt_vtest_protocol *p, uint32_t res_id)
{
   uint32_t h[VTEST_HDR_SIZE], d[VCMD_RES_UNREF_SIZE];
   h[VTEST_CMD_LEN] = VCMD_RES_UNREF_SIZE;
   h[VTEST_CMD_ID] = VCMD_RESOURCE_UNREF;
   d[VCMD_RES_UNREF_RES_HANDLE] = res_id;
   npt_vtest_proto_write(p, h, sizeof(h));
   npt_vtest_proto_write(p, d, sizeof(d));
}

void
npt_vtest_vcmd_submit_cmd(struct npt_vtest_protocol *p, const void *data, size_t sz)
{
   uint32_t cw = (uint32_t)(sz / 4), hw = 1 + 8;
   uint32_t h[VTEST_HDR_SIZE];
   h[VTEST_CMD_LEN] = hw + cw;
   h[VTEST_CMD_ID] = VCMD_SUBMIT_CMD2;
   uint32_t bc = 1;
   struct vcmd_submit_cmd2_batch b;
   memset(&b, 0, sizeof(b));
   b.flags = VCMD_SUBMIT_CMD2_FLAG_RING_IDX;
   b.cmd_offset = hw;
   b.cmd_size = cw;
   npt_vtest_proto_write(p, h, sizeof(h));
   npt_vtest_proto_write(p, &bc, 4);
   npt_vtest_proto_write(p, &b, sizeof(b));
   if (sz) npt_vtest_proto_write(p, data, sz);
}
