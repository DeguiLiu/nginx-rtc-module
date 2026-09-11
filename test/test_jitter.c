/*
 * test_jitter.c - host unit tests for the WHIP uplink reorder buffer.
 */

#include "ngx_rtc_test.h"
#include "ngx_rtc_jitter.h"

#include <string.h>

#define JITTER_TEST_LEN  32u

static uint16_t g_emit_seq[256];
static uint8_t  g_emit_gop[256];
static uint32_t g_emit_count;

static int32_t
jitter_record_emit(void *opaque, const uint8_t *rtp, uint32_t len,
                   uint8_t is_gop_start)
{
    (void)opaque;
    (void)len;

    if (g_emit_count < 256u) {
        g_emit_seq[g_emit_count] = (uint16_t)(((uint16_t) rtp[2] << 8)
                                              | (uint16_t) rtp[3]);
        g_emit_gop[g_emit_count] = (uint8_t) (0 != is_gop_start ? 1 : 0);
        g_emit_count++;
    }
    return NGX_RTC_OK;
}

static void
jitter_make_rtp(uint8_t *buf, uint16_t seq)
{
    (void)memset(buf, 0, JITTER_TEST_LEN);
    buf[0] = 0x80u;
    buf[1] = 102u;
    buf[2] = (uint8_t)(seq >> 8);
    buf[3] = (uint8_t)(seq & 0xFFu);
}

static void
jitter_reset_recorder(void)
{
    g_emit_count = 0;
}

NGX_RTC_TEST(jitter_in_order_emits_immediately)
{
    ngx_rtc_jitter_t j;
    uint8_t          pkt[JITTER_TEST_LEN];

    (void)memset(&j, 0, sizeof(j));
    jitter_reset_recorder();

    jitter_make_rtp(pkt, 0u);
    (void)ngx_rtc_jitter_push(&j, pkt, sizeof(pkt), 0u, 1u, 0u,
                              jitter_record_emit, NULL);
    jitter_make_rtp(pkt, 1u);
    (void)ngx_rtc_jitter_push(&j, pkt, sizeof(pkt), 1u, 0u, 0u,
                              jitter_record_emit, NULL);

    NGX_RTC_TEST_ASSERT_I64_EQ(g_emit_count, 2);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_emit_seq[0], 0u);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_emit_seq[1], 1u);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_emit_gop[0], 1u);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_emit_gop[1], 0u);
}

NGX_RTC_TEST(jitter_reorders_and_drains)
{
    ngx_rtc_jitter_t j;
    uint8_t          pkt[JITTER_TEST_LEN];

    (void)memset(&j, 0, sizeof(j));
    jitter_reset_recorder();

    /* seq 0 anchors the stream and emits. */
    jitter_make_rtp(pkt, 0u);
    (void)ngx_rtc_jitter_push(&j, pkt, sizeof(pkt), 0u, 0u, 0u,
                              jitter_record_emit, NULL);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_emit_count, 1);

    /* seq 2 arrives before seq 1: it is cached, nothing new emits yet. */
    jitter_make_rtp(pkt, 2u);
    (void)ngx_rtc_jitter_push(&j, pkt, sizeof(pkt), 2u, 0u, 0u,
                              jitter_record_emit, NULL);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_emit_count, 1);

    /* seq 1 unlocks the cached seq 2. */
    jitter_make_rtp(pkt, 1u);
    (void)ngx_rtc_jitter_push(&j, pkt, sizeof(pkt), 1u, 0u, 0u,
                              jitter_record_emit, NULL);

    NGX_RTC_TEST_ASSERT_I64_EQ(g_emit_count, 3);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_emit_seq[1], 1u);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_emit_seq[2], 2u);
}

NGX_RTC_TEST(jitter_gap_timeout_skips)
{
    ngx_rtc_jitter_t j;
    uint8_t          pkt[JITTER_TEST_LEN];

    (void)memset(&j, 0, sizeof(j));
    jitter_reset_recorder();

    jitter_make_rtp(pkt, 0u);
    (void)ngx_rtc_jitter_push(&j, pkt, sizeof(pkt), 0u, 0u, 0u,
                              jitter_record_emit, NULL);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_emit_count, 1);

    /* seq 1 is lost: seq 2 opens a gap at t=10. */
    jitter_make_rtp(pkt, 2u);
    (void)ngx_rtc_jitter_push(&j, pkt, sizeof(pkt), 2u, 0u, 10u,
                              jitter_record_emit, NULL);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_emit_count, 1);

    /* seq 3 arrives after the 50 ms timeout: the gap is skipped, 2 and 3 emit. */
    jitter_make_rtp(pkt, 3u);
    (void)ngx_rtc_jitter_push(&j, pkt, sizeof(pkt), 3u, 0u, 70u,
                              jitter_record_emit, NULL);

    NGX_RTC_TEST_ASSERT_I64_EQ(g_emit_count, 3);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_emit_seq[1], 2u);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_emit_seq[2], 3u);
}

NGX_RTC_TEST(jitter_stale_duplicate_dropped)
{
    ngx_rtc_jitter_t j;
    uint8_t          pkt[JITTER_TEST_LEN];
    uint16_t         seq;

    (void)memset(&j, 0, sizeof(j));
    jitter_reset_recorder();

    for (seq = 0u; seq < 3u; seq++) {
        jitter_make_rtp(pkt, seq);
        (void)ngx_rtc_jitter_push(&j, pkt, sizeof(pkt), seq, 0u, 0u,
                                  jitter_record_emit, NULL);
    }
    NGX_RTC_TEST_ASSERT_I64_EQ(g_emit_count, 3);

    /* A duplicate of an already-emitted sequence is dropped. */
    jitter_make_rtp(pkt, 1u);
    (void)ngx_rtc_jitter_push(&j, pkt, sizeof(pkt), 1u, 0u, 0u,
                              jitter_record_emit, NULL);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_emit_count, 3);
}

NGX_RTC_TEST(jitter_far_ahead_reanchors)
{
    ngx_rtc_jitter_t j;
    uint8_t          pkt[JITTER_TEST_LEN];
    uint16_t         seq;

    (void)memset(&j, 0, sizeof(j));
    jitter_reset_recorder();

    /* seq 0 anchors the stream; next_seq is 1. */
    jitter_make_rtp(pkt, 0u);
    (void)ngx_rtc_jitter_push(&j, pkt, sizeof(pkt), 0u, 0u, 0u,
                              jitter_record_emit, NULL);

    /* seq 10 is in-window and cached at slot 10. */
    jitter_make_rtp(pkt, 10u);
    (void)ngx_rtc_jitter_push(&j, pkt, sizeof(pkt), 10u, 0u, 1u,
                              jitter_record_emit, NULL);

    /* seq 138 is far ahead (dist 137 >= CAP) and aliases slot 10. The guard
     * must NOT cache it in place (that would corrupt the pending seq 10);
     * instead the buffer resets and re-anchors at 138. */
    jitter_make_rtp(pkt, 138u);
    (void)ngx_rtc_jitter_push(&j, pkt, sizeof(pkt), 138u, 0u, 2u,
                              jitter_record_emit, NULL);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_emit_count, 2);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_emit_seq[1], 138u);
    NGX_RTC_TEST_ASSERT_U64_EQ(j.next_seq, 139u);

    /* In-flight packets of the previous generation arrive late: dropped as
     * stale, never emitted out of order. */
    jitter_make_rtp(pkt, 5u);
    (void)ngx_rtc_jitter_push(&j, pkt, sizeof(pkt), 5u, 0u, 3u,
                              jitter_record_emit, NULL);
    jitter_make_rtp(pkt, 10u);
    (void)ngx_rtc_jitter_push(&j, pkt, sizeof(pkt), 10u, 0u, 4u,
                              jitter_record_emit, NULL);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_emit_count, 2);

    /* The stream continues normally from the new anchor. */
    for (seq = 139u; seq <= 141u; seq++) {
        jitter_make_rtp(pkt, seq);
        (void)ngx_rtc_jitter_push(&j, pkt, sizeof(pkt), seq, 0u,
                                  (uint32_t) (5u + seq), jitter_record_emit,
                                  NULL);
    }
    NGX_RTC_TEST_ASSERT_I64_EQ(g_emit_count, 5);
    for (seq = 0u; seq < 3u; seq++) {
        NGX_RTC_TEST_ASSERT_I64_EQ(g_emit_seq[2u + seq], (uint16_t) (139u + seq));
    }
}

NGX_RTC_TEST(jitter_far_ahead_does_not_open_gap)
{
    ngx_rtc_jitter_t j;
    uint8_t          pkt[JITTER_TEST_LEN];

    (void)memset(&j, 0, sizeof(j));
    jitter_reset_recorder();

    jitter_make_rtp(pkt, 0u);
    (void)ngx_rtc_jitter_push(&j, pkt, sizeof(pkt), 0u, 0u, 0u,
                              jitter_record_emit, NULL);

    /* Anomalous +1000 jump: re-anchor emits it and moves next_seq directly.
     * Without the guard the packet would be cached 1000 sequences ahead and
     * the gap timeout would skip_gap() across ~65536 candidate slots. */
    jitter_make_rtp(pkt, 1000u);
    (void)ngx_rtc_jitter_push(&j, pkt, sizeof(pkt), 1000u, 0u, 100u,
                              jitter_record_emit, NULL);

    NGX_RTC_TEST_ASSERT_I64_EQ(g_emit_count, 2);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_emit_seq[1], 1000u);
    NGX_RTC_TEST_ASSERT_U64_EQ(j.next_seq, 1001u);
    NGX_RTC_TEST_ASSERT_U64_EQ(j.gap_ts, 0u);
}

NGX_RTC_TEST(jitter_reset_clears_state)
{
    ngx_rtc_jitter_t j;
    uint8_t          pkt[JITTER_TEST_LEN];

    (void)memset(&j, 0, sizeof(j));
    jitter_reset_recorder();

    jitter_make_rtp(pkt, 5u);
    (void)ngx_rtc_jitter_push(&j, pkt, sizeof(pkt), 5u, 0u, 0u,
                              jitter_record_emit, NULL);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_emit_count, 1);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_emit_seq[0], 5u);

    /* After reset the buffer re-anchors at the next packet. */
    ngx_rtc_jitter_reset(&j);
    jitter_reset_recorder();
    jitter_make_rtp(pkt, 100u);
    (void)ngx_rtc_jitter_push(&j, pkt, sizeof(pkt), 100u, 0u, 0u,
                              jitter_record_emit, NULL);

    NGX_RTC_TEST_ASSERT_I64_EQ(g_emit_count, 1);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_emit_seq[0], 100u);
}

NGX_RTC_TEST(jitter_gap_timer_survives_partial_drain)
{
    ngx_rtc_jitter_t j;
    uint8_t          pkt[JITTER_TEST_LEN];

    (void)memset(&j, 0, sizeof(j));
    jitter_reset_recorder();

    /* seq 0 anchors the stream; next_seq becomes 1. */
    jitter_make_rtp(pkt, 0u);
    (void)ngx_rtc_jitter_push(&j, pkt, sizeof(pkt), 0u, 0u, 0u,
                              jitter_record_emit, NULL);

    /* seq 3 arrives first: it is cached and the gap at seq 1 opens at t=10. */
    jitter_make_rtp(pkt, 3u);
    (void)ngx_rtc_jitter_push(&j, pkt, sizeof(pkt), 3u, 0u, 10u,
                              jitter_record_emit, NULL);
    NGX_RTC_TEST_ASSERT_U64_EQ(j.gap_ts, 10u);

    /* seq 1 arrives late but in order: it emits and the drain stops at the
     * still missing seq 2, leaving the cached seq 3 ahead of next_seq. The
     * gap at seq 2 is still open, so its timer must keep running. */
    jitter_make_rtp(pkt, 1u);
    (void)ngx_rtc_jitter_push(&j, pkt, sizeof(pkt), 1u, 0u, 20u,
                              jitter_record_emit, NULL);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_emit_count, 2);
    NGX_RTC_TEST_ASSERT_U64_EQ(j.gap_ts, 10u);

    /* seq 4 arrives 60 ms after the gap opened. The timer must still date
     * from t=10, so the timeout fires now: seq 2 is skipped and 3 and 4 emit.
     * Had the in-order branch cleared gap_ts the timer would restart at t=70
     * and the recovery would stall for one more whole window. */
    jitter_make_rtp(pkt, 4u);
    (void)ngx_rtc_jitter_push(&j, pkt, sizeof(pkt), 4u, 0u, 70u,
                              jitter_record_emit, NULL);

    NGX_RTC_TEST_ASSERT_I64_EQ(g_emit_count, 4);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_emit_seq[2], 3u);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_emit_seq[3], 4u);
    NGX_RTC_TEST_ASSERT_U64_EQ(j.n_skipped, 1u);
    NGX_RTC_TEST_ASSERT_U64_EQ(j.gap_ts, 0u);
}

/* The timeout is a deadline, not a threshold that has to be passed: a packet
 * arriving exactly gap_timeout_ms after the gap opened must already give up on
 * it. The comparison is >= for that reason, and a strict > would hold the queue
 * for one extra whole window -- on a sender whose packets land on neat
 * multiples of the configured interval, that is every window, so the recovery
 * would be a permanent timeout behind. */
NGX_RTC_TEST(jitter_gap_timer_fires_on_the_exact_timeout)
{
    ngx_rtc_jitter_t j;
    uint8_t          pkt[JITTER_TEST_LEN];

    (void)memset(&j, 0, sizeof(j));
    jitter_reset_recorder();

    /* seq 0 anchors the stream; next_seq becomes 1. */
    jitter_make_rtp(pkt, 0u);
    (void)ngx_rtc_jitter_push(&j, pkt, sizeof(pkt), 0u, 0u, 0u,
                              jitter_record_emit, NULL);

    /* seq 3 arrives first: it is cached and the gap at seq 1 opens at t=10. */
    jitter_make_rtp(pkt, 3u);
    (void)ngx_rtc_jitter_push(&j, pkt, sizeof(pkt), 3u, 0u, 10u,
                              jitter_record_emit, NULL);
    NGX_RTC_TEST_ASSERT_U64_EQ(j.gap_ts, 10u);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_emit_count, 1);

    /* seq 4 arrives exactly on the deadline. Nothing has arrived to fill the
     * gap, so the timer alone decides: on the boundary it must give up, skip
     * both missing sequences, and let 3 and 4 out. */
    jitter_make_rtp(pkt, 4u);
    (void)ngx_rtc_jitter_push(&j, pkt, sizeof(pkt), 4u, 0u,
                              10u + NGX_RTC_JITTER_TIMEOUT_MS,
                              jitter_record_emit, NULL);

    NGX_RTC_TEST_ASSERT_I64_EQ(g_emit_count, 3);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_emit_seq[1], 3u);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_emit_seq[2], 4u);
    NGX_RTC_TEST_ASSERT_U64_EQ(j.n_skipped, 2u);
    NGX_RTC_TEST_ASSERT_U64_EQ(j.gap_ts, 0u);
}
