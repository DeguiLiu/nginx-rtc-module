/*
 * test_avsync.c - Sender-Report (SR) anchor plumbing for audio/video sync.
 *
 * SRS reference scenarios (KernelRTCTest):
 *   SyncTimestampBySenderReportNormal      - 2 SRs establish the NTP<->RTP anchor,
 *                                            then every RTP timestamp maps to a
 *                                            wall clock through linear scaling.
 *   SyncTimestampBySenderReportOutOfOrder  - a late SR1 must not move the anchor
 *                                            established by the newer SR2.
 *   SyncTimestampBySenderReportConsecutive - periodic SRs refresh the anchor.
 *   SyncTimestampBySenderReportDuplicated  - repeated SRs with the same anchor
 *                                            must stay idempotent.
 *
 * The SR anchor mapping lives in ngx_rtc_avsync (ngx_rtc_avsync_on_sr +
 * ngx_rtc_avsync_map_rtp_to_wall): two SRs calibrate the NTP<->RTP slope and
 * every RTP timestamp then maps to a wall clock through linear scaling. The
 * four scenarios above are exercised directly against that algorithm below,
 * together with a 32-bit rtp_ts wrap case. The two remaining tests pin the
 * codec layer the sync code builds on: the SR anchor survives the
 * encode/decode round trip (including a 32-bit rtp_ts wrap) and the parser
 * stays stateless under out-of-order / duplicated SRs.
 */

#include "ngx_rtc_test.h"
#include "ngx_rtc_rtcp.h"
#include "ngx_rtc_avsync.h"

#define AVSYNC_BUF 256u
#define AVSYNC_BASE_US  1700000000000000ULL
#define AVSYNC_STEP_US  40000ULL /* 40 ms */
#define AVSYNC_STEP_RTP 3600u    /* 90000 Hz * 40 ms */
#define AVSYNC_NTP_TOL  5000ULL  /* ~1.16 us; absorbs integer slope rounding */

static int32_t avsync_encode_anchor(uint64_t ntp, uint32_t rtp_ts,
                                    uint8_t *buf, uint32_t *len)
{
    ngx_rtc_rtcp_sr_t sr;

    (void)memset(&sr, 0, sizeof(sr));
    sr.ssrc = 200u;
    sr.ntp = ntp;
    sr.rtp_ts = rtp_ts;
    sr.rb = NULL;

    return ngx_rtc_rtcp_encode_sr(&sr, buf, AVSYNC_BUF, len);
}

NGX_RTC_TEST(sr_anchor_survives_roundtrip_and_rtp_ts_wrap)
{
    uint64_t ntp[3];
    uint32_t rtp_ts[3];
    uint8_t  buf[AVSYNC_BUF];
    uint32_t len = 0;
    uint32_t consumed = 0;
    ngx_rtc_rtcp_pkt_t pkt;
    uint32_t i;

    /* Three anchors 40ms apart (90000Hz x 0.04s = 3600 ticks), the second and
     * third straddling the 32-bit RTP timestamp wrap. */
    ntp[0] = ngx_rtc_rtcp_ntp_from_unix_us(1700000000000000ULL);
    rtp_ts[0] = 0xFFFFFF00u;
    ntp[1] = ngx_rtc_rtcp_ntp_from_unix_us(1700000000000000ULL + 40000ULL);
    rtp_ts[1] = 0x00000000u;   /* 0xFFFFFF00 + 3600 wrapped mod 2^32 */
    ntp[2] = ngx_rtc_rtcp_ntp_from_unix_us(1700000000000000ULL + 80000ULL);
    rtp_ts[2] = 3600u;

    for (i = 0u; i < 3u; i++)
    {
        NGX_RTC_TEST_ASSERT_I64_EQ(avsync_encode_anchor(ntp[i], rtp_ts[i],
                                                        buf, &len),
                                   NGX_RTC_OK);
        NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtcp_parse(buf, len, &pkt, &consumed),
                                   NGX_RTC_OK);
        NGX_RTC_TEST_ASSERT_I64_EQ(pkt.type, NGX_RTC_RTCP_SR);
        NGX_RTC_TEST_ASSERT_U64_EQ(pkt.ssrc, 200u);
        NGX_RTC_TEST_ASSERT_U64_EQ(pkt.ntp, ntp[i]);
        NGX_RTC_TEST_ASSERT_U64_EQ(pkt.rtp_ts, rtp_ts[i]);
    }
}

NGX_RTC_TEST(sr_duplicate_and_out_of_order_decode_is_stateless)
{
    uint8_t  buf_a[AVSYNC_BUF];
    uint8_t  buf_b[AVSYNC_BUF];
    uint32_t len_a = 0;
    uint32_t len_b = 0;
    uint32_t consumed = 0;
    ngx_rtc_rtcp_pkt_t pkt;
    const uint64_t ntp_a = 0x0102030405060708ULL;
    const uint64_t ntp_b = 0x1112131415161718ULL;

    NGX_RTC_TEST_ASSERT_I64_EQ(avsync_encode_anchor(ntp_a, 1000u,
                                                    buf_a, &len_a),
                               NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_I64_EQ(avsync_encode_anchor(ntp_b, 4600u,
                                                    buf_b, &len_b),
                               NGX_RTC_OK);

    /* Deliver in order B, A, then duplicate B: each parse returns its own
     * anchor; the codec carries no cross-packet state. */
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtcp_parse(buf_b, len_b, &pkt, &consumed),
                               NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_U64_EQ(pkt.ntp, ntp_b);
    NGX_RTC_TEST_ASSERT_U64_EQ(pkt.rtp_ts, 4600u);

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtcp_parse(buf_a, len_a, &pkt, &consumed),
                               NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_U64_EQ(pkt.ntp, ntp_a);
    NGX_RTC_TEST_ASSERT_U64_EQ(pkt.rtp_ts, 1000u);

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtcp_parse(buf_b, len_b, &pkt, &consumed),
                               NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_U64_EQ(pkt.ntp, ntp_b);
    NGX_RTC_TEST_ASSERT_U64_EQ(pkt.rtp_ts, 4600u);
}

static uint64_t
avsync_abs_diff(uint64_t a, uint64_t b)
{
    return (a > b) ? (a - b) : (b - a);
}

NGX_RTC_TEST(sync_timestamp_by_sender_report_normal)
{
    ngx_rtc_avsync_t sync;
    uint64_t ntp;
    uint64_t expected;
    uint32_t rtp_ts;
    uint32_t i;

    ngx_rtc_avsync_init(&sync);

    /* No anchor yet: mapping must be refused. */
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_avsync_map_rtp_to_wall(&sync, 10000u, &ntp),
                               NGX_RTC_AGAIN);

    /* The first SR only anchors; it cannot map until a second SR calibrates. */
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_avsync_on_sr(
                                   &sync, ngx_rtc_rtcp_ntp_from_unix_us(AVSYNC_BASE_US),
                                   10000u),
                               NGX_RTC_AGAIN);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_avsync_map_rtp_to_wall(&sync, 10000u, &ntp),
                               NGX_RTC_AGAIN);

    /* Second SR 40 ms later establishes the NTP<->RTP line. */
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_avsync_on_sr(
                                   &sync,
                                   ngx_rtc_rtcp_ntp_from_unix_us(AVSYNC_BASE_US + AVSYNC_STEP_US),
                                   10000u + AVSYNC_STEP_RTP),
                               NGX_RTC_OK);

    rtp_ts = 10000u + AVSYNC_STEP_RTP;
    for (i = 0u; i <= 1000u; i++)
    {
        rtp_ts += AVSYNC_STEP_RTP;
        expected = ngx_rtc_rtcp_ntp_from_unix_us(
            AVSYNC_BASE_US + AVSYNC_STEP_US * ((uint64_t)(i + 2u)));
        NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_avsync_map_rtp_to_wall(&sync, rtp_ts, &ntp),
                                   NGX_RTC_OK);
        NGX_RTC_TEST_ASSERT(avsync_abs_diff(ntp, expected) <= AVSYNC_NTP_TOL);
    }
}

NGX_RTC_TEST(sync_timestamp_by_sender_report_out_of_order)
{
    ngx_rtc_avsync_t sync;
    uint64_t ntp;
    uint64_t expected;
    uint32_t rtp_ts;
    uint32_t i;

    ngx_rtc_avsync_init(&sync);

    /* The newer SR2 arrives first. */
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_avsync_on_sr(
                                   &sync,
                                   ngx_rtc_rtcp_ntp_from_unix_us(AVSYNC_BASE_US + AVSYNC_STEP_US),
                                   10000u + AVSYNC_STEP_RTP),
                               NGX_RTC_AGAIN);

    /* Late SR1 completes the slope but must not move the SR2 anchor. */
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_avsync_on_sr(
                                   &sync, ngx_rtc_rtcp_ntp_from_unix_us(AVSYNC_BASE_US),
                                   10000u),
                               NGX_RTC_OK);

    rtp_ts = 10000u + AVSYNC_STEP_RTP;
    for (i = 0u; i <= 1000u; i++)
    {
        rtp_ts += AVSYNC_STEP_RTP;
        expected = ngx_rtc_rtcp_ntp_from_unix_us(
            AVSYNC_BASE_US + AVSYNC_STEP_US * ((uint64_t)(i + 2u)));
        NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_avsync_map_rtp_to_wall(&sync, rtp_ts, &ntp),
                                   NGX_RTC_OK);
        NGX_RTC_TEST_ASSERT(avsync_abs_diff(ntp, expected) <= AVSYNC_NTP_TOL);
    }
}

NGX_RTC_TEST(sync_timestamp_by_sender_report_consecutive)
{
    ngx_rtc_avsync_t sync;
    uint64_t ntp;
    uint64_t expected;
    uint32_t rtp_ts;
    uint32_t i;

    ngx_rtc_avsync_init(&sync);

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_avsync_on_sr(
                                   &sync, ngx_rtc_rtcp_ntp_from_unix_us(AVSYNC_BASE_US),
                                   10000u),
                               NGX_RTC_AGAIN);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_avsync_on_sr(
                                   &sync,
                                   ngx_rtc_rtcp_ntp_from_unix_us(AVSYNC_BASE_US + AVSYNC_STEP_US),
                                   10000u + AVSYNC_STEP_RTP),
                               NGX_RTC_OK);

    rtp_ts = 10000u + AVSYNC_STEP_RTP;
    for (i = 0u; i <= 1000u; i++)
    {
        rtp_ts += AVSYNC_STEP_RTP;
        expected = ngx_rtc_rtcp_ntp_from_unix_us(
            AVSYNC_BASE_US + AVSYNC_STEP_US * ((uint64_t)(i + 2u)));
        NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_avsync_map_rtp_to_wall(&sync, rtp_ts, &ntp),
                                   NGX_RTC_OK);
        NGX_RTC_TEST_ASSERT(avsync_abs_diff(ntp, expected) <= AVSYNC_NTP_TOL);

        /* Periodic SR every 100 frames (4 s) refreshes the anchor. */
        if (99u == (i % 100u))
        {
            NGX_RTC_TEST_ASSERT_I64_EQ(
                ngx_rtc_avsync_on_sr(&sync,
                                     ngx_rtc_rtcp_ntp_from_unix_us(
                                         AVSYNC_BASE_US +
                                         AVSYNC_STEP_US * ((uint64_t)(i + 2u))),
                                     rtp_ts),
                NGX_RTC_OK);
        }
    }
}

NGX_RTC_TEST(sync_timestamp_by_sender_report_duplicated)
{
    ngx_rtc_avsync_t sync;
    uint64_t ntp;
    uint64_t expected;
    uint64_t sr_ntp;
    uint32_t rtp_ts;
    uint32_t sr_rtp;
    uint32_t i;

    ngx_rtc_avsync_init(&sync);

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_avsync_on_sr(
                                   &sync, ngx_rtc_rtcp_ntp_from_unix_us(AVSYNC_BASE_US),
                                   10000u),
                               NGX_RTC_AGAIN);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_avsync_on_sr(
                                   &sync,
                                   ngx_rtc_rtcp_ntp_from_unix_us(AVSYNC_BASE_US + AVSYNC_STEP_US),
                                   10000u + AVSYNC_STEP_RTP),
                               NGX_RTC_OK);

    rtp_ts = 10000u + AVSYNC_STEP_RTP;
    sr_rtp = 10000u + AVSYNC_STEP_RTP;
    sr_ntp = ngx_rtc_rtcp_ntp_from_unix_us(AVSYNC_BASE_US + AVSYNC_STEP_US);

    for (i = 0u; i <= 1000u; i++)
    {
        rtp_ts += AVSYNC_STEP_RTP;
        expected = ngx_rtc_rtcp_ntp_from_unix_us(
            AVSYNC_BASE_US + AVSYNC_STEP_US * ((uint64_t)(i + 2u)));
        NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_avsync_map_rtp_to_wall(&sync, rtp_ts, &ntp),
                                   NGX_RTC_OK);
        NGX_RTC_TEST_ASSERT(avsync_abs_diff(ntp, expected) <= AVSYNC_NTP_TOL);

        /* Refresh the SR every 3rd frame; the two in-between resends are the
         * same anchor and must be idempotent. */
        if (0u == (i % 3u))
        {
            sr_ntp = ngx_rtc_rtcp_ntp_from_unix_us(
                AVSYNC_BASE_US + AVSYNC_STEP_US * ((uint64_t)(i + 2u)));
            sr_rtp = rtp_ts;
        }
        NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_avsync_on_sr(&sync, sr_ntp, sr_rtp),
                                   NGX_RTC_OK);
    }
}

NGX_RTC_TEST(sync_timestamp_by_sender_report_rtp_ts_wrap)
{
    ngx_rtc_avsync_t sync;
    uint64_t ntp;
    uint64_t expected;
    const uint32_t rtp_before_wrap = 0xFFFFFF00u;
    const uint32_t rtp_after_wrap = 0x00000D10u; /* rtp_before_wrap + 3600 */
    uint32_t rtp_next;

    ngx_rtc_avsync_init(&sync);

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_avsync_on_sr(
                                   &sync, ngx_rtc_rtcp_ntp_from_unix_us(AVSYNC_BASE_US),
                                   rtp_before_wrap),
                               NGX_RTC_AGAIN);

    /* The second anchor straddles the 32-bit RTP timestamp wrap. */
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_avsync_on_sr(
                                   &sync,
                                   ngx_rtc_rtcp_ntp_from_unix_us(AVSYNC_BASE_US + AVSYNC_STEP_US),
                                   rtp_after_wrap),
                               NGX_RTC_OK);

    /* Forward across the wrap. */
    rtp_next = rtp_after_wrap + AVSYNC_STEP_RTP;
    expected = ngx_rtc_rtcp_ntp_from_unix_us(AVSYNC_BASE_US + 2u * AVSYNC_STEP_US);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_avsync_map_rtp_to_wall(&sync, rtp_next, &ntp),
                               NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT(avsync_abs_diff(ntp, expected) <= AVSYNC_NTP_TOL);

    /* Backward across the wrap lands on the first anchor. */
    expected = ngx_rtc_rtcp_ntp_from_unix_us(AVSYNC_BASE_US);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_avsync_map_rtp_to_wall(&sync, rtp_before_wrap, &ntp),
                               NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT(avsync_abs_diff(ntp, expected) <= AVSYNC_NTP_TOL);
}
