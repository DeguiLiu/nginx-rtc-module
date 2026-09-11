/*
 * ngx_rtc_rtp.c - H264 NALU to RTP packetization (RFC 6184)
 *
 * Pure C11 translation of SRS 6.0:
 *   src/kernel/srs_kernel_rtc_rtp.cpp  (SrsRtpHeader::encode,
 *                                       SrsRtpSTAPPayload::encode,
 *                                       SrsRtpFUAPayload2::encode)
 *   src/app/srs_app_rtc_source.cpp     (SrsRtcRtpBuilder::package_single_nalu,
 *                                       package_fu_a, package_stap_a)
 *   src/kernel/srs_kernel_codec.cpp    (SrsVideoFrame::parse_avc_b_frame)
 *   src/kernel/srs_kernel_utility.cpp  (srs_avc_nalu_read_uev)
 */

#include "ngx_rtc_rtp.h"

#include <stddef.h>
#include <string.h>

/* Build the 12-byte fixed header in place, return bytes written. */
static uint32_t ngx_rtc_rtp_build_header(uint8_t *buf, uint16_t seq, uint32_t timestamp,
                                         uint32_t ssrc, uint8_t payload_type, int32_t marker)
{
    ngx_rtc_rtp_header_t hdr;

    hdr.version_cc = NGX_RTC_RTP_VERSION_CC;
    hdr.marker_pt = (uint8_t)(payload_type & 0x7fu);
    if (0 != marker)
    {
        hdr.marker_pt = (uint8_t)(hdr.marker_pt | NGX_RTC_RTP_MARKER);
    }
    hdr.seq = seq;
    hdr.timestamp = timestamp;
    hdr.ssrc = ssrc;

    (void)ngx_rtc_rtp_header_write(&hdr, buf, NGX_RTC_RTP_HEADER_SIZE);
    return NGX_RTC_RTP_HEADER_SIZE;
}

/*
 * Drop CSRCs and the header extension from an incoming RTP packet in place.
 *
 * The downlink re-stamps its own transport-wide-cc extension per viewer, so a
 * publisher's per-hop extensions must not reach the broadcast path: a browser
 * adds them whenever the answer echoed its extmap, which would both shift the
 * H264 payload offset used by the STAP-A/FU-A checks and corrupt every packet
 * the moment this module inserts its own extension at offset 12.
 */
int32_t ngx_rtc_rtp_strip_header_ext(uint8_t *rtp, uint32_t *len)
{
    uint32_t hdr;
    uint32_t ext_words;

    if ((NULL == rtp) || (NULL == len) || (*len < NGX_RTC_RTP_HEADER_SIZE))
    {
        return NGX_RTC_ERR_PARSE;
    }

    hdr = NGX_RTC_RTP_HEADER_SIZE + (4u * (uint32_t)(rtp[0] & NGX_RTC_RTP_CSRC_MASK));

    if (0 != (rtp[0] & NGX_RTC_RTP_EXTENSION))
    {
        if ((hdr + 4u) > *len)
        {
            return NGX_RTC_ERR_PARSE;
        }
        ext_words = ((uint32_t)rtp[hdr + 2] << 8) | (uint32_t)rtp[hdr + 3];
        hdr += 4u + (4u * ext_words);
    }

    if (hdr > *len)
    {
        return NGX_RTC_ERR_PARSE;
    }

    if (hdr != NGX_RTC_RTP_HEADER_SIZE)
    {
        (void)memmove(rtp + NGX_RTC_RTP_HEADER_SIZE, rtp + hdr, *len - hdr);
        *len = NGX_RTC_RTP_HEADER_SIZE + (*len - hdr);
    }

    rtp[0] = (uint8_t)(rtp[0] & (uint8_t)~(NGX_RTC_RTP_CSRC_MASK | NGX_RTC_RTP_EXTENSION));

    return NGX_RTC_OK;
}

int32_t ngx_rtc_rtp_header_write(const ngx_rtc_rtp_header_t *hdr, uint8_t *buf, uint32_t cap)
{
    if ((NULL == hdr) || (NULL == buf))
    {
        return NGX_RTC_ERR_INVALID;
    }
    if (cap < NGX_RTC_RTP_HEADER_SIZE)
    {
        return NGX_RTC_ERR_TOO_SMALL;
    }

    buf[0] = hdr->version_cc;
    buf[1] = hdr->marker_pt;
    buf[2] = (uint8_t)((hdr->seq >> 8) & 0xffu);
    buf[3] = (uint8_t)(hdr->seq & 0xffu);
    buf[4] = (uint8_t)((hdr->timestamp >> 24) & 0xffu);
    buf[5] = (uint8_t)((hdr->timestamp >> 16) & 0xffu);
    buf[6] = (uint8_t)((hdr->timestamp >> 8) & 0xffu);
    buf[7] = (uint8_t)(hdr->timestamp & 0xffu);
    buf[8] = (uint8_t)((hdr->ssrc >> 24) & 0xffu);
    buf[9] = (uint8_t)((hdr->ssrc >> 16) & 0xffu);
    buf[10] = (uint8_t)((hdr->ssrc >> 8) & 0xffu);
    buf[11] = (uint8_t)(hdr->ssrc & 0xffu);

    return NGX_RTC_OK;
}

uint8_t ngx_rtc_h264_nalu_type(const uint8_t *nalu, uint32_t len)
{
    if ((NULL == nalu) || (0 == len))
    {
        return (uint8_t)NGX_RTC_NALU_RESERVED;
    }
    return (uint8_t)(nalu[0] & NGX_RTC_H264_NAL_TYPE_MASK);
}

int32_t ngx_rtc_h264_find_nalu(const uint8_t *data, uint32_t len, uint32_t *off)
{
    uint32_t i;

    if ((NULL == data) || (NULL == off))
    {
        return NGX_RTC_ERR_INVALID;
    }

    /* Search for 00 00 01; a 4-byte start code ends with the same bytes. */
    for (i = 0; (i + 2u) < len; i++)
    {
        if ((0x00u == data[i]) && (0x00u == data[i + 1u]) && (0x01u == data[i + 2u]))
        {
            break;
        }
    }
    if ((i + 2u) >= len)
    {
        return NGX_RTC_ERR_PARSE;
    }

    /* In both 3-byte and 4-byte forms the NALU header follows the final 01. */
    *off = i + 3u;
    return NGX_RTC_OK;
}

/* Read one bit from a bitstream, MSB first (matches SrsBitBuffer::read_bit). */
static int32_t ngx_rtc_h264_read_bit(const uint8_t *data, uint32_t bit_len,
                                     uint32_t *pos, uint8_t *bit)
{
    if (*pos >= bit_len)
    {
        return NGX_RTC_ERR_PARSE;
    }
    *bit = (uint8_t)((data[*pos >> 3] >> (7 - (*pos & 0x07u))) & 0x01u);
    (*pos)++;
    return NGX_RTC_OK;
}

/*
 * Read an unsigned Exp-Golomb ue(v) code (H.264 9.1). Same algorithm as
 * srs_avc_nalu_read_uev in src/kernel/srs_kernel_utility.cpp.
 */
static int32_t ngx_rtc_h264_read_ue(const uint8_t *data, uint32_t bit_len,
                                    uint32_t *pos, uint32_t *value)
{
    uint32_t leading = 0;
    uint32_t i;
    uint8_t b = 0;
    int32_t r;

    for (;;)
    {
        r = ngx_rtc_h264_read_bit(data, bit_len, pos, &b);
        if (0 != r)
        {
            return r;
        }
        if (1 == b)
        {
            break;
        }
        leading++;
        if (leading > 31u)
        {
            return NGX_RTC_ERR_PARSE;
        }
    }

    *value = (1u << leading) - 1u;
    for (i = 0; i < leading; i++)
    {
        r = ngx_rtc_h264_read_bit(data, bit_len, pos, &b);
        if (0 != r)
        {
            return r;
        }
        *value += ((uint32_t)b) << (leading - 1u - i);
    }

    return NGX_RTC_OK;
}

int32_t ngx_rtc_h264_is_b_frame(const uint8_t *nalu, uint32_t len)
{
    uint8_t nal_type;
    uint32_t bit_pos;
    uint32_t bit_len;
    uint32_t first_mb;
    uint32_t slice_type;
    int32_t r;

    if ((NULL == nalu) || (0 == len))
    {
        return NGX_RTC_ERR_INVALID;
    }

    nal_type = (uint8_t)(nalu[0] & NGX_RTC_H264_NAL_TYPE_MASK);

    /* Only slice NALUs carry a slice header; SRS checks the same four types. */
    if ((NGX_RTC_NALU_NON_IDR != nal_type) &&
        (NGX_RTC_NALU_DATA_PARTITION_A != nal_type) &&
        (NGX_RTC_NALU_DATA_PARTITION_B != nal_type) &&
        (NGX_RTC_NALU_DATA_PARTITION_C != nal_type))
    {
        return 0;
    }
    if (len < 2u)
    {
        return 0;
    }

    /* Skip the 1-byte NALU header, then read first_mb_in_slice and slice_type. */
    bit_pos = 8u;
    bit_len = len * 8u;

    r = ngx_rtc_h264_read_ue(nalu, bit_len, &bit_pos, &first_mb);
    if (0 != r)
    {
        return 0;
    }
    (void)first_mb;

    r = ngx_rtc_h264_read_ue(nalu, bit_len, &bit_pos, &slice_type);
    if (0 != r)
    {
        return 0;
    }

    /* SrsAvcSliceTypeB == 1, SrsAvcSliceTypeB1 == 6. */
    if ((1u == slice_type) || (6u == slice_type))
    {
        return 1;
    }
    return 0;
}

int32_t ngx_rtc_h264_packetize_single(const uint8_t *nalu, uint32_t len,
                                      uint32_t timestamp, uint16_t *seq, uint32_t ssrc,
                                      uint8_t payload_type, int32_t marker,
                                      uint8_t *scratch, uint32_t scratch_cap,
                                      ngx_rtc_rtp_emit_fn emit, void *opaque)
{
    uint32_t total;

    if ((NULL == nalu) || (NULL == seq) || (NULL == scratch) || (NULL == emit))
    {
        return NGX_RTC_ERR_INVALID;
    }
    if (0 == len)
    {
        return NGX_RTC_ERR_INVALID;
    }

    total = NGX_RTC_RTP_HEADER_SIZE + len;
    if (scratch_cap < total)
    {
        return NGX_RTC_ERR_TOO_SMALL;
    }

    (void)ngx_rtc_rtp_build_header(scratch, *seq, timestamp, ssrc, payload_type, marker);
    memcpy(scratch + NGX_RTC_RTP_HEADER_SIZE, nalu, len);
    (*seq)++;

    return emit(opaque, scratch, total);
}

int32_t ngx_rtc_h264_packetize_fu_a(const uint8_t *nalu, uint32_t len,
                                    uint32_t timestamp, uint16_t *seq, uint32_t ssrc,
                                    uint8_t payload_type, uint32_t mtu, int32_t marker,
                                    uint8_t *scratch, uint32_t scratch_cap,
                                    ngx_rtc_rtp_emit_fn emit, void *opaque)
{
    uint8_t nal_type;
    uint8_t fu_indicator;
    const uint8_t *p;
    uint32_t nb_left;
    uint32_t fu_payload_size;
    uint32_t num;
    uint32_t i;
    int32_t r;

    if ((NULL == nalu) || (NULL == seq) || (NULL == scratch) || (NULL == emit))
    {
        return NGX_RTC_ERR_INVALID;
    }
    if ((len < 2u) || (0 == mtu))
    {
        return NGX_RTC_ERR_INVALID;
    }

    /* The first byte moves into the FU indicator: nri is kept, type becomes 28. */
    nal_type = (uint8_t)(nalu[0] & NGX_RTC_H264_NAL_TYPE_MASK);
    fu_indicator = (uint8_t)((nalu[0] & NGX_RTC_H264_NRI_MASK) | NGX_RTC_H264_FU_A);

    p = nalu + 1;
    nb_left = len - 1u;
    fu_payload_size = mtu;

    /* SRS: num_of_packet = 1 + (nb_left - 1) / fu_payload_size. */
    num = 1u + ((nb_left - 1u) / fu_payload_size);

    for (i = 0; i < num; i++)
    {
        uint32_t chunk;
        uint32_t total;
        uint8_t fu_header;
        int32_t pkt_marker;

        chunk = (nb_left < fu_payload_size) ? nb_left : fu_payload_size;
        total = NGX_RTC_RTP_HEADER_SIZE + 2u + chunk;
        if (scratch_cap < total)
        {
            return NGX_RTC_ERR_TOO_SMALL;
        }

        fu_header = nal_type;
        if (0 == i)
        {
            fu_header = (uint8_t)(fu_header | NGX_RTC_H264_FU_START);
        }
        if (i == (num - 1u))
        {
            fu_header = (uint8_t)(fu_header | NGX_RTC_H264_FU_END);
        }

        /* RTP marker only rides the last fragment of the access unit. */
        pkt_marker = 0;
        if ((0 != marker) && (i == (num - 1u)))
        {
            pkt_marker = 1;
        }

        (void)ngx_rtc_rtp_build_header(scratch, *seq, timestamp, ssrc, payload_type, pkt_marker);
        scratch[NGX_RTC_RTP_HEADER_SIZE] = fu_indicator;
        scratch[NGX_RTC_RTP_HEADER_SIZE + 1u] = fu_header;
        memcpy(scratch + NGX_RTC_RTP_HEADER_SIZE + 2u, p, chunk);
        (*seq)++;

        r = emit(opaque, scratch, total);
        if (0 != r)
        {
            return r;
        }

        p += chunk;
        nb_left -= chunk;
    }

    return NGX_RTC_OK;
}

int32_t ngx_rtc_h264_packetize_stap_a(const uint8_t * const *nalus,
                                      const uint32_t *sizes, uint32_t count,
                                      uint32_t timestamp, uint16_t *seq, uint32_t ssrc,
                                      uint8_t payload_type,
                                      uint8_t *scratch, uint32_t scratch_cap,
                                      ngx_rtc_rtp_emit_fn emit, void *opaque)
{
    uint32_t payload_len;
    uint32_t total;
    uint32_t i;
    uint8_t nri;
    uint8_t *out;

    if ((NULL == nalus) || (NULL == sizes) || (NULL == seq) || (NULL == scratch) || (NULL == emit))
    {
        return NGX_RTC_ERR_INVALID;
    }
    if (0 == count)
    {
        return NGX_RTC_ERR_INVALID;
    }

    /* First pass: validate sizes and compute payload length (STAP header + each 2-byte length). */
    payload_len = 1u;
    for (i = 0; i < count; i++)
    {
        if (sizes[i] > 0xffffu)
        {
            return NGX_RTC_ERR_TOO_LARGE;
        }
        payload_len += 2u + sizes[i];
        if (payload_len > NGX_RTC_H264_MTU)
        {
            return NGX_RTC_ERR_TOO_LARGE;
        }
    }

    total = NGX_RTC_RTP_HEADER_SIZE + payload_len;
    if (scratch_cap < total)
    {
        return NGX_RTC_ERR_TOO_SMALL;
    }

    /* STAP-A header keeps the NRI of the first NALU, like SrsRtpSTAPPayload::encode. */
    out = scratch;
    out += ngx_rtc_rtp_build_header(out, *seq, timestamp, ssrc, payload_type, 0);

    nri = (uint8_t)(nalus[0][0] & NGX_RTC_H264_NRI_MASK);
    *out = (uint8_t)(nri | NGX_RTC_H264_STAP_A);
    out++;

    for (i = 0; i < count; i++)
    {
        *out = (uint8_t)((sizes[i] >> 8) & 0xffu);
        out++;
        *out = (uint8_t)(sizes[i] & 0xffu);
        out++;
        memcpy(out, nalus[i], sizes[i]);
        out += sizes[i];
    }

    (*seq)++;
    return emit(opaque, scratch, total);
}

int32_t ngx_rtc_h264_packetize(const uint8_t *nalu, uint32_t len,
                               uint32_t timestamp, uint16_t *seq, uint32_t ssrc,
                               uint8_t payload_type, int32_t marker,
                               uint8_t *scratch, uint32_t scratch_cap,
                               ngx_rtc_rtp_emit_fn emit, void *opaque)
{
    if ((NULL == nalu) || (NULL == seq) || (NULL == scratch) || (NULL == emit))
    {
        return NGX_RTC_ERR_INVALID;
    }
    if (0 == len)
    {
        return NGX_RTC_ERR_INVALID;
    }

    if (len <= NGX_RTC_H264_MTU)
    {
        return ngx_rtc_h264_packetize_single(nalu, len, timestamp, seq, ssrc,
                                             payload_type, marker,
                                             scratch, scratch_cap, emit, opaque);
    }

    return ngx_rtc_h264_packetize_fu_a(nalu, len, timestamp, seq, ssrc,
                                       payload_type, NGX_RTC_H264_MTU, marker,
                                       scratch, scratch_cap, emit, opaque);
}

int32_t ngx_rtc_opus_packetize(const uint8_t *opus, uint32_t len,
                               uint32_t timestamp, uint16_t *seq, uint32_t ssrc,
                               uint8_t payload_type, int32_t marker,
                               uint8_t *scratch, uint32_t scratch_cap,
                               ngx_rtc_rtp_emit_fn emit, void *opaque)
{
    uint32_t total;

    if ((NULL == opus) || (NULL == seq) || (NULL == scratch) || (NULL == emit))
    {
        return NGX_RTC_ERR_INVALID;
    }
    if (0 == len)
    {
        return NGX_RTC_ERR_INVALID;
    }

    /* RFC 7587: no Opus payload header; the frame follows the RTP header. */
    total = NGX_RTC_RTP_HEADER_SIZE + len;
    if (scratch_cap < total)
    {
        return NGX_RTC_ERR_TOO_SMALL;
    }

    (void)ngx_rtc_rtp_build_header(scratch, *seq, timestamp, ssrc, payload_type, marker);
    memcpy(scratch + NGX_RTC_RTP_HEADER_SIZE, opus, len);
    (*seq)++;

    return emit(opaque, scratch, total);
}
