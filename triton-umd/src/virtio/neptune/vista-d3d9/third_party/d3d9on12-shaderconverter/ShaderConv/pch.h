// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
/*==========================================================================;
*
*  Copyright (C) Microsoft Corporation.  All rights reserved.
*
*  Precompiled header file
*
****************************************************************************/
#pragma once

//
// Disable optimize for speed and instead optimize for size in this library
//
#pragma optimize ("s", on)
#pragma optimize ("t", off)
#undef WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <strsafe.h>
#ifndef __assume
#if defined(__GNUC__)
#define __assume(condition) do { if (!(condition)) __builtin_unreachable(); } while (0)
#else
#define __assume(condition) ((void)0)
#endif
#endif
#include <ShaderConvCommon.h>

#include <cstdlib>
#include <cfloat>
#include <cstring>
#include <utility>

/* The checked source build uses the WDK DDI headers with a non-MSVC syntax
 * pass.  Older mingw SAL headers omit these annotation spellings.  They are
 * metadata only, so define no-op fallbacks without changing generated code. */
#ifndef __success
#define __success(condition)
#endif
#ifndef __field_xcount_part
#define __field_xcount_part(size, count)
#endif
#ifndef __nullterminated
#define __nullterminated
#endif
#ifndef __in_range
#define __in_range(lower, upper)
#endif

/* D3D9On12 only uses ATL's CComPtr for CCodeBlob.  CCodeBlob is not a COM
 * interface: it is an intrusive ShaderConverter object with AddRef/Release.
 * Keep the same ownership convention without dragging ATL or a newer CRT
 * into the Vista UMD. */
template <typename T>
class CComPtr
{
public:
    CComPtr() : m_pointer(nullptr) {}
    ~CComPtr()
    {
        if (m_pointer)
            m_pointer->Release();
    }

    CComPtr(const CComPtr &) = delete;
    CComPtr &operator=(const CComPtr &) = delete;

    T *operator->() const { return m_pointer; }
    operator T *() const { return m_pointer; }
    T **operator&() { return &m_pointer; }

private:
    T *m_pointer;
};

#pragma warning(push, 3)
#include <d3d9.h>
/* ShaderConv needs only this d3dhal constant.  Pulling d3dhal.h after
 * d3d9.h makes several legacy D3D type definitions collide in Vista SDK and
 * mingw header combinations.  The Windows SDK defines it as 16. */
#ifndef D3DHAL_SAMPLER_MAXSAMP
#define D3DHAL_SAMPLER_MAXSAMP 16
#endif
#define _d3d9TYPES_H_    // Fix d3d9 types redefinitions errors in d3d10umddi.h
#define D3D12_TOKENIZED_PROGRAM_FORMAT_HEADER
/* d3d10umddi.h exposes this WDDM 2 member when it uses its current minor
 * header version.  The Vista d3dumddi.h path correctly omits the definition.
 * ShaderConverter only sees the pointer declaration, so an incomplete type
 * keeps the converter headers independent from the runtime ABI version. */
typedef struct _D3DDDI_OPENALLOCATIONINFO2 D3DDDI_OPENALLOCATIONINFO2;
#include <d3d10umddi.h>
#pragma warning(pop)

/* mingw exposes the mask and shift but not the helper present in the Windows
 * SDK d3dhal header.  ShaderConv uses the documented token layout only. */
#ifndef D3DSI_GETINSTLENGTH
#define D3DSI_GETINSTLENGTH(token) \
    (((token) & D3DSI_INSTLENGTH_MASK) >> D3DSI_INSTLENGTH_SHIFT)
#endif
#ifndef D3DSI_GETREGNUM
#define D3DSI_GETREGNUM(token) ((token) & D3DSP_REGNUM_MASK)
#endif
#ifndef D3DSI_GETREGTYPE
#define D3DSI_GETREGTYPE(token) ((D3DSHADER_PARAM_REGISTER_TYPE)( \
    (((token) & D3DSP_REGTYPE_MASK) >> D3DSP_REGTYPE_SHIFT) | \
    (((token) & D3DSP_REGTYPE_MASK2) >> D3DSP_REGTYPE_SHIFT2)))
#endif
#ifndef D3DSI_GETWRITEMASK
#define D3DSI_GETWRITEMASK(token) ((token) & D3DSP_WRITEMASK_ALL)
#endif
#ifndef D3DSI_GETUSAGE
#define D3DSI_GETUSAGE(token) \
    (((token) & D3DSP_DCL_USAGE_MASK) >> D3DSP_DCL_USAGE_SHIFT)
#endif
#ifndef D3DSI_GETUSAGEINDEX
#define D3DSI_GETUSAGEINDEX(token) \
    (((token) & D3DSP_DCL_USAGEINDEX_MASK) >> D3DSP_DCL_USAGEINDEX_SHIFT)
#endif
#ifndef D3DSI_GETCOMPARISON
#ifndef D3DSHADER_COMPARISON_SHIFT
#define D3DSHADER_COMPARISON_SHIFT 16
#endif
#ifndef D3DSHADER_COMPARISON_MASK
#define D3DSHADER_COMPARISON_MASK (7u << D3DSHADER_COMPARISON_SHIFT)
#endif
#define D3DSI_GETCOMPARISON(token) ((D3DSHADER_COMPARISON)( \
    ((token) & D3DSHADER_COMPARISON_MASK) >> D3DSHADER_COMPARISON_SHIFT))
#endif
#ifndef D3DSI_GETTEXTURETYPE
#define D3DSI_GETTEXTURETYPE(token) ((token) & D3DSP_TEXTURETYPE_MASK)
#endif
#ifndef D3DSI_GETADDRESSMODE
#define D3DSI_GETADDRESSMODE(token) ((token) & D3DVS_ADDRESSMODE_MASK)
#endif

inline D3DSHADER_PARAM_REGISTER_TYPE
D3DSI_GETREGTYPE_RESOLVING_CONSTANTS(DWORD token)
{
    const D3DSHADER_PARAM_REGISTER_TYPE registerType = D3DSI_GETREGTYPE(token);
    switch (registerType) {
    case D3DSPR_CONST4:
    case D3DSPR_CONST3:
    case D3DSPR_CONST2:
        return D3DSPR_CONST;
    default:
        return registerType;
    }
}

inline UINT
D3DSI_GETREGNUM_RESOLVING_CONSTANTS(DWORD token)
{
    const UINT registerNumber = D3DSI_GETREGNUM(token);
    switch (D3DSI_GETREGTYPE(token)) {
    case D3DSPR_CONST4:
        return registerNumber + 6144;
    case D3DSPR_CONST3:
        return registerNumber + 4096;
    case D3DSPR_CONST2:
        return registerNumber + 2048;
    default:
        return registerNumber;
    }
}

#include <math.h>

#include <ShaderBinary.h>

#include "ShaderBinaryEx.hpp"
#include "disasm.hpp"

#include <vector>

