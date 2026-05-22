#ifndef CMCP_TEST_H
#define CMCP_TEST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_test_count = 0;
static int g_test_fails = 0;

#define TEST_ASSERT(cond) do {                                                \
    g_test_count++;                                                            \
    if (!(cond)) {                                                             \
        g_test_fails++;                                                        \
        fprintf(stderr, "    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
    }                                                                          \
} while (0)

#define TEST_RUN(fn) do {                                                      \
    fprintf(stderr, "  - %s\n", #fn);                                          \
    fn();                                                                       \
} while (0)

/* Like TEST_RUN, but for test functions taking a single argument —
 * e.g. a fixture or connection reused across cases. */
#define TEST_RUN_ARG(fn, arg) do {                                             \
    fprintf(stderr, "  - %s\n", #fn);                                          \
    fn(arg);                                                                    \
} while (0)

#define TEST_DONE() do {                                                       \
    fprintf(stderr, "  %d/%d assertions passed\n",                             \
            g_test_count - g_test_fails, g_test_count);                        \
    return g_test_fails == 0 ? 0 : 1;                                          \
} while (0)

#endif
