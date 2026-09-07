/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 */

#include "../triton9_cpu_layout.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int
testBackingOwnership(void)
{
    TRITON9_CPU_BACKING backing;
    uint8_t runtimeBuffer[32];

    if (!triton9CpuBackingInit(&backing, runtimeBuffer, 1) ||
        backing.data != runtimeBuffer || backing.driverOwned)
        return 1;
    backing.data[7] = 0x5a;
    if (runtimeBuffer[7] != 0x5a)
        return 2;
    if (!triton9CpuBackingInit(&backing, NULL, 0) || backing.data ||
        !backing.driverOwned)
        return 3;
    if (triton9CpuBackingInit(&backing, NULL, 1))
        return 4;
    return 0;
}

static int
testPartialPaddedCopy(void)
{
    enum { WIDTH = 5, HEIGHT = 3, BPP = 4, PITCH = 24, SIZE = 72 };
    TRITON9_CPU_LAYOUT layout;
    uint8_t source[SIZE];
    uint8_t destination[SIZE];
    size_t row;
    size_t byte;

    if (!triton9CpuLayout2D(&layout, WIDTH, HEIGHT, BPP, PITCH, SIZE) ||
        layout.rowCount != HEIGHT ||
        !triton9CpuRegionFits(&layout, BPP, 1, 2 * BPP, 2))
        return 1;
    for (byte = 0; byte < SIZE; ++byte)
        source[byte] = (uint8_t)(byte + 1u);
    memset(destination, 0xa5, sizeof(destination));
    triton9CpuCopyRows(destination + PITCH + BPP, PITCH,
                       source + PITCH + BPP, PITCH, 2 * BPP, 2);

    for (row = 0; row < HEIGHT; ++row) {
        for (byte = 0; byte < PITCH; ++byte) {
            int copied = row >= 1 && byte >= BPP && byte < 3 * BPP;
            uint8_t expected = copied ? source[row * PITCH + byte] : 0xa5;
            if (destination[row * PITCH + byte] != expected)
                return 2;
        }
    }
    return 0;
}

static int
testPaddedRuntimeSurface(void)
{
    enum { WIDTH = 3, HEIGHT = 2, BPP = 4, PITCH = 16, SIZE = 32 };
    TRITON9_CPU_LAYOUT layout;
    uint8_t source[SIZE];
    uint8_t destination[SIZE];
    size_t i;

    if (!triton9CpuLayout2D(&layout, WIDTH, HEIGHT, BPP, PITCH, 0) ||
        layout.rowBytes != WIDTH * BPP || layout.rowPitch != PITCH ||
        layout.rowCount != HEIGHT || layout.slicePitch != SIZE ||
        layout.dataSize != SIZE)
        return 1;
    memset(source, 0xcc, sizeof(source));
    memset(destination, 0xa5, sizeof(destination));
    for (i = 0; i < WIDTH * BPP; ++i) {
        source[i] = (uint8_t)(0x10u + i);
        source[PITCH + i] = (uint8_t)(0x40u + i);
    }
    triton9CpuCopyRows(destination, layout.rowPitch, source, PITCH,
                       layout.rowBytes, HEIGHT);
    if (memcmp(destination, source, WIDTH * BPP) ||
        memcmp(destination + PITCH, source + PITCH, WIDTH * BPP))
        return 2;
    for (i = WIDTH * BPP; i < PITCH; ++i) {
        if (destination[i] != 0xa5 || destination[PITCH + i] != 0xa5)
            return 3;
    }
    return 0;
}

static int
testRegionBounds(void)
{
    TRITON9_CPU_LAYOUT layout;

    if (!triton9CpuLayout2D(&layout, 8, 4, 4, 40, 0))
        return 1;
    if (!triton9CpuRegionFits(&layout, 4, 1, 16, 2))
        return 2;
    if (triton9CpuRegionFits(&layout, 28, 0, 16, 1) ||
        triton9CpuRegionFits(&layout, 0, 3, 4, 2) ||
        triton9CpuRegionFits(&layout, layout.rowBytes, 0, 1, 1))
        return 3;
    /* Extra slice padding is not an extra logical row. */
    if (!triton9CpuLayout2D(&layout, 2, 2, 4, 16, 64) ||
        triton9CpuRegionFits(&layout, 0, 2, 4, 1))
        return 4;
    return 0;
}

static int
testRejectedLayouts(void)
{
    TRITON9_CPU_LAYOUT layout;

    if (triton9CpuLayout2D(&layout, 8, 4, 4, 31, 0) ||
        triton9CpuLayout2D(&layout, 8, 4, 4, 40, 100) ||
        triton9CpuLayout2D(&layout, UINT32_MAX, UINT32_MAX, UINT32_MAX,
                           0, 0) ||
        triton9CpuLayoutLinear(&layout, 0))
        return 1;
    if (!triton9CpuLayoutLinear(&layout, 4096) ||
        layout.dataSize != 4096 || layout.rowPitch != 4096 ||
        layout.rowCount != 1)
        return 2;
    return 0;
}

static int
testDitherFormatDependency(void)
{
    if (!triton9CpuDitherStateSupported(
            0, TRITON9_CPU_COLOR_LAYOUT_OTHER) ||
        !triton9CpuDitherStateSupported(
            1, TRITON9_CPU_COLOR_LAYOUT_BGRA8) ||
        !triton9CpuDitherStateSupported(
            1, TRITON9_CPU_COLOR_LAYOUT_BGRX8))
        return 1;
    if (triton9CpuDitherStateSupported(
            1, TRITON9_CPU_COLOR_LAYOUT_OTHER) ||
        triton9CpuDitherStateSupported(
            2, TRITON9_CPU_COLOR_LAYOUT_BGRA8))
        return 2;
    return 0;
}

int
main(void)
{
    int result;

    result = testBackingOwnership();
    if (result) {
        fprintf(stderr, "backing ownership test failed: %d\n", result);
        return result;
    }
    result = testPaddedRuntimeSurface();
    if (result) {
        fprintf(stderr, "padded runtime surface test failed: %d\n", result);
        return 10 + result;
    }
    result = testRegionBounds();
    if (result) {
        fprintf(stderr, "region bounds test failed: %d\n", result);
        return 20 + result;
    }
    result = testPartialPaddedCopy();
    if (result) {
        fprintf(stderr, "partial padded copy test failed: %d\n", result);
        return 30 + result;
    }
    result = testRejectedLayouts();
    if (result) {
        fprintf(stderr, "layout rejection test failed: %d\n", result);
        return 40 + result;
    }
    result = testDitherFormatDependency();
    if (result) {
        fprintf(stderr, "dither format dependency test failed: %d\n", result);
        return 50 + result;
    }
    puts("triton9 CPU layout contract: PASS");
    return 0;
}
