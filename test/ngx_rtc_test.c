/*
 * ngx_rtc_test.c - registry and runner for the minimal host test framework.
 */

#include "ngx_rtc_test.h"

/*
 * Which nginx headers this run was compiled against.
 *
 * The two worlds are different struct layouts and different macros, so a green
 * run only ever says "these headers are fine" -- and the make output that named
 * the world has long scrolled away by the time a result gets quoted. A log that
 * does not carry the world with it is how a stub-only 145/145 came to be read as
 * covering nginx's own headers.
 */
#ifdef NGX_RTC_REAL_NGINX_HEADERS
#define NGX_RTC_HEADER_WORLD "real (nginx's own headers)"
#else
#define NGX_RTC_HEADER_WORLD "stub (test/include)"
#endif

#define NGX_RTC_TEST_MAX_CASES 256

static ngx_rtc_test_entry_t g_entries[NGX_RTC_TEST_MAX_CASES];
static int g_entry_count = 0;
static int g_current_failed = 0;

void ngx_rtc_test_register(const char *name, void (*fn)(void))
{
    if (g_entry_count < NGX_RTC_TEST_MAX_CASES)
    {
        g_entries[g_entry_count].name = name;
        g_entries[g_entry_count].fn = fn;
        g_entry_count++;
    }
}

static void ngx_rtc_test_mark_failed(const char *file, int line, const char *msg)
{
    g_current_failed = 1;
    (void)fprintf(stderr, "  FAIL %s:%d: %s\n", file, line, msg);
}

void ngx_rtc_test_fail_bool(const char *file, int line, const char *cond)
{
    ngx_rtc_test_mark_failed(file, line, cond);
}

void ngx_rtc_test_fail_i64(const char *file, int line,
                           const char *a_expr, const char *b_expr,
                           long long a, long long b)
{
    char msg[256];

    (void)snprintf(msg, sizeof(msg), "%s (%lld) != %s (%lld)",
                   a_expr, a, b_expr, b);
    ngx_rtc_test_mark_failed(file, line, msg);
}

void ngx_rtc_test_fail_u64(const char *file, int line,
                           const char *a_expr, const char *b_expr,
                           unsigned long long a, unsigned long long b)
{
    char msg[256];

    (void)snprintf(msg, sizeof(msg), "%s (%llu) != %s (%llu)",
                   a_expr, a, b_expr, b);
    ngx_rtc_test_mark_failed(file, line, msg);
}

void ngx_rtc_test_fail_str(const char *file, int line,
                           const char *a_expr, const char *b_expr,
                           const char *a, const char *b)
{
    char msg[512];

    (void)snprintf(msg, sizeof(msg), "%s (\"%s\") != %s (\"%s\")",
                   a_expr, a, b_expr, b);
    ngx_rtc_test_mark_failed(file, line, msg);
}

int ngx_rtc_test_run_all(void)
{
    int i;
    int total = g_entry_count;
    int passed = 0;
    int failed = 0;

    for (i = 0; i < g_entry_count; i++)
    {
        g_current_failed = 0;
        g_entries[i].fn();
        if (0 == g_current_failed)
        {
            passed++;
            (void)printf("PASS  %s\n", g_entries[i].name);
        }
        else
        {
            failed++;
            /*
             * Name the case on stdout as well, not only on stderr. The detail
             * lines ("FAIL file:line: expr (a) != expr (b)") go to stderr with
             * the assertion, and stderr is unbuffered, so a CI that keeps only
             * stdout used to end up with the summary line and no idea which
             * case produced it. Flush immediately for the same reason the
             * summary flushes below: if a later case crashes under a sanitizer,
             * the record of this failure is already on disk.
             */
            (void)printf("FAIL  %s\n", g_entries[i].name);
            (void)fflush(stdout);
        }
    }

    (void)printf("\n==== ngx-rtc host unit tests ====\n");
    (void)printf("header world: %s\n", NGX_RTC_HEADER_WORLD);
    (void)printf("TOTAL: %d  PASS: %d  FAIL: %d\n", total, passed, failed);

    /*
     * Flush before returning. Under a sanitizer, an exit that reports a problem
     * goes through the runtime's own _exit()-style path rather than normal
     * stdio cleanup, so a redirected run loses whatever is still sitting in the
     * stdout buffer -- which is exactly this summary plus the tail of the PASS
     * lines. The symptom is a run that looks truncated for no visible reason.
     */
    (void)fflush(stdout);

    return (0 == failed) ? 0 : 1;
}
