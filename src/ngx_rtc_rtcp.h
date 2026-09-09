/*
 * ngx_rtc_rtcp.h - RTCP codec core (RFC 3550 / RFC 4585)
 *
 * Pure C11 translation of SRS 6.0:
 *   src/kernel/srs_kernel_rtc_rtcp.cpp  (SrsRtcpCommon/SrsRtcpSR/SrsRtcpRR,
 *                                        SrsRtcpNack/SrsRtcpPli/SrsRtcpCompound,
 *                                        SrsRtcpHeader/SrsRtcpRB bit layout)
 *
 * Covers the control plane of the media link:
 *   decode (server <- client): RR(201), SDES(202), NACK(RTPFB FMT=1), PLI(PSFB FMT=1)
 *   encode (server -> client): SR(200), SDES(202)
 *   compound: append several encoded sub-packets into one UDP datagram
 *
 * No global state; every input/output is passed by parameter so this unit can
 * be tested in isolation. The caller owns every buffer.
 *
 * Integration notes for the main agent (do NOT edit this unit for wiring):
 *
 * 1. config: add this unit to the pure-C core so it links into all modules.
 *      RTC_CORE_SRCS: append  $ngx_addon_dir/src/ngx_rtc_rtcp.c
 *      RTC_CORE_DEPS: append  $ngx_addon_dir/src/ngx_rtc_rtcp.h
 *
 * 2. RTMP bridge module (sender): periodically send a compound SR + SDES to
 *    each player, e.g. once per second in the existing bridge timer:
 *
 *      uint8_t tmp[NGX_RTC_RTCP_MAX_PACKET];
 *      uint8_t out[NGX_RTC_RTCP_MAX_PACKET];
 *      uint32_t out_len = 0, sub_len = 0;
 *      ngx_rtc_rtcp_sr_t sr;
 *      memset(&sr, 0, sizeof(sr));
 *      sr.ssrc = source->video_ssrc;
 *      sr.rtp_ts = source->video_ts;   90kHz, caller-owned counter
 *      sr.ntp = 0;                     0 = fill from wall clock (time.h)
 *      if (ngx_rtc_rtcp_encode_sr(&sr, tmp, sizeof(tmp), &sub_len) == NGX_RTC_OK) {
 *          ngx_rtc_rtcp_compound_append(out, sizeof(out), &out_len, tmp, sub_len);
 *      }
 *      if (ngx_rtc_rtcp_encode_sdes(sr.ssrc, cname, tmp, sizeof(tmp), &sub_len) == NGX_RTC_OK) {
 *          ngx_rtc_rtcp_compound_append(out, sizeof(out), &out_len, tmp, sub_len);
 *      }
 *      sendto(out, out_len) once for the whole compound
 *
 * 3. Stream UDP module (receiver): feed the SRTP-unprotected payload to
 *    ngx_rtc_rtcp_decode with a callback:
 *
 *      static int32_t on_rtcp(const ngx_rtc_rtcp_pkt_t *pkt, void *opaque) {
 *          if (pkt->type == NGX_RTC_RTCP_RR)      { read pkt->rb.*; }
 *          if (pkt->type == NGX_RTC_RTCP_RTPFB && pkt->fmt == NGX_RTC_RTCP_FMT_NACK) {
 *              uint16_t seqs[512]; uint16_t n = 0;
 *              ngx_rtc_rtcp_nack_expand(pkt, seqs, 512, &n);
 *              retransmit seqs[0..n-1]
 *          }
 *          if (pkt->type == NGX_RTC_RTCP_PSFB && pkt->fmt == NGX_RTC_RTCP_FMT_PLI) {
 *              queue a keyframe (IDR) for pkt->media_ssrc
 *          }
 *          return NGX_RTC_OK;
 *      }
 *      ngx_rtc_rtcp_decode(buf, len, on_rtcp, session);
 */

#ifndef NGX_RTC_RTCP_H
#define NGX_RTC_RTCP_H

#include <stdint.h>

/* Return codes (shared with ngx_rtc_rtp.h; guarded so both can be included). */
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
/* Truncated input: not enough bytes for the declared packet length. */
#ifndef NGX_RTC_ERR_NEED_MORE
#define NGX_RTC_ERR_NEED_MORE (-5)
#endif

/* RTCP payload types (RFC 3550 section 13, RFC 4585 section 3). */
#define NGX_RTC_RTCP_SR    200u
#define NGX_RTC_RTCP_RR    201u
#define NGX_RTC_RTCP_SDES  202u
#define NGX_RTC_RTCP_BYE   203u
#define NGX_RTC_RTCP_RTPFB 205u /* generic RTP feedback */
#define NGX_RTC_RTCP_PSFB  206u /* payload-specific feedback */

/* Feedback message types carried in the header FMT field (RFC 4585). */
#define NGX_RTC_RTCP_FMT_NACK 1u  /* RTPFB generic NACK (section 6.2.1) */
#define NGX_RTC_RTCP_FMT_PLI  1u  /* PSFB PLI keyframe request (section 6.3.1) */
#define NGX_RTC_RTCP_FMT_TWCC 15u /* RTPFB transport-wide CC (not decoded here) */

/* SRS kRtcpPacketSize; enough for any single RTCP datagram on the wire. */
#define NGX_RTC_RTCP_MAX_PACKET 1500u

/* Bounds for the decoded NACK state kept in ngx_rtc_rtcp_pkt_t. */
#define NGX_RTC_RTCP_MAX_NACK_ENTRIES 128u

/* SDES CNAME text is at most 255 octets (RFC 3550 6.5.1) plus NUL. */
#define NGX_RTC_RTCP_MAX_CNAME 256u

/* Seconds between the NTP epoch (1900-01-01) and the Unix epoch (1970-01-01). */
#define NGX_RTC_RTCP_NTP_UNIX_OFFSET 2208988800ULL

/* One RR/SR report block (RFC 3550 section 6.4.1). */
typedef struct
{
    uint32_t ssrc;           /* source being reported */
    uint8_t  fraction_lost;  /* fixed point 1/256 */
    uint32_t lost_packets;   /* 24-bit cumulative number of packets lost */
    uint32_t highest_seq;    /* extended highest sequence number received */
    uint32_t jitter;         /* interarrival jitter, RTP timestamp units */
    uint32_t lsr;            /* middle 32 bits of the last SR NTP timestamp */
    uint32_t dlsr;           /* delay since last SR, units of 1/65536 s */
} ngx_rtc_rtcp_rb_t;

/*
 * Decoded state of one RTCP packet. The same struct is reused by every
 * packet type; only the fields that apply to the parsed type are filled.
 */
typedef struct
{
    uint8_t  version;       /* RTCP version, must be 2 */
    uint8_t  padding;       /* P bit: trailing padding present */
    uint8_t  type;          /* NGX_RTC_RTCP_* payload type */
    uint8_t  fmt;           /* RC (SR/RR), SC (SDES) or FMT (FB) */
    uint16_t length_words;  /* header length field (32-bit words minus one) */
    uint32_t total_len;     /* whole packet length on the wire, incl. padding */
    uint32_t ssrc;          /* SSRC of the packet sender */

    /* SR sender info (200). */
    uint64_t ntp;                  /* 64-bit NTP timestamp */
    uint32_t rtp_ts;               /* RTP timestamp corresponding to ntp */
    uint32_t sender_packet_count;  /* cumulative RTP packets sent */
    uint32_t sender_octet_count;   /* cumulative RTP payload octets sent */

    /* RR first report block (201); has_rb is 0 for an empty RR (RC=0). */
    uint8_t           has_rb;
    ngx_rtc_rtcp_rb_t rb;

    /* SDES CNAME (202); empty string when no CNAME item was present. */
    char cname[NGX_RTC_RTCP_MAX_CNAME];

    /* RTPFB generic NACK (205/FMT=1): raw PID+BLP FCI entries. */
    uint32_t media_ssrc;   /* SSRC of the media source (also used by PSFB) */
    uint16_t nack_count;   /* number of valid pid/blp entries */
    uint16_t nack_pid[NGX_RTC_RTCP_MAX_NACK_ENTRIES];
    uint16_t nack_blp[NGX_RTC_RTCP_MAX_NACK_ENTRIES];

    /* RTPFB transport-wide CC (205/FMT=15) summary (RFC 8888). */
    uint16_t twcc_base_seq;   /* base sequence number of the feedback */
    uint16_t twcc_pkt_count;  /* packets covered by this feedback */
    uint32_t twcc_lost;       /* packets reported as not received */
    uint32_t twcc_received;   /* packets reported as received */
} ngx_rtc_rtcp_pkt_t;

/* Input for ngx_rtc_rtcp_encode_sr. */
typedef struct
{
    uint32_t ssrc;
    uint64_t ntp;        /* 0 = fill from the current wall clock (time.h) */
    uint32_t rtp_ts;     /* RTP timestamp matching the NTP timestamp */
    uint32_t packet_count;
    uint32_t octet_count;
    const ngx_rtc_rtcp_rb_t *rb; /* optional report block, NULL to omit */
} ngx_rtc_rtcp_sr_t;

/*
 * Callback invoked once per sub-packet while walking a compound packet.
 * Return NGX_RTC_OK to continue, any other value to abort the walk.
 */
typedef int32_t (*ngx_rtc_rtcp_decode_cb)(const ngx_rtc_rtcp_pkt_t *pkt, void *opaque);

/*
 * Parse exactly one RTCP packet that starts at buf. On success writes the
 * decoded fields into *pkt and the full wire length (header + payload +
 * padding) into *consumed. Returns:
 *   NGX_RTC_OK          - parsed one packet
 *   NGX_RTC_ERR_NEED_MORE - buf holds fewer bytes than the packet declares
 *   NGX_RTC_ERR_PARSE   - not RTCP (version != 2) or malformed fixed fields
 */
int32_t ngx_rtc_rtcp_parse(const uint8_t *buf, uint32_t len,
                           ngx_rtc_rtcp_pkt_t *pkt, uint32_t *consumed);

/*
 * Walk a compound RTCP packet (RFC 3550 section 6.1: one or more RTCP
 * packets back to back). Calls cb for every sub-packet. Returns NGX_RTC_OK
 * after the last sub-packet, the first non-zero cb result, or a negative
 * error code when parsing fails.
 */
int32_t ngx_rtc_rtcp_decode(const uint8_t *buf, uint32_t len,
                            ngx_rtc_rtcp_decode_cb cb, void *opaque);

/*
 * Expand the NACK PID+BLP entries of a decoded packet into the full list of
 * lost RTP sequence numbers (PID and every bit set in BLP, PID+bit_index+1).
 * Returns NGX_RTC_OK, or NGX_RTC_ERR_TOO_SMALL when the list does not fit;
 * *count always holds the number of sequence numbers actually written.
 */
int32_t ngx_rtc_rtcp_nack_expand(const ngx_rtc_rtcp_pkt_t *pkt,
                                 uint16_t *seqs, uint16_t cap, uint16_t *count);

/*
 * Convert a Unix epoch time in microseconds to the 64-bit NTP timestamp used
 * by SR/RR (RFC 3550 section 4). Pure function, kept public for unit tests.
 */
uint64_t ngx_rtc_rtcp_ntp_from_unix_us(uint64_t unix_us);

/* Return the current wall clock as a 64-bit NTP timestamp (uses time.h). */
uint64_t ngx_rtc_rtcp_ntp_now(void);

/*
 * Encode an SR sender report (RFC 3550 section 6.4.1) into buf. The report
 * block is optional. Note that SR carries the already-computed RTP timestamp,
 * so the RTP clock rate is not needed by the encoder. Returns NGX_RTC_OK or
 * NGX_RTC_ERR_TOO_SMALL when cap is not enough.
 */
int32_t ngx_rtc_rtcp_encode_sr(const ngx_rtc_rtcp_sr_t *sr,
                               uint8_t *buf, uint32_t cap, uint32_t *len);

/*
 * Encode an SDES packet with a single CNAME item (RFC 3550 section 6.5).
 * Returns NGX_RTC_OK, NGX_RTC_ERR_TOO_LARGE when cname is longer than 255
 * octets, or NGX_RTC_ERR_TOO_SMALL when cap is not enough.
 */
int32_t ngx_rtc_rtcp_encode_sdes(uint32_t ssrc, const char *cname,
                                 uint8_t *buf, uint32_t cap, uint32_t *len);

/*
 * Append one already-encoded sub-packet to a compound buffer. The caller
 * owns compound and the running length *len, so several sub-packets can be
 * concatenated before a single sendto. Returns NGX_RTC_OK or
 * NGX_RTC_ERR_TOO_SMALL.
 */
int32_t ngx_rtc_rtcp_compound_append(uint8_t *compound, uint32_t cap, uint32_t *len,
                                     const uint8_t *pkt, uint32_t pkt_len);

#endif /* NGX_RTC_RTCP_H */
