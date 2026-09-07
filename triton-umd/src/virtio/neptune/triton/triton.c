/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * DllMain.  Triton statically links the Neptune COM factory through
 * npt_entry_internal.h; there is no runtime LoadLibrary step.
 */

#include <windows.h>

#include "triton_log.h"

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD fdwReason, LPVOID lpvReserved)
{
    (void)lpvReserved;
    switch (fdwReason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hInst);
        TR_LOG("DllMain DLL_PROCESS_ATTACH");
        break;
    case DLL_PROCESS_DETACH:
        TR_LOG("DllMain DLL_PROCESS_DETACH");
        break;
    default:
        break;
    }
    return TRUE;
}
