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

//
// DDK headers
//
#pragma warning( push,3 )
#undef WIN32_LEAN_AND_MEAN
#include "d3d11.h"
#include "d3d10_1.h"
#define D3D12_TOKENIZED_PROGRAM_FORMAT_HEADER
/* The current d3d10 UMD header refers to this WDDM 2 pointer member even
 * when Vista's d3dumddi.h does not define the member type.  This converter
 * path never dereferences it. */
typedef struct _D3DDDI_OPENALLOCATIONINFO2 D3DDDI_OPENALLOCATIONINFO2;
#include "d3d10umddi.h"
#pragma warning( pop )

/* The static Vista build shares these headers with a non-MSVC syntax pass.
 * These SAL spellings are annotations only and are absent from older mingw
 * headers. */
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

#include <ShaderConvCommon.h>

//
// Other external headers
//
#include <stdio.h>

//
// WARP headers
//
#include <ShaderBinary.h>
