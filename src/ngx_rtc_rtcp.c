/**
 * @file    ngx_rtc_rtcp.c
 * @brief   RTCP codec core (RFC 3550 / RFC 4585).
 * @version 0.5.0
 * @license MIT, see LICENSE
 *
 * Pure C11 translation of SRS 6.0:
 *   src/kernel/srs_kernel_rtc_rtcp.cpp
 *     SrsRtcpCommon::decode_header / SrsRtcpSR / SrsRtcpRR / SrsRtcpNack /
 *     SrsRtcpPli / SrsRtcpCompound
 *
 * The wire formats follow:
 *   RFC 3550 section 6.1  - RTCP common header
 *   RFC 3550 section 6.4  - SR/RR
 *   RFC 3550 section 6.5  - SDES (CNAME item)
 *   RFC 4585 section 6.1  - transport/payload-specific feedback
 *   RFC 4585 section 6.2.1 - generic NACK
 *   RFC 4585 section 6.3.1 - PLI
 */

#include "ngx_rtc_rtcp.h"

#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* Big-endian wire helpers.                                            */
/* ------------------------------------------------------------------ */

static uint16_t ngx_rtc_rtcp_rd_u16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint32_t ngx_rtc_rtcp_rd_u24(const uint8_t *p)
{
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}

static uint32_t ngx_rtc_rtcp_rd_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t ngx_rtc_rtcp_rd_u64(const uint8_t *p)
{
    return ((uint64_t)ngx_rtc_rtcp_rd_u32(p) << 32) |
           (uint64_t)ngx_rtc_rtcp_rd_u32(p + 4);
}

static void ngx_rtc_rtcp_wr_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)((v >> 8) & 0xffu);
    p[1] = (uint8_t)(v & 0xffu);
}

static void ngx_rtc_rtcp_wr_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)((v >> 24) & 0xffu);
    p[1] = (uint8_t)((v >> 16) & 0xffu);
    p[2] = (uint8_t)((v >> 8) & 0xffu);
    p[3] = (uint8_t)(v & 0xffu);
}

static void ngx_rtc_rtcp_wr_u64(uint8_t *p, uint64_t v)
{
    ngx_rtc_rtcp_wr_u32(p, (uint32_t)(v >> 32));
    ngx_rtc_rtcp_wr_u32(p + 4, (uint32_t)(v & 0xffffffffu));
}

/*
 * Write a 4-byte RTCP header. total_len is the full packet length in bytes
 * (must be a multiple of 4); the header length field stores words minus one.
 */
static void ngx_rtc_rtcp_wr_header(uint8_t *p, uint8_t type, uint8_t fmt, uint32_t total_len)
{
    uint16_t length = (uint16_t)((total_len / 4u) - 1u);

    p[0] = (uint8_t)(0x80u | (fmt & 0x1fu)); /* V=2, P=0, RC/FMT/SC */
    p[1] = type;
    ngx_rtc_rtcp_wr_u16(p + 2, length);
}

/* ------------------------------------------------------------------ */
/* Report block.                                                       */
/* ------------------------------------------------------------------ */

static void ngx_rtc_rtcp_read_rb(const uint8_t *p, ngx_rtc_rtcp_rb_t *rb)
{
    rb->ssrc = ngx_rtc_rtcp_rd_u32(p);
    rb->fraction_lost = p[4];
    rb->lost_packets = ngx_rtc_rtcp_rd_u24(p + 5);
    rb->highest_seq = ngx_rtc_rtcp_rd_u32(p + 8);
    rb->jitter = ngx_rtc_rtcp_rd_u32(p + 12);
    rb->lsr = ngx_rtc_rtcp_rd_u32(p + 16);
    rb->dlsr = ngx_rtc_rtcp_rd_u32(p + 20);
}

static void ngx_rtc_rtcp_write_rb(uint8_t *p, const ngx_rtc_rtcp_rb_t *rb)
{
    ngx_rtc_rtcp_wr_u32(p, rb->ssrc);
    p[4] = rb->fraction_lost;
    p[5] = (uint8_t)((rb->lost_packets >> 16) & 0xffu);
    p[6] = (uint8_t)((rb->lost_packets >> 8) & 0xffu);
    p[7] = (uint8_t)(rb->lost_packets & 0xffu);
    ngx_rtc_rtcp_wr_u32(p + 8, rb->highest_seq);
    ngx_rtc_rtcp_wr_u32(p + 12, rb->jitter);
    ngx_rtc_rtcp_wr_u32(p + 16, rb->lsr);
    ngx_rtc_rtcp_wr_u32(p + 20, rb->dlsr);
}

/* ------------------------------------------------------------------ */
/* NTP conversion (time.h).                                            */
/* ------------------------------------------------------------------ */

uint64_t ngx_rtc_rtcp_ntp_from_unix_us(uint64_t unix_us)
{
    uint64_t seconds = unix_us / 1000000u;
    uint64_t usec = unix_us % 1000000u;
    uint64_t fraction = (usec << 32) / 1000000u;

    return ((seconds + NGX_RTC_RTCP_NTP_UNIX_OFFSET) << 32) | fraction;
}

uint64_t ngx_rtc_rtcp_ntp_now(void)
{
#if defined(CLOCK_REALTIME)
    struct timespec ts;
    if (0 == clock_gettime(CLOCK_REALTIME, &ts))
    {
        uint64_t unix_us = (uint64_t)ts.tv_sec * 1000000u + (uint64_t)(ts.tv_nsec / 1000);
        return ngx_rtc_rtcp_ntp_from_unix_us(unix_us);
    }
#endif

    /* Fallback for platforms without clock_gettime: second resolution. */
    return ngx_rtc_rtcp_ntp_from_unix_us((uint64_t)time(NULL) * 1000000u);
}

/* ------------------------------------------------------------------ */
/* Decode.                                                             */
/* ------------------------------------------------------------------ */

/* Parse SDES chunks; store the first CNAME item found (RFC 3550 6.5). */
static void ngx_rtc_rtcp_parse_sdes(const uint8_t *buf, uint32_t total, ngx_rtc_rtcp_pkt_t *pkt)
{
    uint32_t off = 4u; /* skip the 4-byte common header, point at chunk 0 SSRC */
    uint8_t sc = pkt->fmt;
    uint8_t chunk;

    for (chunk = 0; (chunk < sc) && ((off + 4u) <= total); chunk++)
    {
        off += 4u; /* SSRC/CSRC of this chunk, already stored in pkt->ssrc */

        /* Walk the SDES items until the terminating zero octet. */
        while ((off + 1u) <= total)
        {
            uint8_t item_type = buf[off];
            uint8_t item_len;
            uint32_t copy;

            if (0u == item_type)
            {
                off += 1u; /* consume the terminator */
                break;
            }
            if ((off + 2u) > total)
            {
                return; /* truncated item header */
            }

            item_len = buf[off + 1];
            if ((uint32_t)(off + 2u + item_len) > total)
            {
                return; /* truncated item text */
            }

            if (1u == item_type) /* CNAME */
            {
                copy = item_len;
                if (copy >= NGX_RTC_RTCP_MAX_CNAME)
                {
                    copy = NGX_RTC_RTCP_MAX_CNAME - 1u;
                }
                memcpy(pkt->cname, buf + off + 2, copy);
                pkt->cname[copy] = '\0';
                return;
            }

            off += 2u + item_len;
        }

        /* Each chunk is padded with null octets to a 32-bit boundary. */
        while (0u != (off % 4u))
        {
            off++;
        }
    }
}

/* Summarise a transport-wide CC feedback: count lost and received packets from
 * the run-length / status-vector chunk list. The wire format is
 * draft-holmer-rmcat-transport-wide-cc-extensions-01 (the one negotiated as the
 * "transport-cc" RTP header extension), NOT RFC 8888, which defines a different
 * feedback layout that has no chunks at all. Recv deltas are not needed for the
 * loss summary, so they are skipped by not consuming them. */
static void ngx_rtc_rtcp_parse_twcc(const uint8_t *buf, uint32_t total,
                                    ngx_rtc_rtcp_pkt_t *pkt)
{
    const uint8_t *p;
    const uint8_t *end;
    uint32_t      count;
    uint32_t      parsed;
    uint32_t      run;
    uint32_t      symbol;
    uint16_t      chunk;

    if (total < 20u) {
        return; /* 12-byte RTCP header + 8-byte fixed FCI */
    }

    p = buf + 12;
    pkt->twcc_base_seq = ngx_rtc_rtcp_rd_u16(p);
    pkt->twcc_pkt_count = ngx_rtc_rtcp_rd_u16(p + 2);
    p += 8; /* reference time (3 bytes) + feedback packet count (1 byte) */

    count = pkt->twcc_pkt_count;
    parsed = 0;
    end = buf + total;

    while (parsed < count && p + 2 <= end) {
        chunk = ngx_rtc_rtcp_rd_u16(p);
        p += 2;

        if (0 == (chunk >> 15)) {
            /* Run length chunk (draft-holmer-rmcat-transport-wide-cc-extensions
             * -01 section 3.1.3): T is one bit, the repeated packet status
             * symbol is TWO bits and the run length is 13. The status vector
             * chunk below does use a single size bit, so reading a single
             * status bit here is an easy mistake -- and a costly one: a normal
             * "received, small delta" run has bit 14 clear, so it reads as a
             * lost run, and its length picks up the status bit and becomes
             * 8192 + n, which the count clamp below turns into "everything
             * left in this feedback was lost". Almost every arrival takes that
             * path, so a clean link reports near-total loss. */
            run = chunk & 0x1FFFu;
            symbol = (chunk >> 13) & 0x3u;

            if (run > count - parsed) {
                run = count - parsed;
            }
            if (0 == symbol) {
                pkt->twcc_lost += run;
            } else {
                pkt->twcc_received += run;
            }
            parsed += run;
        } else {
            /* Status vector chunk (draft-holmer-rmcat-transport-wide-cc-
             * extensions-01 section 3.1.4): T is one bit, S is one bit, and the
             * symbol list fills the remaining 14 bits. S=0 packs fourteen 1-bit
             * symbols, S=1 packs seven 2-bit ones. Only the 2-bit form was read
             * here, so an S=0 chunk advanced the cursor by 7 symbols instead of
             * 14, walked the rest of the feedback at the wrong stride, drifted
             * into the recv-delta section and counted arrival-time bytes as
             * packet status.
             *
             * A symbol of zero means "not received" in BOTH forms. Section
             * 3.1.4's prose says the 1-bit form is "'packet received' (0) and
             * 'packet not received' (1)", but its own Example 1 draws the
             * symbols `0 1 1 1 1 1 0 0 0 1 1 1 0 0` and labels the leading 0
             * "packet not received", which agrees with the 2-bit semantics in
             * 3.1.1. The example is right and the prose is not. */
            if (0 == (chunk & 0x4000u)) {
                for (symbol = 0; symbol < 14u && parsed < count; symbol++) {
                    if (0 == ((chunk >> (13u - symbol)) & 0x1u)) {
                        pkt->twcc_lost++;
                    } else {
                        pkt->twcc_received++;
                    }
                    parsed++;
                }
            } else {
                for (symbol = 0; symbol < 7u && parsed < count; symbol++) {
                    if (0 == ((chunk >> (12u - symbol * 2u)) & 0x3u)) {
                        pkt->twcc_lost++;
                    } else {
                        pkt->twcc_received++;
                    }
                    parsed++;
                }
            }
        }
    }
}

/*
 * Decode a REMB PSFB FCI (draft-alvestrand-rmcat-remb). PT=206, FMT=15. The
 * FCI after the 12-byte RTCP+media header is:
 *
 *   "REMB" (4 bytes)                                          unique identifier
 *   (numSSRC<<24)|(brExp<<18)|(brMantissa)                    bitrate word
 *   numSSRC * 4 bytes                                         SSRC feedback list
 *
 * bitrate = brMantissa * 2^brExp, in bits per second. A FMT=15 PSFB whose FCI
 * does not start with "REMB" is an application-layer feedback of another kind
 * and is left untouched (has_remb stays 0).
 */
static int32_t ngx_rtc_rtcp_parse_remb(const uint8_t *buf, uint32_t total,
                                       ngx_rtc_rtcp_pkt_t *pkt)
{
    const uint8_t *p;
    uint32_t fci;
    uint32_t word;
    uint32_t num_ssrc;
    uint32_t br_exp;
    uint32_t br_mantissa;
    uint32_t keep;
    uint32_t i;

    fci = total - 12u;
    if (fci < 4u)
    {
        return NGX_RTC_OK; /* FCI too short to hold the "REMB" magic: not REMB */
    }

    p = buf + 12;
    if ((0x52u != p[0]) || (0x45u != p[1]) || (0x4Du != p[2]) || (0x42u != p[3]))
    {
        return NGX_RTC_OK; /* application-layer feedback, not REMB */
    }

    if (fci < 8u)
    {
        return NGX_RTC_ERR_PARSE; /* "REMB" magic present but the bitrate word is missing */
    }

    word = ngx_rtc_rtcp_rd_u32(p + 4);
    num_ssrc = (word >> 24) & 0xffu;
    br_exp = (word >> 18) & 0x3fu;
    br_mantissa = word & 0x3ffffu;

    if (fci < (8u + num_ssrc * 4u))
    {
        return NGX_RTC_ERR_PARSE; /* truncated SSRC feedback list */
    }

    pkt->has_remb = 1u;
    pkt->remb_bitrate_bps = (uint32_t)((uint64_t)br_mantissa * (1ULL << br_exp));

    keep = num_ssrc;
    if (keep > NGX_RTC_RTCP_MAX_REMB_SSRCS)
    {
        keep = NGX_RTC_RTCP_MAX_REMB_SSRCS;
    }
    pkt->remb_ssrc_count = (uint16_t)keep;
    p += 8;
    for (i = 0; i < keep; i++)
    {
        pkt->remb_ssrcs[i] = ngx_rtc_rtcp_rd_u32(p);
        p += 4;
    }
    if (keep > 0u)
    {
        pkt->remb_ssrc = pkt->remb_ssrcs[0];
    }

    return NGX_RTC_OK;
}

int32_t ngx_rtc_rtcp_parse(const uint8_t *buf, uint32_t len,
                           ngx_rtc_rtcp_pkt_t *pkt, uint32_t *consumed)
{
    uint8_t type;
    uint8_t fmt;
    uint8_t version;
    uint8_t padding;
    uint16_t length_words;
    uint32_t total;
    uint32_t off;
    uint32_t i;

    if ((NULL == buf) || (NULL == pkt) || (NULL == consumed))
    {
        return NGX_RTC_ERR_INVALID;
    }
    if (len < 4u)
    {
        return NGX_RTC_ERR_NEED_MORE;
    }

    version = (uint8_t)((buf[0] >> 6) & 0x3u);
    padding = (uint8_t)((buf[0] >> 5) & 0x1u);
    fmt = (uint8_t)(buf[0] & 0x1fu);
    type = buf[1];
    length_words = ngx_rtc_rtcp_rd_u16(buf + 2);
    total = ((uint32_t)length_words + 1u) * 4u;

    if (total > len)
    {
        return NGX_RTC_ERR_NEED_MORE;
    }
    if (2u != version)
    {
        return NGX_RTC_ERR_PARSE;
    }

    memset(pkt, 0, sizeof(*pkt));
    pkt->version = version;
    pkt->padding = padding;
    pkt->type = type;
    pkt->fmt = fmt;
    pkt->length_words = length_words;
    pkt->total_len = total;
    pkt->ssrc = ngx_rtc_rtcp_rd_u32(buf + 4);
    off = 8u;

    switch (type)
    {
    case NGX_RTC_RTCP_SR:
        if (total < 28u)
        {
            return NGX_RTC_ERR_PARSE;
        }
        pkt->ntp = ngx_rtc_rtcp_rd_u64(buf + 8);
        pkt->rtp_ts = ngx_rtc_rtcp_rd_u32(buf + 16);
        pkt->sender_packet_count = ngx_rtc_rtcp_rd_u32(buf + 20);
        pkt->sender_octet_count = ngx_rtc_rtcp_rd_u32(buf + 24);
        if (fmt > 0u)
        {
            if (total < 52u)
            {
                return NGX_RTC_ERR_PARSE;
            }
            ngx_rtc_rtcp_read_rb(buf + 28, &pkt->rb);
            pkt->has_rb = 1u;
        }
        break;

    case NGX_RTC_RTCP_RR:
        /* An empty RR (RC=0) is valid and has no report block (RFC 3550 6.4.2). */
        if (fmt > 0u)
        {
            if (total < (off + 24u))
            {
                return NGX_RTC_ERR_PARSE;
            }
            ngx_rtc_rtcp_read_rb(buf + off, &pkt->rb);
            pkt->has_rb = 1u;
        }
        break;

    case NGX_RTC_RTCP_SDES:
        if (total < 8u)
        {
            return NGX_RTC_ERR_PARSE;
        }
        ngx_rtc_rtcp_parse_sdes(buf, total, pkt);
        break;

    case NGX_RTC_RTCP_RTPFB:
        if (total < 12u)
        {
            return NGX_RTC_ERR_PARSE;
        }
        pkt->media_ssrc = ngx_rtc_rtcp_rd_u32(buf + 8);
        if (NGX_RTC_RTCP_FMT_NACK == fmt)
        {
            uint32_t fci_bytes = total - 12u;
            uint32_t entries = fci_bytes / 4u;
            uint32_t keep = entries;
            const uint8_t *p = buf + 12;

            if (keep > NGX_RTC_RTCP_MAX_NACK_ENTRIES)
            {
                keep = NGX_RTC_RTCP_MAX_NACK_ENTRIES;
            }
            for (i = 0; i < keep; i++)
            {
                pkt->nack_pid[i] = ngx_rtc_rtcp_rd_u16(p);
                pkt->nack_blp[i] = ngx_rtc_rtcp_rd_u16(p + 2);
                p += 4;
            }
            pkt->nack_count = (uint16_t)keep;
        }
        else if (NGX_RTC_RTCP_FMT_TWCC == fmt)
        {
            ngx_rtc_rtcp_parse_twcc(buf, total, pkt);
        }
        break;

    case NGX_RTC_RTCP_PSFB:
        if (total < 12u)
        {
            return NGX_RTC_ERR_PARSE;
        }
        pkt->media_ssrc = ngx_rtc_rtcp_rd_u32(buf + 8);
        if (NGX_RTC_RTCP_FMT_REMB == fmt)
        {
            int32_t r = ngx_rtc_rtcp_parse_remb(buf, total, pkt);
            if (NGX_RTC_OK != r)
            {
                return r;
            }
        }
        break;

    default:
        /* Unknown packet type: header is already exposed, payload skipped. */
        break;
    }

    *consumed = total;
    return NGX_RTC_OK;
}

int32_t ngx_rtc_rtcp_decode(const uint8_t *buf, uint32_t len,
                            ngx_rtc_rtcp_decode_cb cb, void *opaque)
{
    uint32_t off = 0u;

    if ((NULL == buf) || (NULL == cb))
    {
        return NGX_RTC_ERR_INVALID;
    }

    while (off < len)
    {
        ngx_rtc_rtcp_pkt_t pkt;
        uint32_t consumed = 0u;
        int32_t r = ngx_rtc_rtcp_parse(buf + off, len - off, &pkt, &consumed);

        if (NGX_RTC_OK != r)
        {
            return r;
        }

        r = cb(&pkt, opaque);
        if (NGX_RTC_OK != r)
        {
            return r;
        }

        off += consumed;
    }

    return NGX_RTC_OK;
}

int32_t ngx_rtc_rtcp_nack_expand(const ngx_rtc_rtcp_pkt_t *pkt,
                                 uint16_t *seqs, uint16_t cap, uint16_t *count)
{
    uint16_t i;
    uint16_t out = 0u;

    if ((NULL == pkt) || (NULL == seqs) || (NULL == count))
    {
        return NGX_RTC_ERR_INVALID;
    }

    for (i = 0; i < pkt->nack_count; i++)
    {
        uint16_t pid = pkt->nack_pid[i];
        uint16_t blp = pkt->nack_blp[i];
        uint16_t j;

        if (out >= cap)
        {
            *count = out;
            return NGX_RTC_ERR_TOO_SMALL;
        }
        seqs[out] = pid;
        out++;

        for (j = 0; j < 16; j++)
        {
            if (0u == (blp & (uint16_t)(1u << j)))
            {
                continue;
            }
            if (out >= cap)
            {
                *count = out;
                return NGX_RTC_ERR_TOO_SMALL;
            }
            seqs[out] = (uint16_t)(pid + j + 1u);
            out++;
        }
    }

    *count = out;
    return NGX_RTC_OK;
}

/* ------------------------------------------------------------------ */
/* Encode.                                                             */
/* ------------------------------------------------------------------ */

int32_t ngx_rtc_rtcp_encode_sr(const ngx_rtc_rtcp_sr_t *sr,
                               uint8_t *buf, uint32_t cap, uint32_t *len)
{
    uint8_t rc;
    uint32_t total;
    uint64_t ntp;
    uint8_t *p;

    if ((NULL == sr) || (NULL == buf) || (NULL == len))
    {
        return NGX_RTC_ERR_INVALID;
    }

    rc = (NULL == sr->rb) ? 0u : 1u;
    total = 28u + ((uint32_t)rc * 24u);
    if (cap < total)
    {
        return NGX_RTC_ERR_TOO_SMALL;
    }

    ntp = sr->ntp;
    if (0u == ntp)
    {
        ntp = ngx_rtc_rtcp_ntp_now();
    }

    p = buf;
    ngx_rtc_rtcp_wr_header(p, NGX_RTC_RTCP_SR, rc, total);
    p += 4;
    ngx_rtc_rtcp_wr_u32(p, sr->ssrc);
    p += 4;
    ngx_rtc_rtcp_wr_u64(p, ntp);
    p += 8;
    ngx_rtc_rtcp_wr_u32(p, sr->rtp_ts);
    p += 4;
    ngx_rtc_rtcp_wr_u32(p, sr->packet_count);
    p += 4;
    ngx_rtc_rtcp_wr_u32(p, sr->octet_count);
    p += 4;

    if (NULL != sr->rb)
    {
        ngx_rtc_rtcp_write_rb(p, sr->rb);
    }

    *len = total;
    return NGX_RTC_OK;
}

int32_t ngx_rtc_rtcp_encode_sdes(uint32_t ssrc, const char *cname,
                                 uint8_t *buf, uint32_t cap, uint32_t *len)
{
    uint32_t cname_len;
    uint32_t total;
    uint32_t pad;
    uint32_t i;
    uint8_t *p;

    if ((NULL == cname) || (NULL == buf) || (NULL == len))
    {
        return NGX_RTC_ERR_INVALID;
    }

    cname_len = (uint32_t)strlen(cname);
    if (cname_len > 255u)
    {
        return NGX_RTC_ERR_TOO_LARGE;
    }

    /* header(4) + SSRC(4) + item(1+1+cname) + terminator(1), then 4-byte pad. */
    total = 4u + 4u + 2u + cname_len + 1u;
    pad = (4u - (total % 4u)) % 4u;
    total += pad;
    if (cap < total)
    {
        return NGX_RTC_ERR_TOO_SMALL;
    }

    p = buf;
    ngx_rtc_rtcp_wr_header(p, NGX_RTC_RTCP_SDES, 1u, total); /* SC=1 */
    p += 4;
    ngx_rtc_rtcp_wr_u32(p, ssrc);
    p += 4;
    p[0] = 1u; /* CNAME */
    p[1] = (uint8_t)cname_len;
    p += 2;
    memcpy(p, cname, cname_len);
    p += cname_len;
    p[0] = 0u; /* end of SDES items */
    p += 1;
    for (i = 0; i < pad; i++)
    {
        p[i] = 0u;
    }

    *len = total;
    return NGX_RTC_OK;
}

int32_t ngx_rtc_rtcp_encode_pli(uint32_t sender_ssrc, uint32_t media_ssrc,
                                uint8_t *buf, uint32_t cap, uint32_t *len)
{
    /* PLI carries no FCI: header + sender SSRC + media SSRC, 12 octets. */
    if ((NULL == buf) || (NULL == len))
    {
        return NGX_RTC_ERR_INVALID;
    }

    if (cap < 12u)
    {
        return NGX_RTC_ERR_TOO_SMALL;
    }

    ngx_rtc_rtcp_wr_header(buf, NGX_RTC_RTCP_PSFB, NGX_RTC_RTCP_FMT_PLI, 12u);
    ngx_rtc_rtcp_wr_u32(buf + 4, sender_ssrc);
    ngx_rtc_rtcp_wr_u32(buf + 8, media_ssrc);

    *len = 12u;
    return NGX_RTC_OK;
}

int32_t ngx_rtc_rtcp_compound_append(uint8_t *compound, uint32_t cap, uint32_t *len,
                                     const uint8_t *pkt, uint32_t pkt_len)
{
    if ((NULL == compound) || (NULL == len) || (NULL == pkt))
    {
        return NGX_RTC_ERR_INVALID;
    }
    if ((*len > cap) || (pkt_len > (cap - *len)))
    {
        return NGX_RTC_ERR_TOO_SMALL;
    }

    memcpy(compound + *len, pkt, pkt_len);
    *len += pkt_len;
    return NGX_RTC_OK;
}
