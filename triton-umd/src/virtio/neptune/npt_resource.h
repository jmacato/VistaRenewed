/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Resource wrappers for ID3D11Buffer and ID3D11Texture{1,2,3}D, plus
 * the per-resource Map shadow-staging rename ring.  Most COM interfaces
 * use a plain npt_com_base; buffer and texture families need aux state
 * for staging shmem.
 *
 * Map staging: each resource owns a rename ring with up to
 * NPT_D3D_MAP_SLOT_MAX slots.  Slot 0 is allocated eagerly on first
 * ensure_shmem (sync MAP_RESOURCE always lands on slot 0); additional
 * slots grow lazily when the wrap-around slot is still in flight.  At
 * the cap, rotate falls back to blocking on pending_seqno.  Slots
 * sub-allocate from the per-device map_pool rather than getting one
 * shmem each, which scales linearly with N on the host.
 */

#ifndef NPT_RESOURCE_H
#define NPT_RESOURCE_H

#include "npt_com.h"
#include "npt_renderer.h"

struct npt_d3d11_buffer;
struct npt_d3d11_texture;

/* ==========================================================================
 * Map shadow-staging rename ring
 * ========================================================================== */

#define NPT_D3D_MAP_SLOT_INIT 1u
#define NPT_D3D_MAP_SLOT_MAX  8u

struct npt_d3d_map_slot {
   /* Ref into the per-device map_pool's current shmem (NULL until
    * lazily allocated).  Held until slot release / ring fini so the
    * pool's old shmem stays alive across pool growth. */
   struct npt_renderer_shmem *shmem;
   /* Mirrors shmem->res_id; 0 if shmem == NULL. */
   uint32_t shmem_res_id;
   /* Byte offset of this slot within shmem (slots cohabit a pool). */
   uint32_t offset;
   /* Next rotate must wait until host head passes this. */
   uint32_t pending_seqno;
   /* Ring the Unmap was submitted on; NULL = dev->ring. */
   struct npt_ring *pending_ring;
   bool in_flight;
};

struct npt_d3d_map_ring {
   struct npt_com_base *com;
   uint32_t aligned_slot_size;
   uint32_t active_count;
   uint32_t current_slot;
   bool     is_mapped;
   uint32_t last_map_access_flags;
   struct npt_d3d_map_slot slots[NPT_D3D_MAP_SLOT_MAX];
};

/* Slot size depends on per-resource sizing (byte_width / row_pitch *
 * h * d), so the alloc is left to the caller's ensure_map_shmem. */
void npt_d3d_map_ring_init(struct npt_d3d_map_ring *r,
                           struct npt_com_base *com);

/* Records aligned_slot_size and ensures NPT_D3D_MAP_SLOT_INIT slots
 * are allocated.  Idempotent on the same size; logs and rejects size
 * drift (buffer/texture descs are immutable). */
bool npt_d3d_map_ring_alloc_shmem(struct npt_d3d_map_ring *r,
                                  uint32_t aligned_slot_size);

/* Drains in-flight slots, drops shmem refs, zeroes the ring. */
void npt_d3d_map_ring_fini(struct npt_d3d_map_ring *r);

static inline uint32_t
npt_d3d_map_ring_slot_offset(const struct npt_d3d_map_ring *r, uint32_t slot)
{
   if (slot >= NPT_D3D_MAP_SLOT_MAX) return 0;
   return r->slots[slot].offset;
}

static inline void *
npt_d3d_map_ring_slot_ptr(const struct npt_d3d_map_ring *r, uint32_t slot)
{
   if (slot >= NPT_D3D_MAP_SLOT_MAX || !r->slots[slot].shmem) return NULL;
   return (uint8_t *)r->slots[slot].shmem->mmap_ptr + r->slots[slot].offset;
}

static inline uint32_t
npt_d3d_map_ring_slot_res_id(const struct npt_d3d_map_ring *r, uint32_t slot)
{
   if (slot >= NPT_D3D_MAP_SLOT_MAX) return 0;
   return r->slots[slot].shmem_res_id;
}

/* Try the next slot in the ring; if it's in flight, grow up to
 * NPT_D3D_MAP_SLOT_MAX before falling back to blocking on
 * pending_seqno. */
uint32_t npt_d3d_map_ring_rotate_slot(struct npt_d3d_map_ring *r);

void npt_d3d_map_ring_mark_slot_submitted(struct npt_d3d_map_ring *r,
                                          uint32_t slot, uint32_t seqno,
                                          struct npt_ring *ring);

/* ==========================================================================
 * ID3D11Buffer wrapper
 * ========================================================================== */

/* NULL on non-buffer resource (used by ctx Map/Unmap). */
struct npt_d3d11_buffer *npt_d3d11_buffer_cast(void *resource);

void npt_d3d11_buffer_set_byte_width(struct npt_d3d11_buffer *b, uint32_t bytes);
uint32_t npt_d3d11_buffer_get_byte_width(struct npt_d3d11_buffer *b);

bool npt_d3d11_buffer_ensure_map_shmem(struct npt_d3d11_buffer *b);
/* Slot 0's res_id (sync MAP_RESOURCE always lands on slot 0). */
uint32_t npt_d3d11_buffer_get_map_shmem_res_id(struct npt_d3d11_buffer *b);
/* Per-slot res_id for async Unmap on the rename ring. */
uint32_t npt_d3d11_buffer_get_slot_shmem_res_id(struct npt_d3d11_buffer *b,
                                                uint32_t slot);
bool npt_d3d11_buffer_get_is_mapped(struct npt_d3d11_buffer *b);
void npt_d3d11_buffer_set_is_mapped(struct npt_d3d11_buffer *b, bool mapped);

/* Always 0 — slots own their shmem and start at offset 0.  Kept for
 * Unmap call-site symmetry. */
uint32_t npt_d3d11_buffer_slot_offset(const struct npt_d3d11_buffer *b,
                                      uint32_t slot);
void *npt_d3d11_buffer_slot_ptr(const struct npt_d3d11_buffer *b,
                                uint32_t slot);
uint32_t npt_d3d11_buffer_get_current_slot(const struct npt_d3d11_buffer *b);
void npt_d3d11_buffer_set_current_slot(struct npt_d3d11_buffer *b,
                                       uint32_t slot);
uint32_t npt_d3d11_buffer_rotate_slot(struct npt_d3d11_buffer *b);
void npt_d3d11_buffer_mark_slot_submitted(struct npt_d3d11_buffer *b,
                                          uint32_t slot, uint32_t seqno,
                                          struct npt_ring *ring);

/* 0 = last Map went through MAP_RESOURCE so the host already
 * populated map_state; non-zero = host replays Map+memcpy+Unmap. */
void npt_d3d11_buffer_set_last_map_access_flags(struct npt_d3d11_buffer *b,
                                                uint32_t flags);
uint32_t npt_d3d11_buffer_get_last_map_access_flags(const struct npt_d3d11_buffer *b);

/* ==========================================================================
 * ID3D11Texture{1,2,3}D wrapper
 * ========================================================================== */

/* NULL on non-texture resource. */
struct npt_d3d11_texture *npt_d3d11_texture_cast(void *resource);

/*
 * Dimension-agnostic flattening of {1D,2D,2D1,3D,3D1}_DESC; unused
 * fields collapse to neutral values (e.g. 1D leaves height/depth at
 * 1, non-*1 tiers leave texture_layout at UNDEFINED).
 */
struct npt_d3d11_texture_desc {
   uint32_t width;
   uint32_t height;
   uint32_t depth;
   uint32_t mip_levels;
   uint32_t array_size;
   DXGI_FORMAT format;
   uint32_t sample_count;
   uint32_t sample_quality;
   uint32_t usage;
   uint32_t bind_flags;
   uint32_t cpu_access_flags;
   uint32_t misc_flags;
   D3D11_TEXTURE_LAYOUT texture_layout;
};

/* Must be called before any Map(); otherwise dimensions are 0 and
 * the cached bytes-per-pixel is 0. */
void npt_d3d11_texture_set_desc(struct npt_d3d11_texture *t,
                                const struct npt_d3d11_texture_desc *d);

void npt_d3d11_texture_fill_desc1d(const struct npt_d3d11_texture *t,
                                   D3D11_TEXTURE1D_DESC *out);
void npt_d3d11_texture_fill_desc2d(const struct npt_d3d11_texture *t,
                                   D3D11_TEXTURE2D_DESC *out);
void npt_d3d11_texture_fill_desc2d1(const struct npt_d3d11_texture *t,
                                    D3D11_TEXTURE2D_DESC1 *out);
void npt_d3d11_texture_fill_desc3d(const struct npt_d3d11_texture *t,
                                   D3D11_TEXTURE3D_DESC *out);
void npt_d3d11_texture_fill_desc3d1(const struct npt_d3d11_texture *t,
                                    D3D11_TEXTURE3D_DESC1 *out);

/* False for textures that arrived via swapchain GetBuffer /
 * OpenSharedResource; GetDesc falls back to the sync round-trip. */
bool npt_d3d11_texture_has_desc(const struct npt_d3d11_texture *t);

/* Mappable = USAGE_DYNAMIC + CPU_ACCESS_WRITE + known-bpp format. */
bool npt_d3d11_texture_is_mappable(const struct npt_d3d11_texture *t);

bool npt_d3d11_texture_ensure_map_shmem(struct npt_d3d11_texture *t);
/* Slot 0's res_id (sync MAP_RESOURCE always lands on slot 0). */
uint32_t npt_d3d11_texture_get_map_shmem_res_id(const struct npt_d3d11_texture *t);
/* Per-slot res_id for async Unmap on the rename ring. */
uint32_t npt_d3d11_texture_get_slot_shmem_res_id(const struct npt_d3d11_texture *t,
                                                 uint32_t slot);
/* Total bytes currently allocated across active slots (informational). */
uint32_t npt_d3d11_texture_get_shmem_size(const struct npt_d3d11_texture *t);
uint32_t npt_d3d11_texture_get_slot_size(const struct npt_d3d11_texture *t);
void *npt_d3d11_texture_shmem_ptr(const struct npt_d3d11_texture *t);

/* Sync Maps always land on slot 0; rename-ring fast path rotates. */
uint32_t npt_d3d11_texture_get_current_slot(const struct npt_d3d11_texture *t);
void npt_d3d11_texture_set_current_slot(struct npt_d3d11_texture *t,
                                        uint32_t slot);
uint32_t npt_d3d11_texture_slot_offset(const struct npt_d3d11_texture *t,
                                       uint32_t slot);
void *npt_d3d11_texture_slot_ptr(const struct npt_d3d11_texture *t,
                                 uint32_t slot);
uint32_t npt_d3d11_texture_rotate_slot(struct npt_d3d11_texture *t);
void npt_d3d11_texture_mark_slot_submitted(struct npt_d3d11_texture *t,
                                           uint32_t slot, uint32_t seqno,
                                           struct npt_ring *ring);

/* access_flags=0 => sync Map populated host map_state and the host
 * uses it; non-zero => host replays Map+memcpy+Unmap with these
 * flags (cached-pitch fast path). */
bool npt_d3d11_texture_get_is_mapped(const struct npt_d3d11_texture *t);
void npt_d3d11_texture_set_mapped_state(struct npt_d3d11_texture *t,
                                        uint32_t subresource,
                                        uint32_t row_pitch,
                                        uint32_t byte_size,
                                        uint32_t access_flags);
void npt_d3d11_texture_clear_mapped_state(struct npt_d3d11_texture *t);
uint32_t npt_d3d11_texture_get_last_map_subresource(const struct npt_d3d11_texture *t);
uint32_t npt_d3d11_texture_get_last_map_byte_size(const struct npt_d3d11_texture *t);
uint32_t npt_d3d11_texture_get_last_map_access_flags(const struct npt_d3d11_texture *t);

/* 0 = uncached: next Map must take the sync path to learn the
 * host's pitches.  Once cached, Map returns a shmem pointer with no
 * wire trip and Unmap fires async. */
uint32_t npt_d3d11_texture_get_cached_row_pitch(const struct npt_d3d11_texture *t);
uint32_t npt_d3d11_texture_get_cached_depth_pitch(const struct npt_d3d11_texture *t);
void npt_d3d11_texture_set_cached_pitches(struct npt_d3d11_texture *t,
                                          uint32_t row_pitch, uint32_t depth_pitch);

uint32_t npt_d3d11_texture_get_subresource_byte_size(const struct npt_d3d11_texture *t,
                                                     uint32_t subresource,
                                                     uint32_t row_pitch);

/* Returns the full-pitch footprint of a no-box UpdateSubresource and
 * stores the D3D-guaranteed readable extent of its source in
 * *out_copy_size; both 0 when the transfer cannot be sized. */
uint32_t npt_d3d11_texture_get_subresource_update_sizes(const struct npt_d3d11_texture *t,
                                                        uint32_t subresource,
                                                        uint32_t row_pitch,
                                                        uint32_t depth_pitch,
                                                        uint32_t *out_copy_size);

void npt_d3d11_texture_get_mip_dimensions(const struct npt_d3d11_texture *t,
                                          uint32_t subresource,
                                          uint32_t *out_height,
                                          uint32_t *out_depth);

/* 0 = unknown / unsupported format. */
uint32_t npt_d3d11_texture_get_bytes_per_pixel(const struct npt_d3d11_texture *t);
uint32_t npt_dxgi_format_bytes_per_pixel(DXGI_FORMAT fmt);

/* DXGI_FORMAT_UNKNOWN when set_desc has not run. */
DXGI_FORMAT npt_d3d11_texture_get_format(const struct npt_d3d11_texture *t);

/* Rows of memory spanned by texel_rows rows of texels: a row pitch addresses
 * one row of memory, and the block-compressed families pack four texel rows
 * into one row of 4x4 blocks.  Valid for a partial (box) region. */
uint32_t npt_dxgi_format_block_rows(DXGI_FORMAT fmt, uint32_t texel_rows);
uint32_t npt_dxgi_format_row_bytes(DXGI_FORMAT fmt, uint32_t texel_width);

/* Rows of memory in one whole 2D slice of a mip.  Adds the chroma-plane rows
 * that the planar YUV formats store after the luma plane, so this is valid
 * only for a whole subresource. */
uint32_t npt_dxgi_format_subresource_rows(DXGI_FORMAT fmt, uint32_t height);

/* ==========================================================================
 * Aux blob storage
 * ========================================================================== */

/* Per-buffer storage attached to the COM wrapper (npt_com_register_family
 * gets sizeof(struct npt_d3d11_buffer_aux); aux_init wires destroy in). */
struct npt_d3d11_buffer_aux {
   struct npt_com_base *com;
   uint32_t byte_width;
   struct npt_d3d_map_ring map_ring;
};

void npt_d3d11_buffer_aux_init(struct npt_com_base *com,
                               struct npt_device *dev, uint64_t host_id);

/* Per-texture storage.  Cached desc + per-Map pitch bookkeeping +
 * the rename ring shared with the buffer aux. */
struct npt_d3d11_texture_aux {
   struct npt_com_base *com;
   uint32_t width, height, depth;
   uint32_t mip_levels, array_size;
   DXGI_FORMAT format;
   uint32_t bytes_per_pixel;
   uint32_t usage, cpu_access_flags, bind_flags, misc_flags;
   uint32_t sample_count, sample_quality;
   D3D11_TEXTURE_LAYOUT texture_layout;
   /* Pitches from the first sync MAP_RESOURCE; reused by the
    * WRITE_DISCARD / WRITE_NO_OVERWRITE fast path. */
   uint32_t cached_row_pitch, cached_depth_pitch;
   /* Per-Map state for the matching Unmap. */
   uint32_t last_map_subresource;
   uint32_t last_map_row_pitch;
   uint32_t last_map_byte_size;
   struct npt_d3d_map_ring map_ring;
};

void npt_d3d11_texture_aux_init(struct npt_com_base *com,
                                struct npt_device *dev, uint64_t host_id);

#endif /* NPT_RESOURCE_H */
