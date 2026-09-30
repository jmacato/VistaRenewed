/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Native Win32 virtgpu transport for Neptune.  Talks to viogpu3d.sys
 * over D3DKMT (no Wine, no unixlib).  Mirrors the D3DKMT plumbing the
 * sister Venus driver uses against the same KMD, adapted to the
 * Neptune renderer surface.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

/* WIN32_NO_STATUS keeps windows.h from defining a subset of STATUS_*
 * codes; we get the full set from ntstatus.h below. */
#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS

#include <limits.h>
#include <winnt.h>
#include <ntstatus.h>
#include <winternl.h>

#include <d3dkmthk.h>
#include <d3dukmdt.h>

#include "virtio/virtio-gpu/wddm_hw.h"

#include "neptune-protocol/npt_protocol_defs.h"
#include "npt_renderer.h"

#if defined(NPT_D3D9_RUNTIME_DDI) || defined(NPT_D3D10_RUNTIME_DDI)
/* The generated Neptune protocol header has already supplied the public
 * D3DCOLORVALUE layout.  Keep MinGW's d3d9types.h from declaring its private
 * _D3DCOLORVALUE version before d3dumddi.h consumes the D3D9 API types. */
#ifndef D3DCOLORVALUE_DEFINED
#define D3DCOLORVALUE_DEFINED
#endif
#include <d3d9types.h>
#include <d3dumddi.h>
#include "npt_runtime_binding.h"
#endif

// Do not use INFINITE here.  Vista has no D3DKMT CPU-event enqueue API, so
// this private marker is also the point at which a lost/reset device must
// become observable to the UMD rather than hanging DWM forever.
#define VIRTGPU_RENDER_EVENT_TIMEOUT_MS 15000

/* Vista predates the VULKAN client hint and can reject that enum value in
 * D3DKMTCreateContext before the miniport callback runs.  The transport is a
 * private 3D command stream, so the DX9 hint is the closest valid WDDM 1.0
 * classification.  Keep the modern hint for normal Triton builds. */
#if defined(_WIN32_WINNT) && _WIN32_WINNT <= 0x0600
#define NPT_D3DKMT_CLIENT_HINT D3DKMT_CLIENTHINT_DX9
#else
#define NPT_D3DKMT_CLIENT_HINT D3DKMT_CLIENTHINT_VULKAN
#endif

struct npt_virtgpu_shmem {
   struct npt_renderer_shmem base;
   D3DKMT_HANDLE alloc;
   D3DKMT_HANDLE res_kmt;
};

struct npt_virtgpu {
   struct npt_renderer base;

   /* Single global mutex guards the kernel-shared command buffer in
    * gpu->ctx; the kernel rotates pNew* pointers on every D3DKMTRender
    * so concurrent submits would race on cmd_buf / alloc_list. */
   CRITICAL_SECTION cs;

   HINSTANCE gdi32;
   struct {
      PFND3DKMT_QUERYADAPTERINFO queryAdapterInfo;
      PFND3DKMT_ESCAPE escape;
      PFND3DKMT_RENDER render;
      PFND3DKMT_CREATECONTEXT createContext;
      PFND3DKMT_DESTROYCONTEXT destroyContext;
      PFND3DKMT_CREATEALLOCATION createAllocation;
      PFND3DKMT_DESTROYALLOCATION destroyAllocation;
      PFND3DKMT_LOCK lock;
      PFND3DKMT_UNLOCK unlock;
      PFND3DKMT_CREATEDEVICE createDevice;
      PFND3DKMT_DESTROYDEVICE destroyDevice;
      PFND3DKMT_GETDEVICESTATE getDeviceState;
      PFND3DKMT_OPENADAPTERFROMHDC openAdapterFromHdc;
      PFND3DKMT_CLOSEADAPTER closeAdapter;
   } cb;

   D3DKMT_HANDLE adapter;
   D3DKMT_HANDLE device;
   D3DKMT_HANDLE context;
   LUID luid;

#if defined(NPT_D3D9_RUNTIME_DDI) || defined(NPT_D3D10_RUNTIME_DDI)
   /* A D3D UMD already owns a runtime/KMD device.  Reopening the adapter and
    * calling D3DKMTCreateDevice from inside its CreateDevice callback
    * deadlocks Vista's graphics runtime.  Use the callbacks that belong to
    * that outer device instead. */
   bool use_runtime_ddi;
   HANDLE h_rt_adapter;
   HANDLE h_rt_device;
   HANDLE h_ddi_context;
   D3DDDI_ADAPTERCALLBACKS adapter_callbacks;
   D3DDDI_DEVICECALLBACKS device_callbacks;
#endif

   /* Kernel-mapped command buffer + alloc/patch lists, returned by
    * D3DKMTCreateContext and rotated by D3DKMTRender. */
   void *cmd_buf;
   UINT cmd_size;
   D3DDDI_ALLOCATIONLIST *alloc_list;
   UINT alloc_size;
   D3DDDI_PATCHLOCATIONLIST *patch_list;
   UINT patch_size;
};

#if defined(NPT_D3D9_RUNTIME_DDI) || defined(NPT_D3D10_RUNTIME_DDI)
struct npt_d3d9_runtime_binding {
   bool valid;
   HANDLE h_rt_adapter;
   HANDLE h_rt_device;
   UINT interface_version;
   UINT runtime_version;
   D3DDDI_ADAPTERCALLBACKS adapter_callbacks;
   D3DDDI_DEVICECALLBACKS device_callbacks;
};

/* D3D9 serializes device creation.  triton9CreateDevice publishes this
 * binding immediately before the synchronous Neptune-device acquire that
 * constructs the renderer.  Copy the callback tables: the argument tables
 * themselves are runtime-owned and need not remain at the same address. */
static struct npt_d3d9_runtime_binding g_d3d9_runtime_binding;

void npt_renderer_bind_d3d9_runtime(
   HANDLE h_rt_adapter, HANDLE h_rt_device,
   UINT interface_version, UINT runtime_version,
   const D3DDDI_ADAPTERCALLBACKS *adapter_callbacks,
   const D3DDDI_DEVICECALLBACKS *device_callbacks);

enum npt_d3d9_runtime_callback_bits {
   NPT_D3D9_CB_QUERY_ADAPTER_INFO = 1u << 0,
   NPT_D3D9_CB_ESCAPE             = 1u << 1,
   NPT_D3D9_CB_RENDER             = 1u << 2,
   NPT_D3D9_CB_CREATE_CONTEXT     = 1u << 3,
   NPT_D3D9_CB_DESTROY_CONTEXT    = 1u << 4,
   NPT_D3D9_CB_ALLOCATE           = 1u << 5,
   NPT_D3D9_CB_DEALLOCATE         = 1u << 6,
   NPT_D3D9_CB_LOCK               = 1u << 7,
   NPT_D3D9_CB_UNLOCK             = 1u << 8,
};

#define NPT_D3D9_REQUIRED_CALLBACKS \
   (NPT_D3D9_CB_QUERY_ADAPTER_INFO | NPT_D3D9_CB_ESCAPE | \
    NPT_D3D9_CB_RENDER | NPT_D3D9_CB_CREATE_CONTEXT | \
    NPT_D3D9_CB_DESTROY_CONTEXT | NPT_D3D9_CB_ALLOCATE | \
    NPT_D3D9_CB_DEALLOCATE | NPT_D3D9_CB_LOCK | NPT_D3D9_CB_UNLOCK)

static UINT
npt_d3d9_runtime_callback_mask(
   const D3DDDI_ADAPTERCALLBACKS *adapter_callbacks,
   const D3DDDI_DEVICECALLBACKS *device_callbacks)
{
   UINT mask = 0;

   if (!adapter_callbacks || !device_callbacks)
      return mask;
   if (adapter_callbacks->pfnQueryAdapterInfoCb)
      mask |= NPT_D3D9_CB_QUERY_ADAPTER_INFO;
   if (device_callbacks->pfnEscapeCb)
      mask |= NPT_D3D9_CB_ESCAPE;
   if (device_callbacks->pfnRenderCb)
      mask |= NPT_D3D9_CB_RENDER;
   if (device_callbacks->pfnCreateContextCb)
      mask |= NPT_D3D9_CB_CREATE_CONTEXT;
   if (device_callbacks->pfnDestroyContextCb)
      mask |= NPT_D3D9_CB_DESTROY_CONTEXT;
   if (device_callbacks->pfnAllocateCb)
      mask |= NPT_D3D9_CB_ALLOCATE;
   if (device_callbacks->pfnDeallocateCb)
      mask |= NPT_D3D9_CB_DEALLOCATE;
   if (device_callbacks->pfnLockCb)
      mask |= NPT_D3D9_CB_LOCK;
   if (device_callbacks->pfnUnlockCb)
      mask |= NPT_D3D9_CB_UNLOCK;
   return mask;
}

void
npt_renderer_bind_runtime(HANDLE h_rt_adapter, HANDLE h_rt_device,
                               UINT interface_version, UINT runtime_version,
                               const D3DDDI_ADAPTERCALLBACKS *adapter_callbacks,
                               const D3DDDI_DEVICECALLBACKS *device_callbacks)
{
   memset(&g_d3d9_runtime_binding, 0, sizeof(g_d3d9_runtime_binding));
   if (!h_rt_adapter || !h_rt_device || !adapter_callbacks ||
       !device_callbacks)
      return;

   g_d3d9_runtime_binding.h_rt_adapter = h_rt_adapter;
   g_d3d9_runtime_binding.h_rt_device = h_rt_device;
   g_d3d9_runtime_binding.interface_version = interface_version;
   g_d3d9_runtime_binding.runtime_version = runtime_version;
   g_d3d9_runtime_binding.adapter_callbacks = *adapter_callbacks;
   g_d3d9_runtime_binding.device_callbacks = *device_callbacks;
   g_d3d9_runtime_binding.valid = true;

   npt_log("virtgpu: D3D9 runtime binding interface=0x%x version=0x%x callbacks=0x%03x",
           interface_version, runtime_version,
           npt_d3d9_runtime_callback_mask(adapter_callbacks, device_callbacks));
}

/* Preserve the existing D3D9 entry point and callback ABI. */
void
npt_renderer_bind_d3d9_runtime(HANDLE adapter, HANDLE device,
    UINT interface_version, UINT runtime_version,
    const D3DDDI_ADAPTERCALLBACKS *adapter_callbacks,
    const D3DDDI_DEVICECALLBACKS *device_callbacks)
{
   npt_renderer_bind_runtime(adapter, device, interface_version, runtime_version,
                            adapter_callbacks, device_callbacks);
}

static NTSTATUS
virtgpu_status_from_hresult(HRESULT hr)
{
   /* HRESULT and NTSTATUS are both signed 32-bit status values.  Preserve the
    * exact runtime callback failure so callers and logs do not collapse device
    * removal, invalid input, and allocation failure into one generic code. */
   return SUCCEEDED(hr) ? STATUS_SUCCESS : (NTSTATUS)hr;
}
#endif

static bool
virtgpu_has_context(const struct npt_virtgpu *gpu)
{
   if (!gpu)
      return false;
#if defined(NPT_D3D9_RUNTIME_DDI) || defined(NPT_D3D10_RUNTIME_DDI)
   if (gpu->use_runtime_ddi)
      return gpu->h_ddi_context != NULL;
#endif
   return gpu->context != 0;
}

static NTSTATUS
virtgpu_send_cmd_locked(struct npt_virtgpu *gpu, UINT type, UINT flags,
                        UINT ring_idx, const void *payload, size_t payload_size,
                        UINT alloc_count);
static NTSTATUS
virtgpu_unlock(struct npt_virtgpu *gpu, D3DKMT_HANDLE alloc);

static bool
npt_align64_checked(uint64_t value, uint64_t alignment, uint64_t *out)
{
   if (!out || !alignment || (alignment & (alignment - 1)) ||
       value > UINT64_MAX - (alignment - 1))
      return false;

   *out = (value + alignment - 1) & ~(alignment - 1);
   return true;
}

static NTSTATUS
virtgpu_render(struct npt_virtgpu *gpu, UINT cmd_offset, UINT cmd_length,
               UINT alloc_count)
{
   if (!gpu || !virtgpu_has_context(gpu) || !gpu->cmd_buf ||
       cmd_offset > gpu->cmd_size || cmd_length > gpu->cmd_size - cmd_offset ||
       alloc_count > gpu->alloc_size || alloc_count > gpu->patch_size ||
       (alloc_count && (!gpu->alloc_list || !gpu->patch_list)))
      return STATUS_INVALID_PARAMETER;

   NTSTATUS status;
   void *new_cmd_buf;
   UINT new_cmd_size;
   D3DDDI_ALLOCATIONLIST *new_alloc_list;
   UINT new_alloc_size;
   D3DDDI_PATCHLOCATIONLIST *new_patch_list;
   UINT new_patch_size;

#if defined(NPT_D3D9_RUNTIME_DDI) || defined(NPT_D3D10_RUNTIME_DDI)
   if (gpu->use_runtime_ddi) {
      if (!gpu->device_callbacks.pfnRenderCb)
         return STATUS_INVALID_PARAMETER;
      D3DDDICB_RENDER render = {
         .CommandLength = cmd_length,
         .CommandOffset = cmd_offset,
         .NumAllocations = alloc_count,
         .NumPatchLocations = alloc_count,
         .hContext = gpu->h_ddi_context,
      };
      status = virtgpu_status_from_hresult(
         gpu->device_callbacks.pfnRenderCb(gpu->h_rt_device, &render));
      new_cmd_buf = render.pNewCommandBuffer;
      new_cmd_size = render.NewCommandBufferSize;
      new_alloc_list = render.pNewAllocationList;
      new_alloc_size = render.NewAllocationListSize;
      new_patch_list = render.pNewPatchLocationList;
      new_patch_size = render.NewPatchLocationListSize;
   } else
#endif
   {
      if (!gpu->cb.render || !gpu->context)
         return STATUS_INVALID_PARAMETER;
      D3DKMT_RENDER render = {
         .hContext = gpu->context,
         .CommandOffset = cmd_offset,
         .CommandLength = cmd_length,
         .AllocationCount = alloc_count,
         .PatchLocationCount = alloc_count,
      };
      status = gpu->cb.render(&render);
      new_cmd_buf = render.pNewCommandBuffer;
      new_cmd_size = render.NewCommandBufferSize;
      new_alloc_list = render.pNewAllocationList;
      new_alloc_size = render.NewAllocationListSize;
      new_patch_list = render.pNewPatchLocationList;
      new_patch_size = render.NewPatchLocationListSize;
   }
   if (!NT_SUCCESS(status))
      return status;

   /* The kernel may rotate the buffers each call; pick up the new pointers
    * so the next render targets the current backing. */
   if (!new_cmd_buf || new_cmd_size < sizeof(VIOGPU_COMMAND_HDR) ||
       !new_alloc_list || !new_alloc_size ||
       !new_patch_list || !new_patch_size) {
      npt_log("virtgpu: render returned incomplete replacement buffers");
      gpu->cmd_buf = NULL;
      gpu->cmd_size = 0;
      gpu->alloc_list = NULL;
      gpu->alloc_size = 0;
      gpu->patch_list = NULL;
      gpu->patch_size = 0;
      return STATUS_INVALID_PARAMETER;
   }

   gpu->cmd_buf = new_cmd_buf;
   gpu->cmd_size = new_cmd_size;
   gpu->alloc_list = new_alloc_list;
   gpu->alloc_size = new_alloc_size;
   gpu->patch_list = new_patch_list;
   gpu->patch_size = new_patch_size;

   return status;
}

/* Submits a terminal private event packet on the current D3DKMT render
 * stream.  viogpu3d references the handle in DxgkDdiRender, then signals its
 * referenced PKEVENT after preceding renderer commands retire. This does not
 * replace a GPU fence for work enqueued asynchronously by host APIs. */
static NTSTATUS
virtgpu_submit_render_event_locked(struct npt_virtgpu *gpu, HANDLE event)
{
   VIOGPU_SIGNAL_EVENT_CMD cmd = { .Event = VioGpuUmHandle(event) };
   return virtgpu_send_cmd_locked(gpu, VIOGPU_CMD_SIGNAL_EVENT, 0, 0,
                                  &cmd, sizeof(cmd), 0);
}

/* Drain pending work without D3DKMTSignalSynchronizationObject2, which is
 * unavailable on Vista.  A finite timeout turns reset/removal into a normal
 * renderer failure instead of an unbounded compositor stall. */
static bool
virtgpu_drain(struct npt_virtgpu *gpu)
{
   if (!virtgpu_has_context(gpu))
      return false;

   HANDLE event = CreateEventA(NULL, FALSE, FALSE, NULL);
   if (!event)
      return false;

   EnterCriticalSection(&gpu->cs);
   NTSTATUS status = virtgpu_submit_render_event_locked(gpu, event);
   LeaveCriticalSection(&gpu->cs);
   if (!NT_SUCCESS(status)) {
      npt_log("virtgpu drain: render-event submit failed 0x%lx", status);
      CloseHandle(event);
      return false;
   }
   DWORD wr = WaitForSingleObject(event, VIRTGPU_RENDER_EVENT_TIMEOUT_MS);
   CloseHandle(event);
   if (wr != WAIT_OBJECT_0) {
      npt_log("virtgpu drain: render-event wait returned %lu (gle=%lu)",
              wr, wr == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS);
      return false;
   }

   /* ResetFromTimeout and device removal must wake the event so teardown does
    * not strand DWM. A wake alone is therefore not proof of successful GPU
    * retirement. D3DKMTGetDeviceState is present on Vista and distinguishes a
    * normal completion from RESET/HUNG/STOPPED without changing the private
    * event packet ABI. */
#if defined(NPT_D3D9_RUNTIME_DDI) || defined(NPT_D3D10_RUNTIME_DDI)
   /* Vista has no device-state query in D3DDDI_DEVICECALLBACKS.  Reset and
    * removal wake the private event, then the outer D3D9 runtime propagates
    * the device-lost result on the next callback. */
   if (gpu->use_runtime_ddi)
      return true;
#endif
   D3DKMT_GETDEVICESTATE device_state = {
      .hDevice = gpu->device,
      .StateType = D3DKMT_DEVICESTATE_EXECUTION,
   };
   NTSTATUS state_status = gpu->cb.getDeviceState(&device_state);
   if (!NT_SUCCESS(state_status) ||
       device_state.ExecutionState != D3DKMT_DEVICEEXECUTION_ACTIVE) {
      npt_log("virtgpu drain: device state query returned 0x%lx state=%u",
              state_status, (unsigned)device_state.ExecutionState);
      return false;
   }
   return true;
}

static NTSTATUS
virtgpu_escape(struct npt_virtgpu *gpu, VIOGPU_ESCAPE *priv)
{
   if (!gpu || !priv)
      return STATUS_INVALID_PARAMETER;

#if defined(NPT_D3D9_RUNTIME_DDI) || defined(NPT_D3D10_RUNTIME_DDI)
   if (gpu->use_runtime_ddi) {
      if (!gpu->device_callbacks.pfnEscapeCb || !gpu->h_rt_adapter ||
          !gpu->h_rt_device)
         return STATUS_INVALID_PARAMETER;
      D3DDDICB_ESCAPE escape = {
         .hDevice = gpu->h_rt_device,
         .pPrivateDriverData = priv,
         .PrivateDriverDataSize = sizeof(*priv),
         .hContext = gpu->h_ddi_context,
      };
      return virtgpu_status_from_hresult(
         gpu->device_callbacks.pfnEscapeCb(gpu->h_rt_adapter, &escape));
   }
#endif
   if (!gpu->cb.escape || !gpu->adapter || !gpu->device)
      return STATUS_INVALID_PARAMETER;

   D3DKMT_ESCAPE escape = {
      .hAdapter = gpu->adapter,
      .hDevice = gpu->device,
      .pPrivateDriverData = priv,
      .PrivateDriverDataSize = sizeof(*priv),
   };
   return gpu->cb.escape(&escape);
}

static NTSTATUS
virtgpu_get_caps(struct npt_virtgpu *gpu, uint32_t id, uint32_t version,
                 void *capset, uint32_t capset_size)
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
   return virtgpu_escape(gpu, &caps);
}

static NTSTATUS
virtgpu_query_adapter_info(struct npt_virtgpu *gpu, void *priv,
                           UINT priv_size)
{
   if (!gpu || !priv || !priv_size)
      return STATUS_INVALID_PARAMETER;

#if defined(NPT_D3D9_RUNTIME_DDI) || defined(NPT_D3D10_RUNTIME_DDI)
   if (gpu->use_runtime_ddi) {
      if (!gpu->adapter_callbacks.pfnQueryAdapterInfoCb || !gpu->h_rt_adapter)
         return STATUS_INVALID_PARAMETER;
      D3DDDICB_QUERYADAPTERINFO query = {
         .pPrivateDriverData = priv,
         .PrivateDriverDataSize = priv_size,
      };
      return virtgpu_status_from_hresult(
         gpu->adapter_callbacks.pfnQueryAdapterInfoCb(gpu->h_rt_adapter,
                                                       &query));
   }
#endif
   if (!gpu->cb.queryAdapterInfo || !gpu->adapter)
      return STATUS_INVALID_PARAMETER;

   D3DKMT_QUERYADAPTERINFO query = {
      .hAdapter = gpu->adapter,
      .Type = KMTQAITYPE_UMDRIVERPRIVATE,
      .pPrivateDriverData = priv,
      .PrivateDriverDataSize = priv_size,
   };
   return gpu->cb.queryAdapterInfo(&query);
}

/* RAII-style: writes hdr + payload into the kernel command buffer
 * under gpu->cs, calls D3DKMTRender, and returns its status. */
static NTSTATUS
virtgpu_send_cmd_locked(struct npt_virtgpu *gpu, UINT type, UINT flags,
                        UINT ring_idx, const void *payload, size_t payload_size,
                        UINT alloc_count)
{
   if (!gpu || !virtgpu_has_context(gpu) || !gpu->cmd_buf ||
       gpu->cmd_size < sizeof(VIOGPU_COMMAND_HDR) ||
       payload_size > UINT_MAX ||
       payload_size > SIZE_MAX - sizeof(VIOGPU_COMMAND_HDR) ||
       (payload_size && !payload) ||
       alloc_count > gpu->alloc_size || alloc_count > gpu->patch_size ||
       (alloc_count && (!gpu->alloc_list || !gpu->patch_list)))
      return STATUS_INVALID_PARAMETER;

   const size_t total = sizeof(VIOGPU_COMMAND_HDR) + payload_size;
   if (total > UINT_MAX || total > gpu->cmd_size) {
      npt_log("virtgpu: cmd %u too large for kernel buffer (%zu > %u)",
              type, total, gpu->cmd_size);
      return STATUS_BUFFER_TOO_SMALL;
   }

   VIOGPU_COMMAND_HDR *hdr = gpu->cmd_buf;
   hdr->type = type;
   hdr->size = (UINT)payload_size;
   hdr->flags = flags;
   hdr->ring_idx = ring_idx;

   if (payload_size)
      memcpy(hdr + 1, payload, payload_size);

   return virtgpu_render(gpu, 0, (UINT)total, alloc_count);
}

/* MAP_BLOB / UNMAP_BLOB take a single allocation reference in slot 0,
 * indexed via a single-ULONG payload. */
static NTSTATUS
virtgpu_map_blob_op(struct npt_virtgpu *gpu, D3DKMT_HANDLE alloc, UINT type)
{
   if (!gpu || !alloc ||
       (type != VIOGPU_CMD_MAP_BLOB && type != VIOGPU_CMD_UNMAP_BLOB))
      return STATUS_INVALID_PARAMETER;

   EnterCriticalSection(&gpu->cs);
   if (!gpu->alloc_list || !gpu->patch_list ||
       gpu->alloc_size < 1 || gpu->patch_size < 1) {
      LeaveCriticalSection(&gpu->cs);
      return STATUS_INVALID_PARAMETER;
   }

   memset(&gpu->alloc_list[0], 0, sizeof(gpu->alloc_list[0]));
   memset(&gpu->patch_list[0], 0, sizeof(gpu->patch_list[0]));
   gpu->alloc_list[0].hAllocation = alloc;
   gpu->patch_list[0].AllocationIndex = 0;

   const ULONG slot = 0;
   NTSTATUS status =
      virtgpu_send_cmd_locked(gpu, type, 0, 0, &slot, sizeof(slot), 1);
   LeaveCriticalSection(&gpu->cs);

   if (!NT_SUCCESS(status))
      return status;
   return virtgpu_drain(gpu) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

static NTSTATUS
virtgpu_lock(struct npt_virtgpu *gpu, D3DKMT_HANDLE alloc, void **out_ptr)
{
   if (!gpu || !alloc || !out_ptr)
      return STATUS_INVALID_PARAMETER;
   *out_ptr = NULL;

#if defined(NPT_D3D9_RUNTIME_DDI) || defined(NPT_D3D10_RUNTIME_DDI)
   if (gpu->use_runtime_ddi) {
      if (!gpu->device_callbacks.pfnLockCb || !gpu->h_rt_device)
         return STATUS_INVALID_PARAMETER;
      D3DDDICB_LOCK lock = {
         .hAllocation = alloc,
         .Flags = { .LockEntire = 1 },
      };
      NTSTATUS status = virtgpu_status_from_hresult(
         gpu->device_callbacks.pfnLockCb(gpu->h_rt_device, &lock));
      if (!NT_SUCCESS(status))
         return status;
      if (!lock.pData) {
         virtgpu_unlock(gpu, alloc);
         return STATUS_INVALID_PARAMETER;
      }
      *out_ptr = lock.pData;
      return STATUS_SUCCESS;
   }
#endif
   if (!gpu->cb.lock || !gpu->device)
      return STATUS_INVALID_PARAMETER;

   D3DKMT_LOCK lock = {
      .hDevice = gpu->device,
      .hAllocation = alloc,
      .Flags = { .LockEntire = 1 },
   };
   NTSTATUS status = gpu->cb.lock(&lock);
   if (!NT_SUCCESS(status))
      return status;
   if (!lock.pData) {
      virtgpu_unlock(gpu, alloc);
      return STATUS_INVALID_PARAMETER;
   }
   *out_ptr = lock.pData;
   return STATUS_SUCCESS;
}

static NTSTATUS
virtgpu_unlock(struct npt_virtgpu *gpu, D3DKMT_HANDLE alloc)
{
   if (!gpu || !alloc)
      return STATUS_INVALID_PARAMETER;

#if defined(NPT_D3D9_RUNTIME_DDI) || defined(NPT_D3D10_RUNTIME_DDI)
   if (gpu->use_runtime_ddi) {
      if (!gpu->device_callbacks.pfnUnlockCb || !gpu->h_rt_device)
         return STATUS_INVALID_PARAMETER;
      D3DDDICB_UNLOCK unlock = {
         .NumAllocations = 1,
         .phAllocations = &alloc,
      };
      return virtgpu_status_from_hresult(
         gpu->device_callbacks.pfnUnlockCb(gpu->h_rt_device, &unlock));
   }
#endif
   if (!gpu->cb.unlock || !gpu->device)
      return STATUS_INVALID_PARAMETER;

   D3DKMT_UNLOCK unlock = {
      .hDevice = gpu->device,
      .NumAllocations = 1,
      .phAllocations = &alloc,
   };
   return gpu->cb.unlock(&unlock);
}

static NTSTATUS
virtgpu_resource_destroy_blob(struct npt_virtgpu *gpu,
                             D3DKMT_HANDLE alloc, D3DKMT_HANDLE res_kmt);
bool npt_vgw32_release_import_res_checked(struct npt_renderer *renderer,
                                          uint32_t alloc, uint32_t res_kmt);

static NTSTATUS
virtgpu_create_allocation(struct npt_virtgpu *gpu,
                          VIOGPU_CREATE_RESOURCE_EXCHANGE *res_priv,
                          D3DDDI_ALLOCATIONINFO *alloc_info,
                          bool is_shareable,
                          D3DKMT_HANDLE *out_res_kmt)
{
   if (!gpu || !res_priv || !alloc_info || !out_res_kmt)
      return STATUS_INVALID_PARAMETER;

#if defined(NPT_D3D9_RUNTIME_DDI) || defined(NPT_D3D10_RUNTIME_DDI)
   if (gpu->use_runtime_ddi) {
      if (!gpu->device_callbacks.pfnAllocateCb || !gpu->h_rt_device)
         return STATUS_INVALID_PARAMETER;
      /* These are Neptune transport allocations, not D3D9 API resources.
       * A NULL runtime-resource handle asks dxgkrnl to create a driver-private
       * resource on the already-open D3D9 device. */
      D3DDDICB_ALLOCATE alloc = {
         .pPrivateDriverData = res_priv,
         .PrivateDriverDataSize = sizeof(*res_priv),
         .hResource = NULL,
         .NumAllocations = 1,
         .pAllocationInfo = alloc_info,
      };
      NTSTATUS status = virtgpu_status_from_hresult(
         gpu->device_callbacks.pfnAllocateCb(gpu->h_rt_device, &alloc));
      if (NT_SUCCESS(status))
         *out_res_kmt = alloc.hKMResource;
      return status;
   }
#endif

   if (!gpu->cb.createAllocation || !gpu->device)
      return STATUS_INVALID_PARAMETER;
   D3DKMT_CREATEALLOCATION alloc = {
      .hDevice = gpu->device,
      .pPrivateDriverData = res_priv,
      .PrivateDriverDataSize = sizeof(*res_priv),
      .NumAllocations = 1,
      .pAllocationInfo = alloc_info,
      .Flags = {
         .CreateResource = 1,
         .CreateShared = is_shareable,
      },
   };
   NTSTATUS status = gpu->cb.createAllocation(&alloc);
   if (NT_SUCCESS(status))
      *out_res_kmt = alloc.hResource;
   return status;
}

/* Create a virtio blob via D3DKMTCreateAllocation, look up the host
 * res_id via the RES_INFO escape, and (for mappable blobs) issue the
 * MAP_BLOB ring command so the kernel maps the host backing into the
 * guest address space.  Lock follows separately.  Any failure after the
 * allocation is created destroys it before returning. */
static NTSTATUS
virtgpu_resource_create_blob(struct npt_virtgpu *gpu, uint32_t blob_mem,
                             uint32_t blob_flags, size_t blob_size,
                             uint64_t blob_id, uint32_t *out_res_id,
                             D3DKMT_HANDLE *out_alloc,
                             D3DKMT_HANDLE *out_res_kmt)
{
   if (!gpu || !out_res_id || !out_alloc || !out_res_kmt)
      return STATUS_INVALID_PARAMETER;

   *out_res_id = 0;
   *out_alloc = 0;
   *out_res_kmt = 0;

   uint64_t aligned_size = 0;
   if (!blob_size ||
       !npt_align64_checked((uint64_t)blob_size, 4096, &aligned_size) ||
       aligned_size > (uint64_t)SIZE_MAX)
      return STATUS_INVALID_PARAMETER;
   blob_size = (size_t)aligned_size;

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

   const bool is_shareable = !!(blob_flags & VIOGPU_BLOB_FLAG_USE_SHAREABLE);
   const bool is_mappable = !!(blob_flags & VIOGPU_BLOB_FLAG_USE_MAPPABLE);

   if (!virtgpu_drain(gpu))
      return STATUS_UNSUCCESSFUL;

   NTSTATUS status = virtgpu_create_allocation(gpu, &res_priv, &alloc_info,
                                                is_shareable, out_res_kmt);
   if (!NT_SUCCESS(status)) {
      npt_log("virtgpu: create allocation failed 0x%lx", status);
      return status;
   }

   if (!alloc_info.hAllocation) {
      npt_log("virtgpu: allocation callback returned no allocation handle");
      return STATUS_UNSUCCESSFUL;
   }
   *out_alloc = alloc_info.hAllocation;

   if (!virtgpu_drain(gpu)) {
      status = STATUS_UNSUCCESSFUL;
      goto fail_destroy;
   }

   VIOGPU_ESCAPE res_info = {
      .Type = VIOGPU_RES_INFO,
      .DataLength = sizeof(res_info.ResourceInfo),
      .ResourceInfo = { .ResHandle = *out_alloc },
   };
   status = virtgpu_escape(gpu, &res_info);
   if (!NT_SUCCESS(status)) {
      npt_log("virtgpu: RES_INFO escape failed 0x%lx", status);
      goto fail_destroy;
   }
   if (!res_info.ResourceInfo.IsBlob || !res_info.ResourceInfo.IsCreated ||
       !res_info.ResourceInfo.Id) {
      npt_log("virtgpu: RES_INFO reports blob not created");
      status = STATUS_INVALID_PARAMETER;
      goto fail_destroy;
   }
   *out_res_id = res_info.ResourceInfo.Id;

   if (is_mappable) {
      status = virtgpu_map_blob_op(gpu, *out_alloc, VIOGPU_CMD_MAP_BLOB);
      if (!NT_SUCCESS(status))
         goto fail_destroy;
   }
   return STATUS_SUCCESS;

fail_destroy:
   virtgpu_resource_destroy_blob(gpu, *out_alloc, *out_res_kmt);
   *out_alloc = 0;
   *out_res_kmt = 0;
   return status;
}

static NTSTATUS
virtgpu_resource_destroy_blob(struct npt_virtgpu *gpu,
                              D3DKMT_HANDLE alloc, D3DKMT_HANDLE res_kmt)
{
   if (!gpu || (!alloc && !res_kmt))
      return STATUS_INVALID_PARAMETER;

   if (!virtgpu_drain(gpu)) {
      /* The host can still reference this allocation.  Preserve it rather
       * than turning an unproven drain into a use-after-free. */
      npt_log("virtgpu: pre-destroy drain failed; allocation preserved");
      return STATUS_UNSUCCESSFUL;
   }

#if defined(NPT_D3D9_RUNTIME_DDI) || defined(NPT_D3D10_RUNTIME_DDI)
   NTSTATUS status;
   if (gpu->use_runtime_ddi) {
      if (!gpu->device_callbacks.pfnDeallocateCb || !gpu->h_rt_device)
         return STATUS_INVALID_PARAMETER;
      D3DDDICB_DEALLOCATE destroy = {
         .hResource = NULL,
         .NumAllocations = 1,
         .HandleList = &alloc,
      };
      status = virtgpu_status_from_hresult(
         gpu->device_callbacks.pfnDeallocateCb(gpu->h_rt_device, &destroy));
   } else
#endif
   {
      if (!gpu->cb.destroyAllocation || !gpu->device)
         return STATUS_INVALID_PARAMETER;
      D3DKMT_DESTROYALLOCATION destroy = {
      .hDevice = gpu->device,
      .hResource = res_kmt,
      .AllocationCount = res_kmt == 0 ? 1 : 0,
      .phAllocationList = res_kmt == 0 ? &alloc : NULL,
      };
      status = gpu->cb.destroyAllocation(&destroy);
   }
   if (!NT_SUCCESS(status)) {
      npt_log("virtgpu: D3DKMTDestroyAllocation failed 0x%lx", status);
      return status;
   }
   /* A successful WDDM deallocation callback ends this UMD's ownership.  The
    * pre-destroy drain already retired every user of the allocation.  A second
    * drain would create an ambiguous result: failure after successful
    * deallocation cannot be retried without double-destroying the handle. */
   return STATUS_SUCCESS;
}

/* Clean up a mapped transport blob in strict lifetime order.  If any step
 * fails, keep the remaining backing alive because the host or dxgkrnl can
 * still reference it. */
static void
virtgpu_cleanup_mapped_blob(struct npt_virtgpu *gpu, D3DKMT_HANDLE alloc,
                            D3DKMT_HANDLE res_kmt, bool is_locked)
{
   NTSTATUS status;

   if (!gpu || !alloc)
      return;
   if (is_locked) {
      status = virtgpu_unlock(gpu, alloc);
      if (!NT_SUCCESS(status)) {
         npt_log("virtgpu: cleanup unlock failed 0x%lx; backing preserved",
                 status);
         return;
      }
   }
   status = virtgpu_map_blob_op(gpu, alloc, VIOGPU_CMD_UNMAP_BLOB);
   if (!NT_SUCCESS(status)) {
      npt_log("virtgpu: cleanup unmap failed 0x%lx; backing preserved", status);
      return;
   }
   status = virtgpu_resource_destroy_blob(gpu, alloc, res_kmt);
   if (!NT_SUCCESS(status))
      npt_log("virtgpu: cleanup destroy failed 0x%lx; backing preserved",
              status);
}

/* Attach an existing VM-global virtio resource to this transport
 * context via a VIOGPU_RESOURCE_TYPE_IMPORT allocation, so the host
 * worker receives the resource fd (proxy attach-forwarding) before a
 * SHARED_OPEN_RES names it. */
static bool
npt_vgw32_import_res(struct npt_renderer *r, uint32_t res_id, uint64_t size,
                     uint32_t *out_alloc, uint32_t *out_res_kmt)
{
   struct npt_virtgpu *gpu = (struct npt_virtgpu *)r;

   if (!gpu || !res_id || !out_alloc || !out_res_kmt)
      return false;
   *out_alloc = 0;
   *out_res_kmt = 0;

   uint64_t aligned_size = 0;
   if (!npt_align64_checked(size ? size : 4096, 4096, &aligned_size) ||
       aligned_size > (uint64_t)SIZE_MAX)
      return false;

   VIOGPU_CREATE_ALLOCATION_EXCHANGE alloc_priv = {
      .Type = VIOGPU_RESOURCE_TYPE_IMPORT,
      .OptionsImport = {
         .res_id = res_id,
      },
      .Size = aligned_size,
   };
   VIOGPU_CREATE_RESOURCE_EXCHANGE res_priv = { 0 };
   D3DDDI_ALLOCATIONINFO alloc_info = {
      .pPrivateDriverData = &alloc_priv,
      .PrivateDriverDataSize = sizeof(alloc_priv),
   };

   if (!virtgpu_drain(gpu))
      return false;

   NTSTATUS status = virtgpu_create_allocation(gpu, &res_priv, &alloc_info,
                                                false, out_res_kmt);
   if (!NT_SUCCESS(status)) {
      npt_log("virtgpu: import_res res_id=%u failed 0x%lx", res_id, status);
      return false;
   }

   if (!alloc_info.hAllocation) {
      npt_log("virtgpu: import_res returned no allocation handle");
      return false;
   }
   *out_alloc = alloc_info.hAllocation;

   /* Flush so the KMD's CTX_ATTACH_RESOURCE reaches the host before
    * the caller's ring command names the resource. */
   if (!virtgpu_drain(gpu)) {
      npt_log("virtgpu: import_res res_id=%u post-drain failed", res_id);
      virtgpu_resource_destroy_blob(gpu, alloc_info.hAllocation,
                                    *out_res_kmt);
      *out_alloc = 0;
      *out_res_kmt = 0;
      return false;
   }

   npt_log("virtgpu: import_res res_id=%u alloc=0x%x", res_id,
           alloc_info.hAllocation);
   return true;
}

bool
npt_vgw32_release_import_res_checked(struct npt_renderer *r, uint32_t alloc,
                                     uint32_t res_kmt)
{
   struct npt_virtgpu *gpu = (struct npt_virtgpu *)r;
   if (!alloc && !res_kmt)
      return true;
   NTSTATUS status = virtgpu_resource_destroy_blob(gpu, (D3DKMT_HANDLE)alloc,
                                                   (D3DKMT_HANDLE)res_kmt);
   if (!NT_SUCCESS(status)) {
      npt_log("virtgpu: import release failed 0x%lx; allocation preserved",
              status);
      return false;
   }
   return true;
}

static bool
npt_vgw32_release_import_res(struct npt_renderer *r, uint32_t alloc,
                             uint32_t res_kmt)
{
   return npt_vgw32_release_import_res_checked(r, alloc, res_kmt);
}

static struct npt_renderer_shmem *
npt_vgw32_shmem_create(struct npt_renderer *r, size_t size)
{
   struct npt_virtgpu *gpu = (struct npt_virtgpu *)r;

   uint64_t aligned_size = 0;
   if (!gpu || !size ||
       !npt_align64_checked((uint64_t)size, 4096, &aligned_size) ||
       aligned_size > (uint64_t)SIZE_MAX)
      return NULL;
   const size_t alloc_size = (size_t)aligned_size;

   D3DKMT_HANDLE alloc = 0, res_kmt = 0;
   uint32_t res_id = 0;
   NTSTATUS status = virtgpu_resource_create_blob(
      gpu, VIOGPU_BLOB_MEM_HOST3D,
      VIOGPU_BLOB_FLAG_USE_MAPPABLE,
      alloc_size, 0, &res_id, &alloc, &res_kmt);
   if (!NT_SUCCESS(status))
      return NULL;

   void *ptr = NULL;
   status = virtgpu_lock(gpu, alloc, &ptr);
   if (!NT_SUCCESS(status)) {
      npt_log("virtgpu: lock failed 0x%lx", status);
      virtgpu_cleanup_mapped_blob(gpu, alloc, res_kmt, false);
      return NULL;
   }

   struct npt_virtgpu_shmem *s = npt_alloc(sizeof(*s));
   if (!s) {
      virtgpu_cleanup_mapped_blob(gpu, alloc, res_kmt, true);
      return NULL;
   }
   npt_refcount_init(&s->base.refcount);
   s->base.res_id = res_id;
   s->base.size = alloc_size;
   s->base.mmap_ptr = ptr;
   s->alloc = alloc;
   s->res_kmt = res_kmt;
   return &s->base;
}

static void
npt_vgw32_shmem_destroy(struct npt_renderer *r, struct npt_renderer_shmem *_s)
{
   struct npt_virtgpu *gpu = (struct npt_virtgpu *)r;
   struct npt_virtgpu_shmem *s = (struct npt_virtgpu_shmem *)_s;
   if (!s)
      return;

   NTSTATUS status = virtgpu_unlock(gpu, s->alloc);
   if (!NT_SUCCESS(status)) {
      npt_log("virtgpu: shmem unlock failed 0x%lx; backing preserved", status);
      return;
   }
   status = virtgpu_map_blob_op(gpu, s->alloc, VIOGPU_CMD_UNMAP_BLOB);
   if (!NT_SUCCESS(status)) {
      npt_log("virtgpu: shmem unmap failed 0x%lx; backing preserved", status);
      return;
   }
   status = virtgpu_resource_destroy_blob(gpu, s->alloc, s->res_kmt);
   if (!NT_SUCCESS(status)) {
      npt_log("virtgpu: shmem destroy failed 0x%lx; wrapper preserved", status);
      return;
   }
   free(s);
}

static bool
npt_vgw32_submit_cmd(struct npt_renderer *r, const void *data, size_t size)
{
   struct npt_virtgpu *gpu = (struct npt_virtgpu *)r;

   EnterCriticalSection(&gpu->cs);
   NTSTATUS status =
      virtgpu_send_cmd_locked(gpu, VIOGPU_CMD_SUBMIT,
                              VIOGPU_EXECBUF_RING_IDX, 0, data, size, 0);
   LeaveCriticalSection(&gpu->cs);

   if (!NT_SUCCESS(status)) {
      npt_log("virtgpu submit_cmd: render failed 0x%lx", status);
      return false;
   }
   return true;
}

static bool
npt_vgw32_submit_cmd_sync(struct npt_renderer *r, const void *data,
                          size_t size)
{
   struct npt_virtgpu *gpu = (struct npt_virtgpu *)r;

   /* A drain-only call submits its own terminal D3DKMT event packet. */
   if (size && !npt_vgw32_submit_cmd(r, data, size))
      return false;
   if (!size && data)
      return false;

   return virtgpu_drain(gpu);
}

/* Arm a Win32 event for the next GPU retirement on the requested ring: create an
 * auto-reset event and hand it to the KMD via VIOGPU_SUBMIT_PRESENT_FENCE, which
 * signals it at GPU completion.  npt_event's waiter blocks on it (wait_one) and
 * then SetEvents the caller's app event.
 *
 * The renderer interface return is int (sync_file-FD shape on Linux).  Windows
 * handles are 32-bit-significant (MSDN 32/64-bit interop), so the value
 * survives; callers treat a negative int as failure, so a handle with bit 31 set
 * would be misread as an error. */
static int
npt_vgw32_submit_present_fence(struct npt_renderer *r, uint32_t ring_idx)
{
   struct npt_virtgpu *gpu = (struct npt_virtgpu *)r;
   HANDLE hWait = CreateEventW(NULL, /*bManualReset*/ FALSE, /*bInitial*/ FALSE, NULL);
   if (!hWait)
      return -1;
   if ((uintptr_t)hWait > (uintptr_t)INT_MAX) {
      npt_log("virtgpu: present-fence handle cannot fit signed renderer ABI");
      CloseHandle(hWait);
      return -1;
   }
   VIOGPU_ESCAPE esc = {
      .Type = VIOGPU_SUBMIT_PRESENT_FENCE,
      .DataLength = sizeof(esc.PresentFence),
      .PresentFence = { .EventUM = VioGpuUmHandle(hWait), .RingIdx = ring_idx },
   };
   NTSTATUS status = virtgpu_escape(gpu, &esc);
   if (!NT_SUCCESS(status)) {
      npt_log("virtgpu: SUBMIT_PRESENT_FENCE failed 0x%lx ring=%u", status, ring_idx);
      CloseHandle(hWait);
      return -1;
   }
   return (int)(uintptr_t)hWait;
}

static void
npt_vgw32_destroy(struct npt_renderer *r)
{
   struct npt_virtgpu *gpu = (struct npt_virtgpu *)r;
   if (!gpu)
      return;

   if (virtgpu_has_context(gpu) && !virtgpu_drain(gpu))
      npt_log("virtgpu: final drain failed during renderer teardown");

#if defined(NPT_D3D9_RUNTIME_DDI) || defined(NPT_D3D10_RUNTIME_DDI)
   if (gpu->use_runtime_ddi && gpu->h_ddi_context &&
       gpu->device_callbacks.pfnDestroyContextCb) {
      D3DDDICB_DESTROYCONTEXT destroy = { .hContext = gpu->h_ddi_context };
      NTSTATUS status = virtgpu_status_from_hresult(
         gpu->device_callbacks.pfnDestroyContextCb(gpu->h_rt_device,
                                                    &destroy));
      if (!NT_SUCCESS(status))
         npt_log("virtgpu: runtime DestroyContextCb failed 0x%lx", status);
      gpu->h_ddi_context = NULL;
   }
#endif
   if (gpu->context && gpu->cb.destroyContext) {
      D3DKMT_DESTROYCONTEXT destroy = { .hContext = gpu->context };
      NTSTATUS status = gpu->cb.destroyContext(&destroy);
      if (!NT_SUCCESS(status))
         npt_log("virtgpu: D3DKMTDestroyContext failed 0x%lx", status);
   }
   if (gpu->device && gpu->cb.destroyDevice) {
      D3DKMT_DESTROYDEVICE destroy = { .hDevice = gpu->device };
      NTSTATUS status = gpu->cb.destroyDevice(&destroy);
      if (!NT_SUCCESS(status))
         npt_log("virtgpu: D3DKMTDestroyDevice failed 0x%lx", status);
   }
   if (gpu->adapter && gpu->cb.closeAdapter) {
      D3DKMT_CLOSEADAPTER close = { .hAdapter = gpu->adapter };
      NTSTATUS status = gpu->cb.closeAdapter(&close);
      if (!NT_SUCCESS(status))
         npt_log("virtgpu: D3DKMTCloseAdapter failed 0x%lx", status);
   }
   if (gpu->gdi32)
      FreeLibrary(gpu->gdi32);

   DeleteCriticalSection(&gpu->cs);
   free(gpu);
}

static bool
virtgpu_load_d3dkmt(struct npt_virtgpu *gpu)
{
   gpu->gdi32 = LoadLibraryA("GDI32.dll");
   if (!gpu->gdi32) {
      npt_log("virtgpu: failed to load GDI32.dll");
      return false;
   }

#define GETPROC(field, fn) \
   gpu->cb.field = (void *)GetProcAddress(gpu->gdi32, "D3DKMT" #fn)

   GETPROC(queryAdapterInfo, QueryAdapterInfo);
   GETPROC(escape, Escape);
   GETPROC(render, Render);
   GETPROC(createContext, CreateContext);
   GETPROC(destroyContext, DestroyContext);
   GETPROC(createAllocation, CreateAllocation);
   GETPROC(destroyAllocation, DestroyAllocation);
   GETPROC(lock, Lock);
   GETPROC(unlock, Unlock);
   GETPROC(createDevice, CreateDevice);
   GETPROC(destroyDevice, DestroyDevice);
   GETPROC(getDeviceState, GetDeviceState);
   GETPROC(openAdapterFromHdc, OpenAdapterFromHdc);
   GETPROC(closeAdapter, CloseAdapter);

#undef GETPROC

   if (!gpu->cb.queryAdapterInfo || !gpu->cb.escape || !gpu->cb.render ||
       !gpu->cb.createContext || !gpu->cb.destroyContext ||
       !gpu->cb.createAllocation || !gpu->cb.destroyAllocation ||
       !gpu->cb.lock || !gpu->cb.unlock || !gpu->cb.createDevice ||
       !gpu->cb.destroyDevice || !gpu->cb.getDeviceState ||
       !gpu->cb.openAdapterFromHdc ||
       !gpu->cb.closeAdapter) {
      npt_log("virtgpu: GDI32 missing required D3DKMT entry points");
      return false;
   }
   return true;
}

static bool
virtgpu_find_adapter(struct npt_virtgpu *gpu)
{
   if (!gpu)
      return false;

   for (DWORD i = 0;; i++) {
      DISPLAY_DEVICEA dev;
      memset(&dev, 0, sizeof(dev));
      dev.cb = sizeof(dev);
      if (!EnumDisplayDevicesA(NULL, i, &dev, 0))
         break;
      if (!dev.DeviceName[0])
         continue;

      HDC hdc = CreateDCA("DISPLAY", dev.DeviceName, NULL, NULL);
      if (!hdc) {
         npt_log("virtgpu: CreateDC failed for %s", dev.DeviceName);
         continue;
      }
      D3DKMT_OPENADAPTERFROMHDC open = { .hDc = hdc };
      NTSTATUS status = gpu->cb.openAdapterFromHdc(&open);
      DeleteDC(hdc);
      if (!NT_SUCCESS(status)) {
         npt_log("virtgpu: OpenAdapterFromHdc failed 0x%lx for %s",
                 status, dev.DeviceName);
         continue;
      }
      gpu->adapter = open.hAdapter;
      gpu->luid = open.AdapterLuid;

      /* Vista does not reliably populate DISPLAY_DEVICE.DeviceID for every
       * display adapter.  Identify Triton through its private KMD ABI after
       * opening the HDC instead of rejecting the correct adapter on an empty
       * or vendor-rewritten PnP string. */
      VIOGPU_ADAPTERINFO info = { 0 };
      status = virtgpu_query_adapter_info(gpu, &info, sizeof(info));
      if (!NT_SUCCESS(status) || info.IamVioGPU != VIOGPU_IAM ||
          !info.Flags.Supports3d) {
         D3DKMT_CLOSEADAPTER close = { .hAdapter = gpu->adapter };
         NTSTATUS close_status = gpu->cb.closeAdapter(&close);
         if (!NT_SUCCESS(close_status))
            npt_log("virtgpu: CloseAdapter failed 0x%lx for %s",
                    close_status, dev.DeviceName);
         gpu->adapter = 0;
         memset(&gpu->luid, 0, sizeof(gpu->luid));
         continue;
      }
      npt_debug("virtgpu: opened adapter %s (LUID %lx-%lx)",
                dev.DeviceName, open.AdapterLuid.HighPart,
                open.AdapterLuid.LowPart);
      return true;
   }
   npt_log("virtgpu: no display adapter exposes the Triton private ABI");
   return false;
}

static bool
virtgpu_create_device(struct npt_virtgpu *gpu)
{
   D3DKMT_CREATEDEVICE create = { .hAdapter = gpu->adapter };
   NTSTATUS status = gpu->cb.createDevice(&create);
   if (!NT_SUCCESS(status)) {
      npt_log("virtgpu: D3DKMTCreateDevice failed 0x%lx", status);
      return false;
   }
   gpu->device = create.hDevice;
   return true;
}

static bool
virtgpu_create_context(struct npt_virtgpu *gpu)
{
#if defined(NPT_D3D9_RUNTIME_DDI) || defined(NPT_D3D10_RUNTIME_DDI)
   if (gpu->use_runtime_ddi) {
      if (!gpu->device_callbacks.pfnCreateContextCb || !gpu->h_rt_device)
         return false;
      D3DDDICB_CREATECONTEXT create = { 0 };
      NTSTATUS status = virtgpu_status_from_hresult(
         gpu->device_callbacks.pfnCreateContextCb(gpu->h_rt_device, &create));
      if (!NT_SUCCESS(status)) {
         npt_log("virtgpu: runtime CreateContextCb failed 0x%lx", status);
         return false;
      }
      if (!create.hContext || !create.pCommandBuffer ||
          create.CommandBufferSize < sizeof(VIOGPU_COMMAND_HDR) ||
          !create.pAllocationList || !create.AllocationListSize ||
          !create.pPatchLocationList || !create.PatchLocationListSize) {
         npt_log("virtgpu: runtime CreateContextCb returned incomplete buffers");
         if (create.hContext && gpu->device_callbacks.pfnDestroyContextCb) {
            D3DDDICB_DESTROYCONTEXT destroy = { .hContext = create.hContext };
            HRESULT cleanup_hr = gpu->device_callbacks.pfnDestroyContextCb(
               gpu->h_rt_device, &destroy);
            if (FAILED(cleanup_hr))
               npt_log("virtgpu: cleanup DestroyContextCb failed 0x%lx",
                       cleanup_hr);
         }
         return false;
      }
      gpu->h_ddi_context = create.hContext;
      gpu->cmd_buf = create.pCommandBuffer;
      gpu->cmd_size = create.CommandBufferSize;
      gpu->alloc_list = create.pAllocationList;
      gpu->alloc_size = create.AllocationListSize;
      gpu->patch_list = create.pPatchLocationList;
      gpu->patch_size = create.PatchLocationListSize;
      return true;
   }
#endif

   D3DKMT_CREATECONTEXT create = {
      .hDevice = gpu->device,
      .ClientHint = NPT_D3DKMT_CLIENT_HINT,
   };
   NTSTATUS status = gpu->cb.createContext(&create);
   if (!NT_SUCCESS(status)) {
      npt_log("virtgpu: D3DKMTCreateContext failed 0x%lx", status);
      return false;
   }
   if (!create.hContext || !create.pCommandBuffer ||
       create.CommandBufferSize < sizeof(VIOGPU_COMMAND_HDR) ||
       !create.pAllocationList || !create.AllocationListSize ||
       !create.pPatchLocationList || !create.PatchLocationListSize) {
      npt_log("virtgpu: D3DKMTCreateContext returned incomplete buffers");
      if (create.hContext) {
         D3DKMT_DESTROYCONTEXT destroy = { .hContext = create.hContext };
         status = gpu->cb.destroyContext(&destroy);
         if (!NT_SUCCESS(status))
            npt_log("virtgpu: cleanup DestroyContext failed 0x%lx", status);
      }
      return false;
   }
   gpu->context = create.hContext;
   gpu->cmd_buf = create.pCommandBuffer;
   gpu->cmd_size = create.CommandBufferSize;
   gpu->alloc_list = create.pAllocationList;
   gpu->alloc_size = create.AllocationListSize;
   gpu->patch_list = create.pPatchLocationList;
   gpu->patch_size = create.PatchLocationListSize;
   return true;
}

struct npt_renderer *
npt_renderer_create_virtgpu(void)
{
   struct npt_virtgpu *gpu = npt_alloc(sizeof(*gpu));
   if (!gpu)
      return NULL;
   InitializeCriticalSection(&gpu->cs);

#if defined(NPT_D3D9_RUNTIME_DDI) || defined(NPT_D3D10_RUNTIME_DDI)
   if (g_d3d9_runtime_binding.valid) {
      gpu->use_runtime_ddi = true;
      gpu->h_rt_adapter = g_d3d9_runtime_binding.h_rt_adapter;
      gpu->h_rt_device = g_d3d9_runtime_binding.h_rt_device;
      gpu->adapter_callbacks = g_d3d9_runtime_binding.adapter_callbacks;
      gpu->device_callbacks = g_d3d9_runtime_binding.device_callbacks;
   }
   if (gpu->use_runtime_ddi) {
      const UINT callback_mask = npt_d3d9_runtime_callback_mask(
         &gpu->adapter_callbacks, &gpu->device_callbacks);
      if ((callback_mask & NPT_D3D9_REQUIRED_CALLBACKS) !=
          NPT_D3D9_REQUIRED_CALLBACKS) {
         npt_log("virtgpu: D3D9 runtime callbacks incomplete interface=0x%x version=0x%x have=0x%03x need=0x%03x missing=0x%03x",
                 g_d3d9_runtime_binding.interface_version,
                 g_d3d9_runtime_binding.runtime_version, callback_mask,
                 NPT_D3D9_REQUIRED_CALLBACKS,
                 NPT_D3D9_REQUIRED_CALLBACKS & ~callback_mask);
         goto fail;
      }
   } else
#endif
   {
   if (!virtgpu_load_d3dkmt(gpu))
      goto fail;
   if (!virtgpu_find_adapter(gpu))
      goto fail;
   if (!virtgpu_create_device(gpu))
      goto fail;
   }

   VIOGPU_ADAPTERINFO_V2 info = { 0 };
   NTSTATUS status = virtgpu_query_adapter_info(gpu, &info, sizeof(info));
   if (!NT_SUCCESS(status)) {
      npt_log("virtgpu: QueryAdapterInfo failed 0x%lx", status);
      goto fail;
   }
   if (info.V1.IamVioGPU != VIOGPU_IAM || !info.V1.Flags.Supports3d ||
       !info.V1.Flags.HasShmem) {
      npt_log("virtgpu: KMD reports no 3D/shmem support (iam=0x%llx 3d=%u shmem=%u)",
              (unsigned long long)info.V1.IamVioGPU,
              info.V1.Flags.Supports3d, info.V1.Flags.HasShmem);
      goto fail;
   }
   if (info.StructureSize < sizeof(info) ||
       info.PrivateAbiVersion < VIOGPU_PRIVATE_ABI_VERSION_V2 ||
       !(info.FeatureBits & VIOGPU_FEATURE_RENDER_EVENT)) {
      npt_log("virtgpu: KMD lacks required render-event ABI (size=%lu version=%lu features=0x%llx)",
              info.StructureSize, info.PrivateAbiVersion,
              (unsigned long long)info.FeatureBits);
      goto fail;
   }
   if (!(info.V1.SupportedCapsetIDs & (1ull << NPT_CAPSET_ID))) {
      npt_log("virtgpu: KMD does not advertise Neptune capset");
      goto fail;
   }

   struct npt_capset capset = { 0 };
   status = virtgpu_get_caps(gpu, NPT_CAPSET_ID, 0, &capset, sizeof(capset));
   if (!NT_SUCCESS(status)) {
      npt_log("virtgpu: GET_CAPS for Neptune failed 0x%lx", status);
      goto fail;
   }
   if (capset.wire_format_version != NPT_PROTOCOL_WIRE_VERSION) {
      npt_log("virtgpu: wire format mismatch host=0x%08x guest=0x%08x",
              capset.wire_format_version,
              (unsigned)NPT_PROTOCOL_WIRE_VERSION);
      goto fail;
   }

   VIOGPU_ESCAPE ctx_init = {
      .Type = VIOGPU_CTX_INIT,
      .DataLength = sizeof(ctx_init.CtxInit),
      .CtxInit = {
         .CapsetID = NPT_CAPSET_ID,
         .NumRings = 64,
         .DebugName = "neptune-win32",
      },
   };
   status = virtgpu_escape(gpu, &ctx_init);
   if (!NT_SUCCESS(status)) {
      npt_log("virtgpu: CTX_INIT failed 0x%lx", status);
      goto fail;
   }

   if (!virtgpu_create_context(gpu))
      goto fail;

   gpu->base.info.max_timeline_count = 64;
   /* The KMD targets this context by id when another device's flip
    * present submits this transport's WSI_PRESENT bytes. */
   gpu->base.info.virtio_ctx_id = ctx_init.CtxInit.CtxId;

   gpu->base.ops.destroy = npt_vgw32_destroy;
   gpu->base.ops.submit_cmd = npt_vgw32_submit_cmd;
   gpu->base.ops.submit_cmd_sync = npt_vgw32_submit_cmd_sync;
   gpu->base.ops.submit_present_fence = npt_vgw32_submit_present_fence;
   gpu->base.ops.import_res = npt_vgw32_import_res;
   gpu->base.ops.release_import_res = npt_vgw32_release_import_res;
   /* create_host_blob and wsi_* are wired alongside the DXGI swapchain
    * Present path; the renderer helpers NULL-check them. */

   gpu->base.shmem_ops.create = npt_vgw32_shmem_create;
   gpu->base.shmem_ops.destroy = npt_vgw32_shmem_destroy;

   npt_log("virtgpu: renderer ready (wire=0x%08x, max_timeline=%u)",
           capset.wire_format_version, gpu->base.info.max_timeline_count);
   return &gpu->base;

fail:
   npt_vgw32_destroy(&gpu->base);
   return NULL;
}

struct npt_renderer *
npt_renderer_create_vtest(void)
{
   return NULL;
}
