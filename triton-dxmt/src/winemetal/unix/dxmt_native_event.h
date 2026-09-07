/*
 * Native (non-Wine) Win32-style event objects for DXMT.
 *
 * On a native macOS build there is no NT event a HANDLE could name, so the
 * embedder creates events through dxmt_event_create() and passes the
 * returned pointer wherever the D3D11 API takes an event HANDLE
 * (ID3D11Fence::SetEventOnCompletion).
 *
 * An event is a tiny tagged box referencing a refcounted core that owns a
 * kqueue fd (its single EVFILT_USER registration IS the signaled state) and
 * a lazily-created pipe whose readable state mirrors the trigger — the pipe
 * is what dxmt_event_dup_fd() hands out, because macOS refuses to pass
 * kqueue fds across processes (SCM_RIGHTS -> EINVAL).
 */

#ifndef DXMT_NATIVE_EVENT_H
#define DXMT_NATIVE_EVENT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DXMT_EVT_WAIT_INFINITE UINT64_MAX

typedef enum dxmt_evt_wait_status {
  DXMT_EVT_WAIT_SIGNALED = 0,
  DXMT_EVT_WAIT_TIMEOUT = 1,
  DXMT_EVT_WAIT_FAILED = -1,
} dxmt_evt_wait_status;

/* Public embedder API (exported from winemetal.dylib). */
void *dxmt_event_create(int manual_reset, int initial_state);
void dxmt_event_signal(void *handle);
void dxmt_event_clear(void *handle);
void dxmt_event_close(void *handle);
int dxmt_event_dup_fd(void *handle);
dxmt_evt_wait_status dxmt_event_wait(void *handle, uint64_t timeout_ns);

/* Internal: for completion callbacks that may outlive the HANDLE.  The
 * embedder can dxmt_event_close() an event while a Metal notify block
 * still holds it, so a block must not capture the HANDLE.  Instead it
 * takes a reference on the shared core up front and signal+releases
 * through that. */
struct dxmt_evt_core;
struct dxmt_evt_core *dxmt_evt_core_retain(void *handle);
void dxmt_evt_core_signal_release(struct dxmt_evt_core *core);

#ifdef __cplusplus
}
#endif

#endif /* DXMT_NATIVE_EVENT_H */
