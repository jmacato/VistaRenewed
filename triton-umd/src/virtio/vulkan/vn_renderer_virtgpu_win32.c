#include "vn_renderer_internal.h"

#include <winddk_compat.h>
#include <d3d10umddi.h>
#include <d3dkmthk.h>
#include <d3dukmdt.h>
#include <dxgiddi.h>
#include <winnt.h>
#include <winternl.h>

#include "util/os_file.h"
#include "util/sparse_array.h"

#include <vulkan/vulkan_d3dddi.h>
#include "virtio/virtio-gpu/venus_hw.h"
#include "virtio/virtio-gpu/wddm_hw.h"

#define VIRTGPU_PCI_VENDOR_ID 0x1af4
#define VIRTGPU_PCI_DEVICE_ID 0x1050
#define VIRTGPU_WIN_DEVICE_ID "PCI\\VEN_1AF4&DEV_1050"

struct virtgpu;

struct virtgpu_shmem {
   struct vn_renderer_shmem base;
   D3DKMT_HANDLE alloc;
   union {
      D3DKMT_HANDLE kmt;
      HANDLE h;
   };
};

struct virtgpu_bo {
   struct vn_renderer_bo base;
   D3DKMT_HANDLE alloc;
   union {
      struct {
         D3DKMT_HANDLE local;
         D3DKMT_HANDLE global;
      } kmt;

      HANDLE h;
   } /* resource */;
   uint32_t blob_flags;
};

struct virtgpu_sync {
   struct vn_renderer_sync base;

   /*
    * drm_syncobj is in one of these states
    *
    *  - value N:      drm_syncobj has a signaled fence chain with seqno N
    *  - pending N->M: drm_syncobj has an unsignaled fence chain with seqno M
    *                  (which may point to another unsignaled fence chain with
    *                   seqno between N and M, and so on)
    *
    * TODO Do we want to use binary drm_syncobjs?  They would be
    *
    *  - value 0: drm_syncobj has no fence
    *  - value 1: drm_syncobj has a signaled fence with seqno 0
    *
    * They are cheaper but require special care.
    */
   uint32_t syncobj_handle;
};

struct virtgpu {
   struct vn_renderer base;

   struct vn_instance *instance;

   VkD3DDDICallbacks *ddicb;
   struct {
      D3DKMT_HANDLE adapter;
      D3DKMT_HANDLE device;
      LUID luid;
      HINSTANCE lib;
      struct {
         PFND3DKMT_QUERYADAPTERINFO queryAdapterInfo;
         PFND3DKMT_ESCAPE escape;
         PFND3DKMT_RENDER render;
         PFND3DKMT_SIGNALSYNCHRONIZATIONOBJECT2 signalSynchronizationObject2;
         PFND3DKMT_CREATECONTEXT createContext;
         PFND3DKMT_DESTROYCONTEXT destroyContext;
         PFND3DKMT_CREATEALLOCATION createAllocation;
         PFND3DKMT_DESTROYALLOCATION destroyAllocation;
         PFND3DKMT_LOCK lock;
         PFND3DKMT_UNLOCK unlock;
         PFND3DKMT_QUERYRESOURCEINFO queryResourceInfo;
         PFND3DKMT_OPENRESOURCE openResource;
         PFND3DKMT_CREATEDEVICE createDevice;
         PFND3DKMT_DESTROYDEVICE destroyDevice;
         PFND3DKMT_OPENADAPTERFROMHDC openAdapterFromHdc;
         PFND3DKMT_CLOSEADAPTER closeAdapter;
      } cb;
   } d3dkmt;

   struct {
      mtx_t lock;

      union {
         D3DKMT_HANDLE kmt;
         HANDLE h;
      };

      void *cmd_buf;
      size_t cmd_size;

      D3DDDI_ALLOCATIONLIST *alloc_list;
      size_t alloc_size;

      D3DDDI_PATCHLOCATIONLIST *patch_list;
      size_t patch_size;
   } ctx;

   struct {
      uint16_t domain;
      uint8_t bus;
      uint8_t dev;
      uint8_t func;
   } pci_bus_info;

   uint32_t max_timeline_count;

   struct {
      uint32_t id;
      uint32_t version;
      struct virgl_renderer_capset_venus data;
   } capset;

   uint32_t shmem_blob_mem;
   uint32_t bo_blob_mem;

   struct util_sparse_array syncobj_array;
   /* note that we use kmt_handle instead of res_id to index because
    * res_id is monotonically increasing by default (see
    * virtio_gpu_resource_id_get)
    */
   struct util_sparse_array shmem_array;
   struct util_sparse_array bo_array;

   mtx_t win32_handle_import_mutex;

   struct vn_renderer_shmem_cache shmem_cache;

   // bool supports_cross_device;
};

static inline NTSTATUS
hr_to_nt(struct virtgpu *gpu, HRESULT hr)
{
   switch (hr) {
   case S_OK:
      return STATUS_SUCCESS;
   case E_OUTOFMEMORY:
      return STATUS_NO_MEMORY;
   case E_INVALIDARG:
      return STATUS_INVALID_PARAMETER;
   default:
      vn_log(gpu->instance, "Unknown HRESULT: %lx", hr);
      return STATUS_INVALID_PARAMETER;
   }
}

#include "util/hash_table.h"
#include "util/u_idalloc.h"

static struct {
   once_flag init;
   mtx_t mutex;
   struct hash_table *syncobjs;
   struct util_idalloc ida;

   // int signaled_fd;
   HANDLE signaled_fd;
} sim;

struct sim_syncobj {
   mtx_t mutex;
   uint64_t point;

   HANDLE pending_fd;
   uint64_t pending_point;
   bool pending_cpu;
};

static uint32_t
sim_syncobj_create(struct virtgpu *gpu, bool signaled)
{
   struct sim_syncobj *syncobj = calloc(1, sizeof(*syncobj));
   if (!syncobj)
      return 0;

   mtx_init(&syncobj->mutex, mtx_plain);
   syncobj->pending_fd = NULL;

   mtx_lock(&sim.mutex);

   /* initialize lazily */
   if (!sim.syncobjs) {
      sim.syncobjs = _mesa_pointer_hash_table_create(NULL);
      if (!sim.syncobjs) {
         mtx_unlock(&sim.mutex);
         mtx_destroy(&syncobj->mutex);
         free(syncobj);
         return 0;
      }

      util_idalloc_init(&sim.ida, 32);

      // TODO: is this actually needed?
      /*
      struct drm_virtgpu_execbuffer args = {
         .flags = VIRTGPU_EXECBUF_RING_IDX | VIRTGPU_EXECBUF_FENCE_FD_OUT,
         .ring_idx = 0, / * CPU ring * /
      };
      int ret = drmIoctl(gpu->fd, DRM_IOCTL_VIRTGPU_EXECBUFFER, &args);
      if (ret || args.fence_fd < 0) {
         _mesa_hash_table_destroy(sim.syncobjs, NULL);
         sim.syncobjs = NULL;
         mtx_unlock(&sim.mutex);
         mtx_destroy(&syncobj->mutex);
         free(syncobj);
         return 0;
      }
      sim.signaled_fd = args.fence_fd;
      */

      sim.signaled_fd = CreateEventA(NULL, TRUE, TRUE, NULL);
      if (sim.signaled_fd == NULL) {
         _mesa_hash_table_destroy(sim.syncobjs, NULL);
         sim.syncobjs = NULL;
         mtx_unlock(&sim.mutex);
         mtx_destroy(&syncobj->mutex);
         free(syncobj);
         return 0;
      }
      // vn_log(gpu->instance, "created handle %p", sim.signaled_fd);
   }

   const unsigned syncobj_handle = util_idalloc_alloc(&sim.ida) + 1;
   _mesa_hash_table_insert(sim.syncobjs,
                           (const void *)(uintptr_t)syncobj_handle, syncobj);

   mtx_unlock(&sim.mutex);

   return syncobj_handle;
}

static void
sim_syncobj_destroy(struct virtgpu *gpu, uint32_t syncobj_handle)
{
   struct sim_syncobj *syncobj = NULL;

   mtx_lock(&sim.mutex);

   struct hash_entry *entry = _mesa_hash_table_search(
      sim.syncobjs, (const void *)(uintptr_t)syncobj_handle);
   if (entry) {
      syncobj = entry->data;
      _mesa_hash_table_remove(sim.syncobjs, entry);
      util_idalloc_free(&sim.ida, syncobj_handle - 1);
   }

   mtx_unlock(&sim.mutex);

   if (syncobj) {
      if (syncobj->pending_fd != NULL)
         CloseHandle(syncobj->pending_fd);
      mtx_destroy(&syncobj->mutex);
      free(syncobj);
   }
}

static VkResult
sim_syncobj_poll(HANDLE fd, int poll_timeout)
{
   DWORD ret = WaitForSingleObject(fd, poll_timeout);

   if (ret == WAIT_OBJECT_0) {
      return VK_SUCCESS;
   } else if (ret == WAIT_TIMEOUT) {
      return VK_TIMEOUT;
   } else {
      return VK_ERROR_DEVICE_LOST;
   }
}

static void
sim_syncobj_set_point_locked(struct sim_syncobj *syncobj, uint64_t point)
{
   syncobj->point = point;

   if (syncobj->pending_fd != NULL) {
      CloseHandle(syncobj->pending_fd);
      syncobj->pending_fd = NULL;
      syncobj->pending_point = point;
   }
}

static void
sim_syncobj_update_point_locked(struct vn_instance *instance,
                                struct sim_syncobj *syncobj,
                                int poll_timeout)
{
   if (syncobj->pending_fd != NULL) {
      VkResult result;
      if (syncobj->pending_cpu) {
         if (poll_timeout == -1) {
            const int max_cpu_timeout = 2000;
            poll_timeout = max_cpu_timeout;
            // vn_log(instance, "waiting for handle %p", syncobj->pending_fd);
            result = sim_syncobj_poll(syncobj->pending_fd, poll_timeout);
            if (result == VK_TIMEOUT) {
               vn_log(NULL, "cpu sync timed out after %dms; ignoring",
                      poll_timeout);
               result = VK_SUCCESS;
            }
         } else {
            // vn_log(instance, "waiting for handle %p", syncobj->pending_fd);
            result = sim_syncobj_poll(syncobj->pending_fd, poll_timeout);
         }
      } else {
         // vn_log(instance, "waiting for handle %p", syncobj->pending_fd);
         result = sim_syncobj_poll(syncobj->pending_fd, poll_timeout);
      }
      if (result == VK_SUCCESS) {
         CloseHandle(syncobj->pending_fd);
         syncobj->pending_fd = NULL;
         syncobj->point = syncobj->pending_point;
      }
   }
}

static struct sim_syncobj *
sim_syncobj_lookup(struct virtgpu *gpu, uint32_t syncobj_handle)
{
   struct sim_syncobj *syncobj = NULL;

   mtx_lock(&sim.mutex);
   struct hash_entry *entry = _mesa_hash_table_search(
      sim.syncobjs, (const void *)(uintptr_t)syncobj_handle);
   if (entry)
      syncobj = entry->data;
   mtx_unlock(&sim.mutex);

   return syncobj;
}

static bool
sim_syncobj_reset(struct virtgpu *gpu, uint32_t syncobj_handle)
{
   struct sim_syncobj *syncobj = sim_syncobj_lookup(gpu, syncobj_handle);
   if (!syncobj)
      return false;

   mtx_lock(&syncobj->mutex);
   sim_syncobj_set_point_locked(syncobj, 0);
   mtx_unlock(&syncobj->mutex);

   return true;
}

static bool
sim_syncobj_query(struct virtgpu *gpu,
                  uint32_t syncobj_handle,
                  uint64_t *point)
{
   struct sim_syncobj *syncobj = sim_syncobj_lookup(gpu, syncobj_handle);
   if (!syncobj)
      return false;

   mtx_lock(&syncobj->mutex);
   sim_syncobj_update_point_locked(gpu->instance, syncobj, 0);
   *point = syncobj->point;
   mtx_unlock(&syncobj->mutex);

   return true;
}

static bool
sim_syncobj_signal(struct virtgpu *gpu,
                   uint32_t syncobj_handle,
                   uint64_t point)
{
   struct sim_syncobj *syncobj = sim_syncobj_lookup(gpu, syncobj_handle);
   if (!syncobj)
      return false;

   mtx_lock(&syncobj->mutex);
   sim_syncobj_set_point_locked(syncobj, point);
   mtx_unlock(&syncobj->mutex);

   return true;
}

static bool
sim_syncobj_submit(struct virtgpu *gpu,
                   uint32_t syncobj_handle,
                   HANDLE sync_fd,
                   uint64_t point,
                   bool cpu)
{
   struct sim_syncobj *syncobj = sim_syncobj_lookup(gpu, syncobj_handle);
   if (!syncobj)
      return false;

   HANDLE pending_fd = NULL;
   HANDLE proc = GetCurrentProcess();
   bool ret = DuplicateHandle(proc, sync_fd, proc, &pending_fd, 0, false,
                              DUPLICATE_SAME_ACCESS);
   if (!ret) {
      vn_log(gpu->instance, "failed to dup sync handle");
      return false;
   }

   mtx_lock(&syncobj->mutex);

   if (syncobj->pending_fd != NULL) {
      mtx_unlock(&syncobj->mutex);

      /* TODO */
      vn_log(gpu->instance, "sorry, no simulated timeline semaphore");
      CloseHandle(pending_fd);
      return false;
   }
   if (syncobj->point >= point)
      vn_log(gpu->instance, "non-monotonic signaling");

   syncobj->pending_fd = pending_fd;
   syncobj->pending_point = point;
   syncobj->pending_cpu = cpu;

   mtx_unlock(&syncobj->mutex);

   return true;
}

static int
timeout_to_poll_timeout(uint64_t timeout)
{
   const uint64_t ns_per_ms = 1000000;
   const uint64_t ms = (timeout + ns_per_ms - 1) / ns_per_ms;
   if (!ms && timeout)
      return INFINITE;
   return ms <= INT_MAX ? ms : INFINITE;
}

static VkResult
sim_syncobj_wait(struct virtgpu *gpu,
                 const struct vn_renderer_wait *wait,
                 bool wait_avail)
{
   if (wait_avail)
      return VK_ERROR_DEVICE_LOST;

   const int poll_timeout = timeout_to_poll_timeout(wait->timeout);

   /* TODO poll all fds at the same time */
   for (uint32_t i = 0; i < wait->sync_count; i++) {
      struct virtgpu_sync *sync = (struct virtgpu_sync *)wait->syncs[i];
      const uint64_t point = wait->sync_values[i];

      struct sim_syncobj *syncobj =
         sim_syncobj_lookup(gpu, sync->syncobj_handle);
      if (!syncobj)
         return VK_ERROR_DEVICE_LOST;

      mtx_lock(&syncobj->mutex);

      if (syncobj->point < point)
         sim_syncobj_update_point_locked(gpu->instance, syncobj,
                                         poll_timeout);

      if (syncobj->point < point) {
         if (wait->wait_any && i < wait->sync_count - 1 &&
             syncobj->pending_fd == NULL) {
            mtx_unlock(&syncobj->mutex);
            continue;
         }
         errno = ETIME;
         mtx_unlock(&syncobj->mutex);
         return VK_TIMEOUT;
      }

      mtx_unlock(&syncobj->mutex);

      if (wait->wait_any)
         break;

      /* TODO adjust poll_timeout */
   }

   return VK_SUCCESS;
}

static HANDLE
sim_syncobj_export(struct virtgpu *gpu, uint32_t syncobj_handle)
{
   struct sim_syncobj *syncobj = sim_syncobj_lookup(gpu, syncobj_handle);
   if (!syncobj)
      return NULL;

   HANDLE fd = NULL;
   HANDLE proc = GetCurrentProcess();
   mtx_lock(&syncobj->mutex);
   HANDLE in =
      syncobj->pending_fd != NULL ? syncobj->pending_fd : sim.signaled_fd;
   if (!DuplicateHandle(proc, in, proc, &fd, 0, false,
                        DUPLICATE_SAME_ACCESS)) {
      vn_log(gpu->instance, "failed to duplicate handle");
   }
   mtx_unlock(&syncobj->mutex);

   return fd;
}

static uint32_t
sim_syncobj_import(struct virtgpu *gpu, uint32_t syncobj_handle, HANDLE fd)
{
   struct sim_syncobj *syncobj = sim_syncobj_lookup(gpu, syncobj_handle);
   if (!syncobj)
      return 0;

   if (!sim_syncobj_submit(gpu, syncobj_handle, fd, 1, false))
      return 0;

   return syncobj_handle;
}

static VkResult
sim_submit_signal_syncs(struct virtgpu *gpu,
                        HANDLE sync_fd,
                        struct vn_renderer_sync *const *syncs,
                        const uint64_t *sync_values,
                        uint32_t sync_count,
                        bool cpu)
{
   for (uint32_t i = 0; i < sync_count; i++) {
      struct virtgpu_sync *sync = (struct virtgpu_sync *)syncs[i];
      const uint64_t pending_point = sync_values[i];

      if (!sim_syncobj_submit(gpu, sync->syncobj_handle, sync_fd,
                              pending_point, cpu)) {
         return VK_ERROR_DEVICE_LOST;
      }
   }

   return VK_SUCCESS;
}

static NTSTATUS
virtgpu_ioctl_create_context(struct virtgpu *gpu)
{
   if (gpu->ddicb != NULL) {
      D3DDDICB_CREATECONTEXT context = {};
      NTSTATUS status =
         hr_to_nt(gpu, gpu->ddicb->pKTCallbacks->pfnCreateContextCb(
                          gpu->ddicb->hRTDevice, &context));
      if (!NT_SUCCESS(status)) {
         return status;
      }

      gpu->ddicb->hContext = context.hContext;

      gpu->ctx.h = context.hContext;

      gpu->ctx.cmd_buf = context.pCommandBuffer;
      gpu->ctx.cmd_size = context.CommandBufferSize;

      gpu->ctx.alloc_list = context.pAllocationList;
      gpu->ctx.alloc_size = context.AllocationListSize;

      gpu->ctx.patch_list = context.pPatchLocationList;
      gpu->ctx.patch_size = context.PatchLocationListSize;

      return STATUS_SUCCESS;
   } else {
      D3DKMT_CREATECONTEXT context = {
         .hDevice = gpu->d3dkmt.device,
         .ClientHint = D3DKMT_CLIENTHINT_VULKAN,
      };

      NTSTATUS status = gpu->d3dkmt.cb.createContext(&context);
      if (!NT_SUCCESS(status)) {
         return status;
      }

      gpu->ctx.kmt = context.hContext;

      gpu->ctx.cmd_buf = context.pCommandBuffer;
      gpu->ctx.cmd_size = context.CommandBufferSize;

      gpu->ctx.alloc_list = context.pAllocationList;
      gpu->ctx.alloc_size = context.AllocationListSize;

      gpu->ctx.patch_list = context.pPatchLocationList;
      gpu->ctx.patch_size = context.PatchLocationListSize;

      return STATUS_SUCCESS;
   }
}

static NTSTATUS
virtgpu_ioctl_render(struct virtgpu *gpu,
                     unsigned cmd_offset,
                     unsigned cmd_length,
                     unsigned alloc_count,
                     void *priv,
                     unsigned priv_size)
{
   if (gpu->ddicb != NULL) {
      D3DDDICB_RENDER render = {
         .hContext = gpu->ctx.h,
         .CommandOffset = cmd_offset,
         .CommandLength = cmd_length,
         .NumAllocations = alloc_count,
         .NumPatchLocations = alloc_count,
         .pPrivateDriverData = priv,
         .PrivateDriverDataSize = priv_size,
      };

      NTSTATUS status = hr_to_nt(gpu, gpu->ddicb->pKTCallbacks->pfnRenderCb(
                                         gpu->ddicb->hRTDevice, &render));

      gpu->ctx.cmd_buf = render.pNewCommandBuffer;
      gpu->ctx.cmd_size = render.NewCommandBufferSize;

      gpu->ctx.alloc_list = render.pNewAllocationList;
      gpu->ctx.alloc_size = render.NewAllocationListSize;

      gpu->ctx.patch_list = render.pNewPatchLocationList;
      gpu->ctx.patch_size = render.NewPatchLocationListSize;

      return status;
   } else {
      D3DKMT_RENDER render = {
         .hContext = gpu->ctx.kmt,
         .CommandOffset = cmd_offset,
         .CommandLength = cmd_length,
         .AllocationCount = alloc_count,
         .PatchLocationCount = alloc_count,
         .pPrivateDriverData = priv,
         .PrivateDriverDataSize = priv_size,
      };

      NTSTATUS status = gpu->d3dkmt.cb.render(&render);

      gpu->ctx.cmd_buf = render.pNewCommandBuffer;
      gpu->ctx.cmd_size = render.NewCommandBufferSize;

      gpu->ctx.alloc_list = render.pNewAllocationList;
      gpu->ctx.alloc_size = render.NewAllocationListSize;

      gpu->ctx.patch_list = render.pNewPatchLocationList;
      gpu->ctx.patch_size = render.NewPatchLocationListSize;

      return status;
   }
}

static NTSTATUS
virtgpu_ioctl_signal(struct virtgpu *gpu, HANDLE fence)
{
   if (gpu->ddicb != NULL) {
      D3DDDICB_SIGNALSYNCHRONIZATIONOBJECT2 signal = {
           .hContext = gpu->ctx.h,
           .ObjectCount = 0,
           .BroadcastContextCount = 0,
           .Flags = {
               .EnqueueCpuEvent = TRUE,
           },
           .CpuEventHandle = fence,
       };
      return hr_to_nt(
         gpu, gpu->ddicb->pKTCallbacks->pfnSignalSynchronizationObject2Cb(
                 gpu->ddicb->hRTDevice, &signal));
   } else {
      D3DKMT_SIGNALSYNCHRONIZATIONOBJECT2 signal = {
           .hContext = gpu->ctx.kmt,
           .ObjectCount = 0,
           .BroadcastContextCount = 0,
           .Flags = {
               .EnqueueCpuEvent = TRUE,
           },
           .CpuEventHandle = fence,
       };

      return gpu->d3dkmt.cb.signalSynchronizationObject2(&signal);
   }
}

static VkResult
sim_submit(struct virtgpu *gpu, const struct vn_renderer_submit *submit)
{
   assert(submit->bo_count < gpu->ctx.alloc_size);
   assert(submit->batch_count);

   VkResult ret = VK_SUCCESS;
   for (uint32_t i = 0; i < submit->batch_count; i++) {
      const struct vn_renderer_submit_batch *batch = &submit->batches[i];
      mtx_lock(&gpu->ctx.lock);

      for (uint32_t i = 0; i < submit->bo_count; i++) {
         struct virtgpu_bo *bo = (struct virtgpu_bo *)submit->bos[i];
         assert(bo->alloc != 0);
         //if (bo->alloc == 0) return VK_ERROR_FEATURE_NOT_PRESENT; // TODO: we should not call render here, but rather save commands into present command buffer
         gpu->ctx.alloc_list[i].hAllocation = bo->alloc;
         gpu->ctx.patch_list[i].AllocationIndex = i;
      }

      VIOGPU_COMMAND_HDR *hdr = gpu->ctx.cmd_buf;
      hdr->type = VIOGPU_CMD_SUBMIT;
      hdr->size = batch->cs_size;
      hdr->flags = VIOGPU_EXECBUF_RING_IDX, hdr->ring_idx = batch->ring_idx;

      assert(batch->cs_size + sizeof(*hdr) <= gpu->ctx.cmd_size);
      memcpy((uint8_t *)gpu->ctx.cmd_buf + sizeof(*hdr), batch->cs_data,
             batch->cs_size);
      NTSTATUS status = virtgpu_ioctl_render(
         gpu, 0, sizeof(*hdr) + batch->cs_size, submit->bo_count, NULL, 0);
      mtx_unlock(&gpu->ctx.lock);
      if (!NT_SUCCESS(status)) {
         vn_log(gpu->instance, "failed to render: 0x%lx", status);
         break;
      }

      if (batch->sync_count > 0) {
         HANDLE fence = CreateEventA(NULL, TRUE, FALSE, NULL);
         // vn_log(gpu->instance, "created handle %p", fence);
         NTSTATUS status = virtgpu_ioctl_signal(gpu, fence);
         if (!NT_SUCCESS(status)) {
            vn_log(gpu->instance, "failed to execbuffer: 0x%lx", status);
            break;
         }

         ret = sim_submit_signal_syncs(gpu, fence, batch->syncs,
                                       batch->sync_values, batch->sync_count,
                                       batch->ring_idx == 0);
         CloseHandle(fence);
         if (ret != VK_SUCCESS)
            break;
      }
   }

   return ret;
}

static NTSTATUS
virtgpu_ioctl_getparam(struct virtgpu *gpu,
                       KMTQUERYADAPTERINFOTYPE type,
                       void *priv,
                       unsigned priv_size)
{
   if (gpu->ddicb != NULL) {
      D3DDDICB_QUERYADAPTERINFO query = {
         .pPrivateDriverData = priv,
         .PrivateDriverDataSize = priv_size,
      };
      return hr_to_nt(gpu,
                      gpu->ddicb->pAdapterCallbacks->pfnQueryAdapterInfoCb(
                         gpu->ddicb->hRTAdapter, &query));
   } else {
      D3DKMT_QUERYADAPTERINFO query = {
         .hAdapter = gpu->d3dkmt.adapter,
         .Type = KMTQAITYPE_UMDRIVERPRIVATE,
         .pPrivateDriverData = priv,
         .PrivateDriverDataSize = priv_size,
      };

      return gpu->d3dkmt.cb.queryAdapterInfo(&query);
   }
}

static NTSTATUS
virtgpu_ioctl_escape(struct virtgpu *gpu, VIOGPU_ESCAPE *priv)
{
   if (gpu->ddicb != NULL) {
      D3DDDICB_ESCAPE escape = {
         .hDevice = gpu->ddicb->hRTDevice,
         .pPrivateDriverData = priv,
         .PrivateDriverDataSize = sizeof(*priv),
         .hContext = gpu->ctx.h,
      };
      return hr_to_nt(gpu, gpu->ddicb->pKTCallbacks->pfnEscapeCb(
                              gpu->ddicb->hRTAdapter, &escape));
   } else {
      D3DKMT_ESCAPE escape = {
         .hAdapter = gpu->d3dkmt.adapter,
         .hDevice = gpu->d3dkmt.device,
         .pPrivateDriverData = priv,
         .PrivateDriverDataSize = sizeof(*priv),
      };

      return gpu->d3dkmt.cb.escape(&escape);
   }
}

static NTSTATUS
virtgpu_ioctl_get_caps(struct virtgpu *gpu,
                       uint32_t id,
                       uint32_t version,
                       void *capset,
                       size_t capset_size)
{
   VIOGPU_ESCAPE caps = {
      .Type = VIOGPU_GET_CAPS,
      .DataLength = sizeof(caps.Capset),
      .Capset = {
         .CapsetId = id,
         .Version = version,
         .Size = capset_size,
         .Capset = VioGpuUmPtr(capset),
      },
   };

   return virtgpu_ioctl_escape(gpu, &caps);
}

static NTSTATUS
virtgpu_ioctl_init_map(struct virtgpu *gpu, D3DKMT_HANDLE handle)
{
   mtx_lock(&gpu->ctx.lock);

   gpu->ctx.alloc_list[0].hAllocation = handle;
   gpu->ctx.patch_list[0].AllocationIndex = 0;

   VIOGPU_COMMAND_HDR *hdr = gpu->ctx.cmd_buf;
   hdr->type = VIOGPU_CMD_MAP_BLOB;
   hdr->size = sizeof(ULONG);
   hdr->flags = 0;
   hdr->ring_idx = 0;

   ULONG *index = (void *)(hdr + 1);
   *index = 0;
   memset(index + 1, 0, sizeof(*hdr));

   NTSTATUS status = virtgpu_ioctl_render(
      gpu, 0, 2 * sizeof(*hdr) + sizeof(ULONG), 1, NULL, 0);
   mtx_unlock(&gpu->ctx.lock);
   if (!NT_SUCCESS(status)) {
      return status;
   }

   HANDLE fence = CreateEventA(NULL, TRUE, FALSE, NULL);
   // vn_log(gpu->instance, "created handle %p", fence);
   status = virtgpu_ioctl_signal(gpu, fence);
   if (!NT_SUCCESS(status)) {
      return status;
   }

   // vn_log(gpu->instance, "waiting for handle %p", fence);
   if (WaitForSingleObject(fence, INFINITE) != WAIT_OBJECT_0) {
      return STATUS_ABANDONED_WAIT_0;
   }

   return STATUS_SUCCESS;
}

static NTSTATUS
virtgpu_ioctl_destroy_map(struct virtgpu *gpu, D3DKMT_HANDLE handle)
{
   mtx_lock(&gpu->ctx.lock);

   gpu->ctx.alloc_list[0].hAllocation = handle;
   gpu->ctx.patch_list[0].AllocationIndex = 0;

   VIOGPU_COMMAND_HDR *hdr = gpu->ctx.cmd_buf;
   hdr->type = VIOGPU_CMD_UNMAP_BLOB;
   hdr->size = sizeof(ULONG);
   hdr->flags = 0;
   hdr->ring_idx = 0;

   ULONG *index = (void *)(hdr + 1);
   *index = 0;
   memset(index + 1, 0, sizeof(*hdr));

   NTSTATUS status = virtgpu_ioctl_render(
      gpu, 0, 2 * sizeof(*hdr) + sizeof(ULONG), 1, NULL, 0);
   mtx_unlock(&gpu->ctx.lock);
   if (!NT_SUCCESS(status)) {
      return status;
   }

   HANDLE fence = CreateEventA(NULL, TRUE, FALSE, NULL);
   // vn_log(gpu->instance, "created handle %p", fence);
   status = virtgpu_ioctl_signal(gpu, fence);
   if (!NT_SUCCESS(status)) {
      return status;
   }

   // vn_log(gpu->instance, "waiting for handle %p", fence);
   if (WaitForSingleObject(fence, INFINITE) != WAIT_OBJECT_0) {
      return STATUS_ABANDONED_WAIT_0;
   }

   return STATUS_SUCCESS;
   // return virtgpu_ioctl_unlock(gpu, handle);
}

static NTSTATUS
virtgpu_ioctl_wait(struct virtgpu *gpu)
{
   HANDLE fence = CreateEventA(NULL, TRUE, FALSE, NULL);
   // vn_log(gpu->instance, "created handle %p", fence);
   NTSTATUS status = virtgpu_ioctl_signal(gpu, fence);
   if (!NT_SUCCESS(status)) {
      return status;
   }
   // vn_log(gpu->instance, "waiting for handle %p", fence);
   if (WaitForSingleObject(fence, INFINITE) != WAIT_OBJECT_0) {
      return STATUS_ABANDONED_WAIT_0;
   }

   return STATUS_SUCCESS;
}

#define VIRTGPU_SYNC_OR_RETURN_NTSTATUS(gpu) \
   do { \
      NTSTATUS status = virtgpu_ioctl_wait(gpu); \
      if (!NT_SUCCESS(status)) { \
         return status; \
      } \
   } while (0)

static NTSTATUS
virtgpu_ioctl_resource_create_blob(struct virtgpu *gpu,
                                   uint32_t blob_mem,
                                   uint32_t blob_flags,
                                   size_t blob_size,
                                   uint64_t blob_id,
                                   uint32_t *res_id,
                                   D3DKMT_HANDLE *alloc_handle,
                                   D3DKMT_HANDLE *res_kmt_local,
                                   D3DKMT_HANDLE *res_kmt_global,
                                   HANDLE *res_h)
{
   blob_size = align64(blob_size, 4096);

   VIOGPU_CREATE_ALLOCATION_EXCHANGE alloc_priv = {
       .Type = VIOGPU_RESOURCE_TYPE_BLOB,
       .OptionsBlob = {
           .blob_mem = blob_mem,
           .blob_flags = blob_flags,
           .blob_id = blob_id,
       },
       .Size = blob_size,
   };

   VIOGPU_CREATE_RESOURCE_EXCHANGE res_priv = { 0 };

   D3DDDI_ALLOCATIONINFO alloc_info = {
      .pPrivateDriverData = &alloc_priv,
      .PrivateDriverDataSize = sizeof(alloc_priv),
   };

   bool is_shareable = !!(blob_flags & VIOGPU_BLOB_FLAG_USE_SHAREABLE);
   bool is_mappable = !!(blob_flags & VIOGPU_BLOB_FLAG_USE_MAPPABLE);

   // TODO: is this required?
   VIRTGPU_SYNC_OR_RETURN_NTSTATUS(gpu);

   if (gpu->ddicb != NULL) {
      D3DDDICB_ALLOCATE alloc = {
         .pPrivateDriverData = &res_priv,
         .PrivateDriverDataSize = sizeof(res_priv),
         .hResource = *res_h,
         .NumAllocations = 1,
         .pAllocationInfo = &alloc_info,
      };

      NTSTATUS status = hr_to_nt(gpu, gpu->ddicb->pKTCallbacks->pfnAllocateCb(
                                         gpu->ddicb->hRTDevice, &alloc));
      if (!NT_SUCCESS(status)) {
         return status;
      }
      *res_kmt_local = alloc.hKMResource;
      *alloc_handle = alloc_info.hAllocation;
   } else {
      D3DKMT_CREATEALLOCATION alloc = {
          .hDevice = gpu->d3dkmt.device,
          .pPrivateDriverData = &res_priv,
          .PrivateDriverDataSize = sizeof(res_priv),
          .NumAllocations = 1,
          .pAllocationInfo = &alloc_info,
          .Flags = {
              .CreateResource = 1,
              .CreateShared = is_shareable,
          },
      };
      NTSTATUS status = gpu->d3dkmt.cb.createAllocation(&alloc);
      if (!NT_SUCCESS(status)) {
         return status;
      }
      *res_kmt_local = alloc.hResource;
      if (res_kmt_global) {
         *res_kmt_global = alloc.hGlobalShare;
      }
      *alloc_handle = alloc_info.hAllocation;
   }

   // TODO: is this required?
   VIRTGPU_SYNC_OR_RETURN_NTSTATUS(gpu);

   VIOGPU_ESCAPE res_info = {
      .Type = VIOGPU_RES_INFO,
      .DataLength = sizeof(res_info.ResourceInfo),
      .ResourceInfo = {
         .ResHandle = *alloc_handle,
      },
   };

   NTSTATUS status = virtgpu_ioctl_escape(gpu, &res_info);
   if (!NT_SUCCESS(status)) {
      return status;
   }

   if (!res_info.ResourceInfo.IsBlob || !res_info.ResourceInfo.IsCreated) {
      return STATUS_INVALID_PARAMETER;
   }

   *res_id = res_info.ResourceInfo.Id;

   return is_mappable ? virtgpu_ioctl_init_map(gpu, *alloc_handle)
                      : STATUS_SUCCESS;
}

static NTSTATUS
virtgpu_ioctl_resource_destroy_blob(struct virtgpu *gpu,
                                    D3DKMT_HANDLE alloc_handle,
                                    D3DKMT_HANDLE res_kmt,
                                    HANDLE res_h)
{
   // TODO: is this required?
   VIRTGPU_SYNC_OR_RETURN_NTSTATUS(gpu);

   if (gpu->ddicb != NULL) {
      D3DDDICB_DEALLOCATE destroy = {
         .hResource = res_h,
         .NumAllocations = res_h == NULL ? 1 : 0,
         .HandleList = res_h == NULL ? &alloc_handle : NULL,
      };

      NTSTATUS status = hr_to_nt(gpu,
         gpu->ddicb->pKTCallbacks->pfnDeallocateCb(gpu->ddicb->hRTDevice,
                                                   &destroy));

      if (!NT_SUCCESS(status)) {
         return status;
      }
   } else {
      D3DKMT_DESTROYALLOCATION destroy = {
         .hDevice = gpu->d3dkmt.device,
         .hResource = res_kmt,
         .AllocationCount = res_kmt == 0 ? 1 : 0,
         .phAllocationList = res_kmt == 0 ? &alloc_handle : NULL,
      };

      NTSTATUS status = gpu->d3dkmt.cb.destroyAllocation(&destroy);
      if (!NT_SUCCESS(status)) {
         return status;
      }
   }

   // TODO: is this required?
   VIRTGPU_SYNC_OR_RETURN_NTSTATUS(gpu);

   return STATUS_SUCCESS;
}

static NTSTATUS
virtgpu_ioctl_lock(struct virtgpu *gpu, D3DKMT_HANDLE handle, void **ptr)
{
   if (gpu->ddicb != NULL) {
      D3DDDICB_LOCK lock = {
          .hAllocation = handle,
          .Flags = {
              // .IgnoreSync = 1,
              .LockEntire = 1,
          },
      };
      NTSTATUS status = hr_to_nt(gpu, gpu->ddicb->pKTCallbacks->pfnLockCb(
                                         gpu->ddicb->hRTDevice, &lock));
      if (!NT_SUCCESS(status)) {
         return status;
      }
      *ptr = lock.pData;
   } else {
      D3DKMT_LOCK lock = {
          .hDevice = gpu->d3dkmt.device,
          .Flags = {
              // .IgnoreSync = 1,
              .LockEntire = 1,
          },
          .hAllocation = handle,
      };
      NTSTATUS status = gpu->d3dkmt.cb.lock(&lock);
      if (!NT_SUCCESS(status)) {
         return status;
      }
      *ptr = lock.pData;
   }

   return STATUS_SUCCESS;
}

static NTSTATUS
virtgpu_ioctl_unlock(struct virtgpu *gpu, D3DKMT_HANDLE handle)
{
   if (gpu->ddicb != NULL) {
      D3DDDICB_UNLOCK unlock = {
         .NumAllocations = 1,
         .phAllocations = &handle,
      };
      return hr_to_nt(gpu, gpu->ddicb->pKTCallbacks->pfnUnlockCb(
                              gpu->ddicb->hRTDevice, &unlock));
   } else {
      D3DKMT_UNLOCK unlock = {
         .hDevice = gpu->d3dkmt.device,
         .NumAllocations = 1,
         .phAllocations = &handle,
      };
      return gpu->d3dkmt.cb.unlock(&unlock);
   }
}

static inline void
virtgpu_init_shmem_blob_mem(ASSERTED struct virtgpu *gpu)
{
   /* VIOGPU_BLOB_MEM_GUEST allocates from the guest system memory.  They are
    * logically contiguous in the guest but are sglists (iovecs) in the host.
    * That makes them slower to process in the host.  With host process
    * isolation, it also becomes impossible for the host to access sglists
    * directly.
    *
    * While there are ideas (and shipped code in some cases) such as creating
    * udmabufs from sglists, or having a dedicated guest heap, it seems the
    * easiest way is to reuse VIRTGPU_BLOB_MEM_HOST3D.  That is, when the
    * renderer sees a request to export a blob where
    *
    *  - blob_mem is VIOGPU_BLOB_MEM_HOST3D
    *  - blob_flags is VIOGPU_BLOB_FLAG_USE_MAPPABLE
    *  - blob_id is 0
    *
    * it allocates a host shmem.
    *
    * supports_blob_id_0 has been enforced by mandated render server config.
    */
   assert(gpu->capset.data.supports_blob_id_0);
   gpu->shmem_blob_mem = VIOGPU_BLOB_MEM_HOST3D;
}

static VkResult
virtgpu_init_context(struct virtgpu *gpu)
{
   assert(!gpu->capset.version);

   VIOGPU_ESCAPE ctx_init = {
       .Type = VIOGPU_CTX_INIT,
       .DataLength = sizeof(ctx_init.CtxInit),
       .CtxInit = {
           .CapsetID = gpu->capset.id,
           .NumRings = 64,
           .DebugName = "venus-win32",
       },
   };

   NTSTATUS status = virtgpu_ioctl_escape(gpu, &ctx_init);
   if (!NT_SUCCESS(status)) {
      if (VN_DEBUG(INIT)) {
         vn_log(gpu->instance, "failed to create context: 0x%lx", status);
      }
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   status = virtgpu_ioctl_create_context(gpu);
   if (!NT_SUCCESS(status)) {
      if (VN_DEBUG(INIT)) {
         vn_log(gpu->instance, "failed to create context: 0x%lx", status);
      }
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   return VK_SUCCESS;
}

static VkResult
virtgpu_init_capset(struct virtgpu *gpu)
{
   gpu->capset.id = VIOGPU_CAPSET_VENUS;
   gpu->capset.version = 0;

   NTSTATUS status =
      virtgpu_ioctl_get_caps(gpu, gpu->capset.id, gpu->capset.version,
                             &gpu->capset.data, sizeof(gpu->capset.data));
   if (!NT_SUCCESS(status)) {
      if (VN_DEBUG(INIT)) {
         vn_log(gpu->instance, "failed to get venus v%d capset: 0x%lx",
                gpu->capset.version, status);
      }
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   if (gpu->capset.data.wire_format_version == 0) {
      if (VN_DEBUG(INIT)) {
         vn_log(gpu->instance, "Unsupported wire format version %u",
                gpu->capset.data.wire_format_version);
      }
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   return VK_SUCCESS;
}

static VkResult
virtgpu_init_params(struct virtgpu *gpu)
{
   VIOGPU_ADAPTERINFO info = { 0 };

   NTSTATUS status = virtgpu_ioctl_getparam(gpu, KMTQAITYPE_UMDRIVERPRIVATE,
                                            &info, sizeof(info));

   if (!NT_SUCCESS(status)) {
      if (VN_DEBUG(INIT)) {
         vn_log(gpu->instance,
                "failed to get adapter info from kernel: 0x%lx", status);
      }
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   if (info.IamVioGPU != VIOGPU_IAM || !info.Flags.Supports3d) {
      if (VN_DEBUG(INIT)) {
         vn_log(gpu->instance, "no venus support in this driver");
      }
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   /* Don't care for VIRTGPU_BLOB_MEM_GUEST_VRAM since this driver is mainly
    * developed for QEMU, but whoever needs it may feel free to implement this */
   if (info.Flags.HasShmem) {
      gpu->bo_blob_mem = VIOGPU_BLOB_MEM_HOST3D;
   } else {
      if (VN_DEBUG(INIT)) {
         vn_log(
            gpu->instance,
            "driver does not support the required host-visible shmem region");
      }
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   /* Don't care about cross-device */
   // gpu->supports_cross_device = false;

   /* implied by CONTEXT_INIT uapi */
   gpu->max_timeline_count = 64;

   VIOGPU_ESCAPE pci_info = {
      .Type = VIOGPU_GET_PCI_INFO,
      .DataLength = sizeof(pci_info.PciInfo),
      .PciInfo = {},
   };

   status = virtgpu_ioctl_escape(gpu, &pci_info);
   if (!NT_SUCCESS(status)) {
      if (VN_DEBUG(INIT)) {
         vn_log(gpu->instance, "failed to get device pci info from kernel");
      }
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   gpu->pci_bus_info.domain = pci_info.PciInfo.Domain;
   gpu->pci_bus_info.bus = pci_info.PciInfo.Bus;
   gpu->pci_bus_info.dev = pci_info.PciInfo.Dev;
   gpu->pci_bus_info.func = pci_info.PciInfo.Func;

   return VK_SUCCESS;
}

static VkResult
virtgpu_find_adapter(struct virtgpu *gpu)
{
   DISPLAY_DEVICE adapter = {
      .cb = sizeof(adapter),
   };

   for (int i = 0; EnumDisplayDevicesA(NULL, i, &adapter, 0); i++) {
      if (_strnicmp(adapter.DeviceID, VIRTGPU_WIN_DEVICE_ID,
                    strlen(VIRTGPU_WIN_DEVICE_ID)) == 0) {
         HDC hdc = CreateDC(NULL, adapter.DeviceName, NULL, NULL);
         D3DKMT_OPENADAPTERFROMHDC open_adapter = {
            .hDc = hdc,
         };

         NTSTATUS status = gpu->d3dkmt.cb.openAdapterFromHdc(&open_adapter);
         if (!NT_SUCCESS(status)) {
            if (VN_DEBUG(INIT)) {
               vn_log(gpu->instance, "failed to open adapter %s: 0x%lx",
                      adapter.DeviceName, status);
            }

            continue;
         }
         // TODO: ReleaseDC(NULL, hdc);
         gpu->d3dkmt.adapter = open_adapter.hAdapter;
         gpu->d3dkmt.luid = open_adapter.AdapterLuid;

         if (VN_DEBUG(INIT)) {
            vn_log(gpu->instance, "using adapter %s (LUID %lx-%lx)",
                   adapter.DeviceName, open_adapter.AdapterLuid.HighPart,
                   open_adapter.AdapterLuid.LowPart);
         }
         return VK_SUCCESS;
      }
   }
   return VK_ERROR_INCOMPATIBLE_DRIVER;
}

static NTSTATUS
virtgpu_ioctl_create_device(struct virtgpu *gpu)
{
   if (gpu->ddicb != NULL) {
      /* Nothing to do here, device was already created before */
      return STATUS_SUCCESS;
   } else {
      D3DKMT_CREATEDEVICE create_device = {
         .hAdapter = gpu->d3dkmt.adapter,
      };
      NTSTATUS status = gpu->d3dkmt.cb.createDevice(&create_device);
      if (!NT_SUCCESS(status)) {
         return status;
      }

      gpu->d3dkmt.device = create_device.hDevice;
      return STATUS_SUCCESS;
   }
}

static VkResult
virtgpu_open(struct virtgpu *gpu, void *info)
{
   VkD3DDDICallbacks *callbacks = vk_find_struct(info, D3DDDI_CALLBACKS);
   if (callbacks != NULL) {
      /* D3D11 UMD */
      gpu->ddicb = callbacks;
   } else {
      /* Standalone Vulkan ICD, using D3DKMT */
      HINSTANCE gdi32lib = LoadLibraryA("GDI32.dll");
      gpu->d3dkmt.lib = gdi32lib;

      gpu->d3dkmt.cb.queryAdapterInfo =
         (void *)GetProcAddress(gdi32lib, "D3DKMTQueryAdapterInfo");
      gpu->d3dkmt.cb.escape =
         (void *)GetProcAddress(gdi32lib, "D3DKMTEscape");
      gpu->d3dkmt.cb.render =
         (void *)GetProcAddress(gdi32lib, "D3DKMTRender");
      gpu->d3dkmt.cb.signalSynchronizationObject2 = (void *)GetProcAddress(
         gdi32lib, "D3DKMTSignalSynchronizationObject2");
      gpu->d3dkmt.cb.createContext =
         (void *)GetProcAddress(gdi32lib, "D3DKMTCreateContext");
      gpu->d3dkmt.cb.destroyContext =
         (void *)GetProcAddress(gdi32lib, "D3DKMTDestroyContext");
      gpu->d3dkmt.cb.createAllocation =
         (void *)GetProcAddress(gdi32lib, "D3DKMTCreateAllocation");
      gpu->d3dkmt.cb.destroyAllocation =
         (void *)GetProcAddress(gdi32lib, "D3DKMTDestroyAllocation");
      gpu->d3dkmt.cb.lock = (void *)GetProcAddress(gdi32lib, "D3DKMTLock");
      gpu->d3dkmt.cb.unlock =
         (void *)GetProcAddress(gdi32lib, "D3DKMTUnlock");
      gpu->d3dkmt.cb.queryResourceInfo =
         (void *)GetProcAddress(gdi32lib, "D3DKMTQueryResourceInfo");
      gpu->d3dkmt.cb.openResource =
         (void *)GetProcAddress(gdi32lib, "D3DKMTOpenResource");
      gpu->d3dkmt.cb.createDevice =
         (void *)GetProcAddress(gdi32lib, "D3DKMTCreateDevice");
      gpu->d3dkmt.cb.destroyDevice =
         (void *)GetProcAddress(gdi32lib, "D3DKMTDestroyDevice");
      gpu->d3dkmt.cb.openAdapterFromHdc =
         (void *)GetProcAddress(gdi32lib, "D3DKMTOpenAdapterFromHdc");
      gpu->d3dkmt.cb.closeAdapter =
         (void *)GetProcAddress(gdi32lib, "D3DKMTCloseAdapter");

      NTSTATUS status = virtgpu_find_adapter(gpu);
      if (!NT_SUCCESS(status)) {
         return VK_ERROR_DEVICE_LOST;
      }
   }

   NTSTATUS status = virtgpu_ioctl_create_device(gpu);
   if (!NT_SUCCESS(status)) {
      return VK_ERROR_DEVICE_LOST;
   }

   return VK_SUCCESS;
}

static uint32_t
virtgpu_bo_blob_flags(struct virtgpu *gpu,
                      VkMemoryPropertyFlags flags,
                      VkExternalMemoryHandleTypeFlags external_handles)
{
   uint32_t blob_flags = 0;
   if (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
      blob_flags |= VIOGPU_BLOB_FLAG_USE_MAPPABLE;
   if (external_handles)
      blob_flags |= VIOGPU_BLOB_FLAG_USE_SHAREABLE;
   // if (external_handles & VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) {
   //    if (gpu->supports_cross_device)
   //       blob_flags |= VIOGPU_BLOB_FLAG_USE_CROSS_DEVICE;
   // }

   return blob_flags;
}

static NTSTATUS
virtgpu_ioctl_open_resource(struct virtgpu *gpu,
                            D3DKMT_HANDLE res_kmt_global,
                            D3DKMT_HANDLE *alloc,
                            D3DKMT_HANDLE *res_kmt_local,
                            VIOGPU_RES_INFO_REQ *res_info,
                            const VkD3DDDIOpenResource *d3d_open)
{
   if (gpu->ddicb != NULL) {
      assert(d3d_open != NULL);

      //const VIOGPU_CREATE_ALLOCATION_EXCHANGE *alloc_priv =
      //   d3d_open->pOpenResource->pOpenAllocationInfo[0].pPrivateDriverData;
      //assert(alloc_priv->Type == VIOGPU_RESOURCE_TYPE_BLOB);
      *alloc =
         d3d_open->pOpenResource->pOpenAllocationInfo[0].hAllocation;
      *res_kmt_local =
         d3d_open->pOpenResource->hKMResource.handle;

      *res_info = *(VIOGPU_RES_INFO_REQ *) d3d_open->pResourceInfo;

      return STATUS_SUCCESS;
   } else {
      D3DKMT_QUERYRESOURCEINFO query = {
         .hDevice = gpu->d3dkmt.device,
         .hGlobalShare = res_kmt_global,
      };

      NTSTATUS status = gpu->d3dkmt.cb.queryResourceInfo(&query);
      if (!NT_SUCCESS(status)) {
         return status;
      }

      assert(query.ResourcePrivateDriverDataSize >= sizeof(VIOGPU_CREATE_RESOURCE_EXCHANGE));
      assert(query.TotalPrivateDriverDataSize >= sizeof(VIOGPU_CREATE_ALLOCATION_EXCHANGE) * query.NumAllocations);

      size_t runtime_data_off = 0;
      size_t res_priv_off =
         runtime_data_off + align64(query.PrivateRuntimeDataSize, 8);
      size_t alloc_priv_off =
         res_priv_off + align64(query.ResourcePrivateDriverDataSize, 8);
      size_t alloc_list_off =
         alloc_priv_off + align64(query.TotalPrivateDriverDataSize, 8);

      size_t total_size = alloc_list_off + sizeof(D3DDDI_OPENALLOCATIONINFO) *
                                              query.NumAllocations;
      void *data = calloc(total_size, 1);
      uintptr_t p = (uintptr_t)data;

      void *runtime = (void *)(p + runtime_data_off);
      VIOGPU_CREATE_RESOURCE_EXCHANGE *resource_priv =
         (void *)(p + res_priv_off);

      VIOGPU_CREATE_ALLOCATION_EXCHANGE *full_alloc_priv =
         (void *)(p + alloc_priv_off);

      D3DDDI_OPENALLOCATIONINFO *alloc_list = (void *)(p + alloc_list_off);

      D3DKMT_OPENRESOURCE open = {
         .hDevice = gpu->d3dkmt.device,
         .hGlobalShare = res_kmt_global,
         .NumAllocations = query.NumAllocations,
         .pOpenAllocationInfo = alloc_list,
         .pResourcePrivateDriverData = resource_priv,
         .ResourcePrivateDriverDataSize = query.ResourcePrivateDriverDataSize,
         .pPrivateRuntimeData = runtime,
         .PrivateRuntimeDataSize = query.PrivateRuntimeDataSize,
         .pTotalPrivateDriverDataBuffer = full_alloc_priv,
         .TotalPrivateDriverDataBufferSize = query.TotalPrivateDriverDataSize,
      };

      status = gpu->d3dkmt.cb.openResource(&open);
      if (!NT_SUCCESS(status)) {
         goto end;
      }

      const VIOGPU_CREATE_ALLOCATION_EXCHANGE *alloc_priv =
         alloc_list[0].pPrivateDriverData;
      (void)alloc_priv;
      //assert(alloc_priv->Type == VIOGPU_RESOURCE_TYPE_BLOB);

      *alloc = alloc_list[0].hAllocation;
      *res_kmt_local = open.hResource;

      VIOGPU_ESCAPE res_esc = {
         .Type = VIOGPU_RES_INFO,
         .DataLength = sizeof(res_esc.ResourceInfo),
         .ResourceInfo = {
             .ResHandle = *alloc,
         },
      };

      status = virtgpu_ioctl_escape(gpu, &res_esc);
      if (!NT_SUCCESS(status)) {
         goto end;
      }
      *res_info = res_esc.ResourceInfo;

   end:
      free(data);
      return status;
   }
}

static VkResult
virtgpu_bo_create_from_handle(struct vn_renderer *renderer,
                              VkDeviceSize size,
                              vn_object_id mem_id,
                              bool is_kmt,
                              void *handle,
                              VkMemoryPropertyFlags flags,
                              const VkMemoryAllocateInfo *alloc_info,
                              struct vn_renderer_bo **out_bo)
{
   struct virtgpu *gpu = (struct virtgpu *)renderer;
   struct virtgpu_bo *bo = NULL;

   VIOGPU_RES_INFO_REQ res_info = {};

   mtx_lock(&gpu->win32_handle_import_mutex);

   // TODO: virtgpu_ioctl_open_resource_from_nthandle
   assert(is_kmt);

   D3DKMT_HANDLE alloc, kmt_local,
      kmt_global = (D3DKMT_HANDLE)(uintptr_t)handle;

   const VkD3DDDIOpenResource *d3d_open =
      vk_find_struct_const(alloc_info, D3DDDI_OPEN_RESOURCE);

   NTSTATUS status =
      virtgpu_ioctl_open_resource(gpu, kmt_global, &alloc, &kmt_local, &res_info, d3d_open);
   if (!NT_SUCCESS(status)) {
      vn_log(gpu->instance, "failed to open resource: 0x%lx", status);
      return VK_ERROR_DEVICE_LOST;
   }

   if (!alloc)
      goto fail;
   bo = util_sparse_array_get(&gpu->bo_array, alloc);

   /* Upon import, blob_flags is not passed to the kernel and is only for
    * internal use. Set it to what works best for us.
    * - blob mem: SHAREABLE + conditional MAPPABLE per VkMemoryPropertyFlags
    * - classic 3d: SHAREABLE only for export and to fail the map
    */
   uint32_t blob_flags = VIOGPU_BLOB_FLAG_USE_SHAREABLE;
   size_t mmap_size = 0;
   if (res_info.BlobMem) {
      /* must be VIOGPU_BLOB_MEM_HOST3D */
      if (res_info.BlobMem != gpu->bo_blob_mem) {
         vn_log(gpu->instance,
                "NT/KMT handle import failed: info.blob_mem(%lu) != "
                "gpu->bo_blob_mem(%u)",
                res_info.BlobMem, gpu->bo_blob_mem);
         goto fail;
      }

      blob_flags |= virtgpu_bo_blob_flags(gpu, flags, 0);

      /* mmap_size is only used when mappable */
      mmap_size = 0;
      if (blob_flags & VIOGPU_BLOB_FLAG_USE_MAPPABLE) {
         if (res_info.Size < size) {
            /* If queried blob size is smaller than requested allocation size,
             * we drop the mappable flag to defer the mapping failure till the
             * app attempts to map the imported memory.
             */
            blob_flags &= ~VIOGPU_BLOB_FLAG_USE_MAPPABLE;
         } else {
            /* Similar to virtgpu_bo_create_from_device_memory, the app can
             * do multiple imports with different sizes for suballocation. So
             * on the initial import, the mapping size has to be initialized
             * with the real size of the backing blob resource.
             */
            mmap_size = res_info.Size;
         }
      }
   }

   /* we check bo->alloc instead of bo->refcount because bo->refcount
    * might only be memset to 0 and is not considered initialized in theory
    */
   if (bo->alloc == alloc) {
      if (bo->base.mmap_size < mmap_size) {
         vn_log(gpu->instance,
                "NT/KMT handle import failed: bo->base.mmap_size(%zu) < "
                "mmap_size(%zu)",
                bo->base.mmap_size, mmap_size);
         goto fail;
      }
      if (blob_flags & ~bo->blob_flags) {
         vn_log(gpu->instance,
                "NT/KMT handle import failed: blob_flags(%u) & "
                "~bo->blob_flags(%u)",
                blob_flags, bo->blob_flags);
         goto fail;
      }

      /* we can't use vn_renderer_bo_ref as the refcount may drop to 0
       * temporarily before virtgpu_bo_destroy grabs the lock
       */
      vn_refcount_fetch_add_relaxed(&bo->base.refcount, 1);
   } else {
      *bo = (struct virtgpu_bo){
         .base = {
            .refcount = VN_REFCOUNT_INIT(1),
            .res_id = res_info.Id,
            .mmap_size = mmap_size,
         },
         .alloc = alloc,
         .blob_flags = blob_flags,
      };
   }
   if (gpu->ddicb != NULL) {
      bo->h = handle;
   } else {
      bo->kmt.local = kmt_local;
      bo->kmt.global = kmt_global;
   }

   mtx_unlock(&gpu->win32_handle_import_mutex);

   *out_bo = &bo->base;

   return VK_SUCCESS;

fail:
   mtx_unlock(&gpu->win32_handle_import_mutex);
   return VK_ERROR_INVALID_EXTERNAL_HANDLE;
}

static VkResult
virtgpu_bo_create_from_device_memory(
   struct vn_renderer *renderer,
   VkDeviceSize size,
   vn_object_id mem_id,
   VkMemoryPropertyFlags flags,
   VkExternalMemoryHandleTypeFlags external_handles,
   const VkMemoryAllocateInfo *alloc_info,
   struct vn_renderer_bo **out_bo)
{
   struct virtgpu *gpu = (struct virtgpu *)renderer;
   const uint32_t blob_flags =
      virtgpu_bo_blob_flags(gpu, flags, external_handles);

   uint32_t res_id;

   HANDLE h = NULL;
   const VkD3DDDICreateResource *d3d_create =
      vk_find_struct_const(alloc_info, D3DDDI_CREATE_RESOURCE);
   if (gpu->ddicb != NULL && d3d_create != NULL) {
      h = d3d_create->hRTResource;
   }

   D3DKMT_HANDLE alloc, kmt_local, kmt_global;
   NTSTATUS status = virtgpu_ioctl_resource_create_blob(
      gpu, gpu->bo_blob_mem, blob_flags, size, mem_id, &res_id, &alloc,
      &kmt_local, &kmt_global, &h);
   if (!NT_SUCCESS(status)) {
      vn_log(gpu->instance,
             "RESOURCE_CREATE_BLOB failed: type=%u, flags=%u, size=%zu, "
             "id=%" PRIu64 ", err=0x%lx",
             gpu->bo_blob_mem, blob_flags, size, mem_id, status);
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   }

   /* There's a single underlying bo mapping shared by the initial alloc here
    * and the later import of the same. The mapping size has to be initialized
    * with the real size of the created blob resource, since the app can query
    * the exported native handle size for re-import. e.g. lseek dma-buf size
    */
   const uint32_t mappable_and_shareable =
      VIOGPU_BLOB_FLAG_USE_MAPPABLE | VIOGPU_BLOB_FLAG_USE_SHAREABLE;
   if ((blob_flags & mappable_and_shareable) == mappable_and_shareable) {
      VIOGPU_ESCAPE res_info = {
         .Type = VIOGPU_RES_INFO,
         .DataLength = sizeof(res_info.ResourceInfo),
         .ResourceInfo = {
            .ResHandle = alloc,
         },
      };

      NTSTATUS status = virtgpu_ioctl_escape(gpu, &res_info);
      if (!NT_SUCCESS(status)) {
         vn_log(gpu->instance, "RESOURCE_INFO failed: handle=%u, err=0x%lx",
                alloc, status);
         virtgpu_ioctl_resource_destroy_blob(gpu, alloc, kmt_local, h);
         return VK_ERROR_INVALID_EXTERNAL_HANDLE;
      }

      assert(res_info.ResourceInfo.IsBlob);
      assert(res_info.ResourceInfo.BlobMem);
      if (res_info.ResourceInfo.Size < size) {
         virtgpu_ioctl_resource_destroy_blob(gpu, alloc, kmt_local, h);
         vn_log(gpu->instance,
                "blob mem create failed: info.size(%llu) < size(%" PRIu64 ")",
                res_info.ResourceInfo.Size, size);
         return VK_ERROR_INVALID_EXTERNAL_HANDLE;
      }

      size = res_info.ResourceInfo.Size;
   }

   struct virtgpu_bo *bo = util_sparse_array_get(&gpu->bo_array, alloc);
   *bo = (struct virtgpu_bo){
      .base = {
         .refcount = VN_REFCOUNT_INIT(1),
         .res_id = res_id,
         .mmap_size = size,
      },
      .alloc = alloc,
      .blob_flags = blob_flags,
   };

   if (gpu->ddicb != NULL) {
      bo->h = h;
   } else {
      bo->kmt.local = kmt_local;
      bo->kmt.global = kmt_global;
   }

   *out_bo = &bo->base;

   return VK_SUCCESS;
}

static void
virtgpu_bo_invalidate(struct vn_renderer *renderer,
                      struct vn_renderer_bo *bo,
                      VkDeviceSize offset,
                      VkDeviceSize size)
{
   /* nop because kernel makes every mapping coherent */
   // TODO: check if this is true
}

static void
virtgpu_bo_flush(struct vn_renderer *renderer,
                 struct vn_renderer_bo *bo,
                 VkDeviceSize offset,
                 VkDeviceSize size)
{
   /* nop because kernel makes every mapping coherent */
   // TODO: check if this is true
}

static void *
virtgpu_bo_map(struct vn_renderer *renderer,
               struct vn_renderer_bo *_bo,
               void *placed_addr)
{
   assert(placed_addr == NULL);
   struct virtgpu *gpu = (struct virtgpu *)renderer;
   struct virtgpu_bo *bo = (struct virtgpu_bo *)_bo;
   const bool mappable = bo->blob_flags & VIOGPU_BLOB_FLAG_USE_MAPPABLE;

   /* not thread-safe but is fine */
   if (!bo->base.mmap_ptr && mappable) {
      NTSTATUS status =
         virtgpu_ioctl_lock(gpu, bo->alloc, &bo->base.mmap_ptr);
      if (!NT_SUCCESS(status)) {
         vn_log(gpu->instance, "failed to map blob resource: 0x%lx", status);
      }
   }

   return bo->base.mmap_ptr;
}

static void *
virtgpu_bo_export_handle(struct vn_renderer *renderer,
                         struct vn_renderer_bo *_bo,
                         bool is_kmt)
{
   struct virtgpu *gpu = (struct virtgpu *)renderer;
   struct virtgpu_bo *bo = (struct virtgpu_bo *)_bo;
   const bool shareable = bo->blob_flags & VIOGPU_BLOB_FLAG_USE_SHAREABLE;

   if (is_kmt && gpu->ddicb != NULL)
      /* Special hack for DXGI DDI */
      return (void *)(uintptr_t)bo->alloc;
   else if (!shareable)
      return NULL;
   else if (is_kmt && gpu->ddicb == NULL)
      return (void *)(uintptr_t)bo->kmt.global;
   else
      return NULL /* TODO */;
}

static bool
virtgpu_bo_destroy(struct vn_renderer *renderer, struct vn_renderer_bo *_bo)
{
   struct virtgpu *gpu = (struct virtgpu *)renderer;
   struct virtgpu_bo *bo = (struct virtgpu_bo *)_bo;

   mtx_lock(&gpu->win32_handle_import_mutex);

   /* Check the refcount again after the import lock is grabbed.  Yes, we use
    * the double-checked locking anti-pattern.
    */
   if (vn_refcount_is_valid(&bo->base.refcount)) {
      mtx_unlock(&gpu->win32_handle_import_mutex);
      return false;
   }

   if (bo->base.mmap_ptr) {
      virtgpu_ioctl_unlock(gpu, bo->alloc);
      virtgpu_ioctl_destroy_map(gpu, bo->alloc);
   }

   /* Set alloc and res to 0 to indicate that the bo is invalid. Must be set
    * before closing the handles. Otherwise the same handles can be reused
    * by another newly created bo and unexpectedly gotten zero'ed out the
    * tracked handles.
    */
   const D3DKMT_HANDLE alloc = bo->alloc, kmt = bo->kmt.local;
   const HANDLE h = bo->h;
   bo->alloc = 0;
   bo->kmt.local = 0;
   bo->h = NULL;
   virtgpu_ioctl_resource_destroy_blob(gpu, alloc, kmt, h);

   mtx_unlock(&gpu->win32_handle_import_mutex);

   return true;
}

static VkResult
virtgpu_sync_write(struct vn_renderer *renderer,
                   struct vn_renderer_sync *_sync,
                   uint64_t val)
{
   struct virtgpu *gpu = (struct virtgpu *)renderer;
   struct virtgpu_sync *sync = (struct virtgpu_sync *)_sync;

   const bool ret = sim_syncobj_signal(gpu, sync->syncobj_handle, val);

   return ret ? VK_SUCCESS : VK_ERROR_OUT_OF_DEVICE_MEMORY;
}

static VkResult
virtgpu_sync_read(struct vn_renderer *renderer,
                  struct vn_renderer_sync *_sync,
                  uint64_t *val)
{
   struct virtgpu *gpu = (struct virtgpu *)renderer;
   struct virtgpu_sync *sync = (struct virtgpu_sync *)_sync;

   const bool ret = sim_syncobj_query(gpu, sync->syncobj_handle, val);

   return ret ? VK_SUCCESS : VK_ERROR_OUT_OF_DEVICE_MEMORY;
}

static VkResult
virtgpu_sync_reset(struct vn_renderer *renderer,
                   struct vn_renderer_sync *_sync,
                   uint64_t initial_val)
{
   struct virtgpu *gpu = (struct virtgpu *)renderer;
   struct virtgpu_sync *sync = (struct virtgpu_sync *)_sync;

   bool ret = sim_syncobj_reset(gpu, sync->syncobj_handle);
   if (!ret) {
      ret = sim_syncobj_signal(gpu, sync->syncobj_handle, initial_val);
   }

   return ret ? VK_SUCCESS : VK_ERROR_OUT_OF_DEVICE_MEMORY;
}

static void *
virtgpu_sync_export_handle(struct vn_renderer *renderer,
                           struct vn_renderer_sync *_sync)
{
   struct virtgpu *gpu = (struct virtgpu *)renderer;
   struct virtgpu_sync *sync = (struct virtgpu_sync *)_sync;

   return sim_syncobj_export(gpu, sync->syncobj_handle);
}

static void
virtgpu_sync_destroy(struct vn_renderer *renderer,
                     struct vn_renderer_sync *_sync)
{
   struct virtgpu *gpu = (struct virtgpu *)renderer;
   struct virtgpu_sync *sync = (struct virtgpu_sync *)_sync;

   sim_syncobj_destroy(gpu, sync->syncobj_handle);

   free(sync);
}

static VkResult
virtgpu_sync_create_from_handle(struct vn_renderer *renderer,
                                void *handle,
                                struct vn_renderer_sync **out_sync)
{
   struct virtgpu *gpu = (struct virtgpu *)renderer;

   uint32_t syncobj_handle = sim_syncobj_create(gpu, false);
   if (!syncobj_handle)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   if (!sim_syncobj_import(gpu, syncobj_handle, handle)) {
      sim_syncobj_destroy(gpu, syncobj_handle);
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   }

   struct virtgpu_sync *sync = calloc(1, sizeof(*sync));
   if (!sync) {
      sim_syncobj_destroy(gpu, syncobj_handle);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   sync->syncobj_handle = syncobj_handle;
   sync->base.sync_id = 0; /* TODO */

   *out_sync = &sync->base;

   return VK_SUCCESS;
}

static VkResult
virtgpu_sync_create(struct vn_renderer *renderer,
                    uint64_t initial_val,
                    uint32_t flags,
                    struct vn_renderer_sync **out_sync)
{
   struct virtgpu *gpu = (struct virtgpu *)renderer;

   /* TODO */
   if (flags & VN_RENDERER_SYNC_SHAREABLE)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;

   /* always false because we don't use binary drm_syncobjs */
   const bool signaled = false;
   const uint32_t syncobj_handle = sim_syncobj_create(gpu, signaled);
   if (!syncobj_handle)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;

   /* add a signaled fence chain with seqno initial_val */
   const bool ret = sim_syncobj_signal(gpu, syncobj_handle, initial_val);
   if (!ret) {
      sim_syncobj_destroy(gpu, syncobj_handle);
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   }

   struct virtgpu_sync *sync = calloc(1, sizeof(*sync));
   if (!sync) {
      sim_syncobj_destroy(gpu, syncobj_handle);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   sync->syncobj_handle = syncobj_handle;
   /* we will have a sync_id when shareable is true and virtio-gpu associates
    * a host sync object with guest drm_syncobj
    */
   sync->base.sync_id = 0;

   *out_sync = &sync->base;

   return VK_SUCCESS;
}

static void
virtgpu_shmem_destroy_now(struct vn_renderer *renderer,
                          struct vn_renderer_shmem *_shmem)
{
   struct virtgpu *gpu = (struct virtgpu *)renderer;
   struct virtgpu_shmem *shmem = (struct virtgpu_shmem *)_shmem;

   virtgpu_ioctl_unlock(gpu, shmem->alloc);
   virtgpu_ioctl_destroy_map(gpu, shmem->alloc);
   virtgpu_ioctl_resource_destroy_blob(gpu, shmem->alloc, shmem->kmt,
                                       shmem->h);
}

static void
virtgpu_shmem_destroy(struct vn_renderer *renderer,
                      struct vn_renderer_shmem *shmem)
{
   struct virtgpu *gpu = (struct virtgpu *)renderer;

   if (vn_renderer_shmem_cache_add(&gpu->shmem_cache, shmem))
      return;

   virtgpu_shmem_destroy_now(&gpu->base, shmem);
}

static struct vn_renderer_shmem *
virtgpu_shmem_create(struct vn_renderer *renderer, size_t size)
{
   struct virtgpu *gpu = (struct virtgpu *)renderer;

   struct vn_renderer_shmem *cached_shmem =
      vn_renderer_shmem_cache_get(&gpu->shmem_cache, size);
   if (cached_shmem) {
      cached_shmem->refcount = VN_REFCOUNT_INIT(1);
      return cached_shmem;
   }

   uint32_t res_id;
   HANDLE h = NULL; /* This is a device allocation */
   D3DKMT_HANDLE alloc, kmt;

   NTSTATUS status = virtgpu_ioctl_resource_create_blob(
      gpu, gpu->shmem_blob_mem, VIOGPU_BLOB_FLAG_USE_MAPPABLE, size, 0,
      &res_id, &alloc, &kmt, NULL, &h);
   if (!NT_SUCCESS(status))
      return NULL;

   void *ptr = NULL;
   status = virtgpu_ioctl_lock(gpu, alloc, &ptr);
   if (!NT_SUCCESS(status)) {
      virtgpu_ioctl_resource_destroy_blob(gpu, alloc, kmt, h);
      vn_log(gpu->instance, "failed to map blob resource: 0x%lx", status);
      return NULL;
   }

   struct virtgpu_shmem *shmem =
      util_sparse_array_get(&gpu->shmem_array, alloc);
   *shmem = (struct virtgpu_shmem){
      .base = {
         .refcount = VN_REFCOUNT_INIT(1),
         .res_id = res_id,
         .mmap_size = size,
         .mmap_ptr = ptr,
      },
      .alloc = alloc,
   };

   if (gpu->ddicb != NULL) {
      shmem->h = h;
   } else {
      shmem->kmt = kmt;
   }

   return &shmem->base;
}

static VkResult
virtgpu_wait(struct vn_renderer *renderer,
             const struct vn_renderer_wait *wait)
{
   struct virtgpu *gpu = (struct virtgpu *)renderer;
   return sim_syncobj_wait(gpu, wait, false);
}

static VkResult
virtgpu_submit(struct vn_renderer *renderer,
               const struct vn_renderer_submit *submit)
{
   struct virtgpu *gpu = (struct virtgpu *)renderer;
   return sim_submit(gpu, submit);
}

static void
virtgpu_init_renderer_info(struct virtgpu *gpu)
{
   struct vn_renderer_info *info = &gpu->base.info;

   info->pci.vendor_id = VIRTGPU_PCI_VENDOR_ID;
   info->pci.device_id = VIRTGPU_PCI_DEVICE_ID;

   info->pci.has_bus_info = true;
   info->pci.props = (VkPhysicalDevicePCIBusInfoPropertiesEXT){
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT,
      .pciDomain = gpu->pci_bus_info.domain,
      .pciBus = gpu->pci_bus_info.bus,
      .pciDevice = gpu->pci_bus_info.dev,
      .pciFunction = gpu->pci_bus_info.func,
   };

   info->has_dma_buf_import = true;
   info->has_external_sync = true;

   info->has_implicit_fencing = false;

   const struct virgl_renderer_capset_venus *capset = &gpu->capset.data;
   info->wire_format_version = capset->wire_format_version;
   info->vk_xml_version = capset->vk_xml_version;
   info->vk_ext_command_serialization_spec_version =
      capset->vk_ext_command_serialization_spec_version;
   info->vk_mesa_venus_protocol_spec_version =
      capset->vk_mesa_venus_protocol_spec_version;
   assert(capset->supports_blob_id_0);

   /* ensure vk_extension_mask is large enough to hold all capset masks */
   STATIC_ASSERT(sizeof(info->vk_extension_mask) >=
                 sizeof(capset->vk_extension_mask1));
   memcpy(info->vk_extension_mask, capset->vk_extension_mask1,
          sizeof(capset->vk_extension_mask1));

   assert(capset->allow_vk_wait_syncs);

   assert(capset->supports_multiple_timelines);
   info->max_timeline_count = gpu->max_timeline_count;

   /* Use guest blob allocations from dedicated heap (Host visible memory) */
   //if (gpu->bo_blob_mem == VIOGPU_BLOB_MEM_HOST3D && capset->use_guest_vram)
   //   info->has_guest_vram = true;
   info->has_guest_vram = false;

   if (gpu->ddicb != NULL) {
      info->id.has_luid = true;
      info->id.node_mask = 1; /* TODO D3D12 interop*/
      memcpy(info->id.luid, &gpu->ddicb->AdapterLuid, VK_LUID_SIZE);
   } else {
      info->id.has_luid = true;
      info->id.node_mask = 1; /* TODO D3D12 interop*/
      static_assert(sizeof(gpu->d3dkmt.luid) == VK_LUID_SIZE);
      memcpy(info->id.luid, &gpu->d3dkmt.luid, VK_LUID_SIZE);
   }
}

static NTSTATUS
virtgpu_ioctl_destroy_context(struct virtgpu *gpu)
{
   if (gpu->ddicb != NULL) {
      D3DDDICB_DESTROYCONTEXT destroy = {
         .hContext = gpu->ctx.h,
      };
      return hr_to_nt(gpu, gpu->ddicb->pKTCallbacks->pfnDestroyContextCb(
                              gpu->ddicb->hRTDevice, &destroy));
   } else {
      D3DKMT_DESTROYCONTEXT destroy = {
         .hContext = gpu->ctx.kmt,
      };
      return gpu->d3dkmt.cb.destroyContext(&destroy);
   }
}

static NTSTATUS
virtgpu_ioctl_destroy_device(struct virtgpu *gpu)
{
   D3DKMT_DESTROYDEVICE destroy = {
      .hDevice = gpu->d3dkmt.device,
   };
   return gpu->d3dkmt.cb.destroyDevice(&destroy);
}

static NTSTATUS
virtgpu_ioctl_close_adapter(struct virtgpu *gpu)
{
   D3DKMT_CLOSEADAPTER close = {
      .hAdapter = gpu->d3dkmt.adapter,
   };
   return gpu->d3dkmt.cb.closeAdapter(&close);
}

static void
virtgpu_destroy(struct vn_renderer *renderer,
                const VkAllocationCallbacks *alloc)
{
   struct virtgpu *gpu = (struct virtgpu *)renderer;

   vn_renderer_shmem_cache_fini(&gpu->shmem_cache);

   if (gpu->ctx.h)
      virtgpu_ioctl_destroy_context(gpu);
   if (gpu->d3dkmt.device)
      virtgpu_ioctl_destroy_device(gpu);
   if (gpu->d3dkmt.adapter)
      virtgpu_ioctl_close_adapter(gpu);
   if (gpu->d3dkmt.lib)
      FreeLibrary(gpu->d3dkmt.lib);

   mtx_destroy(&gpu->win32_handle_import_mutex);
   mtx_destroy(&gpu->ctx.lock);

   util_sparse_array_finish(&gpu->shmem_array);
   util_sparse_array_finish(&gpu->bo_array);

   vk_free(alloc, gpu);
}

static VkResult
virtgpu_init(struct virtgpu *gpu, void *info)
{
   util_sparse_array_init(&gpu->syncobj_array, sizeof(struct virtgpu_sync),
                          1024);

   util_sparse_array_init(&gpu->shmem_array, sizeof(struct virtgpu_shmem),
                          1024);
   util_sparse_array_init(&gpu->bo_array, sizeof(struct virtgpu_bo), 1024);

   mtx_init(&gpu->win32_handle_import_mutex, mtx_plain);

   mtx_init(&gpu->ctx.lock, mtx_plain);

   VkResult result = virtgpu_open(gpu, info);
   if (result == VK_SUCCESS)
      result = virtgpu_init_params(gpu);
   if (result == VK_SUCCESS)
      result = virtgpu_init_capset(gpu);
   if (result == VK_SUCCESS)
      result = virtgpu_init_context(gpu);
   if (result != VK_SUCCESS)
      return result;

   virtgpu_init_shmem_blob_mem(gpu);

   vn_renderer_shmem_cache_init(&gpu->shmem_cache, &gpu->base,
                                virtgpu_shmem_destroy_now);

   virtgpu_init_renderer_info(gpu);

   gpu->base.ops.destroy = virtgpu_destroy;
   gpu->base.ops.submit = virtgpu_submit;
   gpu->base.ops.wait = virtgpu_wait;

   gpu->base.shmem_ops.create = virtgpu_shmem_create;
   gpu->base.shmem_ops.destroy = virtgpu_shmem_destroy;

   gpu->base.bo_ops.create_from_device_memory =
      virtgpu_bo_create_from_device_memory;
   gpu->base.bo_ops.destroy = virtgpu_bo_destroy;
   gpu->base.bo_ops.create_from_handle = virtgpu_bo_create_from_handle;
   gpu->base.bo_ops.export_handle = virtgpu_bo_export_handle;
   gpu->base.bo_ops.map = virtgpu_bo_map;
   gpu->base.bo_ops.flush = virtgpu_bo_flush;
   gpu->base.bo_ops.invalidate = virtgpu_bo_invalidate;

   gpu->base.sync_ops.create = virtgpu_sync_create;
   gpu->base.sync_ops.create_from_handle = virtgpu_sync_create_from_handle;
   gpu->base.sync_ops.destroy = virtgpu_sync_destroy;
   gpu->base.sync_ops.export_handle = virtgpu_sync_export_handle;
   gpu->base.sync_ops.reset = virtgpu_sync_reset;
   gpu->base.sync_ops.read = virtgpu_sync_read;
   gpu->base.sync_ops.write = virtgpu_sync_write;
   return VK_SUCCESS;
}

static void
sim_init_mutex(void)
{
   mtx_init(&sim.mutex, mtx_plain);
}

VkResult
vn_renderer_create_virtgpu_win32(struct vn_instance *instance,
                                 const VkAllocationCallbacks *alloc,
                                 const VkInstanceCreateInfo *pCreateInfo,
                                 struct vn_renderer **renderer)
{
   struct virtgpu *gpu = vk_zalloc(alloc, sizeof(*gpu), VN_DEFAULT_ALIGN,
                                   VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!gpu)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   call_once(&sim.init, sim_init_mutex);

   gpu->instance = instance;

   VkResult result = virtgpu_init(gpu, (void *) pCreateInfo->pNext);
   if (result != VK_SUCCESS) {
      virtgpu_destroy(&gpu->base, alloc);
      return result;
   }

   *renderer = &gpu->base;

   return VK_SUCCESS;
}
