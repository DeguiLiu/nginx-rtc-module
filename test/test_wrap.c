/*
 * test_wrap.c - 16-bit RTP sequence / 32-bit RTP timestamp wrap-around.
 *
 * SRS references: KernelRTCTest.SequenceCompare / JitterSequence /
 *                 SrsRtcFrameBuilderSequenceWrapAroundFix.
 *
 * The host core has no standalone srs_rtp_seq_distance / jitter helper (that
 * arithmetic is the modular 16-bit distance in ngx_rtc_rtp_ring_get and is
 * covered by test_rtc_core.c), so these tests pin the wrap behaviour at the
 * boundaries the packetizers and codecs actually expose:
 *   - the caller-owned uint16_t sequence counter advancing across 65535 -> 0,
 *   - the 12-byte RTP header serialization at 0xFFFF / 0x0000,
 *   - the 32-bit ms -> 90kHz / 48kHz clock conversions wrapping mod 2^32,
 *   - the RTCP NACK PID+BLP expansion wrapping past 65535.
 */

#include "ngx_rtc_test.h"
#include "ngx_rtc_rtp.h"
#include "ngx_rtc_rtcp.h"

#define WRAP_PKT   1500
#define WRAP_MAX   8

static uint8_t  g_wrap_buf[WRAP_PKT];
static uint16_t g_wrap_seq[WRAP_MAX];
static uint32_t g_wrap_ts[WRAP_MAX];
static int      g_wrap_n;

static int32_t wrap_emit(void *opaque, const uint8_t *rtp, uint32_t len)
{
    int *count = (int *)opaque;

    (void)len;
    if (*count < WRAP_MAX)
    {
        g_wrap_seq[*count] = (uint16_t)(((uint16_t)rtp[2] << 8) | (uint16_t)rtp[3]);
        g_wrap_ts[*count] = ((uint32_t)rtp[4] << 24) | ((uint32_t)rtp[5] << 16) |
                            ((uint32_t)rtp[6] << 8) | (uint32_t)rtp[7];
    }
    (*count)++;
    return NGX_RTC_OK;
}

NGX_RTC_TEST(rtp_seq_wraps_16bit_single)
{
    uint8_t  nalu[4] = { 0x65u, 0x00u, 0x00u, 0x00u };
    uint16_t seq = 0xFFFFu;

    g_wrap_n = 0;
    NGX_RTC_TEST_ASSERT_I64_EQ(
        ngx_rtc_h264_packetize_single(nalu, sizeof(nalu), 90000u, &seq,
                                      0x10u, 102u, 0,
                                      g_wrap_buf, sizeof(g_wrap_buf),
                                      wrap_emit, &g_wrap_n),
        NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_wrap_n, 1);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_wrap_seq[0], 0xFFFF);
    NGX_RTC_TEST_ASSERT_U64_EQ(seq, 0u); /* counter wrapped to 0 */

    g_wrap_n = 0;
    NGX_RTC_TEST_ASSERT_I64_EQ(
        ngx_rtc_h264_packetize_single(nalu, sizeof(nalu), 90000u, &seq,
                                      0x10u, 102u, 0,
                                      g_wrap_buf, sizeof(g_wrap_buf),
                                      wrap_emit, &g_wrap_n),
        NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_wrap_n, 1);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_wrap_seq[0], 0u);
    NGX_RTC_TEST_ASSERT_U64_EQ(seq, 1u);
}

NGX_RTC_TEST(rtp_seq_wraps_16bit_fu_a)
{
    uint8_t  nalu[3000];
    uint16_t seq = 0xFFFEu;
    uint32_t i;

    nalu[0] = 0x65u;
    for (i = 1; i < sizeof(nalu); i++)
    {
        nalu[i] = (uint8_t)i;
    }

    g_wrap_n = 0;
    NGX_RTC_TEST_ASSERT_I64_EQ(
        ngx_rtc_h264_packetize_fu_a(nalu, sizeof(nalu), 90000u, &seq,
                                    0x11223344u, 102u, 1200u, 1,
                                    g_wrap_buf, sizeof(g_wrap_buf),
                                    wrap_emit, &g_wrap_n),
        NGX_RTC_OK);

    /* 1 + (len-1-1)/mtu = 1 + 2998/1200 = 3 fragments: 0xFFFE, 0xFFFF, 0x0000. */
    NGX_RTC_TEST_ASSERT_I64_EQ(g_wrap_n, 3);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_wrap_seq[0], 0xFFFE);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_wrap_seq[1], 0xFFFF);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_wrap_seq[2], 0x0000);
    NGX_RTC_TEST_ASSERT_U64_EQ(seq, 1u); /* 0xFFFE + 3 mod 2^16 = 1 */

    /* The wrap is only in the sequence counter: timestamp/SSRC stay constant. */
    NGX_RTC_TEST_ASSERT_U64_EQ(g_wrap_ts[0], 90000u);
    NGX_RTC_TEST_ASSERT_U64_EQ(g_wrap_ts[2], 90000u);
}

NGX_RTC_TEST(rtp_timestamp_32bit_wrap_values)
{
    ngx_rtc_rtp_header_t hdr;
    uint8_t buf[12];

    hdr.version_cc = NGX_RTC_RTP_VERSION_CC;
    hdr.marker_pt = 102u;
    hdr.seq = 0u;
    hdr.ssrc = 1u;

    hdr.timestamp = 0xFFFFFFFFu;
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtp_header_write(&hdr, buf, sizeof(buf)),
                               NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_I64_EQ(buf[4], 0xFF);
    NGX_RTC_TEST_ASSERT_I64_EQ(buf[7], 0xFF);

    /* The 32-bit timestamp wraps to zero exactly like the wire field does. */
    hdr.timestamp = 0u;
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtp_header_write(&hdr, buf, sizeof(buf)),
                               NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_I64_EQ(buf[4], 0x00);
    NGX_RTC_TEST_ASSERT_I64_EQ(buf[7], 0x00);

    hdr.timestamp = 0x80000000u;
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtp_header_write(&hdr, buf, sizeof(buf)),
                               NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_I64_EQ(buf[4], 0x80);
    NGX_RTC_TEST_ASSERT_I64_EQ(buf[5], 0x00);
    NGX_RTC_TEST_ASSERT_I64_EQ(buf[7], 0x00);
}

NGX_RTC_TEST(rtp_h264_timestamp_from_ms_wraps)
{
    /* 90 kHz x 47721858 ms = 4294967220 (< 2^32); one ms later crosses 2^32. */
    NGX_RTC_TEST_ASSERT_U64_EQ(ngx_rtc_h264_timestamp_from_ms(47721858u),
                               4294967220u);
    NGX_RTC_TEST_ASSERT_U64_EQ(ngx_rtc_h264_timestamp_from_ms(47721859u), 14u);

    /* Monotonic across the wrap: next 40ms step stays +3600 mod 2^32. */
    NGX_RTC_TEST_ASSERT_U64_EQ(
        (uint32_t)(ngx_rtc_h264_timestamp_from_ms(47721899u)
                   - ngx_rtc_h264_timestamp_from_ms(47721859u)),
        3600u);
}

NGX_RTC_TEST(rtp_opus_timestamp_from_ms_wraps)
{
    /* 48 kHz x 89478485 ms = 4294967280 (< 2^32); one ms later crosses 2^32. */
    NGX_RTC_TEST_ASSERT_U64_EQ(ngx_rtc_opus_timestamp_from_ms(89478485u),
                               4294967280u);
    NGX_RTC_TEST_ASSERT_U64_EQ(ngx_rtc_opus_timestamp_from_ms(89478486u), 32u);

    /* 20 ms Opus frames keep a +960 delta across the wrap. */
    NGX_RTC_TEST_ASSERT_U64_EQ(
        (uint32_t)(ngx_rtc_opus_timestamp_from_ms(89478506u)
                   - ngx_rtc_opus_timestamp_from_ms(89478486u)),
        960u);
}

NGX_RTC_TEST(rtcp_nack_expand_seq_wraps_16bit)
{
    ngx_rtc_rtcp_pkt_t pkt;
    uint16_t seqs[8];
    uint16_t count = 0;

    /* PID 65535 + BLP bit0 -> 65535, 0 (RFC 4585 6.2.1 wraps the 16-bit seq). */
    (void)memset(&pkt, 0, sizeof(pkt));
    pkt.nack_count = 1;
    pkt.nack_pid[0] = 0xFFFFu;
    pkt.nack_blp[0] = 0x0001u;

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtcp_nack_expand(&pkt, seqs, 8u, &count),
                               NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_I64_EQ(count, 2);
    NGX_RTC_TEST_ASSERT_I64_EQ(seqs[0], 0xFFFF);
    NGX_RTC_TEST_ASSERT_I64_EQ(seqs[1], 0);

    /* BLP bit15 -> 65535 + 16 wraps to 15. */
    count = 0;
    pkt.nack_blp[0] = 0x8000u;
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtcp_nack_expand(&pkt, seqs, 8u, &count),
                               NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_I64_EQ(count, 2);
    NGX_RTC_TEST_ASSERT_I64_EQ(seqs[1], 15);
}
