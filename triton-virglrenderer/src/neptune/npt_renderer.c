/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 */

#include "npt_common.h"

#include "drm-uapi/virtgpu_drm.h"
#include "neptune-protocol/npt_protocol_defs.h"
#include "neptune_hw.h"
#include "npt_context.h"
#include "npt_library.h"
#include "npt_profile.h"

struct npt_renderer_state {
   const struct npt_renderer_callbacks *cbs;
   bool initialized;
   struct npt_d3d_library library;

   struct list_head contexts;
};

static struct npt_renderer_state npt_state;

size_t
npt_get_capset(void *capset, UNUSED uint32_t flags)
{
   struct virgl_renderer_capset_neptune *c = capset;
   if (c) {
      memset(c, 0, sizeof(*c));
      c->wire_format_version = NPT_PROTOCOL_WIRE_VERSION;
   }

   return sizeof(struct virgl_renderer_capset_neptune);
}

bool
npt_renderer_init(UNUSED uint32_t flags, const struct npt_renderer_callbacks *cbs)
{
   if (npt_state.initialized)
      return true;

   npt_debug_init();
   npt_profile_init();

   /* Route log output through the host's log handler instead of
    * stderr directly. */
   if (cbs && cbs->debug_logger)
      virgl_log_set_handler(cbs->debug_logger, NULL, NULL);

   npt_library_init(&npt_state.library);

   list_inithead(&npt_state.contexts);

   npt_state.cbs = cbs;
   npt_state.initialized = true;

   npt_log("Neptune renderer initialized");

   return true;
}

void
npt_renderer_fini(void)
{
   if (!npt_state.initialized)
      return;

   list_for_each_entry_safe (struct npt_context, ctx, &npt_state.contexts, head) {
      list_del(&ctx->head);
      npt_context_destroy(ctx);
   }

   npt_library_fini(&npt_state.library);

   npt_state.cbs = NULL;
   npt_state.initialized = false;
}

struct npt_d3d_library *
npt_renderer_get_library(void)
{
   if (!npt_state.initialized)
      return NULL;
   return &npt_state.library;
}

static struct npt_context *
npt_renderer_lookup_context(uint32_t ctx_id)
{
   list_for_each_entry (struct npt_context, ctx, &npt_state.contexts, head) {
      if (ctx->ctx_id == ctx_id)
         return ctx;
   }
   return NULL;
}

bool
npt_renderer_create_context(uint32_t ctx_id,
                            uint32_t ctx_flags,
                            uint32_t nlen,
                            const char *name)
{
   assert(ctx_id);
   assert(!(ctx_flags & ~VIRGL_RENDERER_CONTEXT_FLAG_CAPSET_ID_MASK));

   /* Defensive: the render-server dispatcher already routes by
    * capset_id, but the renderer API is public so re-check. */
   if ((ctx_flags & VIRGL_RENDERER_CONTEXT_FLAG_CAPSET_ID_MASK) !=
       VIRTGPU_DRM_CAPSET_NEPTUNE)
      return false;

   if (npt_renderer_lookup_context(ctx_id))
      return false;

   npt_renderer_retire_fence_callback_type retire_cb =
      npt_state.cbs ? npt_state.cbs->retire_fence : NULL;

   struct npt_context *ctx =
      npt_context_create(ctx_id, retire_cb, nlen, name);
   if (!ctx)
      return false;

   list_addtail(&ctx->head, &npt_state.contexts);
   return true;
}

void
npt_renderer_destroy_context(uint32_t ctx_id)
{
   struct npt_context *ctx = npt_renderer_lookup_context(ctx_id);
   if (!ctx)
      return;

   list_del(&ctx->head);
   npt_context_destroy(ctx);
}

bool
npt_renderer_submit_cmd(uint32_t ctx_id, void *cmd, uint32_t size)
{
   struct npt_context *ctx = npt_renderer_lookup_context(ctx_id);
   if (!ctx)
      return false;

   return npt_context_submit_cmd(ctx, cmd, size);
}

bool
npt_renderer_submit_fence(uint32_t ctx_id,
                          uint32_t flags,
                          uint64_t ring_idx,
                          uint64_t fence_id)
{
   struct npt_context *ctx = npt_renderer_lookup_context(ctx_id);
   if (!ctx)
      return false;

   if (!ctx->retire_fence) {
      npt_log("submit_fence: no retire_fence callback installed");
      return false;
   }

   /* virgl_context passes ring_idx as uint64; sync_queues[] is
    * indexed by uint32_t. */
   if (ring_idx > UINT32_MAX) {
      npt_log("submit_fence: ring_idx %" PRIu64 " out of range", ring_idx);
      return false;
   }

   return npt_context_submit_fence(ctx, flags, (uint32_t)ring_idx, fence_id);
}

bool
npt_renderer_create_resource(uint32_t ctx_id,
                             uint32_t res_id,
                             uint64_t blob_id,
                             uint64_t blob_size,
                             uint32_t blob_flags,
                             enum virgl_resource_fd_type *out_fd_type,
                             int *out_res_fd,
                             uint32_t *out_map_info,
                             uint32_t *out_export_format)
{
   struct npt_context *ctx = npt_renderer_lookup_context(ctx_id);
   if (!ctx)
      return false;

   struct virgl_context_blob blob;
   if (!npt_context_create_resource(ctx, res_id, blob_id, blob_size, blob_flags, &blob))
      return false;

   *out_fd_type = blob.type;
   *out_res_fd = blob.u.fd;
   *out_map_info = blob.map_info;
   *out_export_format = blob.export_format;

   return true;
}

bool
npt_renderer_import_resource(uint32_t ctx_id,
                             uint32_t res_id,
                             enum virgl_resource_fd_type fd_type,
                             int fd,
                             uint64_t size)
{
   struct npt_context *ctx = npt_renderer_lookup_context(ctx_id);
   if (!ctx)
      return false;

   return npt_context_import_resource(ctx, res_id, fd_type, fd, size);
}

void
npt_renderer_destroy_resource(uint32_t ctx_id, uint32_t res_id)
{
   struct npt_context *ctx = npt_renderer_lookup_context(ctx_id);
   if (!ctx)
      return;

   npt_context_destroy_resource(ctx, res_id);
}
