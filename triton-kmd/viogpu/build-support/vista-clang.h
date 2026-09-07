#pragma once
/* WDK 7.1's pointer-cast offsetof is not a C++ constant expression in
 * Clang. Preserve its layout assertions using the compiler's intrinsic. */
#if defined(__clang__)
#include <stddef.h>
#undef offsetof
#define offsetof(type, member) __builtin_offsetof(type, member)
#endif
#if defined(__clang__) && defined(_X86_)
/*
 * WDK 7.1's x86 vadefs.h computes the first variable argument from the
 * address of the named argument.  That is valid for its original compiler,
 * but is not a contract Clang preserves after it inlines an optimized
 * variadic function.  Use Clang's ABI-aware intrinsics instead.  Include the
 * WDK stdarg wrapper first so va_list retains the type expected by the WDK
 * formatting APIs, then replace only its x86 access macros.
 */
#include <stdarg.h>
#undef _crt_va_start
#undef _crt_va_arg
#undef _crt_va_end
#ifdef _crt_va_copy
#undef _crt_va_copy
#endif
#undef va_start
#undef va_arg
#undef va_end
#ifdef va_copy
#undef va_copy
#endif
#define _crt_va_start(ap, last) __builtin_va_start(ap, last)
#define _crt_va_arg(ap, type) __builtin_va_arg(ap, type)
#define _crt_va_end(ap) __builtin_va_end(ap)
#define _crt_va_copy(destination, source) __builtin_va_copy(destination, source)
#define va_start(ap, last) _crt_va_start(ap, last)
#define va_arg(ap, type) _crt_va_arg(ap, type)
#define va_end(ap) _crt_va_end(ap)
#define va_copy(destination, source) _crt_va_copy(destination, source)

/* WDK 7.1 supplies CAS-loop implementations on x86. Give them private
 * names so their definitions do not collide with Clang's builtins. */
#define _InterlockedAnd64 vista_wdk_InterlockedAnd64
#define _InterlockedOr64 vista_wdk_InterlockedOr64
#define _InterlockedXor64 vista_wdk_InterlockedXor64
#define _InterlockedIncrement64 vista_wdk_InterlockedIncrement64
#define _InterlockedDecrement64 vista_wdk_InterlockedDecrement64
#define _InterlockedExchange64 vista_wdk_InterlockedExchange64
#define _InterlockedExchangeAdd64 vista_wdk_InterlockedExchangeAdd64
#endif
