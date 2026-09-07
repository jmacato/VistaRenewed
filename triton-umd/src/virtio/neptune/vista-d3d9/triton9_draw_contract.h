/*
 * Vista D3D9 draw-range arithmetic shared by the UMD and native tests.
 * Keep this header free of Windows types so the contract can run on the host.
 */

#ifndef TRITON9_DRAW_CONTRACT_H
#define TRITON9_DRAW_CONTRACT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum triton9_draw_primitive_type {
    TRITON9_DRAW_POINT_LIST = 1,
    TRITON9_DRAW_LINE_LIST = 2,
    TRITON9_DRAW_LINE_STRIP = 3,
    TRITON9_DRAW_TRIANGLE_LIST = 4,
    TRITON9_DRAW_TRIANGLE_STRIP = 5,
    TRITON9_DRAW_TRIANGLE_FAN = 6,
};

/* Return the number of source elements and D3D11 draw elements. A fan has
 * primitive_count + 2 source elements and 3 * primitive_count draw indices. */
int triton9_draw_primitive_counts(uint32_t primitive_type,
                                  uint32_t primitive_count,
                                  uint32_t *source_count,
                                  uint32_t *draw_count,
                                  int *requires_expansion);

/* Size of a prefix upload that preserves the D3D9 draw's original first
 * element.  Returns zero if the range is empty or cannot fit in UINT32. */
int triton9_draw_um_prefix_bytes(uint32_t first_element,
                                 uint32_t element_count,
                                 uint32_t stride,
                                 uint32_t *bytes);

/* Resolve DrawIndexedPrimitive's signed base vertex and declared range. */
int triton9_draw_indexed_vertex_range(int32_t base_vertex,
                                      uint32_t min_index,
                                      uint32_t vertex_count,
                                      uint32_t *first_vertex);

/* Resolve DrawIndexedPrimitive2's byte-based stream-zero window. */
int triton9_draw2_vertex_window(int32_t base_vertex_offset,
                                uint32_t min_index,
                                uint32_t vertex_count,
                                uint32_t stride,
                                uint64_t *first_byte,
                                uint32_t *byte_count);

/* Validate the caller-provided indices against [min_index, min_index +
 * vertex_count), subtract min_index, and write a zero-based index stream. */
int triton9_draw2_normalize_indices(const void *source,
                                    uint32_t index_stride,
                                    uint32_t index_count,
                                    uint32_t min_index,
                                    uint32_t vertex_count,
                                    void *destination);

/* Validate an index stream without changing its representation. */
int triton9_draw_validate_indices(const void *source,
                                  uint32_t index_stride,
                                  uint32_t index_count,
                                  uint32_t min_index,
                                  uint32_t vertex_count);

/* Expand a triangle fan into a 32-bit triangle-list index stream. Source can
 * be NULL for a non-indexed fan. In that case, first_index is the fan base. */
int triton9_draw_expand_fan(const void *source,
                            uint32_t index_stride,
                            uint32_t first_index,
                            uint32_t primitive_count,
                            uint32_t *destination,
                            uint32_t destination_count);

#ifdef __cplusplus
}
#endif

#endif
