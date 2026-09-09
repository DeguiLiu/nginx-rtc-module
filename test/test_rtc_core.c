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

NGX_RTC_TEST(rtx_retransmit_dedup_per_window)
{
    ngx_rtc_session_t sess;
    fake_conn_t       conn;
    uint8_t           pkt[CORE_PKT_LEN];

    rtx_ready_session(&sess, &conn);
    g_replay_count = 0;

    /* Cache four video packets, the first opening a GOP. */
    make_rtp(pkt, 100u, 0u);
    ngx_rtc_session_rtx_push(&sess, pkt, sizeof(pkt), 1u);
    make_rtp(pkt, 101u, 0u);
    ngx_rtc_session_rtx_push(&sess, pkt, sizeof(pkt), 0u);
    make_rtp(pkt, 102u, 0u);
    ngx_rtc_session_rtx_push(&sess, pkt, sizeof(pkt), 0u);
    make_rtp(pkt, 103u, 0u);
    ngx_rtc_session_rtx_push(&sess, pkt, sizeof(pkt), 0u);

    /* First window: 100 is retransmitted once, a duplicate NACK is skipped. */
    sess.rtx_gen = 1;
    NGX_RTC_TEST_ASSERT_I64_EQ(
        ngx_rtc_session_rtx_retransmit(&sess, 100u), NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_replay_count, 1);
    NGX_RTC_TEST_ASSERT_U64_EQ(g_replay_seq[0], 100u);

    NGX_RTC_TEST_ASSERT_I64_EQ(
        ngx_rtc_session_rtx_retransmit(&sess, 100u), NGX_RTC_ERR_PARSE);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_replay_count, 1); /* dedup: not re-sent */

    NGX_RTC_TEST_ASSERT_I64_EQ(
        ngx_rtc_session_rtx_retransmit(&sess, 102u), NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_replay_count, 2);
    NGX_RTC_TEST_ASSERT_U64_EQ(g_replay_seq[1], 102u);

    /* Outside the retained window is a miss. */
    NGX_RTC_TEST_ASSERT_I64_EQ(
        ngx_rtc_session_rtx_retransmit(&sess, 999u), NGX_RTC_ERR_PARSE);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_replay_count, 2);

    /* New NACK window (generation bump): 100 may be sent again. */
    sess.rtx_gen = 2;
    NGX_RTC_TEST_ASSERT_I64_EQ(
        ngx_rtc_session_rtx_retransmit(&sess, 100u), NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_replay_count, 3);

    ngx_rtc_session_rtx_free(&sess);
}

NGX_RTC_TEST(rtx_replay_gop_clamps_to_latest_gop)
{
    ngx_rtc_session_t sess;
    fake_conn_t       conn;
    uint8_t           pkt[CORE_PKT_LEN];
    uint32_t          i;

    rtx_ready_session(&sess, &conn);
    g_replay_count = 0;

    /* A first GOP (seq 100..103), then a newer GOP starting at 200. */
    make_rtp(pkt, 100u, 0u);
    ngx_rtc_session_rtx_push(&sess, pkt, sizeof(pkt), 1u);
    for (i = 101u; i < 104u; i++) {
        make_rtp(pkt, (uint16_t)i, 0u);
        ngx_rtc_session_rtx_push(&sess, pkt, sizeof(pkt), 0u);
    }
    make_rtp(pkt, 200u, 0u);
    ngx_rtc_session_rtx_push(&sess, pkt, sizeof(pkt), 1u);
    for (i = 201u; i < 204u; i++) {
        make_rtp(pkt, (uint16_t)i, 0u);
        ngx_rtc_session_rtx_push(&sess, pkt, sizeof(pkt), 0u);
    }

    ngx_rtc_session_rtx_replay_gop(&sess);

    /* PLI replay must start at the latest IDR (200) and skip the old GOP. */
    NGX_RTC_TEST_ASSERT_I64_EQ(g_replay_count, 4);
    NGX_RTC_TEST_ASSERT_U64_EQ(g_replay_seq[0], 200u);
    NGX_RTC_TEST_ASSERT_U64_EQ(g_replay_seq[3], 203u);

    ngx_rtc_session_rtx_free(&sess);
}

NGX_RTC_TEST(rtx_push_reserves_lazily_and_caches)
{
    ngx_rtc_session_t sess;
    fake_conn_t       conn;
    uint8_t           pkt[CORE_PKT_LEN];
    const uint8_t    *out = NULL;
    uint16_t          out_len = 0;

    rtx_ready_session(&sess, &conn);

    /* Nothing allocated until the first video packet arrives. */
    NGX_RTC_TEST_ASSERT(NULL == sess.rtx.slots);

    make_rtp(pkt, 100u, 0u);
    ngx_rtc_session_rtx_push(&sess, pkt, sizeof(pkt), 1u);

    NGX_RTC_TEST_ASSERT(NULL != sess.rtx.slots);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.rtx.capacity, NGX_RTC_RTX_RING_CAP);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtp_ring_get(&sess.rtx, 100u,
                                                    &out, &out_len),
                               NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_I64_EQ(out_len, CORE_PKT_LEN);

    ngx_rtc_session_rtx_free(&sess);
    NGX_RTC_TEST_ASSERT(NULL == sess.rtx.slots);
}
