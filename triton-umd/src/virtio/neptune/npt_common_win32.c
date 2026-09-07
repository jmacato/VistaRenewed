/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Native Windows diagnostic sink.  A display UMD normally has no console, so
 * stderr-only logging hides the adapter or context failure that rejected it.
 * OutputDebugString is available on Vista and adds no post-Vista dependency.
 */

#if defined(_WIN32) && !defined(__WINE__)

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "npt_common.h"

#if defined(NPT_D3D9_RUNTIME_DDI)
static void
npt_append_vista_log(const char *message)
{
   static const char *const paths[] = {
      "C:\\Windows\\Temp\\triton9-ddi.log",
      "C:\\triton9-ddi.log",
   };
   HANDLE file = INVALID_HANDLE_VALUE;
   DWORD written;
   size_t length;
   unsigned i;

   if (!message || !message[0])
      return;
   length = strlen(message);
   if (length > MAXDWORD)
      length = MAXDWORD;
   for (i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
      file = CreateFileA(paths[i], GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                         OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
      if (file != INVALID_HANDLE_VALUE)
         break;
   }
   if (file == INVALID_HANDLE_VALUE)
      return;
   if (SetFilePointer(file, 0, NULL, FILE_END) != INVALID_SET_FILE_POINTER ||
       GetLastError() == NO_ERROR)
      WriteFile(file, message, (DWORD)length, &written, NULL);
   CloseHandle(file);
}
#endif

void
npt_log_impl(const char *fmt, ...)
{
   char buffer[1024];
   va_list args;
   int length;

   if (!fmt)
      return;
   va_start(args, fmt);
   length = vsnprintf(buffer, sizeof(buffer), fmt, args);
   va_end(args);
   if (length <= 0)
      return;
   buffer[sizeof(buffer) - 1] = '\0';
   OutputDebugStringA(buffer);
#if defined(NPT_D3D9_RUNTIME_DDI)
   /* OutputDebugString is not retrievable through the QMP-only Vista test
    * harness.  Persist the same Neptune transport errors beside the D3D9
    * DDI breadcrumbs so device creation can be diagnosed after a reboot. */
   npt_append_vista_log(buffer);
#endif
}

#endif
