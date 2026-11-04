// Shared harness for the EmuUSBAudio user-mode unit tests (driver/utest.sh).
// SPDX-License-Identifier: LGPL-2.1-or-later
#ifndef _EMU_TEST_HARNESS_H_
#define _EMU_TEST_HARNESS_H_

#include <stdio.h>
#include "emu_desc.h"   // NTSTATUS / NT_SUCCESS on the user-mode side

extern int g_checks;
extern int g_failures;
extern const char* g_case;

#define CHECK(cond)                                                            \
    do {                                                                       \
        g_checks++;                                                            \
        if (!(cond)) {                                                         \
            g_failures++;                                                      \
            printf("  FAIL [%s] %s:%d: %s\n",                                  \
                   g_case, __FILE__, __LINE__, #cond);                         \
        }                                                                      \
    } while (FALSE)

#define CHECK_EQ(actual, expected)                                             \
    do {                                                                       \
        g_checks++;                                                            \
        unsigned long long a_ = (unsigned long long)(actual);                   \
        unsigned long long e_ = (unsigned long long)(expected);                 \
        if (a_ != e_) {                                                        \
            g_failures++;                                                      \
            printf("  FAIL [%s] %s:%d: %s = 0x%llX, want 0x%llX\n",            \
                   g_case, __FILE__, __LINE__, #actual, a_, e_);               \
        }                                                                      \
    } while (FALSE)

__inline void
CheckStatus(_In_ const char* name, _In_ NTSTATUS expected, _In_ NTSTATUS actual)
{
    g_checks++;
    if (expected != actual) {
        g_failures++;
        printf("  FAIL [%s] %s: status 0x%08X, want 0x%08X\n",
               name, g_case, (unsigned)actual, (unsigned)expected);
    }
}

// Printed by every test binary; the runner script greps for "RESULT:".
__inline int
TestResult(void)
{
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    printf("RESULT: %s\n", g_failures == 0 ? "PASS" : "FAIL");
    return g_failures == 0 ? 0 : 1;
}

#endif // _EMU_TEST_HARNESS_H_
