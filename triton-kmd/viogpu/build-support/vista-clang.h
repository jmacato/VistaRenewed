#pragma once
/* WDK 7.1's pointer-cast offsetof is not a C++ constant expression in
 * Clang. Preserve its layout assertions using the compiler's intrinsic. */
#if defined(__clang__)
#include <stddef.h>
#undef offsetof
#define offsetof(type, member) __builtin_offsetof(type, member)
#endif
#if defined(__clang__) && defined(_X86_)
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
