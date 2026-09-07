/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Transport-agnostic vtest VCMD protocol.  Backends supply a
 * transport ops table; the protocol never touches the fd directly.
 */
#ifndef NPT_RENDERER_VTEST_PROTOCOL_H
#define NPT_RENDERER_VTEST_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* read/write follow POSIX (>0 partial/full, 0 EOF, <0 error); the
 * protocol's loops handle shorts.  receive_fd returns -1 on failure. */
struct npt_vtest_transport_ops {
   long (*read)(void *ctx, void *buf, size_t size);
   long (*write)(void *ctx, const void *buf, size_t size);
   int  (*receive_fd)(void *ctx);
};

/* protocol_version is filled in by the handshake. */
struct npt_vtest_protocol {
   const struct npt_vtest_transport_ops *transport;
   void *transport_ctx;
   uint32_t protocol_version;
};

/* Abort on fatal: socket is gone, no recovery. */
void npt_vtest_proto_read(struct npt_vtest_protocol *p,
                          void *buf, size_t size);
void npt_vtest_proto_write(struct npt_vtest_protocol *p,
                           const void *buf, size_t size);

void npt_vtest_vcmd_create_renderer(struct npt_vtest_protocol *p,
                                    const char *name);

/* True iff the server understood the ping. */
bool npt_vtest_vcmd_ping_protocol_version(struct npt_vtest_protocol *p);

uint32_t npt_vtest_vcmd_protocol_version(struct npt_vtest_protocol *p);

uint32_t npt_vtest_vcmd_get_param(struct npt_vtest_protocol *p,
                                  uint32_t param);

bool npt_vtest_vcmd_get_capset(struct npt_vtest_protocol *p,
                               uint32_t id, uint32_t version,
                               void *capset, size_t capset_size);

void npt_vtest_vcmd_context_init(struct npt_vtest_protocol *p,
                                 uint32_t capset_id);

/* *out_fd = SCM_RIGHTS-passed memfd or -1 on failure. */
uint32_t npt_vtest_vcmd_resource_create_blob(struct npt_vtest_protocol *p,
                                             uint32_t blob_type,
                                             uint32_t blob_flags,
                                             uint64_t size,
                                             uint64_t blob_id,
                                             int *out_fd);

void npt_vtest_vcmd_resource_unref(struct npt_vtest_protocol *p,
                                   uint32_t res_id);

void npt_vtest_vcmd_submit_cmd(struct npt_vtest_protocol *p,
                               const void *data, size_t sz);

#endif /* NPT_RENDERER_VTEST_PROTOCOL_H */
