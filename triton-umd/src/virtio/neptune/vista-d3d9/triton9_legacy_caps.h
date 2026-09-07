/* SPDX-License-Identifier: MIT
 * Layouts from the Vista d3dhal.h capability ABI. The legacy d3d.h and
 * modern d3d9types.h cannot share a translation unit, so keep the wire-only
 * structures here with private names. No COM interfaces are duplicated.
 */
#ifndef TRITON9_LEGACY_CAPS_H
#define TRITON9_LEGACY_CAPS_H

typedef struct {
    DWORD size, misc, raster, zCompare, srcBlend, dstBlend, alphaCompare;
    DWORD shade, texture, textureFilter, textureBlend, textureAddress;
    DWORD stippleWidth, stippleHeight;
} TRITON9_LEGACY_PRIM_CAPS;

typedef struct {
    DWORD size, flags, colorModel, devCaps;
    DWORD transformSize, transformCaps;
    BOOL clipping;
    DWORD lightingSize, lightingCaps, lightingModel, lights;
    TRITON9_LEGACY_PRIM_CAPS line, triangle;
    DWORD renderDepth, zDepth, maxBufferSize, maxVertexCount;
} TRITON9_LEGACY_DEVICE_CAPS;

typedef struct {
    DWORD size;
    TRITON9_LEGACY_DEVICE_CAPS hardware;
    DWORD vertices, clipVertices, textureFormats;
    void *formats;
} TRITON9_LEGACY_GLOBAL_CAPS;

typedef struct {
    DWORD size;
    DWORD minTextureWidth, maxTextureWidth, minTextureHeight, maxTextureHeight;
    DWORD minStippleWidth, maxStippleWidth, minStippleHeight, maxStippleHeight;
    DWORD maxTextureRepeat, maxTextureAspectRatio, maxAnisotropy;
    float guardBandLeft, guardBandTop, guardBandRight, guardBandBottom;
    float extentsAdjust;
    DWORD stencilCaps, fvfCaps, textureOpCaps;
    WORD maxTextureBlendStages, maxSimultaneousTextures;
    DWORD maxActiveLights;
    float maxVertexW;
    WORD maxUserClipPlanes, maxVertexBlendMatrices;
    DWORD vertexProcessingCaps;
    DWORD reserved[4];
} TRITON9_LEGACY_EXTENDED_CAPS;

_Static_assert(sizeof(TRITON9_LEGACY_PRIM_CAPS) == 56, "D3DPRIMCAPS ABI");
_Static_assert(sizeof(TRITON9_LEGACY_DEVICE_CAPS) == 172, "D3DDEVICEDESC_V1 ABI");
_Static_assert(sizeof(TRITON9_LEGACY_GLOBAL_CAPS) ==
               (sizeof(void *) == 8 ? 200 : 192), "D3DHAL_GLOBALDRIVERDATA ABI");
_Static_assert(sizeof(TRITON9_LEGACY_EXTENDED_CAPS) == 116,
               "D3DHAL_D3DEXTENDEDCAPS ABI");
#endif
