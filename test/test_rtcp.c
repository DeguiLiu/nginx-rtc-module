/*
 * test_rtcp.c - host unit tests for ngx_rtc_rtcp.c (SR/SDES encode+parse,
 * NACK PID+BLP expansion, compound packets, NTP conversion).
 */

#include "ngx_rtc_test.h"
#include "ngx_rtc_rtcp.h"

/* Decode-callback capture state. */
#define RTCP_CB_MAX 8

static int   g_cb_types[RTCP_CB_MAX];
static int   g_cb_count;
static char  g_cb_cname[NGX_RTC_RTCP_MAX_CNAME];

static int32_t rtcp_capture_cb(const ngx_rtc_rtcp_pkt_t *pkt, void *opaque)
{
    (void)opaque;

    if (g_cb_count < RTCP_CB_MAX)
    {
        g_cb_types[g_cb_count++] = (int)pkt->type;
    }
    if (NGX_RTC_RTCP_SDES == pkt->type)
    {
        (void)memcpy(g_cb_cname, pkt->cname, sizeof(g_cb_cname));
        g_cb_cname[sizeof(g_cb_cname) - 1u] = '\0';
    }
    return NGX_RTC_OK;
}

static void rtcp_wr_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xffu);
}

static void rtcp_wr_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v & 0xffu);
}

NGX_RTC_TEST(rtcp_sr_roundtrip)
{
    ngx_rtc_rtcp_sr_t sr;
    ngx_rtc_rtcp_rb_t rb;
    ngx_rtc_rtcp_pkt_t pkt;
    uint8_t buf[256];
    uint32_t len = 0;
    uint32_t consumed = 0;

    (void)memset(&sr, 0, sizeof(sr));
    sr.ssrc = 0x11223344u;
    sr.ntp = 0x0102030405060708ULL;
    sr.rtp_ts = 90000u;
    sr.packet_count = 12345u;
    sr.octet_count = 67890u;

    rb.ssrc = 0xAABBCCDDu;
    rb.fraction_lost = 5u;
    rb.lost_packets = 0x010203u;
    rb.highest_seq = 0x01020304u;
    rb.jitter = 0x05060708u;
    rb.lsr = 0x090A0B0Cu;
    rb.dlsr = 0x0D0E0F10u;
    sr.rb = &rb;

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtcp_encode_sr(&sr, buf, sizeof(buf), &len),
                               NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_I64_EQ(len, 52u); /* 28 header + 24 report block */

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtcp_parse(buf, len, &pkt, &consumed),
                               NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_I64_EQ(consumed, len);
    NGX_RTC_TEST_ASSERT_I64_EQ(pkt.version, 2);
    NGX_RTC_TEST_ASSERT_I64_EQ(pkt.type, NGX_RTC_RTCP_SR);
    NGX_RTC_TEST_ASSERT_I64_EQ(pkt.fmt, 1);
    NGX_RTC_TEST_ASSERT_U64_EQ(pkt.ssrc, 0x11223344u);
    NGX_RTC_TEST_ASSERT_U64_EQ(pkt.ntp, 0x0102030405060708ULL);
    NGX_RTC_TEST_ASSERT_U64_EQ(pkt.rtp_ts, 90000u);
    NGX_RTC_TEST_ASSERT_U64_EQ(pkt.sender_packet_count, 12345u);
    NGX_RTC_TEST_ASSERT_U64_EQ(pkt.sender_octet_count, 67890u);
    NGX_RTC_TEST_ASSERT_I64_EQ(pkt.has_rb, 1);
    NGX_RTC_TEST_ASSERT_U64_EQ(pkt.rb.ssrc, 0xAABBCCDDu);
    NGX_RTC_TEST_ASSERT_I64_EQ(pkt.rb.fraction_lost, 5);
    NGX_RTC_TEST_ASSERT_U64_EQ(pkt.rb.lost_packets, 0x010203u);
    NGX_RTC_TEST_ASSERT_U64_EQ(pkt.rb.highest_seq, 0x01020304u);
    NGX_RTC_TEST_ASSERT_U64_EQ(pkt.rb.jitter, 0x05060708u);
    NGX_RTC_TEST_ASSERT_U64_EQ(pkt.rb.lsr, 0x090A0B0Cu);
    NGX_RTC_TEST_ASSERT_U64_EQ(pkt.rb.dlsr, 0x0D0E0F10u);
}

NGX_RTC_TEST(rtcp_sr_without_report_block)
{
    ngx_rtc_rtcp_sr_t sr;
    ngx_rtc_rtcp_pkt_t pkt;
    uint8_t buf[256];
    uint32_t len = 0;
    uint32_t consumed = 0;

    (void)memset(&sr, 0, sizeof(sr));
    sr.ssrc = 7u;
    sr.ntp = 0x0102030405060708ULL;
    sr.rb = NULL;

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtcp_encode_sr(&sr, buf, sizeof(buf), &len),
                               NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_I64_EQ(len, 28u);

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtcp_parse(buf, len, &pkt, &consumed),
                               NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_I64_EQ(pkt.fmt, 0);
    NGX_RTC_TEST_ASSERT_I64_EQ(pkt.has_rb, 0);
}

NGX_RTC_TEST(rtcp_sdes_roundtrip)
{
    ngx_rtc_rtcp_pkt_t pkt;
    uint8_t buf[256];
    uint32_t len = 0;
    uint32_t consumed = 0;

    NGX_RTC_TEST_ASSERT_I64_EQ(
        ngx_rtc_rtcp_encode_sdes(0x01020304u, "abc123", buf, sizeof(buf), &len),
        NGX_RTC_OK);

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtcp_parse(buf, len, &pkt, &consumed),
                               NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_I64_EQ(pkt.type, NGX_RTC_RTCP_SDES);
    NGX_RTC_TEST_ASSERT_I64_EQ(pkt.fmt, 1);
    NGX_RTC_TEST_ASSERT_U64_EQ(pkt.ssrc, 0x01020304u);
    NGX_RTC_TEST_ASSERT_STR_EQ(pkt.cname, "abc123");
}

NGX_RTC_TEST(rtcp_sdes_cname_too_long)
{
    char cname[257];
    uint8_t buf[256];
    uint32_t len = 0;

    (void)memset(cname, 'c', sizeof(cname));
    cname[256] = '\0'; /* 256 chars: one over the RFC 3550 255-octet limit */

    NGX_RTC_TEST_ASSERT_I64_EQ(
        ngx_rtc_rtcp_encode_sdes(1u, cname, buf, sizeof(buf), &len),
        NGX_RTC_ERR_TOO_LARGE);
}

NGX_RTC_TEST(rtcp_nack_expand)
{
    ngx_rtc_rtcp_pkt_t pkt;
    uint8_t raw[16];
    uint16_t seqs[16];
    uint16_t count = 0;
    uint32_t consumed = 0;

    /* One FCI entry: PID=100, BLP bits 0 and 2 set -> 100, 101, 103. */
    (void)memset(raw, 0, sizeof(raw));
    raw[0] = 0x81u;                 /* V=2, FMT=1 (generic NACK) */
    raw[1] = NGX_RTC_RTCP_RTPFB;
    rtcp_wr_u16(raw + 2, 3u);       /* 16 bytes = 4 words -> length 3 */
    raw[4] = 0x11; raw[5] = 0x22; raw[6] = 0x33; raw[7] = 0x44; /* sender */
    raw[8] = 0x55; raw[9] = 0x66; raw[10] = 0x77; raw[11] = 0x88; /* media */
    rtcp_wr_u16(raw + 12, 100u);    /* PID */
    rtcp_wr_u16(raw + 14, 0x0005u); /* BLP */

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtcp_parse(raw, sizeof(raw), &pkt, &consumed),
                               NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_I64_EQ(pkt.type, NGX_RTC_RTCP_RTPFB);
    NGX_RTC_TEST_ASSERT_I64_EQ(pkt.fmt, NGX_RTC_RTCP_FMT_NACK);
    NGX_RTC_TEST_ASSERT_U64_EQ(pkt.media_ssrc, 0x55667788u);
    NGX_RTC_TEST_ASSERT_I64_EQ(pkt.nack_count, 1);
    NGX_RTC_TEST_ASSERT_I64_EQ(pkt.nack_pid[0], 100);
    NGX_RTC_TEST_ASSERT_I64_EQ(pkt.nack_blp[0], 5);

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtcp_nack_expand(&pkt, seqs, 16u, &count),
                               NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_I64_EQ(count, 3);
    NGX_RTC_TEST_ASSERT_I64_EQ(seqs[0], 100);
    NGX_RTC_TEST_ASSERT_I64_EQ(seqs[1], 101);
    NGX_RTC_TEST_ASSERT_I64_EQ(seqs[2], 103);
}

NGX_RTC_TEST(rtcp_nack_expand_cap_and_wrap)
{
    ngx_rtc_rtcp_pkt_t pkt;
    uint16_t seqs[16];
    uint16_t count = 0;

    /* PID=200 BLP bit1 (-> 202); PID=300 BLP bit15 (-> 316). */
    (void)memset(&pkt, 0, sizeof(pkt));
    pkt.nack_count = 2;
    pkt.nack_pid[0] = 200u;
    pkt.nack_blp[0] = 0x0002u;
    pkt.nack_pid[1] = 300u;
    pkt.nack_blp[1] = 0x8000u;

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtcp_nack_expand(&pkt, seqs, 16u, &count),
                               NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_I64_EQ(count, 4);
    NGX_RTC_TEST_ASSERT_I64_EQ(seqs[0], 200);
    NGX_RTC_TEST_ASSERT_I64_EQ(seqs[1], 202);
    NGX_RTC_TEST_ASSERT_I64_EQ(seqs[2], 300);
    NGX_RTC_TEST_ASSERT_I64_EQ(seqs[3], 316);

    /* Too-small output returns NGX_RTC_ERR_TOO_SMALL and keeps the count. */
    count = 0;
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtcp_nack_expand(&pkt, seqs, 3u, &count),
                               NGX_RTC_ERR_TOO_SMALL);
    NGX_RTC_TEST_ASSERT_I64_EQ(count, 3);
    NGX_RTC_TEST_ASSERT_I64_EQ(seqs[2], 300);
}

NGX_RTC_TEST(rtcp_ntp_conversion)
{
    uint64_t t;

    t = ngx_rtc_rtcp_ntp_from_unix_us(0u);
    NGX_RTC_TEST_ASSERT_U64_EQ(t, 2208988800ULL << 32);

    /* 0.5 s after the Unix epoch -> fraction 0x80000000. */
    t = ngx_rtc_rtcp_ntp_from_unix_us(500000u);
    NGX_RTC_TEST_ASSERT_U64_EQ(t, (2208988800ULL << 32) | 0x80000000ULL);

    /* Exactly one second: seconds carry, fraction zero. */
    t = ngx_rtc_rtcp_ntp_from_unix_us(1000000u);
    NGX_RTC_TEST_ASSERT_U64_EQ(t, (2208988800ULL + 1ULL) << 32);

    /* The NTP epoch offset is included in the seconds word. */
    NGX_RTC_TEST_ASSERT((ngx_rtc_rtcp_ntp_from_unix_us(0u) >> 32)
                        == (2208988800ULL));
}

NGX_RTC_TEST(rtcp_compound_encode_decode)
{
    ngx_rtc_rtcp_sr_t sr;
    uint8_t sr_buf[256];
    uint8_t sdes_buf[256];
    uint8_t compound[512];
    uint32_t sr_len = 0;
    uint32_t sdes_len = 0;
    uint32_t clen = 0;

    (void)memset(&sr, 0, sizeof(sr));
    sr.ssrc = 0xDEADBEEFu;
    sr.ntp = 0x0102030405060708ULL;
    sr.rb = NULL;

    NGX_RTC_TEST_ASSERT_I64_EQ(
        ngx_rtc_rtcp_encode_sr(&sr, sr_buf, sizeof(sr_buf), &sr_len), NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_I64_EQ(
        ngx_rtc_rtcp_encode_sdes(sr.ssrc, "cname42", sdes_buf, sizeof(sdes_buf),
                                 &sdes_len), NGX_RTC_OK);

    NGX_RTC_TEST_ASSERT_I64_EQ(
        ngx_rtc_rtcp_compound_append(compound, sizeof(compound), &clen,
                                     sr_buf, sr_len), NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_I64_EQ(
        ngx_rtc_rtcp_compound_append(compound, sizeof(compound), &clen,
                                     sdes_buf, sdes_len), NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_I64_EQ(clen, sr_len + sdes_len);

    g_cb_count = 0;
    (void)memset(g_cb_cname, 0, sizeof(g_cb_cname));
    NGX_RTC_TEST_ASSERT_I64_EQ(
        ngx_rtc_rtcp_decode(compound, clen, rtcp_capture_cb, NULL), NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_cb_count, 2);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_cb_types[0], NGX_RTC_RTCP_SR);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_cb_types[1], NGX_RTC_RTCP_SDES);
    NGX_RTC_TEST_ASSERT_STR_EQ(g_cb_cname, "cname42");
}

NGX_RTC_TEST(rtcp_parse_errors)
{
    ngx_rtc_rtcp_pkt_t pkt;
    uint8_t raw[16];
    uint32_t consumed = 0;

    /* Truncated input. */
    (void)memset(raw, 0, sizeof(raw));
    raw[0] = 0x81u;
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtcp_parse(raw, 2u, &pkt, &consumed),
                               NGX_RTC_ERR_NEED_MORE);

    /* Declared length larger than the buffer. */
    rtcp_wr_u16(raw + 2, 100u);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtcp_parse(raw, sizeof(raw), &pkt, &consumed),
                               NGX_RTC_ERR_NEED_MORE);

    /* Bad version. */
    raw[0] = 0x00u;
    rtcp_wr_u16(raw + 2, 0u);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtcp_parse(raw, sizeof(raw), &pkt, &consumed),
                               NGX_RTC_ERR_PARSE);
}

NGX_RTC_TEST(rtcp_twcc_loss_summary)
{
    ngx_rtc_rtcp_pkt_t pkt;
    uint8_t raw[24];
    uint32_t consumed = 0;

    (void)memset(raw, 0, sizeof(raw));

    /* V=2, P=0, FMT=15 (TWCC), PT=205 (RTPFB), length = 5 words minus one. */
    raw[0] = 0x8Fu;
    raw[1] = NGX_RTC_RTCP_RTPFB;
    rtcp_wr_u16(raw + 2, 5u);
    rtcp_wr_u32(raw + 4, 0x11111111u); /* sender SSRC */
    rtcp_wr_u32(raw + 8, 0x22222222u); /* media SSRC */

    /* FCI fixed part: base seq, packet status count, reference time, fb count */
    rtcp_wr_u16(raw + 12, 100u);
    rtcp_wr_u16(raw + 14, 5u);

    /* chunks: T=0 S=0 run=2 (not received), then T=0 S=2 run=3 (received) */
    rtcp_wr_u16(raw + 20, 0x0002u);
    rtcp_wr_u16(raw + 22, 0x4003u);

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtcp_parse(raw, sizeof(raw), &pkt,
                                                  &consumed), NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_I64_EQ(pkt.twcc_base_seq, 100);
    NGX_RTC_TEST_ASSERT_I64_EQ(pkt.twcc_pkt_count, 5);
    NGX_RTC_TEST_ASSERT_I64_EQ(pkt.twcc_lost, 2);
    NGX_RTC_TEST_ASSERT_I64_EQ(pkt.twcc_received, 3);
}

NGX_RTC_TEST(rtcp_twcc_status_vector)
{
    ngx_rtc_rtcp_pkt_t pkt;
    uint8_t raw[24];
    uint32_t consumed = 0;

    (void)memset(raw, 0, sizeof(raw));

    raw[0] = 0x8Fu;
    raw[1] = NGX_RTC_RTCP_RTPFB;
    rtcp_wr_u16(raw + 2, 5u);
    rtcp_wr_u32(raw + 4, 0x11111111u);
    rtcp_wr_u32(raw + 8, 0x22222222u);
    rtcp_wr_u16(raw + 12, 200u);
    rtcp_wr_u16(raw + 14, 7u);

    /* Status vector chunk T=1, S=1: seven 2-bit symbols in bits 13..0, here
     * 00 00 01 10 11 01 10. Two of them are "not received". Nothing else in the
     * suite exercises this layout, and the 1-bit form below is a separate branch,
     * so a wrong shift or a wrong symbol count here would otherwise go unseen. */
    rtcp_wr_u16(raw + 20, 0xC1B6u);

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtcp_parse(raw, sizeof(raw), &pkt,
                                                  &consumed), NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_I64_EQ(pkt.twcc_pkt_count, 7);
    NGX_RTC_TEST_ASSERT_I64_EQ(pkt.twcc_lost, 2);
    NGX_RTC_TEST_ASSERT_I64_EQ(pkt.twcc_received, 5);
}

/* draft-holmer-rmcat-transport-wide-cc-extensions-01 section 3.1.4 gives the
 * status vector chunk a symbol-size bit: bit 15 is the chunk type, bit 14 is S,
 * and bits 13..0 hold either fourteen 1-bit symbols (S=0) or seven 2-bit symbols
 * (S=1). A parser that assumes the 2-bit form unconditionally reads an S=0 chunk
 * as 7 symbols instead of 14, then walks the rest of the feedback at the wrong
 * stride, drifts into the recv-delta section, and counts arrival-time bytes as
 * packet status -- which is how a path with no loss at all comes to be reported
 * as most of the stream lost. (RFC 8888 does not define this chunk at all.) */
NGX_RTC_TEST(rtcp_twcc_status_vector_one_bit_symbols)
{
    ngx_rtc_rtcp_pkt_t pkt;
    uint8_t raw[24];
    uint32_t consumed = 0;

    (void)memset(raw, 0, sizeof(raw));

    raw[0] = 0x8Fu;
    raw[1] = NGX_RTC_RTCP_RTPFB;
    rtcp_wr_u16(raw + 2, 5u);
    rtcp_wr_u32(raw + 4, 0x11111111u);
    rtcp_wr_u32(raw + 8, 0x22222222u);
    rtcp_wr_u16(raw + 12, 300u);
    rtcp_wr_u16(raw + 14, 14u);

    /* T=1, S=0: fourteen 1-bit symbols, twelve received then two lost
     * (0xBFFC = 10 11111111111100). A zero symbol means "not received" in both
     * symbol forms, which follows section 3.1.4's own Example 1 (its leading 0
     * is labelled "packet not received") rather than its prose, where "packet
     * received" is (0).
     *
     * The totals must be ASYMMETRIC to pin that down. An earlier revision of
     * this case used 0xBF80 (seven and seven) and passed unchanged under a
     * polarity reversal -- the mutation swapped the two counters, so a test
     * asserting 7 and 7 could not see it. The count assertion below is what
     * pins the symbol WIDTH: read as seven 2-bit symbols these same bits
     * account for only seven of the fourteen declared packets.
     *
     * Order is deliberately not asserted here: reversing a bit string preserves
     * its number of ones and zeros, so no assertion over twcc_lost /
     * twcc_received -- the only order-free outputs the parser produces -- can
     * ever catch a reader that walks the symbol list backwards. */
    rtcp_wr_u16(raw + 20, 0xBFFCu);

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtcp_parse(raw, sizeof(raw), &pkt,
                                                  &consumed), NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_I64_EQ(pkt.twcc_pkt_count, 14);
    NGX_RTC_TEST_ASSERT_I64_EQ(pkt.twcc_lost, 2);
    NGX_RTC_TEST_ASSERT_I64_EQ(pkt.twcc_received, 12);
}

/* draft-holmer-rmcat-transport-wide-cc-extensions-01 section 3.1.3 lays the run
 * length chunk out as T (1 bit) | S (2 bits) | run length (13 bits). Reading a
 * single status bit and a 14-bit run -- easy to do, because the *status vector*
 * chunk's size flag really is one bit -- shifts everything by one: a normal
 * "received, small delta" run (S=1) has bit 14 clear, so it is read as a lost
 * run, and its length picks up the S bit and becomes 8192 + n, which the
 * caller's clamp turns into "everything left in this feedback was lost". Almost
 * every arrival takes that path, so a link with no loss reports near-total loss
 * and any loss-feedback controller collapses to its floor. */
NGX_RTC_TEST(rtcp_twcc_run_length_chunk_symbol_bits)
{
    ngx_rtc_rtcp_pkt_t pkt;
    uint8_t raw[24];
    uint32_t consumed = 0;

    (void)memset(raw, 0, sizeof(raw));

    raw[0] = 0x8Fu;
    raw[1] = NGX_RTC_RTCP_RTPFB;
    rtcp_wr_u16(raw + 2, 5u);
    rtcp_wr_u32(raw + 4, 0x11111111u);
    rtcp_wr_u32(raw + 8, 0x22222222u);
    rtcp_wr_u16(raw + 12, 400u);
    rtcp_wr_u16(raw + 14, 5u);

    /* T=0, S=1 (received, small delta), run length 5: every reported packet
     * arrived. Read with the draft layout this is five lost packets. */
    rtcp_wr_u16(raw + 20, 0x2005u);

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtcp_parse(raw, sizeof(raw), &pkt,
                                                  &consumed), NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_I64_EQ(pkt.twcc_lost, 0);
    NGX_RTC_TEST_ASSERT_I64_EQ(pkt.twcc_received, 5);

    /* T=0, S=2 (received, large delta), run length 5. */
    (void)memset(&pkt, 0, sizeof(pkt));
    rtcp_wr_u16(raw + 20, 0x4005u);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtcp_parse(raw, sizeof(raw), &pkt,
                                                  &consumed), NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_I64_EQ(pkt.twcc_lost, 0);
    NGX_RTC_TEST_ASSERT_I64_EQ(pkt.twcc_received, 5);

    /* T=0, S=0 (not received), run length 5. */
    (void)memset(&pkt, 0, sizeof(pkt));
    rtcp_wr_u16(raw + 20, 0x0005u);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtcp_parse(raw, sizeof(raw), &pkt,
                                                  &consumed), NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_I64_EQ(pkt.twcc_lost, 5);
    NGX_RTC_TEST_ASSERT_I64_EQ(pkt.twcc_received, 0);

    /* A run longer than the declared packet count is clamped to it, so a bogus
     * or overlapping feedback cannot inflate the totals past packet_count. */
    (void)memset(&pkt, 0, sizeof(pkt));
    rtcp_wr_u16(raw + 20, 0x3FFFu);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtcp_parse(raw, sizeof(raw), &pkt,
                                                  &consumed), NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_I64_EQ(pkt.twcc_lost + pkt.twcc_received, 5);

    /* Symbol 3 means received (without a recv delta), not "reserved": reading it
     * as a loss would invert the status of every run a sender emits that way. */
    (void)memset(&pkt, 0, sizeof(pkt));
    rtcp_wr_u16(raw + 20, 0x6005u);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtcp_parse(raw, sizeof(raw), &pkt,
                                                  &consumed), NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_I64_EQ(pkt.twcc_lost, 0);
    NGX_RTC_TEST_ASSERT_I64_EQ(pkt.twcc_received, 5);

    /* The symbol field is TWO bits wide, not three. 0x1005 = 0001 0000 0000
     * 0101 puts the symbol at 00 (not received) with the top bit of the run
     * length set; read with a 3-bit symbol field those three bits are 001 --
     * received -- so the entire run flips from lost to received. Every vector
     * above reads as non-zero under both widths, which is why this misread
     * survived them all; this is the placement that separates the two. */
    (void)memset(&pkt, 0, sizeof(pkt));
    rtcp_wr_u16(raw + 20, 0x1005u);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtcp_parse(raw, sizeof(raw), &pkt,
                                                  &consumed), NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_I64_EQ(pkt.twcc_lost, 5);
    NGX_RTC_TEST_ASSERT_I64_EQ(pkt.twcc_received, 0);
}

NGX_RTC_TEST(rtcp_remb_single_ssrc)
{
    ngx_rtc_rtcp_pkt_t pkt;
    uint8_t raw[24];
    uint32_t consumed = 0;

    (void)memset(raw, 0, sizeof(raw));

    /* V=2, P=0, FMT=15 (REMB), PT=206 (PSFB), 24 bytes -> length 5. */
    raw[0] = 0x8Fu;
    raw[1] = NGX_RTC_RTCP_PSFB;
    rtcp_wr_u16(raw + 2, 5u);
    rtcp_wr_u32(raw + 4, 0x11111111u); /* sender SSRC */
    rtcp_wr_u32(raw + 8, 0x22222222u); /* media SSRC */
    raw[12] = 'R'; raw[13] = 'E'; raw[14] = 'M'; raw[15] = 'B';
    /* numSSRC=1, brExp=3, brMantissa=62500 -> 62500 * 2^3 = 500000 bps. */
    rtcp_wr_u32(raw + 16, (1u << 24) | (3u << 18) | 62500u);
    rtcp_wr_u32(raw + 20, 0xAABBCCDDu); /* REMB SSRC */

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtcp_parse(raw, sizeof(raw), &pkt, &consumed),
                               NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_I64_EQ(consumed, 24u);
    NGX_RTC_TEST_ASSERT_I64_EQ(pkt.type, NGX_RTC_RTCP_PSFB);
    NGX_RTC_TEST_ASSERT_I64_EQ(pkt.fmt, NGX_RTC_RTCP_FMT_REMB);
    NGX_RTC_TEST_ASSERT_U64_EQ(pkt.media_ssrc, 0x22222222u);
    NGX_RTC_TEST_ASSERT_I64_EQ(pkt.has_remb, 1);
    NGX_RTC_TEST_ASSERT_U64_EQ(pkt.remb_bitrate_bps, 500000u);
    NGX_RTC_TEST_ASSERT_U64_EQ(pkt.remb_ssrc, 0xAABBCCDDu);
    NGX_RTC_TEST_ASSERT_I64_EQ(pkt.remb_ssrc_count, 1);
    NGX_RTC_TEST_ASSERT_U64_EQ(pkt.remb_ssrcs[0], 0xAABBCCDDu);
}

NGX_RTC_TEST(rtcp_remb_multi_ssrc)
{
    ngx_rtc_rtcp_pkt_t pkt;
    uint8_t raw[32];
    uint32_t consumed = 0;

    (void)memset(raw, 0, sizeof(raw));

    raw[0] = 0x8Fu;
    raw[1] = NGX_RTC_RTCP_PSFB;
    rtcp_wr_u16(raw + 2, 7u); /* 32 bytes -> length 7 */
    rtcp_wr_u32(raw + 4, 0x11111111u);
    rtcp_wr_u32(raw + 8, 0x22222222u);
    raw[12] = 'R'; raw[13] = 'E'; raw[14] = 'M'; raw[15] = 'B';
    /* numSSRC=3, brExp=5, brMantissa=4096 -> 4096 * 2^5 = 131072 bps. */
    rtcp_wr_u32(raw + 16, (3u << 24) | (5u << 18) | 4096u);
    rtcp_wr_u32(raw + 20, 0x00000001u);
    rtcp_wr_u32(raw + 24, 0x00000002u);
    rtcp_wr_u32(raw + 28, 0x00000003u);

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtcp_parse(raw, sizeof(raw), &pkt, &consumed),
                               NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_I64_EQ(pkt.has_remb, 1);
    NGX_RTC_TEST_ASSERT_U64_EQ(pkt.remb_bitrate_bps, 131072u);
    NGX_RTC_TEST_ASSERT_I64_EQ(pkt.remb_ssrc_count, 3);
    NGX_RTC_TEST_ASSERT_U64_EQ(pkt.remb_ssrc, 0x00000001u);
    NGX_RTC_TEST_ASSERT_U64_EQ(pkt.remb_ssrcs[0], 0x00000001u);
    NGX_RTC_TEST_ASSERT_U64_EQ(pkt.remb_ssrcs[1], 0x00000002u);
    NGX_RTC_TEST_ASSERT_U64_EQ(pkt.remb_ssrcs[2], 0x00000003u);
}

NGX_RTC_TEST(rtcp_remb_truncated_ssrc_list)
{
    ngx_rtc_rtcp_pkt_t pkt;
    uint8_t raw[24];
    uint32_t consumed = 0;

    (void)memset(raw, 0, sizeof(raw));

    raw[0] = 0x8Fu;
    raw[1] = NGX_RTC_RTCP_PSFB;
    rtcp_wr_u16(raw + 2, 5u); /* 24 bytes */
    rtcp_wr_u32(raw + 4, 0x11111111u);
    rtcp_wr_u32(raw + 8, 0x22222222u);
    raw[12] = 'R'; raw[13] = 'E'; raw[14] = 'M'; raw[15] = 'B';
    /* numSSRC=2 but only one 4-byte SSRC follows. */
    rtcp_wr_u32(raw + 16, (2u << 24) | (0u << 18) | 1000u);
    rtcp_wr_u32(raw + 20, 0xAABBCCDDu);

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtcp_parse(raw, sizeof(raw), &pkt, &consumed),
                               NGX_RTC_ERR_PARSE);
}

NGX_RTC_TEST(rtcp_remb_missing_bitrate_word)
{
    ngx_rtc_rtcp_pkt_t pkt;
    uint8_t raw[16];
    uint32_t consumed = 0;

    (void)memset(raw, 0, sizeof(raw));

    raw[0] = 0x8Fu;
    raw[1] = NGX_RTC_RTCP_PSFB;
    rtcp_wr_u16(raw + 2, 3u); /* 16 bytes */
    rtcp_wr_u32(raw + 4, 0x11111111u);
    rtcp_wr_u32(raw + 8, 0x22222222u);
    raw[12] = 'R'; raw[13] = 'E'; raw[14] = 'M'; raw[15] = 'B';

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtcp_parse(raw, sizeof(raw), &pkt, &consumed),
                               NGX_RTC_ERR_PARSE);
}

NGX_RTC_TEST(rtcp_remb_not_remb_afb)
{
    ngx_rtc_rtcp_pkt_t pkt;
    uint8_t raw[16];
    uint32_t consumed = 0;

    (void)memset(raw, 0, sizeof(raw));

    raw[0] = 0x8Fu;
    raw[1] = NGX_RTC_RTCP_PSFB;
    rtcp_wr_u16(raw + 2, 3u); /* 16 bytes */
    rtcp_wr_u32(raw + 4, 0x11111111u);
    rtcp_wr_u32(raw + 8, 0x22222222u);
    rtcp_wr_u32(raw + 12, 0xDEADBEEFu); /* application data, not "REMB" */

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtcp_parse(raw, sizeof(raw), &pkt, &consumed),
                               NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_I64_EQ(pkt.has_remb, 0);
    NGX_RTC_TEST_ASSERT_I64_EQ(pkt.remb_ssrc_count, 0);
}

NGX_RTC_TEST(rtcp_remb_pli_not_misdetected)
{
    ngx_rtc_rtcp_pkt_t pkt;
    uint8_t raw[12];
    uint32_t consumed = 0;

    (void)memset(raw, 0, sizeof(raw));

    raw[0] = 0x81u; /* V=2, FMT=1 (PLI) */
    raw[1] = NGX_RTC_RTCP_PSFB;
    rtcp_wr_u16(raw + 2, 2u); /* 12 bytes */
    rtcp_wr_u32(raw + 4, 0x11111111u);
    rtcp_wr_u32(raw + 8, 0x22222222u);

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtcp_parse(raw, sizeof(raw), &pkt, &consumed),
                               NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_I64_EQ(pkt.type, NGX_RTC_RTCP_PSFB);
    NGX_RTC_TEST_ASSERT_I64_EQ(pkt.fmt, NGX_RTC_RTCP_FMT_PLI);
    NGX_RTC_TEST_ASSERT_I64_EQ(pkt.has_remb, 0);
}

/*
 * The uplink keyframe request. A PLI is a PSFB with FMT=1 and no FCI: common
 * header, sender SSRC, media SSRC, nothing else (RFC 4585 6.3.1). It has to
 * survive a round trip through the same parser this module reads feedback with,
 * otherwise we would be sending something we could not ourselves recognise.
 */
NGX_RTC_TEST(rtcp_encode_pli_round_trip)
{
    ngx_rtc_rtcp_pkt_t pkt;
    uint8_t buf[NGX_RTC_RTCP_MAX_PACKET];
    uint32_t len = 0;
    uint32_t consumed = 0;

    NGX_RTC_TEST_ASSERT_I64_EQ(
            ngx_rtc_rtcp_encode_pli(0x11111111u, 0x22222222u, buf, sizeof(buf), &len),
            NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_I64_EQ(len, 12u);

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtcp_parse(buf, len, &pkt, &consumed),
                               NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_I64_EQ(consumed, len);
    NGX_RTC_TEST_ASSERT_I64_EQ(pkt.version, 2);
    NGX_RTC_TEST_ASSERT_I64_EQ(pkt.type, NGX_RTC_RTCP_PSFB);
    NGX_RTC_TEST_ASSERT_I64_EQ(pkt.fmt, NGX_RTC_RTCP_FMT_PLI);
    NGX_RTC_TEST_ASSERT_U64_EQ(pkt.ssrc, 0x11111111u);
    NGX_RTC_TEST_ASSERT_U64_EQ(pkt.media_ssrc, 0x22222222u);
}

NGX_RTC_TEST(rtcp_encode_pli_rejects_small_buffer)
{
    uint8_t buf[11];
    uint32_t len = 0;

    NGX_RTC_TEST_ASSERT_I64_EQ(
            ngx_rtc_rtcp_encode_pli(1u, 2u, buf, sizeof(buf), &len),
            NGX_RTC_ERR_TOO_SMALL);
}

NGX_RTC_TEST(rtcp_remb_ssrc_list_truncated_to_bound)
{
    ngx_rtc_rtcp_pkt_t pkt;
    uint8_t raw[180];
    uint32_t consumed = 0;
    uint32_t i;

    (void)memset(raw, 0, sizeof(raw));

    raw[0] = 0x8Fu;
    raw[1] = NGX_RTC_RTCP_PSFB;
    rtcp_wr_u16(raw + 2, (uint16_t)(sizeof(raw) / 4u - 1u)); /* 180 bytes -> 44 */
    rtcp_wr_u32(raw + 4, 0x11111111u);
    rtcp_wr_u32(raw + 8, 0x22222222u);
    raw[12] = 'R'; raw[13] = 'E'; raw[14] = 'M'; raw[15] = 'B';
    /* numSSRC=40 (over the 32-entry bound), brExp=1, brMantissa=1000 -> 2000 bps. */
    rtcp_wr_u32(raw + 16, (40u << 24) | (1u << 18) | 1000u);
    for (i = 0; i < 40u; i++)
    {
        rtcp_wr_u32(raw + 20 + i * 4u, 0xA0000000u + i);
    }

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtcp_parse(raw, sizeof(raw), &pkt, &consumed),
                               NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_I64_EQ(pkt.has_remb, 1);
    NGX_RTC_TEST_ASSERT_U64_EQ(pkt.remb_bitrate_bps, 2000u);
    NGX_RTC_TEST_ASSERT_I64_EQ(pkt.remb_ssrc_count, NGX_RTC_RTCP_MAX_REMB_SSRCS);
    NGX_RTC_TEST_ASSERT_U64_EQ(pkt.remb_ssrc, 0xA0000000u);
    NGX_RTC_TEST_ASSERT_U64_EQ(pkt.remb_ssrcs[0], 0xA0000000u);
    NGX_RTC_TEST_ASSERT_U64_EQ(pkt.remb_ssrcs[31], 0xA0000000u + 31u);
}
