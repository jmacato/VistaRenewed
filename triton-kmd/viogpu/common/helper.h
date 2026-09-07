/*
 * Copyright (C) 2019-2020 Red Hat, Inc.
 *
 * Written By: Vadim Rozenfeld <vrozenfe@redhat.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met :
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and / or other materials provided with the distribution.
 * 3. Neither the names of the copyright holders nor the names of their contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#pragma once

extern "C"
{

#define __CPLUSPLUS

#include <stddef.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include <initguid.h>

#include <ntddk.h>

#ifndef FAR
#define FAR
#endif

#include <windef.h>
#include <winerror.h>

#include <wingdi.h>
#include <stdarg.h>

#include <winddi.h>
#include <ntddvdeo.h>

#include <d3dkmddi.h>
#include <d3dkmthk.h>

#include <ntstrsafe.h>
#include <ntintsafe.h>

#include <dispmprt.h>

#include "trace.h"
}

// WDK 7.1 predates the SAL 2 annotation names used by the current Triton
// sources.  They are compile-time annotations only, so Vista builds retain
// the declarations while compiling them as no-ops.
#if defined(VIOGPU_TARGET_VISTA)
#ifndef _In_
#define _In_
#define _In_opt_
#define _In_reads_bytes_(size)
#define _Inout_
#define _Out_
#define _Out_opt_
#define _Out_writes_bytes_(size)
#define _Outptr_
#define _Outptr_result_bytebuffer_(size)
#define _Outptr_result_maybenull_
#define _When_(condition, annotation)
#define _IRQL_raises_(irql)
#define _IRQL_requires_(irql)
#define _IRQL_requires_max_(irql)
#define _IRQL_restores_global_(irql, oldIrql)
#define _IRQL_saves_global_(oldIrql, irql)
#endif
#endif

#define MAX_CHILDREN                       1
#define MAX_VIEWS                          1
#define BITS_PER_BYTE                      8

#define POINTER_SIZE                       64

#define MIN_WIDTH_SIZE                     640
#define MIN_HEIGHT_SIZE                    480

#define NOM_WIDTH_SIZE                     1024
#define NOM_HEIGHT_SIZE                    768

#define VIOGPUTAG                          'OIVg'

// Vista SP2 has neither NonPagedPoolNx/POOL_NX_ALLOCATION nor
// ExAllocatePoolUninitialized.  Keep the compatibility decision in one place
// so the exact same sources can continue to build for modern Triton targets.
#if defined(VIOGPU_TARGET_VISTA)
#define VIOGPU_NONPAGED_POOL              NonPagedPool
#define VIOGPU_NPAGED_LOOKASIDE_POOL      NonPagedPool
static __forceinline PVOID
VioGpuAllocatePool(POOL_TYPE poolType, SIZE_T size, ULONG tag)
{
    return ExAllocatePoolWithTag(poolType, size, tag);
}
#else
#define VIOGPU_NONPAGED_POOL              NonPagedPoolNx
#define VIOGPU_NPAGED_LOOKASIDE_POOL      POOL_NX_ALLOCATION
static __forceinline PVOID
VioGpuAllocatePool(POOL_TYPE poolType, SIZE_T size, ULONG tag)
{
    return ExAllocatePoolUninitialized(poolType, size, tag);
}
#endif

// DbgPrintEx, not DbgPrint: WPP's textual scan emits a
// WPP_CALL_<file>_cpp<line> stub only at literal DbgPrint sites, so a
// DbgPrint nested inside this macro would expand at a line for which
// no stub exists.
#define VIOGPU_LOG_ASSERTION0(Msg)                                                                                     \
    do                                                                                                                 \
    {                                                                                                                  \
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL, "ASSERTION FAILED %s: %s\n", __FUNCTION__, Msg);             \
        NT_ASSERT(FALSE);                                                                                              \
    } while (0)
#define VIOGPU_LOG_ASSERTION1(Msg, Param1)                                                                             \
    do                                                                                                                 \
    {                                                                                                                  \
        DbgPrintEx(DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL, "ASSERTION FAILED %s: " Msg, __FUNCTION__, Param1);          \
        NT_ASSERT(FALSE);                                                                                              \
    } while (0)
#define VIOGPU_ASSERT(exp)                                                                                             \
    {                                                                                                                  \
        if (!(exp))                                                                                                    \
        {                                                                                                              \
            VIOGPU_LOG_ASSERTION0(#exp);                                                                               \
        }                                                                                                              \
    }

#if DBG && !defined(VIOGPU_DIAGNOSTIC_NO_BREAKS)
#define VIOGPU_ASSERT_CHK(exp) VIOGPU_ASSERT(exp)
#else
// The serial-trace probe runs against Vista's checked kernel with no
// interactive KD client.  An assertion there prevents the probe from
// reporting the callback that led to it.  Keep assertion checks active in
// every shipping configuration; only the explicitly named diagnostic build
// suppresses the break so its surrounding trace can be collected.
#define VIOGPU_ASSERT_CHK(exp)                                                                                         \
    {                                                                                                                  \
    }
#endif

#define PAGED_CODE_SEG       __declspec(code_seg("PAGE"))
#define PAGED_CODE_SEG_BEGIN __pragma(code_seg(push)) __pragma(code_seg("PAGE"))

#define PAGED_CODE_SEG_END   __pragma(code_seg(pop))
