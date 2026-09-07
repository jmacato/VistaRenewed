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

static const TRITON9_FORMAT g_formats[] = {
    { D3DDDIFMT_A8R8G8B8, DXGI_FORMAT_B8G8R8A8_UNORM, 4, TRITON9_ARGB_OPS, FALSE },
    { D3DDDIFMT_X8R8G8B8, DXGI_FORMAT_B8G8R8X8_UNORM, 4, TRITON9_DISPLAY_OPS, FALSE },
    { D3DDDIFMT_A8, DXGI_FORMAT_A8_UNORM, 1, FORMATOP_TEXTURE, FALSE },
    { D3DDDIFMT_D16, DXGI_FORMAT_D16_UNORM, 2, TRITON9_DEPTH_OPS, TRUE },
    /* Keep D24S8 last. triton9FormatCount() excludes this entry unless the
     * sibling depth-clear implementation confirms the complete create,
     * bind, clear, preserve, completion, and readback contract. */
    { D3DDDIFMT_D24S8, DXGI_FORMAT_D24_UNORM_S8_UINT, 4,
      TRITON9_DEPTH_OPS, TRUE },
};

enum {
    TRITON9_BASE_FORMAT_COUNT = 4,
    TRITON9_ALL_FORMAT_COUNT = sizeof(g_formats) / sizeof(g_formats[0])
};

_Static_assert(TRITON9_ALL_FORMAT_COUNT == TRITON9_BASE_FORMAT_COUNT + 1,
               "D24S8 must remain the only conditional format");

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

    for (i = 0; i < count; ++i) {
        if (g_formats[i].hostFormat == format)
            return &g_formats[i];
    }
    return NULL;
}

UINT
triton9FormatCount(void)
{
    return triton9HasCompleteD24S8ClearContract()
        ? TRITON9_ALL_FORMAT_COUNT : TRITON9_BASE_FORMAT_COUNT;
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
        destination[i].FlipMsTypes = 0;
        destination[i].BltMsTypes = 0;
        destination[i].PrivateFormatBitCount = 0;
    }
}
