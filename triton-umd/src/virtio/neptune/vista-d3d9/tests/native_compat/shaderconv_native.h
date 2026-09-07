#ifndef TRITON9_TEST_SHADERCONV_NATIVE_H
#define TRITON9_TEST_SHADERCONV_NATIVE_H

#include <windows.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef _countof
#define _countof(array) (sizeof(array) / sizeof((array)[0]))
#endif
#ifndef ARRAYSIZE
#define ARRAYSIZE(array) _countof(array)
#endif
#ifndef C_ASSERT
#ifdef __cplusplus
#define C_ASSERT(expression) static_assert((expression), #expression)
#else
#define C_ASSERT(expression) _Static_assert((expression), #expression)
#endif
#endif
#ifndef __out_ecount
#define __out_ecount(count)
#endif
#ifndef __checkReturn
#define __checkReturn
#endif
#ifndef _Field_range_
#define _Field_range_(minimum, maximum)
#endif
#ifndef _In_
#define _In_
#endif
#ifndef _Inout_
#define _Inout_
#endif
#ifndef _Out_
#define _Out_
#endif
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
#define __in_range(minimum, maximum)
#endif

typedef unsigned char byte;
#ifndef __int64
#define __int64 long long
#endif
#ifndef __fallthrough
#ifdef __cplusplus
#define __fallthrough [[fallthrough]]
#else
#define __fallthrough ((void)0)
#endif
#endif

static inline LONG
InterlockedIncrement(volatile LONG *value)
{
    return __sync_add_and_fetch(value, 1);
}

static inline LONG
InterlockedDecrement(volatile LONG *value)
{
    return __sync_sub_and_fetch(value, 1);
}

static inline LONG
InterlockedExchange(volatile LONG *value, LONG replacement)
{
    return __sync_lock_test_and_set(value, replacement);
}

#ifndef HEAP_ZERO_MEMORY
#define HEAP_ZERO_MEMORY 0x00000008u
#endif

static inline HANDLE
GetProcessHeap(void)
{
    return NULL;
}

static inline void *
HeapAlloc(HANDLE heap, DWORD flags, size_t bytes)
{
    (void)heap;
    return flags & HEAP_ZERO_MEMORY ? calloc(1, bytes) : malloc(bytes);
}

static inline BOOL
HeapFree(HANDLE heap, DWORD flags, void *memory)
{
    (void)heap;
    (void)flags;
    free(memory);
    return TRUE;
}

#ifndef _TRUNCATE
#define _TRUNCATE ((size_t)-1)
#endif

static inline int
_snprintf_s(char *destination, size_t destinationCount, size_t maximumCount,
            const char *format, ...)
{
    int result;
    va_list arguments;

    (void)maximumCount;
    va_start(arguments, format);
    result = vsnprintf(destination, destinationCount, format, arguments);
    va_end(arguments);
    if (destinationCount)
        destination[destinationCount - 1] = '\0';
    return result;
}

static inline void
OutputDebugStringA(const char *message)
{
    (void)message;
}

/* Converter assertions also return an HRESULT.  Do not trap the host-side
 * contract test before it can report the exact conversion failure. */
static inline void
DebugBreak(void)
{
}

#endif
