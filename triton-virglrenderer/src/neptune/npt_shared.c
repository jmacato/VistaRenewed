/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * D3D11 shared / presentable textures over virtio-gpu blob resources.
 * See npt_shared.h for the model.  The COM flow (GetSharedHandle /
 * OpenSharedResource) is platform-neutral; only the descriptor behind
 * the HANDLE differs: dxvk's DxvkSharedTextureDescriptor (dmabuf) on
 * Linux, the darwin backend's shared-texture descriptor (shm fd) on macOS.
 */

#include "npt_shared.h"

#include <errno.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef __APPLE__
#include <dxvk_shared_resource.h>
#endif

#include "c11/threads.h"

#include "virgl_hw.h"

#include "npt_context.h"
#include "npt_transport_defs.h"

#include "neptune-protocol/npt_protocol_defs.h"
#include "neptune-protocol/npt_protocol_host_dispatch_types.h"

/* D3D11_BIND_SHADER_RESOURCE (d3d11.h); consumers sample the texture. */
#define NPT_D3D11_BIND_SHADER_RESOURCE 0x8u
#define NPT_D3D11_BIND_RENDER_TARGET   0x20u
#define NPT_D3D11_RESOURCE_MISC_SHARED 0x2u

/* The exporting backend's own description of the texture format.  The
 * Darwin backend records the DXGI format directly; the dxvk backend
 * carries it in the shared-resource metadata. */
#ifdef __APPLE__
#define NPT_SHARED_EXPORT_FORMAT(d) ((d)->dxgi_format)
#else
#define NPT_SHARED_EXPORT_FORMAT(d) ((d)->meta.Format)
#endif

/* DXGI_FORMAT -> enum virgl_formats for the typed shared-resource contract.
 * A shared format is not necessarily eligible for display scanout. */
static uint32_t
npt_shared_dxgi_to_virgl_format(uint32_t dxgi_format)
{
   switch (dxgi_format) {
   case 87: /* DXGI_FORMAT_B8G8R8A8_UNORM */
      return VIRGL_FORMAT_B8G8R8A8_UNORM;
   case 88: /* DXGI_FORMAT_B8G8R8X8_UNORM */
      return VIRGL_FORMAT_B8G8R8X8_UNORM;
   case 28: /* DXGI_FORMAT_R8G8B8A8_UNORM */
      return VIRGL_FORMAT_R8G8B8A8_UNORM;
   case 29: /* DXGI_FORMAT_R8G8B8A8_UNORM_SRGB */
      return VIRGL_FORMAT_R8G8B8A8_SRGB;
   case 91: /* DXGI_FORMAT_B8G8R8A8_UNORM_SRGB */
      return VIRGL_FORMAT_B8G8R8A8_SRGB;
   case 93: /* DXGI_FORMAT_B8G8R8X8_UNORM_SRGB */
      return VIRGL_FORMAT_B8G8R8X8_SRGB;
   case 24: /* DXGI_FORMAT_R10G10B10A2_UNORM */
      return VIRGL_FORMAT_R10G10B10A2_UNORM;
   case 10: /* DXGI_FORMAT_R16G16B16A16_FLOAT */
      return VIRGL_FORMAT_R16G16B16A16_FLOAT;
   default:
      return 0;
   }
}

#ifdef __APPLE__
/* The POD both darwin backends hand through GetSharedHandle /
 * OpenSharedResource, defined here so the renderer neither links nor
 * includes a backend header.  d3dmetal's dmn_shared_texture_handle and
 * dxmt's dxmt_shared_texture_handle share this exact ABI: same 'DMTX'
 * magic, version, and field layout. */
#define NPT_DARWIN_SHARED_TEXTURE_MAGIC   0x58544D44u /* 'DMTX' */
#define NPT_DARWIN_SHARED_HANDLE_VERSION  1u
struct npt_darwin_shared_texture {
   uint32_t magic;
   uint32_t version;
   int32_t  fd;            /* process-local; sent via SCM_RIGHTS, then patched */
   uint32_t width, height;
   uint32_t dxgi_format;
   uint32_t mip_levels, array_size, sample_count;
   uint32_t bind_flags, misc_flags, cpu_access;
   uint64_t stride;        /* bytesPerRow */
   uint64_t size;          /* logical stride*height */
};
#endif

/* OPEN_RES arrives on the consumer's ring thread; the resource fd
 * arrives on the dispatch thread via the proxy's attach-forwarding
 * (triggered by the guest KMD's CTX_ATTACH_RESOURCE, a virtio ctrl
 * command that is not ordered against ring commands).  Bounded poll
 * bridges the race; the budget stays well under dxgkrnl's ~2 s TDR
 * so a missing attach fails the open instead of wedging the ring. */
#define NPT_SHARED_ATTACH_WAIT_MS   1000
#define NPT_SHARED_ATTACH_POLL_MS   2

static bool
npt_shared_linear_desc_valid(uint32_t width, uint32_t height,
                             uint32_t format, uint32_t mip_levels,
                             uint32_t array_size, uint32_t sample_count,
                             uint32_t usage, uint32_t bind_flags,
                             uint32_t cpu_access_flags, uint32_t misc_flags,
                             const struct npt_blob_export_info *info)
{
   const uint32_t virgl_format = npt_shared_dxgi_to_virgl_format(format);
   uint64_t row_bytes;
   uint64_t last_row;

   if (!width || !height || !virgl_format || mip_levels != 1 ||
       array_size != 1 || sample_count != 1 || usage != 0 ||
       (bind_flags & (NPT_D3D11_BIND_SHADER_RESOURCE |
                      NPT_D3D11_BIND_RENDER_TARGET)) !=
          (NPT_D3D11_BIND_SHADER_RESOURCE | NPT_D3D11_BIND_RENDER_TARGET) ||
       cpu_access_flags != 0 ||
       !(misc_flags & NPT_D3D11_RESOURCE_MISC_SHARED) || !info ||
       info->plane_count != 1 || info->texture_layout > 2 || !info->allocation_size ||
       !info->planes[0].pitch)
      return false;

   row_bytes = (uint64_t)width * (format == 10 ? 8 : 4);
   if (info->planes[0].pitch < row_bytes ||
       info->planes[0].offset >= info->allocation_size)
      return false;

   last_row = info->planes[0].offset;
   if ((uint64_t)(height - 1) >
       (UINT64_MAX - last_row) / info->planes[0].pitch)
      return false;
   last_row += (uint64_t)(height - 1) * info->planes[0].pitch;
   if (row_bytes > UINT64_MAX - last_row ||
       last_row + row_bytes > info->allocation_size)
      return false;

   for (uint32_t i = 1; i < NPT_BLOB_EXPORT_MAX_PLANES; i++) {
      if (info->planes[i].offset || info->planes[i].pitch)
         return false;
   }
   return true;
}

/* Restrict imports to the formats and plane count our D3D shared path
 * supports.  Preserve modifiers and padded strides; guessing width*4 can
 * alias the wrong rows of a tiled standard primary. */
static bool
npt_shared_layout_to_reply(const struct virgl_attachment_layout *layout,
                           uint64_t size,
                           struct npt_cmd_shared_query_layout_reply *reply)
{
   struct npt_blob_export_info info = {0};
   uint32_t format;
   switch (layout->fourcc) {
   case 0x34325241u: format = 87; break; /* DRM AR24: BGRA */
   case 0x34325258u: format = 88; break; /* DRM XR24: BGRX */
   case 0x34324241u: format = 28; break; /* DRM AB24: RGBA */
   /* DXGI has no RGBX8 format.  Import its physical RGBA8 storage; the
    * primary's logical X format and opaque scanout remain unchanged. */
   case 0x34324258u: format = 28; break; /* DRM XB24: RGBX */
   default: return false;
   }
   info.modifier = layout->modifier;
   info.allocation_size = size;
   info.plane_count = layout->plane_count;
   for (uint32_t i = 0; i < NPT_BLOB_EXPORT_MAX_PLANES; i++) {
      info.planes[i].pitch = layout->strides[i];
      info.planes[i].offset = layout->offsets[i];
   }
   if (!npt_shared_linear_desc_valid(layout->width, layout->height, format,
          1, 1, 1, 0, NPT_D3D11_BIND_SHADER_RESOURCE |
          NPT_D3D11_BIND_RENDER_TARGET, 0, NPT_D3D11_RESOURCE_MISC_SHARED,
          &info))
      return false;
   reply->width = layout->width;
   reply->height = layout->height;
   reply->format = format;
   reply->export_info = info;
   return true;
}

HRESULT
npt_shared_query_layout(struct npt_context *ctx, uint32_t res_id,
                        struct npt_cmd_shared_query_layout_reply *reply)
{
   for (int waited_ms = 0;; waited_ms += NPT_SHARED_ATTACH_POLL_MS) {
      bool found = false;
      bool valid = false;
      /* Copy all metadata under the same lock as attach/detach.  No resource
       * pointer survives the unlock, including on the polling path. */
      mtx_lock(&ctx->resource_mutex);
      const struct hash_entry *entry =
         _mesa_hash_table_search(ctx->resource_table, &res_id);
      const struct npt_resource *res = entry ? entry->data : NULL;
      if (res) {
         found = true;
         if (res->fd_type == VIRGL_RESOURCE_FD_DMABUF && res->u.fd >= 0)
            valid = npt_shared_layout_to_reply(&res->layout, res->size, reply);
      }
      mtx_unlock(&ctx->resource_mutex);
      if (valid)
         return NPT_S_OK;
      if (found || waited_ms >= NPT_SHARED_ATTACH_WAIT_MS)
         return NPT_E_INVALIDARG;
      thrd_sleep(&(struct timespec){
                    .tv_nsec = NPT_SHARED_ATTACH_POLL_MS * 1000000L }, NULL);
   }
}

/* Duplicate the attached resource fd while its table entry is protected.
 * Returning a raw npt_resource pointer after unlocking lets a concurrent
 * detach free the entry before dup() reads it. */
static int
npt_shared_wait_and_dup_resource(struct npt_context *ctx, uint32_t res_id,
                                 uint64_t required_size,
                                 enum virgl_resource_fd_type *found_type)
{
   for (int waited_ms = 0;; waited_ms += NPT_SHARED_ATTACH_POLL_MS) {
      int fd = -1;
      enum virgl_resource_fd_type type = VIRGL_RESOURCE_FD_INVALID;
      bool found = false;

      mtx_lock(&ctx->resource_mutex);
      const struct hash_entry *entry =
         _mesa_hash_table_search(ctx->resource_table, &res_id);
      const struct npt_resource *res = entry ? entry->data : NULL;
      if (res) {
         found = true;
         type = res->fd_type;
         if (type == NPT_SHARED_FD_TYPE && res->u.fd >= 0 &&
             required_size && required_size <= res->size) {
            fd = dup(res->u.fd);
            if (fd < 0) {
               int dup_errno = errno;
               mtx_unlock(&ctx->resource_mutex);
               npt_log("shared: open: res_id %u dup failed (errno=%d)",
                       res_id, dup_errno);
               return -2;
            }
         }
      }
      mtx_unlock(&ctx->resource_mutex);

      if (fd >= 0) {
         if (found_type)
            *found_type = type;
         return fd;
      }
      if (found || waited_ms >= NPT_SHARED_ATTACH_WAIT_MS) {
         npt_log("shared: open: res_id %u unavailable "
                 "(found=%d type=%d size=%" PRIu64 ")",
                 res_id, found, (int)type, required_size);
         return -1;
      }
      thrd_sleep(&(struct timespec){
                    .tv_nsec = NPT_SHARED_ATTACH_POLL_MS * 1000000L }, NULL);
   }
}

HRESULT
npt_shared_export_blob(struct npt_context *ctx, uint64_t texture_id,
                       uint64_t blob_id, uint32_t data_res_id,
                       uint32_t data_off)
{
   if (!texture_id || !blob_id)
      return NPT_E_INVALIDARG;

   void *texture = npt_context_lookup_object(ctx, NULL, texture_id,
                                             NPT_OBJECT_TYPE_ID3D11TEXTURE2D);
   if (!texture) {
      npt_log("shared: export: texture id 0x%016" PRIx64 " not found",
              texture_id);
      return NPT_E_INVALIDARG;
   }

   /* Export the texture's shared descriptor.  The descriptor and its
    * fd are owned by the texture; only copies leave this frame. */
   void *dxgi_res = NULL;
   if (NPT_FAILED(npt_com_query_interface(texture, &NPT_IID_IDXGIResource,
                                          &dxgi_res)) || !dxgi_res) {
      npt_log("shared: export: blob_id %" PRIu64 " has no IDXGIResource",
              blob_id);
      return NPT_E_FAIL;
   }

   PFN_IDXGIResource_GetSharedHandle get_shared =
      NPT_COM_VTBL_FUNC(PFN_IDXGIResource_GetSharedHandle,
                        npt_com_vtable(dxgi_res),
                        NPT_VTBL_IDXGIResource_GetSharedHandle);
   HANDLE handle = 0;
   HRESULT hr = get_shared(dxgi_res, &handle);
   npt_com_release(dxgi_res);

   if (NPT_FAILED(hr) || !handle) {
      npt_log("shared: export: blob_id %" PRIu64
              " GetSharedHandle failed (hr=0x%x)", blob_id, hr);
      return NPT_FAILED(hr) ? hr : NPT_E_FAIL;
   }

   struct npt_blob_export_info info;
   memset(&info, 0, sizeof(info));

#ifdef __APPLE__
   const struct npt_darwin_shared_texture *desc =
      (const struct npt_darwin_shared_texture *)(uintptr_t)handle;
   struct npt_blob_export_info validation_info;
   memset(&validation_info, 0, sizeof(validation_info));
   validation_info.allocation_size = desc->size;
   validation_info.plane_count = 1;
   validation_info.planes[0].pitch = desc->stride;
   if (desc->magic != NPT_DARWIN_SHARED_TEXTURE_MAGIC ||
       desc->version != NPT_DARWIN_SHARED_HANDLE_VERSION || desc->fd < 0 ||
       !npt_shared_linear_desc_valid(desc->width, desc->height,
                                     desc->dxgi_format, desc->mip_levels,
                                     desc->array_size, desc->sample_count,
                                     0, desc->bind_flags, desc->cpu_access,
                                     desc->misc_flags, &validation_info)) {
      npt_log("shared: export: blob_id %" PRIu64 " bad descriptor", blob_id);
      return NPT_E_FAIL;
   }

   /* Backend shared textures are always one linear plane of shared
    * memory; modifier/texture_layout have no meaning here. */
   info.allocation_size = desc->size;
   info.plane_count = 1;
   info.planes[0].offset = 0;
   info.planes[0].pitch = desc->stride;

   const int export_fd = desc->fd;
   const uint64_t export_size = desc->size;
#else
   const struct DxvkSharedTextureDescriptor *desc =
      (const struct DxvkSharedTextureDescriptor *)(uintptr_t)handle;
   struct npt_blob_export_info validation_info;
   memset(&validation_info, 0, sizeof(validation_info));
   validation_info.modifier = desc->drmFormatModifier;
   validation_info.allocation_size = desc->allocationSize;
   validation_info.plane_count = desc->planeCount;
   validation_info.texture_layout = desc->meta.TextureLayout;
   for (uint32_t i = 0;
        i < desc->planeCount && i < NPT_BLOB_EXPORT_MAX_PLANES; i++) {
      validation_info.planes[i].offset = desc->planes[i].offset;
      validation_info.planes[i].pitch = desc->planes[i].pitch;
   }
   if (desc->magic != DXVK_SHARED_DESCRIPTOR_TEXTURE ||
       desc->structSize != sizeof(*desc) || desc->fd < 0 ||
       desc->planeCount < 1 ||
       desc->planeCount > NPT_BLOB_EXPORT_MAX_PLANES ||
       !npt_shared_linear_desc_valid(desc->meta.Width, desc->meta.Height,
                                     desc->meta.Format,
                                     desc->meta.MipLevels,
                                     desc->meta.ArraySize,
                                     desc->meta.SampleDesc.Count,
                                     desc->meta.Usage,
                                     desc->meta.BindFlags,
                                     desc->meta.CPUAccessFlags,
                                     desc->meta.MiscFlags,
                                     &validation_info)) {
      npt_log("shared: export: blob_id %" PRIu64 " bad descriptor", blob_id);
      return NPT_E_FAIL;
   }

   info.modifier = desc->drmFormatModifier;
   info.allocation_size = desc->allocationSize;
   info.plane_count = desc->planeCount;
   info.texture_layout = desc->meta.TextureLayout;
   for (uint32_t i = 0; i < desc->planeCount; i++) {
      info.planes[i].offset = desc->planes[i].offset;
      info.planes[i].pitch = desc->planes[i].pitch;
   }

   const int export_fd = desc->fd;
   const uint64_t export_size = desc->allocationSize;
#endif

   /* The channel order the exporter actually used.  An importer cannot
    * derive it: DRI3 carries no fourcc, so it can only guess from
    * depth/bpp and always guesses the screen visual's BGRA. */
   const uint32_t export_virgl_format =
      npt_shared_dxgi_to_virgl_format(NPT_SHARED_EXPORT_FORMAT(desc));
   if (!export_virgl_format)
      return NPT_E_INVALIDARG;

   struct virgl_attachment_layout layout = {0};
#ifndef __APPLE__
   /* Keep the backend's layout with the host blob as well as the guest handle. */
   layout.width = desc->meta.Width;
   layout.height = desc->meta.Height;
   switch (desc->meta.Format) {
   case 87: case 91: layout.fourcc = 0x34325241u; break; /* AR24 */
   case 88: case 93: layout.fourcc = 0x34325258u; break; /* XR24 */
   case 28: case 29: layout.fourcc = 0x34324241u; break; /* AB24 */
   case 24: layout.fourcc = 0x30334241u; break; /* AB30 */
   case 10: layout.fourcc = 0x48344241u; break; /* AB4H */
   default: return NPT_E_INVALIDARG;
   }
   layout.modifier = info.modifier;
   layout.plane_count = info.plane_count;
   for (uint32_t i = 0; i < info.plane_count; i++) {
      if (info.planes[i].offset > UINT32_MAX ||
          info.planes[i].pitch > UINT32_MAX)
         return NPT_E_INVALIDARG;
      layout.offsets[i] = info.planes[i].offset;
      layout.strides[i] = info.planes[i].pitch;
   }
#endif

   /* Publish the export-level facts into the exporter's shmem window. */
   bool data_ok = false;
   mtx_lock(&ctx->resource_mutex);
   const struct hash_entry *data_entry =
      _mesa_hash_table_search(ctx->resource_table, &data_res_id);
   struct npt_resource *data_res = data_entry ? data_entry->data : NULL;
   if (data_res && data_res->fd_type == VIRGL_RESOURCE_FD_SHM &&
       data_res->u.data && (uint64_t)data_off <= data_res->size &&
       sizeof(info) <= data_res->size - (uint64_t)data_off) {
      memcpy((uint8_t *)data_res->u.data + data_off, &info, sizeof(info));
      data_ok = true;
   }
   mtx_unlock(&ctx->resource_mutex);
   if (!data_ok) {
      npt_log("shared: export: data resource %u not found", data_res_id);
      return NPT_E_INVALIDARG;
   }

   /* Stage the pending blob the guest KMD claims via
    * RESOURCE_CREATE_BLOB(HOST3D, blob_id).  The table takes fd
    * ownership; the texture keeps its own. */
   int fd = dup(export_fd);
   if (fd < 0) {
      npt_log("shared: export: blob_id %" PRIu64 " dup failed", blob_id);
      return NPT_E_FAIL;
   }
   if (!npt_context_register_pending_blob(ctx, blob_id,
                                          NPT_SHARED_FD_TYPE, fd,
                                          export_size,
                                          export_virgl_format, &layout)) {
      close(fd);
      return NPT_E_FAIL;
   }

#ifdef __APPLE__
   npt_log("shared: exported blob_id=%" PRIu64 " %ux%u fmt=%u pitch=%" PRIu64
           " (ctx %u)", blob_id, desc->width, desc->height, desc->dxgi_format,
           desc->stride, ctx->ctx_id);
#else
   npt_log("shared: exported blob_id=%" PRIu64 " %ux%u fmt=%u mod=0x%016"
           PRIx64 " pitch=%" PRIu64 " (ctx %u)", blob_id, desc->meta.Width,
           desc->meta.Height, desc->meta.Format, desc->drmFormatModifier,
           desc->planes[0].pitch, ctx->ctx_id);
#endif
   return NPT_S_OK;
}

HRESULT
npt_shared_open_res(struct npt_context *ctx, uint64_t device_id,
                    const struct npt_cmd_shared_open_res *cmd)
{
   if (!ctx || !cmd || !device_id || !cmd->res_id || !cmd->mint_object_id)
      return NPT_E_INVALIDARG;
   if (!npt_shared_linear_desc_valid(cmd->width, cmd->height, cmd->format,
                                     cmd->mip_levels, cmd->array_size,
                                     cmd->sample_count, cmd->usage,
                                     cmd->bind_flags, cmd->cpu_access_flags,
                                     cmd->misc_flags, &cmd->export_info))
      return NPT_E_INVALIDARG;

   if (npt_context_lookup_object(ctx, NULL, cmd->mint_object_id,
                                 NPT_OBJECT_TYPE_IUNKNOWN)) {
      npt_log("shared: open: minted id 0x%016" PRIx64 " already exists",
              cmd->mint_object_id);
      return NPT_E_INVALIDARG;
   }

   void *device = npt_context_lookup_object(ctx, NULL, device_id,
                                            NPT_OBJECT_TYPE_ID3D11DEVICE);
   if (!device) {
      npt_log("shared: open: device id 0x%016" PRIx64 " not found", device_id);
      return NPT_E_INVALIDARG;
   }

   /* Wait for the attach-forwarded resource.  Same-context opens hit
    * immediately.  The helper duplicates the fd while holding the table
    * mutex, so detach cannot free the resource between lookup and dup. */
   int attached_fd = npt_shared_wait_and_dup_resource(
      ctx, cmd->res_id, cmd->export_info.allocation_size, NULL);
   if (attached_fd == -2)
      return NPT_E_FAIL;
   if (attached_fd < 0)
      return NPT_E_INVALIDARG;

   /* Rebuild the exporter's descriptor around our own fd reference. */
#ifdef __APPLE__
   struct npt_darwin_shared_texture desc;
   memset(&desc, 0, sizeof(desc));
   desc.magic = NPT_DARWIN_SHARED_TEXTURE_MAGIC;
   desc.version = NPT_DARWIN_SHARED_HANDLE_VERSION;
   desc.width = cmd->width;
   desc.height = cmd->height;
   desc.dxgi_format = cmd->format;
   desc.mip_levels = cmd->mip_levels;
   desc.array_size = cmd->array_size;
   desc.sample_count = cmd->sample_count;
   desc.bind_flags = cmd->bind_flags;
   desc.misc_flags = cmd->misc_flags;
   desc.cpu_access = cmd->cpu_access_flags;
   /* One linear plane of shared memory; usage/layout/modifier from the
    * wire have no darwin-backend equivalent. */
   desc.stride = cmd->export_info.planes[0].pitch;
   desc.size = cmd->export_info.allocation_size;
#else
   struct DxvkSharedTextureDescriptor desc;
   memset(&desc, 0, sizeof(desc));
   desc.magic = DXVK_SHARED_DESCRIPTOR_TEXTURE;
   desc.version = DXVK_SHARED_DESCRIPTOR_VERSION;
   desc.structSize = sizeof(desc);
   desc.meta.Width = cmd->width;
   desc.meta.Height = cmd->height;
   desc.meta.MipLevels = cmd->mip_levels;
   desc.meta.ArraySize = cmd->array_size;
   desc.meta.Format = cmd->format;
   desc.meta.SampleDesc.Count = cmd->sample_count;
   desc.meta.SampleDesc.Quality = 0;
   desc.meta.Usage = cmd->usage;
   desc.meta.BindFlags = cmd->bind_flags;
   desc.meta.CPUAccessFlags = cmd->cpu_access_flags;
   desc.meta.MiscFlags = cmd->misc_flags;
   desc.meta.TextureLayout = cmd->export_info.texture_layout;
   desc.drmFormatModifier = cmd->export_info.modifier;
   desc.planeCount = cmd->export_info.plane_count;
   for (uint32_t i = 0; i < desc.planeCount; i++) {
      desc.planes[i].offset = cmd->export_info.planes[i].offset;
      desc.planes[i].pitch = cmd->export_info.planes[i].pitch;
   }
   desc.allocationSize = cmd->export_info.allocation_size;
#endif

   /* The import dup()s the fd internally; hold our own reference so a
    * concurrent resource destroy can't invalidate res->u.fd mid-call. */
   desc.fd = attached_fd;

   PFN_ID3D11Device_OpenSharedResource open_shared =
      NPT_COM_VTBL_FUNC(PFN_ID3D11Device_OpenSharedResource,
                        npt_com_vtable(device),
                        NPT_VTBL_ID3D11Device_OpenSharedResource);

   void *texture = NULL;
   HRESULT hr = open_shared(device, (HANDLE)(uintptr_t)&desc,
                            &NPT_IID_ID3D11Texture2D, &texture);
   close(desc.fd);

   if (NPT_FAILED(hr) || !texture) {
      npt_log("shared: open: res_id %u import failed "
              "(hr=0x%x format=%u modifier=0x%016" PRIx64
              " pitch=%" PRIu64 " size=%" PRIu64 ")",
              cmd->res_id, hr, cmd->format, cmd->export_info.modifier,
              cmd->export_info.planes[0].pitch,
              cmd->export_info.allocation_size);
      return NPT_FAILED(hr) ? hr : NPT_E_FAIL;
   }

   /* The freshly imported texture carries one reference; the object
    * table registration is what the guest's minted id releases. */
   npt_context_register_object(ctx, cmd->mint_object_id, texture,
                               NPT_OBJECT_TYPE_ID3D11TEXTURE2D);
   if (npt_context_lookup_object(ctx, NULL, cmd->mint_object_id,
                                 NPT_OBJECT_TYPE_ID3D11TEXTURE2D) != texture) {
      npt_log("shared: open: failed to register id 0x%016" PRIx64,
              cmd->mint_object_id);
      npt_com_release(texture);
      return NPT_E_FAIL;
   }

   npt_log("shared: opened res_id=%u -> id 0x%016" PRIx64 " (ctx %u)",
           cmd->res_id, cmd->mint_object_id, ctx->ctx_id);
   return NPT_S_OK;
}
