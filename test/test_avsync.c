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
 * INTEGRATION-TEST GAP: the host core has no avsync algorithm yet. ngx_rtc_rtcp
 * only decodes the SR fields (ntp / rtp_ts) and exposes the NTP conversion
 * utility; the mapping from a media RTP timestamp to a wall clock (what SRS
 * does in SrsRtcPublishStream::on_rtcp_sr + SrsRtcVideoRecvTrack::on_rtp) is
 * not present in any host-compiled source. The four scenarios above are kept
 * here as the contract for that future implementation; until then these tests
 * pin the codec layer that the sync code will build on: the SR anchor survives
 * the encode/decode round trip (including a 32-bit rtp_ts wrap) and the parser
 * stays stateless under out-of-order / duplicated SRs.
 */

#include "ngx_rtc_test.h"
#include "ngx_rtc_rtcp.h"

#define AVSYNC_BUF 256u

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
