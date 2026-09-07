/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 */

#define TRITON9_CLEAR_CONTRACT_PORTABLE
#include "../triton9_output.c"

#include <math.h>
#include <stdio.h>

static unsigned test_count;
static unsigned failure_count;

#define EXPECT(name, condition)                                                \
    do {                                                                       \
        ++test_count;                                                          \
        if (!(condition)) {                                                    \
            ++failure_count;                                                   \
            fprintf(stderr, "FAIL %s\n", name);                               \
        }                                                                      \
    } while (0)

static BOOL
rectEquals(const RECT *rect, LONG left, LONG top, LONG right, LONG bottom)
{
    return rect->left == left && rect->top == top && rect->right == right &&
           rect->bottom == bottom;
}

int
main(void)
{
    const RECT bounds = { 0, 0, 100, 80 };
    const RECT viewport = { 10, 5, 90, 70 };
    const RECT scissor = { 20, 10, 70, 60 };
    RECT output[4];
    UINT count = UINT32_MAX;
    enum triton9_clear_contract_result result;
    float color[4];

    result = triton9NormalizeClearRects(0, 0, NULL, &viewport, TRUE,
                                        &scissor, &bounds, output, 4, &count);
    EXPECT("zero_no_compute_result", result == TRITON9_CLEAR_CONTRACT_OK);
    EXPECT("zero_no_compute_noop", count == 0);

    result = triton9NormalizeClearRects(
        D3DCLEAR_COMPUTERECTS, 0, NULL, &viewport, TRUE, &scissor, &bounds,
        output, 4, &count);
    EXPECT("zero_compute_result", result == TRITON9_CLEAR_CONTRACT_OK);
    EXPECT("zero_compute_viewport", count == 1 &&
           rectEquals(&output[0], 10, 5, 90, 70));

    {
        const RECT input = { 2, 3, 40, 50 };
        result = triton9NormalizeClearRects(0, 1, &input, &viewport, TRUE,
                                            &scissor, &bounds, output, 4,
                                            &count);
        EXPECT("positive_runtime_clipped_result",
               result == TRITON9_CLEAR_CONTRACT_OK);
        EXPECT("positive_runtime_clipped_preserved", count == 1 &&
               rectEquals(&output[0], 2, 3, 40, 50));
    }

    {
        const RECT input[] = {
            { -10, -10, 30, 30 },
            { 65, 55, 95, 75 },
            { 0, 0, 5, 5 },
        };
        result = triton9NormalizeClearRects(
            D3DCLEAR_COMPUTERECTS, 3, input, &viewport, TRUE, &scissor,
            &bounds, output, 4, &count);
        EXPECT("positive_compute_result",
               result == TRITON9_CLEAR_CONTRACT_OK);
        EXPECT("positive_compute_clipped", count == 2 &&
               rectEquals(&output[0], 20, 10, 30, 30) &&
               rectEquals(&output[1], 65, 55, 70, 60));
    }

    {
        const RECT input = { 5, 6, 15, 16 };
        result = triton9NormalizeClearRects(
            D3DCLEAR_COMPUTERECTS, 1, &input, &viewport, FALSE, &scissor,
            &bounds, output, 4, &count);
        EXPECT("disabled_scissor_result",
               result == TRITON9_CLEAR_CONTRACT_OK);
        EXPECT("disabled_scissor_ignored", count == 1 &&
               rectEquals(&output[0], 10, 6, 15, 16));
    }

    result = triton9NormalizeClearRects(
        D3DCLEAR_COMPUTERECTS, 1, NULL, &viewport, TRUE, &scissor, &bounds,
        output, 4, &count);
    EXPECT("positive_null_input",
           result == TRITON9_CLEAR_CONTRACT_INVALID_ARGUMENT);

    {
        const RECT inverted = { 20, 20, 10, 30 };
        result = triton9NormalizeClearRects(
            D3DCLEAR_COMPUTERECTS, 1, &inverted, &viewport, TRUE, &scissor,
            &bounds, output, 4, &count);
        EXPECT("inverted_input",
               result == TRITON9_CLEAR_CONTRACT_INVALID_ARGUMENT);
    }

    {
        const RECT outside = { -1, 0, 10, 10 };
        result = triton9NormalizeClearRects(0, 1, &outside, &viewport, FALSE,
                                            &scissor, &bounds, output, 4,
                                            &count);
        EXPECT("runtime_clipped_out_of_bounds",
               result == TRITON9_CLEAR_CONTRACT_INVALID_ARGUMENT);
    }

    result = triton9NormalizeClearRects(
        D3DCLEAR_COMPUTERECTS, 0, NULL, &viewport, FALSE, &scissor, &bounds,
        output, 0, &count);
    EXPECT("insufficient_capacity",
           result == TRITON9_CLEAR_CONTRACT_INSUFFICIENT_CAPACITY);

    {
        const RECT full = { 0, 0, 100, 80 };
        EXPECT("full_resource",
               triton9ClearRectsCoverResource(&full, 1, 100, 80));
        EXPECT("partial_resource",
               !triton9ClearRectsCoverResource(&viewport, 1, 100, 80));
    }

    triton9ConvertClearColor(0x7f112233u, color);
    EXPECT("color_red", fabsf(color[0] - 17.0f / 255.0f) < 0.000001f);
    EXPECT("color_green", fabsf(color[1] - 34.0f / 255.0f) < 0.000001f);
    EXPECT("color_blue", fabsf(color[2] - 51.0f / 255.0f) < 0.000001f);
    EXPECT("color_alpha", fabsf(color[3] - 127.0f / 255.0f) < 0.000001f);

    if (failure_count) {
        fprintf(stderr, "TRITON9_CLEAR_CONTRACT_TEST FAIL cases=%u failures=%u\n",
                test_count, failure_count);
        return 1;
    }
    printf("TRITON9_CLEAR_CONTRACT_TEST PASS cases=%u\n", test_count);
    return 0;
}
