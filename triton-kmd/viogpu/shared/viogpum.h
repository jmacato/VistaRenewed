/*
 * Copyright (C) 2019-2020 Red Hat, Inc.
 *
 * Written By: Vadim Rozenfeld <vrozenfe@redhat.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met :
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and / or other materials provided with the distribution.
 * 3. Neither the names of the copyright holders nor the names of their contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#pragma once

#include <stddef.h>
#include <d3dkmthk.h>

// ================= UMD <-> KMD POINTER/HANDLE TRANSPORT
//
// A 32-bit (WOW) UMD and a 64-bit KMD must agree on the byte layout of every
// structure crossing this interface, so user-mode addresses and handles are
// carried in fixed 64-bit fields rather than in pointer-sized ones.  Use the
// helpers below instead of open-coding the casts: the UMD packs with
// VioGpuUmPtr()/VioGpuUmHandle(), the KMD unpacks with VIOGPU_UM_PTR_AS()/
// VioGpuUmHandleValue().  The two-step cast through ULONG_PTR is deliberate --
// it is what makes the round trip lossless in a 32-bit process.

typedef ULONGLONG VIOGPU_UM_PTR;    // user-mode VA, zero-extended
typedef ULONGLONG VIOGPU_UM_HANDLE; // user-mode HANDLE, zero-extended

// Pack (user mode).
static __inline VIOGPU_UM_PTR VioGpuUmPtr(const void *p)
{
    return (VIOGPU_UM_PTR)(ULONG_PTR)p;
}

static __inline VIOGPU_UM_HANDLE VioGpuUmHandle(HANDLE h)
{
    return (VIOGPU_UM_HANDLE)(ULONG_PTR)h;
}

// Unpack (kernel mode).  The pointer form is a macro because the caller picks
// the target type; C++ will not implicitly convert the void * an inline
// function would have to return.
#define VIOGPU_UM_PTR_AS(type, v) ((type)(ULONG_PTR)(v))

static __inline HANDLE VioGpuUmHandleValue(VIOGPU_UM_HANDLE v)
{
    return (HANDLE)(ULONG_PTR)v;
}

#pragma pack(1)
typedef struct _VIOGPU_BOX
{
    ULONG x;
    ULONG y;
    ULONG z;
    ULONG width;
    ULONG height;
    ULONG depth;
} VIOGPU_BOX;
#pragma pack()

#pragma pack(1)
typedef struct _VIOGPU_BLOB_INFO {
    ULONG width;
    ULONG height;
    ULONG format;
    ULONG bind; // Same as virgl
    ULONG strides[4];
    ULONG offsets[4];
} VIOGPU_BLOB_INFO, *PVIOGPU_BLOB_INFO;
#pragma pack()

// ================= QueryAdapterInfo UMDRIVERPRIVATE
#define VIOGPU_IAM 0x56696f475055 // Identifier for queryadapterinfo (VioGPU as hex)

#define VIOGPU_CAPSET_GFXSTREAM_VULKAN 3
#define VIOGPU_CAPSET_VENUS 4
#define VIOGPU_CAPSET_NEPTUNE 7

typedef struct _VIOGPU_ADAPTERINFO
{
    ULONGLONG IamVioGPU; // Should be set by driver to VIOGPU_IAM
    struct
    {
        UINT Supports3d : 1;
        UINT HasShmem : 1;
        UINT Reserved : 30;
    } Flags;
    ULONGLONG SupportedCapsetIDs;
    LUID AdapterLuid;
} VIOGPU_ADAPTERINFO;

// The V1 layout above is an ABI prefix and is intentionally left untouched:
// existing Triton UMDs query exactly sizeof(VIOGPU_ADAPTERINFO).  New UMDs
// opt into this extension by supplying a V2-sized output buffer.
#define VIOGPU_PRIVATE_ABI_VERSION_V2 2u
#define VIOGPU_FEATURE_RENDER_EVENT   (1ull << 0)

typedef struct _VIOGPU_ADAPTERINFO_V2
{
    VIOGPU_ADAPTERINFO V1;
    ULONG StructureSize;
    ULONG PrivateAbiVersion;
    ULONGLONG FeatureBits;
} VIOGPU_ADAPTERINFO_V2;

static_assert(sizeof(VIOGPU_ADAPTERINFO) == 32, "VIOGPU_ADAPTERINFO must stay ABI-stable");
static_assert(sizeof(VIOGPU_ADAPTERINFO_V2) == 48, "VIOGPU_ADAPTERINFO_V2 layout");

// ================= ESCAPES
#define VIOGPU_GET_DEVICE_ID         0x000
#define VIOGPU_GET_CUSTOM_RESOLUTION 0x001
#define VIOGPU_GET_CAPS              0x002
#define VIOGPU_GET_PCI_INFO          0x003

#define VIOGPU_RES_INFO              0x100
#define VIOGPU_RES_BUSY              0x101
#define VIOGPU_CTX_INIT              0x200

#define VIOGPU_BLIT_INIT             0x300

// Submit an empty GPU-done fence on an event ring (>= NPT_EVENT_RING_BASE) and
// signal EventUM from the completion DPC when the host retires it.  Because an
// event-ring fence's used-ring response is deferred by the host until real GPU
// completion, EventUM fires exactly when the frame's render finished on the GPU
// -- the async present-completion wait the UMD blocks on before the flip.
#define VIOGPU_SUBMIT_PRESENT_FENCE  0x400

#pragma pack(1)
typedef struct _VIOGPU_DISP_MODE
{
    USHORT XResolution;
    USHORT YResolution;
} VIOGPU_DISP_MODE, *PVIOGPU_DISP_MODE;
#pragma pack()

#pragma pack(1)
typedef struct _VIOGPU_PARAM_REQ
{
    ULONG ParamId;
    UINT64 Value;
} VIOGPU_PARAM_REQ;
#pragma pack()

#pragma pack(1)
typedef struct _VIOGPU_CAPSET_REQ
{
    ULONG CapsetId;
    ULONG Version;
    ULONG Size;
    VIOGPU_UM_PTR Capset; // output buffer, Size bytes
} VIOGPU_CAPSET_REQ;
#pragma pack()

#pragma pack(1)
typedef struct _VIOGPU_PCI_INFO_REQ
{
    ULONG Domain;
    ULONG Bus;
    ULONG Dev;
    ULONG Func;
} VIOGPU_PCI_INFO_REQ;
#pragma pack()

#pragma pack(1)
typedef struct _VIOGPU_RES_INFO_REQ
{
    D3DKMT_HANDLE ResHandle;
    ULONG Id;

    BOOL IsBlob;
    BOOL IsCreated;
    BOOL InfoValid;

    VIOGPU_BLOB_INFO Info;

    ULONG BlobMem;
    ULONGLONG BlobId;
    ULONGLONG Size;
} VIOGPU_RES_INFO_REQ;
#pragma pack()

#pragma pack(1)
typedef struct _VIOGPU_RES_BUSY_REQ
{
    D3DKMT_HANDLE ResHandle;
    BOOL Wait;
    BOOL IsBusy;
} VIOGPU_RES_BUSY_REQ;
#pragma pack()

#pragma pack(1)
typedef struct _VIOGPU_CTX_INIT_REQ
{
    UINT CapsetID;
    UINT NumRings;
    UCHAR DebugName[64];
    UINT CtxId; // out: virtio context id of the created context
} VIOGPU_CTX_INIT_REQ;
#pragma pack()

typedef struct _VIOGPU_BLIT_PRESENT VIOGPU_BLIT_PRESENT, *PVIOGPU_BLIT_PRESENT;

#pragma pack(1)
typedef struct _VIOGPU_BLIT_INIT_REQ
{
    VIOGPU_UM_HANDLE EventUM;
    VIOGPU_UM_HANDLE EventKM;
    VIOGPU_UM_PTR pBlitPresent; // PVIOGPU_BLIT_PRESENT
} VIOGPU_BLIT_INIT_REQ;
#pragma pack()

#pragma pack(1)
typedef struct _VIOGPU_PRESENT_FENCE_REQ
{
    VIOGPU_UM_HANDLE EventUM; // UMD auto-reset event the KMD signals from the
                              // completion DPC
    ULONG RingIdx;            // event ring (>= NPT_EVENT_RING_BASE) to carry the fence
} VIOGPU_PRESENT_FENCE_REQ;
#pragma pack()

// ================= CreateResource
#pragma pack(1)
typedef struct _VIOGPU_RESOURCE_3D_OPTIONS
{
    ULONG target;
    ULONG format;
    ULONG bind;
    ULONG width;
    ULONG height;
    ULONG depth;
    ULONG array_size;
    ULONG last_level;
    ULONG nr_samples;
    ULONG flags;
} VIOGPU_RESOURCE_3D_OPTIONS;
#pragma pack()

/* Private to the WDDM resource-allocation handshake.  It marks the legacy
 * non-blob primary which backs Vista's display mode.  The KMD consumes this
 * bit before issuing RESOURCE_CREATE_3D, so it never reaches the host. */
#define VIOGPU_RESOURCE_FLAG_STANDARD_PRIMARY 0x80000000u

#define VIOGPU_BLOB_MEM_GUEST             0x0001
#define VIOGPU_BLOB_MEM_HOST3D            0x0002
#define VIOGPU_BLOB_MEM_HOST3D_GUEST      0x0003

#define VIOGPU_BLOB_FLAG_USE_MAPPABLE     0x0001
#define VIOGPU_BLOB_FLAG_USE_SHAREABLE    0x0002
//#define VIOGPU_BLOB_FLAG_USE_CROSS_DEVICE 0x0004
#define VIOGPU_BLOB_FLAG_PINNED           0x0008
#pragma pack(1)
typedef struct _VIOGPU_RESOURCE_BLOB_OPTIONS
{
	ULONG blob_mem;
	ULONG blob_flags;
    ULONGLONG blob_id;
} VIOGPU_RESOURCE_BLOB_OPTIONS;
#pragma pack()

#pragma pack(1)
typedef struct _VIOGPU_CREATE_RESOURCE_EXCHANGE
{
    ULONG magic;
} VIOGPU_CREATE_RESOURCE_EXCHANGE;
#pragma pack()

// Import an existing VM-global virtio resource (a shared texture's blob,
// created by another process/device) into this device's context.  The KMD
// does NOT mint a res_id, issue RESOURCE_CREATE_BLOB, or destroy the
// resource: ownership stays with the creating allocation.  Opening the
// allocation attaches the resource to the opening device's virtio context
// (CTX_ATTACH_RESOURCE), which is what forwards the host dmabuf into that
// context's render worker.  Venus analog: dma-buf import.
// Keep in lockstep with virtio-win-mesa/src/virtio/virtio-gpu/wddm_hw.h.
#pragma pack(1)
typedef struct _VIOGPU_RESOURCE_IMPORT_OPTIONS
{
    ULONG res_id;
} VIOGPU_RESOURCE_IMPORT_OPTIONS;
#pragma pack()

// Shared / presentable D3D11 texture backed by a virtio-gpu blob resource
// (Venus model).  The UMD created the host texture with exportable storage
// and staged its dmabuf as a pending blob under blob_id on create_ctx_id
// (the UMD's transport context) via SHARED_EXPORT_BLOB.  The KMD mints a
// res_id and issues RESOURCE_CREATE_BLOB(HOST3D, blob_id) on create_ctx_id
// when the creating device opens the allocation, binding the res_id to the
// dmabuf VM-globally.  primary != 0 marks a flippable scanout primary:
// segment-1 residency, scanout promotion, and FlushToScreen uses
// ScanoutInfo for SET_SCANOUT_BLOB.  The trailing D3D11/dmabuf description
// is opaque to the KMD; it round-trips through the WDDM allocation private
// data so an opening process's UMD can rebuild the texture (paired with
// the res_id from VIOGPU_RES_INFO) via SHARED_OPEN_RES.
// Keep in lockstep with virtio-win-mesa/src/virtio/virtio-gpu/wddm_hw.h.
#pragma pack(1)
typedef struct _VIOGPU_RESOURCE_SHARED_TEXTURE_OPTIONS
{
    // --- blob binding (consumed by the KMD) ---
    ULONGLONG blob_id;
    ULONG create_ctx_id;    // 0 = the opening device's own context
    ULONG primary;
    VIOGPU_BLOB_INFO ScanoutInfo;
    // --- D3D11 + dmabuf rebuild info for opening UMDs (opaque to KMD) ---
    ULONG width;
    ULONG height;
    ULONG mip_levels;
    ULONG array_size;
    ULONG format;           // DXGI_FORMAT
    ULONG sample_count;
    ULONG usage;            // D3D11_USAGE
    ULONG bind_flags;
    ULONG cpu_access_flags;
    ULONG misc_flags;
    ULONG texture_layout;   // D3D11_TEXTURE_LAYOUT
    ULONG plane_count;
    ULONGLONG modifier;     // DRM format modifier of the export
    ULONGLONG allocation_size;
    struct {
        ULONGLONG offset;
        ULONGLONG pitch;
    } planes[4];
} VIOGPU_RESOURCE_SHARED_TEXTURE_OPTIONS;
#pragma pack()

#define VIOGPU_RESOURCE_TYPE_3D     0
#define VIOGPU_RESOURCE_TYPE_BLOB   1
#define VIOGPU_RESOURCE_TYPE_IMPORT 2
#define VIOGPU_RESOURCE_TYPE_SHARED 3

#pragma pack(1)
typedef struct _VIOGPU_CREATE_ALLOCATION_EXCHANGE
{
    ULONG Type;
    union {
        VIOGPU_RESOURCE_3D_OPTIONS Options3D;
        VIOGPU_RESOURCE_BLOB_OPTIONS OptionsBlob;
        VIOGPU_RESOURCE_IMPORT_OPTIONS OptionsImport;
        VIOGPU_RESOURCE_SHARED_TEXTURE_OPTIONS OptionsShared;
    };
    ULONGLONG Size;
} VIOGPU_CREATE_ALLOCATION_EXCHANGE;
#pragma pack()

#pragma pack(1)
typedef struct _VIOGPU_ESCAPE
{
    USHORT Type;
    USHORT DataLength;
    union {
        ULONG Id;
        VIOGPU_DISP_MODE Resolution;
        VIOGPU_PARAM_REQ Parameter;
        VIOGPU_CAPSET_REQ Capset;
        VIOGPU_PCI_INFO_REQ PciInfo;

        VIOGPU_RES_INFO_REQ ResourceInfo;
        VIOGPU_RES_BUSY_REQ ResourceBusy;
        VIOGPU_CTX_INIT_REQ CtxInit;

        VIOGPU_BLIT_INIT_REQ BlitInit;

        VIOGPU_PRESENT_FENCE_REQ PresentFence;
    } DUMMYUNIONNAME;
} VIOGPU_ESCAPE, *PVIOGPU_ESCAPE;
#pragma pack()

// ================= BLIT

#pragma pack(1)
struct _VIOGPU_BLIT_PRESENT
{
    struct {
        VIOGPU_UM_PTR resource; // UMD-private, opaque to the KMD (round-tripped)
        RECT rect;
    } src;
    struct {
        VIOGPU_CREATE_ALLOCATION_EXCHANGE alloc;
        VIOGPU_RES_INFO_REQ res_info;
        RECT rect;
    } dst;
};
#pragma pack()

// ================= COMMAND BUFFER
#define VIOGPU_CMD_NOP                0x0
#define VIOGPU_CMD_SUBMIT             0x1 // Submit Command to virgl
#define VIOGPU_CMD_TRANSFER_TO_HOST   0x2 // Transfer resource to host
#define VIOGPU_CMD_TRANSFER_FROM_HOST 0x3 // Transfer resource to host
#define VIOGPU_CMD_MAP_BLOB           0x4 // Map blob resource
#define VIOGPU_CMD_UNMAP_BLOB         0x5 // Unmap blob resource
// A terminal command in a render stream.  The KMD references Event while it
// is still executing in the submitting process, then signals that referenced
// object after every preceding command in the stream has retired.
#define VIOGPU_CMD_SIGNAL_EVENT        0x6
// KMD-generated packet used by Vista's basic display Present path.  It is
// deliberately rejected by DxgkDdiRender, so user mode cannot request an
// arbitrary kernel copy or choose allocation indices.
#define VIOGPU_CMD_COPY_FIXED_PRIMARY  0x7
// KMD-generated terminal step for Vista's fixed-primary Present packet.  It
// executes only after the preceding asynchronous TRANSFER_TO_HOST completes,
// so QEMU flushes the uploaded pixels instead of the resource's old contents.
// DxgkDdiRender rejects this command from user mode.
#define VIOGPU_CMD_FLUSH_FIXED_PRIMARY 0x8

// #define VIOGPU_EXECBUF_FENCE_FD_IN  0x01
// #define VIOGPU_EXECBUF_FENCE_FD_OUT 0x02
#define VIOGPU_EXECBUF_RING_IDX     0x04
#define VIOGPU_EXECBUF_VIRGL        0x08
// #define VIOGPU_EXECBUF_FLAGS        (VIOGPU_EXECBUF_FENCE_FD_IN | VIOGPU_EXECBUF_FENCE_FD_OUT | VIOGPU_EXECBUF_RING_IDX)

#pragma pack(1)
typedef struct _VIOGPU_COMMAND_HDR
{
    UINT type;
    UINT size;
    UINT flags;
    UINT ring_idx;
} VIOGPU_COMMAND_HDR;
#pragma pack()

// Never use a pointer-sized HANDLE in this packet.  A 32-bit WoW64 UMD must
// have the same byte stream as native x64, and the KMD must not retain the
// untrusted handle after DxgkDdiRender returns.
#pragma pack(1)
typedef struct _VIOGPU_SIGNAL_EVENT_CMD
{
    VIOGPU_UM_HANDLE Event;
} VIOGPU_SIGNAL_EVENT_CMD;
#pragma pack()

static_assert(sizeof(VIOGPU_COMMAND_HDR) == 16, "VIOGPU_COMMAND_HDR layout");
static_assert(sizeof(VIOGPU_SIGNAL_EVENT_CMD) == 8, "VIOGPU_SIGNAL_EVENT_CMD layout");
static_assert(offsetof(VIOGPU_SIGNAL_EVENT_CMD, Event) == 0, "VIOGPU_SIGNAL_EVENT_CMD.Event");

#pragma pack(1)
typedef struct _VIOGPU_COPY_FIXED_PRIMARY_CMD
{
    UINT SourceAllocationIndex;
    UINT DestinationAllocationIndex;
    LONG SourceDeltaX;
    LONG SourceDeltaY;
    UINT RectCount;
    // RectCount RECT records immediately follow this fixed header.
} VIOGPU_COPY_FIXED_PRIMARY_CMD;
#pragma pack()

static_assert(sizeof(VIOGPU_COPY_FIXED_PRIMARY_CMD) == 20,
              "VIOGPU_COPY_FIXED_PRIMARY_CMD layout");

#pragma pack(1)
typedef struct _VIOGPU_FLUSH_FIXED_PRIMARY_CMD
{
    UINT DestinationAllocationIndex;
} VIOGPU_FLUSH_FIXED_PRIMARY_CMD;
#pragma pack()

static_assert(sizeof(VIOGPU_FLUSH_FIXED_PRIMARY_CMD) == 4,
              "VIOGPU_FLUSH_FIXED_PRIMARY_CMD layout");

#pragma pack(1)
typedef struct _VIOGPU_TRANSFER_CMD
{
    ULONG res_id;

    VIOGPU_BOX box;

    ULONGLONG offset;
    ULONG level;
    ULONG stride;
    ULONG layer_stride;
} VIOGPU_TRANSFER_CMD;
#pragma pack()

#pragma pack(1)
typedef struct _VIOGPU_BEGIN_UM_BLIT_CMD
{
    RECT src, dst;
} VIOGPU_BEGIN_UM_BLIT_CMD;
#pragma pack()

#define BASE_NAMED_OBJECTS    L"\\BaseNamedObjects\\"
#define GLOBAL_OBJECTS        L"Global\\"
#define RESOLUTION_EVENT_NAME L"VioGpuResolutionEvent"
