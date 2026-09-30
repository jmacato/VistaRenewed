/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * D3D11 resource manipulation for the RESOURCE_{UPDATE, MAP, UNMAP}
 * transport commands.  Resolves host resources via the object table,
 * walks the COM vtable for D3D11 Map/Unmap/Update, and (for MAP)
 * stashes sync-map state per (resource, subresource) so the matching
 * UNMAP can write back.
 */

#include "npt_resource.h"

#include <stdlib.h>
#include <string.h>

#include "npt_com.h"
#include "npt_context.h"
#include "npt_transport_defs.h"

#include "neptune-protocol/npt_protocol_host_dispatch_types.h"

void
npt_resource_update(struct npt_context *ctx,
                    uint64_t resource_id, uint32_t subresource,
                    uint32_t row_pitch, uint32_t depth_pitch,
                    UNUSED uint32_t byte_size,
                    bool has_box,
                    uint32_t box_left, uint32_t box_top, uint32_t box_front,
                    uint32_t box_right, uint32_t box_bottom, uint32_t box_back,
                    const void *payload)
{
   /* Recover the device and immediate context from the resource.
    * Both calls add refs that we release after UpdateSubresource. */
   void *resource = npt_context_lookup_object(ctx, NULL, resource_id,
                                              NPT_OBJECT_TYPE_IUNKNOWN);
   if (!resource) {
      npt_log("resource_update: NULL resource");
      return;
   }

   ID3D11Device *device = NULL;
   PFN_ID3D11DeviceChild_GetDevice get_dev =
      NPT_COM_VTBL_FUNC(PFN_ID3D11DeviceChild_GetDevice,
                        npt_com_vtable(resource),
                        NPT_VTBL_ID3D11DeviceChild_GetDevice);
   get_dev(resource, &device);
   if (!device) {
      npt_log("resource_update: GetDevice returned NULL");
      return;
   }

   ID3D11DeviceContext *imm_ctx = NULL;
   PFN_ID3D11Device_GetImmediateContext get_ctx =
      NPT_COM_VTBL_FUNC(PFN_ID3D11Device_GetImmediateContext,
                        npt_com_vtable(device),
                        NPT_VTBL_ID3D11Device_GetImmediateContext);
   get_ctx(device, &imm_ctx);
   if (!imm_ctx) {
      npt_log("resource_update: GetImmediateContext returned NULL");
      npt_com_release(device);
      return;
   }

   D3D11_BOX box;
   const D3D11_BOX *box_arg = NULL;
   if (has_box) {
      box.left   = box_left;
      box.top    = box_top;
      box.front  = box_front;
      box.right  = box_right;
      box.bottom = box_bottom;
      box.back   = box_back;
      box_arg = &box;
   }

   PFN_ID3D11DeviceContext_UpdateSubresource update =
      NPT_COM_VTBL_FUNC(PFN_ID3D11DeviceContext_UpdateSubresource,
                        npt_com_vtable(imm_ctx),
                        NPT_VTBL_ID3D11DeviceContext_UpdateSubresource);
   update(imm_ctx, resource, subresource, box_arg, payload,
          row_pitch, depth_pitch);

   npt_com_release(imm_ctx);
   npt_com_release(device);
}

/* Returns 0 on invalid flags (causes D3D11 Map to fail). */
static D3D11_MAP
npt_access_flags_to_d3d11_map(uint32_t access_flags)
{
   const bool read = access_flags & NPT_MAP_ACCESS_READ;
   const bool write = access_flags & NPT_MAP_ACCESS_WRITE;

   if (read && write)
      return D3D11_MAP_READ_WRITE;
   if (read)
      return D3D11_MAP_READ;
   if (access_flags & NPT_MAP_ACCESS_DISCARD)
      return D3D11_MAP_WRITE_DISCARD;
   if (access_flags & NPT_MAP_ACCESS_NO_OVERWRITE)
      return D3D11_MAP_WRITE_NO_OVERWRITE;
   if (write)
      return D3D11_MAP_WRITE;

   return (D3D11_MAP)0;
}

/* ---- per-context sync-map bookkeeping --------------------------------- */

static struct npt_sync_map_entry *
npt_sync_map_find(struct npt_context *ctx, uint64_t resource_id,
                  uint32_t subresource)
{
   for (uint32_t i = 0; i < ctx->sync_maps.count; ++i) {
      struct npt_sync_map_entry *e = &ctx->sync_maps.entries[i];
      if (e->resource_id == resource_id && e->subresource == subresource)
         return e;
   }
   return NULL;
}

static struct npt_sync_map_entry *
npt_sync_map_add(struct npt_context *ctx)
{
   if (ctx->sync_maps.count == ctx->sync_maps.cap) {
      if (ctx->sync_maps.cap > UINT32_MAX / 2u)
         return NULL;
      uint32_t cap = ctx->sync_maps.cap ? ctx->sync_maps.cap * 2 : 8;
      const size_t bytes = (size_t)cap * sizeof(struct npt_sync_map_entry);
      if (bytes / sizeof(struct npt_sync_map_entry) != cap)
         return NULL;
      struct npt_sync_map_entry *e = realloc(ctx->sync_maps.entries, bytes);
      if (!e)
         return NULL;
      ctx->sync_maps.entries = e;
      ctx->sync_maps.cap = cap;
   }
   return &ctx->sync_maps.entries[ctx->sync_maps.count++];
}

static void
npt_sync_map_remove(struct npt_context *ctx, struct npt_sync_map_entry *e)
{
   uint32_t idx = (uint32_t)(e - ctx->sync_maps.entries);
   ctx->sync_maps.entries[idx] =
      ctx->sync_maps.entries[--ctx->sync_maps.count];
}

struct npt_resource_map_layout {
   uint32_t buffer_bytes;
   uint32_t rows;
   uint32_t depth;
};

/* Derive the copy extent from the backend resource, never from guest-provided
 * dimensions. QueryInterface also prevents interpreting another COM object as
 * a resource. On success the caller owns one reference to *out_resource. */
static bool
npt_resource_map_layout(void *object, uint32_t subresource,
                         void **out_resource, struct npt_resource_map_layout *out)
{
   *out_resource = NULL;
   memset(out, 0, sizeof(*out));
   void *resource = NULL;
   if (NPT_FAILED(npt_com_query_interface(object, &NPT_IID_ID3D11Resource, &resource)))
      return false;
   PFN_ID3D11Resource_GetType get_type =
      NPT_COM_VTBL_FUNC(PFN_ID3D11Resource_GetType, npt_com_vtable(resource),
                        NPT_VTBL_ID3D11Resource_GetType);
   D3D11_RESOURCE_DIMENSION type;
   get_type(resource, &type);
   uint32_t height = 1, depth = 1, mips = 1, arrays = 1;
   DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
   switch (type) {
   case D3D11_RESOURCE_DIMENSION_BUFFER: {
      D3D11_BUFFER_DESC desc;
      PFN_ID3D11Buffer_GetDesc get_desc =
         NPT_COM_VTBL_FUNC(PFN_ID3D11Buffer_GetDesc, npt_com_vtable(resource),
                           NPT_VTBL_ID3D11Buffer_GetDesc);
      get_desc(resource, &desc);
      if (subresource || !desc.ByteWidth) goto invalid;
      out->buffer_bytes = desc.ByteWidth;
      *out_resource = resource;
      return true;
   }
   case D3D11_RESOURCE_DIMENSION_TEXTURE1D: {
      D3D11_TEXTURE1D_DESC desc;
      PFN_ID3D11Texture1D_GetDesc get_desc =
         NPT_COM_VTBL_FUNC(PFN_ID3D11Texture1D_GetDesc, npt_com_vtable(resource),
                           NPT_VTBL_ID3D11Texture1D_GetDesc);
      get_desc(resource, &desc);
      if (!desc.Width) goto invalid;
      mips = desc.MipLevels; arrays = desc.ArraySize; format = desc.Format;
      break;
   }
   case D3D11_RESOURCE_DIMENSION_TEXTURE2D: {
      D3D11_TEXTURE2D_DESC desc;
      PFN_ID3D11Texture2D_GetDesc get_desc =
         NPT_COM_VTBL_FUNC(PFN_ID3D11Texture2D_GetDesc, npt_com_vtable(resource),
                           NPT_VTBL_ID3D11Texture2D_GetDesc);
      get_desc(resource, &desc);
      if (!desc.Width || !desc.Height) goto invalid;
      height = desc.Height; mips = desc.MipLevels;
      arrays = desc.ArraySize; format = desc.Format;
      break;
   }
   case D3D11_RESOURCE_DIMENSION_TEXTURE3D: {
      D3D11_TEXTURE3D_DESC desc;
      PFN_ID3D11Texture3D_GetDesc get_desc =
         NPT_COM_VTBL_FUNC(PFN_ID3D11Texture3D_GetDesc, npt_com_vtable(resource),
                           NPT_VTBL_ID3D11Texture3D_GetDesc);
      get_desc(resource, &desc);
      if (!desc.Width || !desc.Height || !desc.Depth) goto invalid;
      height = desc.Height; depth = desc.Depth;
      mips = desc.MipLevels; format = desc.Format;
      break;
   }
   default: goto invalid;
   }
   if (!mips || mips > 32 || !arrays || (uint64_t)subresource >= (uint64_t)mips * arrays)
      goto invalid;
   const uint32_t mip = subresource % mips;
   height = (height >> mip) ? (height >> mip) : 1;
   depth = (depth >> mip) ? (depth >> mip) : 1;
   uint64_t rows = height;
   switch (format) {
   case DXGI_FORMAT_BC1_TYPELESS: case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB:
   case DXGI_FORMAT_BC2_TYPELESS: case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC2_UNORM_SRGB:
   case DXGI_FORMAT_BC3_TYPELESS: case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB:
   case DXGI_FORMAT_BC4_TYPELESS: case DXGI_FORMAT_BC4_UNORM: case DXGI_FORMAT_BC4_SNORM:
   case DXGI_FORMAT_BC5_TYPELESS: case DXGI_FORMAT_BC5_UNORM: case DXGI_FORMAT_BC5_SNORM:
   case DXGI_FORMAT_BC6H_TYPELESS: case DXGI_FORMAT_BC6H_UF16: case DXGI_FORMAT_BC6H_SF16:
   case DXGI_FORMAT_BC7_TYPELESS: case DXGI_FORMAT_BC7_UNORM: case DXGI_FORMAT_BC7_UNORM_SRGB:
      rows = height / 4u + (height % 4u != 0); break;
   case DXGI_FORMAT_NV12: case DXGI_FORMAT_P010: case DXGI_FORMAT_P016:
   case DXGI_FORMAT_420_OPAQUE: rows += height / 2u; break;
   case DXGI_FORMAT_NV11: rows *= 2; break;
   case DXGI_FORMAT_P208: case DXGI_FORMAT_V208: case DXGI_FORMAT_V408:
      /* No established shared Map layout for these multi-plane formats. */
      goto invalid;
   default: break;
   }
   if (!rows || rows > UINT32_MAX) goto invalid;
   out->rows = (uint32_t)rows; out->depth = depth;
   *out_resource = resource;
   return true;
invalid:
   npt_com_release(resource);
   return false;
}

static bool
npt_resource_mapped_size(const struct npt_resource_map_layout *layout,
                          const D3D11_MAPPED_SUBRESOURCE *mapped, uint32_t *out)
{
   if (!mapped->pData) return false;
   if (layout->buffer_bytes) { *out = layout->buffer_bytes; return true; }
   const uint64_t slice = (uint64_t)mapped->RowPitch * layout->rows;
   if (!slice || slice > UINT32_MAX || !layout->depth) return false;
   if (layout->depth == 1) { *out = (uint32_t)slice; return true; }
   if (mapped->DepthPitch < slice ||
       (uint64_t)(layout->depth - 1) > (UINT32_MAX - slice) / mapped->DepthPitch)
      return false;
   *out = (uint32_t)((uint64_t)mapped->DepthPitch * (layout->depth - 1) + slice);
   return true;
}

static void
npt_resource_backend_unmap(void *imm_ctx, void *resource, uint32_t subresource)
{
   PFN_ID3D11DeviceContext_Unmap unmap_fn =
      NPT_COM_VTBL_FUNC(PFN_ID3D11DeviceContext_Unmap,
                        npt_com_vtable(imm_ctx),
                        NPT_VTBL_ID3D11DeviceContext_Unmap);
   unmap_fn(imm_ctx, resource, subresource);
}

HRESULT
npt_resource_map(struct npt_context *ctx,
                 uint64_t context_id, uint64_t resource_id,
                 uint32_t subresource, uint32_t access_flags,
                 uint32_t api_map_flags, uint32_t shmem_res_id,
                 UNUSED uint64_t read_range_begin,
                 UNUSED uint64_t read_range_end,
                 uint64_t byte_size, uint32_t mip_height, uint32_t mip_depth,
                 uint32_t shmem_offset, uint32_t *out_row_pitch,
                 uint32_t *out_depth_pitch, uint32_t *out_mapped_size)
{
   *out_row_pitch = *out_depth_pitch = *out_mapped_size = 0;
   if (access_flags & NPT_MAP_ACCESS_ABORT) return NPT_E_FAIL;
   void *object = npt_context_lookup_object(ctx, NULL, resource_id, NPT_OBJECT_TYPE_IUNKNOWN);
   void *imm_ctx = npt_context_lookup_object(ctx, NULL, context_id,
                                             NPT_OBJECT_TYPE_ID3D11DEVICECONTEXT);
   struct npt_resource *shmem = npt_context_get_resource(ctx, shmem_res_id);
   if (!object || !context_id || !imm_ctx || !shmem ||
       shmem->fd_type != VIRGL_RESOURCE_FD_SHM || !shmem->u.data ||
       (uint64_t)shmem_offset >= shmem->size ||
       npt_sync_map_find(ctx, resource_id, subresource))
      return NPT_E_FAIL;

   struct npt_resource_map_layout layout;
   void *resource;
   if (!npt_resource_map_layout(object, subresource, &resource, &layout))
      return NPT_E_FAIL;
   HRESULT hr = NPT_E_FAIL;
   /* Buffer callers provide byte width and zero dimensions; texture callers
    * provide the actual subresource's rows of storage and depth. */
   if (layout.buffer_bytes ? (mip_height || mip_depth ||
       (byte_size && byte_size != layout.buffer_bytes)) :
       (mip_height != layout.rows || mip_depth != layout.depth))
      goto release;
   PFN_ID3D11DeviceContext_Map map_fn =
      NPT_COM_VTBL_FUNC(PFN_ID3D11DeviceContext_Map, npt_com_vtable(imm_ctx),
                        NPT_VTBL_ID3D11DeviceContext_Map);
   D3D11_MAPPED_SUBRESOURCE mapped = {0};
   hr = map_fn(imm_ctx, resource, subresource,
               npt_access_flags_to_d3d11_map(access_flags), api_map_flags, &mapped);
   if (NPT_FAILED(hr)) goto release;
   uint32_t actual_size;
   if (!npt_resource_mapped_size(&layout, &mapped, &actual_size)) {
      npt_resource_backend_unmap(imm_ctx, resource, subresource);
      hr = NPT_E_FAIL; goto release;
   }
   uint64_t bound = actual_size;
   /* The pool contains adjacent slots. Never copy beyond this slot even when
    * the backend pitch exceeds the guest's pre-allocation estimate. */
   if (byte_size && bound > byte_size) bound = byte_size;
   if (bound > shmem->size - shmem_offset) bound = shmem->size - shmem_offset;
   if (!bound) {
      npt_resource_backend_unmap(imm_ctx, resource, subresource);
      hr = NPT_E_FAIL; goto release;
   }
   struct npt_sync_map_entry *entry = npt_sync_map_add(ctx);
   if (!entry) {
      npt_log("map_resource: sync map table OOM");
      npt_resource_backend_unmap(imm_ctx, resource, subresource);
      hr = NPT_E_OUTOFMEMORY; goto release;
   }
   if (access_flags & NPT_MAP_ACCESS_READ)
      memcpy((uint8_t *)shmem->u.data + shmem_offset, mapped.pData, (size_t)bound);
   *entry = (struct npt_sync_map_entry){
      .resource_id = resource_id, .subresource = subresource,
      .access_flags = access_flags, .mapped_data = mapped.pData,
      .mapped_size = (uint32_t)bound,
      .persistent = !!(access_flags & NPT_MAP_ACCESS_PERSISTENT),
   };
   *out_row_pitch = mapped.RowPitch; *out_depth_pitch = mapped.DepthPitch;
   *out_mapped_size = (uint32_t)bound;
release:
   npt_com_release(resource);
   return hr;
}

HRESULT
npt_resource_unmap(struct npt_context *ctx,
                   uint64_t context_id, uint64_t resource_id,
                   uint32_t subresource, uint32_t shmem_res_id,
                   uint32_t shmem_offset, uint64_t byte_size,
                   uint32_t access_flags,
                   UNUSED uint64_t written_range_begin,
                   UNUSED uint64_t written_range_end)
{
   void *object = npt_context_lookup_object(ctx, NULL, resource_id, NPT_OBJECT_TYPE_IUNKNOWN);
   void *imm_ctx = npt_context_lookup_object(ctx, NULL, context_id,
                                             NPT_OBJECT_TYPE_ID3D11DEVICECONTEXT);
   if (!object || !context_id || !imm_ctx) return NPT_E_FAIL;
   struct npt_resource_map_layout layout;
   void *resource;
   if (!npt_resource_map_layout(object, subresource, &resource, &layout)) return NPT_E_FAIL;
   HRESULT hr = NPT_E_FAIL;
   struct npt_sync_map_entry *entry = npt_sync_map_find(ctx, resource_id, subresource);
   if (access_flags & NPT_MAP_ACCESS_ABORT) {
      if (access_flags != NPT_MAP_ACCESS_ABORT || !entry) goto release;
      npt_resource_backend_unmap(imm_ctx, resource, subresource);
      npt_sync_map_remove(ctx, entry);
      hr = NPT_S_OK; goto release;
   }
   struct npt_resource *shmem = npt_context_get_resource(ctx, shmem_res_id);
   if (!shmem || shmem->fd_type != VIRGL_RESOURCE_FD_SHM || !shmem->u.data ||
       (uint64_t)shmem_offset > shmem->size || byte_size > shmem->size - shmem_offset)
      goto invalid;
   const uint8_t *src = (const uint8_t *)shmem->u.data + shmem_offset;
   if (access_flags) {
      if (entry) goto release;
      PFN_ID3D11DeviceContext_Map map_fn =
         NPT_COM_VTBL_FUNC(PFN_ID3D11DeviceContext_Map, npt_com_vtable(imm_ctx),
                           NPT_VTBL_ID3D11DeviceContext_Map);
      D3D11_MAPPED_SUBRESOURCE mapped = {0};
      hr = map_fn(imm_ctx, resource, subresource,
                  npt_access_flags_to_d3d11_map(access_flags), 0, &mapped);
      if (NPT_FAILED(hr)) goto release;
      uint32_t actual_size;
      if (!npt_resource_mapped_size(&layout, &mapped, &actual_size) || byte_size > actual_size) {
         npt_resource_backend_unmap(imm_ctx, resource, subresource);
         hr = NPT_E_FAIL; goto release;
      }
      if (access_flags & NPT_MAP_ACCESS_WRITE) memcpy(mapped.pData, src, (size_t)byte_size);
      npt_resource_backend_unmap(imm_ctx, resource, subresource);
      hr = NPT_S_OK; goto release;
   }
   if (!entry || entry->persistent) goto release;
   const uint64_t bound = byte_size ? byte_size : entry->mapped_size;
   if (bound > entry->mapped_size || bound > shmem->size - shmem_offset) goto invalid;
   if (entry->access_flags & NPT_MAP_ACCESS_WRITE) memcpy(entry->mapped_data, src, (size_t)bound);
   npt_resource_backend_unmap(imm_ctx, resource, subresource);
   npt_sync_map_remove(ctx, entry);
   hr = NPT_S_OK; goto release;
invalid:
   /* A malformed paired Unmap abandons the mapping without copying. */
   if (!access_flags && entry && !entry->persistent) {
      npt_resource_backend_unmap(imm_ctx, resource, subresource);
      npt_sync_map_remove(ctx, entry);
   }
release:
   npt_com_release(resource);
   return hr;
}
