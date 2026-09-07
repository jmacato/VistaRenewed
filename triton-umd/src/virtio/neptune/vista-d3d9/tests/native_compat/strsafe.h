/* Minimal host-test replacement for the Windows strsafe header. */
#ifndef TRITON9_TEST_STRSAFE_H
#define TRITON9_TEST_STRSAFE_H

#include <stdarg.h>
#include <stdio.h>

static inline HRESULT
StringCchPrintfA(char *destination, size_t destinationCount,
                 const char *format, ...)
{
    int result;
    va_list arguments;

    if (!destination || !destinationCount || !format)
        return E_INVALIDARG;
    va_start(arguments, format);
    result = vsnprintf(destination, destinationCount, format, arguments);
    va_end(arguments);
    if (result < 0 || (size_t)result >= destinationCount) {
        destination[destinationCount - 1] = '\0';
        return E_FAIL;
    }
    return S_OK;
}

#endif
