/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Portable CPU-layout helpers for the Vista D3D9 UMD.  Keep this header free
 * of Windows types so the ownership and padded-pitch rules can be tested on
 * the build host without starting a guest.
 */

#ifndef TRITON9_CPU_LAYOUT_H
#define TRITON9_CPU_LAYOUT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct TRITON9_CPU_LAYOUT {
    size_t rowBytes;
    size_t rowPitch;
    size_t rowCount;
    size_t slicePitch;
    size_t dataSize;
} TRITON9_CPU_LAYOUT;

typedef struct TRITON9_CPU_BACKING {
    uint8_t *data;
    int driverOwned;
} TRITON9_CPU_BACKING;

enum triton9_cpu_color_layout {
    TRITON9_CPU_COLOR_LAYOUT_OTHER = 0,
    TRITON9_CPU_COLOR_LAYOUT_BGRA8,
    TRITON9_CPU_COLOR_LAYOUT_BGRX8,
};

int triton9CpuBackingInit(TRITON9_CPU_BACKING *backing, void *runtimeData,
                          int systemMemory);
int triton9CpuLayoutLinear(TRITON9_CPU_LAYOUT *layout, size_t byteCount);
int triton9CpuLayout2D(TRITON9_CPU_LAYOUT *layout, uint32_t width,
                      uint32_t height, uint32_t bytesPerPixel,
                      size_t suppliedRowPitch, size_t suppliedSlicePitch);
int triton9CpuRegionFits(const TRITON9_CPU_LAYOUT *layout,
                        size_t xBytes, size_t y, size_t rowBytes,
                        size_t rowCount);
int triton9CpuDitherStateSupported(uint32_t enabled,
                                   enum triton9_cpu_color_layout layout);
void triton9CpuCopyRows(void *destination, size_t destinationPitch,
                        const void *source, size_t sourcePitch,
                        size_t rowBytes, size_t rowCount);

#ifdef __cplusplus
}
#endif

#endif
