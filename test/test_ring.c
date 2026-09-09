/*
 * test_ring.c - host tests for the bounded fixed-size ring (ngx_rtc_ring).
 */

#include "ngx_rtc_test.h"
#include "ngx_rtc_ring.h"

NGX_RTC_TEST(ring_init_push_pop_fifo)
{
    ngx_rtc_ring_t r;
    uint32_t       v;
    uint32_t       i;

    (void)memset(&r, 0, sizeof(r));
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_ring_init(&r, 4u, sizeof(uint32_t)), 0);
    NGX_RTC_TEST_ASSERT_U64_EQ(r.capacity, 4u);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_ring_empty(&r), 1);
    NGX_RTC_TEST_ASSERT_U64_EQ(ngx_rtc_ring_count(&r), 0u);

    for (i = 0u; i < 4u; i++) {
        NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_ring_push(&r, &i), 0);
    }

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_ring_full(&r), 1);
    NGX_RTC_TEST_ASSERT_U64_EQ(ngx_rtc_ring_count(&r), 4u);

    for (i = 0u; i < 4u; i++) {
        NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_ring_pop(&r, &v), 0);
        NGX_RTC_TEST_ASSERT_U64_EQ(v, i);
    }

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_ring_empty(&r), 1);
    ngx_rtc_ring_destroy(&r);
}

NGX_RTC_TEST(ring_overflow_and_wrap)
{
    ngx_rtc_ring_t r;
    uint32_t       v;
    uint32_t       w;
    uint32_t       i;

    (void)memset(&r, 0, sizeof(r));
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_ring_init(&r, 4u, sizeof(uint32_t)), 0);

    for (i = 0u; i < 4u; i++) {
        NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_ring_push(&r, &i), 0);
    }

    /* A full ring rejects the next push. */
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_ring_push(&r, &i), -1);

    /* Drain and refill repeatedly to exercise index wrap-around. */
    for (i = 0u; i < 100u; i++) {
        NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_ring_pop(&r, &v), 0);
        NGX_RTC_TEST_ASSERT_U64_EQ(v, i);
        w = i + 4u;
        NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_ring_push(&r, &w), 0);
    }

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_ring_full(&r), 1);
    ngx_rtc_ring_destroy(&r);
}

NGX_RTC_TEST(ring_init_rounds_and_guards)
{
    ngx_rtc_ring_t r;
    uint32_t       v;

    (void)memset(&r, 0, sizeof(r));

    /* Non-power-of-two capacity is rounded up to the next power of two. */
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_ring_init(&r, 3u, sizeof(uint32_t)), 0);
    NGX_RTC_TEST_ASSERT_U64_EQ(r.capacity, 4u);

    /* Empty pop and uninitialised-ring checks fail cleanly. */
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_ring_pop(&r, &v), -1);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_ring_init(&r, 0u, sizeof(uint32_t)), -1);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_ring_init(&r, 4u, 0u), -1);

    ngx_rtc_ring_destroy(&r);
    ngx_rtc_ring_destroy(&r); /* double destroy is a safe no-op */
}
