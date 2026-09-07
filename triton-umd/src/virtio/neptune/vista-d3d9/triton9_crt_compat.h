/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 */

/*
 * The Vista profile deliberately uses msvcrt.dll.  GCC 15's libstdc++ still
 * expects the C11 quick-exit declarations even though they are not provided
 * by that CRT.  The D3D9 UMD never exposes these process-global facilities;
 * map them to the normal CRT exit path so C++ headers remain usable.
 */
#ifndef TRITON9_CRT_COMPAT_H
#define TRITON9_CRT_COMPAT_H

#ifndef __ASSEMBLER__

#include <stdlib.h>

static inline int
at_quick_exit(void (*function)(void))
{
   return atexit(function);
}

static inline __attribute__((noreturn)) void
quick_exit(int status)
{
   exit(status);
}

#endif

#endif
