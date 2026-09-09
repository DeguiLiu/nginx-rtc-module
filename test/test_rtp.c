/*
 * test_rtp.c - host unit tests for ngx_rtc_rtp.c (RFC 6184 packetization,
 * H264 B-frame detection, Opus/H264 timestamp conversion).
 */

#include "ngx_rtc_test.h"
#include "ngx_rtc_rtp.h"

/* ------------------------------------------------------------------ */
/* Packet collector for the emit callback.                             */
/* ------------------------------------------------------------------ */

#define RTP_COLLECT_MAX 64
#define RTP_COLLECT_PKT 1500

static uint8_t  g_coll_buf[RTP_COLLECT_MAX][RTP_COLLECT_PKT];
static uint32_t g_coll_len[RTP_COLLECT_MAX];
static uint8_t  g_coll_marker[RTP_COLLECT_MAX];

static int32_t collect_emit(void *opaque, const uint8_t *rtp, uint32_t len)
{
    int *count = (int *)opaque;

    if ((*count >= RTP_COLLECT_MAX) || (len > RTP_COLLECT_PKT))
    {
        return NGX_RTC_ERR_TOO_LARGE;
    }

    (void)memcpy(g_coll_buf[*count], rtp, len);
    g_coll_len[*count] = len;
    g_coll_marker[*count] = (uint8_t)(rtp[1] >> 7);
    (*count)++;

    return NGX_RTC_OK;
}

static uint16_t rtp_seq(const uint8_t *pkt)
{
    return (uint16_t)(((uint16_t)pkt[2] << 8) | (uint16_t)pkt[3]);
}

static uint32_t rtp_timestamp(const uint8_t *pkt)
{
    return ((uint32_t)pkt[4] << 24) | ((uint32_t)pkt[5] << 16) |
           ((uint32_t)pkt[6] << 8) | (uint32_t)pkt[7];
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

NGX_RTC_TEST(rtp_header_write_endianness)
{
    ngx_rtc_rtp_header_t hdr;
    uint8_t buf[12];

    hdr.version_cc = NGX_RTC_RTP_VERSION_CC;
    hdr.marker_pt = 0x80u | 102u;
    hdr.seq = 0x1234u;
    hdr.timestamp = 0x01020304u;
    hdr.ssrc = 0xA0B0C0D0u;

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtp_header_write(&hdr, buf, sizeof(buf)),
                               NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_I64_EQ(buf[0], 0x80);
    NGX_RTC_TEST_ASSERT_I64_EQ(buf[1], 0xE6);
    NGX_RTC_TEST_ASSERT_I64_EQ(buf[2], 0x12);
    NGX_RTC_TEST_ASSERT_I64_EQ(buf[3], 0x34);
    NGX_RTC_TEST_ASSERT_I64_EQ(buf[4], 0x01);
    NGX_RTC_TEST_ASSERT_I64_EQ(buf[7], 0x04);
    NGX_RTC_TEST_ASSERT_I64_EQ(buf[8], 0xA0);
    NGX_RTC_TEST_ASSERT_I64_EQ(buf[11], 0xD0);

    /* Buffer too small. */
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_rtp_header_write(&hdr, buf, 11u),
                               NGX_RTC_ERR_TOO_SMALL);
}

NGX_RTC_TEST(rtp_nalu_type_and_find)
{
    uint8_t idr[4] = { 0x65u, 0x00, 0x00, 0x00 };
    uint8_t stream3[] = { 0x00, 0x00, 0x01, 0x41, 0x9A };
    uint8_t stream4[] = { 0x00, 0x00, 0x00, 0x01, 0x41, 0x9A };
    uint32_t off = 0;

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_h264_nalu_type(idr, sizeof(idr)),
                               NGX_RTC_NALU_IDR);
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_h264_nalu_type(NULL, 0),
                               NGX_RTC_NALU_RESERVED);

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_h264_find_nalu(stream3, sizeof(stream3), &off),
                               NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_I64_EQ(off, 3);

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_h264_find_nalu(stream4, sizeof(stream4), &off),
                               NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_I64_EQ(off, 4);

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_h264_find_nalu(idr, sizeof(idr), &off),
                               NGX_RTC_ERR_PARSE);
}

/*
 * Build a 2-byte NALU whose RBSP starts with two ue(v) codes:
 * first_mb_in_slice, then slice_type. The helper packs MSB-first.
 */
static void make_slice(uint8_t *out, uint32_t first_mb, uint32_t slice_type)
{
    uint8_t bits[64];
    uint32_t nbits = 0;
    uint32_t code_num;
    uint32_t m;
    uint32_t i;

    out[0] = 0x41u; /* type 1 (non-IDR slice), NRI=1 */

    /* ue(v) for first_mb. */
    code_num = first_mb + 1u;
    m = 0;
    while ((1u << (m + 1u)) <= code_num)
    {
        m++;
    }
    for (i = 0; i < m; i++)
    {
        bits[nbits++] = 0;
    }
    bits[nbits++] = 1;
    for (i = 0; i < m; i++)
    {
        bits[nbits++] = (uint8_t)((code_num >> (m - 1u - i)) & 1u);
    }

    /* ue(v) for slice_type. */
    code_num = slice_type + 1u;
    m = 0;
    while ((1u << (m + 1u)) <= code_num)
    {
        m++;
    }
    for (i = 0; i < m; i++)
    {
        bits[nbits++] = 0;
    }
    bits[nbits++] = 1;
    for (i = 0; i < m; i++)
    {
        bits[nbits++] = (uint8_t)((code_num >> (m - 1u - i)) & 1u);
    }

    /* Pack into byte 1 (byte 0 is the NALU header). */
    out[1] = 0;
    for (i = 0; i < nbits && i < 8u; i++)
    {
        out[1] = (uint8_t)(out[1] | (bits[i] << (7u - i)));
    }
}

NGX_RTC_TEST(rtp_b_frame_b_and_b1_slices)
{
    uint8_t b_frame[2];
    uint8_t b1_frame[2];

    make_slice(b_frame, 0u, 1u);   /* slice_type B */
    make_slice(b1_frame, 0u, 6u);  /* slice_type B1 */

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_h264_is_b_frame(b_frame, 2u), 1);
    /* KNOWN SRC DEFECT: ngx_rtc_h264_read_ue() accumulates the Exp-Golomb
     * suffix bits with bitwise OR instead of ADD, so slice_type 6 (B1) decodes
     * as 3 and is not recognized as a B frame. This assertion is the
     * regression tripwire and will FAIL until src is fixed. */
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_h264_is_b_frame(b1_frame, 2u), 1);
}

NGX_RTC_TEST(rtp_b_frame_non_b_slices)
{
    uint8_t p_frame[2];
    uint8_t i_frame[2];
    uint8_t sps[3] = { 0x67u, 0x42, 0x00 };

    make_slice(p_frame, 0u, 0u);   /* slice_type P */
    make_slice(i_frame, 0u, 2u);   /* slice_type I */

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_h264_is_b_frame(p_frame, 2u), 0);
    /* Same KNOWN SRC DEFECT as above: slice_type 2 (I) decodes as 1, so an I
     * slice is misreported as a B frame until src is fixed. */
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_h264_is_b_frame(i_frame, 2u), 0);

    /* Non-slice NALU (SPS) is never a B frame. */
    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_h264_is_b_frame(sps, sizeof(sps)), 0);

    /* Invalid input. */
    NGX_RTC_TEST_ASSERT(ngx_rtc_h264_is_b_frame(NULL, 0) < 0);
}

NGX_RTC_TEST(rtp_b_frame_nonzero_first_mb)
{
    uint8_t nalu[2];
    uint8_t bits[16];
    uint32_t nbits = 0;
    uint32_t i;

    /* first_mb_in_slice = 1 (ue code "010"), slice_type = 1 (ue code "010"). */
    nalu[0] = 0x41u;
    bits[nbits++] = 0; /* first_mb leading zero */
    bits[nbits++] = 1;
    bits[nbits++] = 0; /* first_mb bit0 */
    bits[nbits++] = 0; /* slice_type leading zero */
    bits[nbits++] = 1;
    bits[nbits++] = 0; /* slice_type bit0 */

    nalu[1] = 0;
    for (i = 0; i < nbits; i++)
    {
        nalu[1] = (uint8_t)(nalu[1] | (bits[i] << (7u - i)));
    }

    NGX_RTC_TEST_ASSERT_I64_EQ(ngx_rtc_h264_is_b_frame(nalu, 2u), 1);
}

NGX_RTC_TEST(rtp_opus_timestamp_monotonic_20ms)
{
    uint32_t prev = 0;
    uint32_t i;

    for (i = 1; i <= 100; i++)
    {
        uint32_t ts = ngx_rtc_opus_timestamp_from_ms(i * 20u);

        NGX_RTC_TEST_ASSERT_U64_EQ((uint32_t)(ts - prev), 960u);
        prev = ts;
    }

    /* 1 ms steps stay monotonic (integer math, no backward jumps). */
    for (i = 1; i <= 1000; i++)
    {
        uint32_t a = ngx_rtc_opus_timestamp_from_ms(i - 1u);
        uint32_t b = ngx_rtc_opus_timestamp_from_ms(i);

        NGX_RTC_TEST_ASSERT(b >= a);
    }
}

NGX_RTC_TEST(rtp_h264_timestamp_conversion)
{
    NGX_RTC_TEST_ASSERT_U64_EQ(ngx_rtc_h264_timestamp_from_ms(0u), 0u);
    NGX_RTC_TEST_ASSERT_U64_EQ(ngx_rtc_h264_timestamp_from_ms(1000u), 90000u);
    NGX_RTC_TEST_ASSERT_U64_EQ(ngx_rtc_h264_timestamp_from_ms(33u), 2970u);
}

NGX_RTC_TEST(rtp_fu_a_bit_layout)
{
    uint8_t nalu[3000];
    uint8_t scratch[RTP_COLLECT_PKT];
    uint16_t seq = 100u;
    uint32_t i;
    int n = 0;
    int32_t r;
    uint32_t offset = 0;

    nalu[0] = 0x65u; /* NRI=3, type=5 (IDR) */
    for (i = 1; i < sizeof(nalu); i++)
    {
        nalu[i] = (uint8_t)i;
    }

    r = ngx_rtc_h264_packetize_fu_a(nalu, sizeof(nalu), 90000u, &seq,
                                    0x11223344u, 102u, 1200u, 1,
                                    scratch, sizeof(scratch), collect_emit, &n);
    NGX_RTC_TEST_ASSERT_I64_EQ(r, NGX_RTC_OK);

    /* 1 + (len-1-1)/mtu = 1 + 2998/1200 = 3 fragments. */
    NGX_RTC_TEST_ASSERT_I64_EQ(n, 3);
    NGX_RTC_TEST_ASSERT_U64_EQ(seq, 103u);

    /* Fragment 0: indicator keeps NRI, sets FU-A; header sets START. */
    NGX_RTC_TEST_ASSERT_I64_EQ(g_coll_buf[0][12], 0x7Cu);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_coll_buf[0][13], 0x85u);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_coll_marker[0], 0);
    NGX_RTC_TEST_ASSERT_I64_EQ(rtp_seq(g_coll_buf[0]), 100);

    /* Fragment 1: plain type, no S/E. */
    NGX_RTC_TEST_ASSERT_I64_EQ(g_coll_buf[1][12], 0x7Cu);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_coll_buf[1][13], 0x05u);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_coll_marker[1], 0);
    NGX_RTC_TEST_ASSERT_I64_EQ(rtp_seq(g_coll_buf[1]), 101);

    /* Fragment 2: END set, RTP marker set. */
    NGX_RTC_TEST_ASSERT_I64_EQ(g_coll_buf[2][12], 0x7Cu);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_coll_buf[2][13], 0x45u);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_coll_marker[2], 1);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_coll_buf[2][1], 0xE6u); /* M=1 + PT 102 */
    NGX_RTC_TEST_ASSERT_I64_EQ(rtp_seq(g_coll_buf[2]), 102);

    /* Every fragment shares timestamp and SSRC. */
    NGX_RTC_TEST_ASSERT_U64_EQ(rtp_timestamp(g_coll_buf[0]), 90000u);
    NGX_RTC_TEST_ASSERT_U64_EQ(rtp_timestamp(g_coll_buf[2]), 90000u);

    /* Reassemble FU payloads and compare against nalu[1..]. */
    offset = 0;
    for (i = 0; i < 3u; i++)
    {
        uint32_t payload = g_coll_len[i] - 12u - 2u;

        NGX_RTC_TEST_ASSERT_MEM_EQ(g_coll_buf[i] + 14u, nalu + 1u + offset, payload);
        offset += payload;
    }
    NGX_RTC_TEST_ASSERT_U64_EQ(offset, sizeof(nalu) - 1u);
}

NGX_RTC_TEST(rtp_stap_a_bit_layout)
{
    uint8_t sps[5] = { 0x67u, 0x42, 0x00, 0x0A, 0x0B };
    uint8_t pps[3] = { 0x68u, 0xCE, 0x3C };
    const uint8_t *nalus[2] = { sps, pps };
    uint32_t sizes[2] = { 5u, 3u };
    uint8_t scratch[RTP_COLLECT_PKT];
    uint16_t seq = 200u;
    int n = 0;
    int32_t r;
    uint8_t *pkt;

    r = ngx_rtc_h264_packetize_stap_a(nalus, sizes, 2u, 90000u, &seq,
                                      0x0A0B0C0Du, 102u,
                                      scratch, sizeof(scratch), collect_emit, &n);
    NGX_RTC_TEST_ASSERT_I64_EQ(r, NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_I64_EQ(n, 1);
    NGX_RTC_TEST_ASSERT_U64_EQ(seq, 201u);

    pkt = g_coll_buf[0];

    /* 12 RTP + 1 STAP header + (2+5) + (2+3) = 25 bytes. */
    NGX_RTC_TEST_ASSERT_I64_EQ(g_coll_len[0], 25);

    /* STAP-A header keeps first NALU NRI, sets type 24. */
    NGX_RTC_TEST_ASSERT_I64_EQ(pkt[12], 0x78u);

    /* First NALU: 2-byte BE length 5, then SPS bytes. */
    NGX_RTC_TEST_ASSERT_I64_EQ(pkt[13], 0x00);
    NGX_RTC_TEST_ASSERT_I64_EQ(pkt[14], 0x05);
    NGX_RTC_TEST_ASSERT_MEM_EQ(pkt + 15, sps, 5u);

    /* Second NALU: 2-byte BE length 3, then PPS bytes. */
    NGX_RTC_TEST_ASSERT_I64_EQ(pkt[20], 0x00);
    NGX_RTC_TEST_ASSERT_I64_EQ(pkt[21], 0x03);
    NGX_RTC_TEST_ASSERT_MEM_EQ(pkt + 22, pps, 3u);

    /* STAP-A always clears the marker bit. */
    NGX_RTC_TEST_ASSERT_I64_EQ(g_coll_marker[0], 0);
    NGX_RTC_TEST_ASSERT_I64_EQ(pkt[1], 102u);
}

NGX_RTC_TEST(rtp_stap_a_too_large)
{
    uint8_t big[1200];
    uint8_t small[2] = { 0x68u, 0x00 };
    const uint8_t *nalus[2] = { big, small };
    uint32_t sizes[2] = { 1200u, 1u };
    uint8_t scratch[RTP_COLLECT_PKT];
    uint16_t seq = 1u;
    int n = 0;

    (void)memset(big, 0, sizeof(big));
    big[0] = 0x67u;

    /* Aggregate payload exceeds the 1200-byte STAP-A MTU. */
    NGX_RTC_TEST_ASSERT_I64_EQ(
        ngx_rtc_h264_packetize_stap_a(nalus, sizes, 2u, 0u, &seq, 0u, 102u,
                                      scratch, sizeof(scratch), collect_emit, &n),
        NGX_RTC_ERR_TOO_LARGE);
    NGX_RTC_TEST_ASSERT_I64_EQ(n, 0);

    /* Empty aggregate is invalid. */
    NGX_RTC_TEST_ASSERT_I64_EQ(
        ngx_rtc_h264_packetize_stap_a(nalus, sizes, 0u, 0u, &seq, 0u, 102u,
                                      scratch, sizeof(scratch), collect_emit, &n),
        NGX_RTC_ERR_INVALID);
}

NGX_RTC_TEST(rtp_single_and_opus_packetize)
{
    uint8_t nalu[100];
    uint8_t opus[40];
    uint8_t scratch[RTP_COLLECT_PKT];
    uint16_t seq = 5u;
    int n = 0;

    (void)memset(nalu, 0x11, sizeof(nalu));
    nalu[0] = 0x65u;
    (void)memset(opus, 0x22, sizeof(opus));

    NGX_RTC_TEST_ASSERT_I64_EQ(
        ngx_rtc_h264_packetize_single(nalu, sizeof(nalu), 90000u, &seq,
                                      0x100u, 102u, 1,
                                      scratch, sizeof(scratch), collect_emit, &n),
        NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_I64_EQ(n, 1);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_coll_len[0], 112);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_coll_marker[0], 1);
    NGX_RTC_TEST_ASSERT_MEM_EQ(g_coll_buf[0] + 12, nalu, sizeof(nalu));
    NGX_RTC_TEST_ASSERT_U64_EQ(seq, 6u);

    n = 0;
    NGX_RTC_TEST_ASSERT_I64_EQ(
        ngx_rtc_opus_packetize(opus, sizeof(opus), 48000u, &seq,
                               0x200u, 111u, 1,
                               scratch, sizeof(scratch), collect_emit, &n),
        NGX_RTC_OK);
    NGX_RTC_TEST_ASSERT_I64_EQ(n, 1);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_coll_len[0], 52);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_coll_marker[0], 1);
    NGX_RTC_TEST_ASSERT_I64_EQ(g_coll_buf[0][1], 0xEFu); /* M=1 + PT 111 */
    NGX_RTC_TEST_ASSERT_MEM_EQ(g_coll_buf[0] + 12, opus, sizeof(opus));
}
