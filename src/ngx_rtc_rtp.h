/*
 * ngx_rtc_rtp.h - H264 NALU to RTP packetization (RFC 6184)
 *
 * Pure C11 translation of SRS 6.0:
 *   src/kernel/srs_kernel_rtc_rtp.hpp  (constants kStapA/kFuA/kStart/kEnd/kNalTypeMask,
 *                                       SrsRtpHeader layout)
 *   src/kernel/srs_kernel_rtc_rtp.cpp  (SrsRtpHeader::encode, SrsRtpSTAPPayload::encode,
 *                                       SrsRtpFUAPayload2::encode)
 *   src/app/srs_app_rtc_source.cpp     (SrsRtcRtpBuilder::package_single_nalu,
 *                                       package_fu_a, package_stap_a, package_nalus)
 *   src/kernel/srs_kernel_codec.cpp    (SrsVideoFrame::parse_avc_nalu_type,
 *                                       parse_avc_b_frame)
 *
 * No global state: every input and output is passed by parameter, so this unit
 * can be tested in isolation. The caller owns the RTP buffer (scratch) and the
 * sequence number counter.
 */

#ifndef NGX_RTC_RTP_H
#define NGX_RTC_RTP_H

#include <stdint.h>

/* Return codes (also declared in ngx_rtc_sdp.h; guarded so both headers can be included). */
#ifndef NGX_RTC_OK
#define NGX_RTC_OK 0
#endif
#ifndef NGX_RTC_ERR_INVALID
#define NGX_RTC_ERR_INVALID (-1)
#endif
#ifndef NGX_RTC_ERR_TOO_SMALL
#define NGX_RTC_ERR_TOO_SMALL (-2)
#endif
#ifndef NGX_RTC_ERR_TOO_LARGE
#define NGX_RTC_ERR_TOO_LARGE (-3)
#endif
#ifndef NGX_RTC_ERR_PARSE
#define NGX_RTC_ERR_PARSE (-4)
#endif

/* RTP fixed header size and marker bit (RFC 3550 section 5.1). */
#define NGX_RTC_RTP_HEADER_SIZE 12u
#define NGX_RTC_RTP_MARKER      0x80u
#define NGX_RTC_RTP_VERSION_CC  0x80u /* V=2, no padding, no extension, CC=0 */

/* H264 NALU header bit masks and RFC 6184 packet types. */
#define NGX_RTC_H264_NAL_TYPE_MASK 0x1fu
#define NGX_RTC_H264_NRI_MASK      0xe0u
#define NGX_RTC_H264_STAP_A        24u
#define NGX_RTC_H264_FU_A          28u
#define NGX_RTC_H264_FU_START      0x80u
#define NGX_RTC_H264_FU_END        0x40u

/*
 * MTU for RTP payload. SRS uses kRtpPacketSize(1500) - 300 = 1200 to leave
 * room for UDP/IP and any RTP extension headers. Each FU-A fragment carries at
 * most this many NAL bytes; the whole RTP packet stays below 12+2+1200=1214.
 */
#define NGX_RTC_H264_MTU 1200u

/* RTP clock rates and default payload types (same as SRS 6.0). */
#define NGX_RTC_H264_CLOCK_RATE  90000u
#define NGX_RTC_OPUS_CLOCK_RATE  48000u
#define NGX_RTC_PAYLOAD_TYPE_H264 102u
#define NGX_RTC_PAYLOAD_TYPE_OPUS 111u

/* H264 NAL unit types (ITU-T H.264 table 7-1). */
typedef enum
{
    NGX_RTC_NALU_RESERVED                = 0,
    NGX_RTC_NALU_NON_IDR                 = 1,
    NGX_RTC_NALU_DATA_PARTITION_A        = 2,
    NGX_RTC_NALU_DATA_PARTITION_B        = 3,
    NGX_RTC_NALU_DATA_PARTITION_C        = 4,
    NGX_RTC_NALU_IDR                     = 5,
    NGX_RTC_NALU_SEI                     = 6,
    NGX_RTC_NALU_SPS                     = 7,
    NGX_RTC_NALU_PPS                     = 8,
    NGX_RTC_NALU_AUD                     = 9,
    NGX_RTC_NALU_EO_SEQ                  = 10,
    NGX_RTC_NALU_EO_STREAM               = 11,
    NGX_RTC_NALU_FILLER                  = 12,
    NGX_RTC_NALU_SPS_EXT                 = 13,
    NGX_RTC_NALU_PREFIX                  = 14,
    NGX_RTC_NALU_SUBSET_SPS              = 15,
    NGX_RTC_NALU_LAYER_WITHOUT_PARTITION = 19,
    NGX_RTC_NALU_CODED_SLICE_EXT         = 20
} ngx_rtc_nalu_type_t;

/* RTP 12-byte fixed header, big endian on the wire (RFC 3550). */
typedef struct
{
    uint8_t  version_cc; /* V=2 in high bits, CSRC count in low nibble */
    uint8_t  marker_pt;  /* M bit in high bit, payload type in low 7 bits */
    uint16_t seq;
    uint32_t timestamp;
    uint32_t ssrc;
} ngx_rtc_rtp_header_t;

/* Callback that receives one fully formed RTP packet. */
typedef int32_t (*ngx_rtc_rtp_emit_fn)(void *opaque, const uint8_t *rtp, uint32_t len);

/*
 * Serialize a 12-byte RTP header into buf (big endian). Returns NGX_RTC_OK or
 * NGX_RTC_ERR_TOO_SMALL if cap < NGX_RTC_RTP_HEADER_SIZE.
 */
int32_t ngx_rtc_rtp_header_write(const ngx_rtc_rtp_header_t *hdr, uint8_t *buf, uint32_t cap);

/*
 * Convert a millisecond timestamp to the 90kHz H264 RTP clock.
 */
static inline uint32_t ngx_rtc_h264_timestamp_from_ms(uint32_t ms)
{
    return (uint32_t)((uint64_t)ms * (uint64_t)NGX_RTC_H264_CLOCK_RATE / 1000u);
}

/*
 * Convert a millisecond timestamp to the 48kHz Opus RTP clock.
 */
static inline uint32_t ngx_rtc_opus_timestamp_from_ms(uint32_t ms)
{
    return (uint32_t)((uint64_t)ms * (uint64_t)NGX_RTC_OPUS_CLOCK_RATE / 1000u);
}

/*
 * Return the H264 NAL unit type of a NALU whose first byte is the NALU header
 * (i.e. the byte right after an Annex-B start code). Empty input returns
 * NGX_RTC_NALU_RESERVED.
 */
uint8_t ngx_rtc_h264_nalu_type(const uint8_t *nalu, uint32_t len);

/*
 * Find the first Annex-B start code (00 00 01 or 00 00 00 01) in data and set
 * *off to the byte offset of the NALU header (first byte after the start code).
 * Returns NGX_RTC_OK, or NGX_RTC_ERR_PARSE when no start code exists.
 */
int32_t ngx_rtc_h264_find_nalu(const uint8_t *data, uint32_t len, uint32_t *off);

/*
 * Return 1 when nalu is an H264 B-frame slice (slice_type B or B1), 0 when it
 * is not (or cannot be a B frame), negative on error. This drops B frames the
 * same way SrsRtcRtpBuilder::filter + SrsVideoFrame::parse_avc_b_frame do,
 * because WebRTC low-latency playout does not support B frames.
 */
int32_t ngx_rtc_h264_is_b_frame(const uint8_t *nalu, uint32_t len);

/*
 * Packetize one H264 NALU (nalu[0] is the NALU header, no start code) into one
 * or more RTP packets:
 *   len <= NGX_RTC_H264_MTU  -> single NAL unit packet
 *   len >  NGX_RTC_H264_MTU  -> FU-A fragments
 *
 * timestamp is the 90kHz RTP timestamp. *seq is advanced once per emitted
 * packet, so the caller owns the sequence counter. marker is applied to the
 * last emitted packet (set it to 1 when this NALU ends an access unit).
 * scratch is a caller buffer that must hold one packet (<= 1214 bytes for FU-A).
 */
int32_t ngx_rtc_h264_packetize(const uint8_t *nalu, uint32_t len,
                               uint32_t timestamp, uint16_t *seq, uint32_t ssrc,
                               uint8_t payload_type, int32_t marker,
                               uint8_t *scratch, uint32_t scratch_cap,
                               ngx_rtc_rtp_emit_fn emit, void *opaque);

/* Package one small NALU as a single NAL unit packet (RFC 6184 section 5.6). */
int32_t ngx_rtc_h264_packetize_single(const uint8_t *nalu, uint32_t len,
                                      uint32_t timestamp, uint16_t *seq, uint32_t ssrc,
                                      uint8_t payload_type, int32_t marker,
                                      uint8_t *scratch, uint32_t scratch_cap,
                                      ngx_rtc_rtp_emit_fn emit, void *opaque);

/* Fragment one large NALU into FU-A packets (RFC 6184 section 5.8). */
int32_t ngx_rtc_h264_packetize_fu_a(const uint8_t *nalu, uint32_t len,
                                    uint32_t timestamp, uint16_t *seq, uint32_t ssrc,
                                    uint8_t payload_type, uint32_t mtu, int32_t marker,
                                    uint8_t *scratch, uint32_t scratch_cap,
                                    ngx_rtc_rtp_emit_fn emit, void *opaque);

/*
 * Aggregate several small NALUs (e.g. SPS + PPS) into one STAP-A packet
 * (RFC 6184 section 5.7). The marker bit is always cleared, matching SRS which
 * prepends SPS/PPS in a STAP-A packet before each IDR frame.
 */
int32_t ngx_rtc_h264_packetize_stap_a(const uint8_t * const *nalus,
                                      const uint32_t *sizes, uint32_t count,
                                      uint32_t timestamp, uint16_t *seq, uint32_t ssrc,
                                      uint8_t payload_type,
                                      uint8_t *scratch, uint32_t scratch_cap,
                                      ngx_rtc_rtp_emit_fn emit, void *opaque);

/*
 * Package one encoded Opus frame as a single RTP packet (RFC 7587). An Opus
 * frame is tiny and complete, so there is no payload header or fragmentation:
 * the whole frame follows the 12-byte RTP header. The 48kHz timestamp is the
 * start time of the frame and marker is normally set on every packet.
 */
int32_t ngx_rtc_opus_packetize(const uint8_t *opus, uint32_t len,
                               uint32_t timestamp, uint16_t *seq, uint32_t ssrc,
                               uint8_t payload_type, int32_t marker,
                               uint8_t *scratch, uint32_t scratch_cap,
                               ngx_rtc_rtp_emit_fn emit, void *opaque);

#endif /* NGX_RTC_RTP_H */
