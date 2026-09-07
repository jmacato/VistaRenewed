/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 */

#ifdef __WINE__

#include <stdarg.h>
#include <stdio.h>
#include <windows.h>
#include <winternl.h>

#include "npt_common.h"

/* msvcrt vsnprintf: user32's wvsprintfA omits %llu/%lld/%zu. */
void npt_log_impl(const char *fmt, ...)
{
   char buf[1024];
   va_list ap;
   va_start(ap, fmt);
   int len = vsnprintf(buf, sizeof(buf), fmt, ap);
   va_end(ap);

   if (len <= 0)
      return;
   if ((size_t)len >= sizeof(buf))
      len = (int)(sizeof(buf) - 1);

   OutputDebugStringA(buf);
   DWORD written;
   WriteFile(GetStdHandle(STD_ERROR_HANDLE), buf, len, &written, NULL);
}

/* Shared idempotent unixlib bridge init for both Wine backends. */
#define MemoryWineUnixFuncs 1000

typedef NTSTATUS (WINAPI *pfn_NtQueryVirtualMemory)(
   HANDLE, LPCVOID, int, PVOID, SIZE_T, SIZE_T *);

typedef UINT64 unixlib_handle_t;

unixlib_handle_t g_npt_unix_handle;
NTSTATUS (WINAPI *g_npt_unix_call_dispatcher)(
   unixlib_handle_t, unsigned int, void *);

static volatile LONG g_npt_unixlib_inited = 0; /* 0=no, 1=busy, 2=done, -1=fail */

int
npt_unixlib_ensure_init(void)
{
   LONG cur = InterlockedCompareExchange(&g_npt_unixlib_inited, 1, 0);
   if (cur == 2)
      return 0;
   if (cur == -1)
      return -1;
   if (cur == 1) {
      while ((cur = InterlockedCompareExchange(&g_npt_unixlib_inited, 1, 1)) == 1)
         SwitchToThread();
      return cur == 2 ? 0 : -1;
   }

   HMODULE bridge = LoadLibraryA("nptunix.dll");
   if (!bridge) {
      npt_log_impl("neptune: LoadLibrary(nptunix.dll) failed: %lu\n",
                    (unsigned long)GetLastError());
      InterlockedExchange(&g_npt_unixlib_inited, -1);
      return -1;
   }

   HMODULE ntdll = GetModuleHandleA("ntdll.dll");
   NTSTATUS (WINAPI **ppDisp)(unixlib_handle_t, unsigned int, void *) =
      (void *)GetProcAddress(ntdll, "__wine_unix_call_dispatcher");
   if (!ppDisp || !*ppDisp) {
      npt_log_impl("neptune: __wine_unix_call_dispatcher not found\n");
      InterlockedExchange(&g_npt_unixlib_inited, -1);
      return -1;
   }
   g_npt_unix_call_dispatcher = *ppDisp;

   pfn_NtQueryVirtualMemory pNtQVM =
      (pfn_NtQueryVirtualMemory)GetProcAddress(ntdll, "NtQueryVirtualMemory");
   if (!pNtQVM) {
      npt_log_impl("neptune: NtQueryVirtualMemory not found\n");
      InterlockedExchange(&g_npt_unixlib_inited, -1);
      return -1;
   }

   NTSTATUS st = pNtQVM(GetCurrentProcess(), bridge, MemoryWineUnixFuncs,
                        &g_npt_unix_handle, sizeof(g_npt_unix_handle), NULL);
   if (st) {
      npt_log_impl("neptune: NtQueryVirtualMemory(MemoryWineUnixFuncs) "
                    "failed: 0x%x\n", (unsigned)st);
      InterlockedExchange(&g_npt_unixlib_inited, -1);
      return -1;
   }

   npt_log_impl("neptune: nptunix unixlib loaded (handle=0x%llx)\n",
                (unsigned long long)g_npt_unix_handle);
   InterlockedExchange(&g_npt_unixlib_inited, 2);
   return 0;
}

NTSTATUS
npt_wine_unix_call(unsigned int code, void *args)
{
   return g_npt_unix_call_dispatcher(g_npt_unix_handle, code, args);
}

#endif /* __WINE__ */
