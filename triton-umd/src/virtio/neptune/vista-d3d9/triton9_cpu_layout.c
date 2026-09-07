/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 */

#include "triton9_cpu_layout.h"

#include <string.h>

static int
triton9CpuCheckedMul(size_t a, size_t b, size_t *result)
{
    if (!result || (b && a > SIZE_MAX / b))
        return 0;
    *result = a * b;
    return 1;
}

static int
triton9CpuCheckedAdd(size_t a, size_t b, size_t *result)
{
    if (!result || a > SIZE_MAX - b)
        return 0;
    *result = a + b;
    return 1;
}

int
triton9CpuBackingInit(TRITON9_CPU_BACKING *backing, void *runtimeData,
                      int systemMemory)
{
    if (!backing || (systemMemory && !runtimeData))
        return 0;
    backing->data = systemMemory ? (uint8_t *)runtimeData : NULL;
    backing->driverOwned = systemMemory ? 0 : 1;
    return 1;
}

int
triton9CpuLayoutLinear(TRITON9_CPU_LAYOUT *layout, size_t byteCount)
{
    if (!layout || !byteCount)
        return 0;
    layout->rowBytes = byteCount;
    layout->rowPitch = byteCount;
    layout->rowCount = 1;
    layout->slicePitch = byteCount;
    layout->dataSize = byteCount;
    return 1;
}

int
triton9CpuLayout2D(TRITON9_CPU_LAYOUT *layout, uint32_t width,
                   uint32_t height, uint32_t bytesPerPixel,
                   size_t suppliedRowPitch, size_t suppliedSlicePitch)
{
    size_t rowBytes;
    size_t rowPitch;
    size_t tightSlice;
    size_t lastRowOffset;
    size_t minimumSize;
    size_t slicePitch;

    if (!layout || !width || !height || !bytesPerPixel ||
        !triton9CpuCheckedMul(width, bytesPerPixel, &rowBytes))
        return 0;
    rowPitch = suppliedRowPitch ? suppliedRowPitch : rowBytes;
    if (rowPitch < rowBytes ||
        !triton9CpuCheckedMul(rowPitch, height, &tightSlice) ||
        !triton9CpuCheckedMul(rowPitch, height - 1u, &lastRowOffset) ||
        !triton9CpuCheckedAdd(lastRowOffset, rowBytes, &minimumSize))
        return 0;
    slicePitch = suppliedSlicePitch ? suppliedSlicePitch : tightSlice;
    if (slicePitch < minimumSize)
        return 0;

    layout->rowBytes = rowBytes;
    layout->rowPitch = rowPitch;
    layout->rowCount = height;
    layout->slicePitch = slicePitch;
    layout->dataSize = slicePitch;
    return 1;
}

int
triton9CpuRegionFits(const TRITON9_CPU_LAYOUT *layout,
                     size_t xBytes, size_t y, size_t rowBytes,
                     size_t rowCount)
{
    size_t firstOffset;
    size_t lastOffset;
    size_t end;

    if (!layout || !layout->rowBytes || !layout->rowPitch ||
        !layout->rowCount || layout->rowBytes > layout->rowPitch ||
        !rowBytes || !rowCount || xBytes > layout->rowBytes ||
        rowBytes > layout->rowBytes - xBytes || y >= layout->rowCount ||
        rowCount > layout->rowCount - y || y > SIZE_MAX / layout->rowPitch)
        return 0;
    firstOffset = y * layout->rowPitch;
    if (rowCount - 1u > SIZE_MAX / layout->rowPitch)
        return 0;
    lastOffset = (rowCount - 1u) * layout->rowPitch;
    if (!triton9CpuCheckedAdd(firstOffset, lastOffset, &lastOffset) ||
        !triton9CpuCheckedAdd(lastOffset, xBytes, &lastOffset) ||
        !triton9CpuCheckedAdd(lastOffset, rowBytes, &end))
        return 0;
    return end <= layout->dataSize;
}

int
triton9CpuDitherStateSupported(uint32_t enabled,
                               enum triton9_cpu_color_layout layout)
{
    if (enabled > 1u)
        return 0;
    if (!enabled)
        return 1;
    /* Triton currently exposes only these 8:8:8 color render targets.  The
     * D3D9 dither switch has no lower-bit target quantization to control. */
    return layout == TRITON9_CPU_COLOR_LAYOUT_BGRA8 ||
           layout == TRITON9_CPU_COLOR_LAYOUT_BGRX8;
}

void
triton9CpuCopyRows(void *destination, size_t destinationPitch,
                   const void *source, size_t sourcePitch,
                   size_t rowBytes, size_t rowCount)
{
    size_t row;

    for (row = 0; row < rowCount; ++row) {
        memcpy((uint8_t *)destination + row * destinationPitch,
               (const uint8_t *)source + row * sourcePitch, rowBytes);
    }
}
