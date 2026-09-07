/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Normalized shader-signature description shared by the D3D10/11 DDI DXBC
 * builder and the Vista D3D9 shader bridge.  Signature producers must fill
 * semanticName/index when they know it; the DXBC builder derives a stable
 * name for NULL names from systemValue.
 */

#ifndef TRITON_DXBC_SIGNATURE_H_INCLUDED
#define TRITON_DXBC_SIGNATURE_H_INCLUDED

#include <windows.h>

typedef struct TRITON_DXBC_SIGNATURE {
    const char *semanticName;
    UINT        semanticIndex;
    UINT        systemValue;
    UINT        registerIdx;
    BYTE        mask;
    BYTE        stream;
    UINT        componentType;
    UINT        minPrecision;
} TRITON_DXBC_SIGNATURE;

/* D3D10DDIARG_SIGNATURE_ENTRY (12 bytes) and D3D11_1's 20-byte extension
 * share their first 12 bytes.  The D3D9 bridge passes this normalized form
 * directly so it can retain D3D9 semantic names and indices. */
static inline BOOL
tritonDxbcSignatureFromDdi(const void *base, UINT stride, UINT index,
                           TRITON_DXBC_SIGNATURE *out)
{
    const BYTE *entry;

    if (!base || !out)
        return FALSE;
    entry = (const BYTE *)base + (SIZE_T)index * stride;
    ZeroMemory(out, sizeof(*out));
    if (stride == sizeof(*out)) {
        *out = *(const TRITON_DXBC_SIGNATURE *)entry;
        return TRUE;
    }
    if (stride != 12 && stride != 20)
        return FALSE;
    out->systemValue = *(const UINT *)(entry + 0);
    out->registerIdx = *(const UINT *)(entry + 4);
    out->mask = *(const BYTE *)(entry + 8);
    out->stream = stride >= 20 ? *(const BYTE *)(entry + 9) : 0;
    out->componentType = stride >= 20 ? *(const UINT *)(entry + 12) : 0;
    out->minPrecision = stride >= 20 ? *(const UINT *)(entry + 16) : 0;
    return TRUE;
}

#endif /* TRITON_DXBC_SIGNATURE_H_INCLUDED */
