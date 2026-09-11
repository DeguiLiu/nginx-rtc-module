/*
 * ngx_rtc_test.h - minimal host unit-test framework for the ngx-rtc pure C core.
 *
 * No external test dependency: plain C11 plus a tiny constructor-based test
 * registry. Each TU registers its tests with the NGX_RTC_TEST(name) macro; a
 * single main() (test_main.c) runs every registered test and prints a
 * PASS/FAIL summary.
 *
 * Assertion macros print the failure location and return from the test
 * function, so one failing assertion does not abort the whole run.
 */

#ifndef NGX_RTC_TEST_H
#define NGX_RTC_TEST_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ngx_core.h"

/* Registry entry; tests are registered at load time (constructor). */
typedef struct ngx_rtc_test_entry_s
{
    const char *name;
    void (*fn)(void);
} ngx_rtc_test_entry_t;

void ngx_rtc_test_register(const char *name, void (*fn)(void));

/* Run all registered tests, print the summary, return 0 when all passed. */
int ngx_rtc_test_run_all(void);

/*
 * Drain ngx_posted_events once, the way ngx_event_process_posted() does.
 *
 * nginx_stub.c defines it for both header worlds -- nginx's own event objects
 * under the real headers, the include/ stubs otherwise -- because the tests
 * that watch the close path's posted finalize have no event loop and must not
 * have to know which world they were compiled in. Declared here so that no
 * test TU calls it implicitly.
 */
void ngx_rtc_host_drain_posted(void);

/* Record a failed assertion and mark the running test as failed. */
void ngx_rtc_test_fail_bool(const char *file, int line, const char *cond);
void ngx_rtc_test_fail_i64(const char *file, int line,
                           const char *a_expr, const char *b_expr,
                           long long a, long long b);
void ngx_rtc_test_fail_u64(const char *file, int line,
                           const char *a_expr, const char *b_expr,
                           unsigned long long a, unsigned long long b);
void ngx_rtc_test_fail_str(const char *file, int line,
                           const char *a_expr, const char *b_expr,
                           const char *a, const char *b);

/*
 * Declare a test function. The constructor registers it before main(); the
 * `used` attribute keeps GCC from eliding the constructor function.
 */
#define NGX_RTC_TEST(name) \
    static void ngx_rtc_test_fn_##name(void); \
    __attribute__((constructor, used)) \
    static void ngx_rtc_test_reg_##name(void) \
    { \
        ngx_rtc_test_register(#name, ngx_rtc_test_fn_##name); \
    } \
    static void ngx_rtc_test_fn_##name(void)

#define NGX_RTC_TEST_ASSERT(cond) \
    do \
    { \
        if (!(cond)) \
        { \
            ngx_rtc_test_fail_bool(__FILE__, __LINE__, #cond); \
            return; \
        } \
    } while (0)

#define NGX_RTC_TEST_ASSERT_I64_EQ(a, b) \
    do \
    { \
        long long ngx_rtc_test_a_ = (long long)(a); \
        long long ngx_rtc_test_b_ = (long long)(b); \
        if (ngx_rtc_test_a_ != ngx_rtc_test_b_) \
        { \
            ngx_rtc_test_fail_i64(__FILE__, __LINE__, #a, #b, \
                                  ngx_rtc_test_a_, ngx_rtc_test_b_); \
            return; \
        } \
    } while (0)

#define NGX_RTC_TEST_ASSERT_U64_EQ(a, b) \
    do \
    { \
        unsigned long long ngx_rtc_test_a_ = (unsigned long long)(a); \
        unsigned long long ngx_rtc_test_b_ = (unsigned long long)(b); \
        if (ngx_rtc_test_a_ != ngx_rtc_test_b_) \
        { \
            ngx_rtc_test_fail_u64(__FILE__, __LINE__, #a, #b, \
                                  ngx_rtc_test_a_, ngx_rtc_test_b_); \
            return; \
        } \
    } while (0)

#define NGX_RTC_TEST_ASSERT_STR_EQ(a, b) \
    do \
    { \
        const char *ngx_rtc_test_a_ = (a); \
        const char *ngx_rtc_test_b_ = (b); \
        if (0 != strcmp(ngx_rtc_test_a_, ngx_rtc_test_b_)) \
        { \
            ngx_rtc_test_fail_str(__FILE__, __LINE__, #a, #b, \
                                  ngx_rtc_test_a_, ngx_rtc_test_b_); \
            return; \
        } \
    } while (0)

#define NGX_RTC_TEST_ASSERT_MEM_EQ(a, b, n) \
    do \
    { \
        const void *ngx_rtc_test_a_ = (const void *)(a); \
        const void *ngx_rtc_test_b_ = (const void *)(b); \
        if (0 != memcmp(ngx_rtc_test_a_, ngx_rtc_test_b_, (size_t)(n))) \
        { \
            ngx_rtc_test_fail_i64(__FILE__, __LINE__, #a, #b, 0, 1); \
            return; \
        } \
    } while (0)

#endif /* NGX_RTC_TEST_H */
