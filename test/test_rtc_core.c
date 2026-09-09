/*
 * test_rtc_core.c - host tests for the GOP ring (plaintext RTP cache) helpers
 * in ngx_rtc_core.c.
 *
 * ngx_rtc_core.c is compiled for the host with the test/include nginx stubs
 * (see the Makefile target and nginx_stub.c). Only the ring helpers are
 * exercised here:
 *   ngx_rtc_rtp_ring_push / ngx_rtc_rtp_ring_get / ngx_rtc_rtp_ring_replay.
 *
 * SRS reference: KernelRTCTest.NACKFetchRTPPacket (fetch by sequence, miss on
 * out-of-window sequence) plus the wrap-around retention semantics.
 *
 * Integration-test gap (not host-testable here): the NACK receive path that
 * filters by media_ssrc and maps the lost RTP sequence numbers to the right
 * source ring lives in ngx_rtc_stream_module.c (nginx), which is not built for
 * the host. The ring primitive below is the pure cache layer it will call.
 */

#include "ngx_rtc_test.h"
#include "ngx_rtc_core.h"
#include "ngx_rtc_session_fsm.h"

#include <stdlib.h>

#define CORE_PKT_LEN 16u

/* ------------------------------------------------------------------ */
/* Fake nginx connection: first member mirrors ngx_connection_t.send,  */
/* so ngx_rtc_session_send_rtp (compiled in core.o) can call through.  */
/* ------------------------------------------------------------------ */

typedef struct fake_conn_s fake_conn_t;
struct fake_conn_s
{
    int (*send)(fake_conn_t *c, unsigned char *buf, size_t n);
};

#define CORE_REPLAY_MAX 64

static int      g_replay_count;
static uint16_t g_replay_seq[CORE_REPLAY_MAX];
static uint32_t g_replay_len[CORE_REPLAY_MAX];

static int fake_conn_send(fake_conn_t *c, unsigned char *buf, size_t n)
{
    (void)c;
    if (g_replay_count < CORE_REPLAY_MAX)
    {
        g_replay_seq[g_replay_count] = (uint16_t)(((uint16_t)buf[2] << 8) |
                                                  (uint16_t)buf[3]);
        g_replay_len[g_replay_count] = (uint32_t)n;
    }
    g_replay_count++;
    return (int)n;   /* emulate a socket that sends the whole datagram */
}

/* A controllable send result so ngx_rtc_session_send_rtp's failure accounting
 * can be exercised without the real UDP socket. */
static int g_send_rc;

static int fake_conn_send_rc(fake_conn_t *c, unsigned char *buf, size_t n)
{
    (void)c;
    (void)buf;
    (void)n;
    return g_send_rc;
}

/* ------------------------------------------------------------------ */
/* Ring helpers.                                                       */
/* ------------------------------------------------------------------ */

static void make_rtp(uint8_t *buf, uint16_t seq, uint8_t fill)
{
    (void)memset(buf, 0, CORE_PKT_LEN);
    buf[0] = 0x80u;
    buf[1] = 102u;
    buf[2] = (uint8_t)(seq >> 8);
    buf[3] = (uint8_t)(seq & 0xFFu);
    buf[12] = fill;
    buf[13] = (uint8_t)(fill + 1u);
    buf[14] = (uint8_t)(fill + 2u);
    buf[15] = (uint8_t)(fill + 3u);
}

static void push_seq(ngx_rtc_rtp_ring_t *r, uint16_t seq, uint8_t is_gop)
{
    uint8_t pkt[CORE_PKT_LEN];

    make_rtp(pkt, seq, (uint8_t)seq);
    ngx_rtc_rtp_ring_push(r, pkt, sizeof(pkt), is_gop);
}

/* ------------------------------------------------------------------ */
/* Tests.                                                              */
/* ------------------------------------------------------------------ */

NGX_RTC_TEST(ring_get_seq_hit_miss)
{
    ngx_rtc_rtp_ring_t ring;
    const uint8_t *out = NULL;
    uint16_t out_len = 0;

    (void)memset(&ring, 0, sizeof(ring));

    /* Empty ring has nothing cached. */
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtp_ring_get(&ring, 0u, &out, &out_len),
                               NGX_RTC_ERR_PARSE);

    push_seq(&ring, 100u, 0);
    push_seq(&ring, 101u, 0);
    push_seq(&ring, 102u, 0);

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtp_ring_get(&ring, 100u, &out, &out_len),
                               NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT(NULL != out);
    NGX_RTC_TEST_ASSERT_I64_EQ(out_len, CORE_PKT_LEN);
    NGX_RTC_TEST_ASSERT_I64_EQ(out[2], 0u);   /* seq high byte */
    NGX_RTC_TEST_ASSERT_I64_EQ(out[3], 100u); /* seq low byte */

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtp_ring_get(&ring, 102u, &out, &out_len),
                               NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_I64_EQ(out[3], 102u);

    /* One past the newest retained packet and far outside the window. */
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtp_ring_get(&ring, 103u, &out, &out_len),
                               NGX_RTC_ERR_PARSE);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtp_ring_get(&ring, 200u, &out, &out_len),
                               NGX_RTC_ERR_PARSE);

    free(ring.slots);
}

NGX_RTC_TEST(ring_get_seq_wrap)
{
    ngx_rtc_rtp_ring_t ring;
    const uint8_t *out = NULL;
    uint16_t out_len = 0;

    (void)memset(&ring, 0, sizeof(ring));

    push_seq(&ring, 65533u, 0);
    push_seq(&ring, 65534u, 0);
    push_seq(&ring, 65535u, 0);
    push_seq(&ring, 0u, 0);
    push_seq(&ring, 1u, 0);

    /* The oldest retained packet is still reachable at the wrap boundary. */
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtp_ring_get(&ring, 65533u, &out, &out_len),
                               NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_I64_EQ(out[2], 0xFFu);
    NGX_RTC_TEST_ASSERT_I64_EQ(out[3], 0xFDu);

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtp_ring_get(&ring, 65535u, &out, &out_len),
                               NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtp_ring_get(&ring, 0u, &out, &out_len),
                               NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_I64_EQ(out[3], 0u);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtp_ring_get(&ring, 1u, &out, &out_len),
                               NGX_RTC_OK);

    /* 65532 is older than the oldest retained packet. */
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtp_ring_get(&ring, 65532u, &out, &out_len),
                               NGX_RTC_ERR_PARSE);
    /* 2 is exactly one past the newest retained packet (dist == count). */
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtp_ring_get(&ring, 2u, &out, &out_len),
                               NGX_RTC_ERR_PARSE);

    free(ring.slots);
}

NGX_RTC_TEST(ring_push_evicts_oldest)
{
    ngx_rtc_rtp_ring_t ring;
    const uint8_t *out = NULL;
    uint16_t out_len = 0;
    uint32_t i;

    (void)memset(&ring, 0, sizeof(ring));

    for (i = 1u; i <= 3000u; i++)
    {
        push_seq(&ring, (uint16_t)i, 0);
    }

    NGX_RTC_TEST_ASSERT_U64_EQ(ring.count, NGX_RTC_GOP_RING_CAP);

    /* Sequence 1 has been evicted; 953 is the oldest retained entry. */
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtp_ring_get(&ring, 1u, &out, &out_len),
                               NGX_RTC_ERR_PARSE);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtp_ring_get(&ring, 953u, &out, &out_len),
                               NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_I64_EQ(out[3], 0xB9u); /* 953 = 0x03B9 */

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtp_ring_get(&ring, 3000u, &out, &out_len),
                               NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtp_ring_get(&ring, 2999u, &out, &out_len),
                               NGX_RTC_OK);

    free(ring.slots);
}

static ngx_rtc_session_t g_replay_sess;
static fake_conn_t       g_replay_conn;

static void replay_session_make_ready(void)
{
    (void)memset(&g_replay_sess, 0, sizeof(g_replay_sess));
    ngx_rtc_session_fsm_init(&g_replay_sess.fsm, g_replay_sess.fsm_path,
                             NGX_RTC_SESSION_FSM_MAX_DEPTH, &g_replay_sess);
    (void)ngx_rtc_session_fsm_dispatch(&g_replay_sess.fsm,
                                       NGX_RTC_SESSION_EVT_STUN_BINDING);
    (void)ngx_rtc_session_fsm_dispatch(&g_replay_sess.fsm,
                                       NGX_RTC_SESSION_EVT_DTLS_PACKET);
    (void)ngx_rtc_session_fsm_dispatch(&g_replay_sess.fsm,
                                       NGX_RTC_SESSION_EVT_DTLS_HANDSHAKE_DONE);

    g_replay_conn.send = fake_conn_send;
    g_replay_sess.conn = (void *)&g_replay_conn;
}

NGX_RTC_TEST(ring_replay_clamps_to_latest_gop)
{
    ngx_rtc_rtp_ring_t ring;
    uint32_t i;
    uint32_t k;

    replay_session_make_ready();

    /* 10 packets; the IDR access unit starts at sequence 5. */
    (void)memset(&ring, 0, sizeof(ring));
    for (i = 0u; i < 10u; i++)
    {
        push_seq(&ring, (uint16_t)i, (i == 5u) ? 1u : 0u);
    }

    g_replay_count = 0;
    ngx_rtc_rtp_ring_replay(&ring, &g_replay_sess);

    /* Replay starts at the latest GOP start (seq 5), not the oldest packet. */
    NGX_RTC_TEST_ASSERT_I64_EQ(g_replay_count, 5);
    for (k = 0u; k < 5u; k++)
    {
        NGX_RTC_TEST_ASSERT_I64_EQ(g_replay_seq[k], (uint16_t)(5u + k));
        NGX_RTC_TEST_ASSERT_U64_EQ(g_replay_len[k], CORE_PKT_LEN);
    }

    free(ring.slots);

    /* Without a GOP marker the whole retained window is replayed. */
    (void)memset(&ring, 0, sizeof(ring));
    for (i = 0u; i < 10u; i++)
    {
        push_seq(&ring, (uint16_t)i, 0);
    }

    g_replay_count = 0;
    ngx_rtc_rtp_ring_replay(&ring, &g_replay_sess);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_replay_count, 10);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_replay_seq[0], 0u);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_replay_seq[9], 9u);

    free(ring.slots);

    /* Empty ring: replay is a no-op. */
    (void)memset(&ring, 0, sizeof(ring));
    g_replay_count = 0;
    ngx_rtc_rtp_ring_replay(&ring, &g_replay_sess);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_replay_count, 0);
}

NGX_RTC_TEST(ring_null_and_size_guards)
{
    ngx_rtc_rtp_ring_t ring;
    const uint8_t *out = NULL;
    uint16_t out_len = 0;
    uint8_t pkt[CORE_PKT_LEN];
    uint8_t big[NGX_RTC_MAX_RTP_PKT + 1u];

    (void)memset(&ring, 0, sizeof(ring));
    make_rtp(pkt, 0u, 0u);
    (void)memset(big, 0, sizeof(big));

    /* NULL arguments are rejected without touching memory. */
    ngx_rtc_rtp_ring_push(NULL, pkt, sizeof(pkt), 0);
    ngx_rtc_rtp_ring_push(&ring, NULL, sizeof(pkt), 0);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtp_ring_get(NULL, 0u, &out, &out_len),
                               NGX_RTC_ERR_INVALID);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtp_ring_get(&ring, 0u, NULL, &out_len),
                               NGX_RTC_ERR_INVALID);
    ngx_rtc_rtp_ring_replay(NULL, &g_replay_sess);
    ngx_rtc_rtp_ring_replay(&ring, NULL);

    /* Zero length and oversize packets are dropped before any allocation. */
    ngx_rtc_rtp_ring_push(&ring, pkt, 0u, 0);
    ngx_rtc_rtp_ring_push(&ring, big, sizeof(big), 0);
    NGX_RTC_TEST_ASSERT(NULL == ring.slots);
    NGX_RTC_TEST_ASSERT_U64_EQ(ring.count, 0u);
}

NGX_RTC_TEST(session_send_accounts_failures)
{
    ngx_rtc_session_t sess;
    fake_conn_t       conn;
    uint8_t           pkt[CORE_PKT_LEN];

    (void)memset(&sess, 0, sizeof(sess));
    ngx_rtc_session_fsm_init(&sess.fsm, sess.fsm_path,
                             NGX_RTC_SESSION_FSM_MAX_DEPTH, &sess);
    (void)ngx_rtc_session_fsm_dispatch(&sess.fsm,
                                       NGX_RTC_SESSION_EVT_STUN_BINDING);
    (void)ngx_rtc_session_fsm_dispatch(&sess.fsm,
                                       NGX_RTC_SESSION_EVT_DTLS_PACKET);
    (void)ngx_rtc_session_fsm_dispatch(&sess.fsm,
                                       NGX_RTC_SESSION_EVT_DTLS_HANDSHAKE_DONE);

    conn.send = fake_conn_send_rc;
    sess.conn = (void *)&conn;
    make_rtp(pkt, 0u, 0u);

    g_send_rc = -1; /* NGX_ERROR */
    ngx_rtc_session_send_rtp(&sess, pkt, sizeof(pkt));
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.send_failed, 1u);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.send_eagain, 0u);

    g_send_rc = -2; /* NGX_AGAIN */
    ngx_rtc_session_send_rtp(&sess, pkt, sizeof(pkt));
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.send_failed, 1u);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.send_eagain, 1u);

    g_send_rc = 5; /* short write, less than CORE_PKT_LEN */
    ngx_rtc_session_send_rtp(&sess, pkt, sizeof(pkt));
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.send_failed, 2u);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.send_eagain, 1u);
}

/* Build an SRTP_READY session wired to the seq-recording fake connection. */
static void rtx_ready_session(ngx_rtc_session_t *sess, fake_conn_t *conn)
{
    (void)memset(sess, 0, sizeof(*sess));
    ngx_rtc_session_fsm_init(&sess->fsm, sess->fsm_path,
                             NGX_RTC_SESSION_FSM_MAX_DEPTH, sess);
    (void)ngx_rtc_session_fsm_dispatch(&sess->fsm,
                                       NGX_RTC_SESSION_EVT_STUN_BINDING);
    (void)ngx_rtc_session_fsm_dispatch(&sess->fsm,
                                       NGX_RTC_SESSION_EVT_DTLS_PACKET);
    (void)ngx_rtc_session_fsm_dispatch(&sess->fsm,
                                       NGX_RTC_SESSION_EVT_DTLS_HANDSHAKE_DONE);
    conn->send = fake_conn_send;
    sess->conn = (void *)conn;
}

NGX_RTC_TEST(retransmit_dedup_per_window)
{
    ngx_rtc_source_t  src;
    ngx_rtc_session_t sess;
    fake_conn_t       conn;

    (void)memset(&src, 0, sizeof(src));
    rtx_ready_session(&sess, &conn);
    sess.source = &src;
    g_replay_count = 0;

    /* Cache four video packets in the source GOP ring, the first opening a GOP. */
    push_seq(&src.gop, 100u, 1u);
    push_seq(&src.gop, 101u, 0u);
    push_seq(&src.gop, 102u, 0u);
    push_seq(&src.gop, 103u, 0u);

    NGX_RTC_TEST_ASSERT(ngx_rtc_source_gop_ready(&src));

    /* 100 is retransmitted once, a duplicate NACK in the window is skipped. */
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_session_retransmit(&sess, 100u),
                               NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_replay_count, 1);
    NGX_RTC_TEST_ASSERT_U64_EQ(g_replay_seq[0], 100u);

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_session_retransmit(&sess, 100u),
                               NGX_RTC_ERR_PARSE);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_replay_count, 1); /* dedup: not re-sent */

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_session_retransmit(&sess, 102u),
                               NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_replay_count, 2);
    NGX_RTC_TEST_ASSERT_U64_EQ(g_replay_seq[1], 102u);

    /* Outside the retained window is a miss. */
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_session_retransmit(&sess, 999u),
                               NGX_RTC_ERR_PARSE);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_replay_count, 2);

    /* New NACK window (dedup set cleared): 100 may be sent again. */
    ngx_rtc_session_nack_reset(&sess);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_session_retransmit(&sess, 100u),
                               NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_replay_count, 3);

    free(src.gop.slots);
}

NGX_RTC_TEST(retransmit_misses_when_source_gop_empty)
{
    ngx_rtc_source_t  src;
    ngx_rtc_session_t sess;
    fake_conn_t       conn;

    (void)memset(&src, 0, sizeof(src));
    rtx_ready_session(&sess, &conn);
    sess.source = &src;

    /* A non-publisher worker's GOP ring is not yet populated: no local cache,
     * so a retransmit is a miss (the caller routes to the shm retransmit ring). */
    NGX_RTC_TEST_ASSERT(0 == ngx_rtc_source_gop_ready(&src));
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_session_retransmit(&sess, 100u),
                               NGX_RTC_ERR_PARSE);
}

/* ------------------------------------------------------------------ */
/* Pacing token bucket + TWCC AIMD bitrate adaptation.                 */
/* ------------------------------------------------------------------ */

NGX_RTC_TEST(pacer_init_clamps_rate_and_fills_bucket)
{
    ngx_rtc_session_t sess;

    (void)memset(&sess, 0, sizeof(sess));

    /* Below the floor: clamped to 64 kbps, bucket = 64e3*100/8000 = 800 B. */
    ngx_rtc_session_pacer_init(&sess, 1000u);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.pacer_target_bps, NGX_RTC_PACER_MIN_BPS);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.pacer_tokens, 800u);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.pacer_started, 0u);

    /* Above the ceiling: clamped to 8 Mbps, bucket = 8e6*100/8000 = 100000 B. */
    ngx_rtc_session_pacer_init(&sess, 99999999u);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.pacer_target_bps, NGX_RTC_PACER_MAX_BPS);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.pacer_tokens, 100000u);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.pacer_started, 0u);
}

NGX_RTC_TEST(pacer_admit_grants_denies_and_refills)
{
    ngx_rtc_session_t sess;

    (void)memset(&sess, 0, sizeof(sess));

    /* 800 kbps -> 100 bytes/ms, bucket = 100 * 100 ms = 10000 bytes. */
    ngx_rtc_session_pacer_init(&sess, 800000u);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.pacer_tokens, 10000u);

    /* First admit only anchors the clock at now_ms=0 (bucket starts full). */
    NGX_RTC_TEST_ASSERT_I64_EQ(
        ngx_rtc_session_pacer_admit(&sess, 1000u, 0u), NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.pacer_tokens, 9000u);

    /* Same instant, not enough tokens: rejected, tokens untouched. */
    NGX_RTC_TEST_ASSERT_I64_EQ(
        ngx_rtc_session_pacer_admit(&sess, 9001u, 0u), NGX_RTC_AGAIN);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.pacer_tokens, 9000u);

    /* 10 ms elapsed -> +1000 bytes, then a 200-byte packet is charged. */
    NGX_RTC_TEST_ASSERT_I64_EQ(
        ngx_rtc_session_pacer_admit(&sess, 200u, 10u), NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.pacer_tokens, 9800u);

    /* A packet larger than the whole burst bucket is always rejected. */
    NGX_RTC_TEST_ASSERT_I64_EQ(
        ngx_rtc_session_pacer_admit(&sess, 20000u, 10u), NGX_RTC_AGAIN);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.pacer_tokens, 9800u);

    /* Exact-fit drain leaves the bucket at zero. */
    NGX_RTC_TEST_ASSERT_I64_EQ(
        ngx_rtc_session_pacer_admit(&sess, 9800u, 10u), NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.pacer_tokens, 0u);
}

NGX_RTC_TEST(on_twcc_high_loss_decreases_rate)
{
    ngx_rtc_session_t sess;

    (void)memset(&sess, 0, sizeof(sess));
    ngx_rtc_session_pacer_init(&sess, 1000000u);

    /* First feedback only opens the window (no decision yet). */
    ngx_rtc_session_on_twcc(&sess, 6u, 94u, 0u);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.pacer_target_bps, 1000000u);

    /* 500 ms later the window holds 12/200 lost = 6% > 5%: x0.85. */
    ngx_rtc_session_on_twcc(&sess, 6u, 94u, 500u);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.pacer_target_bps, 850000u);
}

NGX_RTC_TEST(on_twcc_zero_loss_increases_rate)
{
    ngx_rtc_session_t sess;

    (void)memset(&sess, 0, sizeof(sess));
    ngx_rtc_session_pacer_init(&sess, 1000000u);

    ngx_rtc_session_on_twcc(&sess, 0u, 100u, 0u);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.pacer_target_bps, 1000000u);

    /* 0% loss < 2%: +8% -> 1,080,000 bps. */
    ngx_rtc_session_on_twcc(&sess, 0u, 100u, 500u);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.pacer_target_bps, 1080000u);
}

NGX_RTC_TEST(on_twcc_clamps_rate_bounds)
{
    ngx_rtc_session_t lower;
    ngx_rtc_session_t upper;

    (void)memset(&lower, 0, sizeof(lower));
    (void)memset(&upper, 0, sizeof(upper));

    /* Decrease from 70 kbps would give 59.5 kbps: clamped to the 64 kbps floor. */
    ngx_rtc_session_pacer_init(&lower, 70000u);
    ngx_rtc_session_on_twcc(&lower, 6u, 94u, 0u);
    ngx_rtc_session_on_twcc(&lower, 6u, 94u, 500u);
    NGX_RTC_TEST_ASSERT_U64_EQ(lower.pacer_target_bps, NGX_RTC_PACER_MIN_BPS);

    /* Increase from 7.9 Mbps would give 8.532 Mbps: clamped to the 8 Mbps cap. */
    ngx_rtc_session_pacer_init(&upper, 7900000u);
    ngx_rtc_session_on_twcc(&upper, 0u, 100u, 0u);
    ngx_rtc_session_on_twcc(&upper, 0u, 100u, 500u);
    NGX_RTC_TEST_ASSERT_U64_EQ(upper.pacer_target_bps, NGX_RTC_PACER_MAX_BPS);
}
