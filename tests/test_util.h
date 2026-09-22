#ifndef TEST_UTIL_H
#define TEST_UTIL_H
#include <stdio.h>
#include <stdlib.h>
static int g_checks = 0, g_failures = 0;
#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failures++; fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); g_checks++; if (_a != _b) { g_failures++; fprintf(stderr, "%s:%d: CHECK_EQ failed: %s = %lld, %s = %lld\n", __FILE__, __LINE__, #a, _a, #b, _b); } } while (0)
static int test_summary(const char *name)
{
    printf("%s: %d checks, %d failures\n", name, g_checks, g_failures);
    return g_failures ? 1 : 0;
}
#endif
