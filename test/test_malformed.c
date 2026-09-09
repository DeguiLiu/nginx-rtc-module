/*
 * test_malformed.c - malformed-input crash regression (RTP / RTCP / STUN).
 *
 * SRS references: KernelRTCTest.RtpSTAPPayloadException /
 *                 DecodeHeaderWithPadding / SrsRtcFrameBuilderPacketVideoRtmpNullPointerCrash.
 *
 * The host core only implements the RTP packetization (encode) side, not an RTP
 * decoder, so the RTP cases below pin the null / oversize guards of the
 * packetizers; RTCP and STUN cover their decode paths (null pointers,
 * truncated / padded / over-declared inputs must fail cleanly, never crash).
 */

#include "ngx_rtc_test.h"
#include "ngx_rtc_rtp.h"
#include "ngx_rtc_rtcp.h"
#include "ngx_rtc_stun.h"

/* ------------------------------------------------------------------ */
/* Helpers.                                                            */
/* ------------------------------------------------------------------ */

static int32_t mal_emit(void *opaque, const uint8_t *rtp, uint32_t len)
{
    (void)opaque;
    (void)rtp;
    (void)len;
    return NGX_RTC_OK;
}

static void mal_wr_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFFu);
}

static int mal_cb_count;

static int32_t mal_rtcp_cb(const ngx_rtc_rtcp_pkt_t *pkt, void *opaque)
{
    (void)pkt;
    (void)opaque;
    mal_cb_count++;
    return NGX_RTC_OK;
}

/* Build a minimal STUN binding-request header (no attributes). */
static void mal_stun_header(uint8_t *buf, uint16_t message_len)
{
    uint32_t i;

    (void)memset(buf, 0, 20u);
    mal_wr_u16(buf, NGX_RTC_STUN_BINDING_REQUEST);
    mal_wr_u16(buf + 2, message_len);
    mal_wr_u16(buf + 4, (uint16_t)(NGX_RTC_STUN_MAGIC_COOKIE >> 16));
    mal_wr_u16(buf + 6, (uint16_t)(NGX_RTC_STUN_MAGIC_COOKIE & 0xFFFFu));
    for (i = 0u; i < 12u; i++)
    {
        buf[8u + i] = (uint8_t)i;
    }
}

/* ------------------------------------------------------------------ */
/* RTP packetizer guards.                                              */
/* ------------------------------------------------------------------ */

NGX_RTC_TEST(rtp_packetize_null_guards)
{
    uint8_t nalu[4] = { 0x65u, 0x00u, 0x00u, 0x00u };
    uint8_t sps[2] = { 0x67u, 0x42u };
    const uint8_t *nalus[1] = { sps };
    uint32_t sizes[1] = { 2u };
    uint16_t seq = 0u;
    uint8_t scratch[1500];
    ngx_rtc_rtp_header_t hdr;
    uint8_t hbuf[12];
    uint32_t off = 0;
    int n = 0;

    NGX_RTC_TEST_ASSERT_I64_EQ(
        ngx_rtc_h264_packetize(NULL, 4u, 0u, &seq, 0u, 102u, 0,
                               scratch, sizeof(scratch), mal_emit, &n),
        NGX_RTC_ERR_INVALID);
    NGX_RTC_TEST_ASSERT_I64_EQ(
        ngx_rtc_h264_packetize(nalu, 4u, 0u, NULL, 0u, 102u, 0,
                               scratch, sizeof(scratch), mal_emit, &n),
        NGX_RTC_ERR_INVALID);
    NGX_RTC_TEST_ASSERT_I64_EQ(
        ngx_rtc_h264_packetize(nalu, 4u, 0u, &seq, 0u, 102u, 0,
                               NULL, sizeof(scratch), mal_emit, &n),
        NGX_RTC_ERR_INVALID);
    NGX_RTC_TEST_ASSERT_I64_EQ(
        ngx_rtc_h264_packetize(nalu, 4u, 0u, &seq, 0u, 102u, 0,
                               scratch, sizeof(scratch), NULL, &n),
        NGX_RTC_ERR_INVALID);
    NGX_RTC_TEST_ASSERT_I64_EQ(
        ngx_rtc_h264_packetize(nalu, 0u, 0u, &seq, 0u, 102u, 0,
                               scratch, sizeof(scratch), mal_emit, &n),
        NGX_RTC_ERR_INVALID);

    NGX_RTC_TEST_ASSERT_I64_EQ(
        ngx_rtc_h264_packetize_fu_a(nalu, 1u, 0u, &seq, 0u, 102u, 1200u, 0,
                                    scratch, sizeof(scratch), mal_emit, &n),
        NGX_RTC_ERR_INVALID);
    NGX_RTC_TEST_ASSERT_I64_EQ(
        ngx_rtc_h264_packetize_fu_a(nalu, 4u, 0u, &seq, 0u, 102u, 0u, 0,
                                    scratch, sizeof(scratch), mal_emit, &n),
        NGX_RTC_ERR_INVALID);

    NGX_RTC_TEST_ASSERT_I64_EQ(
        ngx_rtc_h264_packetize_stap_a(NULL, sizes, 1u, 0u, &seq, 0u, 102u,
                                      scratch, sizeof(scratch), mal_emit, &n),
        NGX_RTC_ERR_INVALID);
    NGX_RTC_TEST_ASSERT_I64_EQ(
        ngx_rtc_h264_packetize_stap_a(nalus, NULL, 1u, 0u, &seq, 0u, 102u,
                                      scratch, sizeof(scratch), mal_emit, &n),
        NGX_RTC_ERR_INVALID);
    NGX_RTC_TEST_ASSERT_I64_EQ(
        ngx_rtc_h264_packetize_stap_a(nalus, sizes, 0u, 0u, &seq, 0u, 102u,
                                      scratch, sizeof(scratch), mal_emit, &n),
        NGX_RTC_ERR_INVALID);

    NGX_RTC_TEST_ASSERT_I64_EQ(
        ngx_rtc_opus_packetize(NULL, 4u, 0u, &seq, 0u, 111u, 0,
                               scratch, sizeof(scratch), mal_emit, &n),
        NGX_RTC_ERR_INVALID);
    NGX_RTC_TEST_ASSERT_I64_EQ(
        ngx_rtc_opus_packetize(nalu, 0u, 0u, &seq, 0u, 111u, 0,
                               scratch, sizeof(scratch), mal_emit, &n),
        NGX_RTC_ERR_INVALID);

    hdr.version_cc = NGX_RTC_RTP_VERSION_CC;
    hdr.marker_pt = 102u;
    hdr.seq = 0u;
    hdr.timestamp = 0u;
    hdr.ssrc = 0u;
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtp_header_write(NULL, hbuf, sizeof(hbuf)),
                               NGX_RTC_ERR_INVALID);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtp_header_write(&hdr, NULL, sizeof(hbuf)),
                               NGX_RTC_ERR_INVALID);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtp_header_write(&hdr, hbuf, 11u),
                               NGX_RTC_ERR_TOO_SMALL);

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_h264_find_nalu(NULL, 4u, &off),
                               NGX_RTC_ERR_INVALID);
}

NGX_RTC_TEST(rtp_fu_a_scratch_too_small_does_not_emit)
{
    uint8_t nalu[100];
    uint8_t scratch[8];
    uint16_t seq = 5u;
    int n = 0;

    (void)memset(nalu, 0, sizeof(nalu));
    nalu[0] = 0x65u;

    NGX_RTC_TEST_ASSERT_I64_EQ(
        ngx_rtc_h264_packetize_fu_a(nalu, sizeof(nalu), 90000u, &seq,
                                    0x10u, 102u, 1200u, 1,
                                    scratch, sizeof(scratch), mal_emit, &n),
        NGX_RTC_ERR_TOO_SMALL);
    NGX_RTC_TEST_ASSERT_I64_EQ(n, 0);
    NGX_RTC_TEST_ASSERT_U64_EQ(seq, 5u); /* no fragment was emitted */
}

/* ------------------------------------------------------------------ */
/* RTCP decode guards.                                                 */
/* ------------------------------------------------------------------ */

NGX_RTC_TEST(rtcp_parse_null_guards)
{
    uint8_t buf[16];
    ngx_rtc_rtcp_pkt_t pkt;
    uint32_t consumed = 0;

    (void)memset(buf, 0, sizeof(buf));

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtcp_parse(NULL, 8u, &pkt, &consumed),
                               NGX_RTC_ERR_INVALID);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtcp_parse(buf, 8u, NULL, &consumed),
                               NGX_RTC_ERR_INVALID);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtcp_parse(buf, 8u, &pkt, NULL),
                               NGX_RTC_ERR_INVALID);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtcp_decode(NULL, 8u, mal_rtcp_cb, NULL),
                               NGX_RTC_ERR_INVALID);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtcp_decode(buf, 8u, NULL, NULL),
                               NGX_RTC_ERR_INVALID);
}

NGX_RTC_TEST(rtcp_sr_short_payload)
{
    uint8_t raw[28];
    ngx_rtc_rtcp_pkt_t pkt;
    uint32_t consumed = 0;

    /* Declared total (8 bytes) shorter than the SR sender-info minimum. */
    (void)memset(raw, 0, sizeof(raw));
    raw[0] = 0x80u;
    raw[1] = NGX_RTC_RTCP_SR;
    mal_wr_u16(raw + 2, 1u);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtcp_parse(raw, 8u, &pkt, &consumed),
                               NGX_RTC_ERR_PARSE);

    /* RC=1 but the buffer only has the 28-byte sender info, no report block. */
    (void)memset(raw, 0, sizeof(raw));
    raw[0] = 0x81u;
    raw[1] = NGX_RTC_RTCP_SR;
    mal_wr_u16(raw + 2, 6u);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtcp_parse(raw, 28u, &pkt, &consumed),
                               NGX_RTC_ERR_PARSE);
}

NGX_RTC_TEST(rtcp_rr_and_fb_short_payload)
{
    uint8_t raw[28];
    ngx_rtc_rtcp_pkt_t pkt;
    uint32_t consumed = 0;

    /* RR RC=1 needs 8-byte header + 24-byte report block. */
    (void)memset(raw, 0, sizeof(raw));
    raw[0] = 0x81u;
    raw[1] = NGX_RTC_RTCP_RR;
    mal_wr_u16(raw + 2, 6u);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtcp_parse(raw, 28u, &pkt, &consumed),
                               NGX_RTC_ERR_PARSE);

    /* Generic NACK needs at least 12 bytes. */
    (void)memset(raw, 0, sizeof(raw));
    raw[0] = 0x81u;
    raw[1] = NGX_RTC_RTCP_RTPFB;
    mal_wr_u16(raw + 2, 1u);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtcp_parse(raw, 8u, &pkt, &consumed),
                               NGX_RTC_ERR_PARSE);
}

NGX_RTC_TEST(rtcp_decode_truncated_second_packet)
{
    ngx_rtc_rtcp_sr_t sr;
    uint8_t comp[64];
    uint32_t len = 0;

    (void)memset(&sr, 0, sizeof(sr));
    sr.ssrc = 7u;
    sr.ntp = 0x0102030405060708ULL;
    sr.rb = NULL;
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtcp_encode_sr(&sr, comp, sizeof(comp),
                                                      &len),
                               NGX_RTC_OK);

    /* Second sub-packet declares far more bytes than the buffer holds. */
    comp[len + 0] = 0x81u;
    comp[len + 1] = NGX_RTC_RTCP_RR;
    mal_wr_u16(comp + len + 2, 100u);

    mal_cb_count = 0;
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtcp_decode(comp, len + 4u,
                                                   mal_rtcp_cb, NULL),
                               NGX_RTC_ERR_NEED_MORE);
    NGX_RTC_TEST_ASSERT_I64_EQ(mal_cb_count, 1); /* only the SR was delivered */
}

NGX_RTC_TEST(rtcp_sdes_truncated_item_no_crash)
{
    uint8_t raw[12];
    ngx_rtc_rtcp_pkt_t pkt;
    uint32_t consumed = 0;

    /* SDES whose single CNAME item declares 10 bytes but carries only 2. */
    (void)memset(raw, 0, sizeof(raw));
    raw[0] = 0x81u;
    raw[1] = NGX_RTC_RTCP_SDES;
    mal_wr_u16(raw + 2, 2u);
    raw[8] = 1u;   /* CNAME */
    raw[9] = 10u;  /* declared item length */
    raw[10] = 'x';
    raw[11] = 'y';

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtcp_parse(raw, sizeof(raw), &pkt, &consumed),
                               NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_I64_EQ(pkt.type, NGX_RTC_RTCP_SDES);
    NGX_RTC_TEST_ASSERT_I64_EQ(pkt.cname[0], 0); /* no partial CNAME written */
}

/* ------------------------------------------------------------------ */
/* STUN decode guards.                                                 */
/* ------------------------------------------------------------------ */

NGX_RTC_TEST(stun_decode_malformed_no_crash)
{
    uint8_t req[256];
    ngx_rtc_stun_t stun;

    (void)memset(req, 0, sizeof(req));

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_stun_decode(NULL, req, sizeof(req)), -1);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_stun_decode(&stun, NULL, sizeof(req)), -1);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_stun_decode(&stun, req, 0u), -1);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_stun_decode(&stun, req, 19u), -1);

    /* Unknown attribute whose declared length runs past the buffer. */
    mal_stun_header(req, 4u);
    mal_wr_u16(req + 20, 0x8022u);
    mal_wr_u16(req + 22, 0xFFFFu);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_stun_decode(&stun, req, 24u), -1);

    /* USERNAME without a ':' is valid but yields no ufrags. */
    (void)memset(req, 0, sizeof(req));
    mal_stun_header(req, 8u);
    mal_wr_u16(req + 20, NGX_RTC_STUN_ATTR_USERNAME);
    mal_wr_u16(req + 22, 3u);
    req[24] = 'a'; req[25] = 'b'; req[26] = 'c'; req[27] = 0u; /* pad */
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_stun_decode(&stun, req, 28u), 0);
    NGX_RTC_TEST_ASSERT_I64_EQ(stun.has_username, 0);

    /* Attribute value length does not include its 4-byte padding: the decoder
     * must still terminate cleanly (no read past the buffer). */
    (void)memset(req, 0, sizeof(req));
    mal_stun_header(req, 7u);
    mal_wr_u16(req + 20, NGX_RTC_STUN_ATTR_USERNAME);
    mal_wr_u16(req + 22, 3u);
    req[24] = 'a'; req[25] = ':'; req[26] = 'b';
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_stun_decode(&stun, req, 27u), 0);
    NGX_RTC_TEST_ASSERT_I64_EQ(stun.has_username, 1);
    NGX_RTC_TEST_ASSERT_STR_EQ(stun.local_ufrag, "a");
    NGX_RTC_TEST_ASSERT_STR_EQ(stun.remote_ufrag, "b");
}
