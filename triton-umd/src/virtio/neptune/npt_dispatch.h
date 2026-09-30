/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Centralized transport command dispatchers.  One function per
 * transport command in active use; each owns the cmd struct
 * construction, picks the right submit primitive (raw / raw_seqno /
 * raw_with_payload / submit_command + reply / renderer_submit_cmd),
 * and reads out the reply where applicable.  Callers pass logical
 * inputs and receive logical outputs through the function signature;
 * no callers should construct npt_cmd_* structs directly.
 *
 * Naming mirrors NPT_TRANSPORT_<SUBGROUP>_<METHOD>: e.g.
 * NPT_TRANSPORT_RESOURCE_UPDATE -> npt_dispatch_resource_update.  Where
 * one logical command has two submit-style variants, the seqno-bearing
 * variant gets a _seqno suffix.
 *
 * Scope: cross-file commands only.  Ring-internal commands (RING_*,
 * CORE_SET_REPLY_STREAM, RESOURCE_EXECUTE_CMD_STREAM, COM_RELEASE) stay
 * inline in npt_ring.c because they're emitted under the ring lock by
 * the locked-submit machinery itself.
 */

#ifndef NPT_DISPATCH_H
#define NPT_DISPATCH_H

#include "npt_common.h"

#include "neptune-protocol/npt_protocol_directx_types.h"

struct npt_renderer;
struct npt_ring;

/* ==========================================================================
 * COM
 * ========================================================================== */

HRESULT npt_dispatch_com_query_interface(struct npt_ring *ring,
                                         uint64_t src_id,
                                         const GUID *iid,
                                         uint64_t guest_id);

/* ==========================================================================
 * RESOURCE
 * ========================================================================== */

HRESULT npt_dispatch_resource_map(struct npt_ring *ring,
                                  uint64_t context_id,
                                  uint64_t resource_id,
                                  uint32_t subresource,
                                  uint32_t access_flags,
                                  uint32_t api_map_flags,
                                  uint32_t shmem_res_id,
                                  uint64_t byte_size,
                                  uint32_t mip_height,
                                  uint32_t mip_depth,
                                  uint32_t shmem_offset,
                                  uint32_t *out_row_pitch,
                                  uint32_t *out_depth_pitch);

/* Synchronous, no-copy cancellation of a successful Map rejected by the guest. */
HRESULT npt_dispatch_resource_abort(struct npt_ring *ring,
                                    uint64_t context_id, uint64_t resource_id,
                                    uint32_t subresource);

/* Query-neutral GPU conversion of equally sized raw color scratch views. */
HRESULT npt_dispatch_resource_copy_color(void *context, void *dst_rtv,
                                          void *src_srv);

bool npt_dispatch_resource_unmap(struct npt_ring *ring,
                                 uint64_t context_id,
                                 uint64_t resource_id,
                                 uint32_t subresource,
                                 uint32_t shmem_res_id,
                                 uint64_t byte_size,
                                 uint32_t shmem_offset,
                                 uint32_t access_flags);

bool npt_dispatch_resource_unmap_seqno(struct npt_ring *ring,
                                       uint64_t context_id,
                                       uint64_t resource_id,
                                       uint32_t subresource,
                                       uint32_t shmem_res_id,
                                       uint64_t byte_size,
                                       uint32_t shmem_offset,
                                       uint32_t access_flags,
                                       uint32_t *out_seqno);

/* byte_size is reserved on the ring and is what the host may consume;
 * copy_size (<= byte_size) is how much of `data` is actually read. */
bool npt_dispatch_resource_update(struct npt_ring *ring,
                                  uint64_t resource_host_id,
                                  uint32_t subresource,
                                  uint32_t row_pitch,
                                  uint32_t depth_pitch,
                                  const D3D11_BOX *box,
                                  const void *data,
                                  uint32_t byte_size,
                                  uint32_t copy_size);

/* ==========================================================================
 * CLEAR
 * ========================================================================== */

HRESULT npt_dispatch_clear_depth_stencil_rects(
    void *context, void *dsv, uint32_t clear_flags, float depth,
    uint8_t stencil, uint32_t rect_count, const int32_t *rect_ltrb);

/* ==========================================================================
 * SHARED
 * ========================================================================== */

struct npt_blob_export_info;
struct npt_cmd_shared_open_res;
struct npt_cmd_shared_query_layout_reply;
HRESULT npt_dispatch_shared_query_layout(
   struct npt_ring *ring, uint32_t res_id,
   struct npt_cmd_shared_query_layout_reply *out);

/* Sync: stage texture_id's dmabuf export as this context's pending
 * blob under blob_id; the host writes npt_blob_export_info into
 * (data_res_id, data_off). */
HRESULT npt_dispatch_shared_cancel_export(struct npt_ring *ring,
                                           uint64_t blob_id);

HRESULT npt_dispatch_shared_export_blob(struct npt_ring *ring,
                                        uint64_t texture_id,
                                        uint64_t blob_id,
                                        uint32_t data_res_id,
                                        uint32_t data_off);

/* Sync: import the texture backed by virtio resource res_id on
 * device_id and register it under mint_object_id.  desc/export info
 * fields are caller-populated in *cmd (header fields are owned by the
 * dispatcher). */
HRESULT npt_dispatch_shared_open_res(struct npt_ring *ring,
                                     uint64_t device_id,
                                     struct npt_cmd_shared_open_res *cmd);

/* ==========================================================================
 * EVENT
 * ========================================================================== */

bool npt_dispatch_event_register(struct npt_renderer *renderer,
                                 uint64_t event_token);

/* Ring-borne REGISTER_EVENT: rides the method ring (not the renderer command
 * stream) so it is ordered ahead of the synchronous ARM_EVENT_FENCE that
 * follows on the same ring.  The host therefore holds a registration reference
 * on the event proxy before the (ctrl-queue) present-fence SUBMIT pops the arm
 * reference -- otherwise a lazily-created proxy is freed the instant the SUBMIT
 * matches its pending-arm, and the following SetEventOnCompletion hands the raw
 * token to the host backend, which dereferences it. */
bool npt_dispatch_event_register_ring(struct npt_ring *ring,
                                      uint64_t event_token);

bool npt_dispatch_event_arm_fence(struct npt_ring *ring,
                                  uint64_t event_token,
                                  uint32_t ring_idx);

bool npt_dispatch_event_release(struct npt_renderer *renderer,
                                uint64_t event_token);

/* Ring-borne RELEASE_EVENT, the counterpart to npt_dispatch_event_register_ring.
 * Riding the same method ring keeps a token's RELEASE strictly before the next
 * REGISTER for that token (ring FIFO), so a stale RELEASE cannot unref a proxy
 * that a subsequent re-arm just created for the reused HANDLE. */
bool npt_dispatch_event_release_ring(struct npt_ring *ring,
                                     uint64_t event_token);

/* ==========================================================================
 * FEEDBACK
 * ========================================================================== */

bool npt_dispatch_feedback_register_query(struct npt_ring *ring,
                                          uint64_t query_id,
                                          uint32_t fb_res_id,
                                          uint32_t fb_offset,
                                          uint32_t query_data_size);

bool npt_dispatch_feedback_unregister_query(struct npt_ring *ring,
                                            uint64_t query_id);

bool npt_dispatch_feedback_register_fence(struct npt_ring *ring,
                                          uint64_t fence_id,
                                          uint32_t fb_res_id,
                                          uint32_t fb_offset);

#endif /* NPT_DISPATCH_H */
