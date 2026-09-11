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
/* Fake nginx connection.                                              */
/*                                                                     */
/* This used to be a bare function pointer whose type was claimed to    */
/* mirror ngx_connection_t.send, and every test reached it through      */
/* `(ngx_connection_t *)&conn`. That only worked because the stub       */
/* header happened to put `send` first; real nginx puts read/write/fd   */
/* ahead of it, so the cast landed on the wrong field and the send path */
/* called through a NULL pointer. Wrapping the real struct keeps the    */
/* cast valid -- the member is first, so the addresses coincide -- and  */
/* makes the field the compiler type-checks the same one the production */
/* code calls.                                                          */
/* ------------------------------------------------------------------ */

typedef struct fake_conn_s fake_conn_t;
struct fake_conn_s
{
    ngx_connection_t  c;
};

/* nginx's ngx_log_error() macro reads log->log_level before deciding whether to
 * call ngx_log_error_core, so every connection the send path touches needs a
 * real ngx_log_t -- a NULL log is a dereference, not a no-op. Level 0 is below
 * every level, so nothing is printed. */
static ngx_log_t core_test_log;

#define CORE_REPLAY_MAX 64

static int      g_replay_count;
static uint16_t g_replay_seq[CORE_REPLAY_MAX];
static uint32_t g_replay_len[CORE_REPLAY_MAX];

static ssize_t fake_conn_send(ngx_connection_t *c, unsigned char *buf, size_t n)
{
    (void)c;
    if (g_replay_count < CORE_REPLAY_MAX)
    {
        g_replay_seq[g_replay_count] = (uint16_t)(((uint16_t)buf[2] << 8) |
                                                  (uint16_t)buf[3]);
        g_replay_len[g_replay_count] = (uint32_t)n;
    }
    g_replay_count++;
    return (ssize_t)n;   /* emulate a socket that sends the whole datagram */
}

/* A controllable send result so ngx_rtc_session_send_rtp's failure accounting
 * can be exercised without the real UDP socket. */
static int g_send_rc;

/* Its counterpart for the SRTP failure branch, defined in nginx_stub.c so that
 * every other case keeps the pass-through. */
extern int ngx_rtc_test_srtp_protect_rc;

static ssize_t fake_conn_send_rc(ngx_connection_t *c, unsigned char *buf, size_t n)
{
    (void)c;
    (void)buf;
    (void)n;
    return g_send_rc;
}

/* Captures the last datagram so the send path's header rewriting (PT + TWCC
 * extension insertion) can be asserted byte for byte. */
static uint8_t g_cap_buf[NGX_RTC_CIPHER_CAP];
static size_t  g_cap_len;

static ssize_t fake_conn_send_capture(ngx_connection_t *c, unsigned char *buf, size_t n)
{
    (void)c;
    if (n <= sizeof(g_cap_buf))
    {
        (void)memcpy(g_cap_buf, buf, n);
        g_cap_len = n;
    }
    return (int)n;
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

/* push_seq, but able to set the RTP marker bit that ends an access unit. */
static void push_seq_marker(ngx_rtc_rtp_ring_t *r, uint16_t seq, uint8_t is_gop,
                            uint8_t marker)
{
    uint8_t pkt[CORE_PKT_LEN];

    make_rtp(pkt, seq, (uint8_t)seq);
    if (0 != marker)
    {
        pkt[1] = (uint8_t)(pkt[1] | NGX_RTC_RTP_MARKER);
    }
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

    g_replay_conn.c.log = &core_test_log;
    g_replay_conn.c.send = fake_conn_send;
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

/* The ring holds the keyframe plus every frame sent after it, but only the
 * keyframe is needed to start decoding. Replaying the whole GOP is a synchronous
 * burst of several hundred packets that lands while a new viewer's receive path
 * is still warming up, so it loses exactly the packets the first decoded frame
 * needs -- which is what a torn first frame looks like, and why the opening of a
 * session was unreliable. The RTP marker bit ends a keyframe access unit, so the
 * replay can stop there: one complete, immediately decodable frame, an order of
 * magnitude smaller burst, no wait for the next IDR. */
NGX_RTC_TEST(ring_replay_stops_at_the_end_of_the_keyframe)
{
    ngx_rtc_rtp_ring_t ring;
    uint16_t           i;

    replay_session_make_ready();

    /* The IDR access unit is the GOP start (seq 2) plus the two fragments after
     * it; seq 4 carries the marker that closes the access unit. Everything from
     * seq 5 on belongs to the same GOP but is a different access unit. */
    (void)memset(&ring, 0, sizeof(ring));
    for (i = 0u; i < 10u; i++)
    {
        push_seq_marker(&ring, i, (i == 2u) ? 1u : 0u, (i == 4u) ? 1u : 0u);
    }

    g_replay_count = 0;
    ngx_rtc_rtp_ring_replay(&ring, &g_replay_sess);

    NGX_RTC_TEST_ASSERT_I64_EQ(g_replay_count, 3);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_replay_seq[0], 2u);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_replay_seq[1], 3u);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_replay_seq[2], 4u);

    free(ring.slots);
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

    conn.c.log = &core_test_log;
    conn.c.send = fake_conn_send_rc;
    sess.conn = (void *)&conn;
    make_rtp(pkt, 0u, 0u);

    g_send_rc = -1; /* NGX_ERROR */
    ngx_rtc_session_send_rtp(&sess, pkt, sizeof(pkt), 0);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.send_failed, 1u);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.send_eagain, 0u);

    g_send_rc = -2; /* NGX_AGAIN */
    ngx_rtc_session_send_rtp(&sess, pkt, sizeof(pkt), 0);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.send_failed, 1u);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.send_eagain, 1u);

    g_send_rc = 5; /* short write, less than CORE_PKT_LEN */
    ngx_rtc_session_send_rtp(&sess, pkt, sizeof(pkt), 0);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.send_failed, 2u);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.send_eagain, 1u);
}

/* Arm the fake session the send-path tests share: ready FSM, fake socket,
 * zeroed video SSRC so make_rtp's packet classifies as video. */
static void
core_make_video_sess(ngx_rtc_session_t *sess, ngx_rtc_source_t *src,
                     fake_conn_t *conn)
{
    (void)memset(sess, 0, sizeof(*sess));
    (void)memset(src, 0, sizeof(*src));
    ngx_rtc_session_fsm_init(&sess->fsm, sess->fsm_path,
                             NGX_RTC_SESSION_FSM_MAX_DEPTH, sess);
    (void)ngx_rtc_session_fsm_dispatch(&sess->fsm,
                                       NGX_RTC_SESSION_EVT_STUN_BINDING);
    (void)ngx_rtc_session_fsm_dispatch(&sess->fsm,
                                       NGX_RTC_SESSION_EVT_DTLS_PACKET);
    (void)ngx_rtc_session_fsm_dispatch(&sess->fsm,
                                       NGX_RTC_SESSION_EVT_DTLS_HANDSHAKE_DONE);

    conn->c.log = &core_test_log;
    conn->c.send = fake_conn_send_rc;
    sess->conn = (void *)conn;
    sess->source = src;
    src->video_ssrc = 0u;
}

/* One NGX_AGAIN is a transient burst (a UDP socket buffer momentarily full);
 * a hard send error or a short write is not. Only a sustained EAGAIN run may
 * freeze the stream until the next keyframe, because that costs up to a whole
 * GOP interval while a single dropped packet costs one packet. */
NGX_RTC_TEST(session_eagain_streak_arms_drop_until_gop)
{
    ngx_rtc_session_t sess;
    ngx_rtc_source_t  src;
    fake_conn_t       conn;
    uint8_t           pkt[CORE_PKT_LEN];

    core_make_video_sess(&sess, &src, &conn);
    make_rtp(pkt, 0u, 0u);

    /* A single EAGAIN drops the packet but the stream keeps flowing. */
    g_send_rc = -2; /* NGX_AGAIN */
    NGX_RTC_TEST_ASSERT_I64_EQ(
            ngx_rtc_session_send_rtp(&sess, pkt, sizeof(pkt), 1), 0);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.send_eagain, 1u);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.drop_until_gop, 0u);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.send_eagain_streak, 1u);

    /* A send that drains resets the run. */
    g_send_rc = (int)sizeof(pkt);
    NGX_RTC_TEST_ASSERT_I64_EQ(
            ngx_rtc_session_send_rtp(&sess, pkt, sizeof(pkt), 1), 1);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.send_eagain_streak, 0u);

    /* Three consecutive EAGAINs mean real backpressure: arm the recovery. */
    g_send_rc = -2;
    (void)ngx_rtc_session_send_rtp(&sess, pkt, sizeof(pkt), 1);
    (void)ngx_rtc_session_send_rtp(&sess, pkt, sizeof(pkt), 1);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.drop_until_gop, 0u);
    (void)ngx_rtc_session_send_rtp(&sess, pkt, sizeof(pkt), 1);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.drop_until_gop, 1u);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.send_eagain_streak, 0u);

    /* Armed: a non-GOP video packet is dropped without reaching the socket. */
    g_send_rc = (int)sizeof(pkt);
    NGX_RTC_TEST_ASSERT_I64_EQ(
            ngx_rtc_session_send_rtp(&sess, pkt, sizeof(pkt), 0), 0);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.drop_until_gop, 1u);

    /* A failed IDR send (socket still full) does NOT clear the flag, so the
     * stream does not resume mid-GOP on a keyframe that never left. */
    g_send_rc = -2; /* NGX_AGAIN */
    NGX_RTC_TEST_ASSERT_I64_EQ(
            ngx_rtc_session_send_rtp(&sess, pkt, sizeof(pkt), 1), 0);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.drop_until_gop, 1u);

    /* A new IDR (socket drained) clears the flag and resumes normal delivery. */
    g_send_rc = (int)sizeof(pkt);
    NGX_RTC_TEST_ASSERT_I64_EQ(
            ngx_rtc_session_send_rtp(&sess, pkt, sizeof(pkt), 1), 1);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.drop_until_gop, 0u);
}

NGX_RTC_TEST(session_hard_send_error_arms_drop_until_gop_at_once)
{
    ngx_rtc_session_t sess;
    ngx_rtc_source_t  src;
    fake_conn_t       conn;
    uint8_t           pkt[CORE_PKT_LEN];

    core_make_video_sess(&sess, &src, &conn);
    make_rtp(pkt, 0u, 0u);

    /* NGX_ERROR is a dead socket, not a transient burst. */
    g_send_rc = -1; /* NGX_ERROR */
    (void)ngx_rtc_session_send_rtp(&sess, pkt, sizeof(pkt), 0);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.drop_until_gop, 1u);

    /* A short write (partial datagram) is equally unrecoverable. */
    sess.drop_until_gop = 0u;
    sess.send_eagain_streak = 0u;
    g_send_rc = 5; /* less than CORE_PKT_LEN */
    (void)ngx_rtc_session_send_rtp(&sess, pkt, sizeof(pkt), 0);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.drop_until_gop, 1u);
}

/* A drop we chose must be countable apart from a drop the path chose. A packet
 * refused by the pacer still carries the RTP sequence number the source gave
 * it, so the receiver scores it as loss; without a counter the only visible
 * symptom of a pacer collapse is a lower send rate, and the loss-based
 * controller has no way to tell its own drops from real congestion. */
NGX_RTC_TEST(session_counts_gop_and_pacer_drops)
{
    ngx_rtc_session_t sess;
    ngx_rtc_source_t  src;
    fake_conn_t       conn;
    uint8_t           pkt[CORE_PKT_LEN];

    core_make_video_sess(&sess, &src, &conn);
    make_rtp(pkt, 0u, 0u);
    ngx_current_msec = 5000u;

    /* Holding video back until the next IDR is the backpressure drop. */
    sess.drop_until_gop = 1u;
    (void)ngx_rtc_session_send_rtp(&sess, pkt, sizeof(pkt), 0);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.drop_gop, 1u);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.drop_pacer, 0u);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.send_failed, 0u);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.send_eagain, 0u);

    /* An empty token bucket is the pacer's drop. The clock is anchored to now
     * with no elapsed time, so no refill can rescue the packet. Mid-GOP video
     * is what the pacer may refuse; an IDR must never be (see
     * send_never_paces_out_a_gop_start). */
    sess.drop_until_gop = 0u;
    sess.pacer_target_bps = 8000000u;
    sess.pacer_started = 1u;
    sess.pacer_last_ms = ngx_current_msec;
    sess.pacer_tokens = 0u;
    (void)ngx_rtc_session_send_rtp(&sess, pkt, sizeof(pkt), 0);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.drop_pacer, 1u);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.drop_gop, 1u);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.send_failed, 0u);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.send_eagain, 0u);
}

/* The EAGAIN run is a measure of sustained backpressure, so it must expire
 * with time, not only on a successful video send. A still picture produces no
 * video packets at all: without expiry a run left at 2 by a burst before the
 * pause would make the first EAGAIN after the picture resumes drop a whole
 * GOP, i.e. freeze the stream on a hiccup the client could absorb. */
NGX_RTC_TEST(session_eagain_streak_expires_when_idle)
{
    ngx_rtc_session_t sess;
    ngx_rtc_source_t  src;
    fake_conn_t       conn;
    uint8_t           pkt[CORE_PKT_LEN];

    core_make_video_sess(&sess, &src, &conn);
    make_rtp(pkt, 0u, 0u);

    ngx_current_msec = 1000u;

    /* Two EAGAINs, then the picture goes still: no send at all for a while. */
    g_send_rc = -2; /* NGX_AGAIN */
    (void)ngx_rtc_session_send_rtp(&sess, pkt, sizeof(pkt), 1);
    (void)ngx_rtc_session_send_rtp(&sess, pkt, sizeof(pkt), 1);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.send_eagain_streak, 2u);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.drop_until_gop, 0u);

    /* Well past the idle threshold the stale run must not count. */
    ngx_current_msec += NGX_RTC_EAGAIN_STREAK_IDLE_MS + 1u;
    g_send_rc = -2;
    (void)ngx_rtc_session_send_rtp(&sess, pkt, sizeof(pkt), 1);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.send_eagain_streak, 1u);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.drop_until_gop, 0u);

    /* Inside the threshold the run keeps accumulating and still arms. */
    ngx_current_msec += 10u;
    (void)ngx_rtc_session_send_rtp(&sess, pkt, sizeof(pkt), 1);
    (void)ngx_rtc_session_send_rtp(&sess, pkt, sizeof(pkt), 1);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.drop_until_gop, 1u);

    ngx_current_msec = 0u;
}

NGX_RTC_TEST(nack_window_backoff_backs_off_and_resets)
{
    ngx_rtc_session_t sess;

    (void)memset(&sess, 0, sizeof(sess));

    /* First call lazy-inits the base window and rolls (start was 0). */
    NGX_RTC_TEST_ASSERT_I64_EQ(
            ngx_rtc_session_nack_window_step(&sess, 100u), 1);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.nack_window_ms, NGX_RTC_NACK_WINDOW_MS);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.nack_window_start, 100u);

    /* Before the window elapses: no roll, width unchanged. */
    NGX_RTC_TEST_ASSERT_I64_EQ(
            ngx_rtc_session_nack_window_step(&sess, 150u), 0);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.nack_window_ms, NGX_RTC_NACK_WINDOW_MS);

    /* A saturated window doubles the next width (100 -> 200). */
    sess.nack_retransmitted = NGX_RTC_NACK_BUDGET;
    NGX_RTC_TEST_ASSERT_I64_EQ(
            ngx_rtc_session_nack_window_step(&sess, 300u), 1);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.nack_window_ms, NGX_RTC_NACK_WINDOW_MS * 2u);

    /* It keeps doubling (200 -> 400) while every window saturates. */
    sess.nack_retransmitted = NGX_RTC_NACK_BUDGET;
    NGX_RTC_TEST_ASSERT_I64_EQ(
            ngx_rtc_session_nack_window_step(
                    &sess, 300u + NGX_RTC_NACK_WINDOW_MS * 2u), 1);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.nack_window_ms, NGX_RTC_NACK_WINDOW_MS * 4u);

    /* Doubling clamps at the ceiling (400 -> 800, not 1600). */
    sess.nack_retransmitted = NGX_RTC_NACK_BUDGET;
    NGX_RTC_TEST_ASSERT_I64_EQ(
            ngx_rtc_session_nack_window_step(
                    &sess, 300u + NGX_RTC_NACK_WINDOW_MS * 6u), 1);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.nack_window_ms, NGX_RTC_NACK_WINDOW_MAX_MS);

    /* A quiet window resets the width back to the base cadence. */
    sess.nack_retransmitted = 0;
    NGX_RTC_TEST_ASSERT_I64_EQ(
            ngx_rtc_session_nack_window_step(
                    &sess, 300u + NGX_RTC_NACK_WINDOW_MS * 6u
                           + NGX_RTC_NACK_WINDOW_MAX_MS), 1);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.nack_window_ms, NGX_RTC_NACK_WINDOW_MS);
}

NGX_RTC_TEST(session_send_inserts_twcc_extension_without_shift)
{
    ngx_rtc_session_t sess;
    ngx_rtc_source_t  src;
    fake_conn_t       conn;
    uint8_t           pkt[CORE_PKT_LEN];
    const uint32_t    ssrc = 0x11223344u;

    (void)memset(&sess, 0, sizeof(sess));
    (void)memset(&src, 0, sizeof(src));
    ngx_rtc_session_fsm_init(&sess.fsm, sess.fsm_path,
                             NGX_RTC_SESSION_FSM_MAX_DEPTH, &sess);
    (void)ngx_rtc_session_fsm_dispatch(&sess.fsm,
                                       NGX_RTC_SESSION_EVT_STUN_BINDING);
    (void)ngx_rtc_session_fsm_dispatch(&sess.fsm,
                                       NGX_RTC_SESSION_EVT_DTLS_PACKET);
    (void)ngx_rtc_session_fsm_dispatch(&sess.fsm,
                                       NGX_RTC_SESSION_EVT_DTLS_HANDSHAKE_DONE);

    conn.c.log = &core_test_log;
    conn.c.send = fake_conn_send_capture;
    sess.conn = (void *)&conn;
    sess.source = &src;
    src.video_ssrc = ssrc;
    sess.video_pt = 96u;
    sess.twcc_video_ext = 3u;
    g_cap_len = 0;

    (void)memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x80u;                     /* V=2, X=0 before the send */
    pkt[1] = 102u;                      /* source PT (rewritten to 96) */
    pkt[2] = 0x12u; pkt[3] = 0x34u;     /* seq */
    pkt[4] = 0xAAu; pkt[5] = 0xBBu; pkt[6] = 0xCCu; pkt[7] = 0xDDu; /* ts */
    pkt[8] = (uint8_t)(ssrc >> 24); pkt[9] = (uint8_t)(ssrc >> 16);
    pkt[10] = (uint8_t)(ssrc >> 8);  pkt[11] = (uint8_t)ssrc;
    pkt[12] = 0x01u; pkt[13] = 0x02u; pkt[14] = 0x03u; pkt[15] = 0x04u;

    NGX_RTC_TEST_ASSERT_I64_EQ(
            ngx_rtc_session_send_rtp(&sess, pkt, sizeof(pkt), 0), 1);

    /* 12 B header + 8 B TWCC extension + 4 B payload. */
    NGX_RTC_TEST_ASSERT_U64_EQ(g_cap_len, sizeof(pkt) + 8u);

    /* X bit set and PT rewritten to the session video_pt. */
    NGX_RTC_TEST_ASSERT_U64_EQ((g_cap_buf[0] & 0x10u), 0x10u);
    NGX_RTC_TEST_ASSERT_U64_EQ((g_cap_buf[1] & 0x7Fu), 96u);

    /* Sequence and timestamp are untouched. */
    NGX_RTC_TEST_ASSERT_U64_EQ(g_cap_buf[2], 0x12u);
    NGX_RTC_TEST_ASSERT_U64_EQ(g_cap_buf[3], 0x34u);
    NGX_RTC_TEST_ASSERT_U64_EQ(g_cap_buf[4], 0xAAu);

    /* One-byte extension: 0xBEDE, 1 word, id 3, transport seq 0. */
    NGX_RTC_TEST_ASSERT_U64_EQ(g_cap_buf[12], 0xBEu);
    NGX_RTC_TEST_ASSERT_U64_EQ(g_cap_buf[13], 0xDEu);
    NGX_RTC_TEST_ASSERT_U64_EQ(g_cap_buf[14], 0x00u);
    NGX_RTC_TEST_ASSERT_U64_EQ(g_cap_buf[15], 0x01u);
    NGX_RTC_TEST_ASSERT_U64_EQ(g_cap_buf[16], 0x31u);   /* (3 << 4) | 1 */
    NGX_RTC_TEST_ASSERT_U64_EQ(g_cap_buf[17], 0x00u);
    NGX_RTC_TEST_ASSERT_U64_EQ(g_cap_buf[18], 0x00u);
    NGX_RTC_TEST_ASSERT_U64_EQ(g_cap_buf[19], 0x00u);

    /* Payload landed at offset 20 with its bytes preserved. */
    NGX_RTC_TEST_ASSERT_U64_EQ(g_cap_buf[20], 0x01u);
    NGX_RTC_TEST_ASSERT_U64_EQ(g_cap_buf[21], 0x02u);
    NGX_RTC_TEST_ASSERT_U64_EQ(g_cap_buf[22], 0x03u);
    NGX_RTC_TEST_ASSERT_U64_EQ(g_cap_buf[23], 0x04u);

    NGX_RTC_TEST_ASSERT_U64_EQ(sess.twcc_seq, 1u);
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
    conn->c.log = &core_test_log;
    conn->c.send = fake_conn_send;
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

NGX_RTC_TEST(ring_reset_serves_only_new_generation)
{
    ngx_rtc_rtp_ring_t ring;
    const uint8_t *out = NULL;
    uint16_t out_len = 0;
    uint32_t i;

    /* The republish reset (bridge: gop.count/gop_start = 0 on a new AVC
     * sequence header; shm retransmit_reset mirrors it with head = 0) must
     * isolate generations: the retained window [head-count, head) only ever
     * covers post-reset packets, so an old-generation sequence can never be
     * served as a retransmit or replayed as fast-start. */
    (void)memset(&ring, 0, sizeof(ring));
    replay_session_make_ready();

    /* Old generation ends exactly at the 16-bit wrap point. */
    for (i = 65530u; i <= 65535u; i++)
    {
        push_seq(&ring, (uint16_t)i, (i == 65530u) ? 1u : 0u);
    }

    ring.count = 0;
    ring.gop_start = 0;

    /* New generation restarts at seq 0; its IDR access unit starts at seq 3. */
    for (i = 0u; i < 10u; i++)
    {
        push_seq(&ring, (uint16_t)i, (i == 3u) ? 1u : 0u);
    }

    /* New-generation lookups hit. */
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtp_ring_get(&ring, 0u, &out, &out_len),
                               NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_I64_EQ(out[3], 0u);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtp_ring_get(&ring, 9u, &out, &out_len),
                               NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_I64_EQ(out[3], 9u);

    /* Old-generation sequences (inside the wrap window) all miss. */
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtp_ring_get(&ring, 65530u, &out, &out_len),
                               NGX_RTC_ERR_PARSE);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtp_ring_get(&ring, 65535u, &out, &out_len),
                               NGX_RTC_ERR_PARSE);

    /* Replay serves the new GOP only (seq 3..9), not the old tail. */
    g_replay_count = 0;
    ngx_rtc_rtp_ring_replay(&ring, &g_replay_sess);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_replay_count, 7);
    for (i = 0u; i < 7u; i++)
    {
        NGX_RTC_TEST_ASSERT_I64_EQ(g_replay_seq[i], (uint16_t)(3u + i));
    }

    free(ring.slots);
}

/* ------------------------------------------------------------------ */
/* Pacing token bucket + TWCC AIMD bitrate adaptation.                 */
/* ------------------------------------------------------------------ */

NGX_RTC_TEST(pacer_init_clamps_rate_and_fills_bucket)
{
    ngx_rtc_session_t sess;

    (void)memset(&sess, 0, sizeof(sess));

    /* Below the floor: clamped to 64 kbps. 100 ms of that rate is 800 B, but
     * the bucket may never be smaller than one maximum-size RTP packet -- see
     * pacer_bucket_holds_one_max_packet_at_the_floor. */
    ngx_rtc_session_pacer_init(&sess, 1000u);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.pacer_target_bps, NGX_RTC_PACER_MIN_BPS);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.pacer_tokens, (uint64_t) NGX_RTC_MAX_RTP_PKT);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.pacer_started, 0u);

    /* Above the ceiling: clamped to 8 Mbps, bucket = 8e6*100/8000 = 100000 B. */
    ngx_rtc_session_pacer_init(&sess, 99999999u);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.pacer_target_bps, NGX_RTC_PACER_MAX_BPS);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.pacer_tokens, 100000u);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.pacer_started, 0u);
}

/* The bucket must hold one maximum-size RTP packet at every target, including
 * the floor. 100 ms of the 64 kbps floor is 800 B, less than the 1214 B an
 * H264 RTP packet can reach, and the refill is capped at the bucket size -- so
 * with a short bucket no video packet is ever admitted again. A session the
 * controller has driven to the floor then stays mute for video forever, even
 * once the path clears, because every later admit is refused for the same
 * reason. */
NGX_RTC_TEST(pacer_bucket_holds_one_max_packet_at_the_floor)
{
    ngx_rtc_session_t sess;

    (void)memset(&sess, 0, sizeof(sess));
    ngx_current_msec = 1000u;

    ngx_rtc_session_pacer_init(&sess, NGX_RTC_PACER_MIN_BPS);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.pacer_tokens, (uint64_t) NGX_RTC_MAX_RTP_PKT);

    /* A full bucket at the floor must admit the largest packet the source can
     * produce; anything smaller means video is permanently refused. */
    NGX_RTC_TEST_ASSERT_I64_EQ(
        ngx_rtc_session_pacer_admit(&sess, (uint32_t) NGX_RTC_MAX_RTP_PKT,
                                    ngx_current_msec), NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.pacer_tokens, 0u);
}

/* REMB is a cap, not a target. pacer_target_bps is the loss controller's only
 * state, so applying every REMB report as an absolute target wipes the
 * controller on each one -- and the viewer's estimate is derived from what it
 * received, so the pacer's own drops push it down. The two feed each other
 * until the session sits on the floor with no way back up. */
NGX_RTC_TEST(pacer_remb_caps_but_never_raises)
{
    ngx_rtc_session_t sess;

    (void)memset(&sess, 0, sizeof(sess));

    ngx_rtc_session_pacer_init(&sess, 2000000u);

    /* A lower estimate caps the current target. */
    ngx_rtc_session_pacer_cap(&sess, 500000u);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.pacer_target_bps, 500000u);

    /* A higher one must not drag the controller back up. */
    ngx_rtc_session_pacer_cap(&sess, 4000000u);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.pacer_target_bps, 500000u);

    /* An estimate below the floor still clamps to the floor, and the bucket
     * shrinks with the target so a stale surplus cannot burst through it. */
    ngx_rtc_session_pacer_cap(&sess, 1000u);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.pacer_target_bps, NGX_RTC_PACER_MIN_BPS);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.pacer_tokens, (uint64_t) NGX_RTC_MAX_RTP_PKT);
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

/* The source is an external, non-adaptive RTMP encoder: the pacer cannot make
 * it offer less, so refusing a packet never relieves congestion -- it only
 * decides which packets the viewer loses. A keyframe is the one packet that
 * can restore a picture, so pacing it out is unrecoverable: the viewer decodes
 * nothing until a later IDR, asks for one with a PLI, and that reply is paced
 * out too. The controller can sit on the floor for as long as its feedback
 * says so, which is exactly when this matters. An IDR must always leave. */
NGX_RTC_TEST(send_never_paces_out_a_gop_start)
{
    ngx_rtc_session_t sess;
    ngx_rtc_source_t  src;
    fake_conn_t       conn;
    uint8_t           pkt[CORE_PKT_LEN];

    core_make_video_sess(&sess, &src, &conn);
    make_rtp(pkt, 0u, 0u);

    ngx_rtc_session_pacer_init(&sess, NGX_RTC_PACER_MIN_BPS);
    sess.pacer_tokens = 0u;          /* drained: the controller is at the floor */
    ngx_current_msec = 1000u;
    g_send_rc = (int)sizeof(pkt);

    /* Mid-GOP video is correctly refused while the budget is spent. */
    NGX_RTC_TEST_ASSERT_I64_EQ(
            ngx_rtc_session_send_rtp(&sess, pkt, sizeof(pkt), 0), 0);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.drop_pacer, 1u);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.pacer_tokens, 0u);

    /* A GOP start must still leave, and must not be charged as a drop. */
    NGX_RTC_TEST_ASSERT_I64_EQ(
            ngx_rtc_session_send_rtp(&sess, pkt, sizeof(pkt), 1), 1);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.drop_pacer, 1u);
}

/* Exempting only the packet marked is_gop_start exempts only the SPS/PPS
 * preamble: that flag is set on the STAP-A that opens the access unit and
 * cleared before the IDR itself is packetized. The pacer would then refuse the
 * fragments that actually carry the picture, leaving the viewer with a
 * parameter set and no image -- the one state it cannot recover from without
 * asking again. The exemption therefore covers the access unit, opened by the
 * GOP start and closed by the RTP marker bit. */
NGX_RTC_TEST(send_exempts_the_whole_keyframe_access_unit)
{
    ngx_rtc_session_t sess;
    ngx_rtc_source_t  src;
    fake_conn_t       conn;
    uint8_t           pkt[CORE_PKT_LEN];

    core_make_video_sess(&sess, &src, &conn);
    make_rtp(pkt, 0u, 0u);

    ngx_rtc_session_pacer_init(&sess, NGX_RTC_PACER_MIN_BPS);
    sess.pacer_tokens = 0u;          /* drained: the controller is at the floor */
    ngx_current_msec = 1000u;
    g_send_rc = (int)sizeof(pkt);

    /* The GOP start opens the access unit. */
    NGX_RTC_TEST_ASSERT_I64_EQ(
            ngx_rtc_session_send_rtp(&sess, pkt, sizeof(pkt), 1), 1);

    /* The IDR fragments after it are the same keyframe and must also leave. */
    NGX_RTC_TEST_ASSERT_I64_EQ(
            ngx_rtc_session_send_rtp(&sess, pkt, sizeof(pkt), 0), 1);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.drop_pacer, 0u);

    /* The marker closes the unit; that packet is still part of it. */
    pkt[1] = (uint8_t)(102u | NGX_RTC_RTP_MARKER);
    NGX_RTC_TEST_ASSERT_I64_EQ(
            ngx_rtc_session_send_rtp(&sess, pkt, sizeof(pkt), 0), 1);

    /* Mid-GOP video is subject to the budget again. */
    pkt[1] = 102u;
    NGX_RTC_TEST_ASSERT_I64_EQ(
            ngx_rtc_session_send_rtp(&sess, pkt, sizeof(pkt), 0), 0);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.drop_pacer, 1u);
}

/* The keyframe exemption is bounded. `keyframe_open` is cleared by the RTP
 * marker bit, and a source that never sets one -- or an access unit large
 * enough that the marker is far away -- would otherwise hold the exemption
 * forever, letting one stream bypass the pacer without limit. The cap is the
 * only thing standing between "a keyframe always leaves" and "nothing is ever
 * paced", so the packet just past it must go back under the budget. */
NGX_RTC_TEST(send_stops_exempting_the_keyframe_at_the_packet_cap)
{
    ngx_rtc_session_t sess;
    ngx_rtc_source_t  src;
    fake_conn_t       conn;
    uint8_t           pkt[CORE_PKT_LEN];
    ngx_uint_t        i;

    core_make_video_sess(&sess, &src, &conn);
    make_rtp(pkt, 0u, 0u);           /* no marker: the unit stays open */

    ngx_rtc_session_pacer_init(&sess, NGX_RTC_PACER_MIN_BPS);
    sess.pacer_tokens = 0u;          /* drained: the controller is at the floor */
    ngx_current_msec = 1000u;
    g_send_rc = (int)sizeof(pkt);

    /* The GOP start opens the unit and is exempt. */
    NGX_RTC_TEST_ASSERT_I64_EQ(
            ngx_rtc_session_send_rtp(&sess, pkt, sizeof(pkt), 1), 1);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.keyframe_open, 1u);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.keyframe_pkts, 1u);

    /* The rest of the unit is exempt up to the cap. The cap counts the GOP
     * start too, so this loop runs one short of it. */
    for (i = 1u; i < NGX_RTC_KEYFRAME_MAX_PKTS; i++) {
        NGX_RTC_TEST_ASSERT_I64_EQ(
                ngx_rtc_session_send_rtp(&sess, pkt, sizeof(pkt), 0), 1);
    }
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.keyframe_pkts, NGX_RTC_KEYFRAME_MAX_PKTS);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.drop_pacer, 0u);

    /* One past the cap the exemption lapses and the depleted budget refuses
     * the packet like any other mid-GOP video. */
    NGX_RTC_TEST_ASSERT_I64_EQ(
            ngx_rtc_session_send_rtp(&sess, pkt, sizeof(pkt), 0), 0);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.drop_pacer, 1u);
}

/* A packet the socket never accepted must not burn a transport-wide sequence
 * number. The receiver would truthfully report the resulting gap as loss, the
 * loss controller would cut the rate, and the cut would make the next send
 * fail the same way -- the sender would be reporting its own failures as
 * network congestion. */
NGX_RTC_TEST(send_does_not_spend_a_transport_seq_on_a_packet_that_never_left)
{
    ngx_rtc_session_t sess;
    ngx_rtc_source_t  src;
    fake_conn_t       conn;
    uint8_t           pkt[CORE_PKT_LEN];

    core_make_video_sess(&sess, &src, &conn);
    make_rtp(pkt, 0u, 0u);
    sess.twcc_video_ext = 3u;        /* transport-cc negotiated */

    /* Hard socket error. */
    g_send_rc = -1; /* NGX_ERROR */
    NGX_RTC_TEST_ASSERT_I64_EQ(
            ngx_rtc_session_send_rtp(&sess, pkt, sizeof(pkt), 0), 0);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.twcc_seq, 0u);

    /* UDP send buffer full. The hard error above armed the video backpressure
     * gate, which refuses mid-GOP packets before they reach the stamp, so clear
     * it or this case would never exercise the rollback it is meant to. */
    sess.drop_until_gop = 0u;
    g_send_rc = -2; /* NGX_AGAIN */
    NGX_RTC_TEST_ASSERT_I64_EQ(
            ngx_rtc_session_send_rtp(&sess, pkt, sizeof(pkt), 0), 0);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.twcc_seq, 0u);

    /* Short write: only a fragment reached the wire. */
    sess.drop_until_gop = 0u;
    g_send_rc = 5;
    NGX_RTC_TEST_ASSERT_I64_EQ(
            ngx_rtc_session_send_rtp(&sess, pkt, sizeof(pkt), 0), 0);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.twcc_seq, 0u);

    /* Only a datagram that actually left spends a sequence. The short write
     * above armed the video backpressure gate, which refuses mid-GOP packets
     * before they ever reach the pacer, so clear it first. */
    sess.drop_until_gop = 0u;
    g_send_rc = (int)sizeof(pkt) + 8;   /* + the 8-byte TWCC extension */
    NGX_RTC_TEST_ASSERT_I64_EQ(
            ngx_rtc_session_send_rtp(&sess, pkt, sizeof(pkt), 0), 1);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.twcc_seq, 1u);
}

/* The one abandon point in the send path that is not a socket error: SRTP
 * refused the packet, so nothing reached the wire and the transport-wide
 * sequence stamped a few lines earlier has to go back. Left in place, the
 * receiver reports the gap as loss and the loss-driven rate controller cuts the
 * send rate over a packet this process never sent -- the sender reporting its
 * own failures as congestion, the same mistake the short-write case above fixes
 * for the socket path.
 *
 * The sequence is driven 0 -> 1 by a send that really leaves first, so the drop
 * back to 0 can only come from the rollback itself. Asserting 0 on its own
 * would pass unchanged if the packet had never been stamped at all.
 *
 * Compiled out of the sanitizer build, which links the real libsrtp2 and so
 * compiles the injectable stub out: there the injection is accepted but ignored
 * and this case would fail for a reason that has nothing to do with the
 * rollback. */
#ifndef NGX_RTC_TEST_REAL_SRTP
NGX_RTC_TEST(send_gives_the_transport_seq_back_when_srtp_fails)
{
    ngx_rtc_session_t sess;
    ngx_rtc_source_t  src;
    fake_conn_t       conn;
    uint8_t           pkt[CORE_PKT_LEN];

    core_make_video_sess(&sess, &src, &conn);
    make_rtp(pkt, 0u, 0u);
    sess.twcc_video_ext = 3u;        /* transport-cc negotiated */

    /* twcc_seq is the NEXT sequence to hand out, not a count of packets sent,
     * so a rolled-back failure leaves it at the value it held before that
     * packet was stamped -- not at zero. Two datagrams that really leave take it
     * to 2. */
    ngx_rtc_test_srtp_protect_rc = 0;
    g_send_rc = (int)sizeof(pkt) + 8;   /* + the 8-byte TWCC extension */
    NGX_RTC_TEST_ASSERT_I64_EQ(
            ngx_rtc_session_send_rtp(&sess, pkt, sizeof(pkt), 0), 1);
    NGX_RTC_TEST_ASSERT_I64_EQ(
            ngx_rtc_session_send_rtp(&sess, pkt, sizeof(pkt), 0), 1);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.twcc_seq, 2u);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.srtp_failed, 0u);

    /* SRTP refuses the third one. Every stamping input is unchanged from the two
     * calls above -- same session, same packet, same length, same extension id,
     * and the pacer is never armed here (pacer_target_bps == 0 passes through
     * unpaced) -- so this call reaches the rollback with twcc_stamped set. Drop
     * the rollback and the counter lands on 3 instead of 2. */
    ngx_rtc_test_srtp_protect_rc = -1;
    NGX_RTC_TEST_ASSERT_I64_EQ(
            ngx_rtc_session_send_rtp(&sess, pkt, sizeof(pkt), 0), 0);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.twcc_seq, 2u);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.srtp_failed, 1u);

    ngx_rtc_test_srtp_protect_rc = 0;
}
#endif

/* The NACK budget must bound attempts, not deliveries. When the pacer refuses
 * every retransmit the success-counted budget never advances, so the retransmit
 * loop never breaks: one NACK feedback packet expands up to 512 lost sequences
 * and offers each of them to the pacer, every refusal adding another
 * drop_pacer. The same counter drives the window backoff, so counting only
 * successes silently disables that too -- the window stays at the base 100 ms
 * and the storm can repeat four times a second. */
NGX_RTC_TEST(nack_budget_charges_every_attempt)
{
    ngx_rtc_session_t sess;
    ngx_uint_t        i;

    (void)memset(&sess, 0, sizeof(sess));

    /* The budget admits exactly NGX_RTC_NACK_BUDGET attempts. */
    for (i = 0; i < NGX_RTC_NACK_BUDGET; i++) {
        NGX_RTC_TEST_ASSERT_I64_EQ(
                ngx_rtc_session_nack_budget_take(&sess), 1);
    }
    NGX_RTC_TEST_ASSERT_I64_EQ(
            ngx_rtc_session_nack_budget_take(&sess), 0);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.nack_retransmitted,
                               (uint64_t) NGX_RTC_NACK_BUDGET);

    /* A window whose attempts were all spent backs off even though not one of
     * them was delivered. */
    ngx_current_msec = 1000u;
    sess.nack_window_start = 0u;
    NGX_RTC_TEST_ASSERT_I64_EQ(
            ngx_rtc_session_nack_window_step(&sess, ngx_current_msec), 1);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.nack_window_ms,
                               NGX_RTC_NACK_WINDOW_MS * 2u);
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

/* ------------------------------------------------------------------ */
/* Subscriber list integrity (source teardown must not strand peers).  */
/* ------------------------------------------------------------------ */

/* A session that was never subscribed (e.g. a WHIP publisher, which sets
 * sess->source without ngx_rtc_source_subscribe) must not unlink itself from
 * the subscriber queue, and removing one subscriber must leave the others
 * linked. */
NGX_RTC_TEST(unsubscribe_leaves_peer_subscribers_linked)
{
    ngx_rtc_source_t  src;
    ngx_rtc_session_t a;
    ngx_rtc_session_t b;
    ngx_rtc_session_t c;
    ngx_rtc_session_t *it;

    (void)memset(&src, 0, sizeof(src));
    ngx_queue_init(&src.subscribers);

    (void)memset(&a, 0, sizeof(a));
    (void)memset(&b, 0, sizeof(b));
    (void)memset(&c, 0, sizeof(c));
    ngx_queue_init(&a.sub_queue);
    ngx_queue_init(&b.sub_queue);
    ngx_queue_init(&c.sub_queue);

    ngx_rtc_source_subscribe(&src, &a);
    ngx_rtc_source_subscribe(&src, &b);
    ngx_rtc_source_subscribe(&src, &c);

    /* Unsubscribe the middle entry: the remaining two stay reachable and no
     * neighbour pointer dangles. */
    ngx_rtc_source_unsubscribe(&src, &b);
    NGX_RTC_TEST_ASSERT(NULL == b.source);
    NGX_RTC_TEST_ASSERT(&src == a.source);
    NGX_RTC_TEST_ASSERT(&src == c.source);

    it = ngx_rtc_source_first_subscriber(&src);
    NGX_RTC_TEST_ASSERT(NULL != it);
    it = ngx_rtc_source_next_subscriber(&src, it);
    NGX_RTC_TEST_ASSERT(NULL != it);
    NGX_RTC_TEST_ASSERT(NULL == ngx_rtc_source_next_subscriber(&src, it));

    ngx_rtc_source_unsubscribe(&src, &a);
    ngx_rtc_source_unsubscribe(&src, &c);
    NGX_RTC_TEST_ASSERT(ngx_queue_empty(&src.subscribers));

    /* Both entries must be self-linked again, so a later removal is a no-op
     * rather than a dereference of a dangling neighbour. */
    NGX_RTC_TEST_ASSERT(&a.sub_queue == a.sub_queue.next);
    NGX_RTC_TEST_ASSERT(&c.sub_queue == c.sub_queue.next);
}

/* A WHIP publisher sets sess->source directly and never calls
 * ngx_rtc_source_subscribe, so its link is self-linked and it is on no list.
 * Unsubscribing it must leave the source's subscriber queue alone: removing a
 * node that was never inserted rewires the queue head, which here would drop
 * the one real viewer. */
NGX_RTC_TEST(unsubscribe_of_a_publisher_that_never_subscribed)
{
    ngx_rtc_source_t  src;
    ngx_rtc_session_t viewer;
    ngx_rtc_session_t publisher;

    (void)memset(&src, 0, sizeof(src));
    (void)memset(&viewer, 0, sizeof(viewer));
    (void)memset(&publisher, 0, sizeof(publisher));
    ngx_queue_init(&src.subscribers);
    ngx_queue_init(&viewer.sub_queue);
    ngx_queue_init(&publisher.sub_queue);   /* as ngx_rtc_shm_session_init does */

    ngx_rtc_source_subscribe(&src, &viewer);
    publisher.source = &src;                /* set directly, never enqueued */

    ngx_rtc_source_unsubscribe(&src, &publisher);

    NGX_RTC_TEST_ASSERT(NULL == publisher.source);
    NGX_RTC_TEST_ASSERT(&publisher.sub_queue == publisher.sub_queue.next);
    NGX_RTC_TEST_ASSERT(&publisher.sub_queue == publisher.sub_queue.prev);

    /* The viewer is untouched and still the only subscriber. */
    NGX_RTC_TEST_ASSERT(&src == viewer.source);
    NGX_RTC_TEST_ASSERT(&viewer.sub_queue == src.subscribers.next);
    NGX_RTC_TEST_ASSERT(&viewer == ngx_rtc_source_first_subscriber(&src));
}

/* The same call on a session whose link was never initialised at all -- a
 * zeroed struct, which is what any allocation path that skips ngx_queue_init
 * leaves behind. "Not on the list" is expressed as self-linked, so a NULL next
 * is neither self-linked nor on the source's queue, and the guard cannot tell
 * it apart from an enqueued node by that test alone: it has to reject NULL on
 * its own or ngx_queue_remove dereferences it. */
NGX_RTC_TEST(unsubscribe_of_a_session_with_an_uninitialised_link)
{
    ngx_rtc_source_t  src;
    ngx_rtc_session_t sess;

    (void)memset(&src, 0, sizeof(src));
    (void)memset(&sess, 0, sizeof(sess));
    ngx_queue_init(&src.subscribers);

    sess.source = &src;                     /* sub_queue stays zeroed */

    ngx_rtc_source_unsubscribe(&src, &sess);

    NGX_RTC_TEST_ASSERT(NULL == sess.source);
}

/*
 * The source teardown paths (reaper close / RTCP timer) can reach unsubscribe
 * twice for one session: the reaper removes the sub_queue link and the RTCP
 * timer's iteration holds a stale pointer. In a release build ngx_queue_remove
 * only rewires the neighbours and leaves the removed node's own next/prev
 * dangling, so a second remove dereferences freed or stale memory and faults
 * at ngx_queue_remove's first statement. Unsubscribe must therefore be
 * idempotent: a node that is not currently linked is self-linked.
 */
NGX_RTC_TEST(unsubscribe_is_idempotent)
{
    ngx_rtc_source_t  src;
    ngx_rtc_session_t a;

    (void)memset(&src, 0, sizeof(src));
    (void)memset(&a, 0, sizeof(a));
    ngx_queue_init(&src.subscribers);
    ngx_queue_init(&a.sub_queue);

    ngx_rtc_source_subscribe(&src, &a);
    ngx_rtc_source_unsubscribe(&src, &a);
    NGX_RTC_TEST_ASSERT(&a.sub_queue == a.sub_queue.next);
    NGX_RTC_TEST_ASSERT(&a.sub_queue == a.sub_queue.prev);

    /* A stale source pointer (source re-created under the same name, or a
     * second close path) must not re-remove an already unlinked node. */
    a.source = &src;
    ngx_rtc_source_unsubscribe(&src, &a);

    NGX_RTC_TEST_ASSERT(NULL == a.source);
    NGX_RTC_TEST_ASSERT(&a.sub_queue == a.sub_queue.next);
    NGX_RTC_TEST_ASSERT(ngx_queue_empty(&src.subscribers));
}

/* Without a configuration snapshot the pure C units must see exactly the
 * historical compile-time values: that is what keeps the host tests - and any
 * deployment that sets none of the rtc_* tunables - behaviourally unchanged. */
NGX_RTC_TEST(tunables_default_when_unset)
{
    const ngx_rtc_tunables_t *t;

    ngx_rtc_core_set_tunables(NULL);

    t = ngx_rtc_core_tunables();
    NGX_RTC_TEST_ASSERT(NULL != t);
    NGX_RTC_TEST_ASSERT_U64_EQ(t->jitter_timeout_ms, NGX_RTC_JITTER_TIMEOUT_MS);
    NGX_RTC_TEST_ASSERT_U64_EQ(t->nack_window_ms, NGX_RTC_NACK_WINDOW_MS);
    NGX_RTC_TEST_ASSERT_U64_EQ(t->nack_window_max_ms,
                               NGX_RTC_NACK_WINDOW_MAX_MS);
    NGX_RTC_TEST_ASSERT_U64_EQ(t->eagain_streak_max, NGX_RTC_EAGAIN_STREAK_MAX);
    NGX_RTC_TEST_ASSERT_U64_EQ(t->gop_ring_slots, NGX_RTC_GOP_RING_CAP);
}

/* An installed snapshot must actually reach the two consumers that live in the
 * pure C core: the NACK window backoff and the lazy GOP ring sizing. */
NGX_RTC_TEST(tunables_override_reaches_consumers)
{
    ngx_rtc_tunables_t  custom;
    ngx_rtc_session_t   sess;
    ngx_rtc_rtp_ring_t  ring;
    uint32_t            i;

    (void)memset(&custom, 0, sizeof(custom));
    custom.jitter_timeout_ms = 120u;
    custom.nack_window_ms = 250u;
    custom.nack_window_max_ms = 1000u;
    custom.eagain_streak_max = 7u;
    custom.gop_ring_slots = 64u;
    ngx_rtc_core_set_tunables(&custom);

    /* First NACK lazy-inits the window from the snapshot, not the macro. */
    (void)memset(&sess, 0, sizeof(sess));
    NGX_RTC_TEST_ASSERT_I64_EQ(
            ngx_rtc_session_nack_window_step(&sess, 1000u), 1);
    NGX_RTC_TEST_ASSERT_U64_EQ(sess.nack_window_ms, 250u);
    NGX_RTC_TEST_ASSERT_I64_EQ(
            ngx_rtc_session_nack_window_step(&sess, 1100u), 0);
    NGX_RTC_TEST_ASSERT_I64_EQ(
            ngx_rtc_session_nack_window_step(&sess, 1250u), 1);

    /* The GOP ring sizes itself from the snapshot on its first push. */
    (void)memset(&ring, 0, sizeof(ring));
    for (i = 1u; i <= 200u; i++) {
        push_seq(&ring, (uint16_t)i, 0);
    }
    NGX_RTC_TEST_ASSERT_U64_EQ(ring.capacity, 64u);
    NGX_RTC_TEST_ASSERT_U64_EQ(ring.count, 64u);

    free(ring.slots);

    /* Restore, so a later test never inherits the override. */
    ngx_rtc_core_set_tunables(NULL);
}

/* ------------------------------------------------------------------ */
/* Source lifetime: an unseen holder must block the free.              */
/* ------------------------------------------------------------------ */

/*
 * A publisher session (WHIP) sets sess->source without subscribing, so it is
 * absent from src->subscribers. If a subscribed peer unsubscribes first, the
 * queue looks empty and the old code freed the source -- leaving the publisher
 * session's ->source dangling for its own close to dereference (SIGSEGV in
 * ngx_rtc_stream_session_close, reached from the reaper timer). has_holder is
 * what the free gate consults.
 */
NGX_RTC_TEST(source_has_holder_sees_publisher_session)
{
    ngx_rtc_source_t  src;
    ngx_rtc_session_t viewer;
    ngx_rtc_session_t publisher;

    (void)memset(&src, 0, sizeof(src));
    ngx_queue_init(&src.subscribers);

    (void)memset(&viewer, 0, sizeof(viewer));
    (void)memset(&publisher, 0, sizeof(publisher));
    ngx_queue_init(&viewer.sub_queue);
    ngx_queue_init(&publisher.sub_queue);

    (void)memcpy(viewer.ice_ufrag, "viewerufrag", sizeof("viewerufrag"));
    (void)memcpy(publisher.ice_ufrag, "pubufrag", sizeof("pubufrag"));
    ngx_rtc_session_add(&viewer);
    ngx_rtc_session_add(&publisher);

    ngx_rtc_source_subscribe(&src, &viewer);

    /* The publisher holds the source but never joins the subscriber queue. */
    publisher.source = &src;

    NGX_RTC_TEST_ASSERT_U64_EQ(ngx_rtc_source_has_holder(&src), 1u);

    /* The viewer leaves: the queue is empty, yet the publisher still holds it. */
    ngx_rtc_source_unsubscribe(&src, &viewer);
    NGX_RTC_TEST_ASSERT(ngx_queue_empty(&src.subscribers));
    NGX_RTC_TEST_ASSERT_U64_EQ(ngx_rtc_source_has_holder(&src), 1u);

    /* Last holder gone: nothing keeps the source alive. */
    publisher.source = NULL;
    NGX_RTC_TEST_ASSERT_U64_EQ(ngx_rtc_source_has_holder(&src), 0u);

    ngx_rtc_session_remove(&viewer);
    ngx_rtc_session_remove(&publisher);
}
