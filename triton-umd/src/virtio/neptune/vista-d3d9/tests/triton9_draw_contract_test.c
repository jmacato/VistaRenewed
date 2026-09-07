/* SPDX-License-Identifier: MIT */

#include "../triton9_draw_contract.h"

#include <stdint.h>
#include <stdio.h>

#define CHECK(expr) do {                                                     \
    if (!(expr)) {                                                           \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr);             \
        return 1;                                                            \
    }                                                                        \
} while (0)

int
main(void)
{
    uint32_t bytes = 0;
    uint32_t first = 0;
    uint64_t first_byte = 0;
    const uint16_t source16[] = { 8, 10, 9, 8 };
    uint16_t normalized16[4] = {};
    const uint32_t source32[] = { 100000, 100002, 100001 };
    uint32_t normalized32[3] = {};
    uint32_t source_count = 0;
    uint32_t draw_count = 0;
    int expand = 0;
    uint32_t fan[9] = {};
    const uint16_t indexed_fan[] = { 8, 10, 9, 11 };

    CHECK(triton9_draw_primitive_counts(TRITON9_DRAW_TRIANGLE_LIST, 2,
                                        &source_count, &draw_count, &expand));
    CHECK(source_count == 6 && draw_count == 6 && !expand);
    CHECK(triton9_draw_primitive_counts(TRITON9_DRAW_POINT_LIST, 7,
                                        &source_count, &draw_count, &expand));
    CHECK(source_count == 7 && draw_count == 7 && !expand);
    CHECK(triton9_draw_primitive_counts(TRITON9_DRAW_LINE_LIST, 3,
                                        &source_count, &draw_count, &expand));
    CHECK(source_count == 6 && draw_count == 6 && !expand);
    CHECK(triton9_draw_primitive_counts(TRITON9_DRAW_LINE_STRIP, 3,
                                        &source_count, &draw_count, &expand));
    CHECK(source_count == 4 && draw_count == 4 && !expand);
    CHECK(triton9_draw_primitive_counts(TRITON9_DRAW_TRIANGLE_STRIP, 3,
                                        &source_count, &draw_count, &expand));
    CHECK(source_count == 5 && draw_count == 5 && !expand);
    CHECK(triton9_draw_primitive_counts(TRITON9_DRAW_TRIANGLE_FAN, 3,
                                        &source_count, &draw_count, &expand));
    CHECK(source_count == 5 && draw_count == 9 && expand);
    CHECK(!triton9_draw_primitive_counts(TRITON9_DRAW_TRIANGLE_LIST,
                                         UINT32_MAX, &source_count,
                                         &draw_count, &expand));
    CHECK(!triton9_draw_primitive_counts(0, 1, &source_count, &draw_count,
                                         &expand));

    CHECK(triton9_draw_um_prefix_bytes(0, 3, 16, &bytes) && bytes == 48);
    CHECK(triton9_draw_um_prefix_bytes(7, 3, 16, &bytes) && bytes == 160);
    CHECK(!triton9_draw_um_prefix_bytes(UINT32_MAX, 1, 16, &bytes));
    CHECK(!triton9_draw_um_prefix_bytes(0, 0, 16, &bytes));

    CHECK(triton9_draw_indexed_vertex_range(-8, 8, 3, &first) && first == 0);
    CHECK(triton9_draw_indexed_vertex_range(4, 8, 3, &first) && first == 12);
    CHECK(!triton9_draw_indexed_vertex_range(-9, 8, 3, &first));
    CHECK(!triton9_draw_indexed_vertex_range(0, UINT32_MAX, 2, &first));

    CHECK(triton9_draw2_vertex_window(-32, 2, 3, 16, &first_byte, &bytes));
    CHECK(first_byte == 0 && bytes == 48);
    CHECK(triton9_draw2_vertex_window(4, 2, 3, 16, &first_byte, &bytes));
    CHECK(first_byte == 36 && bytes == 48);
    CHECK(!triton9_draw2_vertex_window(-33, 2, 3, 16, &first_byte, &bytes));

    CHECK(triton9_draw2_normalize_indices(source16, 2, 4, 8, 3,
                                          normalized16));
    CHECK(normalized16[0] == 0 && normalized16[1] == 2 &&
          normalized16[2] == 1 && normalized16[3] == 0);
    CHECK(triton9_draw2_normalize_indices(source32, 4, 3, 100000, 3,
                                          normalized32));
    CHECK(normalized32[0] == 0 && normalized32[1] == 2 &&
          normalized32[2] == 1);
    CHECK(!triton9_draw2_normalize_indices(source16, 2, 4, 9, 2,
                                           normalized16));
    CHECK(!triton9_draw2_normalize_indices(source16, 3, 4, 8, 3,
                                           normalized16));

    CHECK(triton9_draw_validate_indices(source16, 2, 4, 8, 3));
    CHECK(!triton9_draw_validate_indices(source16, 2, 4, 9, 2));
    CHECK(triton9_draw_expand_fan(NULL, 0, 7, 3, fan, 9));
    CHECK(fan[0] == 7 && fan[1] == 8 && fan[2] == 9);
    CHECK(fan[3] == 7 && fan[4] == 9 && fan[5] == 10);
    CHECK(fan[6] == 7 && fan[7] == 10 && fan[8] == 11);
    CHECK(triton9_draw_expand_fan(indexed_fan, 2, 0, 2, fan, 9));
    CHECK(fan[0] == 8 && fan[1] == 10 && fan[2] == 9);
    CHECK(fan[3] == 8 && fan[4] == 9 && fan[5] == 11);
    CHECK(!triton9_draw_expand_fan(NULL, 0, UINT32_MAX, 1, fan, 9));

    puts("triton9 draw contract: PASS");
    return 0;
}
