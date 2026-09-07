/*
 * dxmt-native embedder API.
 *
 *  - dxmt_event_*: kqueue-backed Win32-style events whose dup_fd() view is
 *    a pipe fd that survives SCM_RIGHTS (see
 *    src/winemetal/unix/dxmt_native_event.c).
 *  - dxmt_shared_texture_handle: the POD that
 *    IDXGIResource::GetSharedHandle returns a pointer to and
 *    ID3D11Device::OpenSharedResource accepts (see
 *    src/d3d11/d3d11_texture_shared_native.cpp).
 */

#ifndef DXMT_NATIVE_H
#define DXMT_NATIVE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* == Win32-style events ==================================================== */

#define DXMT_WAIT_INFINITE UINT64_MAX

typedef enum dxmt_wait_status {
    DXMT_WAIT_SIGNALED = 0,
    DXMT_WAIT_TIMEOUT  = 1,
    DXMT_WAIT_FAILED   = -1, /* not an event handle */
} dxmt_wait_status;

/* Create a Win32-style event. Returns an opaque handle to pass wherever the
 * D3D API takes an event HANDLE (ID3D11Fence::SetEventOnCompletion). */
void* dxmt_event_create(int manual_reset, int initial_state);
void  dxmt_event_signal(void* handle);
void  dxmt_event_clear(void* handle);
void  dxmt_event_close(void* handle);
/* A NEW poll(2)-able fd mirroring the event's signaled state; survives
 * SCM_RIGHTS to another process. Caller closes it. */
int   dxmt_event_dup_fd(void* handle);
dxmt_wait_status dxmt_event_wait(void* handle, uint64_t timeout_ns);

/* == Private D3D11 depth/stencil rectangle clear ========================== */

#define DXMT_D3D11_CLEAR_DEPTH   0x1u
#define DXMT_D3D11_CLEAR_STENCIL 0x2u

/* rect_ltrb contains rect_count groups of four signed values:
 * left, top, right, and bottom.  The call waits for GPU completion. */
int32_t dxmt_d3d11_clear_depth_stencil_rects(
    void *context, void *depth_stencil_view, uint32_t clear_flags,
    float depth, uint8_t stencil, uint32_t rect_count,
    const int32_t *rect_ltrb);

/* == Cross-process shared textures ========================================= */

#define DXMT_SHARED_TEXTURE_MAGIC 0x58544D44u /* 'DMTX' */
#define DXMT_SHARED_HANDLE_VERSION 1u

typedef struct dxmt_shared_texture_handle {
    uint32_t magic;         /* DXMT_SHARED_TEXTURE_MAGIC */
    uint32_t version;       /* DXMT_SHARED_HANDLE_VERSION */
    int32_t  fd;            /* process-local; send via SCM_RIGHTS, then patch */
    uint32_t width, height;
    uint32_t dxgi_format;   /* DXGI_FORMAT */
    uint32_t mip_levels, array_size, sample_count;
    uint32_t bind_flags, misc_flags, cpu_access;
    uint64_t stride;        /* bytesPerRow, byte-exact for the consumer */
    uint64_t size;          /* logical stride*height (NOT page-padded) */
} dxmt_shared_texture_handle;

#ifdef __cplusplus
}
#endif

#endif /* DXMT_NATIVE_H */
