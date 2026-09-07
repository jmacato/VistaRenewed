/* SPDX-License-Identifier: MIT */

#include "triton9_draw_contract.h"

#include <limits.h>
#include <string.h>

int
triton9_draw_primitive_counts(uint32_t primitive_type,
                              uint32_t primitive_count,
                              uint32_t *source_count,
                              uint32_t *draw_count,
                              int *requires_expansion)
{
    uint64_t source;
    uint64_t draw;
    int expansion = 0;

    if (!source_count || !draw_count || !requires_expansion)
        return 0;
    switch (primitive_type) {
    case TRITON9_DRAW_POINT_LIST:
        source = draw = primitive_count;
        break;
    case TRITON9_DRAW_LINE_LIST:
        source = draw = (uint64_t)primitive_count * 2u;
        break;
    case TRITON9_DRAW_LINE_STRIP:
        source = draw = (uint64_t)primitive_count + 1u;
        break;
    case TRITON9_DRAW_TRIANGLE_LIST:
        source = draw = (uint64_t)primitive_count * 3u;
        break;
    case TRITON9_DRAW_TRIANGLE_STRIP:
        source = draw = (uint64_t)primitive_count + 2u;
        break;
    case TRITON9_DRAW_TRIANGLE_FAN:
        source = (uint64_t)primitive_count + 2u;
        draw = (uint64_t)primitive_count * 3u;
        expansion = 1;
        break;
    default:
        return 0;
    }
    if (source > UINT32_MAX || draw > UINT32_MAX)
        return 0;
    *source_count = (uint32_t)source;
    *draw_count = (uint32_t)draw;
    *requires_expansion = expansion;
    return 1;
}

int
triton9_draw_um_prefix_bytes(uint32_t first_element,
                             uint32_t element_count,
                             uint32_t stride,
                             uint32_t *bytes)
{
    uint64_t end;

    if (!bytes || !element_count || !stride)
        return 0;
    end = ((uint64_t)first_element + element_count) * stride;
    if (!end || end > UINT32_MAX)
        return 0;
    *bytes = (uint32_t)end;
    return 1;
}

int
triton9_draw_indexed_vertex_range(int32_t base_vertex,
                                  uint32_t min_index,
                                  uint32_t vertex_count,
                                  uint32_t *first_vertex)
{
    int64_t first;

    if (!first_vertex || !vertex_count)
        return 0;
    first = (int64_t)base_vertex + min_index;
    if (first < 0 || first > UINT32_MAX ||
        vertex_count > UINT32_MAX - (uint32_t)first)
        return 0;
    *first_vertex = (uint32_t)first;
    return 1;
}

int
triton9_draw2_vertex_window(int32_t base_vertex_offset,
                            uint32_t min_index,
                            uint32_t vertex_count,
                            uint32_t stride,
                            uint64_t *first_byte,
                            uint32_t *byte_count)
{
    uint64_t index_offset;
    uint64_t start;
    uint64_t size;

    if (!first_byte || !byte_count || !vertex_count || !stride)
        return 0;
    index_offset = (uint64_t)min_index * stride;
    if (base_vertex_offset < 0) {
        uint64_t magnitude = (uint64_t)(-(int64_t)base_vertex_offset);

        if (index_offset < magnitude)
            return 0;
        start = index_offset - magnitude;
    } else {
        if (index_offset > UINT64_MAX - (uint32_t)base_vertex_offset)
            return 0;
        start = index_offset + (uint32_t)base_vertex_offset;
    }
    size = (uint64_t)vertex_count * stride;
    if (!size || size > UINT32_MAX || start > UINT64_MAX - size)
        return 0;
    *first_byte = start;
    *byte_count = (uint32_t)size;
    return 1;
}

int
triton9_draw2_normalize_indices(const void *source,
                                uint32_t index_stride,
                                uint32_t index_count,
                                uint32_t min_index,
                                uint32_t vertex_count,
                                void *destination)
{
    const uint8_t *src = (const uint8_t *)source;
    uint8_t *dst = (uint8_t *)destination;
    uint64_t limit;
    uint32_t index;

    if (!source || !destination || !index_count || !vertex_count ||
        (index_stride != 2 && index_stride != 4))
        return 0;
    limit = (uint64_t)min_index + vertex_count;
    if (limit > (uint64_t)UINT32_MAX + 1u)
        return 0;

    for (uint32_t i = 0; i < index_count; ++i) {
        if (index_stride == 2) {
            uint16_t value;

            memcpy(&value, src + (size_t)i * index_stride, sizeof(value));
            index = value;
        } else {
            memcpy(&index, src + (size_t)i * index_stride, sizeof(index));
        }
        if (index < min_index || (uint64_t)index >= limit)
            return 0;
        index -= min_index;
        if (index_stride == 2) {
            uint16_t value = (uint16_t)index;

            if (index != value)
                return 0;
            memcpy(dst + (size_t)i * index_stride, &value, sizeof(value));
        } else {
            memcpy(dst + (size_t)i * index_stride, &index, sizeof(index));
        }
    }
    return 1;
}

int
triton9_draw_validate_indices(const void *source,
                              uint32_t index_stride,
                              uint32_t index_count,
                              uint32_t min_index,
                              uint32_t vertex_count)
{
    const uint8_t *src = (const uint8_t *)source;
    uint64_t limit;

    if (!source || !index_count || !vertex_count ||
        (index_stride != 2 && index_stride != 4))
        return 0;
    limit = (uint64_t)min_index + vertex_count;
    if (limit > (uint64_t)UINT32_MAX + 1u)
        return 0;
    for (uint32_t i = 0; i < index_count; ++i) {
        uint32_t index;

        if (index_stride == 2) {
            uint16_t value;

            memcpy(&value, src + (size_t)i * index_stride, sizeof(value));
            index = value;
        } else {
            memcpy(&index, src + (size_t)i * index_stride, sizeof(index));
        }
        if (index < min_index || (uint64_t)index >= limit)
            return 0;
    }
    return 1;
}

static uint32_t
triton9_draw_read_index(const void *source, uint32_t stride, uint32_t index)
{
    const uint8_t *bytes = (const uint8_t *)source + (size_t)index * stride;
    uint32_t value;

    if (stride == 2) {
        uint16_t value16;

        memcpy(&value16, bytes, sizeof(value16));
        return value16;
    }
    memcpy(&value, bytes, sizeof(value));
    return value;
}

int
triton9_draw_expand_fan(const void *source,
                        uint32_t index_stride,
                        uint32_t first_index,
                        uint32_t primitive_count,
                        uint32_t *destination,
                        uint32_t destination_count)
{
    uint64_t required = (uint64_t)primitive_count * 3u;

    if (!primitive_count || !destination || required > destination_count)
        return 0;
    if (source && index_stride != 2 && index_stride != 4)
        return 0;
    if (!source && first_index > UINT32_MAX - primitive_count - 1u)
        return 0;
    for (uint32_t triangle = 0; triangle < primitive_count; ++triangle) {
        destination[triangle * 3u] = source
            ? triton9_draw_read_index(source, index_stride, 0) : first_index;
        destination[triangle * 3u + 1u] = source
            ? triton9_draw_read_index(source, index_stride, triangle + 1u)
            : first_index + triangle + 1u;
        destination[triangle * 3u + 2u] = source
            ? triton9_draw_read_index(source, index_stride, triangle + 2u)
            : first_index + triangle + 2u;
    }
    return 1;
}
