/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Guest command-stream interface used by the generated neptune-protocol
 * headers.  Must NEVER include any neptune-protocol/ header except
 * directx_types -- doing so would create an include cycle.
 */

#ifndef NPT_CS_H
#define NPT_CS_H

#include "npt_common.h"

/* enum values > INT_MAX trip -Wpedantic. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include "neptune-protocol/npt_protocol_directx_types.h"
#pragma GCC diagnostic pop

#define NPT_S_OK          ((HRESULT)0)
#define NPT_S_FALSE       ((HRESULT)1)
#define NPT_E_NOTIMPL     ((HRESULT)0x80004001)
#define NPT_E_NOINTERFACE ((HRESULT)0x80004002)
#define NPT_E_POINTER     ((HRESULT)0x80004003)
#define NPT_E_FAIL        ((HRESULT)0x80004005)
#define NPT_E_OUTOFMEMORY ((HRESULT)0x8007000E)
#define NPT_E_INVALIDARG  ((HRESULT)0x80070057)

/* DXGI-specific HRESULTs (from <dxgi.h>). */
#define NPT_DXGI_ERROR_INVALID_CALL ((HRESULT)0x887A0001)
#define NPT_DXGI_ERROR_NOT_FOUND    ((HRESULT)0x887A0002)
#define NPT_DXGI_ERROR_MORE_DATA    ((HRESULT)0x887A0003)
#define NPT_DXGI_ERROR_UNSUPPORTED  ((HRESULT)0x887A0004)
#define NPT_DXGI_ERROR_NOT_CURRENTLY_AVAILABLE ((HRESULT)0x887A0022)
/* Success-range status (high bit clear): a fullscreen/windowed transition
 * is already running on another thread. */
#define NPT_DXGI_STATUS_MODE_CHANGE_IN_PROGRESS ((HRESULT)0x087A0025)

#define NPT_SUCCEEDED(hr) ((HRESULT)(hr) >= 0)
#define NPT_FAILED(hr)    ((HRESULT)(hr) < 0)

/* npt_object_id is the host COM pointer; type tag goes unused on guest. */
typedef uint64_t npt_object_id;
typedef int npt_object_type;

struct npt_cs_encoder {
   uint8_t *cur;
   const uint8_t *end;
};

/*
 * Temp suballocator for the generated reply decoders (out-of-line
 * output arrays/structs).  List of malloc'd buffers; alloc bumps cur
 * in the current buffer; pool grows by appending; fini frees
 * everything.  Per-submission lifetime.
 */
struct npt_cs_decoder_temp_pool {
   uint8_t **buffers;
   uint32_t buffer_count;
   uint32_t buffer_max;
   size_t total_size;

   uint8_t *cur;
   const uint8_t *end;
};

struct npt_cs_decoder {
   const uint8_t *cur;
   const uint8_t *end;
   struct npt_cs_decoder_temp_pool temp_pool;
};

/* Caps total temp-pool size; guards against bogus reply length fields. */
#define NPT_CS_DECODER_TEMP_POOL_MAX_SIZE (1u * 1024 * 1024 * 1024)

static inline void
npt_cs_encoder_write(struct npt_cs_encoder *enc,
                     size_t size,
                     const void *val,
                     size_t val_size)
{
   assert(val_size <= size);
   if (unlikely(size > (size_t)(enc->end - enc->cur)))
      return;
   if (enc->cur != val)
      memcpy(enc->cur, val, val_size);
   enc->cur += size;
}

static inline void
npt_cs_encoder_set_fatal(const struct npt_cs_encoder *enc)
{
   (void)enc;
}

static inline void
npt_cs_decoder_read(struct npt_cs_decoder *dec,
                    size_t size,
                    void *val,
                    size_t val_size)
{
   assert(val_size <= size);
   if (unlikely(size > (size_t)(dec->end - dec->cur))) {
      memset(val, 0, val_size);
      return;
   }
   if (dec->cur != val)
      memcpy(val, dec->cur, val_size);
   dec->cur += size;
}

static inline void
npt_cs_decoder_peek(const struct npt_cs_decoder *dec,
                    size_t size,
                    void *val,
                    size_t val_size)
{
   assert(val_size <= size);
   if (unlikely(size > (size_t)(dec->end - dec->cur))) {
      memset(val, 0, val_size);
      return;
   }
   if (dec->cur != val)
      memcpy(val, dec->cur, val_size);
}

static inline void
npt_cs_decoder_set_fatal(const struct npt_cs_decoder *dec)
{
   (void)dec;
}

/* Hot path is inline; growth path lives in npt_cs.c. */
bool
npt_cs_decoder_alloc_temp_internal(struct npt_cs_decoder *dec, size_t size);

void
npt_cs_decoder_fini_temp_pool(struct npt_cs_decoder *dec);

static inline void *
npt_cs_decoder_alloc_temp(struct npt_cs_decoder *dec, size_t size)
{
   struct npt_cs_decoder_temp_pool *pool = &dec->temp_pool;

   if (unlikely(size > (size_t)(pool->end - pool->cur))) {
      if (!npt_cs_decoder_alloc_temp_internal(dec, size))
         return NULL;
   }

   const size_t aligned = (size + 7u) & ~(size_t)7u;
   assert(aligned <= (size_t)(pool->end - pool->cur));

   void *ptr = pool->cur;
   pool->cur += aligned;
   return ptr;
}

static inline bool
npt_size_mul_overflow(size_t a, size_t b, size_t *out)
{
#if defined(__GNUC__) || defined(__clang__)
   return __builtin_mul_overflow(a, b, out);
#else
   *out = a * b;
   return a != 0 && *out / a != b;
#endif
}

static inline void *
npt_cs_decoder_alloc_temp_array(struct npt_cs_decoder *dec,
                                size_t element_size,
                                size_t count)
{
   size_t total;
   if (unlikely(npt_size_mul_overflow(element_size, count, &total))) {
      npt_log("overflow in array allocation of %zu * %zu bytes",
              element_size, count);
      npt_cs_decoder_set_fatal(dec);
      return NULL;
   }
   return npt_cs_decoder_alloc_temp(dec, total);
}


struct npt_ring;
struct npt_renderer_shmem;

/*
 * Per-submission state, caller-owned (always stack).  For sync
 * commands the ring stashes a per-call reply window; reply_shmem
 * holds a ref dropped by npt_ring_free_command_reply.
 */
struct npt_ring_submit_command {
   void *cmd_data;
   size_t cmd_size;
   size_t reply_size;
   struct npt_cs_encoder enc;
   struct npt_cs_decoder dec;

   /* NULL for async commands. */
   struct npt_renderer_shmem *reply_shmem;
   uint32_t reply_offset;

   /* Reply is ready once ring head reaches this. */
   uint32_t seqno;

   /* Captured at submit; sync thunks free cmd_data before reply, so
    * re-reading would be UAF.  Only written when profiling is on. */
   uint32_t prof_cmd_type;
};

struct npt_cs_encoder *
npt_ring_submit_command_init(struct npt_ring *ring,
                             struct npt_ring_submit_command *submit,
                             void *cmd_data, size_t cmd_size,
                             size_t reply_size);

void
npt_ring_submit_command(struct npt_ring *ring,
                        struct npt_ring_submit_command *submit);

struct npt_cs_decoder *
npt_ring_get_command_reply(struct npt_ring *ring,
                           struct npt_ring_submit_command *submit);

void
npt_ring_free_command_reply(struct npt_ring *ring,
                            struct npt_ring_submit_command *submit);

struct npt_com_base;

/* Prefix of struct npt_com_base, letting the compiler apply the same
 * padding between lpVtbl and the 8-byte id that the full struct gets
 * (they differ on 32-bit).  Avoids including npt_object.h; the
 * static_asserts in npt_com.h pin the two layouts together. */
struct npt_com_head {
   const void **lpVtbl;
   npt_object_id id;
};

static inline npt_object_id
npt_object_get_id(const void *handle)
{
   if (!handle)
      return 0;
   return ((const struct npt_com_head *)handle)->id;
}

static inline void *
npt_object_from_id(npt_object_id id)
{
   return (void *)(uintptr_t)id;
}

/*
 * Win32 handles (HWND, HMONITOR, HANDLE) are opaque tokens; encode
 * and decode are verbatim uintptr_t casts.  Preserves KMT-vs-NT
 * handle-style bits in shared-resource HANDLEs.
 */
static inline npt_object_id
npt_win32_handle_get_id(const void *handle)
{
   return (npt_object_id)(uintptr_t)handle;
}

static inline void *
npt_win32_handle_from_id(npt_object_id id)
{
   return (void *)(uintptr_t)id;
}

#endif /* NPT_CS_H */
