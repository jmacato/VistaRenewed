/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Public interface for Triton's DXBC container helpers.  The D3D10/11 DDI
 * and the Vista D3D9 bridge use the same builder.
 */

#ifndef TRITON_DXBC_H_INCLUDED
#define TRITON_DXBC_H_INCLUDED

#include <windows.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void *tritonBuildDxbc(const UINT *tokens, SIZE_T byteCount,
                      const void *inputEntries, UINT inputCount,
                      const void *outputEntries, UINT outputCount,
                      const void *patchEntries, UINT patchCount,
                      UINT entryStride, SIZE_T *outputByteCount);

void *tritonBuildInputSigDxbc(const char *const *names,
                              const UINT *semanticIndices,
                              const UINT *registers,
                              UINT entryCount, SIZE_T *outputByteCount);

void *tritonReconcileVsInputSig(const void *dxbc, SIZE_T dxbcByteCount,
                                const unsigned char componentTypes[32],
                                SIZE_T *outputByteCount);

#ifdef __cplusplus
}
#endif

#endif /* TRITON_DXBC_H_INCLUDED */
