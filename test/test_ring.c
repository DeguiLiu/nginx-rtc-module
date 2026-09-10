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

NGX_RTC_TEST(vring_push_pop_variable_length)
{
    ngx_rtc_vring_t r;
    uint8_t         out[16];
    uint32_t        out_len;
    uint8_t         a[3] = {1u, 2u, 3u};
    uint8_t         b[5] = {4u, 5u, 6u, 7u, 8u};

    (void)memset(&r, 0, sizeof(r));
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_vring_init(&r, 32u), 0);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_vring_empty(&r), 1);

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_vring_push(&r, a, 3u), 0);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_vring_push(&r, b, 5u), 0);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_vring_empty(&r), 0);

    out_len = 0u;
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_vring_pop(&r, out, sizeof(out), &out_len), 0);
    NGX_RTC_TEST_ASSERT_U64_EQ(out_len, 3u);
    NGX_RTC_TEST_ASSERT_U64_EQ(out[0], 1u);
    NGX_RTC_TEST_ASSERT_U64_EQ(out[2], 3u);

    out_len = 0u;
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_vring_pop(&r, out, sizeof(out), &out_len), 0);
    NGX_RTC_TEST_ASSERT_U64_EQ(out_len, 5u);
    NGX_RTC_TEST_ASSERT_U64_EQ(out[4], 8u);

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_vring_empty(&r), 1);
    ngx_rtc_vring_destroy(&r);
}

NGX_RTC_TEST(vring_wrap_split_entry)
{
    ngx_rtc_vring_t r;
    uint8_t         out[16];
    uint32_t        out_len;
    uint8_t         a[10];
    uint8_t         b[6];
    uint32_t        i;

    for (i = 0u; i < 10u; i++) {
        a[i] = (uint8_t)(0xA0u + i);
    }
    for (i = 0u; i < 6u; i++) {
        b[i] = (uint8_t)(0xB0u + i);
    }

    (void)memset(&r, 0, sizeof(r));
    /* capacity 16: an entry of len 10 takes 14 bytes (head -> 14), so the next
     * push of len 6 (10 bytes) wraps the length prefix across the boundary. */
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_vring_init(&r, 16u), 0);

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_vring_push(&r, a, 10u), 0);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_vring_pop(&r, out, sizeof(out), &out_len), 0);
    NGX_RTC_TEST_ASSERT_U64_EQ(out_len, 10u);

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_vring_push(&r, b, 6u), 0);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_vring_pop(&r, out, sizeof(out), &out_len), 0);
    NGX_RTC_TEST_ASSERT_U64_EQ(out_len, 6u);
    for (i = 0u; i < 6u; i++) {
        NGX_RTC_TEST_ASSERT_U64_EQ(out[i], (uint32_t)b[i]);
    }

    ngx_rtc_vring_destroy(&r);
}

NGX_RTC_TEST(vring_overflow_and_guards)
{
    ngx_rtc_vring_t r;
    uint8_t         out[8];
    uint32_t        out_len;
    uint8_t         payload[8];
    uint32_t        i;

    (void)memset(&r, 0, sizeof(r));
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_vring_init(&r, 0u), -1);

    /* capacity 16: two entries of len 4 (8 bytes each) fill it exactly. */
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_vring_init(&r, 16u), 0);
    (void)memset(payload, 0x5Au, sizeof(payload));

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_vring_push(&r, payload, 4u), 0);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_vring_push(&r, payload, 4u), 0);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_vring_push(&r, payload, 4u), -1); /* full */

    /* Empty pop and out_cap-too-small reject cleanly. */
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_vring_pop(&r, out, sizeof(out), &out_len), 0);
    NGX_RTC_TEST_ASSERT_U64_EQ(out_len, 4u);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_vring_pop(&r, out, 3u, &out_len), -1);

    /* Fill-and-drain repeatedly to exercise head/tail wrap of full entries. */
    for (i = 0u; i < 100u; i++) {
        NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_vring_push(&r, payload, 4u), 0);
        NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_vring_pop(&r, out, sizeof(out), &out_len), 0);
        NGX_RTC_TEST_ASSERT_U64_EQ(out_len, 4u);
    }

    ngx_rtc_vring_destroy(&r);
}

NGX_RTC_TEST(vring_count_tracks_entries)
{
    ngx_rtc_vring_t r;
    uint8_t         out[8];
    uint32_t        out_len;
    uint8_t         payload[4];

    (void)memset(&r, 0, sizeof(r));
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_vring_init(&r, 64u), 0);
    NGX_RTC_TEST_ASSERT_U64_EQ(ngx_rtc_vring_count(&r), 0u);

    (void)memset(payload, 0x5Au, sizeof(payload));
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_vring_push(&r, payload, 4u), 0);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_vring_push(&r, payload, 4u), 0);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_vring_push(&r, payload, 4u), 0);
    NGX_RTC_TEST_ASSERT_U64_EQ(ngx_rtc_vring_count(&r), 3u);

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_vring_pop(&r, out, sizeof(out), &out_len), 0);
    NGX_RTC_TEST_ASSERT_U64_EQ(ngx_rtc_vring_count(&r), 2u);

    /* Draining to empty returns to a zero count. */
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_vring_pop(&r, out, sizeof(out), &out_len), 0);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_vring_pop(&r, out, sizeof(out), &out_len), 0);
    NGX_RTC_TEST_ASSERT_U64_EQ(ngx_rtc_vring_count(&r), 0u);

    ngx_rtc_vring_destroy(&r);
}
