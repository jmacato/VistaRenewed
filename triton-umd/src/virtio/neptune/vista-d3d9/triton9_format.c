/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * The only D3D9 format authority.  Resource construction, lock pitch,
 * D3D9 caps and D3D11 view creation must use this table rather than grow
 * independent switches.  New entries stay out of this table until their
 * create/lock/render/blit coverage has been added to the Vista probe.
 */

#include "triton9.h"

#define TRITON9_COLOR_OPS \
    (FORMATOP_TEXTURE | FORMATOP_OFFSCREEN_RENDERTARGET | \
     FORMATOP_SAME_FORMAT_RENDERTARGET | FORMATOP_OFFSCREENPLAIN)

#define TRITON9_ARGB_OPS \
    (TRITON9_COLOR_OPS | FORMATOP_SAME_FORMAT_UP_TO_ALPHA_RENDERTARGET)
#define TRITON9_DISPLAY_OPS \
    (TRITON9_COLOR_OPS | FORMATOP_DISPLAYMODE | FORMATOP_3DACCELERATION)
#define TRITON9_DEPTH_OPS \
    (FORMATOP_ZSTENCIL | FORMATOP_ZSTENCIL_WITH_ARBITRARY_COLOR_DEPTH)

_Static_assert((TRITON9_ARGB_OPS & FORMATOP_DISPLAYMODE) == 0,
               "an alpha format cannot be a display mode");
_Static_assert((TRITON9_ARGB_OPS & FORMATOP_3DACCELERATION) == 0,
               "3DACCELERATION belongs only on a display-mode entry");
_Static_assert((TRITON9_DISPLAY_OPS & FORMATOP_DISPLAYMODE) != 0,
               "the XRGB display format must advertise display modes");
_Static_assert((TRITON9_DISPLAY_OPS & FORMATOP_3DACCELERATION) != 0,
               "the display-mode entry must advertise 3D acceleration");

/* Keep view-compatible resources typeless only when sRGB views are usable.
 * Sharing uses its existing typed host-import contract. */
#define TEX (FORMATOP_TEXTURE | FORMATOP_CUBETEXTURE | FORMATOP_VOLUMETEXTURE | FORMATOP_VERTEXTEXTURE)
#define COLOR (TEX | FORMATOP_OFFSCREEN_RENDERTARGET | FORMATOP_SAME_FORMAT_RENDERTARGET | FORMATOP_OFFSCREENPLAIN | FORMATOP_AUTOGENMIPMAP)
#define FMT(d,h,b,ops) { D3DDDIFMT_##d, DXGI_FORMAT_##h, b, ops, FALSE, 1, 1, b, DXGI_FORMAT_##h, DXGI_FORMAT_UNKNOWN }
#define SRGB(d,h,t,g,b,ops) { D3DDDIFMT_##d, DXGI_FORMAT_##h, b, (ops) | FORMATOP_SRGBREAD | FORMATOP_SRGBWRITE, FALSE, 1, 1, b, DXGI_FORMAT_##t, DXGI_FORMAT_##g }
#define BC(d,h,t,g,b) { D3DDDIFMT_##d, DXGI_FORMAT_##h, 0, (TEX & ~FORMATOP_VOLUMETEXTURE) | FORMATOP_SRGBREAD, FALSE, 4, 4, b, DXGI_FORMAT_##t, DXGI_FORMAT_##g }
#define DEPTH(d,h,b) { D3DDDIFMT_##d, DXGI_FORMAT_##h, b, TRITON9_DEPTH_OPS, TRUE, 1, 1, b, DXGI_FORMAT_##h, DXGI_FORMAT_UNKNOWN }
static const TRITON9_FORMAT g_formats[] = {
    SRGB(A8R8G8B8, B8G8R8A8_UNORM, B8G8R8A8_TYPELESS, B8G8R8A8_UNORM_SRGB, 4, COLOR | FORMATOP_SAME_FORMAT_UP_TO_ALPHA_RENDERTARGET),
    SRGB(X8R8G8B8, B8G8R8X8_UNORM, B8G8R8X8_TYPELESS, B8G8R8X8_UNORM_SRGB, 4, COLOR | FORMATOP_DISPLAYMODE | FORMATOP_3DACCELERATION),
    SRGB(A8B8G8R8, R8G8B8A8_UNORM, R8G8B8A8_TYPELESS, R8G8B8A8_UNORM_SRGB, 4, COLOR),
    SRGB(X8B8G8R8, R8G8B8A8_UNORM, R8G8B8A8_TYPELESS, R8G8B8A8_UNORM_SRGB, 4, COLOR),
    FMT(A8L8, R8G8_UNORM, 2, TEX),
    FMT(A2R10G10B10, R10G10B10A2_UNORM, 4, COLOR),
    FMT(A8, A8_UNORM, 1, TEX),
    FMT(L8, R8_UNORM, 1, TEX),
    FMT(L16, R16_UNORM, 2, TEX),
    FMT(Q8W8V8U8, R8G8B8A8_SNORM, 4, TEX | FORMATOP_BUMPMAP),
    FMT(V8U8, R8G8_SNORM, 2, TEX | FORMATOP_BUMPMAP),
    FMT(V16U16, R16G16_SNORM, 4, TEX | FORMATOP_BUMPMAP),
    FMT(R5G6B5, B5G6R5_UNORM, 2, COLOR),
    FMT(X1R5G5B5, B5G5R5A1_UNORM, 2, COLOR),
    FMT(X4R4G4B4, B4G4R4A4_UNORM, 2, COLOR),
    FMT(A1R5G5B5, B5G5R5A1_UNORM, 2, COLOR),
    FMT(A4R4G4B4, B4G4R4A4_UNORM, 2, COLOR),
    FMT(A2B10G10R10, R10G10B10A2_UNORM, 4, COLOR),
    FMT(A16B16G16R16, R16G16B16A16_UNORM, 8, COLOR),
    FMT(G16R16, R16G16_UNORM, 4, COLOR),
    FMT(R16F, R16_FLOAT, 2, COLOR),
    FMT(G16R16F, R16G16_FLOAT, 4, COLOR),
    FMT(A16B16G16R16F, R16G16B16A16_FLOAT, 8, COLOR),
    FMT(R32F, R32_FLOAT, 4, COLOR),
    FMT(G32R32F, R32G32_FLOAT, 8, COLOR),
    FMT(A32B32G32R32F, R32G32B32A32_FLOAT, 16, COLOR),
    BC(DXT1, BC1_UNORM, BC1_TYPELESS, BC1_UNORM_SRGB, 8),
    BC(DXT2, BC2_UNORM, BC2_TYPELESS, BC2_UNORM_SRGB, 16),
    BC(DXT3, BC2_UNORM, BC2_TYPELESS, BC2_UNORM_SRGB, 16),
    BC(DXT4, BC3_UNORM, BC3_TYPELESS, BC3_UNORM_SRGB, 16),
    BC(DXT5, BC3_UNORM, BC3_TYPELESS, BC3_UNORM_SRGB, 16),
    DEPTH(D16_LOCKABLE, D16_UNORM, 2),
    DEPTH(D16, D16_UNORM, 2),
    DEPTH(D24S8, D24_UNORM_S8_UINT, 4),
    DEPTH(D24X8, D24_UNORM_S8_UINT, 4),
};
#undef TEX
#undef COLOR
#undef FMT
#undef SRGB
#undef BC
#undef DEPTH

BOOL
triton9FormatSupportsSrgb(D3DDDIFORMAT format)
{
    const TRITON9_FORMAT *entry = triton9FormatLookup(format);
    return entry && entry->srgbFormat != DXGI_FORMAT_UNKNOWN;
}

const TRITON9_FORMAT *
triton9FormatLookup(D3DDDIFORMAT format)
{
    UINT i;
    UINT count = triton9FormatCount();

    for (i = 0; i < count; ++i) {
        if (g_formats[i].d3dFormat == format)
            return &g_formats[i];
    }
    return NULL;
}

const TRITON9_FORMAT *
triton9FormatLookupHost(DXGI_FORMAT format)
{
    UINT i;
    UINT count = triton9FormatCount();

    /* Shared DXGI storage has canonical R,G,B channel order. A2R10 uses
     * a CPU-side R/B conversion, so prefer the directly matching D3D9
     * format when importing a DXGI resource without a D3D9 declaration. */
    if (format == DXGI_FORMAT_R10G10B10A2_UNORM)
        return triton9FormatLookup(D3DDDIFMT_A2B10G10R10);

    for (i = 0; i < count; ++i) {
        if (g_formats[i].hostFormat == format)
            return &g_formats[i];
    }
    return NULL;
}

UINT
triton9FormatCount(void)
{
    UINT count = sizeof(g_formats) / sizeof(g_formats[0]);
    /* Both D24 rows remain last and depend on the complete clear path. */
    return triton9HasCompleteD24S8ClearContract() ? count : count - 2;
}

void
triton9CopyFormatOperations(FORMATOP *destination, UINT count)
{
    UINT i;
    if (!destination)
        return;
    if (count > triton9FormatCount())
        count = triton9FormatCount();
    for (i = 0; i < count; ++i) {
        destination[i].Format = g_formats[i].d3dFormat;
        destination[i].Operations = g_formats[i].operations;
        destination[i].FlipMsTypes = destination[i].BltMsTypes =
            triton9FormatMultisampleQuality(g_formats[i].d3dFormat, 2)
                ? 1u | (1u << (2 - 1)) | (1u << (4 - 1)) : 0;
        destination[i].PrivateFormatBitCount = 0;
    }
}

/* Baseline sample counts guaranteed by the host feature contract for these
 * formats. Creation additionally queries the live device before allocating. */
UINT
triton9FormatMultisampleQuality(D3DDDIFORMAT format, UINT samples)
{
    const TRITON9_FORMAT *entry = triton9FormatLookup(format);
    if (!entry || !(entry->operations & (FORMATOP_OFFSCREEN_RENDERTARGET | FORMATOP_ZSTENCIL))) return 0;
    if (samples == D3DDDIMULTISAMPLE_NONE) return 1;
    if (samples != D3DDDIMULTISAMPLE_NONMASKABLE && samples != 2 && samples != 4) return 0;
    switch (format) {
    case D3DDDIFMT_A8R8G8B8: case D3DDDIFMT_X8R8G8B8:
    case D3DDDIFMT_A8B8G8R8: case D3DDDIFMT_D16:
    case D3DDDIFMT_D24S8: case D3DDDIFMT_D24X8:
        return samples == D3DDDIMULTISAMPLE_NONMASKABLE ? 2 : 1;
    default: return 0;
    }
}
