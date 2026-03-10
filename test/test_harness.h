/*
** test_harness.h — Minimal C test framework for azqlite
**
** No external dependencies. Provides test declaration, assertion macros,
** suite grouping, colored output, and pass/fail summary.
**
** Usage:
**   TEST(my_test) {
**       ASSERT_EQ(1, 1);
**       ASSERT_STR_EQ("hello", "hello");
**   }
**
**   int main(void) {
**       TEST_SUITE_BEGIN("My Suite");
**       RUN_TEST(my_test);
**       TEST_SUITE_END();
**       return test_harness_summary();
**   }
*/
#ifndef TEST_HARNESS_H
#define TEST_HARNESS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <unistd.h>

/* ── Color output ─────────────────────────────────────────────────── */

static int th_use_color = -1;

static int th_colors_enabled(void) {
    if (th_use_color < 0) {
        th_use_color = isatty(STDOUT_FILENO);
    }
    return th_use_color;
}

#define TH_GREEN  (th_colors_enabled() ? "\033[32m" : "")
#define TH_RED    (th_colors_enabled() ? "\033[31m" : "")
#define TH_YELLOW (th_colors_enabled() ? "\033[33m" : "")
#define TH_BOLD   (th_colors_enabled() ? "\033[1m"  : "")
#define TH_RESET  (th_colors_enabled() ? "\033[0m"  : "")

/* ── Global state ─────────────────────────────────────────────────── */

static int th_total   = 0;
static int th_passed  = 0;
static int th_failed  = 0;
static int th_current_failed = 0;
static const char *th_current_test = NULL;
static const char *th_current_suite = NULL;
static jmp_buf th_jump;

/* ── Test declaration ─────────────────────────────────────────────── */

#define TEST(name) static void test_##name(void)

/* ── Suite grouping ───────────────────────────────────────────────── */

#define TEST_SUITE_BEGIN(name) \
    do { \
        th_current_suite = (name); \
        fprintf(stdout, "\n%s%s=== %s ===%s\n", TH_BOLD, TH_YELLOW, (name), TH_RESET); \
    } while (0)

#define TEST_SUITE_END() \
    do { \
        th_current_suite = NULL; \
    } while (0)

/* ── Test runner ──────────────────────────────────────────────────── */

#define RUN_TEST(name) \
    do { \
        th_total++; \
        th_current_test = #name; \
        th_current_failed = 0; \
        if (setjmp(th_jump) == 0) { \
            test_##name(); \
        } \
        if (th_current_failed) { \
            th_failed++; \
            fprintf(stdout, "  %s%sFAIL%s  %s\n", TH_BOLD, TH_RED, TH_RESET, #name); \
        } else { \
            th_passed++; \
            fprintf(stdout, "  %sPASS%s  %s\n", TH_GREEN, TH_RESET, #name); \
        } \
    } while (0)

/* ── Summary ──────────────────────────────────────────────────────── */

static int test_harness_summary(void) {
    fprintf(stdout, "\n%s────────────────────────────────%s\n", TH_BOLD, TH_RESET);
    if (th_failed == 0) {
        fprintf(stdout, "%s%sAll %d tests passed%s\n",
                TH_BOLD, TH_GREEN, th_total, TH_RESET);
    } else {
        fprintf(stdout, "%s%s%d of %d tests FAILED%s\n",
                TH_BOLD, TH_RED, th_failed, th_total, TH_RESET);
    }
    fprintf(stdout, "  Passed: %d  Failed: %d  Total: %d\n\n",
            th_passed, th_failed, th_total);
    return th_failed > 0 ? 1 : 0;
}

/* ── Assertion failure helper ─────────────────────────────────────── */

#define TH_FAIL(...) \
    do { \
        fprintf(stderr, "    %sASSERTION FAILED%s at %s:%d\n      ", \
                TH_RED, TH_RESET, __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
        th_current_failed = 1; \
        longjmp(th_jump, 1); \
    } while (0)

/* ── Assertion macros ─────────────────────────────────────────────── */

#define ASSERT_TRUE(x) \
    do { if (!(x)) TH_FAIL("Expected TRUE, got FALSE: %s", #x); } while (0)

#define ASSERT_FALSE(x) \
    do { if ((x)) TH_FAIL("Expected FALSE, got TRUE: %s", #x); } while (0)

#define ASSERT_EQ(a, b) \
    do { \
        long long _a = (long long)(a), _b = (long long)(b); \
        if (_a != _b) TH_FAIL("Expected %s == %s (%lld != %lld)", #a, #b, _a, _b); \
    } while (0)

#define ASSERT_NE(a, b) \
    do { \
        long long _a = (long long)(a), _b = (long long)(b); \
        if (_a == _b) TH_FAIL("Expected %s != %s (both %lld)", #a, #b, _a); \
    } while (0)

#define ASSERT_GT(a, b) \
    do { \
        long long _a = (long long)(a), _b = (long long)(b); \
        if (_a <= _b) TH_FAIL("Expected %s > %s (%lld <= %lld)", #a, #b, _a, _b); \
    } while (0)

#define ASSERT_GE(a, b) \
    do { \
        long long _a = (long long)(a), _b = (long long)(b); \
        if (_a < _b) TH_FAIL("Expected %s >= %s (%lld < %lld)", #a, #b, _a, _b); \
    } while (0)

#define ASSERT_LT(a, b) \
    do { \
        long long _a = (long long)(a), _b = (long long)(b); \
        if (_a >= _b) TH_FAIL("Expected %s < %s (%lld >= %lld)", #a, #b, _a, _b); \
    } while (0)

#define ASSERT_LE(a, b) \
    do { \
        long long _a = (long long)(a), _b = (long long)(b); \
        if (_a > _b) TH_FAIL("Expected %s <= %s (%lld > %lld)", #a, #b, _a, _b); \
    } while (0)

#define ASSERT_NULL(x) \
    do { if ((x) != NULL) TH_FAIL("Expected NULL: %s", #x); } while (0)

#define ASSERT_NOT_NULL(x) \
    do { if ((x) == NULL) TH_FAIL("Expected non-NULL: %s", #x); } while (0)

#define ASSERT_STR_EQ(a, b) \
    do { \
        const char *_a = (a), *_b = (b); \
        if (_a == NULL && _b == NULL) break; \
        if (_a == NULL || _b == NULL || strcmp(_a, _b) != 0) \
            TH_FAIL("Expected \"%s\" == \"%s\"", _a ? _a : "(null)", _b ? _b : "(null)"); \
    } while (0)

#define ASSERT_STR_NE(a, b) \
    do { \
        const char *_a = (a), *_b = (b); \
        if (_a != NULL && _b != NULL && strcmp(_a, _b) == 0) \
            TH_FAIL("Expected strings to differ: \"%s\"", _a); \
    } while (0)

#define ASSERT_MEM_EQ(a, b, len) \
    do { \
        const void *_a = (a), *_b = (b); \
        size_t _len = (size_t)(len); \
        if (memcmp(_a, _b, _len) != 0) \
            TH_FAIL("Memory mismatch at %s vs %s (%zu bytes)", #a, #b, _len); \
    } while (0)

/* ── SQLite-specific assertions ───────────────────────────────────── */

#define ASSERT_OK(rc) \
    do { \
        int _rc = (rc); \
        if (_rc != SQLITE_OK) \
            TH_FAIL("Expected SQLITE_OK (0), got %d: %s", _rc, #rc); \
    } while (0)

#define ASSERT_ERR(rc, expected) \
    do { \
        int _rc = (rc), _exp = (expected); \
        if (_rc != _exp) \
            TH_FAIL("Expected error %d, got %d: %s", _exp, _rc, #rc); \
    } while (0)

/* ── azure_err_t assertions ───────────────────────────────────────── */

#define ASSERT_AZURE_OK(rc) \
    do { \
        azure_err_t _rc = (rc); \
        if (_rc != AZURE_OK) \
            TH_FAIL("Expected AZURE_OK (0), got %d: %s", (int)_rc, #rc); \
    } while (0)

#define ASSERT_AZURE_ERR(rc, expected) \
    do { \
        azure_err_t _rc = (rc), _exp = (expected); \
        if (_rc != _exp) \
            TH_FAIL("Expected azure error %d, got %d: %s", (int)_exp, (int)_rc, #rc); \
    } while (0)

#endif /* TEST_HARNESS_H */
