/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * D3D11 shared / presentable textures over virtio-gpu blob resources.
 *
 * Venus-shaped model: the exporter's host texture is created shared by
 * the host D3D library and exported as an fd (a dmabuf under dxvk on
 * Linux, a POSIX shm fd under d3dmetal-native on macOS); EXPORT_BLOB
 * stages {fd, size} as this context's pending blob under a guest-chosen
 * blob_id, which the guest KMD claims via RESOURCE_CREATE_BLOB(HOST3D,
 * blob_id).  That binds a VM-global virtio res_id to the export.  A
 * consumer context receives the fd through the proxy's attach-
 * forwarding (guest KMD CTX_ATTACH_RESOURCE) and OPEN_RES rebuilds the
 * host library's shared-texture descriptor around it — zero-copy, the
 * imported texture aliases the exporter's memory.  No broker: the
 * canonical fd table is the VMM-side virgl resource table, keyed by
 * res_id.
 */

#ifndef NPT_SHARED_H
#define NPT_SHARED_H

#include <stdint.h>

#include "npt_com.h" /* HRESULT */
#include "virgl_resource.h"

/* fd type of a shared-texture export: what the host D3D library backs
 * shared allocations with on this platform. */
#ifdef __APPLE__
#define NPT_SHARED_FD_TYPE VIRGL_RESOURCE_FD_SHM
#else
#define NPT_SHARED_FD_TYPE VIRGL_RESOURCE_FD_DMABUF
#endif

struct npt_context;
struct npt_cmd_shared_open_res;

/* Exporter: export the shared host texture identified by
 * \p texture_id (a guest object id in \p ctx) as an fd, stage it as
 * a pending blob under \p blob_id, and write the npt_blob_export_info
 * into the shmem window at (data_res_id, data_off). */
HRESULT
npt_shared_export_blob(struct npt_context *ctx, uint64_t texture_id,
                       uint64_t blob_id, uint32_t data_res_id,
                       uint32_t data_off);

/* Consumer: import the texture backed by virtio resource cmd->res_id
 * on the device identified by \p device_id and register the imported
 * texture in \p ctx under cmd->mint_object_id.  Waits (bounded) for
 * the resource to be attached to this context. */
HRESULT
npt_shared_open_res(struct npt_context *ctx, uint64_t device_id,
                    const struct npt_cmd_shared_open_res *cmd);

#endif /* NPT_SHARED_H */
