/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Cross-build common header: C11 threading/TLS shim for Win32 PE,
 * plus one-shot init, allocation, logging, capset id, and the Wine
 * unixlib bridge prototypes.
 */

#ifndef NPT_COMMON_H
#define NPT_COMMON_H

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <stdatomic.h>

#if defined(_MSC_VER)
#define NPT_TLS __declspec(thread)
#else
#define NPT_TLS __thread
#endif

#if defined(_WIN32)
#include <windows.h>
typedef CRITICAL_SECTION mtx_t;
enum { mtx_plain = 0 };
enum { thrd_success = 0, thrd_error = -1 };
static inline int mtx_init(mtx_t *m, int type) { (void)type; InitializeCriticalSection(m); return 0; }
static inline void mtx_destroy(mtx_t *m) { DeleteCriticalSection(m); }
static inline int mtx_lock(mtx_t *m) { EnterCriticalSection(m); return 0; }
static inline int mtx_unlock(mtx_t *m) { LeaveCriticalSection(m); return 0; }
static inline void thrd_yield(void) { SwitchToThread(); }
typedef CONDITION_VARIABLE cnd_t;
static inline int cnd_init(cnd_t *c) { InitializeConditionVariable(c); return thrd_success; }
static inline void cnd_destroy(cnd_t *c) { (void)c; }
static inline int cnd_signal(cnd_t *c) { WakeConditionVariable(c); return thrd_success; }
static inline int cnd_broadcast(cnd_t *c) { WakeAllConditionVariable(c); return thrd_success; }
static inline int cnd_wait(cnd_t *c, mtx_t *m) {
   return SleepConditionVariableCS(c, m, INFINITE) ? thrd_success : thrd_error;
}
/* FLS rather than TLS so the destructor fires on thread exit, matching
 * C11 tss_create(dtor). */
typedef DWORD tss_t;
typedef void (*tss_dtor_t)(void *);
static inline int tss_create(tss_t *key, tss_dtor_t dtor) {
   *key = FlsAlloc((PFLS_CALLBACK_FUNCTION)dtor);
   return (*key == FLS_OUT_OF_INDEXES) ? thrd_error : thrd_success;
}
static inline void *tss_get(tss_t key) { return FlsGetValue(key); }
static inline int tss_set(tss_t key, void *value) {
   return FlsSetValue(key, value) ? thrd_success : thrd_error;
}
static inline void tss_delete(tss_t key) { FlsFree(key); }
#define likely(x)   (x)
#define unlikely(x) (x)
static inline bool util_is_power_of_two_nonzero(unsigned v) { return v && !(v & (v - 1)); }
/* 1-based highest-set-bit, 0 for v==0. */
static inline unsigned util_last_bit(unsigned v) {
   unsigned long idx;
   return _BitScanReverse(&idx, v) ? (unsigned)idx + 1u : 0u;
}
/* MinGW's nanosleep is unreliable; round up to 1ms minimum. */
static inline void npt_relax_sleep_us(unsigned us) {
   Sleep(us < 1000u ? 1u : (us / 1000u));
}
#else
#include "c11/threads.h"
#include "util/macros.h"
#include "util/os_misc.h"
#include "util/u_math.h"
#include <time.h>
static inline void npt_relax_sleep_us(unsigned us) {
   const struct timespec ts = {
      .tv_sec = us / 1000000u,
      .tv_nsec = (us % 1000000u) * 1000u,
   };
   nanosleep(&ts, NULL);
}
#endif

#include "util/list.h"

#ifdef VIRTGPU_DRM_CAPSET_NEPTUNE
#define NPT_CAPSET_ID VIRTGPU_DRM_CAPSET_NEPTUNE
#else
#define NPT_CAPSET_ID 7
#endif

struct npt_capset {
   uint32_t wire_format_version;
   uint32_t pad[14];
};

#if defined(_WIN32)
void npt_log_impl(const char *fmt, ...);
#define npt_log(fmt, ...) npt_log_impl("neptune: " fmt "\n", ##__VA_ARGS__)
#define npt_debug(fmt, ...) npt_log_impl("neptune: " fmt "\n", ##__VA_ARGS__)
#else
#define npt_log(fmt, ...)                                                    \
   fprintf(stderr, "neptune: " fmt "\n", ##__VA_ARGS__)
#ifdef NDEBUG
#define npt_debug(fmt, ...) ((void)0)
#else
#define npt_debug(fmt, ...)                                                  \
   fprintf(stderr, "neptune: " fmt "\n", ##__VA_ARGS__)
#endif
#endif

static inline void *
npt_alloc(size_t size)
{
   void *ptr = calloc(1, size);
   if (!ptr)
      npt_log("out of memory");
   return ptr;
}

/*
 * One-time init via a 3-state atomic ladder (0=uninit, 1=in-progress,
 * 2=done).  MinGW PE doesn't ship <threads.h>, so we can't use call_once.
 * init_expr must not return out of the macro.
 */
#define NPT_CALL_ONCE(state, init_expr)                                      \
   do {                                                                      \
      int _npt_once_cur =                                                    \
         atomic_load_explicit(&(state), memory_order_acquire);               \
      if (_npt_once_cur != 2) {                                              \
         int _npt_once_expected = 0;                                         \
         if (_npt_once_cur == 0 &&                                           \
             atomic_compare_exchange_strong(&(state),                        \
                                            &_npt_once_expected, 1)) {       \
            init_expr;                                                       \
            atomic_store_explicit(&(state), 2, memory_order_release);        \
         } else {                                                            \
            while (atomic_load_explicit(&(state),                            \
                                        memory_order_acquire) != 2)          \
               thrd_yield();                                                 \
         }                                                                   \
      }                                                                      \
   } while (0)

typedef uint64_t npt_object_id;

#if defined(__WINE__)
#include <windows.h>
#include <winternl.h>
typedef UINT64 npt_unixlib_handle_t;
extern npt_unixlib_handle_t g_npt_unix_handle;
extern NTSTATUS (WINAPI *g_npt_unix_call_dispatcher)(
   npt_unixlib_handle_t, unsigned int, void *);
int npt_unixlib_ensure_init(void);
NTSTATUS npt_wine_unix_call(unsigned int code, void *args);
#endif

#endif /* NPT_COMMON_H */
