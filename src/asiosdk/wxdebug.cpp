// Minimal implementation of the DShow-style debug helpers referenced by the
// ASIO SDK common code (dllentry.cpp / combase.cpp). The original wxdebug.cpp
// ships in the DirectShow base classes; these are functional no-ops.
// SPDX-License-Identifier: LGPL-2.1-or-later
#include <windows.h>
#include "wxdebug.h"

#if defined(DEBUG)
void DbgInitKeyLevels(HKEY hKey) { (void)hKey; }
void DbgInitGlobalSettings() {}
void DbgInitModuleSettings() {}
void DbgInitModuleName() {}
void DbgSetModuleLevel(DWORD, DWORD) {}
void DbgInitialise(HINSTANCE hInst) { (void)hInst; }
void DbgTerminate() {}
void DbgDumpObjectRegister() {}
void DbgAssert(const TCHAR*, const TCHAR*, INT) {}
void DbgBreakPoint(const TCHAR*, const TCHAR*, INT) {}
void DbgKernelAssert(const TCHAR*, const TCHAR*, INT) {}
void DbgOutString(LPCTSTR) {}
void DbgSetWaitTimeout(DWORD) {}
void DbgLogInfo(DWORD, DWORD, const TCHAR* pFormat, ...) {
    (void)pFormat;
}
#endif
